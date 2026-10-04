#include "AppController.h"
#include <commdlg.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include "SessionPersistence.h"
#include "AppIdentity.h"
#include "WorkProjectStore.h"
#include "NewItemDialog.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>

namespace Lattice
{
    AppController::AppController()
    {
        InitializeMenus();
    }

    void AppController::Initialize(HWND hwnd)
    {
        m_hwnd = hwnd;
        if (hwnd)
        {
            ReadRecentProjects();
            PWSTR appData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
            {
                const auto path = std::filesystem::path(appData) / AppIdentity::SettingsFolder / L"item-types.registry";
                CoTaskMemFree(appData);
                std::wstring ignored;
                m_itemTypeRegistry.Load(path.wstring(), ignored);
            }
        }
    }

    void AppController::InitializeMenus()
    {
        // Menus are available even when the workspace has no sessions.
        m_projectMenuItems = {
            { L"New Project", L"Ctrl+Shift+N", false },
            { L"Close Project", L"Ctrl+Shift+W", false },
            { L"Recent Projects", L"›", true }
        };

        m_sessionMenuItems = {
            { L"New Session", L"Ctrl+N", false },
            { L"Open Session", L"Ctrl+O", false },
            { L"Save Session", L"Ctrl+S", false },
            { L"Close Session", L"Ctrl+W", false }
        };

        m_modelMenuItems = {
            { L"Claude 3.7 Sonnet", L"Offline", false },
            { L"GPT-4.1", L"Offline", false },
            { L"Gemini 2.5 Pro", L"Offline", false }
        };
    }

