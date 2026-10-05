#include "ItemTypeRegistry.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Lattice
{
    namespace
    {
        constexpr std::size_t BuiltinCount = 3, MaxTypes = 200, MaxFileBytes = 1024 * 1024;
        constexpr char Header[] = "CONNECTOR_STUDIO_TYPES\t1";
        bool IsIdChar(wchar_t ch)
        { return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.'; }
        bool ToUtf8(const std::wstring& value, std::string& out)
        {
            out.clear();
            if (value.empty()) return true;
            const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (!length) return false;
            out.resize(length);
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), length, nullptr, nullptr) == length;
        }
        bool FromUtf8(const std::string& value, std::wstring& out)
        {
            out.clear();
            if (value.empty()) return true;
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (!length) return false;
            out.resize(length);
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), length) == length;
        }
        bool IsDisplayText(const std::wstring& value, std::size_t limit, bool allowEmpty)
        {
            if ((!allowEmpty && value.empty()) || value.size() > limit) return false;
            if (!allowEmpty && std::all_of(value.begin(), value.end(), [](wchar_t ch) { return iswspace(ch) != 0; })) return false;
            if (std::any_of(value.begin(), value.end(), [](wchar_t ch) { return ch < L' ' || ch == 0x7f; })) return false;
            std::string ignored;
            return ToUtf8(value, ignored);
        }
        std::string Escape(const std::string& text)
        {
            std::string result;
            for (char ch : text)
            {
                if (ch == '\\') result += "\\\\";
                else if (ch == '\t') result += "\\t";
                else if (ch == '\n') result += "\\n";
                else if (ch == '\r') result += "\\r";
                else result += ch;
            }
            return result;
        }
        bool Unescape(const std::string& text, std::string& result)
        {
            result.clear();
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != '\\') { result += text[i]; continue; }
                if (++i == text.size()) return false;
                switch (text[i])
                {
                case '\\': result += '\\'; break;
                case 't': result += '\t'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                default: return false;
                }
            }
            return true;
        }
        bool ReadFields(const std::string& line, std::vector<std::wstring>& fields)
        {
            fields.clear();
            std::size_t begin = 0;
            for (;;)
            {
                const auto end = line.find('\t', begin);
                std::string raw;
                std::wstring field;
                if (!Unescape(line.substr(begin, end == std::string::npos ? end : end - begin), raw) || !FromUtf8(raw, field)) return false;
                fields.push_back(std::move(field));
                if (end == std::string::npos) break;
                begin = end + 1;
                if (fields.size() >= 5) return false;
            }
            return fields.size() == 5;
        }
    }

    ItemTypeRegistry::ItemTypeRegistry()
    {
        m_types = {
            {L"text-file", L"Text File", L"Create an empty UTF-8 text file in the project's files folder.", L".txt", ItemCategory::Files},
            {L"work-session", L"Work Session", L"Create a saved, blank work session in the project's sessions folder.", L".lattice", ItemCategory::Sessions},
            {L"work-project", L"Work Project", L"Create a project folder containing files and saved work sessions.", L"", ItemCategory::Projects}
        };
    }

    const ItemType* ItemTypeRegistry::Find(const std::wstring& id) const
    {
        const auto found = std::find_if(m_types.begin(), m_types.end(), [&id](const ItemType& type) { return type.id == id; });
        return found == m_types.end() ? nullptr : &*found;
    }

    bool ItemTypeRegistry::RegisterType(const ItemType& type, std::wstring& error)
    {
        error.clear();
        if (type.category != ItemCategory::Files && type.category != ItemCategory::Sessions && type.category != ItemCategory::Projects)
        { error = L"Choose Files, Sessions, or Projects as the type category."; return false; }
        if (type.id.empty() || type.id.size() > 64 || !std::all_of(type.id.begin(), type.id.end(), IsIdChar))
        { error = L"Type identifiers must contain 1–64 lowercase letters, digits, dots, underscores, or hyphens."; return false; }
        if (!IsDisplayText(type.name, 80, false) || !IsDisplayText(type.description, 512, true))
        { error = L"Provide a valid type name (up to 80 characters) and description (up to 512 characters)."; return false; }
        if (type.category == ItemCategory::Projects)
        {
            if (!type.extension.empty()) { error = L"Project types use folders and must not have a file extension."; return false; }
        }
        else if (type.category == ItemCategory::Sessions)
        {
            if (type.extension != L".lattice") { error = L"Work session types must use the compatible .lattice session format."; return false; }
        }
        else if (type.extension.size() < 2 || type.extension.size() > 16 || type.extension.front() != L'.' ||
            !std::all_of(type.extension.begin() + 1, type.extension.end(), [](wchar_t ch) {
                return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_'; }))
        { error = L"Text file types need a safe lowercase extension, such as .txt or .md."; return false; }
        if (Find(type.id)) { error = L"That type identifier is already registered. Built-in types cannot be replaced."; return false; }
        if (m_types.size() >= MaxTypes) { error = L"The registry already contains 200 types."; return false; }
        m_types.push_back(type);
        return true;
    }

    bool ItemTypeRegistry::Save(const std::wstring& path, std::wstring& error) const
    {
        error.clear();
        std::ostringstream output;
        output << Header << '\n';
        for (std::size_t i = BuiltinCount; i < m_types.size(); ++i)
        {
            const auto& type = m_types[i];
            output << static_cast<int>(type.category);
            for (const auto* field : {&type.id, &type.name, &type.description, &type.extension})
            {
                std::string value;
                if (!ToUtf8(*field, value)) { error = L"A type contains invalid Unicode."; return false; }
                output << '\t' << Escape(value);
            }
            output << '\n';
        }
        const std::string bytes = output.str();
        if (bytes.size() > MaxFileBytes) { error = L"The type registry is too large."; return false; }
        const std::filesystem::path destination(path);
        if (destination.empty() || destination.filename().empty()) { error = L"Choose a registry file path."; return false; }
        const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::current_path();
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(parent.c_str(), L"ctr", 0, temporary)) { error = L"The registry location is unavailable or not writable."; return false; }
        HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool saved = file != INVALID_HANDLE_VALUE;
        if (saved)
        {
            saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
            if (!CloseHandle(file)) saved = false;
        }
        if (saved) saved = MoveFileExW(temporary, destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!saved) { DeleteFileW(temporary); error = L"The type registry could not be saved. Existing registrations were kept."; }
        return saved;
    }

    bool ItemTypeRegistry::Load(const std::wstring& path, std::wstring& error)
    {
        error.clear();
        std::error_code filesystemError;
        const auto size = std::filesystem::file_size(path, filesystemError);
        if (filesystemError || size > MaxFileBytes) { error = L"The registry is unavailable or exceeds the size limit."; return false; }
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!file || (size && !file.read(bytes.data(), static_cast<std::streamsize>(size))))
        { error = L"The registry could not be read."; return false; }
        std::istringstream input(bytes);
        std::string line;
        if (!std::getline(input, line) || line != Header) { error = L"This is not a supported Connector Studio type registry."; return false; }
        ItemTypeRegistry loaded;
        while (std::getline(input, line))
        {
            std::vector<std::wstring> fields;
            if (!ReadFields(line, fields) || fields[0].size() != 1 || fields[0][0] < L'0' || fields[0][0] > L'2')
            { error = L"The registry contains an invalid type record."; return false; }
            ItemType type{fields[1], fields[2], fields[3], fields[4], static_cast<ItemCategory>(fields[0][0] - L'0')};
            if (!loaded.RegisterType(type, error)) return false;
        }
        m_types = std::move(loaded.m_types);
        return true;
    }
}
