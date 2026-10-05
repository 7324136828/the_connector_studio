#include "ConnectorClient.h"
#include "AppIdentity.h"
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include "../third_party/nlohmann/json.hpp"

namespace Lattice::Connector
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::size_t MaxResponse = 8 * 1024 * 1024, MaxRequest = 16 * 1024 * 1024;
        struct InternetHandle
        {
            HINTERNET value = nullptr;
            ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
        };
        struct Event
        {
            HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ~Event() { if (value) CloseHandle(value); }
        };
        struct AsyncState
        {
            Event ready, closed;
            std::atomic<DWORD> error{0}, bytes{0};
        };
        void CALLBACK StatusCallback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length)
        {
            auto* state = reinterpret_cast<AsyncState*>(context);
            if (!state) return;
            if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(state->closed.value); return; }
            if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            {
                state->error = info && length >= sizeof(WINHTTP_ASYNC_RESULT) ?
                    static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
                SetEvent(state->ready.value);
            }
            else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE || status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)
            {
                state->bytes = status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? length :
                    status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && info && length >= sizeof(DWORD) ? *static_cast<DWORD*>(info) : 0;
                SetEvent(state->ready.value);
            }
        }
        // Only this worker closes the asynchronous request. Its callback context and
        // read buffers remain alive until the final HANDLE_CLOSING notification.
        struct AsyncRequest
        {
            AsyncState state;
            HINTERNET value = nullptr;
            bool hasContext = false;
            ~AsyncRequest()
            {
                if (value)
                {
                    WinHttpCloseHandle(value);
                    if (hasContext) WaitForSingleObject(state.closed.value, INFINITE);
                }
            }
            void Begin() { state.error = 0; state.bytes = 0; ResetEvent(state.ready.value); }
        };
        std::string Utf8(const std::wstring& text)
        {
            if (text.empty()) return {};
            if (text.size() > MaxRequest) throw std::runtime_error("Text too large");
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!count) throw std::runtime_error("Invalid Unicode");
            std::string bytes(count, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(), count, nullptr, nullptr))
                throw std::runtime_error("Invalid Unicode");
            return bytes;
        }
        std::wstring Wide(const std::string& bytes)
        {
            if (bytes.empty()) return {};
            if (bytes.size() > MaxRequest) throw std::runtime_error("Text too large");
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (!count) throw std::runtime_error("Invalid UTF-8");
            std::wstring text(count, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
            if (text.find(L'\0') != std::wstring::npos) throw std::runtime_error("Null in text");
            return text;
        }
        Json Parse(const std::string& bytes)
        {
            return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
                if (depth > 64) throw std::runtime_error("JSON nesting limit");
                return true;
            });
        }
        bool Cancelled(const std::shared_ptr<Cancellation>& cancel) { return cancel && cancel->IsCancelled(); }
        bool ValidModelId(const std::wstring& id)
        {
            const auto letterOrDigit = [](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
            };
            return !id.empty() && id.size() <= 100 && letterOrDigit(id.front()) &&
                std::all_of(id.begin(), id.end(), [&](wchar_t c) { return letterOrDigit(c) || c == L'.' || c == L'_' || c == L'-'; });
        }
        std::wstring NetworkError(DWORD code)
        {
            if (code == ERROR_WINHTTP_TIMEOUT) return L"The Connector server timed out. Check its status and try again.";
            if (code == ERROR_WINHTTP_SECURE_FAILURE) return L"The server's HTTPS certificate could not be verified.";
            if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return L"The Connector server hostname could not be resolved. Check Settings.";
            if (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_CONNECTION_ERROR)
                return L"Cannot connect to the Connector server. Check its URL in Settings and make sure it is running.";
            return L"The Connector request failed (Windows error " + std::to_wstring(code) + L").";
        }
        bool Wait(AsyncRequest& request, const std::shared_ptr<Cancellation>& cancel,
            const std::chrono::steady_clock::time_point& deadline, HttpResult& result)
        {
            for (;;)
            {
                if (Cancelled(cancel)) { result.cancelled = true; return false; }
                if (std::chrono::steady_clock::now() >= deadline) { result.error = NetworkError(ERROR_WINHTTP_TIMEOUT); return false; }
                const auto status = WaitForSingleObject(request.state.ready.value, 50);
                if (status == WAIT_OBJECT_0)
                {
                    if (request.state.error) { result.error = NetworkError(request.state.error); return false; }
                    return true;
                }
                if (status == WAIT_FAILED) { result.error = L"Windows could not wait for the Connector response."; return false; }
            }
        }
        bool Started(BOOL started, AsyncRequest& request, const std::shared_ptr<Cancellation>& cancel,
            const std::chrono::steady_clock::time_point& deadline, HttpResult& result)
        {
            if (!started && GetLastError() != ERROR_IO_PENDING) { result.error = NetworkError(GetLastError()); return false; }
            return Wait(request, cancel, deadline, result);
        }
        std::wstring ResponseHeader(HINTERNET request, const wchar_t* name)
        {
            DWORD bytes = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 16384 || bytes < sizeof(wchar_t)) return {};
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
            value.resize(bytes / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            return value;
        }
        HttpResult Exchange(const std::wstring& endpoint, const std::wstring& method,
            const std::string& body, const std::vector<std::pair<std::wstring, std::wstring>>& extraHeaders,
            const std::shared_ptr<Cancellation>& cancel, unsigned timeoutSeconds,
            const std::function<bool(const std::string&)>& stopAfter)
        {
            HttpResult result;
            if (Cancelled(cancel)) { result.cancelled = true; return result; }
            if (body.size() > MaxRequest || method.empty() || method.size() > 16 ||
                !std::all_of(method.begin(), method.end(), [](wchar_t c) { return c >= L'A' && c <= L'Z'; }))
            { result.error = L"Invalid HTTP request."; return result; }
            std::wstring url;
            if (!NormalizeServerUrl(endpoint, url, result.error)) return result;
            // Server-root normalization trims slashes; an absolute transport
            // endpoint keeps every trailing path slash, including after whitespace.
            auto endpointEnd = endpoint.size();
            while (endpointEnd && iswspace(endpoint[endpointEnd - 1])) --endpointEnd;
            while (endpointEnd && endpoint[endpointEnd - 1] == L'/') { url += L'/'; --endpointEnd; }
            std::wstring headers = L"Accept: application/json\r\nContent-Type: application/json; charset=utf-8\r\n";
            if (extraHeaders.size() > 64) { result.error = L"Too many HTTP headers."; return result; }
            std::set<std::wstring> headerNames;
            for (const auto& header : extraHeaders)
            {
                const auto token = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                    (c >= L'0' && c <= L'9') || std::wstring(L"!#$%&'*+-.^_`|~").find(c) != std::wstring::npos; };
                if (header.first.empty() || header.first.size() > 128 || !std::all_of(header.first.begin(), header.first.end(), token) ||
                    header.second.size() > 8192 || std::any_of(header.second.begin(), header.second.end(), [](wchar_t c) { return c < L' ' || c == 0x7f; }))
                { result.error = L"Invalid HTTP header."; return result; }
                auto lower = header.first; std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
                if (!headerNames.insert(lower).second) { result.error = L"Duplicate HTTP header."; return result; }
                if (lower == L"host" || lower == L"content-length" || lower == L"transfer-encoding")
                { result.error = L"Unsupported HTTP header override."; return result; }
                if (lower == L"accept") headers.erase(0, headers.find(L"\r\n") + 2);
                if (lower == L"content-type")
                {
                    const auto at = headers.find(L"Content-Type:");
                    if (at != std::wstring::npos) headers.erase(at, headers.find(L"\r\n", at) + 2 - at);
                }
                headers += header.first + L": " + header.second + L"\r\n";
                if (headers.size() > 65536) { result.error = L"HTTP headers exceed the size limit."; return result; }
            }
            timeoutSeconds = std::clamp(timeoutSeconds, 1u, 180u);
            URL_COMPONENTS parts{sizeof(parts)};
            parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), ICU_REJECT_USERPWD, &parts))
            { result.error = L"The Connector URL could not be read."; return result; }
            const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
            InternetHandle session;
            session.value = WinHttpOpen(L"Connector Studio/1.2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
            if (!session.value) { result.error = NetworkError(GetLastError()); return result; }
            if (!WinHttpSetTimeouts(session.value, 5000, 5000, 10000, static_cast<int>(timeoutSeconds * 1000)))
            { result.error = NetworkError(GetLastError()); return result; }
            InternetHandle connection;
            connection.value = WinHttpConnect(session.value, host.c_str(), parts.nPort, 0);
            if (!connection.value) { result.error = NetworkError(GetLastError()); return result; }
            // Buffers are declared before the request so they outlive HANDLE_CLOSING.
            std::vector<char> buffer(65536);
            AsyncRequest request;
            if (!request.state.ready.value || !request.state.closed.value) { result.error = L"Windows could not prepare the request."; return result; }
            request.value = WinHttpOpenRequest(connection.value, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (!request.value) { result.error = NetworkError(GetLastError()); return result; }
            const DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
            if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, const_cast<DWORD*>(&disabled), sizeof(disabled)) ||
                WinHttpSetStatusCallback(request.value, StatusCallback, WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                    WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
                    WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE |
                    WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
            { result.error = NetworkError(GetLastError()); return result; }
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&request.state);
            if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
            { result.error = NetworkError(GetLastError()); return result; }
            request.hasContext = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
            request.Begin();
            if (!Started(WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), context), request, cancel, deadline, result)) return result;
            request.Begin();
            if (!Started(WinHttpReceiveResponse(request.value, nullptr), request, cancel, deadline, result)) return result;
            DWORD statusSize = sizeof(result.status);
            if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize, WINHTTP_NO_HEADER_INDEX))
            { result.error = NetworkError(GetLastError()); return result; }
            result.contentType = ResponseHeader(request.value, L"Content-Type");
            result.mcpSessionId = ResponseHeader(request.value, L"Mcp-Session-Id");
            auto lowerContentType = result.contentType;
            std::transform(lowerContentType.begin(), lowerContentType.end(), lowerContentType.begin(), towlower);
            const bool eventStream = lowerContentType.find(L"text/event-stream") != std::wstring::npos;
            for (;;)
            {
                DWORD readSize = static_cast<DWORD>(buffer.size());
                if (eventStream)
                {
                    // A stream can remain open after its final JSON-RPC result.
                    // Read only currently available bytes instead of waiting for
                    // a large buffer to fill or for the peer to close the stream.
                    request.Begin();
                    if (!Started(WinHttpQueryDataAvailable(request.value, nullptr), request, cancel, deadline, result)) return result;
                    readSize = (std::min)(readSize, request.state.bytes.load());
                    if (!readSize) break;
                }
                request.Begin();
                if (!Started(WinHttpReadData(request.value, buffer.data(), readSize, nullptr),
                    request, cancel, deadline, result)) return result;
                if (!request.state.bytes) break;
                if (result.body.size() + request.state.bytes > MaxResponse)
                { result.error = L"The Connector response exceeds the 8 MiB limit."; return result; }
                result.body.append(buffer.data(), request.state.bytes);
                if (eventStream && stopAfter && stopAfter(result.body)) break;
            }
            if (Cancelled(cancel)) { result.cancelled = true; return result; }
            result.success = result.status >= 200 && result.status < 300;
            if (!result.success)
            {
                result.error = L"Connector returned HTTP " + std::to_wstring(result.status) + L".";
                try
                {
                    const auto error = Parse(result.body);
                    std::string message;
                    if (error.contains("error") && error["error"].is_object() && error["error"].contains("message") && error["error"]["message"].is_string())
                        message = error["error"]["message"].get<std::string>();
                    else if (error.contains("detail") && error["detail"].is_string()) message = error["detail"].get<std::string>();
                    if (!message.empty()) result.error += L" " + Wide(message).substr(0, 2048);
                }
                catch (...) {}
                if (result.status >= 300 && result.status < 400) result.error += L" Use the final server URL in Settings; redirects are not followed.";
            }
            return result;
        }
        HttpResult Http(const Settings& settings, const wchar_t* method, const wchar_t* endpoint,
            const std::string& body, const std::shared_ptr<Cancellation>& cancel, bool discovery)
        {
            HttpResult result;
            std::wstring normalized;
            if (!NormalizeServerUrl(settings.serverUrl, normalized, result.error)) return result;
            return Exchange(ApiBaseUrl(normalized) + endpoint, method, body, {}, cancel, discovery ? 20 : 125, {});
        }
        bool WriteAtomic(const std::wstring& path, const std::string& bytes, std::wstring& error)
        {
            const std::filesystem::path destination(path);
            if (destination.empty() || destination.filename().empty()) { error = L"Choose a settings file."; return false; }
            const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::current_path();
            std::error_code fsError;
            std::filesystem::create_directories(parent, fsError);
            if (fsError) { error = L"The settings folder cannot be created."; return false; }
            wchar_t temporary[MAX_PATH]{};
            if (!GetTempFileNameW(parent.c_str(), L"cst", 0, temporary)) { error = L"The settings location is not writable."; return false; }
            const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            bool saved = file != INVALID_HANDLE_VALUE;
            if (saved)
            {
                saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
                if (!CloseHandle(file)) saved = false;
            }
            if (saved) saved = MoveFileExW(temporary, destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!saved) { DeleteFileW(temporary); error = L"Settings could not be saved. The previous server URL was kept."; }
            return saved;
        }
    }

    HttpResult ExchangeJson(const std::wstring& url, const std::wstring& method, const std::string& body,
        const std::vector<std::pair<std::wstring, std::wstring>>& headers,
        const std::shared_ptr<Cancellation>& cancel, unsigned timeoutSeconds,
        const std::function<bool(const std::string&)>& stopAfter)
    {
        try { return Exchange(url, method, body, headers, cancel, timeoutSeconds, stopAfter); }
        catch (...) { HttpResult result; result.error = L"The HTTP response could not be processed."; return result; }
    }

    bool NormalizeServerUrl(const std::wstring& input, std::wstring& normalized, std::wstring& error)
    {
        error.clear();
        auto begin = std::find_if_not(input.begin(), input.end(), [](wchar_t c) { return iswspace(c) != 0; });
        auto end = std::find_if_not(input.rbegin(), input.rend(), [](wchar_t c) { return iswspace(c) != 0; }).base();
        std::wstring url = begin < end ? std::wstring(begin, end) : std::wstring{};
        const auto invalid = [&error]() { error = L"Enter an HTTP or HTTPS server URL, such as http://127.0.0.1:8301. Do not include credentials, queries, or fragments."; return false; };
        if (url.empty() || url.size() > 2048 || std::any_of(url.begin(), url.end(), [](wchar_t c) {
            return c <= L' ' || c == 0x7f || c == L'\\' || c == L'?' || c == L'#'; })) return invalid();
        try { Utf8(url); } catch (...) { return invalid(); }
        const auto schemeEnd = url.find(L"://");
        if (schemeEnd == std::wstring::npos) return invalid();
        auto scheme = url.substr(0, schemeEnd);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), towlower);
        if (scheme != L"http" && scheme != L"https") return invalid();
        url.replace(0, schemeEnd, scheme);
        const auto authorityEnd = url.find(L'/', schemeEnd + 3);
        const auto authority = url.substr(schemeEnd + 3, authorityEnd == std::wstring::npos ? authorityEnd : authorityEnd - schemeEnd - 3);
        if (authority.empty() || authority.find(L'@') != std::wstring::npos) return invalid();
        std::wstring host, port;
        if (authority.front() == L'[')
        {
            const auto closing = authority.find(L']');
            if (closing == std::wstring::npos || closing < 3) return invalid();
            host = authority.substr(1, closing - 1);
            if (host.find(L':') == std::wstring::npos || !std::all_of(host.begin(), host.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L':' || c == L'.'; })) return invalid();
            if (closing + 1 < authority.size())
            { if (authority[closing + 1] != L':') return invalid(); port = authority.substr(closing + 2); if (port.empty()) return invalid(); }
        }
        else
        {
            const auto colon = authority.find(L':');
            host = authority.substr(0, colon);
            if (colon != std::wstring::npos) { port = authority.substr(colon + 1); if (port.empty()) return invalid(); }
            if (host.empty() || !std::all_of(host.begin(), host.end(), [](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_'; })) return invalid();
        }
        if (host == L"0.0.0.0" || host == L"::") { error = L"Use a real hostname or IP address, such as 127.0.0.1, instead of a server binding address."; return false; }
        if (!port.empty())
        {
            if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return invalid();
            const auto number = std::stoul(port);
            if (!number || number > 65535) return invalid();
        }
        URL_COMPONENTS parts{sizeof(parts)}; parts.dwHostNameLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), ICU_REJECT_USERPWD, &parts) || !parts.dwHostNameLength || parts.dwExtraInfoLength) return invalid();
        while (!url.empty() && url.back() == L'/') url.pop_back();
        normalized = std::move(url); return true;
    }
    std::wstring ApiBaseUrl(const std::wstring& normalized)
    { return normalized.size() >= 3 && normalized.compare(normalized.size() - 3, 3, L"/v1") == 0 ? normalized : normalized + L"/v1"; }
    std::wstring DefaultSettingsPath()
    {
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) return {};
        const auto path = std::filesystem::path(appData) / AppIdentity::SettingsFolder / L"connection.json";
        CoTaskMemFree(appData); return path.wstring();
    }
    bool SaveSettings(const std::wstring& path, const Settings& settings, std::wstring& error)
    {
        std::wstring normalized;
        if (!NormalizeServerUrl(settings.serverUrl, normalized, error)) return false;
        if (!std::isfinite(settings.chatFontSize) || settings.chatFontSize < 10.0f || settings.chatFontSize > 24.0f)
        { error = L"Choose a chat font size between 10 and 24."; return false; }
        try { return WriteAtomic(path, Json{{"version", 1}, {"server_url", Utf8(normalized)}, {"chat_font_size", settings.chatFontSize}}.dump(2) + "\n", error); }
        catch (...) { error = L"The Connector settings could not be saved."; return false; }
    }
    bool LoadSettings(const std::wstring& path, Settings& settings, std::wstring& error)
    {
        error.clear();
        std::error_code fsError;
        const auto length = std::filesystem::file_size(path, fsError);
        if (fsError || length > 16384) { error = L"The Connector settings file is missing or too large."; return false; }
        try
        {
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            std::string bytes(static_cast<size_t>(length), '\0');
            if (!file || (length && !file.read(bytes.data(), static_cast<std::streamsize>(length)))) throw std::runtime_error("Read failed");
            const auto json = Parse(bytes);
            if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer() || json["version"] != 1 ||
                !json.contains("server_url") || !json["server_url"].is_string()) throw std::runtime_error("Wrong format");
            std::wstring normalized;
            if (!NormalizeServerUrl(Wide(json["server_url"].get<std::string>()), normalized, error)) return false;
            float fontSize = 11.5f;
            if (json.contains("chat_font_size"))
            {
                if (!json["chat_font_size"].is_number()) throw std::runtime_error("Invalid font size type");
                fontSize = json["chat_font_size"].get<float>();
                if (!std::isfinite(fontSize) || fontSize < 10.0f || fontSize > 24.0f) throw std::runtime_error("Invalid font size range");
            }
            settings.serverUrl = std::move(normalized); settings.chatFontSize = fontSize; return true;
        }
        catch (...) { error = L"The Connector settings file is invalid. The current settings were kept."; return false; }
    }
    DiscoveryResult DiscoverModels(const Settings& settings, const std::shared_ptr<Cancellation>& cancel)
    {
        DiscoveryResult result;
        try
        {
            auto response = Http(settings, L"GET", L"/models", {}, cancel, true);
            result.cancelled = response.cancelled; result.error = response.error;
            if (!response.success) return result;
            const auto json = Parse(response.body);
            if (!json.is_object() || !json.contains("data") || !json["data"].is_array() || json["data"].size() > 1000) throw std::runtime_error("Model list shape");
            std::set<std::wstring> ids;
            for (const auto& item : json["data"])
            {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) throw std::runtime_error("Model ID missing");
                auto id = Wide(item["id"].get<std::string>());
                if (!ValidModelId(id)) throw std::runtime_error("Unsafe model ID");
                if (!ids.insert(id).second) continue;
                std::wstring name = id;
                if (item.contains("name") && item["name"].is_string())
                {
                    auto display = Wide(item["name"].get<std::string>());
                    if (!display.empty() && display.size() <= 200 && std::none_of(display.begin(), display.end(), [](wchar_t c) { return c < L' ' || c == 0x7f; })) name = std::move(display);
                }
                result.models.push_back({std::move(id), std::move(name)});
            }
            result.success = true;
        }
        catch (...) { result.models.clear(); result.error = L"The server returned an invalid model list. Use a Connector server with the /v1/models API."; }
        return result;
    }
    CompletionResult Complete(const Settings& settings, const std::wstring& model,
        const std::vector<ChatMessage>& messages, const std::shared_ptr<Cancellation>& cancel)
    {
        CompletionResult result;
        if (Cancelled(cancel)) { result.cancelled = true; return result; }
        try
        {
            if (!ValidModelId(model) || messages.empty() || messages.size() > 10000)
            { result.error = L"Choose an active Connector model and enter a message."; return result; }
            // Encode incrementally, stopping before constructing an oversized DOM
            // or duplicating a very large conversation into the final request.
            std::string body = "{\"model\":" + Json(Utf8(model)).dump() + ",\"stream\":false,\"messages\":[";
            bool first = true;
            for (const auto& message : messages)
            {
                if (message.role != L"user" && message.role != L"assistant" && message.role != L"system" && message.role != L"developer")
                { result.error = L"The conversation contains an unsupported message role."; return result; }
                const auto content = Utf8(message.content);
                if (content.size() > MaxRequest - body.size())
                { result.error = L"The conversation exceeds the 16 MiB request limit. Start a new session."; return result; }
                const auto encoded = Json{{"role", Utf8(message.role)}, {"content", content}}.dump();
                if (encoded.size() + body.size() + 3 > MaxRequest)
                { result.error = L"The conversation exceeds the 16 MiB request limit. Start a new session."; return result; }
                if (!first) body += ',';
                body += encoded; first = false;
            }
            body += "]}";
            auto response = Http(settings, L"POST", L"/chat/completions", body, cancel, false);
            result.cancelled = response.cancelled; result.error = response.error;
            if (!response.success) return result;
            const auto json = Parse(response.body);
            if (!json.is_object() || !json.contains("choices") || !json["choices"].is_array() || json["choices"].empty()) throw std::runtime_error("Choices missing");
            const auto& choice = json["choices"][0];
            if (!choice.is_object() || !choice.contains("message") || !choice["message"].is_object()) throw std::runtime_error("Message missing");
            const auto& message = choice["message"];
            if (message.contains("role") && message["role"] != "assistant") throw std::runtime_error("Unexpected response role");
            if (message.contains("function_call") && !message["function_call"].is_null())
            { result.error = L"The model returned a function call. Studio's text connection does not execute tools."; return result; }
            if (message.contains("tool_calls") && !message["tool_calls"].is_null() &&
                (!message["tool_calls"].is_array() || !message["tool_calls"].empty()))
            { result.error = L"The model returned tool calls. Studio's text connection does not execute tools; choose a text-capable configuration."; return result; }
            std::string text;
            if (message.contains("content") && message["content"].is_string()) text = message["content"].get<std::string>();
            else if (message.contains("refusal") && message["refusal"].is_string()) text = message["refusal"].get<std::string>();
            else if (message.contains("content") && message["content"].is_null() && choice.value("finish_reason", std::string{}) == "content_filter")
                result.text = L"The server filtered this response.";
            else throw std::runtime_error("Text missing");
            // SessionPersistence limits a saved string to4MiB of UTF-8 bytes.
            // Reject before showing a reply that could never be saved.
            if (text.size() > 4 * 1024 * 1024)
            { result.error = L"The Connector reply exceeds the 4 MiB text limit. Start a shorter conversation."; return result; }
            if (!text.empty()) result.text = Wide(text);
            if (result.text.size() > 4 * 1024 * 1024) throw std::runtime_error("Text too large");
            result.success = true;
        }
        catch (...) { result.error = L"The Connector request or reply contains invalid text or JSON."; }
        return result;
    }
}