    namespace
    {
        constexpr std::size_t MaxSessions = 100, MaxAttachments = 1000, MaxProjectEntries = 1000;
        std::wstring Timestamp()
        {
            SYSTEMTIME time{}; GetLocalTime(&time); wchar_t buffer[40]{};
            if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, buffer, 40)) return L"Just now";
            return buffer;
        }
        bool SamePath(const std::wstring& lhs, const std::wstring& rhs)
        { return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) == CSTR_EQUAL; }
        void DialogError(HWND owner, const std::wstring& error, const wchar_t* title)
        { MessageBoxW(owner, error.c_str(), title, MB_OK | MB_ICONERROR); }
        std::wstring FileType(const std::filesystem::path& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (ext == L".md" || ext == L".txt") return L"md";
            if (ext == L".tsx" || ext == L".jsx") return L"react";
            if (ext == L".ts" || ext == L".js") return L"ts";
            if (path.filename().wstring().find(L".env") == 0) return L"env";
            return L"file";
        }
        MessageItem LocalReply(const SessionTab& tab)
        {
            return {L"assistant", AppIdentity::Name, Timestamp(),
                L"Added to this local conversation. No AI provider is connected, so I cannot analyze the request or edit your project. The selected " + tab.selectedModel + L" model is a session preference only.",
                {L"Save the conversation, draft, context paths, and preferences with Ctrl+S.", L"Attached file contents are not uploaded."}, {}};
        }
        std::wstring RecentFilePath(bool forWrite = false)
        {
            PWSTR appData = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) return {};
            const std::filesystem::path base(appData);
            CoTaskMemFree(appData);
            const auto folder = base / AppIdentity::SettingsFolder;
            const auto path = folder / L"recent-projects.txt";
            std::error_code error;
            if (forWrite)
            {
                std::filesystem::create_directories(folder, error);
                return error ? std::wstring{} : path.wstring();
            }
            // A renamed installation can read preferences without moving user files.
            if (!std::filesystem::exists(path, error) && !error)
            {
                const auto legacy = base / AppIdentity::LegacySettingsFolder / L"recent-projects.txt";
                if (std::filesystem::is_regular_file(legacy, error) && !error) return legacy.wstring();
            }
            return path.wstring();
        }
        void ScanDirectory(const std::filesystem::path& directory, int indent, std::vector<FileItem>& files)
        {
            if (indent > 8 || files.size() >= MaxProjectEntries) return;
            std::error_code error; std::vector<std::filesystem::directory_entry> entries;
            std::filesystem::directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied, error);
            while (!error && iterator != std::filesystem::directory_iterator{})
            { if (entries.size() >= MaxProjectEntries) break; entries.push_back(*iterator); iterator.increment(error); }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
            {
                std::error_code ae, be; const bool ad = a.is_directory(ae), bd = b.is_directory(be);
                if (ad != bd) return ad;
                return CompareStringOrdinal(a.path().filename().c_str(), -1, b.path().filename().c_str(), -1, TRUE) == CSTR_LESS_THAN;
            });
            for (const auto& entry : entries)
            {
                if (files.size() >= MaxProjectEntries) break;
                const auto name = entry.path().filename().wstring();
                if (name == L".git" || name == L"node_modules" || name == L"build" || name == L".vs") continue;
                if (SamePath(name, L"project.connector") || (name.size() >= 15 && SamePath(name.substr(name.size() - 15), L".connector-item"))) continue;
                error.clear(); const bool folder = entry.is_directory(error); if (error) continue;
                files.push_back({name, folder ? L"folder" : FileType(entry.path()), indent, folder, true, entry.path().wstring()});
                const auto attributes = GetFileAttributesW(entry.path().c_str());
                if (folder && attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) ScanDirectory(entry.path(), indent + 1, files);
            }
        }
    }
    void AppController::Redraw() { if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); }
    SessionTab* AppController::ActiveSession()
    { return m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size()) ? &m_tabs[m_activeTab] : nullptr; }
    const SessionTab* AppController::ActiveSession() const
    { return m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size()) ? &m_tabs[m_activeTab] : nullptr; }
    const std::wstring& AppController::GetDraftText() const
    { static const std::wstring empty; const auto* tab = ActiveSession(); return tab ? tab->draftText : empty; }
    const std::wstring& AppController::GetSelectedModel() const
    { const auto* tab = ActiveSession(); return tab ? tab->selectedModel : m_defaultSelectedModel; }
    bool AppController::GetPlanBeforeEdits() const { const auto* tab = ActiveSession(); return tab ? tab->planBeforeEdits : m_defaultPlanBeforeEdits; }
    bool AppController::GetAutoRunSafeTools() const { const auto* tab = ActiveSession(); return tab ? tab->autoRunSafeTools : m_defaultAutoRunSafeTools; }
    void AppController::SetDraftText(const std::wstring& text)
    { if (auto* tab = ActiveSession(); tab && tab->draftText != text) { tab->draftText = text; tab->dirty = true; } }
    void AppController::AppendDraftText(const std::wstring& text) { SetDraftText(GetDraftText() + text); Redraw(); }
    bool AppController::IsGeneratingReply() const
    { const auto* tab = ActiveSession(); return tab && std::any_of(m_pendingReplies.begin(), m_pendingReplies.end(), [tab](const auto& reply) { return reply.sessionId == tab->id; }); }
    const std::vector<MenuItem>& AppController::GetCurrentDropdownItems() const
    {
        if (m_activeDropdown == ActiveDropdown::Project) return m_projectMenuItems;
        if (m_activeDropdown == ActiveDropdown::Session) return m_sessionMenuItems;
        if (m_activeDropdown == ActiveDropdown::Model || m_activeDropdown == ActiveDropdown::SidebarModel) return m_modelMenuItems;
        static const std::vector<MenuItem> empty; return empty;
    }
    void AppController::HandleClick(const HitTestResult& target)
    {
        if (target.type == HitTargetType::DropdownItem) { HandleDropdownSelect(target.index); return; }
        const auto previous = m_activeDropdown; CloseDropdown();
        auto toggle = [this, previous](ActiveDropdown value) { m_activeDropdown = previous == value ? ActiveDropdown::None : value; };
        switch (target.type)
        {
        case HitTargetType::TrafficClose: if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
        case HitTargetType::TrafficMinimize: if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE); break;
        case HitTargetType::TrafficMaximize: if (m_hwnd) ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case HitTargetType::MenuProject: toggle(ActiveDropdown::Project); break;
        case HitTargetType::MenuSession: toggle(ActiveDropdown::Session); break;
        case HitTargetType::SidebarModelSelect: toggle(ActiveDropdown::SidebarModel); break;
        case HitTargetType::ComposerModelSelect: toggle(ActiveDropdown::Model); break;
        case HitTargetType::TitleSearch: break; // Main provides native Find.
        case HitTargetType::ActivityFiles: m_sidebarMode = SidebarMode::Files; m_sidebarScrollOffset = 0; break;
        case HitTargetType::ActivitySession: m_sidebarMode = SidebarMode::Session; m_sidebarScrollOffset = 0; break;
        case HitTargetType::ActivityPlugins: m_sidebarMode = SidebarMode::Plugins; m_sidebarScrollOffset = 0; break;
        case HitTargetType::ActivitySettings:
            MessageBoxW(m_hwnd, L"Native dark theme\nPer-monitor DPI support\nVersioned local sessions (.lattice)\nWindows dictation: microphone or Win+H\n\nModel, planning, and tool preferences are stored per session. AI providers and plugin execution are not configured.", L"Settings", MB_OK | MB_ICONINFORMATION); break;
        case HitTargetType::TabItem: SelectSession(target.index); break;
        case HitTargetType::TabClose: CloseSession(target.index); break;
        case HitTargetType::TabNew: NewSession(); break;
        case HitTargetType::SidebarProjectRow: m_projectExpanded = !m_projectExpanded; m_sidebarScrollOffset = 0; break;
        case HitTargetType::SidebarFileRow:
            if (target.index >= 0 && target.index < static_cast<int>(m_files.size()))
            {
                auto& file = m_files[target.index];
                if (file.isFolder) file.isExpanded = !file.isExpanded;
                else if (!file.fullPath.empty()) AttachFilePaths({file.fullPath});
                else MessageBoxW(m_hwnd, L"This file has no local path. Choose Project > New Project to browse and attach a file.", L"Project File", MB_OK | MB_ICONINFORMATION);
            } break;
        case HitTargetType::SidebarTogglePlan:
            if (auto* tab = ActiveSession()) { tab->planBeforeEdits = !tab->planBeforeEdits; tab->dirty = true; }
            else m_defaultPlanBeforeEdits = !m_defaultPlanBeforeEdits;
            break;
        case HitTargetType::SidebarAgentSelect:
            MessageBoxW(m_hwnd, L"Code Agent is a saved design preference. No agent service or tool executor is connected in this local build. Messages do not trigger file edits or commands.", L"Code Agent", MB_OK | MB_ICONINFORMATION); break;
        case HitTargetType::SidebarToggleSafeTools:
            if (auto* tab = ActiveSession()) { tab->autoRunSafeTools = !tab->autoRunSafeTools; tab->dirty = true; }
            else m_defaultAutoRunSafeTools = !m_defaultAutoRunSafeTools;
            break;
        case HitTargetType::SidebarSkillItem:
            if (target.index >= 0 && target.index < static_cast<int>(m_skills.size()))
            {
                auto& skill = m_skills[target.index];
                if (skill.fullPath.empty()) MessageBoxW(m_hwnd, L"This skill has no local instruction file. Use Add Custom Skill to attach Markdown instructions.", L"Skill", MB_OK | MB_ICONINFORMATION);
                else skill.isEnabled = !skill.isEnabled;
            } break;
        case HitTargetType::SidebarAddSkill: AddCustomSkill(); break;
        case HitTargetType::SidebarPluginCard:
        case HitTargetType::SidebarBrowsePlugins: BrowsePlugins(); break;
        case HitTargetType::ComposerAttach: AttachFile(); break;
        case HitTargetType::ComposerContextChip: ShowContextFiles(); break;
        case HitTargetType::ComposerMic: ToggleVoice(); break;
        case HitTargetType::ComposerSend: SendCurrentMessage(); break;
        default: break;
        } Redraw();
    }
    void AppController::HandleDropdownSelect(int index)
    {
        const auto dropdown = m_activeDropdown;
        if (index < 0 || index >= static_cast<int>(GetCurrentDropdownItems().size())) return;
        CloseDropdown();
        if (dropdown == ActiveDropdown::Project) { if (index == 0) NewProject(); else if (index == 1) CloseProject(); else if (index == 2) ShowRecentProjects(); }
        else if (dropdown == ActiveDropdown::Session) { if (index == 0) NewSession(); else if (index == 1) OpenSession(); else if (index == 2) SaveSession(); else if (index == 3) CloseSession(m_activeTab); }
        else if (dropdown == ActiveDropdown::Model || dropdown == ActiveDropdown::SidebarModel)
        {
            if (auto* tab = ActiveSession()) { tab->selectedModel = m_modelMenuItems[index].label; tab->dirty = true; }
            else m_defaultSelectedModel = m_modelMenuItems[index].label;
        }
        Redraw();
    }
    void AppController::SetActiveScrollMetrics(float maxScroll)
    {
        if (auto* tab = ActiveSession())
        {
            const bool atBottom = tab->maxScroll > 0 && tab->scrollOffset >= tab->maxScroll - 1;
            tab->maxScroll = std::isfinite(maxScroll) ? (std::max)(0.0f, maxScroll) : 0;
            tab->scrollOffset = tab->scrollToEnd || atBottom ? tab->maxScroll : (std::clamp)(tab->scrollOffset, 0.0f, tab->maxScroll);
            tab->scrollToEnd = false;
        }
    }
    void AppController::SetActiveScrollOffset(float offset)
    { if (auto* tab = ActiveSession(); tab && std::isfinite(offset)) { tab->scrollOffset = (std::clamp)(offset, 0.0f, tab->maxScroll); tab->scrollToEnd = false; Redraw(); } }
    void AppController::ScrollActiveTab(float delta) { if (const auto* tab = ActiveSession()) SetActiveScrollOffset(tab->scrollOffset - delta * 40); }
    void AppController::SetSidebarScrollMetrics(float maxScroll)
    { m_sidebarMaxScroll = std::isfinite(maxScroll) ? (std::max)(0.0f, maxScroll) : 0; m_sidebarScrollOffset = (std::clamp)(m_sidebarScrollOffset, 0.0f, m_sidebarMaxScroll); }
    void AppController::SetSidebarScrollOffset(float offset)
    { if (std::isfinite(offset)) { m_sidebarScrollOffset = (std::clamp)(offset, 0.0f, m_sidebarMaxScroll); Redraw(); } }
    void AppController::ScrollSidebar(float delta) { SetSidebarScrollOffset(m_sidebarScrollOffset - delta * 40); }
    void AppController::NewSession()
    {
        if (m_tabs.size() >= MaxSessions) { MessageBoxW(m_hwnd, L"Close a session before opening another (100 maximum).", L"New Session", MB_OK | MB_ICONINFORMATION); return; }
        SessionTab tab;
        tab.id = m_nextSessionId++; tab.title = L"Session " + std::to_wstring(tab.id + 1); tab.eyebrow = L"SESSION " + std::to_wstring(tab.id + 1);
        tab.projectName = m_currentProjectName;
        tab.projectPath = m_currentProjectPath; tab.selectedModel = GetSelectedModel(); tab.filesCount = 0;
        tab.planBeforeEdits = GetPlanBeforeEdits(); tab.autoRunSafeTools = GetAutoRunSafeTools();
        m_tabs.push_back(std::move(tab)); m_activeTab = static_cast<int>(m_tabs.size()) - 1;
        CloseDropdown(); RefreshProjectFiles(); Redraw();
    }
    bool AppController::ConfirmClose(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_tabs.size())) return false;
        if (!m_tabs[index].dirty) return true;
        const auto text = L"Save changes to \"" + m_tabs[index].title + L"\" before closing?\nThis includes conversation, draft, context paths, and preferences.";
        const int choice = MessageBoxW(m_hwnd, text.c_str(), L"Unsaved Session", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDYES) return SaveSessionWithDialog(index); return choice == IDNO;
    }
    bool AppController::CanCloseApplication()
    { for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) if (!ConfirmClose(i)) return false; return true; }
    void AppController::CloseSession(int index)
    {
        if (!ConfirmClose(index)) return;
        const int id = m_tabs[index].id;
        m_pendingReplies.erase(std::remove_if(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; }), m_pendingReplies.end());
        m_tabs.erase(m_tabs.begin() + index);
        if (index < m_activeTab) --m_activeTab;
        else if (index == m_activeTab) m_activeTab = (std::min)(index, static_cast<int>(m_tabs.size()) - 1);
        if (m_tabs.empty()) m_activeTab = -1;
        if (ActiveSession()) SyncProjectFromActiveSession();
        RefreshProjectFiles(); CloseDropdown(); Redraw();
    }
    void AppController::SelectSession(int index)
    { if (index >= 0 && index < static_cast<int>(m_tabs.size())) { m_activeTab = index; SyncProjectFromActiveSession(); CloseDropdown(); RefreshProjectFiles(); Redraw(); } }
    void AppController::SyncProjectFromActiveSession()
    {
        if (const auto* tab = ActiveSession())
        { m_currentProjectPath = tab->projectPath; m_currentProjectName = tab->projectName; m_projectExpanded = true; }
    }
    bool AppController::SaveSessionToPath(int index, const std::wstring& path, std::wstring& error)
    {
        if (index < 0 || index >= static_cast<int>(m_tabs.size())) { error = L"Select a session first."; return false; }
        SessionTab snapshot = m_tabs[index]; const int id = snapshot.id;
        const bool pending = std::any_of(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; });
        if (pending)
        {
            snapshot.scrollToEnd = snapshot.scrollToEnd || snapshot.scrollOffset >= snapshot.maxScroll - 1;
            snapshot.messages.push_back(LocalReply(snapshot));
        }
        if (!SessionPersistence::Save(path, snapshot, error)) return false;
        snapshot.savedPath = path; snapshot.dirty = false; m_tabs[index] = std::move(snapshot);
        m_pendingReplies.erase(std::remove_if(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; }), m_pendingReplies.end());
        Redraw(); return true;
    }
    bool AppController::SaveSessionWithDialog(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_tabs.size())) return false;
        std::wstring path = m_tabs[index].savedPath;
        if (path.empty())
        {
            std::wstring filename = m_tabs[index].title + L".lattice", ignored;
            if (!WorkProjectStore::ValidateName(filename, ignored)) filename = L"Session.lattice";
            std::vector<wchar_t> buffer(32768, 0); wcscpy_s(buffer.data(), buffer.size(), filename.c_str()); OPENFILENAMEW dialog{};
            std::wstring initialDirectory; WorkProjectStore::ProjectInfo project;
            if (!m_tabs[index].projectPath.empty() && WorkProjectStore::Open(m_tabs[index].projectPath, project, ignored) && project.managed)
                initialDirectory = (std::filesystem::path(project.rootPath) / L"sessions").wstring();
            dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = m_hwnd; dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
            if (!initialDirectory.empty()) dialog.lpstrInitialDir = initialDirectory.c_str();
            dialog.lpstrFilter = L"Connector Studio Sessions (*.lattice)\0*.lattice\0All Files (*.*)\0*.*\0";
            dialog.nFilterIndex = 1; dialog.lpstrDefExt = L"lattice"; dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&dialog)) { if (CommDlgExtendedError()) DialogError(m_hwnd, L"Windows could not display Save Session.", L"Save Session"); return false; }
            path = buffer.data();
        }
        std::wstring error; if (!SaveSessionToPath(index, path, error)) { DialogError(m_hwnd, error, L"Save Session"); return false; } return true;
    }
    void AppController::SaveSession() { SaveSessionWithDialog(m_activeTab); }
    bool AppController::OpenSessionFromPath(const std::wstring& path, std::wstring& error)
    {
        std::error_code filesystemError; const auto canonical = std::filesystem::weakly_canonical(path, filesystemError);
        if (path.empty() || filesystemError) { error = L"Choose an existing saved session."; return false; }
        const auto sessionPath = canonical.wstring();
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
            if (!m_tabs[i].savedPath.empty() && SamePath(m_tabs[i].savedPath, sessionPath)) { SelectSession(i); error.clear(); return true; }
        if (m_tabs.size() >= MaxSessions) { error = L"Close a session before opening another (100 maximum)."; return false; }
        SessionTab tab; if (!SessionPersistence::Load(sessionPath, tab, error)) return false;
        tab.id = m_nextSessionId++; tab.savedPath = sessionPath; tab.dirty = false;
        m_tabs.push_back(std::move(tab)); m_activeTab = static_cast<int>(m_tabs.size()) - 1;
        SyncProjectFromActiveSession(); CloseDropdown(); RefreshProjectFiles(); Redraw(); return true;
    }
    void AppController::OpenSession()
    {
        std::vector<wchar_t> buffer(32768, 0); OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = m_hwnd; dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter = L"Connector Studio Sessions (*.lattice)\0*.lattice\0All Files (*.*)\0*.*\0"; dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) { if (CommDlgExtendedError()) DialogError(m_hwnd, L"Windows could not display Open Session.", L"Open Session"); return; }
        std::wstring error; if (!OpenSessionFromPath(buffer.data(), error)) DialogError(m_hwnd, error, L"Open Session");
    }
    bool AppController::OpenWorkProject(const std::wstring& path, std::wstring& error)
    {
        WorkProjectStore::ProjectInfo project;
        if (!WorkProjectStore::Open(path, project, error)) return false;
        std::vector<SessionTab> loaded;
        std::vector<int> matched;
        int selected = -1;
        for (const auto& sessionPath : project.sessionPaths)
        {
            const auto existing = std::find_if(m_tabs.begin(), m_tabs.end(), [&sessionPath](const auto& tab)
            { return !tab.savedPath.empty() && SamePath(tab.savedPath, sessionPath); });
            if (existing != m_tabs.end())
            {
                const int index = static_cast<int>(existing - m_tabs.begin());
                matched.push_back(index); if (selected < 0) selected = index; continue;
            }
            if (m_tabs.size() + loaded.size() >= MaxSessions)
            { error = L"Close sessions before opening this project (100 open sessions maximum)."; return false; }
            SessionTab tab;
            if (!SessionPersistence::Load(sessionPath, tab, error))
            { error = L"The project could not open a saved session: " + std::filesystem::path(sessionPath).filename().wstring() + L"\n" + error; return false; }
            // A moved project owns its saved sessions at its current location.
            tab.projectPath = project.rootPath; tab.projectName = project.name;
            tab.savedPath = sessionPath; tab.dirty = false;
            loaded.push_back(std::move(tab));
        }
        // Nothing in the current workspace is changed until every new import is valid.
        const int firstNew = static_cast<int>(m_tabs.size());
        m_tabs.reserve(m_tabs.size() + loaded.size());
        for (const int index : matched)
        {
            auto& tab = m_tabs[index];
            if (tab.projectPath != project.rootPath || tab.projectName != project.name)
            { tab.projectPath = project.rootPath; tab.projectName = project.name; tab.dirty = true; }
        }
        for (auto& tab : loaded) { tab.id = m_nextSessionId++; m_tabs.push_back(std::move(tab)); }
        m_activeTab = selected >= 0 ? selected : (loaded.empty() ? -1 : firstNew);
        m_currentProjectPath = project.rootPath; m_currentProjectName = project.name;
        m_projectExpanded = true; CloseDropdown(); RefreshProjectFiles(); RememberProject(project.rootPath);
        Redraw(); error.clear(); return true;
    }
    std::wstring AppController::GetDefaultItemLocation(ItemCategory category) const
    {
        if (category != ItemCategory::Projects)
        {
            WorkProjectStore::ProjectInfo project; std::wstring ignored;
            if (!m_currentProjectPath.empty() && WorkProjectStore::Open(m_currentProjectPath, project, ignored) && project.managed) return project.rootPath;
            for (const auto& recent : m_recentProjects)
                if (WorkProjectStore::Open(recent, project, ignored) && project.managed) return project.rootPath;
            return {};
        }
        if (!m_currentProjectPath.empty()) return std::filesystem::path(m_currentProjectPath).parent_path().wstring();
        PWSTR documents = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) return {};
        std::wstring location(documents); CoTaskMemFree(documents); return location;
    }
    void AppController::ShowNewItemWindow(ItemCategory initial)
    {
        if (!m_hwnd) return;
        NewItemDialogOptions options;
        options.initialCategory = initial; options.types = m_itemTypeRegistry.Types(); options.recentProjects = m_recentProjects;
        options.location = GetDefaultItemLocation(ItemCategory::Projects); options.projectLocation = GetDefaultItemLocation(ItemCategory::Sessions);
        NewItemDialogCallbacks callbacks;
        callbacks.create = [this](const NewItemRequest& request, std::wstring& error) { return CreateNewItem(request, error); };
        callbacks.openProject = [this](const std::wstring& path, std::wstring& error) { return OpenWorkProject(path, error); };
        ShowNewItemDialog(m_hwnd, options, callbacks);
    }
    void AppController::NewProject() { ShowNewItemWindow(ItemCategory::Projects); }
    bool AppController::CreateNewItem(const NewItemRequest& request, std::wstring& error)
    {
        const auto* type = m_itemTypeRegistry.Find(request.typeId);
        if (!type || type->category != request.category) { error = L"Choose a registered type from this category."; return false; }
        if (request.category == ItemCategory::Sessions && m_tabs.size() >= MaxSessions)
        { error = L"Close a session before creating another (100 open sessions maximum)."; return false; }
        SessionTab defaults;
        defaults.selectedModel = GetSelectedModel(); defaults.planBeforeEdits = GetPlanBeforeEdits(); defaults.autoRunSafeTools = GetAutoRunSafeTools();
        WorkProjectStore::CreatedItem created;
        if (!WorkProjectStore::Create(request, *type, created, error, &defaults)) return false;
        bool opened = true;
        if (request.category == ItemCategory::Projects) opened = OpenWorkProject(created.path, error);
        else if (request.category == ItemCategory::Sessions) opened = OpenSessionFromPath(created.path, error);
        else
        {
            WorkProjectStore::ProjectInfo project;
            opened = WorkProjectStore::Open(created.projectRoot, project, error);
            if (opened)
            {
                m_currentProjectPath = project.rootPath; m_currentProjectName = project.name;
                // Creating a file never creates a conversation or edits a draft.
                if (const auto* tab = ActiveSession(); tab && !SamePath(tab->projectPath, project.rootPath)) m_activeTab = -1;
                m_projectExpanded = true; RefreshProjectFiles(); RememberProject(project.rootPath); Redraw();
            }
        }
        if (!opened)
        {
            const auto failure = error; std::wstring rollbackError;
            if (!WorkProjectStore::RollbackCreated(created, rollbackError)) error = failure + L"\n" + rollbackError;
            return false;
        }
        if (request.category == ItemCategory::Sessions) RememberProject(created.projectRoot);
        error.clear(); return true;
    }
    bool AppController::RegisterItemType(const ItemType& type, std::wstring& error)
    {
        auto registered = m_itemTypeRegistry;
        if (!registered.RegisterType(type, error)) return false;
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
        { error = L"Windows could not locate the item-type settings folder."; return false; }
        const auto folder = std::filesystem::path(appData) / AppIdentity::SettingsFolder; CoTaskMemFree(appData);
        std::error_code filesystemError; std::filesystem::create_directories(folder, filesystemError);
        if (filesystemError) { error = L"The item-type settings folder could not be created."; return false; }
        if (!registered.Save((folder / L"item-types.registry").wstring(), error)) return false;
        m_itemTypeRegistry = std::move(registered); error.clear(); return true;
    }
    void AppController::RefreshProjectFiles()
    {
        m_sidebarScrollOffset = 0; m_sidebarMaxScroll = 0;
        m_files.clear();
        if (m_currentProjectPath.empty()) return;
        ScanDirectory(std::filesystem::path(m_currentProjectPath), 0, m_files);
    }
    void AppController::CloseProject()
    {
        if (m_currentProjectPath.empty()) return;
        if (auto* tab = ActiveSession(); tab && SamePath(tab->projectPath, m_currentProjectPath))
        {
            if (m_hwnd && MessageBoxW(m_hwnd, L"Close this session's project and clear its context paths? The conversation and draft will be kept.", L"Close Project", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            tab->projectPath.clear(); tab->projectName.clear(); tab->attachedPaths.clear(); tab->filesCount = 0; tab->dirty = true;
        }
        m_currentProjectPath.clear(); m_currentProjectName.clear(); m_projectExpanded = true;
        RefreshProjectFiles(); Redraw();
    }
    void AppController::RememberProject(const std::wstring& path)
    {
        m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(), [&path](const auto& existing) { return SamePath(existing, path); }), m_recentProjects.end());
        m_recentProjects.insert(m_recentProjects.begin(), path); if (m_recentProjects.size() > 10) m_recentProjects.resize(10); WriteRecentProjects();
    }
    void AppController::ReadRecentProjects()
    {
        const auto path = RecentFilePath(); if (path.empty()) return; std::ifstream file(std::filesystem::path(path), std::ios::binary);
        std::error_code error; if (std::filesystem::file_size(path, error) > 1024 * 1024 || error) return;
        std::string line;
        while (m_recentProjects.size() < 10 && std::getline(file, line))
        {
            if (line.empty() || line.size() > 131072) continue;
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0); if (!length) continue;
            std::wstring folder(length, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), folder.data(), length);
            if (folder.find(L'\0') == std::wstring::npos && std::filesystem::path(folder).is_absolute()) m_recentProjects.push_back(std::move(folder));
        }
    }
    void AppController::WriteRecentProjects()
    {
        if (!m_hwnd) return; // Headless controllers never write per-user preferences.
        const auto path = RecentFilePath(true); if (path.empty()) return; std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        for (const auto& folder : m_recentProjects)
        {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folder.data(), static_cast<int>(folder.size()), nullptr, 0, nullptr, nullptr); if (!size) continue;
            std::string text(size, '\0'); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folder.data(), static_cast<int>(folder.size()), text.data(), size, nullptr, nullptr); file << text << '\n';
        }
    }
    void AppController::ShowRecentProjects()
    {
        ShowNewItemWindow(ItemCategory::Projects);
    }
    void AppController::AttachFile()
    {
        std::vector<wchar_t> buffer(32768, 0); OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = m_hwnd; dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter = L"All Files (*.*)\0*.*\0"; dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) { if (CommDlgExtendedError()) DialogError(m_hwnd, L"Windows could not select these files. Try selecting fewer attachments.", L"Attach Files"); return; }
        std::vector<std::wstring> paths; const std::wstring first = buffer.data(); const wchar_t* next = buffer.data() + first.size() + 1;
        if (*next == L'\0') paths.push_back(first);
        else while (*next) { paths.push_back((std::filesystem::path(first) / next).wstring()); next += wcslen(next) + 1; } AttachFilePaths(paths);
    }
    void AppController::AttachFilePaths(const std::vector<std::wstring>& paths)
    {
        auto* tab = ActiveSession(); bool skipped = false;
        for (const auto& supplied : paths)
        {
            std::error_code error; const auto path = std::filesystem::weakly_canonical(supplied, error);
            if (error || !std::filesystem::is_regular_file(path, error) || error) { skipped = true; continue; }
            if (!tab) { NewSession(); tab = ActiveSession(); }
            if (!tab) { skipped = true; continue; }
            if (std::any_of(tab->attachedPaths.begin(), tab->attachedPaths.end(), [&path](const auto& existing) { return SamePath(existing, path.wstring()); })) continue;
            if (tab->attachedPaths.size() >= MaxAttachments) { skipped = true; continue; }
            tab->attachedPaths.push_back(path.wstring()); tab->filesCount = static_cast<int>(tab->attachedPaths.size()); tab->dirty = true;
            AppendDraftText((tab->draftText.empty() ? std::wstring{} : std::wstring(L" ")) + L"@" + path.filename().wstring());
        }
        if (skipped && m_hwnd) MessageBoxW(m_hwnd, L"Some files were skipped because they are missing, are folders, or the session has 1,000 attachments.", L"Attach Files", MB_OK | MB_ICONINFORMATION); Redraw();
    }
    void AppController::ShowContextFiles()
    {
        auto* tab = ActiveSession();
        if (!tab || tab->attachedPaths.empty()) { MessageBoxW(m_hwnd, L"No files are attached. Use the paperclip, drag files here, or select a file in the explorer.", L"Session Context", MB_OK | MB_ICONINFORMATION); return; }
        std::wstring text = L"Attached paths (contents are not uploaded):\n\n";
        for (std::size_t i = 0; i < tab->attachedPaths.size() && i < 20; ++i) text += tab->attachedPaths[i] + L"\n";
        if (tab->attachedPaths.size() > 20) text += L"\n...and " + std::to_wstring(tab->attachedPaths.size() - 20) + L" more.\n";
        text += L"\nClear all attached paths? Draft mentions will remain editable.";
        if (MessageBoxW(m_hwnd, text.c_str(), L"Session Context", MB_YESNO | MB_ICONQUESTION) == IDYES) { tab->attachedPaths.clear(); tab->filesCount = 0; tab->dirty = true; Redraw(); }
    }
    void AppController::ToggleVoice()
    {
        if (!m_hwnd || !ActiveSession()) return; HWND edit = GetDlgItem(m_hwnd, 201); if (!edit) return; SetFocus(edit);
        INPUT input[4]{}; for (auto& key : input) key.type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_LWIN; input[1].ki.wVk = 'H'; input[2].ki.wVk = 'H'; input[2].ki.dwFlags = KEYEVENTF_KEYUP; input[3].ki.wVk = VK_LWIN; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(4, input, sizeof(INPUT)) != 4)
        { SendInput(2, input + 2, sizeof(INPUT)); MessageBoxW(m_hwnd, L"Windows dictation could not be invoked. Focus the message field and press Win+H. Your draft was preserved.", L"Voice Input", MB_OK | MB_ICONINFORMATION); }
    }
    void AppController::SendCurrentMessage()
    {
        auto* tab = ActiveSession(); if (!tab || tab->draftText.empty() || IsGeneratingReply()) return;
        if (std::all_of(tab->draftText.begin(), tab->draftText.end(), [](wchar_t c) { return iswspace(c) != 0; })) return;
        if (tab->messages.size() >= 9998) { MessageBoxW(m_hwnd, L"The message limit was reached. Save this session and start a new one.", L"Send Message", MB_OK | MB_ICONINFORMATION); return; }
        const auto query = tab->draftText; tab->messages.push_back({L"user", L"You", Timestamp(), query, {}, {}}); tab->draftText.clear(); tab->dirty = true;
        tab->scrollToEnd = true;
        m_pendingReplies.push_back({tab->id, 6, query}); Redraw();
    }
    void AppController::CancelPendingReply()
    {
        const auto* tab = ActiveSession(); if (!tab) return; const int id = tab->id;
        m_pendingReplies.erase(std::remove_if(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; }), m_pendingReplies.end()); Redraw();
    }
    void AppController::GenerateReply(int sessionId, const std::wstring&)
    {
        auto found = std::find_if(m_tabs.begin(), m_tabs.end(), [sessionId](const auto& tab) { return tab.id == sessionId; });
        if (found == m_tabs.end()) return;
        found->scrollToEnd = found->scrollToEnd || found->scrollOffset >= found->maxScroll - 1;
        found->messages.push_back(LocalReply(*found)); found->dirty = true; Redraw();
    }
    void AppController::OnTimer()
    {
        for (auto it = m_pendingReplies.begin(); it != m_pendingReplies.end();)
        {
            if (--it->countdown <= 0) { const int id = it->sessionId; const auto query = it->query; it = m_pendingReplies.erase(it); GenerateReply(id, query); } else ++it;
        }
    }
    void AppController::AddCustomSkill()
    {
        std::vector<wchar_t> buffer(32768, 0); OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = m_hwnd; dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter = L"Markdown Instructions (*.md)\0*.md\0"; dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return; std::error_code error; const auto path = std::filesystem::weakly_canonical(buffer.data(), error);
        if (error || !std::filesystem::is_regular_file(path, error)) { DialogError(m_hwnd, L"The instruction file could not be read.", L"Add Custom Skill"); return; }
        for (const auto& skill : m_skills) if (SamePath(skill.fullPath, path.wstring())) return;
        if (m_skills.size() >= 100) { MessageBoxW(m_hwnd, L"The workspace already has 100 skills.", L"Add Custom Skill", MB_OK | MB_ICONINFORMATION); return; }
        m_skills.push_back({L"MD", path.stem().wstring(), L"Local instruction file; no execution", true, path.wstring()}); AttachFilePaths({path.wstring()}); Redraw();
    }
    void AppController::BrowsePlugins()
    { MessageBoxW(m_hwnd, L"A plugin loader and marketplace are not configured in this build; no plugins have been installed or executed.", L"Plugins", MB_OK | MB_ICONINFORMATION); }
}

