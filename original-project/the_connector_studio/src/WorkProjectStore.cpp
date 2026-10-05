#include "WorkProjectStore.h"
#include "SessionPersistence.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <cstdint>

namespace Lattice::WorkProjectStore
{
    namespace fs = std::filesystem;
    namespace
    {
        constexpr wchar_t Descriptor[] = L"project.connector";
        constexpr wchar_t ProjectSignature[] = L"CONNECTOR_WORK_PROJECT_1";
        constexpr wchar_t ItemSignature[] = L"CONNECTOR_WORK_ITEM_1";
        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        };
        struct Artifact
        {
            std::wstring path;
            BY_HANDLE_FILE_INFORMATION identity{};
            bool directory = false;
            std::uint64_t size = 0;
            std::uint64_t hash = 0;
        };
        std::uint64_t Hash(const std::uint8_t* bytes, std::size_t size)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
            return hash;
        }
        bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b)
        { return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow; }
        bool SameText(const std::wstring& a, const std::wstring& b)
        { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }
        bool SingleLine(const std::wstring& text)
        {
            return !text.empty() && text.size() <= 256 &&
                std::none_of(text.begin(), text.end(), [](wchar_t c) { return c < 32 || c == 127; });
        }
        bool Utf8(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& error)
        {
            if (text.size() > 16384) { error = L"Item metadata is too large."; return false; }
            const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!length) { error = L"The name or type contains invalid Unicode."; return false; }
            bytes.resize(static_cast<std::size_t>(length));
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                reinterpret_cast<char*>(bytes.data()), length, nullptr, nullptr) != 0;
        }
        bool Document(const wchar_t* signature, const std::wstring& name, const std::wstring& type,
            std::vector<std::uint8_t>& bytes, std::wstring& error)
        {
            if (!SingleLine(type)) { error = L"The item type ID is invalid."; return false; }
            return Utf8(std::wstring(signature) + L"\n" + name + L"\n" + type + L"\n", bytes, error);
        }
        bool ReadDescriptor(const fs::path& path, std::wstring& name, std::wstring& type, std::wstring& error)
        {
            Handle file;
            file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.value == INVALID_HANDLE_VALUE) { error = L"The project descriptor cannot be opened."; return false; }
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 1 || size.QuadPart > 16384)
            { error = L"The project descriptor is invalid or too large."; return false; }
            std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart)); DWORD count = 0;
            if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
            { error = L"The project descriptor could not be read completely."; return false; }
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (!length) { error = L"The project descriptor must contain valid UTF-8."; return false; }
            std::wstring text(length, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
            std::wistringstream input(text); std::wstring signature, extra;
            if (!std::getline(input, signature) || signature != ProjectSignature || !std::getline(input, name)
                || !std::getline(input, type) || std::getline(input, extra) || !SingleLine(type) || !ValidateName(name, error))
            { error = L"The project descriptor is incomplete or uses an unsupported format."; return false; }
            return true;
        }
        bool Directory(const fs::path& path, bool forbidReparse, std::wstring& error)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
                || (forbidReparse && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)))
            { error = L"Choose an existing project folder. Its files and sessions folders must remain inside the project."; return false; }
            return true;
        }
        bool LockDirectory(const fs::path& path, Handle& handle, std::wstring& error)
        {
            // Prevent directory replacement while a child is created. Open the directory
            // itself so a junction cannot redirect an item outside its chosen project.
            handle.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION information{};
            if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value, &information)
                || !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            { error = L"The selected directory cannot be safely opened. Choose an existing folder inside the project."; return false; }
            return true;
        }
        bool CreateFolder(const fs::path& path, std::vector<Artifact>& artifacts, std::wstring& error)
        {
            if (!CreateDirectoryW(path.c_str(), nullptr))
            { error = GetLastError() == ERROR_ALREADY_EXISTS ? L"The destination already exists. Choose a different name." : L"The project folder could not be created. Check permissions and the location."; return false; }
            Handle folder;
            folder.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            Artifact artifact; artifact.path = path.wstring(); artifact.directory = true;
            if (folder.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(folder.value, &artifact.identity))
            { RemoveDirectoryW(path.c_str()); error = L"The new project folder could not be verified."; return false; }
            artifacts.push_back(std::move(artifact)); return true;
        }
        bool CreateExclusiveFile(const fs::path& path, const std::vector<std::uint8_t>& bytes,
            std::vector<Artifact>& artifacts, std::wstring& error)
        {
            Handle file;
            file.value = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.value == INVALID_HANDLE_VALUE)
            { error = GetLastError() == ERROR_FILE_EXISTS ? L"The destination already exists. Choose a different name." : L"The new item could not be created. Check permissions and free disk space."; return false; }
            DWORD written = 0;
            Artifact artifact; artifact.path = path.wstring(); artifact.size = bytes.size(); artifact.hash = Hash(bytes.data(), bytes.size());
            if ((!bytes.empty() && !WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
                || written != bytes.size() || !FlushFileBuffers(file.value) || !GetFileInformationByHandle(file.value, &artifact.identity))
            {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
                error = L"The new item could not be written completely. Existing items were kept."; return false;
            }
            artifacts.push_back(std::move(artifact)); return true;
        }
        bool RemoveArtifact(const Artifact& artifact)
        {
            Handle file;
            file.value = CreateFileW(artifact.path.c_str(), DELETE | GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                artifact.directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
            BY_HANDLE_FILE_INFORMATION identity{};
            if (!GetFileInformationByHandle(file.value, &identity) || !SameFile(identity, artifact.identity)) return false;
            if (!artifact.directory)
            {
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(file.value, &size) || static_cast<std::uint64_t>(size.QuadPart) != artifact.size) return false;
                std::uint64_t hash = 14695981039346656037ULL; std::uint8_t buffer[16384]; DWORD read = 0;
                for (;;)
                {
                    if (!ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr)) return false;
                    if (!read) break;
                    for (DWORD i = 0; i < read; ++i) { hash ^= buffer[i]; hash *= 1099511628211ULL; }
                }
                if (hash != artifact.hash) return false;
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            return SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
        }
        bool Rollback(const std::vector<Artifact>& artifacts)
        {
            bool success = true;
            for (auto it = artifacts.rbegin(); it != artifacts.rend(); ++it) if (!RemoveArtifact(*it)) success = false;
            return success;
        }
        std::wstring WithExtension(const std::wstring& name, const std::wstring& extension)
        {
            if (name.size() >= extension.size() && SameText(name.substr(name.size() - extension.size()), extension)) return name;
            return name + extension;
        }
    }
    struct CreationRecord { std::vector<Artifact> artifacts; };

    bool ValidateName(const std::wstring& name, std::wstring& error)
    {
        if (name.empty() || name.size() > 120 || name == L"." || name == L".." || name.front() == L' ' || name.back() == L' ' || name.back() == L'.'
            || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == 127 || wcschr(L"<>:\"/\\|?*", c) != nullptr; }))
        { error = L"Use a name of 1 to 120 characters without path separators, control characters, reserved symbols, or leading/trailing spaces and periods."; return false; }
        auto stem = name.substr(0, name.find(L'.'));
        while (!stem.empty() && stem.back() == L' ') stem.pop_back();
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$"
            || (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT")
                && ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00B9' || stem[3] == L'\u00B2' || stem[3] == L'\u00B3')))
        { error = L"Windows reserves this name. Choose a different item name."; return false; }
        std::vector<std::uint8_t> bytes;
        if (!Utf8(name, bytes, error)) return false;
        error.clear(); return true;
    }

    bool Open(const std::wstring& supplied, ProjectInfo& project, std::wstring& error)
    {
        std::error_code filesystemError;
        fs::path path(supplied);
        const DWORD suppliedAttributes = GetFileAttributesW(path.c_str());
        if (SameText(path.filename().wstring(), Descriptor) && suppliedAttributes != INVALID_FILE_ATTRIBUTES
            && !(suppliedAttributes & FILE_ATTRIBUTE_DIRECTORY)) path = path.parent_path();
        const fs::path root = fs::weakly_canonical(path, filesystemError);
        if (supplied.empty() || filesystemError) { error = L"Choose an existing project folder."; return false; }
        if (!Directory(root, false, error)) return false;
        ProjectInfo opened; opened.rootPath = root.wstring();
        opened.name = root.filename().empty() ? root.wstring() : root.filename().wstring();
        const DWORD descriptorAttributes = GetFileAttributesW((root / Descriptor).c_str());
        if (descriptorAttributes == INVALID_FILE_ATTRIBUTES)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            { error = L"The project descriptor cannot be checked."; return false; }
            project = std::move(opened); error.clear(); return true;
        }
        if (descriptorAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        { error = L"The project descriptor must be a regular file inside the project."; return false; }
        if (!ReadDescriptor(root / Descriptor, opened.name, opened.typeId, error)
            || !Directory(root / L"files", true, error) || !Directory(root / L"sessions", true, error)) return false;
        opened.managed = true;
        fs::directory_iterator it(root / L"sessions", fs::directory_options::none, filesystemError);
        while (!filesystemError && it != fs::directory_iterator{})
        {
            const auto entry = *it;
            if (SameText(entry.path().extension().wstring(), L".lattice"))
            {
                const DWORD attributes = GetFileAttributesW(entry.path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                { error = L"Saved sessions must be regular files inside the sessions folder."; return false; }
                if (opened.sessionPaths.size() >= 100) { error = L"This project has more than 100 saved sessions."; return false; }
                opened.sessionPaths.push_back(entry.path().wstring());
            }
            it.increment(filesystemError);
        }
        if (filesystemError) { error = L"The saved sessions folder cannot be read."; return false; }
        std::sort(opened.sessionPaths.begin(), opened.sessionPaths.end(), [](const auto& a, const auto& b)
        { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
        project = std::move(opened); error.clear(); return true;
    }

    bool Create(const NewItemRequest& request, const ItemType& type, CreatedItem& item,
        std::wstring& error, const SessionTab* sessionDefaults)
    {
        if ((request.category != ItemCategory::Files && request.category != ItemCategory::Sessions && request.category != ItemCategory::Projects)
            || request.category != type.category || request.typeId != type.id)
        { error = L"Choose a registered type from the selected category."; return false; }
        if (!ValidateName(request.name, error)) return false;
        if (!SingleLine(type.id)) { error = L"The item type ID is invalid."; return false; }
        auto record = std::make_shared<CreationRecord>();
        CreatedItem created; created.category = request.category;
        bool success = false;
        if (request.category == ItemCategory::Projects)
        {
            std::error_code filesystemError;
            const auto parent = fs::weakly_canonical(request.location, filesystemError);
            if (request.location.empty() || filesystemError) { error = L"Choose an existing parent folder for the project."; return false; }
            if (!Directory(parent, false, error)) return false;
            Handle parentLock;
            if (!LockDirectory(parent, parentLock, error)) return false;
            const auto root = parent / request.name;
            created.path = root.wstring(); created.projectRoot = created.path; created.metadataPath = (root / Descriptor).wstring();
            std::vector<std::uint8_t> descriptor;
            if (!Document(ProjectSignature, request.name, type.id, descriptor, error)) return false;
            success = CreateFolder(root, record->artifacts, error) && CreateFolder(root / L"files", record->artifacts, error)
                && CreateFolder(root / L"sessions", record->artifacts, error)
                && CreateExclusiveFile(root / Descriptor, descriptor, record->artifacts, error);
        }
        else
        {
            ProjectInfo project;
            if (!Open(request.location, project, error)) return false;
            if (!project.managed) { error = L"Choose a Work Project created by Connector Studio for this item."; return false; }
            Handle rootLock, folderLock;
            const auto folder = fs::path(project.rootPath) / (request.category == ItemCategory::Sessions ? L"sessions" : L"files");
            if (!LockDirectory(project.rootPath, rootLock, error) || !LockDirectory(folder, folderLock, error)) return false;
            const auto extension = request.category == ItemCategory::Sessions ? std::wstring(L".lattice") : type.extension;
            if (extension.empty() || extension[0] != L'.' || !ValidateName(L"item" + extension, error))
            { error = L"This item type has an invalid file extension."; return false; }
            const auto filename = WithExtension(request.name, extension);
            if (!ValidateName(filename, error)) return false;
            const auto path = folder / filename;
            created.path = path.wstring(); created.metadataPath = created.path + L".connector-item"; created.projectRoot = project.rootPath;
            std::vector<std::uint8_t> bytes, metadata;
            if (!Document(ItemSignature, request.name, type.id, metadata, error)) return false;
            if (request.category == ItemCategory::Sessions)
            {
                SessionTab session;
                session.title = fs::path(filename).stem().wstring(); session.eyebrow = L"SESSION";
                session.projectName = project.name; session.projectPath = project.rootPath; session.filesCount = 0;
                if (sessionDefaults)
                { session.selectedModel = sessionDefaults->selectedModel; session.planBeforeEdits = sessionDefaults->planBeforeEdits; session.autoRunSafeTools = sessionDefaults->autoRunSafeTools; }
                if (!SessionPersistence::Serialize(session, bytes, error)) return false;
            }
            success = CreateExclusiveFile(path, bytes, record->artifacts, error) && CreateExclusiveFile(created.metadataPath, metadata, record->artifacts, error);
        }
        if (!success)
        {
            const auto failure = error;
            if (!Rollback(record->artifacts)) error = failure + L" New artifacts that were changed or are in use were preserved.";
            return false;
        }
        created.ownership = std::move(record); item = std::move(created); error.clear(); return true;
    }

    bool RollbackCreated(CreatedItem& item, std::wstring& error)
    {
        if (!item.ownership) { error = L"This item has no creation ownership record; existing files were kept."; return false; }
        if (!Rollback(item.ownership->artifacts))
        { error = L"Some new artifacts changed or are in use and were preserved. No recursive removal was performed."; return false; }
        item.ownership.reset(); error.clear(); return true;
    }
}
