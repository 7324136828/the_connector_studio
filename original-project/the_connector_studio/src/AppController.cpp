#include "AppController.h"
#include <commdlg.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include "SessionPersistence.h"
#include "AppIdentity.h"
#include "WorkProjectStore.h"
#include "NewItemDialog.h"
#include "SettingsDialog.h"
#include "ChatCopy.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <thread>

namespace Lattice
{
    struct AppController::WorkerMailbox
    {
        struct Result
        {
            bool discovery = false;
            bool resources = false;
            bool mcpConnection = false;
            std::uint64_t requestId = 0;
            std::wstring projectPath;
            ProjectResources::Snapshot resourceSnapshot;
            Mcp::ConnectionResult mcpResult;
            Connector::DiscoveryResult models;
            Connector::CompletionResult completion;
        };
        std::mutex mutex;
        std::vector<Result> results;
    };
    AppController::AppController()
        : m_mailbox(std::make_shared<WorkerMailbox>())
    {
        InitializeMenus();
    }
    AppController::~AppController() { ShutdownConnector(); }

    void AppController::Initialize(HWND hwnd)
    {
        m_hwnd = hwnd;
        if (hwnd)
        {
            if (m_resourcePreferencesDirectory.empty()) m_resourcePreferencesDirectory = ProjectResources::DefaultPreferencesDirectory();
            if (m_connectorSettingsPath.empty()) m_connectorSettingsPath = Connector::DefaultSettingsPath();
            std::wstring ignoredSettingsError;
            Connector::LoadSettings(m_connectorSettingsPath, m_connectorSettings, ignoredSettingsError);
            ReadRecentProjects();
            PWSTR appData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
            {
                const auto path = std::filesystem::path(appData) / AppIdentity::SettingsFolder / L"item-types.registry";
                CoTaskMemFree(appData);
                std::wstring ignored;
                m_itemTypeRegistry.Load(path.wstring(), ignored);
            }
            RefreshConnectorModels();
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
            { L"Close Session", L"Ctrl+W", false },
            { L"Copy chat", L"Ctrl+Shift+C", true, false }
        };

        RebuildModelMenu();
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
    const std::wstring& AppController::GetActiveRequestError() const
    {
        static const std::wstring empty;
        const auto* tab = ActiveSession(); if (!tab) return empty;
        const auto found = m_requestErrors.find(tab->id);
        return found == m_requestErrors.end() ? empty : found->second;
    }
    void AppController::ShowSettings()
    {
        if (!m_hwnd) return;
        ShowSettingsDialog(m_hwnd, m_connectorSettings,
            [this](const Connector::Settings& settings, std::wstring& error) { return ApplyConnectorSettings(settings, error); });
    }
    bool AppController::ApplyConnectorSettings(const Connector::Settings& settings, std::wstring& error)
    {
        Connector::Settings normalized;
        normalized.chatFontSize = settings.chatFontSize;
        if (!Connector::NormalizeServerUrl(settings.serverUrl, normalized.serverUrl, error)) return false;
        auto settingsPath = m_connectorSettingsPath;
        if (settingsPath.empty() && m_hwnd) settingsPath = Connector::DefaultSettingsPath();
        if (settingsPath.empty()) { error = L"Choose an isolated settings path before changing a headless controller's server."; return false; }
        if (!Connector::SaveSettings(settingsPath, normalized, error)) return false;
        if (normalized.serverUrl != m_connectorSettings.serverUrl)
        {
            CancelAllReplies(true);
            // A model ID belongs to the server that advertised it.
            m_connectorModels.clear();
        }
        const bool refreshModels = normalized.serverUrl != m_connectorSettings.serverUrl || normalized.chatFontSize == m_connectorSettings.chatFontSize ||
            m_modelDiscoveryState == ModelDiscoveryState::NotLoaded || m_connectorShutdown;
        m_connectorSettingsPath = std::move(settingsPath); m_connectorSettings = std::move(normalized);
        m_connectorShutdown = false;
        if (refreshModels) RefreshConnectorModels();
        error.clear(); Redraw(); return true;
    }
    void AppController::RefreshConnectorModels()
    {
        if (m_connectorShutdown) return;
        if (m_discoveryCancellation) m_discoveryCancellation->Cancel();
        m_discoveryCancellation = std::make_shared<Connector::Cancellation>();
        m_discoveryRequestId = m_nextRequestId++;
        const auto settings = m_connectorSettings; const auto mailbox = m_mailbox;
        const auto cancellation = m_discoveryCancellation; const auto requestId = m_discoveryRequestId;
        m_connectorConnected = false; m_connectionStatus = L"Connecting to Connector...";
        m_modelDiscoveryState = ModelDiscoveryState::Loading; RebuildModelMenu();
        try
        {
            std::thread([settings, mailbox, cancellation, requestId]
            {
                WorkerMailbox::Result result; result.discovery = true; result.requestId = requestId;
                try { result.models = Connector::DiscoverModels(settings, cancellation); }
                catch (...) { result.models.error = L"Model discovery could not finish."; }
                std::lock_guard<std::mutex> guard(mailbox->mutex); mailbox->results.push_back(std::move(result));
            }).detach();
        }
        catch (...)
        {
            m_discoveryCancellation.reset(); m_connectionStatus = L"Unable to start model discovery.";
            m_modelDiscoveryState = ModelDiscoveryState::Failed; RebuildModelMenu();
        }
        Redraw();
    }
    void AppController::RebuildModelMenu()
    {
        m_modelMenuItems.clear(); m_modelMenuActions.clear();
        const auto add = [this](MenuItem item, ModelMenuAction action)
        { m_modelMenuItems.push_back(std::move(item)); m_modelMenuActions.push_back(action); };
        for (const auto& model : m_connectorModels)
            add({model.id, model.name == model.id ? std::wstring{} : model.name, false, m_connectorConnected}, ModelMenuAction::Select);
        if (m_modelDiscoveryState == ModelDiscoveryState::Loading)
            add({m_connectorModels.empty() ? L"Loading Connector models..." : L"Refreshing Connector models...", {}, false, false}, ModelMenuAction::None);
        else if (m_modelDiscoveryState == ModelDiscoveryState::Failed)
            add({L"Unable to load models", m_connectionStatus, false, false}, ModelMenuAction::None);
        else if (m_connectorModels.empty())
            add({m_modelDiscoveryState == ModelDiscoveryState::Ready ? L"No active models on Connector" : L"Models have not been loaded", {}, false, false}, ModelMenuAction::None);
        add({L"Refresh models", {}, true, m_modelDiscoveryState != ModelDiscoveryState::Loading}, ModelMenuAction::Refresh);
        add({L"Connection settings", {}, false}, ModelMenuAction::Settings);
    }
    void AppController::SetActiveDropdown(ActiveDropdown dropdown)
    {
        m_activeDropdown = dropdown;
        if ((dropdown == ActiveDropdown::Model || dropdown == ActiveDropdown::SidebarModel)
            && !m_connectorConnected && !m_discoveryCancellation && (m_hwnd || !m_connectorSettingsPath.empty()))
            RefreshConnectorModels();
    }
    void AppController::RestoreSubmittedDraft(const PendingReply& pending)
    {
        auto found = std::find_if(m_tabs.begin(), m_tabs.end(), [&pending](const auto& tab) { return tab.id == pending.sessionId; });
        if (found == m_tabs.end()) return;
        if (pending.messageIndex < found->messages.size() && found->messages[pending.messageIndex].role == L"user"
            && found->messages[pending.messageIndex].text == pending.query)
            found->messages.erase(found->messages.begin() + static_cast<std::ptrdiff_t>(pending.messageIndex));
        found->draftText = found->draftText.empty() ? pending.query : pending.query + L"\r\n\r\n" + found->draftText;
        found->dirty = true;
    }
    void AppController::CancelAllReplies(bool restoreDrafts)
    {
        for (const auto& pending : m_pendingReplies)
        { pending.cancellation->Cancel(); if (restoreDrafts) RestoreSubmittedDraft(pending); }
        m_pendingReplies.clear();
    }
    void AppController::ShutdownConnector()
    {
        m_connectorShutdown = true;
        if (m_discoveryCancellation) m_discoveryCancellation->Cancel();
        m_discoveryCancellation.reset(); CancelAllReplies(false);
        if (m_resourceCancellation) m_resourceCancellation->Cancel();
        m_resourceCancellation.reset();
        CancelMcpConnections();
        // Workers own only the shared mailbox and immutable request snapshots.
        // Cancellation lets transport finish without waiting on the window thread.
    }
    const std::vector<MenuItem>& AppController::GetCurrentDropdownItems() const
    {
        if (m_activeDropdown == ActiveDropdown::Project) return m_projectMenuItems;
        if (m_activeDropdown == ActiveDropdown::Session)
        {
            const auto* session = ActiveSession();
            m_sessionMenuItems.back().enabled = session && std::any_of(session->messages.begin(), session->messages.end(), ChatCopy::HasContent);
            return m_sessionMenuItems;
        }
        if (m_activeDropdown == ActiveDropdown::Model || m_activeDropdown == ActiveDropdown::SidebarModel)
        {
            for (std::size_t index = 0; index < m_modelMenuItems.size(); ++index)
                m_modelMenuItems[index].selected = m_modelMenuActions[index] == ModelMenuAction::Select
                    && m_modelMenuItems[index].label == GetSelectedModel();
            return m_modelMenuItems;
        }
        static const std::vector<MenuItem> empty; return empty;
    }
    void AppController::HandleClick(const HitTestResult& target)
    {
        if (target.type == HitTargetType::DropdownItem) { HandleDropdownSelect(target.index); return; }
        const auto previous = m_activeDropdown; CloseDropdown();
        auto toggle = [this, previous](ActiveDropdown value) { SetActiveDropdown(previous == value ? ActiveDropdown::None : value); };
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
            ShowSettings(); break;
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
        case HitTargetType::SidebarAgentSelect:
        case HitTargetType::SidebarToggleSafeTools:
        case HitTargetType::SidebarAddSkill: break; // Removed controls remain inert for old hit targets.
        case HitTargetType::SidebarSkillItem: ToggleSkill(target.index); break;
        case HitTargetType::SidebarMcpServerItem: ToggleMcpServer(target.index); break;
        case HitTargetType::SidebarAgentMoveUp: MoveAgentPriority(target.index, -1); break;
        case HitTargetType::SidebarAgentMoveDown: MoveAgentPriority(target.index, 1); break;
        case HitTargetType::SidebarAgentItem: ToggleAgent(target.index); break;
        case HitTargetType::SidebarPluginCard:
        case HitTargetType::SidebarBrowsePlugins: BrowsePlugins(); break;
        case HitTargetType::ComposerAttach: AttachFile(); break;
        case HitTargetType::ComposerContextChip: ShowContextFiles(); break;
        case HitTargetType::ComposerMic: ToggleVoice(); break;
        case HitTargetType::ComposerSend: if (IsGeneratingReply()) CancelPendingReply(); else SendCurrentMessage(); break;
        default: break;
        } Redraw();
    }
    void AppController::HandleDropdownSelect(int index)
    {
        const auto dropdown = m_activeDropdown;
        if (index < 0 || index >= static_cast<int>(GetCurrentDropdownItems().size())) return;
        if (!GetCurrentDropdownItems()[index].enabled) return;
        if (dropdown == ActiveDropdown::Model || dropdown == ActiveDropdown::SidebarModel)
        {
            const auto action = m_modelMenuActions[index];
            if (action == ModelMenuAction::Refresh) { RefreshConnectorModels(); return; }
            if (action == ModelMenuAction::Settings) { CloseDropdown(); ShowSettings(); Redraw(); return; }
            if (action != ModelMenuAction::Select || !m_connectorConnected) return;
            const auto model = m_modelMenuItems[index].label;
            if (auto* tab = ActiveSession()) { tab->selectedModel = model; tab->dirty = true; }
            else m_defaultSelectedModel = model;
            CloseDropdown(); Redraw(); return;
        }
        CloseDropdown();
        if (dropdown == ActiveDropdown::Project) { if (index == 0) NewProject(); else if (index == 1) CloseProject(); else if (index == 2) ShowRecentProjects(); }
        else if (dropdown == ActiveDropdown::Session) { if (index == 0) NewSession(); else if (index == 1) OpenSession(); else if (index == 2) SaveSession(); else if (index == 3) CloseSession(m_activeTab); }
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
    {
        struct ReviewedSession
        {
            std::size_t messageCount;
            std::wstring draft, model;
        };
        std::map<int, ReviewedSession> reviewed;
        const auto matches = [&reviewed](const SessionTab& tab)
        {
            const auto found = reviewed.find(tab.id);
            return found != reviewed.end() && found->second.messageCount == tab.messages.size()
                && found->second.draft == tab.draftText && found->second.model == tab.selectedModel;
        };
        for (;;)
        {
            OnTimer();
            for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
            {
                if (matches(m_tabs[i])) continue;
                if (!ConfirmClose(i)) return false;
                const auto& tab = m_tabs[i];
                reviewed[tab.id] = {tab.messages.size(), tab.draftText, tab.selectedModel};
            }
            // A later modal prompt pumps WM_TIMER. Review an earlier session again
            // if its response arrived after that session's save/discard decision.
            OnTimer();
            if (std::all_of(m_tabs.begin(), m_tabs.end(), matches))
                return true; // WM_QUERYENDSESSION can still be cancelled by Windows.
        }
    }
    void AppController::CloseSession(int index)
    {
        OnTimer();
        if (!ConfirmClose(index)) return;
        const int id = m_tabs[index].id;
        for (const auto& pending : m_pendingReplies) if (pending.sessionId == id) pending.cancellation->Cancel();
        m_pendingReplies.erase(std::remove_if(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; }), m_pendingReplies.end());
        m_requestErrors.erase(id);
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
        OnTimer(); // Include already-received results without waiting for the network.
        if (index < 0 || index >= static_cast<int>(m_tabs.size())) { error = L"Select a session first."; return false; }
        SessionTab snapshot = m_tabs[index];
        if (!SessionPersistence::Save(path, snapshot, error)) return false;
        snapshot.savedPath = path; snapshot.dirty = false; m_tabs[index] = std::move(snapshot);
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
        MaybeRefreshProjectResources(false);
        if (m_currentProjectPath.empty()) return;
        ScanDirectory(std::filesystem::path(m_currentProjectPath), 0, m_files);
    }
    void AppController::RefreshProjectResources() { MaybeRefreshProjectResources(true); }
    void AppController::MaybeRefreshProjectResources(bool force)
    {
        const auto now = GetTickCount64();
        const bool changedProject = !SamePath(m_resourceProjectPath, m_currentProjectPath);
        if (changedProject)
        {
            CancelMcpConnections();
            if (m_resourceCancellation) m_resourceCancellation->Cancel();
            m_resourceCancellation.reset(); ++m_resourceRequestId;
            m_resourceProjectPath = m_currentProjectPath;
            m_skills.clear(); m_mcpServers.clear(); m_agents.clear(); m_resourcePreferencesError.clear();
            if (!m_resourceProjectPath.empty() && m_resourcePreferences.find(m_resourceProjectPath) == m_resourcePreferences.end())
            {
                ProjectResources::Preferences preferences;
                ProjectResources::LoadPreferences(m_resourcePreferencesDirectory, m_resourceProjectPath, preferences, m_resourcePreferencesError);
                m_resourcePreferences.emplace(m_resourceProjectPath, std::move(preferences));
            }
        }
        if (m_currentProjectPath.empty() || m_connectorShutdown) return;
        if (!changedProject && !force && (m_resourceCancellation || now - m_lastResourceRefresh < 3000)) return;
        if (m_resourceCancellation) m_resourceCancellation->Cancel();
        m_resourceCancellation.reset(); m_lastResourceRefresh = now;
        if (!m_hwnd)
        {
            ApplyResourceSnapshot(ProjectResources::Discover(m_currentProjectPath));
            return;
        }
        const auto projectPath = m_currentProjectPath;
        const auto requestId = ++m_resourceRequestId;
        const auto mailbox = m_mailbox;
        const auto cancellation = std::make_shared<Connector::Cancellation>(); m_resourceCancellation = cancellation;
        try
        {
            std::thread([projectPath, requestId, mailbox, cancellation]
            {
                WorkerMailbox::Result result; result.resources = true; result.requestId = requestId; result.projectPath = projectPath;
                try { result.resourceSnapshot = ProjectResources::Discover(projectPath); }
                catch (...) { result.resourceSnapshot.projectPath = projectPath; }
                if (cancellation->IsCancelled()) return;
                std::lock_guard<std::mutex> guard(mailbox->mutex); mailbox->results.push_back(std::move(result));
            }).detach();
        }
        catch (...) { m_resourceCancellation.reset(); m_resourcePreferencesError = L"Project resources could not be refreshed."; }
    }
    void AppController::ApplyResourceSnapshot(ProjectResources::Snapshot snapshot)
    {
        const auto preferences = m_resourcePreferences.find(m_resourceProjectPath);
        if (preferences != m_resourcePreferences.end()) ProjectResources::ApplyPreferences(snapshot, preferences->second);
        if (preferences != m_resourcePreferences.end())
        {
            // Remember discovered priority in memory without writing on scans.
            // Newly added agents consequently append even before a manual reorder.
            preferences->second.agents.clear();
            for (const auto& agent : snapshot.agents) preferences->second.agents.push_back(ProjectResources::Key(snapshot.projectPath, agent.fullPath));
        }
        m_skills = std::move(snapshot.skills); m_mcpServers = std::move(snapshot.mcpServers); m_agents = std::move(snapshot.agents);
        // Deleting a server folder cancels its in-flight connection and forgets
        // its transient status. Stable folders retain their status across scans.
        for (auto pending = m_pendingMcpConnections.begin(); pending != m_pendingMcpConnections.end();)
        {
            const bool exists = std::any_of(m_mcpServers.begin(), m_mcpServers.end(), [&pending](const auto& server)
                { return SamePath(server.fullPath, pending->resourcePath) && server.serverKey == pending->serverKey; });
            if (!exists) { pending->cancellation->Cancel(); pending = m_pendingMcpConnections.erase(pending); }
            else ++pending;
        }
        for (auto state = m_mcpStates.begin(); state != m_mcpStates.end();)
        {
            const bool exists = std::any_of(m_mcpServers.begin(), m_mcpServers.end(), [this, &state](const auto& server)
                { return SamePath(ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey), state->first); });
            if (!exists) state = m_mcpStates.erase(state); else ++state;
        }
        for (int index = 0; index < static_cast<int>(m_mcpServers.size()); ++index)
        {
            auto& server = m_mcpServers[index];
            const auto state = m_mcpStates.find(ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey));
            if (state != m_mcpStates.end())
            {
                server.connectionState = state->second.state; server.connectionError = state->second.error;
                // A pending enable has not been persisted yet; preserve it while
                // the asynchronous handshake runs through a filesystem refresh.
                if (state->second.state == McpConnectionState::Connecting || state->second.state == McpConnectionState::Connected) server.isEnabled = true;
            }
            if (server.isEnabled && (state == m_mcpStates.end() || !state->second.attempted)) StartMcpConnection(index);
        }
    }
    bool AppController::SaveResourceChoices(const std::vector<SkillItem>& skills, const std::vector<McpServerItem>& servers, const std::vector<AgentItem>& agents)
    {
        if (m_resourceProjectPath.empty()) return false;
        ProjectResources::Preferences preferences;
        for (const auto& skill : skills) preferences.skills.emplace(ProjectResources::Key(m_resourceProjectPath, skill.fullPath), skill.isEnabled);
        for (const auto& server : servers) preferences.mcpServers.emplace(ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey),
            server.isEnabled && server.connectionState == McpConnectionState::Connected);
        for (const auto& agent : agents)
        {
            const auto key = ProjectResources::Key(m_resourceProjectPath, agent.fullPath);
            preferences.agents.push_back(key); preferences.agentEnabled.emplace(key, agent.isEnabled);
        }
        if ((m_hwnd || !m_resourcePreferencesDirectory.empty())
            && !ProjectResources::SavePreferences(m_resourcePreferencesDirectory, m_resourceProjectPath, preferences, m_resourcePreferencesError))
        { Redraw(); return false; }
        m_resourcePreferences[m_resourceProjectPath] = std::move(preferences);
        m_resourcePreferencesError.clear(); return true;
    }
    bool AppController::ToggleSkill(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_skills.size())) return false;
        auto skills = m_skills; skills[index].isEnabled = !skills[index].isEnabled;
        if (!SaveResourceChoices(skills, m_mcpServers, m_agents)) return false;
        m_skills = std::move(skills); Redraw(); return true;
    }
    bool AppController::ToggleAgent(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_agents.size())) return false;
        auto agents = m_agents; agents[index].isEnabled = !agents[index].isEnabled;
        if (!SaveResourceChoices(m_skills, m_mcpServers, agents)) return false;
        m_agents = std::move(agents); Redraw(); return true;
    }
    bool AppController::ToggleMcpServer(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_mcpServers.size())) return false;
        auto& server = m_mcpServers[index];
        if (!server.isEnabled)
        {
            server.isEnabled = true; StartMcpConnection(index); Redraw(); return true;
        }
        for (auto pending = m_pendingMcpConnections.begin(); pending != m_pendingMcpConnections.end();)
        {
            if (SamePath(pending->resourcePath, server.fullPath) && pending->serverKey == server.serverKey) { pending->cancellation->Cancel(); pending = m_pendingMcpConnections.erase(pending); }
            else ++pending;
        }
        server.isEnabled = false; server.connectionState = McpConnectionState::Disabled; server.connectionError.clear();
        const auto identity = ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey);
        m_mcpStates[identity] = {McpConnectionState::Disabled, {}, true};
        const bool saved = SaveResourceChoices(m_skills, m_mcpServers, m_agents);
        // A save failure must not leave the runtime enabled or trigger retries.
        m_resourcePreferences[m_resourceProjectPath].mcpServers[identity] = false;
        Redraw(); return saved;
    }
    void AppController::CancelMcpConnections()
    {
        for (const auto& pending : m_pendingMcpConnections) pending.cancellation->Cancel();
        m_pendingMcpConnections.clear();
        m_mcpStates.clear(); // Release connected transports and owned stdio jobs.
    }
    void AppController::StartMcpConnection(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_mcpServers.size()) || m_connectorShutdown) return;
        auto& server = m_mcpServers[index];
        server.connectionState = McpConnectionState::Connecting; server.connectionError.clear();
        const auto identity = ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey);
        m_mcpStates[identity] = {McpConnectionState::Connecting, {}, true};
        const auto requestId = m_nextRequestId++;
        const auto cancellation = std::make_shared<Connector::Cancellation>();
        const auto resourcePath = server.fullPath, projectPath = m_resourceProjectPath, serverKey = server.serverKey;
        m_pendingMcpConnections.push_back({projectPath, resourcePath, serverKey, requestId, cancellation});
        const auto mailbox = m_mailbox;
        try
        {
            std::thread([resourcePath, serverKey, requestId, mailbox, cancellation]
            {
                WorkerMailbox::Result result; result.mcpConnection = true; result.requestId = requestId;
                try { result.mcpResult = Mcp::Connect(resourcePath, cancellation, serverKey); }
                catch (...) { result.mcpResult.error = L"The MCP server connection could not finish."; }
                std::lock_guard<std::mutex> guard(mailbox->mutex); mailbox->results.push_back(std::move(result));
            }).detach();
        }
        catch (...)
        {
            m_pendingMcpConnections.pop_back(); server.isEnabled = false; server.connectionState = McpConnectionState::Disabled;
            server.connectionError = L"The MCP connection could not start.";
            m_mcpStates[identity] = {server.connectionState, server.connectionError, true};
            SaveResourceChoices(m_skills, m_mcpServers, m_agents);
            m_resourcePreferences[m_resourceProjectPath].mcpServers[identity] = false;
        }
    }
    bool AppController::MoveAgentPriority(int index, int delta)
    {
        if (index < 0 || index >= static_cast<int>(m_agents.size()) || (delta != -1 && delta != 1)) return false;
        const int destination = index + delta;
        if (destination < 0 || destination >= static_cast<int>(m_agents.size())) return false;
        auto agents = m_agents; std::swap(agents[index], agents[destination]);
        for (std::size_t position = 0; position < agents.size(); ++position) agents[position].priority = static_cast<int>(position + 1);
        if (!SaveResourceChoices(m_skills, m_mcpServers, agents)) return false;
        m_agents = std::move(agents); Redraw(); return true;
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
        if (m_connectorShutdown || !m_connectorConnected)
        { m_requestErrors[tab->id] = L"Connector is not connected. Use Refresh models in the model menu, or open Connection settings to check the server URL."; Redraw(); return; }
        if (tab->selectedModel.empty() || std::none_of(m_connectorModels.begin(), m_connectorModels.end(), [tab](const auto& model) { return model.id == tab->selectedModel; }))
        { m_requestErrors[tab->id] = L"Select an active Connector model. Use Refresh models in the model menu to reload the list."; Redraw(); return; }
        if (tab->messages.size() >= 9998)
        { m_requestErrors[tab->id] = L"The message limit was reached. Save this session and start a new one."; Redraw(); return; }
        std::vector<Connector::ChatMessage> history;
        for (const auto& message : tab->messages)
        {
            // Older local-only builds stored an offline acknowledgement as an assistant
            // message. Keep it in saved transcripts, but never treat it as model context.
            if ((message.author == AppIdentity::Name || message.author == L"Lattice Studio")
                && message.text.find(L"Added to this local conversation. No AI provider is connected") == 0) continue;
            std::wstring content = message.text;
            for (const auto& item : message.items) content += L"\n" + item;
            for (const auto& step : message.planSteps) content += L"\n" + step.title + L"\n" + step.description;
            history.push_back({message.role, std::move(content)});
        }
        history.push_back({L"user", tab->draftText});
        const auto query = tab->draftText; tab->messages.push_back({L"user", L"You", Timestamp(), query, {}, {}}); tab->draftText.clear(); tab->dirty = true;
        tab->scrollToEnd = true;
        const auto cancellation = std::make_shared<Connector::Cancellation>(); const auto requestId = m_nextRequestId++;
        m_pendingReplies.push_back({tab->id, requestId, tab->messages.size() - 1, query, tab->selectedModel, cancellation});
        m_requestErrors.erase(tab->id);
        const auto settings = m_connectorSettings; const auto model = tab->selectedModel; const auto mailbox = m_mailbox;
        try
        {
            std::thread([settings, model, history = std::move(history), mailbox, cancellation, requestId]
            {
                WorkerMailbox::Result result; result.requestId = requestId;
                try { result.completion = Connector::Complete(settings, model, history, cancellation); }
                catch (...) { result.completion.error = L"The request could not finish."; }
                std::lock_guard<std::mutex> guard(mailbox->mutex); mailbox->results.push_back(std::move(result));
            }).detach();
        }
        catch (...)
        {
            RestoreSubmittedDraft(m_pendingReplies.back()); m_pendingReplies.pop_back();
            m_requestErrors[tab->id] = L"The request could not be started. Your draft was preserved.";
        }
        Redraw();
    }
    void AppController::CancelPendingReply()
    {
        const auto* tab = ActiveSession(); if (!tab) return; const int id = tab->id;
        for (const auto& pending : m_pendingReplies)
            if (pending.sessionId == id) { pending.cancellation->Cancel(); RestoreSubmittedDraft(pending); }
        m_pendingReplies.erase(std::remove_if(m_pendingReplies.begin(), m_pendingReplies.end(), [id](const auto& reply) { return reply.sessionId == id; }), m_pendingReplies.end()); Redraw();
        m_requestErrors.erase(id);
    }
    void AppController::OnTimer()
    {
        MaybeRefreshProjectResources(false);
        for (auto& server : m_mcpServers)
        {
            const auto identity = ProjectResources::McpKey(m_resourceProjectPath, server.fullPath, server.serverKey);
            const auto state = m_mcpStates.find(identity);
            if (!server.isEnabled || server.connectionState != McpConnectionState::Connected || state == m_mcpStates.end()
                || !state->second.connection || state->second.connection->IsAlive()) continue;
            server.isEnabled = false; server.connectionState = McpConnectionState::Disabled;
            server.connectionError = L"The MCP server disconnected. Enable it to try again.";
            m_mcpStates[identity] = {server.connectionState, server.connectionError, true};
            SaveResourceChoices(m_skills, m_mcpServers, m_agents);
            m_resourcePreferences[m_resourceProjectPath].mcpServers[identity] = false;
            Redraw();
        }
        std::vector<WorkerMailbox::Result> results;
        { std::lock_guard<std::mutex> guard(m_mailbox->mutex); results.swap(m_mailbox->results); }
        for (auto& result : results)
        {
            if (result.mcpConnection)
            {
                const auto pending = std::find_if(m_pendingMcpConnections.begin(), m_pendingMcpConnections.end(), [&result](const auto& connection) { return connection.requestId == result.requestId; });
                if (pending == m_pendingMcpConnections.end()) continue;
                const auto submitted = *pending; m_pendingMcpConnections.erase(pending);
                if (submitted.cancellation->IsCancelled() || !SamePath(submitted.projectPath, m_currentProjectPath)) continue;
                const auto server = std::find_if(m_mcpServers.begin(), m_mcpServers.end(), [&submitted](const auto& item)
                    { return SamePath(item.fullPath, submitted.resourcePath) && item.serverKey == submitted.serverKey; });
                if (server == m_mcpServers.end() || !server->isEnabled) continue;
                server->isEnabled = result.mcpResult.success && !result.mcpResult.cancelled;
                server->connectionState = server->isEnabled ? McpConnectionState::Connected : McpConnectionState::Disabled;
                server->connectionError = server->isEnabled ? std::wstring{} :
                    (result.mcpResult.error.empty() ? L"The connection failed. Enable this server to try again." : result.mcpResult.error);
                if (!SaveResourceChoices(m_skills, m_mcpServers, m_agents))
                {
                    server->isEnabled = false; server->connectionState = McpConnectionState::Disabled;
                    if (server->connectionError.empty()) server->connectionError = L"The enabled preference could not be saved.";
                }
                if (!server->isEnabled)
                    m_resourcePreferences[m_resourceProjectPath].mcpServers[ProjectResources::McpKey(m_resourceProjectPath, server->fullPath, server->serverKey)] = false;
                m_mcpStates[ProjectResources::McpKey(m_resourceProjectPath, server->fullPath, server->serverKey)] = {server->connectionState, server->connectionError, true,
                    server->isEnabled ? std::move(result.mcpResult.connection) : std::shared_ptr<Mcp::Connection>{}};
                Redraw(); continue;
            }
            if (result.resources)
            {
                if (result.requestId != m_resourceRequestId || !m_resourceCancellation || m_resourceCancellation->IsCancelled()
                    || !SamePath(result.projectPath, m_currentProjectPath)) continue;
                m_resourceCancellation.reset();
                ApplyResourceSnapshot(std::move(result.resourceSnapshot)); Redraw(); continue;
            }
            if (result.discovery)
            {
                if (result.requestId != m_discoveryRequestId || !m_discoveryCancellation || m_discoveryCancellation->IsCancelled()) continue;
                m_discoveryCancellation.reset();
                if (!result.models.success)
                {
                    m_connectorConnected = false; m_connectionStatus = result.models.error.empty() ? L"Connector connection failed." : result.models.error;
                    m_modelDiscoveryState = ModelDiscoveryState::Failed;
                }
                else
                {
                    m_connectorConnected = true; m_connectorModels = std::move(result.models.models);
                    m_modelDiscoveryState = ModelDiscoveryState::Ready;
                    m_connectionStatus = m_connectorModels.empty() ? L"Connected - no active configurations" : L"Connected to Connector";
                    if (std::none_of(m_connectorModels.begin(), m_connectorModels.end(), [this](const auto& model) { return model.id == m_defaultSelectedModel; }))
                        m_defaultSelectedModel = m_connectorModels.empty() ? std::wstring{} : m_connectorModels.front().id;
                    for (auto& tab : m_tabs)
                        if (tab.selectedModel.empty()) { tab.selectedModel = m_defaultSelectedModel; if (!tab.savedPath.empty()) tab.dirty = true; }
                }
                RebuildModelMenu();
                Redraw(); continue;
            }
            const auto pending = std::find_if(m_pendingReplies.begin(), m_pendingReplies.end(), [&result](const auto& reply) { return reply.requestId == result.requestId; });
            if (pending == m_pendingReplies.end()) continue;
            const auto submitted = *pending; m_pendingReplies.erase(pending);
            const auto tab = std::find_if(m_tabs.begin(), m_tabs.end(), [&submitted](const auto& session) { return session.id == submitted.sessionId; });
            if (tab == m_tabs.end()) continue;
            if (!result.completion.success || result.completion.cancelled)
            {
                RestoreSubmittedDraft(submitted);
                m_requestErrors[submitted.sessionId] = result.completion.cancelled ? L"Request cancelled." :
                    (result.completion.error.empty() ? L"Connector request failed. Your draft was restored." : result.completion.error);
            }
            else
            {
                tab->scrollToEnd = tab->scrollToEnd || tab->scrollOffset >= tab->maxScroll - 1;
                tab->messages.push_back({L"assistant", submitted.model, Timestamp(), std::move(result.completion.text), {}, {}});
                tab->dirty = true; m_requestErrors.erase(submitted.sessionId);
            }
            Redraw();
        }
    }
    void AppController::BrowsePlugins()
    { MessageBoxW(m_hwnd, L"A plugin loader and marketplace are not configured in this build; no plugins have been installed or executed.", L"Plugins", MB_OK | MB_ICONINFORMATION); }
}

