#include "AppController.h"
#include "SessionPersistence.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    void Tick(Lattice::AppController& controller) { for (int i = 0; i < 7; ++i) controller.OnTimer(); }
}
int main()
{
    const auto directory = std::filesystem::current_path() / (L"controller-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto file = directory / L"session.lattice";
    try
    {
        Lattice::AppController controller;
        controller.Initialize(nullptr);
        Check(controller.GetTabs().empty() && controller.GetActiveTab() == -1, "Startup created a default session.");
        Check(controller.GetFiles().empty() && controller.GetSkills().empty() && controller.GetPlugins().empty(), "Startup populated demo workspace data.");
        Check(controller.GetDraftText().empty() && !controller.IsGeneratingReply() && controller.CanCloseApplication(), "Empty workspace has conversation state.");
        controller.SetDraftText(L"No session yet"); controller.SendCurrentMessage(); controller.CancelPendingReply();
        controller.ScrollActiveTab(1); controller.SetActiveScrollMetrics(100); controller.SetActiveScrollOffset(100);
        controller.SelectSession(0); controller.CloseSession(0); Tick(controller);
        controller.AttachFilePaths({(directory / L"missing.file").wstring(), std::filesystem::current_path().wstring()});
        Check(controller.GetTabs().empty() && controller.GetActiveTab() == -1 && controller.GetDraftText().empty(), "An invalid or inactive action created a session.");
        controller.NewSession();
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
        Check(controller.GetTabs()[0].messages.back().text.find(L"No AI provider") != std::wstring::npos, "Offline reply makes a false provider claim.");
        controller.SelectSession(0); Check(controller.GetDraftText() == L"Next message", "Timer replaced the draft.");
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
        Check(controller.GetSelectedModel() == L"GPT-4.1", "Model selection did not change.");
        controller.SelectSession(1); Check(controller.GetSelectedModel() == L"Claude 3.7 Sonnet", "Model leaked across sessions.");
        controller.SelectSession(0);
        std::filesystem::create_directory(directory); std::wstring error;
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
            && reopened.GetSelectedModel() == L"GPT-4.1", "Controller import lost saved data or added a default tab.");

        Lattice::AppController closeController;
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
        preferences.SetActiveDropdown(Lattice::ActiveDropdown::SidebarModel); preferences.HandleDropdownSelect(1);
        preferences.HandleClick({Lattice::HitTargetType::SidebarTogglePlan, -1, {}});
        preferences.HandleClick({Lattice::HitTargetType::SidebarToggleSafeTools, -1, {}});
        Check(preferences.GetTabs().empty() && preferences.GetActiveTab() == -1, "Changing default preferences created a session.");
        preferences.NewSession();
        Check(preferences.GetSelectedModel() == L"GPT-4.1" && !preferences.GetPlanBeforeEdits() && !preferences.GetAutoRunSafeTools()
            && preferences.GetTabs()[0].messages.empty(), "Explicit blank session lost selected default preferences.");
        preferences.CloseSession(0);
        Check(preferences.GetTabs().empty() && preferences.GetSelectedModel() == L"GPT-4.1", "Empty workspace reset default preferences.");
        std::filesystem::remove(file); std::filesystem::remove(directory);
        std::cout << "Controller empty startup, explicit sessions, draft isolation, replies, persistence, scrolling, and attachments passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored; std::filesystem::remove(file, ignored); std::filesystem::remove(directory, ignored);
        std::cerr << exception.what() << '\n'; return 1;
    }
}
