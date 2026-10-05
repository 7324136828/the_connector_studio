#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "AppTypes.h"
#include "UIComponents.h"
#include "ItemTypeRegistry.h"
#include "ConnectorClient.h"
#include "ProjectResources.h"
#include "McpClient.h"
#include <cstdint>
#include <map>

namespace Lattice
{
    class AppController
    {
    public:
        AppController();
        ~AppController();
        AppController(const AppController&) = delete;
        AppController& operator=(const AppController&) = delete;
        void Initialize(HWND hwnd);
        void ShowSettings();
        bool ApplyConnectorSettings(const Connector::Settings& settings, std::wstring& error);
        const Connector::Settings& GetConnectorSettings() const { return m_connectorSettings; }
        float GetChatFontSize() const { return m_connectorSettings.chatFontSize; }
        const std::wstring& GetConnectionStatusLabel() const { return m_connectionStatus; }
        bool IsConnectorConnected() const { return m_connectorConnected; }
        bool IsDiscoveringModels() const { return m_discoveryCancellation != nullptr; }
        const std::wstring& GetActiveRequestError() const;
        void ConfigureSettingsPathForTesting(const std::wstring& path) { m_connectorSettingsPath = path; }
        void RefreshConnectorModels();
        void ShutdownConnector();
        SidebarMode GetSidebarMode() const { return m_sidebarMode; }
        ActiveDropdown GetActiveDropdown() const { return m_activeDropdown; }
        int GetActiveTab() const { return m_activeTab; }
        const std::vector<SessionTab>& GetTabs() const { return m_tabs; }
        const std::vector<FileItem>& GetFiles() const { return m_files; }
        const std::vector<SkillItem>& GetSkills() const { return m_skills; }
        const std::vector<McpServerItem>& GetMcpServers() const { return m_mcpServers; }
        const std::vector<AgentItem>& GetAgents() const { return m_agents; }
        const std::wstring& GetResourcePreferencesError() const { return m_resourcePreferencesError; }
        void ConfigureResourcePreferencesPathForTesting(const std::wstring& directory) { m_resourcePreferencesDirectory = directory; }
        void RefreshProjectResources();
        bool ToggleSkill(int index);
        bool ToggleAgent(int index);
        bool ToggleMcpServer(int index);
        bool MoveAgentPriority(int index, int delta);
        const std::vector<PluginItem>& GetPlugins() const { return m_plugins; }
        const std::vector<MenuItem>& GetCurrentDropdownItems() const;
        const std::wstring& GetSelectedModel() const;
        bool GetPlanBeforeEdits() const;
        bool GetAutoRunSafeTools() const;
        bool GetIsListening() const { return false; } // Windows owns dictation status.
        const std::wstring& GetDraftText() const;
        float GetSidebarScrollOffset() const { return m_sidebarScrollOffset; }
        bool GetProjectExpanded() const { return m_projectExpanded; }
        bool IsGeneratingReply() const;
        void SetDraftText(const std::wstring& text);
        void AppendDraftText(const std::wstring& text);
        void HandleClick(const HitTestResult& target);
        void HandleDropdownSelect(int index);
        void ScrollActiveTab(float delta);
        void SetActiveScrollMetrics(float maxScroll);
        void SetActiveScrollOffset(float offset);
        void ScrollSidebar(float delta);
        void SetSidebarScrollMetrics(float maxScroll);
        void SetSidebarScrollOffset(float offset);
        void SendCurrentMessage();
        void CancelPendingReply();
        void ToggleVoice();
        void AttachFile();
        void AttachFilePaths(const std::vector<std::wstring>& paths);
        void NewSession();
        void CloseSession(int index);
        void SelectSession(int index);
        void SaveSession();
        void OpenSession();
        bool SaveSessionToPath(int index, const std::wstring& path, std::wstring& error);
        bool OpenSessionFromPath(const std::wstring& path, std::wstring& error);
        bool CanCloseApplication();
        void NewProject();
        void ShowNewItemWindow(ItemCategory initial);
        bool CreateNewItem(const NewItemRequest& request, std::wstring& error);
        bool OpenWorkProject(const std::wstring& path, std::wstring& error);
        ItemTypeRegistry& GetItemTypeRegistry() { return m_itemTypeRegistry; }
        const ItemTypeRegistry& GetItemTypeRegistry() const { return m_itemTypeRegistry; }
        bool RegisterItemType(const ItemType& type, std::wstring& error);
        const std::vector<std::wstring>& GetRecentProjects() const { return m_recentProjects; }
        std::wstring GetDefaultItemLocation(ItemCategory category) const;
        const std::wstring& GetCurrentProjectPath() const { return m_currentProjectPath; }
        const std::wstring& GetCurrentProjectName() const { return m_currentProjectName; }
        void CloseProject();
        void ShowRecentProjects();
        void BrowsePlugins();
        void SetActiveDropdown(ActiveDropdown dropdown);
        void CloseDropdown() { m_activeDropdown = ActiveDropdown::None; }
        void OnTimer();

