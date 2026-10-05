#include "AppController.h"
#include "ChatCopy.h"
#include "SessionPersistence.h"
#include "MockConnectorServer.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    void Tick(Lattice::AppController& controller)
    {
        for (int i = 0; i < 500; ++i)
        {
            controller.OnTimer(); const int active = controller.GetActiveTab(); bool pending = false;
            for (int tab = 0; tab < static_cast<int>(controller.GetTabs().size()); ++tab)
            { controller.SelectSession(tab); pending = pending || controller.IsGeneratingReply(); }
            if (active >= 0) controller.SelectSession(active);
            if (!pending) return; Sleep(5);
        }
        throw std::runtime_error("Connector completion timed out.");
    }
    void Connect(Lattice::AppController& controller, const ConnectorTest::MockConnectorServer& server, const std::filesystem::path& path)
    {
        controller.ConfigureSettingsPathForTesting(path.wstring()); std::wstring error;
        Check(controller.ApplyConnectorSettings({server.Url()}, error), "Test server settings failed.");
        for (int i = 0; i < 500 && !controller.IsConnectorConnected(); ++i) { controller.OnTimer(); Sleep(5); }
        Check(controller.IsConnectorConnected(), "Test model discovery failed.");
    }
}
int main()
{
    const auto directory = std::filesystem::current_path() / (L"controller-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto file = directory / L"session.lattice";
    try
    {
        Lattice::SessionTab copyFixture;
        copyFixture.draftText = L"Unsent private draft";
        Check(!Lattice::ChatCopy::Transcript(copyFixture).success, "Empty chat copied a draft or fabricated text.");
        copyFixture.messages = {
            {L"user", L"You", L"10:00", L"**Question** \U0001F680\nsecond line", {}, {}},
            {L"assistant", L"test-model", L"10:01", L"| A | B |\r\n| --- | --- |\r| 1 | 2 |", {L"First item", L"Second item"}, {{2, L"Saved step", L"Step detail\ncontinued"}}},
            {L"assistant", L"", L"", L"", {}, {}}
        };
        const auto copiedMessage = Lattice::ChatCopy::Message(copyFixture.messages[1]);
        Check(copiedMessage.success && copiedMessage.text == L"| A | B |\r\n| --- | --- |\r\n| 1 | 2 |\r\n\r\n1. First item\r\n2. Second item\r\n\r\n2. Saved step\r\nStep detail\r\ncontinued",
            "Copy lost Markdown, structured message content, or Windows line endings.");
        const auto copiedChat = Lattice::ChatCopy::Transcript(copyFixture);
        Check(copiedChat.success && copiedChat.text == L"You:\r\n**Question** \U0001F680\r\nsecond line\r\n\r\ntest-model:\r\n" + copiedMessage.text
            && copyFixture.draftText == L"Unsent private draft" && copyFixture.messages.size() == 3,
            "Copy chat lost authors/order/Unicode, included an empty entry or changed session state.");
        Lattice::MessageItem invalidCopy; invalidCopy.text = std::wstring(L"hello\0hidden", 12);
        Check(!Lattice::ChatCopy::Message(invalidCopy).success && Lattice::ChatCopy::Message(invalidCopy).text.empty(),
            "An embedded null silently truncated copied chat.");
        ConnectorTest::MockConnectorServer server;
        Lattice::AppController controller;
        controller.Initialize(nullptr);
        Check(controller.GetTabs().empty() && controller.GetActiveTab() == -1, "Startup created a default session.");
        Check(controller.GetFiles().empty() && controller.GetSkills().empty() && controller.GetPlugins().empty(), "Startup populated demo workspace data.");
        Check(controller.GetAgents().empty() && !controller.ToggleAgent(-1) && !controller.ToggleAgent(0)
            && controller.GetTabs().empty(), "An empty workspace agent toggle created state.");
        Check(controller.GetDraftText().empty() && !controller.IsGeneratingReply() && controller.CanCloseApplication(), "Empty workspace has conversation state.");
        controller.SetDraftText(L"No session yet"); controller.SendCurrentMessage(); controller.CancelPendingReply();
        controller.ScrollActiveTab(1); controller.SetActiveScrollMetrics(100); controller.SetActiveScrollOffset(100);
        controller.SelectSession(0); controller.CloseSession(0); Tick(controller);
        controller.AttachFilePaths({(directory / L"missing.file").wstring(), std::filesystem::current_path().wstring()});
        Check(controller.GetTabs().empty() && controller.GetActiveTab() == -1 && controller.GetDraftText().empty(), "An invalid or inactive action created a session.");
        std::filesystem::create_directory(directory);
        Connect(controller, server, directory / L"controller.settings");
        controller.NewSession();
        controller.SetActiveDropdown(Lattice::ActiveDropdown::Session);
        Check(!controller.GetCurrentDropdownItems().back().enabled, "Copy chat was enabled for an empty session.");
        controller.CloseDropdown();
        Check(controller.GetActiveTab() == 0 && controller.GetTabs()[0].id == 0 && controller.GetTabs()[0].title == L"Session 1", "First explicit session did not start fresh.");
        Check(controller.GetTabs()[0].messages.empty() && controller.GetTabs()[0].projectName.empty() && controller.GetTabs()[0].projectPath.empty()
            && controller.GetTabs()[0].attachedPaths.empty() && controller.GetTabs()[0].filesCount == 0, "New session contains preload data.");
        controller.SetDraftText(L"First draft \U0001F680");
        controller.NewSession(); Check(controller.GetDraftText().empty(), "Draft leaked to another tab.");
        controller.SetDraftText(L"Second draft");
        controller.SelectSession(0); Check(controller.GetDraftText() == L"First draft \U0001F680", "Original draft was lost.");
        controller.SendCurrentMessage(); Check(controller.IsGeneratingReply(), "Reply was not queued.");
        controller.SetDraftText(L"Next message"); controller.SendCurrentMessage();
        Check(controller.GetDraftText() == L"Next message", "Busy send discarded the next draft.");
        controller.SelectSession(1); controller.SendCurrentMessage();
        const auto firstCount = controller.GetTabs()[0].messages.size(), secondCount = controller.GetTabs()[1].messages.size();
        Tick(controller);
        Check(controller.GetTabs()[0].messages.size() == firstCount + 1 && controller.GetTabs()[1].messages.size() == secondCount + 1, "Replies went to the wrong tab or were overwritten.");
        Check(controller.GetTabs()[0].messages.back().text == L"Connector test reply", "Reply did not come from the configured Connector server.");
        controller.SelectSession(0); Check(controller.GetDraftText() == L"Next message", "Timer replaced the draft.");
        controller.SetActiveDropdown(Lattice::ActiveDropdown::Session);
        Check(controller.GetCurrentDropdownItems().back().label == L"Copy chat" && controller.GetCurrentDropdownItems().back().enabled,
            "Copy chat was unavailable for a conversation with messages.");
        controller.CloseDropdown();
        controller.ToggleVoice(); Check(controller.GetDraftText() == L"Next message", "Voice command overwrote the draft.");
        controller.SetDraftText(L" \r\n\t"); controller.SendCurrentMessage();
        Check(controller.GetDraftText() == L" \r\n\t" && !controller.IsGeneratingReply(), "Whitespace send lost input.");
        controller.SetActiveScrollMetrics(250); controller.SetActiveScrollOffset(999);
        Check(controller.GetTabs()[0].scrollOffset == 250, "Scroll escaped content bounds.");
        controller.SetActiveScrollOffset(-10); Check(controller.GetTabs()[0].scrollOffset == 0, "Scroll offset became negative.");
        controller.SetDraftText(L"New scroll message"); controller.SendCurrentMessage(); controller.SetActiveScrollMetrics(400);
        Check(controller.GetTabs()[0].scrollOffset == 400, "Send did not reveal latest message.");
        controller.SetActiveScrollOffset(50); Tick(controller); controller.SetActiveScrollMetrics(600);
        Check(controller.GetTabs()[0].scrollOffset == 50, "Reply jumped while reading earlier messages.");

        controller.HandleClick({Lattice::HitTargetType::MenuProject, -1, {}});
        Check(controller.GetActiveDropdown() == Lattice::ActiveDropdown::Project, "Menu did not open.");
        controller.HandleClick({Lattice::HitTargetType::MenuProject, -1, {}});
        Check(controller.GetActiveDropdown() == Lattice::ActiveDropdown::None, "Menu did not close on second click.");
        controller.SetActiveDropdown(Lattice::ActiveDropdown::Model); controller.HandleDropdownSelect(1);
        Check(controller.GetSelectedModel() == L"second-model", "Model selection did not change.");
        controller.SelectSession(1); Check(controller.GetSelectedModel() == L"studio-test", "Model leaked across sessions.");
        controller.SelectSession(0);
        std::wstring error;
        controller.SetDraftText(L"Durable draft");
        const auto draft = controller.GetDraftText();
        Check(!controller.SaveSessionToPath(0, directory.wstring(), error), "Invalid destination accepted.");
        Check(controller.GetDraftText() == draft && controller.GetTabs()[0].dirty, "Failed save discarded changes.");
        Check(controller.SaveSessionToPath(0, file.wstring(), error), "Controller save failed.");
        Check(!controller.GetTabs()[0].dirty, "Save did not clear dirty state.");
        const auto existing = controller.GetTabs().size();
        Check(!controller.OpenSessionFromPath((directory / L"missing.lattice").wstring(), error) && controller.GetTabs().size() == existing, "Failed open changed sessions.");
        Lattice::AppController reopened;
        Check(reopened.GetTabs().empty(), "Reopened controller preloaded an unrelated session.");
        Check(reopened.OpenSessionFromPath(file.wstring(), error), "Controller open failed.");
        Check(reopened.GetTabs().size() == 1 && reopened.GetActiveTab() == 0 && reopened.GetDraftText() == draft
            && reopened.GetSelectedModel() == L"second-model", "Controller import lost saved data or added a default tab.");

        Lattice::AppController closeController;
        Connect(closeController, server, directory / L"close.settings");
        closeController.NewSession(); closeController.NewSession(); const int activeId = closeController.GetTabs()[1].id;
        closeController.SetDraftText(L"Reply ownership"); closeController.SendCurrentMessage();
        closeController.CloseSession(0); Check(closeController.GetActiveTab() == 0 && closeController.GetTabs()[0].id == activeId, "Closing earlier tab changed selection.");
        const auto count = closeController.GetTabs()[0].messages.size(); Tick(closeController);
        Check(closeController.GetTabs()[0].messages.size() == count + 1, "Stable reply ownership broke after close.");
        Check(closeController.SaveSessionToPath(0, file.wstring(), error), "Clean close preparation failed.");
        closeController.CloseSession(0);
        Check(closeController.GetTabs().empty() && closeController.GetActiveTab() == -1 && closeController.GetFiles().empty()
            && closeController.GetDraftText().empty(), "Closing the last session recreated a tab or kept project context.");
        closeController.AttachFilePaths({directory.wstring()});
        Check(closeController.GetTabs().empty(), "An invalid attachment recreated the closed session.");
        closeController.AttachFilePaths({file.wstring(), file.wstring(), directory.wstring()});
        Check(closeController.GetTabs().size() == 1 && closeController.GetTabs()[0].id > activeId && closeController.GetTabs()[0].messages.empty(), "First valid attachment did not create a fresh blank session.");
        Check(closeController.GetTabs()[0].attachedPaths.size() == 1 && closeController.GetTabs()[0].filesCount == 1
            && closeController.GetTabs()[0].projectPath.empty() && closeController.GetTabs()[0].projectName.empty(), "Attachments were duplicated, accepted a directory, or created a project.");
        closeController.CloseSession(-1); Check(closeController.GetTabs().size() == 1, "Invalid close cleared a session.");
        Lattice::AppController preferences;
        Connect(preferences, server, directory / L"preferences.settings");
        preferences.SetActiveDropdown(Lattice::ActiveDropdown::SidebarModel); preferences.HandleDropdownSelect(1);
        preferences.HandleClick({Lattice::HitTargetType::SidebarTogglePlan, -1, {}});
        preferences.HandleClick({Lattice::HitTargetType::SidebarToggleSafeTools, -1, {}});
        Check(preferences.GetTabs().empty() && preferences.GetActiveTab() == -1, "Changing default preferences created a session.");
        preferences.NewSession();
        Check(preferences.GetSelectedModel() == L"second-model" && preferences.GetPlanBeforeEdits() && preferences.GetAutoRunSafeTools()
            && preferences.GetTabs()[0].messages.empty(), "Removed legacy preference controls changed defaults or model selection.");
        preferences.CloseSession(0);
        Check(preferences.GetTabs().empty() && preferences.GetSelectedModel() == L"second-model", "Empty workspace reset default preferences.");
        for (const auto* settings : {L"controller.settings", L"close.settings", L"preferences.settings"}) std::filesystem::remove(directory / settings);
        std::filesystem::remove(file); std::filesystem::remove(directory);
        std::cout << "Controller empty startup, explicit sessions, draft isolation, replies, persistence, scrolling, and attachments passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored; std::filesystem::remove(file, ignored);
        for (const auto* settings : {L"controller.settings", L"close.settings", L"preferences.settings"}) std::filesystem::remove(directory / settings, ignored);
        std::filesystem::remove(directory, ignored);
        std::cerr << exception.what() << '\n'; return 1;
    }
}
