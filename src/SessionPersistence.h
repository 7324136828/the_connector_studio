#pragma once
#include "AppTypes.h"
#include <cstdint>

namespace Lattice::SessionPersistence
{
    // Versioned UTF-8 session files. Loads never mutate output on failure.
    bool Serialize(const SessionTab& session, std::vector<std::uint8_t>& bytes, std::wstring& error);
    bool Deserialize(const std::vector<std::uint8_t>& bytes, SessionTab& session, std::wstring& error);
    bool Save(const std::wstring& path, const SessionTab& session, std::wstring& error);
    bool Load(const std::wstring& path, SessionTab& session, std::wstring& error);
}
