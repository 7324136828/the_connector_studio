#pragma once
#include <string>
#include <vector>
#include <d2d1.h>

namespace Lattice
{
    enum class SidebarMode
    {
        Files,
        Session,
        Plugins
    };

    enum class ActiveDropdown
    {
        None,
        Project,
        Session,
        Model,
        SidebarModel
    };

    enum class IconType
    {
        Agent,
        Branch,
        Chevron,
        Close,
        File,
        Folder,
        Mic,
        More,
        Paperclip,
        Plugin,
        Search,
        Send,
        Settings,
        Sparkles
    };

    struct FileItem
    {
        std::wstring name;
        std::wstring type; // "folder", "react", "ts", "md", "env"
        int indent = 0;
        bool isFolder = false;
        bool isExpanded = true;
        std::wstring fullPath;
    };

    struct PlanStep
    {
        int number = 1;
        std::wstring title;
        std::wstring description;
    };

    struct MessageItem
    {
        std::wstring role; // "user" or "assistant"
        std::wstring author; // User or assistant display name.
        std::wstring time;
        std::wstring text;
        std::vector<std::wstring> items;
        std::vector<PlanStep> planSteps;
    };

    struct SessionTab
    {
        int id = 0;
        std::wstring title;
        bool isAmber = false;
        std::wstring eyebrow;
        std::wstring projectName;
        int filesCount = 0;
        std::vector<MessageItem> messages;
        float scrollOffset = 0.0f;
        float maxScroll = 0.0f;
        std::wstring draftText;
        std::wstring savedPath;
        bool dirty = false;
        std::wstring projectPath;
        std::vector<std::wstring> attachedPaths;
        std::wstring selectedModel;
        bool planBeforeEdits = true;
        bool autoRunSafeTools = true;
        bool scrollToEnd = false;
    };

    struct SkillItem
    {
        std::wstring badge;
        std::wstring name;
        std::wstring description;
        bool isEnabled = false;
        std::wstring fullPath;
    };

    struct AgentItem
    {
        std::wstring name;
        std::wstring fullPath;
        int priority = 0; // Display order is the vector order, starting at one.
        bool isEnabled = false;
    };

    enum class McpConnectionState { Disabled, Connecting, Connected };

    struct McpServerItem
    {
        std::wstring name;
        std::wstring fullPath;
        bool isEnabled = false;
        std::wstring description;
        McpConnectionState connectionState = McpConnectionState::Disabled;
        std::wstring connectionError;
        std::wstring serverKey;
    };

    struct PluginItem
    {
        std::wstring name;
        std::wstring description;
        bool isEnabled = true;
        bool isViolet = false;
    };

    struct MenuItem
    {
        std::wstring label;
        std::wstring shortcut;
        bool hasDivider = false;
        bool enabled = true;
        bool selected = false;
    };
}
