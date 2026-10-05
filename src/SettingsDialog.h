#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include "ConnectorClient.h"

namespace Lattice
{
    using SaveConnectorSettings = std::function<bool(const Connector::Settings&, std::wstring&)>;
    // Test Connection is asynchronous and does not persist settings. Save invokes
    // the callback with a normalized URL; failed saves preserve input for retry.
    bool ShowSettingsDialog(HWND owner, const Connector::Settings& settings,
        const SaveConnectorSettings& save);
    HWND CreateSettingsDialogWindow(HWND owner, const Connector::Settings& settings,
        const SaveConnectorSettings& save, bool visible = false);
    bool CaptureSettingsDialog(HWND window, const std::wstring& path, std::wstring& error);
    bool SaveSettingsDialogPreview(const Connector::Settings& settings,
        const std::wstring& path, std::wstring& error);

    namespace SettingsDialogControls
    {
        inline constexpr int ServerUrl = 201;
        inline constexpr int TestConnection = 202;
        inline constexpr int Status = 203;
        inline constexpr int ServerUrlLabel = 204;
        inline constexpr int Example = 205;
        inline constexpr int ModelHelp = 206;
        inline constexpr int Save = IDOK;
        inline constexpr int Cancel = IDCANCEL;
        inline constexpr int Close = 207;
        inline constexpr UINT_PTR PollTimer = 208;
        inline constexpr int ChatFontSize = 209;
        inline constexpr int ChatFontSizeLabel = 210;
    }
}
