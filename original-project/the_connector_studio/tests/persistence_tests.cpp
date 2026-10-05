#include "SessionPersistence.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    void Rechecksum(std::vector<std::uint8_t>& bytes)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t i = 24; i < bytes.size(); ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
        for (unsigned i = 0; i < 8; ++i) bytes[16 + i] = static_cast<std::uint8_t>(hash >> (8 * i));
    }
    Lattice::SessionTab Sample()
    {
        Lattice::SessionTab tab;
        tab.title = L"Session \u65E5\u672C\u8A9E \U0001F680";
        tab.projectName = L"A project"; tab.projectPath = L"C:\\Projects\\\u0394";
        tab.eyebrow = L"SESSION 42"; tab.isAmber = true; tab.filesCount = 2;
        tab.scrollOffset = 123.5f; tab.draftText = L"Draft\r\nSecond line \U0001F680";
        tab.selectedModel = L"GPT-4.1"; tab.planBeforeEdits = false; tab.autoRunSafeTools = false;
        tab.attachedPaths = {L"C:\\Projects\\\u0394\\one.cpp", L"C:\\Projects\\\u0394\\two.md"};
        tab.messages = {{L"user", L"You", L"11:11 AM", std::wstring(2500, L'x') + L"\r\n\u0394", {}, {}},
            {L"assistant", L"Lattice", L"11:12 AM", L"Response \U0001F680", {L"Item one", L"Item\nTwo"},
                {{1, L"Title \u65E5", L"Description\nsecond line"}, {2, L"Next", L"Another description"}}}};
        return tab;
    }
    void SameData(const Lattice::SessionTab& lhs, const Lattice::SessionTab& rhs)
    {
        Check(lhs.title == rhs.title && lhs.eyebrow == rhs.eyebrow && lhs.isAmber == rhs.isAmber, "Session metadata did not survive.");
        Check(lhs.projectName == rhs.projectName && lhs.projectPath == rhs.projectPath && lhs.filesCount == rhs.filesCount, "Project metadata did not survive.");
        Check(lhs.scrollOffset == rhs.scrollOffset && lhs.draftText == rhs.draftText && lhs.attachedPaths == rhs.attachedPaths, "Draft, scroll, or paths did not survive.");
        Check(lhs.selectedModel == rhs.selectedModel && lhs.planBeforeEdits == rhs.planBeforeEdits && lhs.autoRunSafeTools == rhs.autoRunSafeTools, "Preferences did not survive.");
        Check(lhs.messages.size() == rhs.messages.size(), "Messages were dropped.");
        for (std::size_t i = 0; i < lhs.messages.size(); ++i)
        {
            const auto& a = lhs.messages[i]; const auto& b = rhs.messages[i];
            Check(a.role == b.role && a.author == b.author && a.time == b.time && a.text == b.text && a.items == b.items, "Message content was changed.");
            Check(a.planSteps.size() == b.planSteps.size(), "Plan steps were dropped.");
            for (std::size_t j = 0; j < a.planSteps.size(); ++j)
                Check(a.planSteps[j].number == b.planSteps[j].number && a.planSteps[j].title == b.planSteps[j].title && a.planSteps[j].description == b.planSteps[j].description, "Plan step was changed.");
        }
    }
}

int main()
{
    const auto directory = std::filesystem::current_path() / (L"persistence-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto file = directory / L"\u65E5\u672C\u8A9E.lattice";
    try
    {
        const auto original = Sample(); std::vector<std::uint8_t> bytes; std::wstring error;
        Check(Lattice::SessionPersistence::Serialize(original, bytes, error), "Serialize failed.");
        Lattice::SessionTab loaded; Check(Lattice::SessionPersistence::Deserialize(bytes, loaded, error), "Deserialize failed."); SameData(original, loaded);
        for (std::size_t length = 0; length < bytes.size(); ++length)
        {
            const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + length);
            loaded.title = L"Preserved";
            Check(!Lattice::SessionPersistence::Deserialize(truncated, loaded, error), "Truncated file was accepted.");
            Check(loaded.title == L"Preserved", "Failed load altered the current session.");
        }
        auto corrupt = bytes; corrupt.back() ^= 1;
        Check(!Lattice::SessionPersistence::Deserialize(corrupt, loaded, error), "Checksum damage was accepted.");
        corrupt = bytes; corrupt[8] = 99;
        Check(!Lattice::SessionPersistence::Deserialize(corrupt, loaded, error), "Unknown format version was accepted.");
        corrupt = bytes; for (unsigned i = 24; i < 28; ++i) corrupt[i] = 255; Rechecksum(corrupt);
        Check(!Lattice::SessionPersistence::Deserialize(corrupt, loaded, error), "Unbounded string was accepted.");
        corrupt = bytes; corrupt[28] = 0xc0; Rechecksum(corrupt);
        Check(!Lattice::SessionPersistence::Deserialize(corrupt, loaded, error), "Invalid UTF-8 was accepted.");
        corrupt = bytes; corrupt.push_back(0);
        Check(!Lattice::SessionPersistence::Deserialize(corrupt, loaded, error), "Trailing data was accepted.");
        auto invalid = original; invalid.messages[0].role = L"invalid";
        Check(!Lattice::SessionPersistence::Serialize(invalid, corrupt, error), "Invalid role was serialized.");
        invalid = original; invalid.draftText.assign(1, static_cast<wchar_t>(0xd800));
        Check(!Lattice::SessionPersistence::Serialize(invalid, corrupt, error), "Invalid UTF-16 was serialized.");

        std::filesystem::create_directory(directory);
        Check(Lattice::SessionPersistence::Save(file.wstring(), original, error), "Save failed.");
        Check(Lattice::SessionPersistence::Load(file.wstring(), loaded, error), "Load failed."); SameData(original, loaded);
        auto changed = original; changed.draftText += L" updated";
        Check(Lattice::SessionPersistence::Save(file.wstring(), changed, error), "Atomic replacement failed.");
        Check(!Lattice::SessionPersistence::Save(file.wstring(), invalid, error), "Invalid save succeeded.");
        Check(Lattice::SessionPersistence::Load(file.wstring(), loaded, error), "Previous file was lost after failed save."); SameData(changed, loaded);
        Check(!Lattice::SessionPersistence::Save(directory.wstring(), changed, error), "Directory was replaced by a session.");
        std::filesystem::remove(file); std::filesystem::remove(directory);
        std::cout << "Persistence round-trip, Unicode, corruption, bounds, and atomic save checks passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored; std::filesystem::remove(file, ignored); std::filesystem::remove(directory, ignored);
        std::cerr << exception.what() << '\n'; return 1;
    }
}
