#include "MockConnectorServer.h"
#include "ConnectorClient.h"
#include "../third_party/nlohmann/json.hpp"
#include <windows.h>
#include <limits>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
    namespace C = Lattice::Connector;
    using Json = nlohmann::json;
    int checks = 0;
    void Check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
    struct Fixture
    {
        std::filesystem::path root, folder;
        bool created = false;
        Fixture()
        {
            wchar_t buffer[32768]{};
            const auto count = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", buffer, static_cast<DWORD>(std::size(buffer)));
            root = std::filesystem::weakly_canonical(count && count < std::size(buffer) ? std::filesystem::path(buffer) : std::filesystem::current_path());
            folder = root / (L"connector-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            created = std::filesystem::create_directory(folder);
            Check(created, "isolated connector fixture must be created exclusively");
        }
        ~Fixture()
        {
            std::error_code ignored;
            if (created && folder.parent_path() == root) std::filesystem::remove_all(folder, ignored);
        }
    };
    void Write(const std::filesystem::path& path, const std::string& bytes)
    { std::ofstream file(path, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    std::string Read(const std::filesystem::path& path)
    { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
    void TestUrlsAndSettings(Fixture& fixture)
    {
        std::wstring normalized, error;
        for (const auto* url : {L"http://127.0.0.1:8301", L"https://connector.example", L"http://localhost:8401/v1", L"http://[::1]:8301"})
            Check(C::NormalizeServerUrl(url, normalized, error) && error.empty(), "valid server root/v1/IPv6 URL rejected");
        Check(C::NormalizeServerUrl(L" HTTPS://connector.example/proxy/v1/ ", normalized, error) && normalized == L"https://connector.example/proxy/v1", "URL normalization must retain path and trim trailing slash");
        Check(C::ApiBaseUrl(normalized) == normalized && C::ApiBaseUrl(L"http://localhost:8301") == L"http://localhost:8301/v1", "v1 must be appended exactly once");
        for (const auto* url : {L"", L"localhost:8301", L"ftp://host", L"http://user:password@host", L"http://host?q=1", L"http://host#x", L"http://host:0", L"http://host:65536", L"http://host:x", L"http://host:", L"http://host/a b", L"http://host\\path", L"http://0.0.0.0:8301", L"http://[::]:8301"})
            Check(!C::NormalizeServerUrl(url, normalized, error) && !error.empty(), "unsafe or invalid server URL accepted");
        auto nullUrl = std::wstring(L"http://host"); nullUrl.push_back(L'\0');
        Check(!C::NormalizeServerUrl(nullUrl, normalized, error), "embedded null URL accepted");
        const auto path = fixture.folder / L"nested" / L"connection.json";
        Check(C::SaveSettings(path.wstring(), {L"http://localhost:8401/v1/"}, error), "connection URL must save atomically");
        C::Settings loaded;
        Check(C::LoadSettings(path.wstring(), loaded, error) && loaded.serverUrl == L"http://localhost:8401/v1" && loaded.chatFontSize == 11.5f, "saved connection URL or default chat font size lost after reload");
        Check(C::SaveSettings(path.wstring(), {L"http://localhost:8401/v1", 18.0f}, error) &&
            C::LoadSettings(path.wstring(), loaded, error) && loaded.chatFontSize == 18.0f,
            "chat font size must persist atomically beside the configured server URL");
        const auto before = Read(path);
        for (const auto fontSize : {9.0f, 25.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            Check(!C::SaveSettings(path.wstring(), {L"http://localhost:8401/v1", fontSize}, error) && Read(path) == before,
                "invalid chat font size must preserve the current settings file");
        Check(!C::SaveSettings(path.wstring(), {L"file://bad"}, error) && Read(path) == before, "invalid URL save must preserve existing settings");
        const auto folderDestination = fixture.folder / L"directory";
        std::filesystem::create_directory(folderDestination);
        Check(!C::SaveSettings(folderDestination.wstring(), {}, error) && std::filesystem::is_directory(folderDestination), "failed atomic replacement must preserve destination directory");
        for (const auto& bytes : {std::string("{}"), std::string("{\"version\":2,\"server_url\":\"http://host\"}"), std::string("{\"version\":1,\"server_url\":\"ftp://host\"}"), std::string("{\"version\":1,\"server_url\":7}"), std::string("\xff"),
            std::string("{\"version\":1,\"server_url\":\"http://new-host\",\"chat_font_size\":9}"),
            std::string("{\"version\":1,\"server_url\":\"http://new-host\",\"chat_font_size\":25}"),
            std::string("{\"version\":1,\"server_url\":\"http://new-host\",\"chat_font_size\":\"large\"}"),
            std::string("{\"version\":1,\"server_url\":\"http://new-host\",\"chat_font_size\":1e100}")})
        {
            Write(path, bytes);
            Check(!C::LoadSettings(path.wstring(), loaded, error) && loaded.serverUrl == L"http://localhost:8401/v1" && loaded.chatFontSize == 18.0f, "malformed settings must not replace the current URL or chat font size");
        }
        Write(path, R"({"version":1,"server_url":"http://localhost:8401/v1"})");
        Check(C::LoadSettings(path.wstring(), loaded, error) && loaded.chatFontSize == 11.5f,
            "settings from older releases must load with the original chat font size");
        Write(path, R"({"version":1,"server_url":"http://localhost:8401/v1","chat_font_size":13.25})");
        Check(C::LoadSettings(path.wstring(), loaded, error) && loaded.chatFontSize == 13.25f,
            "valid custom chat font sizes must preserve their exact stored value");
        Write(path, std::string(16385, ' '));
        Check(!C::LoadSettings(path.wstring(), loaded, error), "settings size bound must be enforced");
    }
    void TestHttpAndConversation()
    {
        ConnectorTest::MockConnectorServer server;
        C::Settings settings{server.Url()};
        const auto models = C::DiscoverModels(settings);
        Check(models.success && models.models.size() == 2 && models.models[0].id == L"studio-test" && models.models[0].name == L"Studio test", "model discovery must use active server IDs and display metadata");
        const std::vector<C::ChatMessage> history = {{L"user", L"First \"quoted\" line\n\u03a9 \u4e2d \U0001f680"}, {L"assistant", L"Earlier reply"}, {L"user", L"Follow up"}};
        const auto reply = C::Complete({server.Url() + L"/v1/"}, L"studio-test", history);
        Check(reply.success && reply.text == L"Connector test reply", "real WinHTTP completion must decode reply");
        const auto requests = server.Requests();
        Check(requests.size() == 2 && requests[0].method == "GET" && requests[0].path == "/v1/models" && requests[1].method == "POST" && requests[1].path == "/v1/chat/completions", "HTTP endpoints or methods differ from supplied Connector contract");
        const auto body = Json::parse(requests[1].body);
        Check(body["model"] == "studio-test" && body["stream"] == false && body["messages"].size() == 3 && body.size() == 3, "request must include full conversation without unsupported provider options");
        Check(body["messages"][0]["content"] == "First \"quoted\" line\n\xce\xa9 \xe4\xb8\xad \xf0\x9f\x9a\x80" && body["messages"][1]["role"] == "assistant", "UTF-8 or prior assistant history corrupted");
        Check(requests[1].headers.at("content-type").find("application/json") == 0 && !requests[1].headers.count("authorization"), "client must use JSON without upstream credentials");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"data":[]})"}; });
        Check(C::DiscoverModels(settings).success && C::DiscoverModels(settings).models.empty(), "an empty configuration library is a reachable server");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{404, R"({"error":{"message":"Activate a configuration","code":"model_not_found"}})"}; });
        const auto failed = C::Complete(settings, L"missing", {{L"user", L"Hello"}});
        Check(!failed.success && failed.error.find(L"HTTP 404") != std::wstring::npos && failed.error.find(L"Activate a configuration") != std::wstring::npos, "server error envelope must reach useful user error");
        for (const auto& payload : {std::string("not-json"), std::string("{\"data\":[{}]}"), std::string("{\"data\":[{\"id\":\"bad/id\"}]}"), std::string("{\"data\":[{\"id\":\"\xff\"}]}"), std::string("[]")})
        {
            server.SetHandler([payload](const ConnectorTest::Request&) { return ConnectorTest::Response{200, payload}; });
            const auto result = C::DiscoverModels(settings);
            Check(!result.success && result.models.empty() && !result.error.empty(), "invalid model JSON must fail without partial model list");
        }
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call-1"}]}}]})"}; });
        Check(!C::Complete(settings, L"studio-test", history).success, "text client must not silently discard tool calls");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"choices":[{"message":{"content":null,"refusal":"Cannot help"}}]})"}; });
        Check(C::Complete(settings, L"studio-test", history).text == L"Cannot help", "refusal text must be retained");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"choices":[{"message":{"content":""},"finish_reason":"stop"}]})"}; });
        Check(C::Complete(settings, L"studio-test", history).success, "legitimate empty-string completion must be accepted");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"choices":[{"message":{"content":null},"finish_reason":"content_filter"}]})"}; });
        Check(C::Complete(settings, L"studio-test", history).success, "content-filter response must be accepted");
        const auto count = server.RequestCount();
        Check(!C::Complete(settings, L"studio-test", {{L"tool", L"No linked tool engine"}}).success && server.RequestCount() == count, "unsupported local roles must fail before network upload");
        Check(!C::Complete(settings, L"-invalid", {{L"user", L"Hello"}}).success && server.RequestCount() == count, "model IDs must start with a letter or digit");
        Check(!C::Complete(settings, L"studio-test", {{L"user", std::wstring(16 * 1024 * 1024, L'\n')}}).success &&
            server.RequestCount() == count, "oversized escaped conversation must fail before HTTP upload");
        std::string unicode;
        unicode.reserve(4 * 1024 * 1024 + 2);
        for (int i = 0; i < 2 * 1024 * 1024 + 1; ++i) unicode += "\xc2\xa9";
        server.SetHandler([unicode](const ConnectorTest::Request&) {
            return ConnectorTest::Response{200, Json{{"choices", Json::array({{{"message", {{"content", unicode}}}}})}}.dump()};
        });
        const auto hugeReply = C::Complete(settings, L"studio-test", history);
        Check(!hugeReply.success && hugeReply.error.find(L"4 MiB") != std::wstring::npos,
            "UTF-8 reply bound must prevent an unsaveable Unicode transcript");
        server.SetHandler([](const ConnectorTest::Request&) { return ConnectorTest::Response{200, R"({"choices":[{"message":{"role":"user","content":"Wrong role"}}]})"}; });
        Check(!C::Complete(settings, L"studio-test", history).success, "reply must be from the assistant");
    }
    void TestAbsoluteTransport()
    {
        ConnectorTest::MockConnectorServer server([](const ConnectorTest::Request&) {
            return ConnectorTest::Response{200, "{}", 0, {{"Mcp-Session-Id", "fixture-session"}}};
        });
        const auto response = C::ExchangeJson(server.Url() + L"/custom/mcp/", L"POST", "{}",
            {{L"Accept", L"application/json, text/event-stream"}, {L"MCP-Protocol-Version", L"2025-11-25"}});
        Check(response.success && response.mcpSessionId == L"fixture-session" && response.contentType.find(L"application/json") == 0,
            "absolute transport must expose MCP response headers");
        const auto requests = server.Requests();
        Check(requests.size() == 1 && requests[0].path == "/custom/mcp/" &&
            requests[0].headers.at("accept") == "application/json, text/event-stream",
            "absolute transport changed endpoint or duplicated its Accept header");
        Check(C::ExchangeJson(L" " + server.Url() + L"/custom/mcp// ", L"POST", "{}").success &&
            server.Requests().back().path == "/custom/mcp//", "absolute transport must preserve meaningful path slashes after trimming whitespace");
        const auto requestCount = server.RequestCount();
        for (const auto& headers : std::vector<std::vector<std::pair<std::wstring, std::wstring>>>{
            {{L"Authorization", L"secret\r\nInjected: value"}}, {{L"Bad Header", L"value"}},
            {{L"Host", L"other-server"}}, {{L"Accept", L"a"}, {L"accept", L"b"}}})
            Check(!C::ExchangeJson(server.Url(), L"POST", "{}", headers).success && server.RequestCount() == requestCount,
                "invalid or duplicate HTTP headers must fail before connecting");
        Check(!C::ExchangeJson(server.Url(), L"POST\r\n", "{}").success && server.RequestCount() == requestCount,
            "invalid HTTP method must fail before connecting");
    }
    void TestCancellationBoundsAndRedirects()
    {
        ConnectorTest::MockConnectorServer server([](const ConnectorTest::Request& request) {
            auto response = ConnectorTest::MockConnectorServer::Default(request); response.delayMs = 3000; return response;
        });
        C::Settings settings{server.Url()};
        auto token = std::make_shared<C::Cancellation>();
        auto future = std::async(std::launch::async, [&] { return C::DiscoverModels(settings, token); });
        Check(server.WaitForRequests(1), "delayed discovery must reach native mock server");
        const auto started = std::chrono::steady_clock::now(); token->Cancel();
        Check(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready && future.get().cancelled,
            "cancellation must stop async WinHTTP without waiting for delayed server reply");
        Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "cancellation exceeded responsive bound");
        token = std::make_shared<C::Cancellation>(); token->Cancel();
        const auto count = server.RequestCount();
        Check(C::Complete(settings, L"studio-test", {{L"user", L"Cancelled"}}, token).cancelled && server.RequestCount() == count, "pre-cancelled request must not upload conversation");
        const std::string oversize(8 * 1024 * 1024 + 1, 'x');
        server.SetHandler([oversize](const ConnectorTest::Request&) { return ConnectorTest::Response{200, oversize}; });
        const auto bounded = C::DiscoverModels(settings);
        Check(!bounded.success && bounded.error.find(L"8 MiB") != std::wstring::npos, "oversized HTTP body must be bounded");
        ConnectorTest::MockConnectorServer target;
        server.SetHandler([&target](const ConnectorTest::Request&) {
            const auto url = target.Url();
            std::string ascii;
            for (const auto ch : url) ascii.push_back(static_cast<char>(ch)); // Fixture URLs contain ASCII only.
            return ConnectorTest::Response{307, "{}", 0, {{"Location", ascii + "/v1/models"}}};
        });
        const auto redirect = C::DiscoverModels(settings);
        Check(!redirect.success && redirect.error.find(L"redirects") != std::wstring::npos && target.RequestCount() == 0, "redirects must not forward request to another server");
        std::string deep(70, '['); deep += "0"; deep += std::string(70, ']');
        server.SetHandler([deep](const ConnectorTest::Request&) { return ConnectorTest::Response{200, deep}; });
        Check(!C::DiscoverModels(settings).success, "JSON nesting must be bounded");
    }
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring(argv[1]).rfind(L"--live-discovery=", 0) == 0)
        {
            const auto result = Lattice::Connector::DiscoverModels({std::wstring(argv[1]).substr(17)});
            if (!result.success) { std::wcerr << result.error << L'\n'; return 1; }
            std::wcout << L"Connector model discovery: " << result.models.size() << L" active configurations\n";
            for (const auto& model : result.models) std::wcout << model.id << L'\n';
            return 0;
        }
        Fixture fixture; TestUrlsAndSettings(fixture); TestHttpAndConversation(); TestAbsoluteTransport(); TestCancellationBoundsAndRedirects();
        std::cout << checks << " native HTTP, settings, Unicode, response, cancellation and limit checks passed.\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
