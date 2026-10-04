#include "SessionPersistence.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Lattice::SessionPersistence
{
    namespace
    {
        constexpr std::uint32_t Version = 1;
        constexpr std::size_t MaxFileBytes = 32 * 1024 * 1024;
        constexpr std::size_t MaxStringBytes = 4 * 1024 * 1024;
        constexpr std::uint32_t MaxCollection = 10000;
        constexpr std::uint8_t Magic[] = {'L', 'A', 'T', 'T', 'I', 'C', 'E', 0};
        constexpr std::size_t HeaderSize = 24;

        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        };

        std::uint64_t Checksum(const std::uint8_t* data, std::size_t size)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        struct Writer
        {
            std::vector<std::uint8_t> bytes;
            void U32(std::uint32_t value)
            {
                for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }
            void U64(std::uint64_t value)
            {
                U32(static_cast<std::uint32_t>(value));
                U32(static_cast<std::uint32_t>(value >> 32));
            }
            void Count(std::size_t size)
            {
                if (size > MaxCollection) throw std::runtime_error("Too many session entries.");
                U32(static_cast<std::uint32_t>(size));
            }
            void String(const std::wstring& text)
            {
                if (text.size() > MaxStringBytes || text.find(L'\0') != std::wstring::npos)
                    throw std::runtime_error("Invalid session text.");
                int count = text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
                if ((!text.empty() && count == 0) || static_cast<std::size_t>(count) > MaxStringBytes)
                    throw std::runtime_error("Invalid Unicode text.");
                U32(static_cast<std::uint32_t>(count));
                const auto start = bytes.size();
                if (start + static_cast<std::size_t>(count) > MaxFileBytes) throw std::runtime_error("Session is too large.");
                bytes.resize(start + count);
                if (count && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                    static_cast<int>(text.size()), reinterpret_cast<char*>(bytes.data() + start), count, nullptr, nullptr))
                    throw std::runtime_error("Invalid Unicode text.");
            }
        };

        struct Reader
        {
            const std::vector<std::uint8_t>& bytes;
            std::size_t pos = 0;
            void Require(std::size_t count) const
            {
                if (count > bytes.size() - pos) throw std::runtime_error("Incomplete session file.");
            }
            std::uint32_t U32()
            {
                Require(4);
                std::uint32_t value = 0;
                for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[pos++]) << (8 * i);
                return value;
            }
            std::uint64_t U64()
            {
                const std::uint64_t low = U32();
                return low | (static_cast<std::uint64_t>(U32()) << 32);
            }
            std::uint32_t Count()
            {
                const auto count = U32();
                if (count > MaxCollection) throw std::runtime_error("Too many session entries.");
                return count;
            }
            std::wstring String()
            {
                const auto count = U32();
                if (count > MaxStringBytes) throw std::runtime_error("Session text is too large.");
                Require(count);
                if (count == 0) return {};
                const char* data = reinterpret_cast<const char*>(bytes.data() + pos);
                const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(count), nullptr, 0);
                if (length == 0) throw std::runtime_error("Invalid UTF-8 session text.");
                std::wstring value(static_cast<std::size_t>(length), L'\0');
                if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(count), value.data(), length)
                    || value.find(L'\0') != std::wstring::npos) throw std::runtime_error("Invalid session text.");
                pos += count;
                return value;
            }
            bool Boolean()
            {
                const auto value = U32();
                if (value > 1) throw std::runtime_error("Invalid session flag.");
                return value != 0;
            }
        };

        void Validate(const SessionTab& tab)
        {
            if (tab.title.empty() || tab.filesCount < 0 || tab.filesCount > static_cast<int>(MaxCollection)
                || !std::isfinite(tab.scrollOffset) || tab.scrollOffset < 0)
                throw std::runtime_error("Invalid session metadata.");
            for (const auto& message : tab.messages)
            {
                if (message.role != L"user" && message.role != L"assistant") throw std::runtime_error("Invalid message role.");
                for (const auto& step : message.planSteps)
                    if (step.number < 1 || step.number > static_cast<int>(MaxCollection)) throw std::runtime_error("Invalid plan step.");
            }
        }

        std::wstring Failure(const char* detail)
        {
            std::wstring text = L"The session could not be read or saved. ";
            while (*detail) text.push_back(static_cast<unsigned char>(*detail++));
            return text;
        }
    }

    bool Serialize(const SessionTab& session, std::vector<std::uint8_t>& bytes, std::wstring& error)
    {
        try
        {
            Validate(session);
            Writer payload;
            payload.String(session.title);
            payload.U32(session.isAmber ? 1 : 0);
            payload.String(session.eyebrow);
            payload.String(session.projectName);
            payload.String(session.projectPath);
            payload.U32(static_cast<std::uint32_t>(session.filesCount));
            std::uint32_t scrollBits = 0;
            static_assert(sizeof(scrollBits) == sizeof(session.scrollOffset));
            std::memcpy(&scrollBits, &session.scrollOffset, sizeof(scrollBits));
            payload.U32(scrollBits);
            payload.String(session.draftText);
            payload.String(session.selectedModel);
            payload.U32(session.planBeforeEdits ? 1 : 0);
            payload.U32(session.autoRunSafeTools ? 1 : 0);
            payload.Count(session.attachedPaths.size());
            for (const auto& path : session.attachedPaths) payload.String(path);
            payload.Count(session.messages.size());
            for (const auto& message : session.messages)
            {
                payload.String(message.role);
                payload.String(message.author);
                payload.String(message.time);
                payload.String(message.text);
                payload.Count(message.items.size());
                for (const auto& item : message.items) payload.String(item);
                payload.Count(message.planSteps.size());
                for (const auto& step : message.planSteps)
                {
                    payload.U32(static_cast<std::uint32_t>(step.number));
                    payload.String(step.title);
                    payload.String(step.description);
                }
            }
            if (payload.bytes.size() > MaxFileBytes - HeaderSize) throw std::runtime_error("Session is too large.");
            Writer file;
            file.bytes.insert(file.bytes.end(), std::begin(Magic), std::end(Magic));
            file.U32(Version);
            file.U32(static_cast<std::uint32_t>(payload.bytes.size()));
            file.U64(Checksum(payload.bytes.data(), payload.bytes.size()));
            file.bytes.insert(file.bytes.end(), payload.bytes.begin(), payload.bytes.end());
            bytes = std::move(file.bytes);
            error.clear();
            return true;
        }
        catch (const std::exception& exception) { error = Failure(exception.what()); return false; }
    }

    bool Deserialize(const std::vector<std::uint8_t>& bytes, SessionTab& session, std::wstring& error)
    {
        try
        {
            if (bytes.size() < HeaderSize || bytes.size() > MaxFileBytes
                || !std::equal(std::begin(Magic), std::end(Magic), bytes.begin()))
                throw std::runtime_error("Choose a Connector Studio session (.lattice), not a text export.");
            Reader reader{bytes, sizeof(Magic)};
            if (reader.U32() != Version) throw std::runtime_error("Unsupported session format version.");
            if (reader.U32() != bytes.size() - HeaderSize) throw std::runtime_error("Incomplete session file.");
            const auto checksum = reader.U64();
            if (checksum != Checksum(bytes.data() + HeaderSize, bytes.size() - HeaderSize))
                throw std::runtime_error("Session checksum does not match; the file may be damaged.");
            SessionTab loaded;
            loaded.title = reader.String();
            loaded.isAmber = reader.Boolean();
            loaded.eyebrow = reader.String();
            loaded.projectName = reader.String();
            loaded.projectPath = reader.String();
            loaded.filesCount = static_cast<int>(reader.Count());
            const auto scrollBits = reader.U32();
            std::memcpy(&loaded.scrollOffset, &scrollBits, sizeof(scrollBits));
            loaded.draftText = reader.String();
            loaded.selectedModel = reader.String();
            loaded.planBeforeEdits = reader.Boolean();
            loaded.autoRunSafeTools = reader.Boolean();
            auto count = reader.Count();
            for (std::uint32_t i = 0; i < count; ++i) loaded.attachedPaths.push_back(reader.String());
            count = reader.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                MessageItem message;
                message.role = reader.String();
                message.author = reader.String();
                message.time = reader.String();
                message.text = reader.String();
                auto entries = reader.Count();
                for (std::uint32_t j = 0; j < entries; ++j) message.items.push_back(reader.String());
                entries = reader.Count();
                for (std::uint32_t j = 0; j < entries; ++j)
                {
                    PlanStep step;
                    step.number = static_cast<int>(reader.Count());
                    step.title = reader.String();
                    step.description = reader.String();
                    message.planSteps.push_back(std::move(step));
                }
                loaded.messages.push_back(std::move(message));
            }
            if (reader.pos != bytes.size()) throw std::runtime_error("Unexpected trailing session data.");
            Validate(loaded);
            session = std::move(loaded);
            error.clear();
            return true;
        }
        catch (const std::exception& exception) { error = Failure(exception.what()); return false; }
    }

    bool Save(const std::wstring& path, const SessionTab& session, std::wstring& error)
    {
        std::vector<std::uint8_t> bytes;
        if (!Serialize(session, bytes, error)) return false;
        if (path.empty()) { error = L"Choose a session file path."; return false; }
        std::wstring temporary;
        Handle file;
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
            file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
        }
        if (file.value == INVALID_HANDLE_VALUE) { error = L"Cannot create a session file in this folder. Check the path and write permissions."; return false; }
        DWORD written = 0;
        bool success = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            && written == bytes.size() && FlushFileBuffers(file.value);
        CloseHandle(file.value);
        file.value = INVALID_HANDLE_VALUE;
        if (success) success = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!success)
        {
            DeleteFileW(temporary.c_str());
            error = L"The session could not be saved. The previous file was kept. Check free disk space and write permissions.";
            return false;
        }
        error.clear();
        return true;
    }

    bool Load(const std::wstring& path, SessionTab& session, std::wstring& error)
    {
        Handle file;
        file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) { error = L"The session file could not be opened. Check that it exists and is readable."; return false; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart < static_cast<LONGLONG>(HeaderSize)
            || size.QuadPart > static_cast<LONGLONG>(MaxFileBytes))
        { error = L"Session files must be complete and smaller than 32 MB."; return false; }
        try
        {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            { error = L"The session file could not be read completely."; return false; }
            return Deserialize(bytes, session, error);
        }
        catch (const std::exception& exception) { error = Failure(exception.what()); return false; }
    }
}
