#include "McpClient.h"
#include "../third_party/nlohmann/json.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Lattice::Mcp
{
    using Json = nlohmann::json;
    struct Connection::State
    {
        HANDLE input = nullptr;
        HANDLE output = nullptr;
        HANDLE process = nullptr;
        HANDLE job = nullptr;
        std::atomic<bool> stop{false};
        std::atomic<bool> alive{false};
        bool http = false;
        bool legacy = false;
        std::wstring url;
        std::wstring session;
        std::wstring protocol;
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        ~State()
        {
            if (input) CloseHandle(input);
            if (output) CloseHandle(output);
            if (job) CloseHandle(job); // KILL_ON_JOB_CLOSE also catches descendant processes.
            if (process) CloseHandle(process);
        }
    };
    struct ClientAccess
    {
        using State = Connection::State;
        static std::shared_ptr<Connection> Make(std::shared_ptr<State> state)
        { return std::shared_ptr<Connection>(new Connection(std::move(state))); }
    };
    namespace
    {
        using State = ClientAccess::State;
        constexpr size_t MaxConfigBytes = 256 * 1024;
        constexpr size_t MaxMessageBytes = 1024 * 1024;
        constexpr auto ProbeTimeout = std::chrono::seconds(3);
        constexpr auto InitializeTimeout = std::chrono::seconds(12);
        constexpr const char* ModernVersion = "2026-07-28";
        constexpr const char* LegacyVersion = "2025-11-25";
        void DisconnectHttp(const std::shared_ptr<State>& state);
        bool Cancelled(const std::shared_ptr<Connector::Cancellation>& token) { return token && token->IsCancelled(); }
        std::wstring Wide(const std::string& value)
        {
            if (value.empty()) return {};
            if (value.find('\0') != std::string::npos) throw std::runtime_error("embedded NUL");
            const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (!size) throw std::runtime_error("invalid UTF-8");
            std::wstring output(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size);
            return output;
        }
        std::string Utf8(const std::wstring& value)
        {
            if (value.empty()) return {};
            if (value.find(L'\0') != std::wstring::npos) throw std::runtime_error("embedded NUL");
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (!size) throw std::runtime_error("invalid UTF-16");
            std::string output(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
            return output;
        }
        Json ParseJson(const std::string& value)
        {
            if (value.size() > MaxMessageBytes) throw std::runtime_error("MCP message is too large");
            return Json::parse(value, [](int depth, Json::parse_event_t, Json&) {
                if (depth > 64) throw std::runtime_error("JSON nesting is too deep");
                return true;
            });
        }
        struct Config
        {
            bool http = false;
            std::wstring url;
            std::wstring command;
            std::vector<std::wstring> args;
            std::vector<std::pair<std::wstring, std::wstring>> headers;
            std::vector<std::pair<std::wstring, std::wstring>> environment;
        };
        Json ReadConfig(const std::wstring& folder)
        {
            const auto path = std::filesystem::path(folder) / L"mcp.json";
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Add an mcp.json configuration inside this MCP folder.");
            if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("mcp.json must be a regular file inside this MCP folder.");
            std::error_code code;
            const auto size = std::filesystem::file_size(path, code);
            if (code) throw std::runtime_error("Add an mcp.json configuration inside this MCP folder.");
            if (size > MaxConfigBytes) throw std::runtime_error("mcp.json is too large (maximum 256 KiB).");
            std::ifstream file(path, std::ios::binary);
            std::string contents(static_cast<size_t>(size), '\0');
            if (!file || (size && !file.read(contents.data(), static_cast<std::streamsize>(size)))) throw std::runtime_error("Cannot read mcp.json.");
            auto document = ParseJson(contents);
            if (!document.is_object()) throw std::runtime_error("mcp.json must contain an object.");
            return document;
        }
        Config LoadConfig(const std::wstring& folder, const std::wstring& serverKey)
        {
            auto document = ReadConfig(folder);
            if (document.contains("servers") || document.contains("mcpServers"))
            {
                const auto& servers = document.at(document.contains("servers") ? "servers" : "mcpServers");
                if (!servers.is_object() || servers.empty() || servers.size() > 100) throw std::runtime_error("mcp.json needs a nonempty servers object.");
                if (!serverKey.empty())
                {
                    const auto key = Utf8(serverKey);
                    if (!servers.contains(key)) throw std::runtime_error("The selected MCP server is no longer in mcp.json.");
                    document = servers.at(key);
                }
                else
                {
                    if (servers.size() != 1) throw std::runtime_error("Select a server from this folder's mcp.json configuration.");
                    document = servers.begin().value();
                }
            }
            if (!document.is_object()) throw std::runtime_error("The MCP server configuration must be an object.");
            Config config;
            const auto type = document.value("type", std::string());
            config.http = document.contains("url") && !document.contains("command");
            if (config.http)
            {
                if (!type.empty() && type != "http" && type != "streamable-http") throw std::runtime_error("Use type http for this MCP endpoint.");
                if (!document.at("url").is_string()) throw std::runtime_error("The MCP URL must be text.");
                config.url = Wide(document.at("url").get<std::string>());
                std::wstring normalized, error;
                if (!Connector::NormalizeServerUrl(config.url, normalized, error)) throw std::runtime_error("The MCP URL must be a valid HTTP or HTTPS endpoint without credentials or fragments.");
                // Preserve the configured endpoint's slash; MCP endpoints do not use /v1.
                if (document.contains("headers"))
                {
                    const auto& headers = document.at("headers");
                    if (!headers.is_object() || headers.size() > 32) throw std::runtime_error("MCP headers must be a bounded object of text values.");
                    for (const auto& entry : headers.items())
                    {
                        if (entry.key().empty() || entry.key().size() > 100 || !entry.value().is_string()) throw std::runtime_error("Invalid MCP header.");
                        const auto name = Wide(entry.key());
                        const auto value = Wide(entry.value().get<std::string>());
                        if (value.size() > 8192) throw std::runtime_error("MCP header value is too long.");
                        for (const auto ch : name)
                            if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-')) throw std::runtime_error("Invalid MCP header name.");
                        for (const auto ch : value) if (ch < 0x20 || ch > 0x7e) throw std::runtime_error("MCP header values must be plain ASCII without line breaks.");
                        if (!_wcsicmp(name.c_str(), L"Content-Length") || !_wcsicmp(name.c_str(), L"Host") ||
                            !_wcsicmp(name.c_str(), L"Connection") || !_wcsicmp(name.c_str(), L"MCP-Protocol-Version") ||
                            !_wcsicmp(name.c_str(), L"Mcp-Method") || !_wcsicmp(name.c_str(), L"Mcp-Session-Id") ||
                            !_wcsicmp(name.c_str(), L"Content-Type") || !_wcsicmp(name.c_str(), L"Accept")) throw std::runtime_error("MCP transport headers cannot be overridden in mcp.json.");
                        config.headers.push_back({name, value});
                    }
                }
            }
            else
            {
                if (!type.empty() && type != "stdio") throw std::runtime_error("Use type stdio for an MCP command.");
                if (!document.contains("command") || !document.at("command").is_string()) throw std::runtime_error("Configure an MCP command or HTTP URL.");
                config.command = Wide(document.at("command").get<std::string>());
                if (config.command.empty() || config.command.size() > 4096) throw std::runtime_error("Invalid MCP command.");
                if (document.contains("args"))
                {
                    if (!document.at("args").is_array() || document.at("args").size() > 128) throw std::runtime_error("MCP args must be an array with at most 128 text arguments.");
                    for (const auto& arg : document.at("args"))
                    {
                        if (!arg.is_string()) throw std::runtime_error("MCP arguments must be text.");
                        const auto value = Wide(arg.get<std::string>());
                        if (value.size() > 8192) throw std::runtime_error("An MCP argument is too long.");
                        config.args.push_back(value);
                    }
                }
                if (document.contains("env"))
                {
                    if (!document.at("env").is_object() || document.at("env").size() > 64) throw std::runtime_error("MCP env must be a bounded object of text values.");
                    for (const auto& entry : document.at("env").items())
                    {
                        if (!entry.value().is_string()) throw std::runtime_error("MCP environment values must be text.");
                        const auto key = Wide(entry.key()), value = Wide(entry.value().get<std::string>());
                        if (key.empty() || key.size() > 200 || key.find(L'=') != std::wstring::npos || value.size() > 8192) throw std::runtime_error("Invalid MCP environment entry.");
                        config.environment.push_back({key, value});
                    }
                }
            }
            return config;
        }
        std::wstring QuoteArgument(const std::wstring& value)
        {
            std::wstring output = L"\"";
            size_t slashes = 0;
            for (const auto ch : value)
            {
                if (ch == L'\\') { ++slashes; continue; }
                if (ch == L'\"') output.append(slashes * 2 + 1, L'\\');
                else output.append(slashes, L'\\');
                slashes = 0;
                output.push_back(ch);
            }
            output.append(slashes * 2, L'\\');
            output.push_back(L'\"');
            return output;
        }
        std::wstring FindExecutable(const std::wstring& command, const std::wstring& folder)
        {
            auto candidate = std::filesystem::path(command);
            std::error_code error;
            if (!candidate.is_absolute()) candidate = std::filesystem::path(folder) / candidate;
            if (std::filesystem::is_regular_file(candidate, error)) return std::filesystem::absolute(candidate).wstring();
            std::vector<wchar_t> path(32768);
            for (const auto extension : {L".exe", L".cmd"})
            {
                const auto length = SearchPathW(nullptr, command.c_str(), extension, static_cast<DWORD>(path.size()), path.data(), nullptr);
                if (length && length < path.size()) return std::wstring(path.data(), length);
            }
            return {};
        }
        struct EnvironmentLess
        { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
        std::vector<wchar_t> EnvironmentBlock(const Config& config)
        {
            if (config.environment.empty()) return {};
            std::map<std::wstring, std::wstring, EnvironmentLess> entries;
            const auto inherited = GetEnvironmentStringsW();
            if (!inherited) throw std::runtime_error("Cannot read the process environment.");
            for (const wchar_t* item = inherited; *item; item += wcslen(item) + 1)
            {
                const std::wstring entry(item);
                const auto separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
                if (separator != std::wstring::npos) entries.emplace(entry.substr(0, separator), entry.substr(separator + 1));
            }
            FreeEnvironmentStringsW(inherited);
            for (const auto& entry : config.environment) entries[entry.first] = entry.second;
            std::vector<wchar_t> block;
            for (const auto& entry : entries)
            {
                const auto value = entry.first + L"=" + entry.second;
                block.insert(block.end(), value.begin(), value.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            if (block.size() > 65535) throw std::runtime_error("The MCP environment is too large.");
            return block;
        }
        std::shared_ptr<State> Launch(Config config, const std::wstring& folder)
        {
            auto executable = FindExecutable(config.command, folder);
            if (executable.empty()) throw std::runtime_error("Cannot find the configured MCP executable. Install it or specify its full path.");
            const auto path = std::filesystem::path(executable);
            if (!_wcsicmp(path.extension().c_str(), L".cmd") || !_wcsicmp(path.extension().c_str(), L".bat"))
            {
                if (_wcsicmp(path.stem().c_str(), L"npx")) throw std::runtime_error("MCP batch wrappers are not supported. Use the executable directly.");
                const auto cli = path.parent_path() / L"node_modules" / L"npm" / L"bin" / L"npx-cli.js";
                if (!std::filesystem::is_regular_file(cli)) throw std::runtime_error("Cannot locate npx-cli.js beside the npx wrapper.");
                auto node = (path.parent_path() / L"node.exe").wstring();
                if (!std::filesystem::is_regular_file(node)) node = FindExecutable(L"node", folder);
                if (node.empty()) throw std::runtime_error("Node.js is required by this configured npx MCP server.");
                executable = node;
                config.args.insert(config.args.begin(), cli.wstring());
            }
            if (_wcsicmp(std::filesystem::path(executable).extension().c_str(), L".exe")) throw std::runtime_error("Use an .exe MCP command, or the supported npx command.");
            std::wstring commandLine = QuoteArgument(executable);
            for (const auto& arg : config.args) commandLine += L" " + QuoteArgument(arg);
            if (commandLine.size() >= 32767) throw std::runtime_error("The MCP command line is too long.");
            auto environment = EnvironmentBlock(config);
            auto state = std::make_shared<State>();
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            HANDLE childInput = nullptr, childOutput = nullptr, childError = nullptr;
            const auto closeChildHandles = [&] {
                if (childInput) { CloseHandle(childInput); childInput = nullptr; }
                if (childOutput) { CloseHandle(childOutput); childOutput = nullptr; }
                if (childError && childError != INVALID_HANDLE_VALUE) { CloseHandle(childError); childError = nullptr; }
            };
            if (!CreatePipe(&childInput, &state->input, &attributes, 8192) || !CreatePipe(&state->output, &childOutput, &attributes, 8192))
            { closeChildHandles(); throw std::runtime_error("Cannot create MCP stdio pipes."); }
            SetHandleInformation(state->input, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(state->output, HANDLE_FLAG_INHERIT, 0);
            childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
            if (childError == INVALID_HANDLE_VALUE) { closeChildHandles(); throw std::runtime_error("Cannot create the MCP log sink."); }
            state->job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!state->job || !SetInformationJobObject(state->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            { closeChildHandles(); throw std::runtime_error("Cannot contain the MCP server process."); }
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<BYTE> storage(size);
            auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) { closeChildHandles(); throw std::runtime_error("Cannot prepare MCP process handles."); }
            HANDLE handles[] = {childInput, childOutput, childError};
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.StartupInfo.wShowWindow = SW_HIDE;
            startup.StartupInfo.hStdInput = childInput;
            startup.StartupInfo.hStdOutput = childOutput;
            startup.StartupInfo.hStdError = childError;
            startup.lpAttributeList = list;
            PROCESS_INFORMATION process{};
            const bool attributesReady = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) != FALSE;
            const bool created = attributesReady && CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                environment.empty() ? nullptr : environment.data(), folder.c_str(), &startup.StartupInfo, &process) != FALSE;
            DeleteProcThreadAttributeList(list);
            closeChildHandles();
            if (!created) throw std::runtime_error("Cannot launch the configured MCP server.");
            state->process = process.hProcess;
            if (!AssignProcessToJobObject(state->job, process.hProcess))
            {
                TerminateProcess(process.hProcess, 1);
                CloseHandle(process.hThread);
                throw std::runtime_error("Cannot contain the MCP server process in its owned job.");
            }
            if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
            { CloseHandle(process.hThread); throw std::runtime_error("Cannot start the MCP server process."); }
            CloseHandle(process.hThread);
            return state;
        }
        bool SendStdio(const std::shared_ptr<State>& state, const Json& message)
        {
            auto bytes = message.dump();
            bytes.push_back('\n');
            if (bytes.size() > 4096 || !state->input) return false;
            DWORD written = 0;
            return WriteFile(state->input, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        }
        bool AnswerLegacyRequest(const std::shared_ptr<State>& state, const Json& message)
        {
            if (!message["method"].is_string() || !(message["id"].is_string() || message["id"].is_number_integer())) return false;
            if (message["method"] == "ping") return SendStdio(state, {{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", Json::object()}});
            return SendStdio(state, {{"jsonrpc", "2.0"}, {"id", message["id"]},
                {"error", {{"code", -32601}, {"message", "Client method not supported"}}}});
        }
        struct RpcReply
        {
            Json message;
            bool received = false;
            bool timedOut = false;
            bool cancelled = false;
            unsigned long status = 0;
            std::wstring error;
            std::wstring session;
        };
        RpcReply ReceiveStdio(const std::shared_ptr<State>& state, const Json& id, std::string& pending,
            const std::shared_ptr<Connector::Cancellation>& token, std::chrono::steady_clock::duration timeout)
        {
            RpcReply result;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            size_t messages = 0, bytes = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (Cancelled(token)) { result.cancelled = true; result.error = L"MCP connection cancelled."; return result; }
                for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
                {
                    if (++messages > 200) { result.error = L"MCP server sent too many messages during connection."; return result; }
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    try
                    {
                        auto message = ParseJson(line);
                        if (!message.is_object() || message.value("jsonrpc", std::string()) != "2.0") throw std::runtime_error("invalid JSON-RPC");
                        if (message.contains("id") && message["id"] == id && (message.contains("result") || message.contains("error")))
                        { result.message = std::move(message); result.received = true; return result; }
                        if (message.contains("method") && message.contains("id"))
                        {
                            if (!AnswerLegacyRequest(state, message))
                            { result.error = L"Cannot answer the MCP server's connection request."; return result; }
                        }
                    }
                    catch (...) { result.error = L"MCP stdout must contain newline-delimited JSON-RPC messages only."; return result; }
                }
                DWORD available = 0;
                if (!PeekNamedPipe(state->output, nullptr, 0, nullptr, &available, nullptr))
                { result.error = L"MCP server closed its output before connection completed."; return result; }
                if (available)
                {
                    char buffer[4096]; DWORD read = 0;
                    if (!ReadFile(state->output, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &read, nullptr) || !read)
                    { result.error = L"Cannot read MCP server output."; return result; }
                    bytes += read;
                    if (bytes > MaxMessageBytes || pending.size() + read > MaxMessageBytes)
                    { result.error = L"MCP server response exceeds the connection size limit."; return result; }
                    pending.append(buffer, read);
                }
                else if (WaitForSingleObject(state->process, 0) == WAIT_OBJECT_0)
                { result.error = L"MCP server exited before connection completed."; return result; }
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            result.timedOut = true;
            result.error = L"MCP server connection timed out.";
            return result;
        }
        Json DiscoverRequest(int id)
        {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "server/discover"}, {"params", {
                {"_meta", {{"io.modelcontextprotocol/protocolVersion", ModernVersion},
                {"io.modelcontextprotocol/clientInfo", {{"name", "Connector Studio"}, {"version", "1.2.0"}}},
                {"io.modelcontextprotocol/clientCapabilities", Json::object()}}}}}};
        }
        Json InitializeRequest(int id)
        {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", {
                {"protocolVersion", LegacyVersion}, {"capabilities", Json::object()},
                {"clientInfo", {{"name", "Connector Studio"}, {"version", "1.2.0"}}}}}};
        }
        bool ModernError(const Json& message)
        {
            if (!message.contains("error") || !message["error"].is_object() || !message["error"].contains("code") || !message["error"]["code"].is_number_integer()) return false;
            const auto code = message["error"]["code"].get<int>();
            return code == -32020 || code == -32021 || code == -32022;
        }
        bool ModernResult(const Json& message)
        {
            if (!message.contains("result") || !message["result"].is_object()) return false;
            const auto& result = message["result"];
            if (!result.contains("supportedVersions") || !result["supportedVersions"].is_array() ||
                result["supportedVersions"].size() > 32 ||
                !result.contains("capabilities") || !result["capabilities"].is_object() || result.value("resultType", std::string()) != "complete") return false;
            for (const auto& version : result["supportedVersions"]) if (version.is_string() && version.get<std::string>() == ModernVersion) return true;
            return false;
        }
        bool LegacyResult(const Json& message, std::wstring& version)
        {
            if (!message.contains("result") || !message["result"].is_object()) return false;
            const auto& result = message["result"];
            if (!result.contains("protocolVersion") || !result["protocolVersion"].is_string() ||
                !result.contains("capabilities") || !result["capabilities"].is_object() ||
                !result.contains("serverInfo") || !result["serverInfo"].is_object()) return false;
            const auto& identity = result["serverInfo"];
            if (!identity.contains("name") || !identity["name"].is_string() || !identity.contains("version") || !identity["version"].is_string()) return false;
            const auto selected = result["protocolVersion"].get<std::string>();
            if (selected != "2025-11-25" && selected != "2025-06-18" && selected != "2025-03-26") return false;
            version = Wide(selected);
            return true;
        }
        void StartMonitor(const std::shared_ptr<State>& state, std::string pending)
        {
            std::thread([state, pending = std::move(pending)]() mutable {
                size_t messages = 0;
                auto period = std::chrono::steady_clock::now();
                bool valid = true;
                while (!state->stop.load())
                {
                    if (std::chrono::steady_clock::now() - period >= std::chrono::seconds(3))
                    { period = std::chrono::steady_clock::now(); messages = 0; }
                    for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
                    {
                        if (++messages > 200) { valid = false; break; }
                        auto line = pending.substr(0, end);
                        pending.erase(0, end + 1);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        try
                        {
                            const auto message = ParseJson(line);
                            if (!message.is_object() || message.value("jsonrpc", std::string()) != "2.0") { valid = false; break; }
                            if (message.contains("method") && message.contains("id"))
                            {
                                if (!state->legacy || !AnswerLegacyRequest(state, message)) { valid = false; break; }
                            }
                        }
                        catch (...) { valid = false; break; }
                        if (state->stop.load()) break;
                    }
                    if (!valid || state->stop.load()) break;
                    if (WaitForSingleObject(state->process, 0) == WAIT_OBJECT_0) break;
                    DWORD available = 0;
                    if (!PeekNamedPipe(state->output, nullptr, 0, nullptr, &available, nullptr)) break;
                    if (available)
                    {
                        char bytes[4096]; DWORD read = 0;
                        if (!ReadFile(state->output, bytes, (std::min)(available, static_cast<DWORD>(sizeof(bytes))), &read, nullptr)) break;
                        if (pending.size() + read > MaxMessageBytes) break;
                        pending.append(bytes, read);
                    }
                    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                state->alive.store(false);
                if (state->input) { CloseHandle(state->input); state->input = nullptr; }
                if (WaitForSingleObject(state->process, 200) != WAIT_OBJECT_0) TerminateJobObject(state->job, 0);
            }).detach();
        }
        struct HandshakeGuard
        {
            std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
            HandshakeGuard(const std::shared_ptr<State>& state, const std::shared_ptr<Connector::Cancellation>& token)
            {
                std::thread([state, token, finished = done] {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(16);
                    while (!finished->load() && !Cancelled(token) && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    // Killing the owned job breaks even a blocked synchronous pipe write.
                    if (!finished->load()) TerminateJobObject(state->job, 0);
                }).detach();
            }
            ~HandshakeGuard() { done->store(true); }
        };
        ConnectionResult ConnectStdio(const Config& config, const std::wstring& folder,
            const std::shared_ptr<Connector::Cancellation>& token)
        {
            ConnectionResult result;
            auto state = Launch(config, folder);
            HandshakeGuard guard(state, token);
            std::string pending;
            if (!SendStdio(state, DiscoverRequest(1))) { result.error = L"Cannot send MCP discovery request."; return result; }
            auto reply = ReceiveStdio(state, 1, pending, token, ProbeTimeout);
            if (reply.cancelled) { result.cancelled = true; result.error = reply.error; return result; }
            if (reply.received && ModernResult(reply.message)) state->protocol = Wide(ModernVersion);
            else
            {
                if (reply.received && (ModernError(reply.message) || reply.message.contains("result")))
                { result.error = L"MCP server does not advertise a compatible modern protocol version."; return result; }
                if (!reply.received && !reply.timedOut) { result.error = reply.error; return result; }
                // Legacy implementations can ignore the probe or return any unknown-method error.
                state->legacy = true;
                if (reply.timedOut) SendStdio(state, {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}});
                if (Cancelled(token)) { result.cancelled = true; result.error = L"MCP connection cancelled."; return result; }
                if (!SendStdio(state, InitializeRequest(2))) { result.error = L"Cannot send MCP initialization request."; return result; }
                reply = ReceiveStdio(state, 2, pending, token, InitializeTimeout);
                if (reply.cancelled) { result.cancelled = true; result.error = reply.error; return result; }
                if (!reply.received) { result.error = reply.error; return result; }
                if (!LegacyResult(reply.message, state->protocol)) { result.error = L"MCP server initialization failed or returned an unsupported version."; return result; }
                if (!SendStdio(state, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}))
                { result.error = L"Cannot finish MCP initialization."; return result; }
            }
            if (Cancelled(token)) { result.cancelled = true; result.error = L"MCP connection cancelled."; return result; }
            state->alive.store(true);
            auto connection = ClientAccess::Make(state);
            StartMonitor(state, std::move(pending));
            result.connection = std::move(connection);
            result.success = true;
            return result;
        }
        RpcReply SseReply(const std::string& body, const Json& id)
        {
            RpcReply reply;
            size_t position = 0, events = 0;
            std::string data;
            while (position < body.size())
            {
                const auto end = body.find('\n', position);
                if (end == std::string::npos) break; // A partially received event is not complete yet.
                auto line = body.substr(position, end - position);
                position = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty())
                {
                    if (data.empty()) continue;
                    if (++events > 200) { reply.error = L"MCP response contains too many SSE events."; return reply; }
                    try
                    {
                        const auto message = ParseJson(data);
                        if (message.is_object() && message.value("jsonrpc", std::string()) == "2.0" &&
                            message.contains("id") && message["id"] == id &&
                            (message.contains("result") || message.contains("error")))
                        { reply.received = true; reply.message = message; return reply; }
                        if (message.is_object() && message.contains("method") && message.contains("id"))
                        { reply.error = L"This MCP server requested an unsupported client feature."; return reply; }
                    }
                    catch (...) { reply.error = L"MCP SSE event contains invalid JSON-RPC."; return reply; }
                    data.clear();
                }
                else if (line.rfind("data:", 0) == 0)
                {
                    auto start = size_t(5);
                    if (start < line.size() && line[start] == ' ') ++start;
                    if (!data.empty()) data.push_back('\n');
                    data += line.substr(start);
                    if (data.size() > MaxMessageBytes) { reply.error = L"MCP SSE event is too large."; return reply; }
                }
            }
            return reply;
        }
        RpcReply HttpRpc(const Config& config, const Json& message, const std::wstring& version,
            const std::wstring& session, const std::shared_ptr<Connector::Cancellation>& token,
            unsigned timeout)
        {
            auto headers = config.headers;
            headers.push_back({L"Accept", L"application/json, text/event-stream"});
            headers.push_back({L"MCP-Protocol-Version", version});
            if (message.contains("method")) headers.push_back({L"Mcp-Method", Wide(message["method"].get<std::string>())});
            if (!session.empty()) headers.push_back({L"Mcp-Session-Id", session});
            const auto id = message.contains("id") ? message["id"] : Json();
            const auto response = Connector::ExchangeJson(config.url, L"POST", message.dump(), headers, token, timeout,
                [&](const std::string& bytes) { return bytes.size() > MaxMessageBytes || (!id.is_null() && SseReply(bytes, id).received); });
            RpcReply reply;
            reply.status = response.status;
            reply.session = response.mcpSessionId;
            reply.cancelled = response.cancelled;
            if (response.cancelled) { reply.error = L"MCP connection cancelled."; return reply; }
            if (id.is_null())
            {
                reply.received = response.success && (response.status == 202 || response.status == 204 || response.status == 200);
                if (!reply.received) reply.error = response.status ? L"MCP initialization notification was rejected." : response.error;
                return reply;
            }
            if (response.body.size() > MaxMessageBytes) { reply.error = L"MCP response exceeds the connection size limit."; return reply; }
            auto contentType = response.contentType;
            std::transform(contentType.begin(), contentType.end(), contentType.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (contentType.rfind(L"text/event-stream", 0) == 0)
            {
                reply = SseReply(response.body, id);
                reply.status = response.status;
                reply.session = response.mcpSessionId;
            }
            else if (!response.body.empty())
            {
                if (response.success && contentType.rfind(L"application/json", 0) != 0)
                { reply.error = L"MCP response must use application/json or text/event-stream."; return reply; }
                try
                {
                    auto parsed = ParseJson(response.body);
                    if (parsed.is_object() && parsed.value("jsonrpc", std::string()) == "2.0" &&
                        ((!parsed.contains("id") && parsed.contains("error")) || (parsed.contains("id") && parsed["id"] == id)) &&
                        (parsed.contains("result") != parsed.contains("error")))
                    { reply.message = std::move(parsed); reply.received = true; }
                    else reply.error = L"MCP server returned an invalid JSON-RPC response or mismatched request ID.";
                }
                catch (...) { reply.error = L"MCP server returned invalid JSON-RPC."; }
            }
            if (!response.success && !reply.received)
            {
                if (response.status == 401 || response.status == 403) reply.error = L"MCP server rejected authentication. Configure its required headers in mcp.json.";
                else if (response.status) reply.error = L"MCP server returned HTTP " + std::to_wstring(response.status) + L".";
                else reply.error = response.error.empty() ? L"Cannot reach the configured MCP endpoint." : response.error;
            }
            if (!reply.received && reply.error.empty()) reply.error = L"MCP server did not return a complete connection response.";
            return reply;
        }
        ConnectionResult ConnectHttp(const Config& config, const std::shared_ptr<Connector::Cancellation>& token)
        {
            ConnectionResult result;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            const auto remaining = [&] {
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count();
                return static_cast<unsigned>((std::max)(int64_t(1), seconds));
            };
            auto reply = HttpRpc(config, DiscoverRequest(1), Wide(ModernVersion), {}, token, (std::min)(remaining(), 6U));
            if (reply.cancelled) { result.cancelled = true; result.error = reply.error; return result; }
            auto state = std::make_shared<State>();
            state->http = true;
            state->url = config.url;
            state->headers = config.headers;
            if (reply.received && reply.status >= 200 && reply.status < 300 && ModernResult(reply.message)) state->protocol = Wide(ModernVersion);
            else
            {
                if (reply.received && (ModernError(reply.message) || reply.message.contains("result")))
                { result.error = L"MCP server does not advertise a compatible modern protocol version."; return result; }
                // Authentication failures, timeouts, malformed success replies, and unrelated
                // HTTP failures stop here. Only a bounded legacy compatibility handshake follows.
                if (reply.status != 400 && reply.status != 404 && !(reply.received && reply.message.contains("error") && reply.status >= 200 && reply.status < 300))
                { result.error = reply.error.empty() ? L"MCP discovery was rejected." : reply.error; return result; }
                if (Cancelled(token) || std::chrono::steady_clock::now() >= deadline)
                { result.cancelled = Cancelled(token); result.error = L"MCP connection cancelled or timed out."; return result; }
                reply = HttpRpc(config, InitializeRequest(2), Wide(LegacyVersion), {}, token, remaining());
                if (reply.cancelled) { result.cancelled = true; result.error = reply.error; return result; }
                if (!reply.received || reply.status < 200 || reply.status >= 300 || !LegacyResult(reply.message, state->protocol))
                { result.error = reply.error.empty() ? L"MCP initialization failed or returned an unsupported version." : reply.error; return result; }
                state->session = reply.session;
                for (const auto ch : state->session)
                    if (ch < 0x21 || ch > 0x7e) { result.error = L"MCP server returned an invalid session identifier."; return result; }
                if (state->session.size() > 1024) { result.error = L"MCP server returned an oversized session identifier."; return result; }
                const auto initialized = HttpRpc(config, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
                    state->protocol, state->session, token, remaining());
                if (!initialized.received)
                {
                    result.cancelled = initialized.cancelled;
                    result.error = initialized.error;
                    DisconnectHttp(state);
                    return result;
                }
            }
            if (Cancelled(token)) { result.cancelled = true; result.error = L"MCP connection cancelled."; DisconnectHttp(state); return result; }
            state->alive.store(true);
            result.connection = ClientAccess::Make(std::move(state));
            result.success = true;
            return result;
        }
        void DisconnectHttp(const std::shared_ptr<State>& state)
        {
            if (state->session.empty()) return; // Modern MCP is stateless.
            try
            {
                std::thread([state] {
                    auto headers = state->headers;
                    headers.push_back({L"MCP-Protocol-Version", state->protocol});
                    headers.push_back({L"Mcp-Session-Id", state->session});
                    Connector::ExchangeJson(state->url, L"DELETE", {}, headers, {}, 3);
                }).detach();
            }
            catch (...) { /* A disconnect never blocks the application's message loop. */ }
        }
    }

    Connection::Connection(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    Connection::~Connection() { Disconnect(); }
    bool Connection::IsAlive() const { return m_state && m_state->alive.load() && !m_state->stop.load(); }
    void Connection::Disconnect()
    {
        if (!m_state || m_state->stop.exchange(true)) return;
        m_state->alive.store(false);
        if (m_state->http) DisconnectHttp(m_state);
        else
        {
            try
            {
                std::thread([state = m_state] {
                    if (WaitForSingleObject(state->process, 250) != WAIT_OBJECT_0) TerminateJobObject(state->job, 0);
                }).detach();
            }
            catch (...) { TerminateJobObject(m_state->job, 0); }
        }
    }
    ConnectionResult Connect(const std::wstring& folder, std::shared_ptr<Connector::Cancellation> cancellation, const std::wstring& serverKey)
    {
        ConnectionResult result;
        if (Cancelled(cancellation)) { result.cancelled = true; result.error = L"MCP connection cancelled."; return result; }
        try
        {
            const auto config = LoadConfig(folder, serverKey);
            result = config.http ? ConnectHttp(config, cancellation) : ConnectStdio(config, folder, cancellation);
            if (Cancelled(cancellation))
            {
                result.connection.reset();
                result.success = false;
                result.cancelled = true;
                result.error = L"MCP connection cancelled.";
            }
            return result;
        }
        catch (const Json::exception&)
        {
            result.cancelled = Cancelled(cancellation);
            result.error = L"mcp.json contains invalid JSON or an invalid MCP configuration value.";
            return result;
        }
        catch (const std::exception& error)
        {
            result.cancelled = Cancelled(cancellation);
            try { result.error = Wide(error.what()); } catch (...) { result.error = L"Cannot connect to this MCP server configuration."; }
            return result;
        }
    }
    std::vector<std::wstring> ServerKeys(const std::wstring& folder)
    {
        try
        {
            const auto document = ReadConfig(folder);
            const auto found = document.find(document.contains("servers") ? "servers" : "mcpServers");
            if (found == document.end() || !found->is_object() || found->size() > 100) return {};
            std::vector<std::wstring> keys;
            for (const auto& entry : found->items())
            {
                if (entry.key().empty() || entry.key().size() > 200) continue;
                keys.push_back(Wide(entry.key()));
            }
            return keys;
        }
        catch (...) { return {}; }
    }
}
