#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "ItemTypeRegistry.h"

namespace Lattice
{
    struct NewItemDialogOptions
    {
        ItemCategory initialCategory = ItemCategory::Projects;
        std::vector<ItemType> types;
        std::vector<std::wstring> recentProjects;
        std::wstring location;
        std::wstring projectLocation;
    };
    struct NewItemDialogCallbacks
    {
        std::function<bool(const NewItemRequest&, std::wstring&)> create;
        std::function<bool(const std::wstring&, std::wstring&)> openProject;
    };
    // Successful callbacks close the dialog. Failed callbacks leave the user's input
    // intact and display the returned error, allowing a corrected retry.
    bool ShowNewItemDialog(HWND owner, const NewItemDialogOptions& options,
        const NewItemDialogCallbacks& callbacks);

    // The same real HWND and native child controls are used by previews and tests.
    HWND CreateNewItemDialogWindow(HWND owner, const NewItemDialogOptions& options,
        const NewItemDialogCallbacks& callbacks, bool visible = false);
    bool CaptureNewItemDialog(HWND window, const std::wstring& path, std::wstring& error);
    bool SaveNewItemDialogPreview(const NewItemDialogOptions& options,
        const std::wstring& path, std::wstring& error);

    namespace NewItemDialogControls
    {
        inline constexpr int FilesTab = 101;
        inline constexpr int SessionsTab = 102;
        inline constexpr int ProjectsTab = 103;
        inline constexpr int Types = 104;
        inline constexpr int Name = 105;
        inline constexpr int Location = 106;
        inline constexpr int Browse = 107;
        inline constexpr int RecentProjects = 108;
        inline constexpr int OpenSelected = 109;
        inline constexpr int OpenExisting = 110;
        inline constexpr int Create = IDOK;
        inline constexpr int Cancel = IDCANCEL;
        inline constexpr int Close = 111;
    }
}
