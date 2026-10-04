#include "ItemTypeRegistry.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    std::filesystem::path TestRoot()
    {
        wchar_t buffer[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", buffer, static_cast<DWORD>(std::size(buffer)));
        return std::filesystem::weakly_canonical(length && length < std::size(buffer) ? std::filesystem::path(buffer) : std::filesystem::current_path());
    }
    std::string Read(const std::filesystem::path& path)
    { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
    void Write(const std::filesystem::path& path, const std::string& bytes)
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << bytes; Check(static_cast<bool>(file), "Fixture write failed."); }
}

int main()
{
    const auto root = TestRoot();
    const auto directory = root / (L"registry-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    bool created = false;
    const auto cleanup = [&] {
        if (created && directory.parent_path() == root)
        { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    };
    try
    {
        Check(std::filesystem::create_directory(directory), "Fixture directory already exists or is unavailable."); created = true;
        Lattice::ItemTypeRegistry registry;
        Check(registry.Types().size() == 3 && registry.Find(L"text-file") && registry.Find(L"work-session") && registry.Find(L"work-project"), "Default categories are incomplete.");
        std::wstring error;
        const Lattice::ItemType fileType{L"research-notes", L"Notes \u03a9 \u4e2d \U0001f680", L"Plain text at C:\\Notes", L".md", Lattice::ItemCategory::Files};
        Check(registry.RegisterType(fileType, error), "Valid custom file type rejected.");
        Check(registry.RegisterType({L"analysis-session", L"Analysis Session", L"Saved work session", L".lattice", Lattice::ItemCategory::Sessions}, error), "Valid custom session rejected.");
        Check(registry.RegisterType({L"analysis-project", L"Analysis Project", L"Project with files and sessions", L"", Lattice::ItemCategory::Projects}, error), "Valid custom project rejected.");
        const auto count = registry.Types().size();
        Check(!registry.RegisterType(fileType, error) && !error.empty(), "Duplicate registration replaced a type.");
        Check(!registry.RegisterType({L"text-file", L"Overwrite", L"", L".txt", Lattice::ItemCategory::Files}, error), "Built-in type was replaced.");
        Check(!registry.RegisterType({L"../unsafe", L"Unsafe", L"", L".txt", Lattice::ItemCategory::Files}, error), "Unsafe identifier accepted.");
        Check(!registry.RegisterType({L"unsafe-extension", L"Unsafe", L"", L".txt/other", Lattice::ItemCategory::Files}, error), "Unsafe extension accepted.");
        Check(!registry.RegisterType({L"wrong-session-format", L"Unsafe", L"", L".txt", Lattice::ItemCategory::Sessions}, error), "Unsupported session format accepted.");
        Check(!registry.RegisterType({L"project-extension", L"Unsafe", L"", L".exe", Lattice::ItemCategory::Projects}, error), "Project extension accepted.");
        Check(!registry.RegisterType({L"bad-category", L"Unsafe", L"", L".txt", static_cast<Lattice::ItemCategory>(99)}, error), "Unknown category accepted.");
        Check(!registry.RegisterType({L"empty-label", L"   ", L"", L".txt", Lattice::ItemCategory::Files}, error), "Blank type label accepted.");
        Check(!registry.RegisterType({L"bad-name", std::wstring(1, static_cast<wchar_t>(0xd800)), L"", L".txt", Lattice::ItemCategory::Files}, error), "Invalid Unicode accepted.");
        Check(registry.Types().size() == count, "Rejected registration mutated the registry.");

        const auto path = directory / L"item-types.registry";
        Check(registry.Save(path.wstring(), error), "Registry save failed.");
        Lattice::ItemTypeRegistry loaded;
        Check(loaded.Load(path.wstring(), error) && loaded.Types().size() == count, "Registrations did not round-trip.");
        const auto* notes = loaded.Find(fileType.id);
        Check(notes && notes->name == fileType.name && notes->description == fileType.description && notes->extension == fileType.extension, "Unicode or escaped path was lost.");
        const auto original = Read(path);
        Check(!registry.Save(directory.wstring(), error) && Read(path) == original, "Failed save changed existing registrations.");
        const auto malformed = directory / L"malformed.registry";
        for (const auto& bytes : {
            std::string("CONNECTOR_STUDIO_TYPES\t9\n"),
            std::string("CONNECTOR_STUDIO_TYPES\t1\n0\ttext-file\tDuplicate\t\t.txt\n"),
            std::string("CONNECTOR_STUDIO_TYPES\t1\n0\tcustom\tBad\\q\t\t.txt\n"),
            std::string("CONNECTOR_STUDIO_TYPES\t1\n0\tcustom\t") + static_cast<char>(0xff) + "\t\t.txt\n"})
        {
            Write(malformed, bytes);
            Check(!loaded.Load(malformed.wstring(), error) && loaded.Types().size() == count && loaded.Find(fileType.id), "Invalid registry load destroyed existing registrations.");
        }
        cleanup();
        std::cout << "Type registration, persistence, Unicode, validation and failure isolation passed.\n";
        return 0;
    }
    catch (const std::exception& failure)
    { cleanup(); std::cerr << failure.what() << '\n'; return 1; }
}
