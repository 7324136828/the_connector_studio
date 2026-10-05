#include "AppController.h"
#include "ProjectResources.h"
#include "SessionPersistence.h"
#include "MockConnectorServer.h"
#include "../third_party/nlohmann/json.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    using namespace Lattice;
    void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    struct Fixture
    {
        fs::path base, root;
        bool owned = false;
        Fixture()
        {
            wchar_t configured[32768]{};
            const auto length = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", configured, 32768);
            base = length && length < 32768 ? fs::path(configured) : fs::current_path();
            fs::create_directories(base); base = fs::weakly_canonical(base);
            root = base / (L"resources-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            owned = fs::create_directory(root); Check(owned, "An existing fixture was preserved.");
        }
        ~Fixture()
        {
            if (owned && root.is_absolute() && root.parent_path() == base && root.filename().wstring().find(L"resources-fixture-") == 0)
            { std::error_code ignored; fs::remove_all(root, ignored); }
        }
    };
    void Write(const fs::path& path, const std::string& text)
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text; Check(file.good(), "Fixture write failed."); }
    std::string Read(const fs::path& path)
    { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
    template<typename T> int Index(const std::vector<T>& entries, const std::wstring& name)
    {
        for (std::size_t index = 0; index < entries.size(); ++index) if (entries[index].name == name) return static_cast<int>(index);
        throw std::runtime_error("Expected resource is missing.");
    }
    void WaitUntil(AppController& controller, const std::function<bool()>& done, const char* error)
    {
        const auto deadline = GetTickCount64() + 6000;
        while (GetTickCount64() < deadline) { controller.OnTimer(); if (done()) return; Sleep(5); }
        throw std::runtime_error(error);
    }
    std::size_t RequestsTo(const ConnectorTest::MockConnectorServer& server, const std::string& path, const std::string& method = "POST")
    {
        std::size_t count = 0;
        for (const auto& request : server.Requests()) if (request.path == path && request.method == method) ++count;
        return count;
    }
    std::string Ascii(const std::wstring& value)
    { std::string result; for (auto ch : value) result.push_back(static_cast<char>(ch)); return result; }
    struct OwnedHandle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    struct RestoreAttributes
    {
        std::wstring path;
        DWORD original = INVALID_FILE_ATTRIBUTES;
        ~RestoreAttributes() { if (original != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(path.c_str(), original); }
    };
}
int main()
{
    try
    {
        Fixture fixture;
        const auto project = fixture.root / L"Project \u03b1";
        const auto preferences = fixture.root / L"preferences";
        fs::create_directory(project);
        const wchar_t* skillAliases[] = {L"skill", L"skills", L".skill", L".skills"};
        const wchar_t* mcpAliases[] = {L"mcp", L".mcp", L"mcps", L".mcps"};
        const wchar_t* agentAliases[] = {L"agent", L"agents", L".agent", L".agents"};
        for (int index = 0; index < 4; ++index)
        {
            fs::create_directories(project / skillAliases[index] / (L"Skill " + std::to_wstring(index)));
            fs::create_directories(project / mcpAliases[index] / (L"Server " + std::to_wstring(index)));
            fs::create_directories(project / agentAliases[index] / (L"Agent " + std::to_wstring(index)));
        }
        Write(project / L"skills" / L"ignored.txt", "Not a skill folder");
        fs::create_directories(project / L"skill" / L"Skill 0" / L"nested-not-an-entry");
        Write(project / L"skill" / L"Skill 0" / L"SKILL.md", "Do not automatically upload this instruction text.");
        fs::create_directories(project / L"unrelated" / L"skills" / L"Ignore me");
        const auto first = ProjectResources::Discover(project.wstring());
        const auto second = ProjectResources::Discover(project.wstring());
        Check(first.skills.size() == 4 && first.mcpServers.size() == 4 && first.agents.size() == 4, "Alias detection required a file or traversed nested folders.");
        for (std::size_t index = 0; index < 4; ++index)
        {
            Check(first.skills[index].fullPath == second.skills[index].fullPath && !first.skills[index].isEnabled
                && !first.mcpServers[index].isEnabled && !first.agents[index].isEnabled && first.agents[index].priority == static_cast<int>(index + 1), "Discovery order or disabled defaults are unstable.");
        }
        Check(ProjectResources::Discover(L"").skills.empty() && ProjectResources::Discover((fixture.root / L"missing").wstring()).agents.empty(), "Missing project produced resource entries.");
        // If Windows permits creating a symlink, verify neither aliases nor children
        // can redirect discovery outside the selected project.
        const auto outside = fixture.root / L"outside"; fs::create_directories(outside / L"foreign");
        const auto linked = project / L"skills" / L"link";
        if (CreateSymbolicLinkW(linked.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
            Check(ProjectResources::Discover(project.wstring()).skills.size() == 4, "A reparse-point child escaped the project.");

        AppController controller; controller.Initialize(nullptr);
        controller.ConfigureResourcePreferencesPathForTesting(preferences.wstring());
        Check(controller.GetSkills().empty() && controller.GetMcpServers().empty() && controller.GetAgents().empty(), "Startup contains synthetic agents or resources.");
        std::wstring error;
        Check(controller.OpenWorkProject(project.wstring(), error), "Resource project open failed.");
        Check(controller.GetSkills().size() == 4 && controller.GetMcpServers().size() == 4 && controller.GetAgents().size() == 4
            && controller.GetTabs().empty(), "Opening a project did not discover resources independently of sessions.");
        Check(!fs::exists(preferences), "Scanning wrote preferences without a user choice.");
        const int selectedSkill = Index(controller.GetSkills(), L"Skill 2");
        Check(controller.ToggleSkill(selectedSkill) && controller.GetSkills()[selectedSkill].isEnabled, "A discovered skill could not be enabled.");
        const bool firstAgentEnabled = controller.ToggleAgent(0);
        if (!firstAgentEnabled) std::wcerr << L"Initial agent enable save failure: " << controller.GetResourcePreferencesError() << L'\n';
        Check(firstAgentEnabled, "A discovered agent enabled preference did not save.");
        Check(controller.GetAgents()[0].isEnabled, "A discovered agent did not enable.");
        const bool firstAgentDisabled = controller.ToggleAgent(0);
        if (!firstAgentDisabled) std::wcerr << L"Initial agent disable save failure: " << controller.GetResourcePreferencesError() << L'\n';
        Check(firstAgentDisabled, "A discovered agent disabled preference did not save.");
        Check(!controller.GetAgents()[0].isEnabled, "A discovered agent did not disable.");
        Check(controller.ToggleAgent(3) && controller.GetAgents()[3].isEnabled, "An agent could not be enabled before reordering.");
        Check(controller.MoveAgentPriority(3, -1) && controller.MoveAgentPriority(2, -1), "Agent priority could not be changed.");
        Check(controller.GetAgents()[1].name == L"Agent 3" && controller.GetAgents()[1].priority == 2 && controller.GetAgents()[1].isEnabled
            && !controller.GetAgents()[0].isEnabled, "Priority did not preserve each agent's enabled flag.");
        Check(!controller.ToggleSkill(-1) && !controller.ToggleAgent(-1) && !controller.ToggleAgent(500)
            && !controller.ToggleMcpServer(500) && !controller.MoveAgentPriority(0, -1)
            && !controller.MoveAgentPriority(0, 3), "Invalid resource actions mutated preferences.");
        const auto savedFile = ProjectResources::PreferencesFile(preferences.wstring(), fs::weakly_canonical(project).wstring());
        const auto saved = Read(savedFile);
        Check(saved.find("\"version\": 1") != std::string::npos && saved.find("mcp_servers") != std::string::npos && saved.find("agent_enabled") != std::string::npos
            && !fs::exists(project / L"resource-preferences.json"), "Preferences were not stored separately from project content.");
        controller.RefreshProjectResources();
        Check(controller.GetSkills()[selectedSkill].isEnabled && controller.GetAgents()[1].name == L"Agent 3" && controller.GetAgents()[1].isEnabled,
            "Refresh lost existing selections, agent enabled state, or priority.");
        const auto serverIndex = Index(controller.GetMcpServers(), L"Server 2");
        Check(controller.ToggleMcpServer(serverIndex) && controller.GetMcpServers()[serverIndex].connectionState == McpConnectionState::Connecting,
            "Enabling a server did not attempt a connection.");
        const auto deadline = GetTickCount64() + 5000;
        while (controller.GetMcpServers()[serverIndex].connectionState == McpConnectionState::Connecting && GetTickCount64() < deadline)
        { controller.OnTimer(); Sleep(5); }
        Check(!controller.GetMcpServers()[serverIndex].isEnabled && controller.GetMcpServers()[serverIndex].connectionState == McpConnectionState::Disabled
            && !controller.GetMcpServers()[serverIndex].connectionError.empty(), "Missing MCP configuration stayed enabled or did not report its failed connection.");
        const auto failureError = controller.GetMcpServers()[serverIndex].connectionError;
        for (int refresh = 0; refresh < 3; ++refresh) { controller.RefreshProjectResources(); controller.OnTimer(); }
        Check(!controller.GetMcpServers()[serverIndex].isEnabled && controller.GetMcpServers()[serverIndex].connectionState == McpConnectionState::Disabled
            && controller.GetMcpServers()[serverIndex].connectionError == failureError, "A failed MCP server retried during a resource refresh.");
        AppController reopened; reopened.Initialize(nullptr); reopened.ConfigureResourcePreferencesPathForTesting(preferences.wstring());
        Check(reopened.OpenWorkProject(project.wstring(), error) && reopened.GetSkills()[selectedSkill].isEnabled
            && reopened.GetAgents()[1].name == L"Agent 3" && reopened.GetAgents()[1].isEnabled && !reopened.GetAgents()[0].isEnabled
            && !reopened.GetMcpServers()[serverIndex].isEnabled
            && reopened.GetMcpServers()[serverIndex].connectionState == McpConnectionState::Disabled, "Saved project choices or failed-server OFF state were not restored.");

        fs::create_directories(project / L"agents" / L"A new agent");
        fs::create_directories(project / L".skills" / L"A new skill");
        controller.RefreshProjectResources();
        Check(controller.GetAgents().back().name == L"A new agent" && !controller.GetAgents().back().isEnabled
            && controller.GetAgents()[1].name == L"Agent 3" && controller.GetAgents()[1].isEnabled
            && !controller.GetSkills()[Index(controller.GetSkills(), L"A new skill")].isEnabled, "New folders displaced priorities or silently enabled a resource.");
        fs::remove(project / L"mcp" / L"Server 0");
        Sleep(3100); controller.OnTimer();
        Check(controller.GetMcpServers().size() == 3, "Periodic refresh did not remove a deleted server folder.");

        controller.NewSession(); controller.SetDraftText(L"Project draft");
        const auto sessionFile = fixture.root / L"resource-session.lattice";
        Check(controller.SaveSessionToPath(controller.GetActiveTab(), sessionFile.wstring(), error), "Resource session save failed.");
        const auto otherProject = fixture.root / L"Other project";
        fs::create_directories(otherProject / L"skills" / L"Other skill");
        Check(controller.OpenWorkProject(otherProject.wstring(), error) && controller.GetSkills().size() == 1
            && controller.GetSkills()[0].name == L"Other skill" && !controller.GetSkills()[0].isEnabled && controller.GetAgents().empty(), "Resources leaked between projects.");
        controller.SelectSession(0);
        Check(controller.GetCurrentProjectPath() == project.wstring() && controller.GetSkills().size() == 5
            && controller.GetSkills()[Index(controller.GetSkills(), L"Skill 2")].isEnabled, "Selecting a saved-project session did not update discovered resources.");
        AppController imported; imported.ConfigureResourcePreferencesPathForTesting(preferences.wstring());
        Check(imported.OpenSessionFromPath(sessionFile.wstring(), error) && imported.GetSkills().size() == 5
            && imported.GetSkills()[Index(imported.GetSkills(), L"Skill 2")].isEnabled, "Importing a session omitted project resource discovery.");
        imported.CloseProject();
        Check(imported.GetSkills().empty() && imported.GetMcpServers().empty() && imported.GetAgents().empty()
            && imported.GetDraftText() == L"Project draft", "Closing a project retained resources or discarded the conversation.");

        const auto blocked = fixture.root / L"blocked-preferences"; Write(blocked, "User-owned file");
        AppController failure; failure.ConfigureResourcePreferencesPathForTesting(blocked.wstring());
        Check(failure.OpenWorkProject(project.wstring(), error), "A preferences error prevented project access.");
        Check(!failure.ToggleSkill(0) && !failure.GetSkills()[0].isEnabled && !failure.GetResourcePreferencesError().empty()
            && Read(blocked) == "User-owned file", "Failed preference save changed the enabled state or damaged existing files.");
        const auto failureFirstName = failure.GetAgents()[0].name;
        Check(!failure.ToggleAgent(0) && !failure.GetAgents()[0].isEnabled && !failure.MoveAgentPriority(1, -1)
            && failure.GetAgents()[0].name == failureFirstName && Read(blocked) == "User-owned file",
            "A failed agent toggle or reorder mutated the original agent state.");

        ProjectResources::Preferences loaded; loaded.skills.emplace(L"skill/Skill 0", true);
        Write(savedFile, "{\"version\":1,\"project\":\"wrong project\",\"skills\":{},\"mcp_servers\":{},\"agents\":[]}");
        Check(!ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), loaded, error)
            && loaded.skills.size() == 1 && loaded.skills.begin()->second, "Another project's or malformed preferences replaced current choices.");
        Write(savedFile, "{\"version\":1}");
        Check(!ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), loaded, error) && loaded.skills.size() == 1, "Incomplete preference schema was accepted.");
        auto legacyJson = nlohmann::json::parse(saved);
        legacyJson.erase("agent_enabled"); Write(savedFile, legacyJson.dump());
        ProjectResources::Preferences legacyPreferences;
        legacyPreferences.agentEnabled.emplace(L"agent/Agent 0", true);
        Check(ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), legacyPreferences, error)
            && legacyPreferences.agentEnabled.empty(), "Legacy v1 preferences inherited an agent enabled flag without an explicit selection.");
        auto legacySnapshot = ProjectResources::Discover(project.wstring());
        ProjectResources::ApplyPreferences(legacySnapshot, legacyPreferences);
        Check(legacySnapshot.agents[1].name == L"Agent 3" && std::none_of(legacySnapshot.agents.begin(), legacySnapshot.agents.end(),
            [](const auto& agent) { return agent.isEnabled; }), "Legacy preferences lost priority or silently enabled an agent.");
        legacyPreferences.agentEnabled.emplace(L"agent/Agent 0", true);
        legacyJson["agent_enabled"] = nlohmann::json::array(); Write(savedFile, legacyJson.dump());
        Check(!ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), legacyPreferences, error)
            && legacyPreferences.agentEnabled.size() == 1 && legacyPreferences.agentEnabled.begin()->second,
            "Invalid agent_enabled schema discarded existing preferences.");
        legacyJson["agent_enabled"] = {{"../outside", true}}; Write(savedFile, legacyJson.dump());
        Check(!ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), legacyPreferences, error)
            && legacyPreferences.agentEnabled.begin()->second, "An escaping agent-enabled path was accepted.");
        legacyJson["agent_enabled"] = {{"agent/Agent 0", "true"}}; Write(savedFile, legacyJson.dump());
        Check(!ProjectResources::LoadPreferences(preferences.wstring(), project.wstring(), legacyPreferences, error)
            && legacyPreferences.agentEnabled.begin()->second, "A text agent-enabled flag was accepted as a boolean.");

        const auto flattenedProject = fixture.root / L"Named servers";
        const auto sharedFolder = flattenedProject / L"mcp" / L"Shared";
        fs::create_directories(sharedFolder);
        Write(sharedFolder / L"mcp.json", R"({"mcpServers":{"First":{"url":"http://127.0.0.1:1/mcp"},"first":{"url":"http://127.0.0.1:1/mcp"},"Other":{"url":"http://127.0.0.1:1/mcp"}}})");
        auto named = ProjectResources::Discover(flattenedProject.wstring());
        Check(named.mcpServers.size() == 3 && named.mcpServers[0].fullPath == named.mcpServers[1].fullPath
            && named.mcpServers[0].serverKey != named.mcpServers[1].serverKey, "Named servers in one folder were collapsed into one toggle.");
        ProjectResources::Preferences independent;
        const auto namedKey = ProjectResources::McpKey(named.projectPath, named.mcpServers[0].fullPath, named.mcpServers[0].serverKey);
        independent.mcpServers.emplace(namedKey, true);
        ProjectResources::ApplyPreferences(named, independent);
        Check(named.mcpServers[0].isEnabled && !named.mcpServers[1].isEnabled && !named.mcpServers[2].isEnabled,
            "One named server's enabled preference leaked to another server in the same folder.");
        Check(ProjectResources::SavePreferences(preferences.wstring(), named.projectPath, independent, error), "Named-server preferences did not save.");
        ProjectResources::Preferences namedReloaded;
        Check(ProjectResources::LoadPreferences(preferences.wstring(), named.projectPath, namedReloaded, error)
            && namedReloaded.mcpServers.size() == 1 && namedReloaded.mcpServers.begin()->first == namedKey, "Named-server identity did not round-trip through JSON.");
        Check(ProjectResources::McpKey(named.projectPath, sharedFolder.wstring(), L"First")
            != ProjectResources::McpKey(named.projectPath, sharedFolder.wstring(), L"first"), "Case-sensitive MCP names share a preference identity.");

        const auto lockedProject = fixture.root / L"Lock project";
        fs::create_directories(lockedProject / L"skills" / L"Lock skill");
        ProjectResources::Preferences lockedPreferences;
        lockedPreferences.skills.emplace(L"skills/Lock skill", false);
        Check(ProjectResources::SavePreferences(preferences.wstring(), lockedProject.wstring(), lockedPreferences, error), "Lock fixture preferences did not save.");
        const auto lockedFile = ProjectResources::PreferencesFile(preferences.wstring(), lockedProject.wstring());
        {
            OwnedHandle lock;
            lock.value = CreateFileW(lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(lock.value != INVALID_HANDLE_VALUE, "The temporary preference lock could not be acquired.");
            lockedPreferences.skills.begin()->second = true;
            std::thread release([&lock]
            {
                Sleep(40); CloseHandle(lock.value); lock.value = INVALID_HANDLE_VALUE;
            });
            const bool savedAfterUnlock = ProjectResources::SavePreferences(preferences.wstring(), lockedProject.wstring(), lockedPreferences, error);
            release.join();
            if (!savedAfterUnlock) std::wcerr << L"Brief preference lock save failure: " << error << L'\n';
            Check(savedAfterUnlock && error.empty(), "A brief Windows sharing lock prevented atomic preference replacement.");
            ProjectResources::Preferences lockReloaded;
            Check(ProjectResources::LoadPreferences(preferences.wstring(), lockedProject.wstring(), lockReloaded, error)
                && lockReloaded.skills.at(L"skills/Lock skill"), "The retried preference save did not persist the new enabled flag.");
        }
        const auto lockedBytes = Read(lockedFile);
        {
            OwnedHandle lock;
            lock.value = CreateFileW(lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(lock.value != INVALID_HANDLE_VALUE, "The permanent preference lock could not be acquired.");
            lockedPreferences.skills.begin()->second = false;
            const auto started = GetTickCount64();
            const bool savedWhileLocked = ProjectResources::SavePreferences(preferences.wstring(), lockedProject.wstring(), lockedPreferences, error);
            Check(!savedWhileLocked && GetTickCount64() - started < 500 && !error.empty() && Read(lockedFile) == lockedBytes,
                "A permanent preference lock blocked indefinitely or changed the existing destination.");
        }
        for (const auto& entry : fs::directory_iterator(preferences))
            Check(entry.path().extension() != L".tmp", "A failed atomic preference save left its owned temporary file behind.");
        {
            RestoreAttributes attributes{lockedFile, GetFileAttributesW(lockedFile.c_str())};
            Check(attributes.original != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(lockedFile.c_str(), attributes.original | FILE_ATTRIBUTE_READONLY),
                "The owned readonly preference fixture could not be configured.");
            const auto started = GetTickCount64();
            const bool savedReadonly = ProjectResources::SavePreferences(preferences.wstring(), lockedProject.wstring(), lockedPreferences, error);
            Check(!savedReadonly && GetTickCount64() - started < 100 && !error.empty() && Read(lockedFile) == lockedBytes,
                "Readonly destination permissions were retried as a sharing lock or existing preference bytes changed.");
        }

        const auto implicitRanks = fixture.root / L"Implicit ranks";
        fs::create_directories(implicitRanks / L"agents" / L"Z agent");
        AppController ranked; ranked.Initialize(nullptr);
        Check(ranked.OpenWorkProject(implicitRanks.wstring(), error), "Implicit-rank project did not open.");
        fs::create_directories(implicitRanks / L"agents" / L"A agent"); ranked.RefreshProjectResources();
        Check(ranked.GetAgents().size() == 2 && ranked.GetAgents()[0].name == L"Z agent" && ranked.GetAgents()[1].name == L"A agent",
            "A new agent displaced existing priority before a manual reorder.");
        Check(!fs::exists(implicitRanks / L"resource-preferences.json"), "Implicit priority refresh wrote project content.");

        using Json = nlohmann::json;
        ConnectorTest::MockConnectorServer mcpServer([](const ConnectorTest::Request& request)
        {
            if (request.path == "/mcp/bad") return ConnectorTest::Response{401, R"({"error":"Authentication required"})", 0, {}};
            if (request.method == "DELETE") return ConnectorTest::Response{204, "", 0, {}};
            const auto body = Json::parse(request.body);
            const auto method = body.value("method", std::string{});
            if (request.path == "/mcp/legacy")
            {
                if (method == "server/discover") return ConnectorTest::Response{200, Json{{"jsonrpc", "2.0"}, {"id", body["id"]}, {"error", {{"code", -32601}, {"message", "Method not found"}}}}.dump(), 0, {}};
                if (method == "notifications/initialized") return ConnectorTest::Response{202, "", 0, {}};
                return ConnectorTest::Response{200, Json{{"jsonrpc", "2.0"}, {"id", body["id"]}, {"result", {
                    {"protocolVersion", "2025-11-25"}, {"capabilities", Json::object()}, {"serverInfo", {{"name", "Native fixture"}, {"version", "1"}}}}}}.dump(), 0,
                    {{"Mcp-Session-Id", "owned-native-session"}}};
            }
            return ConnectorTest::Response{200, Json{{"jsonrpc", "2.0"}, {"id", body["id"]}, {"result", {
                {"supportedVersions", Json::array({"2026-07-28"})}, {"capabilities", Json::object()}, {"resultType", "complete"}}}}.dump(),
                request.path == "/mcp/slow" ? 500 : 0, {}};
        });
        const auto connectedProject = fixture.root / L"Connected project";
        const auto connectedFolder = connectedProject / L"mcps" / L"Services";
        fs::create_directories(connectedFolder);
        const auto endpoint = Ascii(mcpServer.Url());
        Json serverConfiguration;
        serverConfiguration["mcpServers"]["Good"] = {{"url", endpoint + "/mcp/good"}};
        serverConfiguration["mcpServers"]["Bad"] = {{"url", endpoint + "/mcp/bad"}};
        serverConfiguration["mcpServers"]["Legacy"] = {{"url", endpoint + "/mcp/legacy"}};
        serverConfiguration["mcpServers"]["Slow"] = {{"url", endpoint + "/mcp/slow"}};
        Write(connectedFolder / L"mcp.json", serverConfiguration.dump());
        const auto connectedPreferences = fixture.root / L"connected-preferences";
        AppController connections; connections.ConfigureResourcePreferencesPathForTesting(connectedPreferences.wstring());
        Check(connections.OpenWorkProject(connectedProject.wstring(), error), "Connection fixture project did not open.");
        Check(connections.GetMcpServers().size() == 4 && mcpServer.RequestCount() == 0, "Configuration discovery contacted a server without enabling it.");
        const int good = Index(connections.GetMcpServers(), L"Services / Good"), bad = Index(connections.GetMcpServers(), L"Services / Bad");
        const int legacy = Index(connections.GetMcpServers(), L"Services / Legacy"), slow = Index(connections.GetMcpServers(), L"Services / Slow");
        Check(connections.ToggleMcpServer(good) && connections.ToggleMcpServer(bad), "Named server enable did not start its independent attempt.");
        WaitUntil(connections, [&] { return connections.GetMcpServers()[good].connectionState != McpConnectionState::Connecting
            && connections.GetMcpServers()[bad].connectionState != McpConnectionState::Connecting; }, "MCP controller connection results timed out.");
        Check(connections.GetMcpServers()[good].isEnabled && connections.GetMcpServers()[good].connectionState == McpConnectionState::Connected
            && !connections.GetMcpServers()[bad].isEnabled && !connections.GetMcpServers()[bad].connectionError.empty(), "One named server's failure changed another named server's connected state.");
        const auto failedCount = RequestsTo(mcpServer, "/mcp/bad");
        Check(failedCount == 1, "An authentication failure was retried during its connection attempt.");
        for (int refresh = 0; refresh < 3; ++refresh) { connections.RefreshProjectResources(); connections.OnTimer(); }
        Sleep(3100); connections.OnTimer();
        Check(RequestsTo(mcpServer, "/mcp/bad") == failedCount && connections.GetMcpServers()[good].connectionState == McpConnectionState::Connected,
            "Periodic or explicit resource refresh retried a failed server or lost a retained connection.");
        const auto goodBeforeReopen = RequestsTo(mcpServer, "/mcp/good");
        AppController connectionReopened; connectionReopened.ConfigureResourcePreferencesPathForTesting(connectedPreferences.wstring());
        Check(connectionReopened.OpenWorkProject(connectedProject.wstring(), error), "MCP project preferences did not reopen.");
        WaitUntil(connectionReopened, [&] { return connectionReopened.GetMcpServers()[good].connectionState != McpConnectionState::Connecting; }, "Enabled MCP server did not reconnect once on project open.");
        Check(connectionReopened.GetMcpServers()[good].isEnabled && !connectionReopened.GetMcpServers()[bad].isEnabled
            && RequestsTo(mcpServer, "/mcp/good") == goodBeforeReopen + 1 && RequestsTo(mcpServer, "/mcp/bad") == failedCount,
            "Successful ON or failed OFF preferences were not respected after reopening.");
        const bool disabled = connectionReopened.ToggleMcpServer(good);
        if (!disabled) std::wcerr << L"Connected MCP disable save failure: " << connectionReopened.GetResourcePreferencesError() << L'\n';
        Check(disabled, "Connected MCP server OFF preference did not save.");
        Check(!connectionReopened.GetMcpServers()[good].isEnabled, "Connected MCP server did not disable.");

        Check(connections.ToggleMcpServer(legacy), "Legacy MCP enable did not start.");
        WaitUntil(connections, [&] { return connections.GetMcpServers()[legacy].connectionState != McpConnectionState::Connecting; }, "Legacy MCP handshake did not finish.");
        Check(connections.GetMcpServers()[legacy].isEnabled && RequestsTo(mcpServer, "/mcp/legacy") == 3, "Legacy connection or initialized notification was not retained.");
        Check(connections.ToggleMcpServer(legacy), "Retained legacy MCP session did not disable.");
        const auto disconnectDeadline = GetTickCount64() + 5000;
        while (RequestsTo(mcpServer, "/mcp/legacy", "DELETE") == 0 && GetTickCount64() < disconnectDeadline) Sleep(5);
        Check(RequestsTo(mcpServer, "/mcp/legacy", "DELETE") == 1, "Disabling a retained MCP session did not release it.");

        const auto slowBefore = RequestsTo(mcpServer, "/mcp/slow");
        Check(connections.ToggleMcpServer(slow), "Slow MCP connection did not start.");
        const auto requestDeadline = GetTickCount64() + 5000;
        while (RequestsTo(mcpServer, "/mcp/slow") == slowBefore && GetTickCount64() < requestDeadline) Sleep(5);
        Check(RequestsTo(mcpServer, "/mcp/slow") == slowBefore + 1 && connections.ToggleMcpServer(slow), "Pending MCP connection could not be cancelled.");
        Sleep(600); connections.OnTimer();
        Check(!connections.GetMcpServers()[slow].isEnabled && connections.GetMcpServers()[slow].connectionState == McpConnectionState::Disabled,
            "A late MCP success re-enabled an explicitly disabled server.");
        Check(connections.ToggleMcpServer(slow), "Second pending MCP connection did not start.");
        const auto secondRequestDeadline = GetTickCount64() + 5000;
        while (RequestsTo(mcpServer, "/mcp/slow") == slowBefore + 1 && GetTickCount64() < secondRequestDeadline) Sleep(5);
        Check(RequestsTo(mcpServer, "/mcp/slow") == slowBefore + 2 && connections.OpenWorkProject(otherProject.wstring(), error), "Switching project while MCP was pending failed.");
        Sleep(600); connections.OnTimer();
        Check(connections.GetCurrentProjectPath() == otherProject.wstring() && connections.GetMcpServers().empty(), "A stale MCP result leaked into another project.");

        AppController connectionSaveFailure; connectionSaveFailure.ConfigureResourcePreferencesPathForTesting(blocked.wstring());
        Check(connectionSaveFailure.OpenWorkProject(connectedProject.wstring(), error) && connectionSaveFailure.ToggleMcpServer(good), "Connection save-failure fixture could not start.");
        WaitUntil(connectionSaveFailure, [&] { return connectionSaveFailure.GetMcpServers()[good].connectionState != McpConnectionState::Connecting; }, "Connection save-failure fixture timed out.");
        Check(!connectionSaveFailure.GetMcpServers()[good].isEnabled && !connectionSaveFailure.GetResourcePreferencesError().empty(), "Unpersistable enable did not switch runtime OFF and report the failure.");
        const auto afterSaveFailure = RequestsTo(mcpServer, "/mcp/good");
        connectionSaveFailure.RefreshProjectResources(); connectionSaveFailure.OnTimer();
        Check(RequestsTo(mcpServer, "/mcp/good") == afterSaveFailure && Read(blocked) == "User-owned file", "Failed enable persistence retried the server or damaged the settings destination.");

        const auto bounded = fixture.root / L"Bounded project";
        for (int index = 0; index < 1010; ++index) fs::create_directories(bounded / L"skills" / (L"Skill-" + std::to_wstring(index)));
        fs::create_directories(bounded / L"agents" / L"Beyond limit");
        const auto capped = ProjectResources::Discover(bounded.wstring());
        Check(capped.skills.size() + capped.mcpServers.size() + capped.agents.size() == 1000, "Resource discovery exceeded its entry limit.");
        std::cout << "Project aliases, folder-only discovery, disabled defaults, ranked agents, isolated atomic preferences, refresh, and session switching passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
