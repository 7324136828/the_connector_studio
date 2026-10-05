#include "ProjectResources.h"
#include "AppIdentity.h"
#include "McpClient.h"
#include "../third_party/nlohmann/json.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Lattice::ProjectResources
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;
        constexpr std::size_t MaxEntries = 1000, MaxFileBytes = 1024 * 1024;
        const wchar_t* SkillAliases[] = {L"skill", L"skills", L".skill", L".skills"};
        const wchar_t* McpAliases[] = {L"mcp", L".mcp", L"mcps", L".mcps"};
        const wchar_t* AgentAliases[] = {L"agent", L"agents", L".agent", L".agents"};
        bool Same(const std::wstring& a, const std::wstring& b)
        { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }
        std::string Utf8(const std::wstring& value)
        {
            if (value.empty()) return {};
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (!count) throw std::runtime_error("Invalid Unicode");
            std::string text(count, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text.data(), count, nullptr, nullptr)) throw std::runtime_error("Invalid Unicode");
            return text;
        }
        std::wstring Wide(const std::string& value)
        {
            if (value.empty()) return {};
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (!count) throw std::runtime_error("Invalid UTF8");
            std::wstring text(count, L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text.data(), count)) throw std::runtime_error("Invalid UTF8");
            if (text.find(L'\0') != std::wstring::npos) throw std::runtime_error("Embedded null");
            return text;
        }
        bool IsPlainDirectory(const fs::path& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
        bool Alias(const std::wstring& name, const wchar_t* const* aliases)
        { for (int i = 0; i < 4; ++i) if (Same(name, aliases[i])) return true; return false; }
        bool ValidKey(const std::wstring& key, const wchar_t* const* aliases)
        {
            if (key.empty() || key.size() > 32767 || key.find(L'\0') != std::wstring::npos || key.find(L'\\') != std::wstring::npos) return false;
            const auto slash = key.find(L'/');
            if (slash == std::wstring::npos || key.find(L'/', slash + 1) != std::wstring::npos || !Alias(key.substr(0, slash), aliases)) return false;
            const auto name = key.substr(slash + 1);
            return !name.empty() && name != L"." && name != L".." && name.find(L':') == std::wstring::npos;
        }
        bool ValidMcpKey(const std::wstring& key)
        {
            const auto delimiter = key.find(L'?');
            if (delimiter == std::wstring::npos) return ValidKey(key, McpAliases);
            if (!ValidKey(key.substr(0, delimiter), McpAliases)) return false;
            const auto suffix = key.substr(delimiter + 1);
            return !suffix.empty() && suffix.size() <= 8192 && !(suffix.size() % 2)
                && std::all_of(suffix.begin(), suffix.end(), [](wchar_t character) { return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f'); });
        }
        std::vector<fs::path> Children(const fs::path& root, const wchar_t* const* aliases)
        {
            std::set<std::wstring, PathLess> seen;
            std::vector<fs::path> paths;
            for (int i = 0; i < 4; ++i)
            {
                const auto folder = root / aliases[i];
                if (!IsPlainDirectory(folder)) continue;
                std::error_code error;
                fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error);
                // Bound inspection as well as the resulting list. No recursive reads,
                // symlink following, instruction loading, or process starts occur here.
                std::size_t inspected = 0;
                while (!error && iterator != fs::directory_iterator{} && inspected++ < 10000)
                {
                    const auto path = iterator->path();
                    if (IsPlainDirectory(path))
                    {
                        const auto canonical = fs::weakly_canonical(path, error);
                        if (!error && Same(canonical.parent_path().wstring(), folder.wstring()) && seen.insert(canonical.wstring()).second)
                            paths.push_back(canonical);
                        error.clear();
                    }
                    iterator.increment(error);
                }
            }
            std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b)
            {
                const auto an = a.filename().wstring(), bn = b.filename().wstring();
                const auto comparison = CompareStringOrdinal(an.c_str(), -1, bn.c_str(), -1, TRUE);
                return comparison == CSTR_EQUAL ? PathLess{}(a.wstring(), b.wstring()) : comparison == CSTR_LESS_THAN;
            });
            if (paths.size() > MaxEntries) paths.resize(MaxEntries);
            return paths;
        }
        bool WriteAtomic(const std::wstring& path, const std::string& bytes, std::wstring& error)
        {
            const fs::path destination(path);
            const auto lockDeadline = GetTickCount64() + 150;
            const auto retryLock = [lockDeadline, &destination](DWORD code, const wchar_t* ownedSource = nullptr)
            {
                if (GetTickCount64() >= lockDeadline) return false;
                if (code == ERROR_ACCESS_DENIED && ownedSource)
                {
                    // Replacing an open destination can report access denied even
                    // when its ACL permits deletion. A scanner may also retain the
                    // newly closed, owned temporary file. Retry only if DELETE-access
                    // probes prove sharing/lock contention, without changing either file.
                    const auto attributes = GetFileAttributesW(destination.c_str());
                    const auto attributeError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                    if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                        || (attributes == INVALID_FILE_ATTRIBUTES && attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)) return false;
                    const auto probeDelete = [](const wchar_t* candidate)
                    {
                        const HANDLE probe = CreateFileW(candidate, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (probe != INVALID_HANDLE_VALUE) { CloseHandle(probe); return DWORD(ERROR_SUCCESS); }
                        return GetLastError();
                    };
                    const auto destinationCode = attributes == INVALID_FILE_ATTRIBUTES ? ERROR_SUCCESS : probeDelete(destination.c_str());
                    if (destinationCode == ERROR_SHARING_VIOLATION || destinationCode == ERROR_LOCK_VIOLATION) code = destinationCode;
                    else
                    {
                        if (destinationCode != ERROR_SUCCESS) return false;
                        code = probeDelete(ownedSource);
                    }
                }
                if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) return false;
                const auto now = GetTickCount64();
                if (now >= lockDeadline) return false;
                Sleep(static_cast<DWORD>((std::min)(ULONGLONG(5), lockDeadline - now)));
                return GetTickCount64() < lockDeadline;
            };
            const auto existingAttributes = GetFileAttributesW(destination.c_str());
            if (existingAttributes != INVALID_FILE_ATTRIBUTES && (existingAttributes & FILE_ATTRIBUTE_READONLY))
            { error = L"Resource preferences could not be saved (Windows error 5). The previous choices were kept."; return false; }
            std::error_code fsError;
            fs::create_directories(destination.parent_path(), fsError);
            if (fsError)
            { error = L"Resource preferences could not create the settings folder (Windows error " + std::to_wstring(fsError.value()) + L")."; return false; }
            wchar_t temporary[MAX_PATH]{};
            if (!GetTempFileNameW(destination.parent_path().c_str(), L"csr", 0, temporary))
            { const auto code = GetLastError(); error = L"Resource preferences could not create a temporary file (Windows error " + std::to_wstring(code) + L")."; return false; }
            HANDLE file = INVALID_HANDLE_VALUE;
            DWORD saveError = ERROR_SUCCESS;
            const wchar_t* stage = L"open the temporary file";
            do
            {
                file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                saveError = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            } while (file == INVALID_HANDLE_VALUE && retryLock(saveError));
            DWORD written = 0;
            bool saved = false;
            if (file != INVALID_HANDLE_VALUE)
            {
                stage = L"write the temporary file";
                saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                    && written == bytes.size() && FlushFileBuffers(file);
                saveError = saved ? ERROR_SUCCESS : GetLastError();
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (saved)
            {
                stage = L"replace the preferences file";
                do
                {
                    saved = MoveFileExW(temporary, destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                    saveError = saved ? ERROR_SUCCESS : GetLastError();
                } while (!saved && retryLock(saveError, temporary));
            }
            if (!saved)
            {
                DeleteFileW(temporary);
                error = L"Resource preferences could not " + std::wstring(stage) + L" (Windows error " + std::to_wstring(saveError) + L"). The previous choices were kept.";
            }
            else error.clear();
            return saved;
        }
    }
    bool PathLess::operator()(const std::wstring& a, const std::wstring& b) const
    { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; }
    Snapshot Discover(const std::wstring& projectPath)
    {
        Snapshot result;
        if (projectPath.empty()) return result;
        std::error_code error;
        const auto root = fs::weakly_canonical(projectPath, error);
        if (error || !root.is_absolute() || !fs::is_directory(root, error) || error) return result;
        result.projectPath = root.wstring();
        std::size_t remaining = MaxEntries;
        for (const auto& path : Children(root, SkillAliases))
        {
            if (!remaining) break;
            --remaining;
            result.skills.push_back({L"SK", path.filename().wstring(), Key(result.projectPath, path.wstring()), false, path.wstring()});
        }
        remaining = MaxEntries - result.skills.size();
        for (const auto& path : Children(root, McpAliases))
        {
            if (!remaining) break;
            std::vector<std::wstring> keys;
            try { keys = Mcp::ServerKeys(path.wstring()); }
            catch (...) { /* A broken config remains a visible, disabled folder. */ }
            if (keys.empty())
            {
                --remaining;
                result.mcpServers.push_back({path.filename().wstring(), path.wstring(), false, Key(result.projectPath, path.wstring())});
            }
            else for (const auto& serverKey : keys)
            {
                if (!remaining) break;
                --remaining;
                McpServerItem item;
                item.name = keys.size() == 1 ? path.filename().wstring() : path.filename().wstring() + L" / " + serverKey;
                item.fullPath = path.wstring(); item.serverKey = serverKey; item.description = Key(result.projectPath, path.wstring());
                result.mcpServers.push_back(std::move(item));
            }
        }
        remaining = MaxEntries - result.skills.size() - result.mcpServers.size();
        for (const auto& path : Children(root, AgentAliases))
        {
            if (!remaining) break;
            --remaining;
            result.agents.push_back({path.filename().wstring(), path.wstring(), static_cast<int>(result.agents.size() + 1)});
        }
        return result;
    }
    std::wstring Key(const std::wstring& projectPath, const std::wstring& resourcePath)
    {
        const auto relative = fs::path(resourcePath).lexically_relative(fs::path(projectPath));
        return relative.generic_wstring();
    }
    std::wstring McpKey(const std::wstring& projectPath, const std::wstring& resourcePath, const std::wstring& serverKey)
    {
        auto key = Key(projectPath, resourcePath);
        if (serverKey.empty()) return key;
        constexpr wchar_t Hex[] = L"0123456789abcdef";
        key += L'?';
        for (const unsigned char byte : Utf8(serverKey)) { key += Hex[byte >> 4]; key += Hex[byte & 15]; }
        return key;
    }
    std::wstring DefaultPreferencesDirectory()
    {
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) return {};
        const auto directory = fs::path(appData) / AppIdentity::SettingsFolder / L"resource-preferences";
        CoTaskMemFree(appData); return directory.wstring();
    }
    std::wstring PreferencesFile(const std::wstring& directory, const std::wstring& projectPath)
    {
        if (directory.empty() || projectPath.empty()) return {};
        // The canonical project path is also stored and checked inside the file.
        // A hash collision therefore cannot apply another project's preferences.
        const auto foldedLength = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, projectPath.data(), static_cast<int>(projectPath.size()), nullptr, 0, nullptr, nullptr, 0);
        std::wstring folded(foldedLength > 0 ? static_cast<std::size_t>(foldedLength) : projectPath.size(), L'\0');
        if (!foldedLength || !LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, projectPath.data(), static_cast<int>(projectPath.size()), folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0)) folded = projectPath;
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto character : folded)
        { hash ^= static_cast<unsigned char>(character & 255); hash *= 1099511628211ull; hash ^= static_cast<unsigned char>((character >> 8) & 255); hash *= 1099511628211ull; }
        std::wostringstream name; name << std::hex << std::setfill(L'0') << std::setw(16) << hash << L".json";
        return (fs::path(directory) / name.str()).wstring();
    }
    bool LoadPreferences(const std::wstring& directory, const std::wstring& projectPath, Preferences& preferences, std::wstring& error)
    {
        error.clear();
        const auto path = PreferencesFile(directory, projectPath);
        if (path.empty()) return true;
        std::error_code fsError;
        if (!fs::exists(path, fsError))
        { if (!fsError) return true; error = L"Resource preferences could not be read."; return false; }
        const auto size = fs::file_size(path, fsError);
        if (fsError || size > MaxFileBytes) { error = L"Resource preferences are unavailable or too large."; return false; }
        try
        {
            std::ifstream file(fs::path(path), std::ios::binary);
            std::string bytes(static_cast<std::size_t>(size), '\0');
            if (!file || (size && !file.read(bytes.data(), static_cast<std::streamsize>(size)))) throw std::runtime_error("Read");
            const auto json = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) { if (depth > 8) throw std::runtime_error("Depth"); return true; });
            if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer() || json["version"] != 1
                || !json.contains("project") || !json["project"].is_string()
                || !Same(Wide(json["project"].get<std::string>()), projectPath)
                || !json.contains("skills") || !json["skills"].is_object() || !json.contains("mcp_servers") || !json["mcp_servers"].is_object()
                || !json.contains("agents") || !json["agents"].is_array()) throw std::runtime_error("Schema");
            if (json["skills"].size() + json["mcp_servers"].size() + json["agents"].size() > MaxEntries) throw std::runtime_error("Count");
            if (json.contains("agent_enabled") && (!json["agent_enabled"].is_object() || json["agent_enabled"].size() > MaxEntries)) throw std::runtime_error("Agent enabled schema");
            Preferences loaded;
            const auto readMap = [](const Json& source, auto& target, const wchar_t* const* aliases)
            {
                for (auto it = source.begin(); it != source.end(); ++it)
                {
                    const auto key = Wide(it.key());
                    if (!(aliases == McpAliases ? ValidMcpKey(key) : ValidKey(key, aliases)) || !it.value().is_boolean()
                        || !target.emplace(key, it.value().get<bool>()).second) throw std::runtime_error("Entry");
                }
            };
            readMap(json["skills"], loaded.skills, SkillAliases); readMap(json["mcp_servers"], loaded.mcpServers, McpAliases);
            if (json.contains("agent_enabled")) readMap(json["agent_enabled"], loaded.agentEnabled, AgentAliases);
            std::set<std::wstring, PathLess> seen;
            for (const auto& entry : json["agents"])
            {
                if (!entry.is_string()) throw std::runtime_error("Agent");
                const auto key = Wide(entry.get<std::string>());
                if (!ValidKey(key, AgentAliases) || !seen.insert(key).second) throw std::runtime_error("Agent path");
                loaded.agents.push_back(key);
            }
            preferences = std::move(loaded); return true;
        }
        catch (...) { error = L"Resource preferences contain invalid data. Existing choices were kept."; return false; }
    }
    bool SavePreferences(const std::wstring& directory, const std::wstring& projectPath, const Preferences& preferences, std::wstring& error)
    {
        error.clear();
        const auto path = PreferencesFile(directory, projectPath);
        if (path.empty()) { error = L"Resource preferences have no settings location."; return false; }
        try
        {
            if (preferences.skills.size() + preferences.mcpServers.size() + preferences.agents.size() > MaxEntries) throw std::runtime_error("Count");
            if (preferences.agentEnabled.size() > MaxEntries) throw std::runtime_error("Agent enabled count");
            Json json{{"version", 1}, {"project", Utf8(projectPath)}, {"skills", Json::object()}, {"mcp_servers", Json::object()}, {"agents", Json::array()}, {"agent_enabled", Json::object()}};
            for (const auto& entry : preferences.skills)
            { if (!ValidKey(entry.first, SkillAliases)) throw std::runtime_error("Skill"); json["skills"][Utf8(entry.first)] = entry.second; }
            for (const auto& entry : preferences.mcpServers)
            { if (!ValidMcpKey(entry.first)) throw std::runtime_error("MCP"); json["mcp_servers"][Utf8(entry.first)] = entry.second; }
            std::set<std::wstring, PathLess> seen;
            for (const auto& entry : preferences.agents)
            { if (!ValidKey(entry, AgentAliases) || !seen.insert(entry).second) throw std::runtime_error("Agent"); json["agents"].push_back(Utf8(entry)); }
            for (const auto& entry : preferences.agentEnabled)
            { if (!ValidKey(entry.first, AgentAliases)) throw std::runtime_error("Agent enabled"); json["agent_enabled"][Utf8(entry.first)] = entry.second; }
            const auto bytes = json.dump(2) + "\n";
            if (bytes.size() > MaxFileBytes) throw std::runtime_error("Size");
            return WriteAtomic(path, bytes, error);
        }
        catch (...) { error = L"Resource preferences could not be saved because their data is invalid."; return false; }
    }
    void ApplyPreferences(Snapshot& snapshot, const Preferences& preferences)
    {
        for (auto& item : snapshot.skills)
        { const auto found = preferences.skills.find(Key(snapshot.projectPath, item.fullPath)); item.isEnabled = found != preferences.skills.end() && found->second; }
        for (auto& item : snapshot.mcpServers)
        { const auto found = preferences.mcpServers.find(McpKey(snapshot.projectPath, item.fullPath, item.serverKey)); item.isEnabled = found != preferences.mcpServers.end() && found->second; }
        for (auto& item : snapshot.agents)
        { const auto found = preferences.agentEnabled.find(Key(snapshot.projectPath, item.fullPath)); item.isEnabled = found != preferences.agentEnabled.end() && found->second; }
        std::vector<AgentItem> ordered;
        std::set<std::wstring, PathLess> used;
        for (const auto& key : preferences.agents)
            for (const auto& item : snapshot.agents)
                if (Same(key, Key(snapshot.projectPath, item.fullPath)) && used.insert(item.fullPath).second) { ordered.push_back(item); break; }
        for (const auto& item : snapshot.agents) if (used.insert(item.fullPath).second) ordered.push_back(item);
        snapshot.agents = std::move(ordered);
        for (std::size_t index = 0; index < snapshot.agents.size(); ++index) snapshot.agents[index].priority = static_cast<int>(index + 1);
    }
}
