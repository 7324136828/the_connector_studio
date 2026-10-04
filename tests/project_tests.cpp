#include "AppController.h"
#include "WorkProjectStore.h"
#include "SessionPersistence.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
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
            fs::create_directories(base);
            base = fs::weakly_canonical(base);
            root = base / (L"project-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            owned = fs::create_directory(root);
            Check(owned, "Test fixture already exists; existing files were preserved.");
        }
        ~Fixture()
        {
            // Recursive test cleanup is confined to the exact, exclusively-created child.
            if (owned && root.is_absolute() && root.parent_path() == base
                && root.filename().wstring().find(L"project-fixture-") == 0)
            { std::error_code ignored; fs::remove_all(root, ignored); }
        }
    };
    void Write(const fs::path& path, const std::string& text)
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text; Check(file.good(), "Fixture write failed."); }
    std::string Read(const fs::path& path)
    { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
    Lattice::NewItemRequest Request(Lattice::ItemCategory category, const wchar_t* id, const std::wstring& name, const fs::path& location)
    { return {category, id, name, location.wstring()}; }
}
int main()
{
    try
    {
        Fixture fixture;
        using namespace Lattice;
        std::wstring error;
        AppController controller; controller.Initialize(nullptr);
        Check(controller.GetTabs().empty() && controller.GetFiles().empty() && controller.GetCurrentProjectPath().empty(), "Startup created a project or session.");
        const auto project = fixture.root / L"Research \u03B1";
        auto request = Request(ItemCategory::Projects, L"work-project", project.filename().wstring(), fixture.root);
        Check(controller.CreateNewItem(request, error), "Work Project creation failed.");
        Check(fs::is_regular_file(project / L"project.connector") && fs::is_directory(project / L"files") && fs::is_directory(project / L"sessions"), "Project structure is incomplete.");
        Check(controller.GetCurrentProjectPath() == project.wstring() && controller.GetCurrentProjectName() == L"Research \u03B1"
            && controller.GetTabs().empty() && controller.GetActiveTab() == -1, "Empty project created a dummy session.");
        Check(controller.GetFiles().size() == 2 && controller.GetRecentProjects().size() == 1, "Project metadata leaked into explorer or recents were not recorded.");
        const auto descriptor = Read(project / L"project.connector");
        Check(!controller.CreateNewItem(request, error) && Read(project / L"project.connector") == descriptor, "Duplicate project overwrote existing data.");
        for (const auto& name : {L"..", L"../escape", L"a\\b", L"CON.txt", L"CON .txt", L"COM\u00B9", L"trail.", L" trailing", L"trailing ", L"bad:name"})
        {
            request.name = name;
            Check(!controller.CreateNewItem(request, error) && !error.empty(), "An unsafe project name was accepted.");
        }
        request = Request(ItemCategory::Files, L"text-file", L"Notes", project);
        Check(controller.CreateNewItem(request, error) && fs::file_size(project / L"files" / L"Notes.txt") == 0, "Text File was not created as empty UTF-8.");
        Check(controller.GetTabs().empty() && controller.GetActiveTab() == -1, "Creating a file fabricated a conversation.");
        Check(Read(project / L"files" / L"Notes.txt.connector-item").find("text-file") != std::string::npos, "File type metadata was lost.");
        for (const auto& file : controller.GetFiles())
            Check(file.name != L"project.connector" && file.name.find(L".connector-item") == std::wstring::npos, "Implementation metadata appeared in explorer.");
        Write(project / L"files" / L"Notes.txt", "User-owned content");
        Check(!controller.CreateNewItem(request, error) && Read(project / L"files" / L"Notes.txt") == "User-owned content", "Duplicate file overwrote content.");
        Check(controller.GetItemTypeRegistry().RegisterType({L"markdown-file", L"Markdown", L"Local notes", L".md", ItemCategory::Files}, error), "Custom file registration failed.");
        Check(controller.CreateNewItem(Request(ItemCategory::Files, L"markdown-file", L"Readme", project), error)
            && fs::exists(project / L"files" / L"Readme.md"), "Registered file type did not dispatch to safe file creation.");
        Check(!controller.CreateNewItem(Request(ItemCategory::Files, L"work-session", L"Mismatch", project), error)
            && !controller.CreateNewItem(Request(ItemCategory::Files, L"unknown", L"Missing", project), error), "Unregistered or wrong-category type was dispatched.");
        controller.SetActiveDropdown(ActiveDropdown::Model); controller.HandleDropdownSelect(1);
        request = Request(ItemCategory::Sessions, L"work-session", L"Planning \u03B2", project);
        Check(controller.CreateNewItem(request, error), "Saved Work Session creation failed.");
        const auto sessionFile = project / L"sessions" / L"Planning \u03B2.lattice";
        Check(controller.GetTabs().size() == 1 && controller.GetActiveTab() == 0 && controller.GetDraftText().empty()
            && controller.GetTabs()[0].messages.empty() && controller.GetTabs()[0].attachedPaths.empty()
            && !controller.GetTabs()[0].dirty && controller.GetSelectedModel() == L"GPT-4.1", "Saved session was not blank, clean, and preference-preserving.");
        SessionTab saved;
        Check(SessionPersistence::Load(sessionFile.wstring(), saved, error) && saved.projectName == L"Research \u03B1"
            && saved.projectPath == project.wstring() && saved.messages.empty(), "Saved session lost its validated project metadata.");
        Check(!controller.CreateNewItem(request, error) && controller.GetTabs().size() == 1, "Duplicate session overwrote or reopened an item.");
        controller.SetDraftText(L"Durable project draft \U0001F680");
        Check(controller.SaveSessionToPath(0, sessionFile.wstring(), error), "Work Session save failed.");
        AppController reopened; reopened.Initialize(nullptr);
        reopened.NewSession(); reopened.SetDraftText(L"Unrelated dirty draft");
        Check(reopened.OpenWorkProject((project / L"project.connector").wstring(), error), "Project descriptor reopen failed.");
        Check(reopened.GetTabs().size() == 2 && reopened.GetTabs()[0].draftText == L"Unrelated dirty draft"
            && reopened.GetTabs()[0].dirty && reopened.GetDraftText() == L"Durable project draft \U0001F680", "Project open lost unrelated work or imported extra sessions.");
        reopened.SetDraftText(L"Unsaved imported draft");
        Check(reopened.OpenWorkProject(project.wstring(), error) && reopened.GetTabs().size() == 2
            && reopened.GetDraftText() == L"Unsaved imported draft", "Reopening a project replaced an open dirty session.");
        reopened.CloseProject();
        Check(reopened.GetCurrentProjectPath().empty() && reopened.GetDraftText() == L"Unsaved imported draft", "Close Project discarded an existing draft.");
        Check(reopened.OpenWorkProject(project.wstring(), error), "Reopening a detached project failed.");
        reopened.SelectSession(reopened.GetActiveTab());
        Check(reopened.GetCurrentProjectPath() == project.wstring() && !reopened.GetFiles().empty()
            && reopened.GetTabs()[reopened.GetActiveTab()].projectPath == project.wstring()
            && reopened.GetTabs()[reopened.GetActiveTab()].projectName == L"Research \u03B1"
            && reopened.GetTabs()[reopened.GetActiveTab()].dirty && reopened.GetDraftText() == L"Unsaved imported draft",
            "Reopen did not restore matched session context or selecting its tab cleared the project.");
        Write(project / L"sessions" / L"Broken.lattice", "not a session");
        const auto active = reopened.GetActiveTab(); const auto tabCount = reopened.GetTabs().size();
        Check(!reopened.OpenWorkProject(project.wstring(), error) && reopened.GetActiveTab() == active
            && reopened.GetTabs().size() == tabCount && reopened.GetDraftText() == L"Unsaved imported draft", "Invalid project import mutated the workspace.");
        fs::remove(project / L"sessions" / L"Broken.lattice");
        const auto legacy = fixture.root / L"Legacy"; fs::create_directory(legacy); Write(legacy / L"existing.txt", "kept");
        Check(reopened.OpenWorkProject(legacy.wstring(), error) && reopened.GetActiveTab() == -1 && reopened.GetTabs().size() == tabCount
            && reopened.GetCurrentProjectName() == L"Legacy", "Legacy folder open created a dummy session or lost tabs.");
        Check(!reopened.CreateNewItem(Request(ItemCategory::Files, L"text-file", L"Invalid", legacy), error)
            && Read(legacy / L"existing.txt") == "kept", "Managed creation accepted a legacy folder or damaged files.");
        reopened.CloseProject();
        Check(reopened.GetCurrentProjectPath().empty() && reopened.GetFiles().empty() && reopened.GetTabs().size() == tabCount, "Close Project erased conversations.");

        const auto* textType = controller.GetItemTypeRegistry().Find(L"text-file");
        WorkProjectStore::CreatedItem created;
        Write(project / L"files" / L"Conflict.txt.connector-item", "existing metadata");
        Check(!WorkProjectStore::Create(Request(ItemCategory::Files, L"text-file", L"Conflict", project), *textType, created, error)
            && !fs::exists(project / L"files" / L"Conflict.txt")
            && Read(project / L"files" / L"Conflict.txt.connector-item") == "existing metadata", "Partial-create rollback deleted existing metadata or kept a new file.");
        Check(WorkProjectStore::Create(Request(ItemCategory::Files, L"text-file", L"Rollback", project), *textType, created, error), "Rollback fixture creation failed.");
        Write(created.path, "changed by user");
        Check(!WorkProjectStore::RollbackCreated(created, error) && Read(project / L"files" / L"Rollback.txt") == "changed by user", "Rollback deleted a modified file.");
        WorkProjectStore::CreatedItem clean;
        Check(WorkProjectStore::Create(Request(ItemCategory::Projects, L"work-project", L"Clean rollback", fixture.root),
            *controller.GetItemTypeRegistry().Find(L"work-project"), clean, error), "Project rollback fixture failed.");
        Check(WorkProjectStore::RollbackCreated(clean, error) && !fs::exists(fixture.root / L"Clean rollback"), "Clean project rollback did not remove only its own artifacts.");
        Check(controller.GetItemTypeRegistry().RegisterType({L"review-session", L"Review", L"Saved review", L".lattice", ItemCategory::Sessions}, error)
            && controller.CreateNewItem(Request(ItemCategory::Sessions, L"review-session", L"Review", project), error)
            && Read(project / L"sessions" / L"Review.lattice.connector-item").find("review-session") != std::string::npos, "Custom session type dispatch or metadata failed.");
        Check(controller.GetItemTypeRegistry().RegisterType({L"research-project", L"Research", L"Research workspace", L"", ItemCategory::Projects}, error)
            && controller.CreateNewItem(Request(ItemCategory::Projects, L"research-project", L"Custom project", fixture.root), error)
            && Read(fixture.root / L"Custom project" / L"project.connector").find("research-project") != std::string::npos,
            "Custom project type dispatch or descriptor failed.");
        Check(controller.GetActiveTab() == -1 && controller.GetTabs().size() == 2, "Empty custom project added a dummy session or removed prior work.");
        Check(controller.CreateNewItem(Request(ItemCategory::Projects, L"work-project", L"project.connector", fixture.root), error)
            && controller.GetCurrentProjectPath() == (fixture.root / L"project.connector").wstring(),
            "A valid project-folder name was confused with its descriptor filename.");
        std::cout << "Projects, saved sessions, text files, registered types, no-overwrite validation, nondestructive imports, and owned rollback passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
