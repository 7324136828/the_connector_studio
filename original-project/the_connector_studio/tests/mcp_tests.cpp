#include "MockConnectorServer.h"
#include "McpClient.h"
#include "../third_party/nlohmann/json.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace Lattice;
using Json = nlohmann::json;
using ConnectorTest::MockConnectorServer;
using ConnectorTest::Request;
using ConnectorTest::Response;
namespace
{
    int checks = 0;
    void Check(bool condition, const char* message)
    { ++checks; if (!condition) throw std::runtime_error(message); }
    std::string Utf8(const std::wstring& value)
    {
        const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string output(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
        return output;
    }
    std::wstring Executable()
    {
        std::vector<wchar_t> path(32768);
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        return std::wstring(path.data(), length);
    }
    std::wstring Environment(const wchar_t* name)
    {
        std::vector<wchar_t> value(32768);
        const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        return length && length < value.size() ? std::wstring(value.data(), length) : std::wstring();
    }
    struct Fixture
    {
        std::filesystem::path root;
        Fixture()
        {
            const auto configured = Environment(L"CONNECTOR_STUDIO_TEST_ROOT");
            const auto parent = std::filesystem::canonical(configured.empty() ? std::filesystem::current_path() : std::filesystem::path(configured));
            root = parent / (L"mcp-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            Check(std::filesystem::create_directory(root), "MCP fixture must create an isolated child directory");
        }
        ~Fixture()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
        std::wstring Write(const Json& config)
        {
            std::ofstream file(root / L"mcp.json", std::ios::binary | std::ios::trunc);
            file << config.dump();
            Check(file.good(), "fixture MCP configuration must save");
            return root.wstring();
        }
    };
    Json ModernReply(const Json& request)
    {
        return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", {{"resultType", "complete"},
            {"supportedVersions", Json::array({"2026-07-28"})}, {"capabilities", {{"tools", Json::object()}}},
            {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "Native MCP fixture"}, {"version", "1"}}}}}}}};
    }
    Json LegacyReply(const Json& request, const std::string& version = "2025-11-25")
    {
        return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", {{"protocolVersion", version},
            {"capabilities", Json::object()}, {"serverInfo", {{"name", "Native legacy fixture"}, {"version", "1"}}}}}};
    }
    Json HttpConfig(const std::wstring& url)
    { return {{"servers", {{"fixture", {{"type", "http"}, {"url", Utf8(url)}}}}}}; }
    void TestHttpModernAndLegacy()
    {
        Fixture fixture;
        MockConnectorServer modern([](const Request& request) {
            const auto message = Json::parse(request.body);
            if (message.value("method", std::string()) == "server/discover") return Response{200, ModernReply(message).dump()};
            return Response{400, "{}"};
        });
        auto config = HttpConfig(modern.Url() + L"/mcp/");
        config["servers"]["fixture"]["headers"] = {{"Authorization", "Bearer fixture-only"}};
        auto result = Mcp::Connect(fixture.Write(config));
        Check(result.success && result.connection && result.connection->IsAlive(), "modern HTTP MCP must complete actual server discovery");
        const auto requests = modern.Requests();
        Check(requests.size() == 1 && requests[0].method == "POST" && requests[0].path == "/mcp/", "MCP HTTP must preserve the configured absolute endpoint without appending /v1");
        Check(requests[0].headers.at("accept") == "application/json, text/event-stream" &&
            requests[0].headers.at("mcp-protocol-version") == "2026-07-28" && requests[0].headers.at("mcp-method") == "server/discover",
            "modern HTTP must send MCP version/method headers and JSON/SSE acceptance");
        Check(requests[0].headers.at("authorization") == "Bearer fixture-only", "HTTP MCP must use explicitly configured authentication headers");
        const auto discovery = Json::parse(requests[0].body);
        Check(discovery["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] == "2026-07-28" &&
            discovery["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"].empty(), "modern discovery must carry per-request protocol metadata without advertising unsupported features");
        result.connection->Disconnect();
        Check(!result.connection->IsAlive() && modern.RequestCount() == 1, "modern stateless disconnect must not send a legacy session DELETE");
        result.connection.reset();

        MockConnectorServer legacy([](const Request& request) {
            if (request.method == "DELETE") return Response{204, ""};
            const auto message = Json::parse(request.body);
            const auto method = message.value("method", std::string());
            if (method == "server/discover") return Response{400, "legacy requires initialize"};
            if (method == "initialize") return Response{200, LegacyReply(message, "2025-06-18").dump(), 0, {{"Mcp-Session-Id", "native-session"}}};
            if (method == "notifications/initialized") return Response{202, ""};
            return Response{400, "{}"};
        });
        result = Mcp::Connect(fixture.Write(HttpConfig(legacy.Url() + L"/rpc")));
        Check(result.success && result.connection, "legacy HTTP MCP must initialize after a bounded compatibility fallback");
        const auto initialized = legacy.Requests();
        Check(initialized.size() == 3 && Json::parse(initialized[1].body)["method"] == "initialize" &&
            Json::parse(initialized[2].body)["method"] == "notifications/initialized", "legacy connection must complete the initialized notification");
        Check(initialized[2].headers.at("mcp-session-id") == "native-session" &&
            initialized[2].headers.at("mcp-protocol-version") == "2025-06-18", "initialized notification must retain the negotiated session ID and protocol version");
        const auto before = std::chrono::steady_clock::now();
        result.connection.reset();
        Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "HTTP session release must not block the GUI thread");
        Check(legacy.WaitForRequests(4), "legacy HTTP disconnect must release its owned session in the background");
        const auto deletion = legacy.Requests().back();
        Check(deletion.method == "DELETE" && deletion.path == "/rpc" && deletion.headers.at("mcp-session-id") == "native-session",
            "legacy session deletion must target the exact owned session and endpoint");
    }
    void TestHttpSseAndFailures()
    {
        Fixture fixture;
        MockConnectorServer sse([](const Request& request) {
            const auto message = Json::parse(request.body);
            auto wrong = ModernReply(message);
            wrong["id"] = 999;
            const auto body = std::string(": heartbeat\r\n\r\nevent: message\r\ndata: ") + wrong.dump() +
                "\r\n\r\nevent: message\r\ndata: " + ModernReply(message).dump() + "\r\n\r\n";
            return Response{200, body, 0, {{"Content-Type", "text/event-stream; charset=utf-8"}}, true, 4000};
        });
        const auto start = std::chrono::steady_clock::now();
        auto result = Mcp::Connect(fixture.Write(HttpConfig(sse.Url() + L"/sse")));
        Check(result.success && std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
            "SSE MCP must stop reading at its matching response ID even while the stream stays open");
        result.connection.reset();
        MockConnectorServer auth([](const Request&) { return Response{401, R"({"secret":"do-not-display"})"}; });
        result = Mcp::Connect(fixture.Write(HttpConfig(auth.Url())));
        Check(!result.success && !result.connection && auth.RequestCount() == 1 &&
            result.error.find(L"authentication") != std::wstring::npos && result.error.find(L"do-not-display") == std::wstring::npos,
            "authentication rejection must fail OFF without retries or exposing response secrets");
        MockConnectorServer unsupported([](const Request& request) {
            const auto message = Json::parse(request.body);
            return Response{400, Json{{"jsonrpc", "2.0"}, {"id", message["id"]}, {"error", {
                {"code", -32022}, {"message", "Unsupported protocol version"}, {"data", {{"supported", Json::array({"2099-01-01"})}}}}}}.dump()};
        });
        result = Mcp::Connect(fixture.Write(HttpConfig(unsupported.Url())));
        Check(!result.success && unsupported.RequestCount() == 1, "recognized modern errors must not trigger a legacy initialize or an automatic retry loop");
        MockConnectorServer malformed([](const Request&) { return Response{200, R"({"jsonrpc":"2.0","id":99,"result":{}})"}; });
        result = Mcp::Connect(fixture.Write(HttpConfig(malformed.Url())));
        Check(!result.success && malformed.RequestCount() == 1 && !result.error.empty(), "a mismatched JSON-RPC response ID must fail without claiming a live connection");
        MockConnectorServer delayed([](const Request& request) { return Response{200, ModernReply(Json::parse(request.body)).dump(), 4000}; });
        auto token = std::make_shared<Connector::Cancellation>();
        const auto folder = fixture.Write(HttpConfig(delayed.Url()));
        auto pending = std::async(std::launch::async, [folder, token] { return Mcp::Connect(folder, token); });
        Check(delayed.WaitForRequests(1), "cancellation fixture must receive the actual HTTP handshake");
        token->Cancel();
        Check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "HTTP MCP cancellation must promptly end a pending native request");
        result = pending.get();
        Check(result.cancelled && !result.success && !result.connection && delayed.RequestCount() == 1,
            "cancelled HTTP MCP must return OFF and never retry");
    }
    bool Alive(DWORD pid)
    {
        if (!pid) return false;
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process) return false;
        const auto alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
    }
    DWORD ReadPid(const std::filesystem::path& path)
    {
        for (int i = 0; i < 200; ++i)
        {
            std::ifstream file(path);
            DWORD pid = 0;
            if (file >> pid) return pid;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
    void CheckStopped(DWORD pid, const char* message)
    {
        for (int i = 0; i < 250 && Alive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(pid && !Alive(pid), message);
    }
    Json StdioConfig(const std::filesystem::path& pid, const std::wstring& mode)
    {
        return {{"servers", {{"native", {{"command", Utf8(Executable())}, {"args", Json::array({
            "--mcp-helper=" + Utf8(mode), "--pid-file=" + Utf8(pid.wstring()),
            "argument with spaces", "quoted\"argument", "trailing\\", "& | > $(literal)"})},
            {"env", {{"CONNECTOR_MCP_NATIVE_TEST", "fixture environment"}}}}}}}};
    }
    void TestStdioAndScan()
    {
        Fixture fixture;
        const auto pidPath = fixture.root / L"modern-pid.txt";
        auto config = StdioConfig(pidPath, L"modern");
        fixture.Write(config);
        const auto keys = Mcp::ServerKeys(fixture.root.wstring());
        Check(keys == std::vector<std::wstring>{L"native"} && !std::filesystem::exists(pidPath), "MCP key enumeration must not launch a server");
        auto result = Mcp::Connect(fixture.root.wstring());
        Check(result.success && result.connection && result.connection->IsAlive(), "modern stdio MCP must negotiate through real hidden process pipes");
        const auto modernPid = ReadPid(pidPath);
        Check(modernPid && Alive(modernPid), "successful stdio connection must retain its owned server process");
        const auto before = std::chrono::steady_clock::now();
        result.connection.reset();
        Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "stdio connection release must not wait on the GUI thread");
        CheckStopped(modernPid, "disabled stdio connection must stop its owned process");
        const auto legacyPidPath = fixture.root / L"legacy-pid.txt";
        result = Mcp::Connect(fixture.Write(StdioConfig(legacyPidPath, L"legacy")));
        Check(result.success && result.connection, "legacy stdio MCP must fall back from unknown discovery method to initialize");
        const auto legacyPid = ReadPid(legacyPidPath);
        Check(ReadPid(std::filesystem::path(legacyPidPath.wstring() + L".ping")) == legacyPid && result.connection->IsAlive(),
            "legacy stdio must answer server pings both during initialize and on its retained connection");
        result.connection.reset();
        CheckStopped(legacyPid, "legacy process must exit when its connection is released");
        const auto failedPidPath = fixture.root / L"failed-pid.txt";
        result = Mcp::Connect(fixture.Write(StdioConfig(failedPidPath, L"bad")));
        Check(!result.success && !result.connection, "malformed stdio stdout must fail OFF");
        CheckStopped(ReadPid(failedPidPath), "failed stdio handshake must leave no owned process running");
        const auto cancelPidPath = fixture.root / L"cancel-pid.txt";
        const auto folder = fixture.Write(StdioConfig(cancelPidPath, L"hang"));
        auto token = std::make_shared<Connector::Cancellation>();
        auto pending = std::async(std::launch::async, [folder, token] { return Mcp::Connect(folder, token); });
        const auto cancelPid = ReadPid(cancelPidPath);
        Check(cancelPid && Alive(cancelPid), "stdio cancellation must occur after an actual process is launched");
        token->Cancel();
        Check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "stdio cancellation must promptly stop waiting for a silent process");
        result = pending.get();
        Check(result.cancelled && !result.success && !result.connection, "cancelled stdio handshake must return OFF");
        CheckStopped(cancelPid, "cancelled stdio handshake must leave no owned process running");
        const auto floodPidPath = fixture.root / L"flood-pid.txt";
        result = Mcp::Connect(fixture.Write(StdioConfig(floodPidPath, L"flood")));
        Check(result.success && result.connection, "legacy ping flood fixture must initially connect");
        const auto floodPid = ReadPid(floodPidPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result.connection.reset();
        CheckStopped(floodPid, "disable must stop an uncooperative MCP process even if its pipe replies block");
        const auto parentPidPath = fixture.root / L"tree-parent-pid.txt";
        const auto childPidPath = fixture.root / L"tree-child-pid.txt";
        config = StdioConfig(parentPidPath, L"spawn");
        config["servers"]["native"]["args"].push_back("--child-pid=" + Utf8(childPidPath.wstring()));
        result = Mcp::Connect(fixture.Write(config));
        Check(result.success, "process-tree fixture must connect before teardown");
        const auto parentPid = ReadPid(parentPidPath), childPid = ReadPid(childPidPath);
        Check(parentPid && childPid && Alive(childPid), "MCP fixture must launch its owned descendant process");
        result.connection.reset();
        CheckStopped(parentPid, "MCP job teardown must stop the parent process");
        CheckStopped(childPid, "MCP job teardown must also stop descendant processes");
        config = {{"servers", {{"zeta", {{"url", "http://127.0.0.1:1"}}}, {"alpha", {{"url", "http://127.0.0.1:1"}}}}}};
        fixture.Write(config);
        Check(Mcp::ServerKeys(fixture.root.wstring()) == std::vector<std::wstring>({L"alpha", L"zeta"}), "named MCP entries must enumerate in deterministic order");
        result = Mcp::Connect(fixture.root.wstring());
        Check(!result.success && result.error.find(L"Select a server") != std::wstring::npos, "multi-server folder must require a specific named entry");
        config = StdioConfig(fixture.root / L"selected-pid.txt", L"modern");
        config["servers"]["second"] = {{"command", "unavailable-mcp.exe"}};
        result = Mcp::Connect(fixture.Write(config), {}, L"native");
        Check(result.success && result.connection, "a selected named server must connect without launching its neighbors");
        const auto selectedPid = ReadPid(fixture.root / L"selected-pid.txt");
        result.connection.reset();
        CheckStopped(selectedPid, "selected named stdio process must be owned and stopped");
        result = Mcp::Connect(fixture.root.wstring(), {}, L"missing");
        Check(!result.success && !result.connection, "a removed named server must not accidentally connect a different server");
    }
    void TestConfigValidation()
    {
        Fixture fixture;
        auto result = Mcp::Connect(fixture.root.wstring());
        Check(!result.success && Mcp::ServerKeys(fixture.root.wstring()).empty(), "missing mcp.json must fail without inventing an active server");
        std::ofstream(fixture.root / L"mcp.json") << "{\"secret\":\"sensitive-token";
        result = Mcp::Connect(fixture.root.wstring());
        Check(!result.success && result.error.find(L"sensitive-token") == std::wstring::npos, "invalid config errors must not disclose raw JSON tokens");
        const auto invalid = HttpConfig(L"http://user:password@127.0.0.1:1/mcp");
        result = Mcp::Connect(fixture.Write(invalid));
        Check(!result.success && !result.connection, "MCP endpoint credentials in URLs must be rejected");
        auto config = HttpConfig(L"http://127.0.0.1:1/mcp");
        config["servers"]["fixture"]["headers"] = {{"Authorization", "bad\r\nInjected: header"}};
        result = Mcp::Connect(fixture.Write(config));
        Check(!result.success && !result.connection, "MCP headers must reject line-break injection before network access");
        auto cancelled = std::make_shared<Connector::Cancellation>();
        cancelled->Cancel();
        result = Mcp::Connect(fixture.root.wstring(), cancelled);
        Check(result.cancelled && !result.success, "pre-cancelled MCP enable must perform no launch or network access");
        result = Mcp::Connect(fixture.Write({{"command", "unavailable-native-mcp.exe"}}));
        Check(!result.success, "an unavailable command must fail without claiming a connection");
    }
    int Helper(int argc, wchar_t** argv, const std::wstring& mode)
    {
        std::wstring pidPath, childPidPath;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring argument(argv[i]);
            if (argument.rfind(L"--pid-file=", 0) == 0) pidPath = argument.substr(11);
            if (argument.rfind(L"--child-pid=", 0) == 0) childPidPath = argument.substr(12);
        }
        if (!pidPath.empty()) std::ofstream(std::filesystem::path(pidPath)) << GetCurrentProcessId();
        if (mode == L"hang") { for (;;) Sleep(100); }
        if (argc < 7 || std::wstring(argv[3]) != L"argument with spaces" || std::wstring(argv[4]) != L"quoted\"argument" ||
            std::wstring(argv[5]) != L"trailing\\" || std::wstring(argv[6]) != L"& | > $(literal)" ||
            Environment(L"CONNECTOR_MCP_NATIVE_TEST") != L"fixture environment") return 20;
        if (mode == L"bad") { std::cout << "this is not JSON-RPC\n" << std::flush; for (;;) Sleep(100); }
        if (mode == L"spawn")
        {
            const auto executable = Executable();
            auto command = L"\"" + executable + L"\" --mcp-helper=hang \"--pid-file=" + childPidPath + L"\"";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 21;
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        std::string line;
        while (std::getline(std::cin, line))
        {
            const auto request = Json::parse(line);
            const auto method = request.value("method", std::string());
            if (method == "server/discover")
            {
                if (mode == L"legacy" || mode == L"flood") std::cout << Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"error", {{"code", -32602}, {"message", "Not initialized"}}}}.dump() << '\n' << std::flush;
                else std::cout << ModernReply(request).dump() << '\n' << std::flush;
            }
            else if (method == "initialize")
            {
                if (mode == L"legacy")
                {
                    std::cout << Json{{"jsonrpc", "2.0"}, {"id", "before-initialize"}, {"method", "ping"}}.dump() << '\n' << std::flush;
                    std::string answer;
                    if (!std::getline(std::cin, answer)) return 30;
                    const auto pong = Json::parse(answer);
                    if (pong["id"] != "before-initialize" || !pong.contains("result") || !pong["result"].empty()) return 31;
                }
                std::cout << LegacyReply(request).dump() << '\n' << std::flush;
            }
            else if (method == "notifications/initialized")
            {
                if (mode == L"legacy")
                {
                    std::cout << Json{{"jsonrpc", "2.0"}, {"id", "after-initialize"}, {"method", "ping"}}.dump() << '\n' << std::flush;
                    std::string answer;
                    if (!std::getline(std::cin, answer)) return 32;
                    const auto pong = Json::parse(answer);
                    if (pong["id"] != "after-initialize" || !pong.contains("result") || !pong["result"].empty()) return 33;
                    std::ofstream(std::filesystem::path(pidPath + L".ping")) << GetCurrentProcessId();
                }
                if (mode == L"flood")
                {
                    for (int index = 0; index < 10000; ++index)
                        std::cout << Json{{"jsonrpc", "2.0"}, {"id", index + 100}, {"method", "ping"}}.dump() << '\n' << std::flush;
                    for (;;) Sleep(100);
                }
            }
            else return 22;
        }
        return 0;
    }
}
int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]).rfind(L"--mcp-helper=", 0) == 0)
    {
        try { return Helper(argc, argv, std::wstring(argv[1]).substr(13)); }
        catch (...) { return 25; }
    }
    try
    {
        TestHttpModernAndLegacy();
        TestHttpSseAndFailures();
        TestStdioAndScan();
        TestConfigValidation();
        std::cout << "MCP tests passed (" << checks << " checks)\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "MCP tests failed after " << checks << " checks: " << error.what() << '\n'; return 1; }
}