    private:
        struct WorkerMailbox;
        enum class ModelDiscoveryState { NotLoaded, Loading, Ready, Failed };
        enum class ModelMenuAction { None, Select, Refresh, Settings };
        struct PendingReply
        {
            int sessionId;
            std::uint64_t requestId;
            std::size_t messageIndex;
            std::wstring query;
            std::wstring model;
            std::shared_ptr<Connector::Cancellation> cancellation;
        };
        struct PendingMcpConnection
        {
            std::wstring projectPath;
            std::wstring resourcePath;
            std::wstring serverKey;
            std::uint64_t requestId = 0;
            std::shared_ptr<Connector::Cancellation> cancellation;
        };
        struct McpState
        {
            McpConnectionState state = McpConnectionState::Disabled;
            std::wstring error;
            bool attempted = false;
            std::shared_ptr<Mcp::Connection> connection;
        };
        HWND m_hwnd = nullptr;
        SidebarMode m_sidebarMode = SidebarMode::Files;
        ActiveDropdown m_activeDropdown = ActiveDropdown::None;
        std::vector<SessionTab> m_tabs;
        int m_activeTab = -1;
        int m_nextSessionId = 0;
        std::vector<FileItem> m_files;
        std::vector<SkillItem> m_skills;
        std::vector<McpServerItem> m_mcpServers;
        std::vector<AgentItem> m_agents;
        std::wstring m_resourceProjectPath;
        std::wstring m_resourcePreferencesDirectory;
        std::wstring m_resourcePreferencesError;
        std::map<std::wstring, ProjectResources::Preferences, ProjectResources::PathLess> m_resourcePreferences;
        ULONGLONG m_lastResourceRefresh = 0;
        std::uint64_t m_resourceRequestId = 0;
        std::shared_ptr<Connector::Cancellation> m_resourceCancellation;
        std::vector<PendingMcpConnection> m_pendingMcpConnections;
        std::map<std::wstring, McpState, ProjectResources::PathLess> m_mcpStates;
        std::vector<PluginItem> m_plugins;
        std::vector<MenuItem> m_projectMenuItems;
        mutable std::vector<MenuItem> m_sessionMenuItems;
        mutable std::vector<MenuItem> m_modelMenuItems;
        std::vector<ModelMenuAction> m_modelMenuActions;
        std::vector<Connector::Model> m_connectorModels;
        ModelDiscoveryState m_modelDiscoveryState = ModelDiscoveryState::NotLoaded;
        std::wstring m_defaultSelectedModel;
        bool m_defaultPlanBeforeEdits = true;
        bool m_defaultAutoRunSafeTools = true;
        std::vector<PendingReply> m_pendingReplies;
        Connector::Settings m_connectorSettings;
        std::wstring m_connectorSettingsPath;
        std::wstring m_connectionStatus = L"Not connected";
        bool m_connectorConnected = false;
        bool m_connectorShutdown = false;
        std::uint64_t m_nextRequestId = 1;
        std::uint64_t m_discoveryRequestId = 0;
        std::shared_ptr<Connector::Cancellation> m_discoveryCancellation;
        std::shared_ptr<WorkerMailbox> m_mailbox;
        std::map<int, std::wstring> m_requestErrors;
        std::vector<std::wstring> m_recentProjects;
        ItemTypeRegistry m_itemTypeRegistry;
        std::wstring m_currentProjectPath;
        std::wstring m_currentProjectName;
        bool m_projectExpanded = true;
        float m_sidebarScrollOffset = 0;
        float m_sidebarMaxScroll = 0;
        SessionTab* ActiveSession();
        const SessionTab* ActiveSession() const;
        void Redraw();
        bool SaveSessionWithDialog(int index);
        bool ConfirmClose(int index);
        void RestoreSubmittedDraft(const PendingReply& pending);
        void CancelAllReplies(bool restoreDrafts);
        void InitializeMenus();
        void RebuildModelMenu();
        void RefreshProjectFiles();
        void MaybeRefreshProjectResources(bool force);
        void ApplyResourceSnapshot(ProjectResources::Snapshot snapshot);
        bool SaveResourceChoices(const std::vector<SkillItem>& skills, const std::vector<McpServerItem>& servers, const std::vector<AgentItem>& agents);
        void StartMcpConnection(int index);
        void CancelMcpConnections();
        void SyncProjectFromActiveSession();
        void RememberProject(const std::wstring& path);
        void ReadRecentProjects();
        void WriteRecentProjects();
        void ShowContextFiles();
    };
}
