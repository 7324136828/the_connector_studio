#include "AppController.h"
#include "SessionPersistence.h"
#include "MockConnectorServer.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <atomic>

namespace
{
    namespace fs = std::filesystem;
    using Lattice::AppController;
    void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    struct Fixture
    {
        fs::path base, root;
        bool owned = false;
        Fixture()
        {
            wchar_t configured[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", configured, 32768);
            base = length && length < 32768 ? fs::path(configured) : fs::current_path();
            fs::create_directories(base); base = fs::weakly_canonical(base);
            root = base / (L"connection-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            owned = fs::create_directory(root); Check(owned, "Existing connection fixture was preserved.");
        }
        ~Fixture()
        {
            if (owned && root.is_absolute() && root.parent_path() == base && root.filename().wstring().find(L"connection-fixture-") == 0)
            { std::error_code ignored; fs::remove_all(root, ignored); }
        }
    };
    std::string Read(const fs::path& path)
    { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
    void WaitUntil(AppController& controller, const std::function<bool()>& predicate, const char* failure)
    {
        const auto deadline = GetTickCount64() + 6000;
        while (GetTickCount64() < deadline)
        { controller.OnTimer(); if (predicate()) return; Sleep(5); }
        throw std::runtime_error(failure);
    }
    void Connect(AppController& controller, const ConnectorTest::MockConnectorServer& server, const fs::path& settings)
    {
        controller.ConfigureSettingsPathForTesting(settings.wstring()); std::wstring error;
        Check(controller.ApplyConnectorSettings({server.Url()}, error), "Applying isolated settings failed.");
        WaitUntil(controller, [&] { return controller.IsConnectorConnected(); }, "Async model discovery failed.");
    }
    void Finish(AppController& controller)
    { WaitUntil(controller, [&] { return !controller.IsGeneratingReply(); }, "Async completion timed out."); }
    int MenuIndex(const AppController& controller, const std::wstring& label)
    {
        const auto& items = controller.GetCurrentDropdownItems();
        for (std::size_t index = 0; index < items.size(); ++index)
            if (items[index].label == label) return static_cast<int>(index);
        throw std::runtime_error("Expected model-menu row was missing.");
    }
    std::vector<ConnectorTest::Request> Completions(const ConnectorTest::MockConnectorServer& server)
    {
        std::vector<ConnectorTest::Request> result;
        for (const auto& request : server.Requests()) if (request.path == "/v1/chat/completions") result.push_back(request);
        return result;
    }
    ConnectorTest::Response Delayed(const ConnectorTest::Request& request)
    {
        auto response = ConnectorTest::MockConnectorServer::Default(request);
        if (request.path == "/v1/chat/completions") response.delayMs = 450;
        return response;
    }
    AppController* closeReviewController = nullptr;
    std::atomic<bool>* closeReviewRelease = nullptr;
    int closeReviewPrompts = 0;
    bool closeReviewObservedReply = false, cancelCloseReview = false;
    LRESULT CALLBACK CloseReviewHook(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HCBT_ACTIVATE && closeReviewController)
        {
            const auto window = reinterpret_cast<HWND>(wParam);
            wchar_t title[128]{}; GetWindowTextW(window, title, 128);
            if (std::wstring(title) == L"Unsaved Session")
            {
                ++closeReviewPrompts;
                int choice = cancelCloseReview ? IDCANCEL : IDYES;
                if (!cancelCloseReview && closeReviewPrompts == 2)
                {
                    if (closeReviewRelease) closeReviewRelease->store(true);
                    const auto deadline = GetTickCount64() + 6000;
                    while (GetTickCount64() < deadline && closeReviewController->GetTabs()[0].messages.size() < 2)
                    { closeReviewController->OnTimer(); Sleep(5); }
                    closeReviewObservedReply = closeReviewController->GetTabs()[0].messages.size() == 2;
                    choice = closeReviewObservedReply ? IDNO : IDCANCEL;
                }
                if (closeReviewPrompts > 3) choice = IDCANCEL;
                PostMessageW(window, WM_COMMAND, MAKEWPARAM(choice, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, choice)));
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    struct ScopedCloseReviewHook
    {
        HHOOK hook;
        ScopedCloseReviewHook(AppController& controller, bool cancel, std::atomic<bool>* release = nullptr)
        {
            closeReviewController = &controller; closeReviewPrompts = 0;
            closeReviewObservedReply = false; cancelCloseReview = cancel; closeReviewRelease = release;
            hook = SetWindowsHookExW(WH_CBT, CloseReviewHook, nullptr, GetCurrentThreadId());
            Check(hook != nullptr, "Close-review automation hook failed.");
        }
        ~ScopedCloseReviewHook() { UnhookWindowsHookEx(hook); closeReviewController = nullptr; closeReviewRelease = nullptr; }
    };
}
int main()
{
    try
    {
        using namespace Lattice;
        Fixture fixture; ConnectorTest::MockConnectorServer server;
        AppController controller; controller.Initialize(nullptr);
        Check(controller.GetTabs().empty() && controller.GetFiles().empty() && controller.GetSelectedModel().empty()
            && !controller.IsConnectorConnected() && server.RequestCount() == 0, "Headless startup created content or accessed a server.");
        controller.HandleClick({HitTargetType::ComposerModelSelect});
        Check(controller.GetActiveDropdown() == ActiveDropdown::Model && controller.GetCurrentDropdownItems().size() == 3
            && !controller.GetCurrentDropdownItems()[0].enabled && !controller.IsDiscoveringModels() && server.RequestCount() == 0,
            "An unconfigured headless picker disappeared or made an ambient network request.");
        controller.HandleDropdownSelect(0);
        Check(controller.GetSelectedModel().empty() && controller.GetActiveDropdown() == ActiveDropdown::Model,
            "A disabled model-menu status became a model selection.");
        controller.CloseDropdown();
        controller.NewSession(); controller.SetDraftText(L"Draft without connection"); controller.SendCurrentMessage();
        Check(controller.GetDraftText() == L"Draft without connection" && controller.GetTabs()[0].messages.empty()
            && !controller.GetActiveRequestError().empty(), "Disconnected send lost input or fabricated a message.");
        Connect(controller, server, fixture.root / L"connection.settings");
        controller.SetActiveDropdown(ActiveDropdown::Model);
        const auto& models = controller.GetCurrentDropdownItems();
        Check(models.size() == 4 && models[0].label == L"studio-test" && models[1].label == L"second-model"
            && models[0].enabled && models[0].selected && !models[1].selected && models[2].label == L"Refresh models"
            && models[3].label == L"Connection settings", "Model menu does not expose active server model IDs and recovery actions.");
        Check(controller.GetSelectedModel() == L"studio-test" && controller.GetTabs().size() == 1, "Discovery changed workspace topology or left a blank model.");
        const auto persisted = Read(fixture.root / L"connection.settings"); std::wstring error;
        Check(!controller.ApplyConnectorSettings({L"file:///unsafe"}, error) && controller.IsConnectorConnected()
            && Read(fixture.root / L"connection.settings") == persisted && controller.GetDraftText() == L"Draft without connection", "Invalid settings changed connection, persistence, or draft.");
        controller.ConfigureSettingsPathForTesting(fixture.root.wstring());
        Check(!controller.ApplyConnectorSettings({server.Url()}, error) && controller.IsConnectorConnected()
            && Read(fixture.root / L"connection.settings") == persisted, "Failed settings persistence replaced a working connection.");
        controller.ConfigureSettingsPathForTesting((fixture.root / L"connection.settings").wstring());
        controller.SetDraftText(L"First request \u03A9"); controller.SendCurrentMessage(); Finish(controller);
        Check(controller.GetTabs()[0].messages.size() == 2 && controller.GetTabs()[0].messages.back().text == L"Connector test reply"
            && controller.GetActiveRequestError().empty(), "Completion was not the actual server response.");
        controller.SetDraftText(L"Second request"); controller.SendCurrentMessage(); Finish(controller);
        auto requests = Completions(server);
        Check(requests.size() == 2 && requests.back().body.find("First request") != std::string::npos
            && requests.back().body.find("Connector test reply") != std::string::npos && requests.back().body.find("Second request") != std::string::npos,
            "A sessionless completion omitted conversation history.");
        Check(requests.back().body.find("\"model\":\"studio-test\"") != std::string::npos
            && requests.back().body.find("\"tools\"") == std::string::npos && requests.back().path == "/v1/chat/completions", "Request model/endpoint is wrong or advertised tools.");

        server.SetHandler(Delayed);
        const auto beforeFontSend = server.RequestCount();
        const auto began = GetTickCount64(); controller.SetDraftText(L"Owned by first tab"); controller.SendCurrentMessage();
        Check(GetTickCount64() - began < 250 && controller.IsGeneratingReply(), "Send blocked the window thread.");
        controller.SetDraftText(L"New first-tab draft");
        Check(server.WaitForRequests(beforeFontSend + 1), "Font change fixture did not reach the server.");
        const auto beforeFontRequests = server.RequestCount();
        Check(controller.ApplyConnectorSettings({server.Url(), 18.0f}, error) && controller.IsGeneratingReply()
            && controller.GetConnectorSettings().chatFontSize == 18.0f && controller.GetSelectedModel() == L"studio-test"
            && controller.GetDraftText() == L"New first-tab draft" && server.RequestCount() == beforeFontRequests
            && !controller.IsDiscoveringModels(), "Changing font size cancelled a reply, refreshed models, or lost session state.");
        const auto fontSettings = Read(fixture.root / L"connection.settings");
        Check(!controller.ApplyConnectorSettings({server.Url(), 100.0f}, error) && controller.IsGeneratingReply()
            && controller.GetConnectorSettings().chatFontSize == 18.0f && Read(fixture.root / L"connection.settings") == fontSettings,
            "Invalid font settings changed a running request or saved preference.");
        controller.NewSession(); controller.SetActiveDropdown(ActiveDropdown::Model); controller.HandleDropdownSelect(1);
        controller.SetDraftText(L"Owned by second tab"); controller.SendCurrentMessage();
        Finish(controller); controller.SelectSession(0); Finish(controller);
        Check(controller.GetTabs()[0].messages.back().text == L"Connector test reply" && controller.GetDraftText() == L"New first-tab draft"
            && controller.GetTabs()[1].messages.size() == 2 && controller.GetTabs()[1].messages.back().author == L"second-model", "Async results/drafts were attached to the wrong session.");
        controller.SetActiveScrollMetrics(500); controller.SetActiveScrollOffset(50);
        controller.SetDraftText(L"Reading earlier messages"); controller.SendCurrentMessage(); controller.SetActiveScrollMetrics(650); controller.SetActiveScrollOffset(50);
        Finish(controller); controller.SetActiveScrollMetrics(800);
        Check(controller.GetTabs()[0].scrollOffset == 50, "Async reply jumped while reading earlier history.");

        server.SetHandler([](const ConnectorTest::Request& request)
        {
            if (request.path == "/v1/chat/completions") return ConnectorTest::Response{502, "{\"error\":{\"message\":\"upstream unavailable\"}}", 100};
            return ConnectorTest::MockConnectorServer::Default(request);
        });
        const auto beforeFailure = controller.GetTabs()[0].messages.size();
        controller.SetDraftText(L"Failed query"); controller.SendCurrentMessage(); controller.SetDraftText(L"Newer draft"); Finish(controller);
        Check(controller.GetTabs()[0].messages.size() == beforeFailure && controller.GetDraftText().find(L"Failed query") != std::wstring::npos
            && controller.GetDraftText().find(L"Newer draft") != std::wstring::npos && controller.GetActiveRequestError().find(L"upstream unavailable") != std::wstring::npos,
            "Failure polluted the transcript or discarded submitted/newer drafts.");
        server.SetHandler(ConnectorTest::MockConnectorServer::Default);
        controller.SetDraftText(L"Retry after failure"); controller.SendCurrentMessage(); Finish(controller);
        requests = Completions(server);
        Check(requests.back().body.find("upstream unavailable") == std::string::npos && requests.back().body.find("Failed query") == std::string::npos
            && controller.GetActiveRequestError().empty(), "Local error or failed message leaked into subsequent API context.");

        server.SetHandler(Delayed);
        const auto beforeCancel = controller.GetTabs()[0].messages.size(), beforeRequests = server.RequestCount();
        controller.SetDraftText(L"Cancelled query"); controller.SendCurrentMessage();
        Check(server.WaitForRequests(beforeRequests + 1), "Cancellation fixture did not reach the server.");
        controller.SetDraftText(L"Typed while waiting"); const auto cancelBegan = GetTickCount64(); controller.CancelPendingReply();
        Check(GetTickCount64() - cancelBegan < 250 && !controller.IsGeneratingReply() && controller.GetTabs()[0].messages.size() == beforeCancel
            && controller.GetDraftText().find(L"Cancelled query") != std::wstring::npos && controller.GetDraftText().find(L"Typed while waiting") != std::wstring::npos,
            "Cancellation blocked, kept an uncompleted message, or lost drafts.");
        const auto cancelledDraft = controller.GetDraftText();
        for (int i = 0; i < 110; ++i) { controller.OnTimer(); Sleep(5); }
        Check(controller.GetTabs()[0].messages.size() == beforeCancel && controller.GetDraftText() == cancelledDraft, "A late cancelled reply mutated its session.");

        controller.SetDraftText(L"Save while pending"); controller.SendCurrentMessage();
        Check(controller.SaveSessionToPath(0, (fixture.root / L"pending.lattice").wstring(), error), "Saving a pending session failed.");
        SessionTab snapshot;
        Check(SessionPersistence::Load((fixture.root / L"pending.lattice").wstring(), snapshot, error)
            && snapshot.messages.back().role == L"user" && snapshot.messages.back().text == L"Save while pending"
            && controller.IsGeneratingReply(), "Saving fabricated a reply or cancelled an actual request.");
        controller.SetDraftText(L"Draft during save"); Finish(controller);
        Check(controller.GetTabs()[0].dirty && controller.GetDraftText() == L"Draft during save", "A reply after save did not mark changes or overwrote the draft.");
        const auto keptCount = controller.GetTabs()[0].messages.size();
        controller.NewSession(); const int closeIndex = controller.GetActiveTab();
        controller.SetDraftText(L"Close while pending"); controller.SendCurrentMessage();
        Check(controller.SaveSessionToPath(closeIndex, (fixture.root / L"closing.lattice").wstring(), error), "Close fixture save failed.");
        controller.CloseSession(closeIndex);
        for (int i = 0; i < 110; ++i) { controller.OnTimer(); Sleep(5); }
        Check(controller.GetTabs().size() == 2 && controller.GetTabs()[0].messages.size() == keptCount, "Closing a pending session leaked a reply into another tab.");

        SessionTab inactive; inactive.title = L"Inactive model"; inactive.selectedModel = L"missing-configuration";
        Check(SessionPersistence::Save((fixture.root / L"inactive.lattice").wstring(), inactive, error)
            && controller.OpenSessionFromPath((fixture.root / L"inactive.lattice").wstring(), error), "Inactive model fixture failed.");
        const auto beforeInvalid = server.RequestCount(); controller.SetDraftText(L"Keep invalid-model input"); controller.SendCurrentMessage();
        Check(controller.GetDraftText() == L"Keep invalid-model input" && controller.GetTabs().back().messages.empty()
            && !controller.GetActiveRequestError().empty() && server.RequestCount() == beforeInvalid, "Inactive model was sent or lost the draft.");
        controller.SetActiveDropdown(ActiveDropdown::Model); controller.HandleDropdownSelect(1); controller.SendCurrentMessage(); Finish(controller);
        Check(controller.GetTabs().back().messages.back().author == L"second-model", "A discovered model could not repair a saved model preference.");
        controller.SetDraftText(L"Old-server request"); controller.SendCurrentMessage();
        ConnectorTest::MockConnectorServer replacementServer;
        Check(controller.ApplyConnectorSettings({replacementServer.Url()}, error), "Changing to another Connector server failed.");
        WaitUntil(controller, [&] { return controller.IsConnectorConnected(); }, "Replacement server model discovery failed.");
        Check(!controller.IsGeneratingReply() && controller.GetDraftText().find(L"Old-server request") != std::wstring::npos,
            "Reconfiguration left an old-server request active or discarded input.");
        const auto replacedCount = controller.GetTabs().back().messages.size();
        for (int i = 0; i < 110; ++i) { controller.OnTimer(); Sleep(5); }
        Check(controller.GetTabs().back().messages.size() == replacedCount, "Old-server result crossed into the replacement connection.");
        replacementServer.SetHandler(Delayed);
        controller.SetDraftText(L"Shutdown pending"); controller.SendCurrentMessage(); const auto shutdownBegan = GetTickCount64();
        controller.ShutdownConnector();
        Check(GetTickCount64() - shutdownBegan < 250 && !controller.IsGeneratingReply(), "Shutdown waited for a network request.");
        for (int i = 0; i < 110; ++i) { controller.OnTimer(); Sleep(5); }
        Check(controller.GetTabs().back().messages.back().role == L"user", "Shutdown added a fake or late assistant reply.");

        server.SetHandler(ConnectorTest::MockConnectorServer::Default);
        AppController legacyHistory; Connect(legacyHistory, server, fixture.root / L"history.settings");
        SessionTab history; history.title = L"Saved history"; history.selectedModel = L"studio-test";
        history.messages = {
            {L"assistant", L"Connector Studio", L"", L"Added to this local conversation. No AI provider is connected, so I cannot analyze the request.", {}, {}},
            {L"assistant", L"studio-test", L"", L"Actual saved answer", {L"Saved item"}, {{1, L"Saved plan title", L"Saved plan detail"}}}
        };
        Check(SessionPersistence::Save((fixture.root / L"history.lattice").wstring(), history, error)
            && legacyHistory.OpenSessionFromPath((fixture.root / L"history.lattice").wstring(), error), "Saved history fixture failed.");
        legacyHistory.SetDraftText(L"Continue saved history"); legacyHistory.SendCurrentMessage(); Finish(legacyHistory);
        requests = Completions(server);
        Check(requests.back().body.find("No AI provider is connected") == std::string::npos
            && requests.back().body.find("Actual saved answer") != std::string::npos && requests.back().body.find("Saved item") != std::string::npos
            && requests.back().body.find("Saved plan detail") != std::string::npos, "Saved structured history was lost or a legacy local status entered model context.");

        std::atomic<bool> releaseCloseReply{false};
        ConnectorTest::MockConnectorServer closeReviewServer([&releaseCloseReply](const ConnectorTest::Request& request)
        {
            if (request.path == "/v1/chat/completions")
            {
                const auto deadline = GetTickCount64() + 6000;
                while (!releaseCloseReply.load() && GetTickCount64() < deadline) Sleep(5);
            }
            return ConnectorTest::MockConnectorServer::Default(request);
        });
        AppController closeReview; Connect(closeReview, closeReviewServer, fixture.root / L"close-review.settings");
        closeReview.NewSession();
        Check(closeReview.SaveSessionToPath(0, (fixture.root / L"close-review.lattice").wstring(), error), "Close-review saved-session preparation failed.");
        closeReview.SetDraftText(L"Reply arriving during another close prompt"); closeReview.SendCurrentMessage();
        closeReview.NewSession(); closeReview.SetDraftText(L"Discard this second-tab draft");
        {
            ScopedCloseReviewHook hook(closeReview, false, &releaseCloseReply);
            Check(closeReview.CanCloseApplication() && closeReviewObservedReply && closeReviewPrompts == 3,
                "Closing skipped a response arriving after an earlier session's save decision.");
        }
        Check(SessionPersistence::Load((fixture.root / L"close-review.lattice").wstring(), snapshot, error)
            && snapshot.messages.size() == 2 && snapshot.messages.back().text == L"Connector test reply" && !closeReview.GetTabs()[0].dirty,
            "The response reviewed again during close was not saved.");
        server.SetHandler(Delayed);
        AppController cancelClose; Connect(cancelClose, server, fixture.root / L"cancel-close.settings");
        cancelClose.NewSession(); cancelClose.SetDraftText(L"Continue request after cancelling close"); cancelClose.SendCurrentMessage();
        {
            ScopedCloseReviewHook hook(cancelClose, true);
            Check(!cancelClose.CanCloseApplication() && cancelClose.IsGeneratingReply() && cancelClose.IsConnectorConnected(),
                "Cancelling the close prompt shut down a live request or connection.");
        }
        Finish(cancelClose);
        Check(cancelClose.GetTabs()[0].messages.back().text == L"Connector test reply", "Cancelling close prevented the pending response from completing.");
        AppController queriedClose; Connect(queriedClose, server, fixture.root / L"queried-close.settings");
        queriedClose.NewSession(); queriedClose.SetDraftText(L"A Windows end-session query can be cancelled"); queriedClose.SendCurrentMessage();
        Check(queriedClose.SaveSessionToPath(0, (fixture.root / L"queried-close.lattice").wstring(), error), "End-session query snapshot preparation failed.");
        Check(queriedClose.CanCloseApplication() && queriedClose.IsGeneratingReply() && queriedClose.IsConnectorConnected(),
            "Authorizing close prematurely shut down a request before the actual window/session end.");
        Finish(queriedClose);
        Check(queriedClose.GetTabs()[0].messages.back().text == L"Connector test reply", "A cancelled Windows session end could not keep the connection alive.");

        AppController noModels; ConnectorTest::MockConnectorServer emptyServer([](const ConnectorTest::Request& request)
        {
            if (request.path == "/v1/models") return ConnectorTest::Response{200, "{\"object\":\"list\",\"data\":[]}", 0};
            return ConnectorTest::MockConnectorServer::Default(request);
        });
        Connect(noModels, emptyServer, fixture.root / L"empty.settings"); noModels.NewSession(); noModels.SetDraftText(L"No active configuration"); noModels.SendCurrentMessage();
        Check(noModels.IsConnectorConnected() && noModels.GetSelectedModel().empty() && noModels.GetTabs()[0].messages.empty()
            && noModels.GetDraftText() == L"No active configuration" && !noModels.GetActiveRequestError().empty(), "An empty active-model list invented a model or discarded input.");
        noModels.HandleClick({HitTargetType::SidebarModelSelect});
        Check(noModels.GetActiveDropdown() == ActiveDropdown::SidebarModel && noModels.GetCurrentDropdownItems().size() == 3
            && noModels.GetCurrentDropdownItems()[0].label == L"No active models on Connector" && !noModels.GetCurrentDropdownItems()[0].enabled,
            "An empty Connector library hid the picker instead of explaining its state.");
        noModels.HandleDropdownSelect(0);
        Check(noModels.GetSelectedModel().empty() && noModels.GetDraftText() == L"No active configuration",
            "Empty-library status was selected as a model or changed the draft.");

        std::atomic<int> discoveryPhase{0};
        ConnectorTest::MockConnectorServer recoveryServer([&discoveryPhase](const ConnectorTest::Request& request)
        {
            if (request.path == "/v1/models")
            {
                if (discoveryPhase.load() == 0) return ConnectorTest::Response{503, "{\"error\":{\"message\":\"model service unavailable\"}}", 150};
                if (discoveryPhase.load() == 2) return ConnectorTest::Response{200, "{\"object\":\"list\",\"data\":[]}", 150};
                auto response = ConnectorTest::MockConnectorServer::Default(request); response.delayMs = 150; return response;
            }
            return ConnectorTest::MockConnectorServer::Default(request);
        });
        AppController recovery;
        recovery.ConfigureSettingsPathForTesting((fixture.root / L"recovery.settings").wstring());
        Check(recovery.ApplyConnectorSettings({recoveryServer.Url()}, error), "Recovery fixture settings failed.");
        recovery.NewSession(); recovery.SetDraftText(L"Keep this while the model list reloads");
        recovery.HandleClick({HitTargetType::ComposerModelSelect});
        Check(recovery.GetActiveDropdown() == ActiveDropdown::Model && recovery.IsDiscoveringModels()
            && recovery.GetCurrentDropdownItems()[0].label == L"Loading Connector models..." && !recovery.GetCurrentDropdownItems()[0].enabled,
            "Loading model discovery left an invisible or selectable status menu.");
        Check(recoveryServer.WaitForRequests(1), "Loading-menu discovery did not reach the isolated server.");
        const auto loadingRequest = recoveryServer.RequestCount();
        recovery.HandleDropdownSelect(MenuIndex(recovery, L"Refresh models"));
        Check(recovery.IsDiscoveringModels() && recoveryServer.RequestCount() == loadingRequest,
            "Disabled refresh duplicated an in-progress discovery.");
        WaitUntil(recovery, [&] { return !recovery.IsDiscoveringModels(); }, "Failed discovery did not complete.");
        Check(recoveryServer.RequestCount() == loadingRequest && !recovery.IsConnectorConnected() && recovery.GetCurrentDropdownItems()[0].label == L"Unable to load models"
            && recovery.GetCurrentDropdownItems()[0].shortcut.find(L"model service unavailable") != std::wstring::npos
            && !recovery.GetCurrentDropdownItems()[0].enabled && recovery.GetCurrentDropdownItems()[MenuIndex(recovery, L"Refresh models")].enabled,
            "Failed discovery hid the model picker or omitted a useful retry/error state.");
        discoveryPhase.store(1); recovery.CloseDropdown();
        recovery.HandleClick({HitTargetType::ComposerModelSelect});
        Check(recovery.IsDiscoveringModels() && recovery.GetActiveDropdown() == ActiveDropdown::Model,
            "Reopening a disconnected picker did not retry model discovery.");
        WaitUntil(recovery, [&] { return recovery.IsConnectorConnected(); }, "Reopened picker failed to recover active models.");
        Check(recovery.GetCurrentDropdownItems().size() == 4 && recovery.GetCurrentDropdownItems()[0].label == L"studio-test"
            && recovery.GetTabs().size() == 1 && recovery.GetDraftText() == L"Keep this while the model list reloads",
            "Recovered model rows changed workspace content or lost the draft.");
        recovery.HandleDropdownSelect(1);
        recovery.HandleClick({HitTargetType::SidebarModelSelect});
        Check(recovery.GetCurrentDropdownItems()[1].selected && !recovery.GetCurrentDropdownItems()[0].selected,
            "Selected model marker did not follow a stable server model ID.");
        const auto savedRecoverySettings = Read(fixture.root / L"recovery.settings");
        discoveryPhase.store(0);
        recovery.HandleDropdownSelect(MenuIndex(recovery, L"Refresh models"));
        Check(recovery.GetActiveDropdown() == ActiveDropdown::SidebarModel && recovery.IsDiscoveringModels()
            && recovery.GetCurrentDropdownItems().size() == 5 && recovery.GetCurrentDropdownItems()[0].label == L"studio-test"
            && !recovery.GetCurrentDropdownItems()[0].enabled && recovery.GetCurrentDropdownItems()[1].selected,
            "Refresh closed the picker or erased models already retrieved from this server.");
        WaitUntil(recovery, [&] { return !recovery.IsDiscoveringModels(); }, "Cached-model failure did not complete.");
        recovery.HandleDropdownSelect(0); recovery.SendCurrentMessage();
        Check(!recovery.IsConnectorConnected() && recovery.GetCurrentDropdownItems().size() == 5
            && recovery.GetSelectedModel() == L"second-model" && recovery.GetDraftText() == L"Keep this while the model list reloads"
            && recovery.GetTabs()[0].messages.empty() && Read(fixture.root / L"recovery.settings") == savedRecoverySettings
            && Completions(recoveryServer).empty(),
            "Failed refresh lost cached IDs/preferences or sent a stale model/status as a completion.");
        discoveryPhase.store(1); recovery.HandleDropdownSelect(MenuIndex(recovery, L"Refresh models"));
        WaitUntil(recovery, [&] { return recovery.IsConnectorConnected(); }, "Explicit refresh failed to recover.");
        Check(recovery.GetActiveDropdown() == ActiveDropdown::SidebarModel && recovery.GetCurrentDropdownItems()[1].selected,
            "Successful refresh closed the picker or reset the saved selection.");
        recovery.HandleDropdownSelect(MenuIndex(recovery, L"Connection settings"));
        Check(recovery.GetActiveDropdown() == ActiveDropdown::None && recovery.GetSelectedModel() == L"second-model",
            "Settings action became a model selection.");
        recovery.SetActiveDropdown(ActiveDropdown::Model);
        Check(recovery.ApplyConnectorSettings({emptyServer.Url()}, error) && recovery.GetCurrentDropdownItems().size() == 3
            && recovery.GetCurrentDropdownItems()[0].label == L"Loading Connector models...",
            "Changing the Connector server exposed cached model IDs from the prior server.");
        WaitUntil(recovery, [&] { return recovery.IsConnectorConnected(); }, "Changed server discovery failed.");
        Check(recovery.GetCurrentDropdownItems()[0].label == L"No active models on Connector"
            && recovery.GetDraftText() == L"Keep this while the model list reloads", "Changed server lost its visible empty state or draft.");
        std::cout << "Async Connector discovery, full history, session ownership, settings, errors, cancellation, pending saves/close/shutdown, and active models passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
