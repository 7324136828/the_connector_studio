#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "AppTypes.h"
#include "UIComponents.h"
#include "ItemTypeRegistry.h"

namespace Lattice
{
    class AppController
    {
    public:
        AppController();
        void Initialize(HWND hwnd);
        SidebarMode GetSidebarMode() const { return m_sidebarMode; }
        ActiveDropdown GetActiveDropdown() const { return m_activeDropdown; }
        int GetActiveTab() const { return m_activeTab; }
        const std::vector<SessionTab>& GetTabs() const { return m_tabs; }
        const std::vector<FileItem>& GetFiles() const { return m_files; }
        const std::vector<SkillItem>& GetSkills() const { return m_skills; }
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
        void AddCustomSkill();
        void BrowsePlugins();
        void SetActiveDropdown(ActiveDropdown dropdown) { m_activeDropdown = dropdown; }
        void CloseDropdown() { m_activeDropdown = ActiveDropdown::None; }
        void OnTimer();

    private:
        struct PendingReply { int sessionId; int countdown; std::wstring query; };
        HWND m_hwnd = nullptr;
        SidebarMode m_sidebarMode = SidebarMode::Files;
        ActiveDropdown m_activeDropdown = ActiveDropdown::None;
        std::vector<SessionTab> m_tabs;
        int m_activeTab = -1;
        int m_nextSessionId = 0;
        std::vector<FileItem> m_files;
        std::vector<SkillItem> m_skills;
        std::vector<PluginItem> m_plugins;
        std::vector<MenuItem> m_projectMenuItems;
        std::vector<MenuItem> m_sessionMenuItems;
        std::vector<MenuItem> m_modelMenuItems;
        std::wstring m_defaultSelectedModel = L"Claude 3.7 Sonnet";
        bool m_defaultPlanBeforeEdits = true;
        bool m_defaultAutoRunSafeTools = true;
        std::vector<PendingReply> m_pendingReplies;
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
        void GenerateReply(int sessionId, const std::wstring& query);
        void InitializeMenus();
        void RefreshProjectFiles();
        void SyncProjectFromActiveSession();
        void RememberProject(const std::wstring& path);
        void ReadRecentProjects();
        void WriteRecentProjects();
        void ShowContextFiles();
    };
}
