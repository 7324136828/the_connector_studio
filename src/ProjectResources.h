#pragma once
#include "AppTypes.h"
#include <map>
#include <string>
#include <vector>

namespace Lattice::ProjectResources
{
    struct PathLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    struct Preferences
    {
        std::map<std::wstring, bool, PathLess> skills;
        std::map<std::wstring, bool, PathLess> mcpServers;
        std::vector<std::wstring> agents;
        std::map<std::wstring, bool, PathLess> agentEnabled;
    };
    struct Snapshot
    {
        std::wstring projectPath;
        std::vector<SkillItem> skills;
        std::vector<McpServerItem> mcpServers;
        std::vector<AgentItem> agents;
    };
    // Only immediate folders below the documented aliases are inspected.
    Snapshot Discover(const std::wstring& projectPath);
    std::wstring Key(const std::wstring& projectPath, const std::wstring& resourcePath);
    std::wstring McpKey(const std::wstring& projectPath, const std::wstring& resourcePath, const std::wstring& serverKey);
    std::wstring DefaultPreferencesDirectory();
    std::wstring PreferencesFile(const std::wstring& directory, const std::wstring& projectPath);
    bool LoadPreferences(const std::wstring& directory, const std::wstring& projectPath, Preferences& preferences, std::wstring& error);
    bool SavePreferences(const std::wstring& directory, const std::wstring& projectPath, const Preferences& preferences, std::wstring& error);
    void ApplyPreferences(Snapshot& snapshot, const Preferences& preferences);
}
