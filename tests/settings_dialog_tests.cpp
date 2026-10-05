#include "MockConnectorServer.h"
#include "SettingsDialog.h"
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace Lattice;
namespace
{
    using namespace SettingsDialogControls;
    using Microsoft::WRL::ComPtr;
    int checks = 0;
    void Check(bool condition, const char* message)
    { ++checks; if (!condition) throw std::runtime_error(message); }
    std::wstring Read(HWND window)
    {
        const int length = GetWindowTextLengthW(window);
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        value.resize(GetWindowTextW(window, value.data(), length + 1)); return value;
    }
    void Click(HWND window, int id)
    { SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, id))); }
    bool ShownStyle(HWND window)
    { return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0; }
    RECT ChildRect(HWND window, int id)
    {
        RECT rect{}; GetWindowRect(GetDlgItem(window, id), &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2); return rect;
    }
    struct Raster
    {
        UINT width = 0, height = 0;
        std::vector<BYTE> pixels;
        explicit Raster(const std::wstring& path)
        {
            ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
            Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "capture decoder must initialize");
            Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)), "native settings dialog capture must decode");
            Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)), "native settings dialog capture must have dimensions");
            Check(SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "native settings dialog capture must have readable pixels");
            pixels.resize(static_cast<size_t>(width) * height * 4);
            Check(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "native settings dialog pixels must copy");
        }
        int TextPixels(RECT area) const
        {
            int count = 0;
            for (LONG y = area.top; y < area.bottom; ++y)
                for (LONG x = area.left; x < area.right; ++x)
                {
                    const auto at = (static_cast<size_t>(y) * width + x) * 4;
                    // Native status text can be green or red. Check brightness
                    // against the dark background rather than requiring gray RGB.
                    const int brightness = 54 * pixels[at + 2] + 183 * pixels[at + 1] + 19 * pixels[at];
                    if (brightness > 110 * 256) ++count;
                }
            return count;
        }
        int ErrorPixels(float scale) const
        {
            int count = 0;
            const int top = static_cast<int>(height - 119 * scale), bottom = static_cast<int>(height - 76 * scale);
            for (int y = top; y < bottom; ++y)
                for (UINT x = 24; x + 24 < width; ++x)
                {
                    const auto at = (static_cast<size_t>(y) * width + x) * 4;
                    if (pixels[at + 2] > 150 && pixels[at + 2] > pixels[at + 1] + 40) ++count;
                }
            return count;
        }
    };
    std::filesystem::path TestRoot()
    {
        wchar_t buffer[32768]{};
        const DWORD copied = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", buffer, static_cast<DWORD>(std::size(buffer)));
        const auto root = copied > 0 && copied < std::size(buffer) ? std::filesystem::path(buffer) :
            std::filesystem::current_path() / L"test-workspaces" / L"settings_dialog_tests";
        std::filesystem::create_directories(root); return std::filesystem::absolute(root);
    }
    void PollUntilIdle(HWND window, int timeout = 4000)
    {
        const auto deadline = GetTickCount64() + timeout;
        while (IsWindow(window) && !IsWindowEnabled(GetDlgItem(window, TestConnection)) && GetTickCount64() < deadline)
        { SendMessageW(window, WM_TIMER, PollTimer, 0); Sleep(5); }
        Check(IsWindow(window) && IsWindowEnabled(GetDlgItem(window, TestConnection)), "async connection tests must finish and enable retry");
    }
    void TestValidationCaptureAndDpi(const std::filesystem::path& root)
    {
        Connector::Settings original; Connector::Settings persisted = original;
        int saves = 0; bool accept = false;
        const SaveConnectorSettings callback = [&](const Connector::Settings& settings, std::wstring& error) {
            ++saves;
            if (!accept) { error = L"The settings could not be written. Choose a valid location and retry."; return false; }
            persisted = settings; return true;
        };
        const HWND window = CreateSettingsDialogWindow(nullptr, original, callback);
        Check(window != nullptr && !ShownStyle(window), "settings regressions must use hidden native windows");
        Check((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == 0, "settings must not show a duplicate native caption");
        Check(Read(GetDlgItem(window, ServerUrl)) == original.serverUrl, "settings must show the actual configured URL");
        Check(Read(GetDlgItem(window, Example)).find(L"http://127.0.0.1:8301") != std::wstring::npos, "settings must explain the server-root URL");
        Check(GetNextDlgTabItem(window, GetDlgItem(window, ServerUrl), FALSE) == GetDlgItem(window, ChatFontSize), "keyboard navigation must reach the chat font selector");
        Check(GetNextDlgTabItem(window, GetDlgItem(window, ChatFontSize), FALSE) == GetDlgItem(window, TestConnection), "keyboard navigation must reach Test Connection after the font selector");
        Check(SendDlgItemMessageW(window, ChatFontSize, CB_GETCOUNT, 0, 0) == 7 && SendDlgItemMessageW(window, ChatFontSize, CB_GETCURSEL, 0, 0) == 0,
            "settings must offer the original chat font size and six larger presets");
        Check(GetNextDlgTabItem(window, GetDlgItem(window, TestConnection), FALSE) == GetDlgItem(window, Cancel), "keyboard navigation must reach Cancel");
        std::wstring error;
        const auto preview = root / L"settings-default.png";
        Check(CaptureSettingsDialog(window, preview.wstring(), error), "settings must capture the real native HWND");
        const Raster initial(preview.wstring());
        Check(initial.TextPixels(ChildRect(window, ServerUrl)) > 30, "settings capture must include actual native URL text");
        Check(initial.TextPixels(ChildRect(window, ChatFontSize)) > 20, "settings capture must include the actual selected chat font size");
        SendDlgItemMessageW(window, ChatFontSize, CB_SETCURSEL, 4, 0);
        SetDlgItemTextW(window, ServerUrl, L"ftp://127.0.0.1:8301");
        Click(window, Save);
        Check(saves == 0 && IsWindow(window) && persisted.serverUrl == original.serverUrl, "invalid URL save must validate without calling persistence");
        Click(window, TestConnection);
        Check(IsWindowEnabled(GetDlgItem(window, TestConnection)) && Read(GetDlgItem(window, Status)).find(L"HTTP") != std::wstring::npos, "invalid test addresses must fail immediately with inline validation");
        SetDlgItemTextW(window, ServerUrl, L"  HTTPS://127.0.0.1:8302/  ");
        SendDlgItemMessageW(window, ServerUrl, WM_IME_STARTCOMPOSITION, 0, 0);
        SendDlgItemMessageW(window, ServerUrl, WM_KEYDOWN, VK_RETURN, 0);
        SendDlgItemMessageW(window, ServerUrl, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(IsWindow(window) && saves == 0, "IME candidate Enter/Escape must not save or close settings");
        SendDlgItemMessageW(window, ServerUrl, WM_IME_ENDCOMPOSITION, 0, 0);
        SendDlgItemMessageW(window, ServerUrl, WM_KEYDOWN, VK_RETURN, 0);
        Check(saves == 1 && IsWindow(window) && Read(GetDlgItem(window, Status)).find(L"could not be written") != std::wstring::npos, "failed persistence must show the callback error and permit retry");
        Check(Read(GetDlgItem(window, ServerUrl)) == L"  HTTPS://127.0.0.1:8302/  ", "failed saves must preserve typed URL input");
        Check(persisted.serverUrl == original.serverUrl && persisted.chatFontSize == original.chatFontSize &&
            SendDlgItemMessageW(window, ChatFontSize, CB_GETCURSEL, 0, 0) == 4,
            "failed saves must preserve the selected font without altering committed settings");
        Check(CaptureSettingsDialog(window, (root / L"settings-validation.png").wstring(), error), "inline settings errors must capture");

        RECT dpiRect{0, 0, 960, 630};
        SendMessageW(window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&dpiRect));
        const auto input = ChildRect(window, ServerUrl);
        Check(input.left == 51 && input.top == 171, "native URL edit must use the same DPI as its dialog");
        const auto font = reinterpret_cast<HFONT>(SendDlgItemMessageW(window, ServerUrl, WM_GETFONT, 0, 0));
        LOGFONTW logical{}; Check(GetObjectW(font, sizeof(logical), &logical) && logical.lfHeight == -20, "settings text must scale with DPI");
        SetWindowPos(window, nullptr, 0, 0, 780, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{}; GetClientRect(window, &client);
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!ShownStyle(child)) continue;
            const auto rect = ChildRect(window, GetDlgCtrlID(child));
            Check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom, "compact settings controls must remain inside the native window");
        }
        Check(ChildRect(window, Status).bottom < ChildRect(window, Save).top, "status must not overlap the compact footer buttons");
        Check(CaptureSettingsDialog(window, (root / L"settings-compact.png").wstring(), error), "compact settings must capture for visual review");
        accept = true; Click(window, Save);
        Check(!IsWindow(window) && saves == 2 && persisted.serverUrl == L"https://127.0.0.1:8302" && persisted.chatFontSize == 18.0f, "successful Save must commit the normalized URL and selected font and close");

        const HWND canceled = CreateSettingsDialogWindow(nullptr, original, callback);
        SetDlgItemTextW(canceled, ServerUrl, L"https://127.0.0.1:9000");
        SendDlgItemMessageW(canceled, ChatFontSize, CB_SETCURSEL, 6, 0);
        SendDlgItemMessageW(canceled, ServerUrl, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!IsWindow(canceled) && saves == 2 && persisted.serverUrl == L"https://127.0.0.1:8302" && persisted.chatFontSize == 18.0f, "Cancel must leave the committed URL and chat font size untouched");
        Connector::Settings custom = original; custom.chatFontSize = 13.25f;
        const auto customWindow = CreateSettingsDialogWindow(nullptr, custom, [&](const Connector::Settings& value, std::wstring&) {
            Check(value.chatFontSize == 13.25f, "saving URL changes must preserve a valid custom configured font size"); return true;
        });
        Check(SendDlgItemMessageW(customWindow, ChatFontSize, CB_GETCOUNT, 0, 0) == 8 && SendDlgItemMessageW(customWindow, ChatFontSize, CB_GETCURSEL, 0, 0) == 7,
            "a valid custom configured size must be displayed and selectable without rounding it on Save");
        Click(customWindow, Save);
        Check(!IsWindow(customWindow), "custom font settings must save through the native selector");
    }
    void TestHttpDiscoveryAndCancellation(const std::filesystem::path& root)
    {
        using namespace ConnectorTest;
        MockConnectorServer server([](const Request& request) {
            if (request.path == "/api/health") return Response{503, R"({"status":"speech unavailable"})", 0, {}};
            auto response = MockConnectorServer::Default(request); response.delayMs = 400; return response;
        });
        Connector::Settings settings; settings.serverUrl = server.Url();
        Connector::Settings committed = settings; int saves = 0;
        const SaveConnectorSettings save = [&](const Connector::Settings& value, std::wstring&) { ++saves; committed = value; return true; };
        HWND window = CreateSettingsDialogWindow(nullptr, settings, save);
        SendDlgItemMessageW(window, ChatFontSize, CB_SETCURSEL, 5, 0);
        Check(window != nullptr, "HTTP settings dialog must be created");
        const auto started = GetTickCount64(); Click(window, TestConnection);
        Check(GetTickCount64() - started < 250, "native Test Connection must return without waiting for HTTP");
        Check(!IsWindowEnabled(GetDlgItem(window, TestConnection)) && IsWindowEnabled(GetDlgItem(window, Save)), "running connection tests must allow saving and disable duplicate tests");
        Check(Read(GetDlgItem(window, Status)).find(L"Testing") != std::wstring::npos, "running tests must show inline progress");
        PollUntilIdle(window);
        Check(Read(GetDlgItem(window, Status)).find(L"2 active models") != std::wstring::npos, "successful discovery must report the server's actual enabled model count");
        Check(saves == 0 && committed.serverUrl == settings.serverUrl && committed.chatFontSize == settings.chatFontSize &&
            SendDlgItemMessageW(window, ChatFontSize, CB_GETCURSEL, 0, 0) == 5,
            "Test Connection must preserve the candidate font selection without persisting or applying it");
        const auto requests = server.Requests();
        Check(requests.size() == 1 && requests[0].method == "GET" && requests[0].path == "/v1/models" && requests[0].body.empty(), "connection testing must use GET models without requiring speech health or sending a chat");
        std::wstring error;
        Check(CaptureSettingsDialog(window, (root / L"settings-connected.png").wstring(), error), "connected native status must capture");
        const Raster connected((root / L"settings-connected.png").wstring());
        Check(connected.TextPixels(ChildRect(window, Status)) > 20, "capture must include the actual native result status");

        server.SetHandler([](const Request&) { return Response{200, R"({"object":"list","data":[]})", 0, {}}; });
        Click(window, TestConnection); PollUntilIdle(window);
        Check(Read(GetDlgItem(window, Status)).find(L"no active models") != std::wstring::npos && IsWindowEnabled(GetDlgItem(window, Save)), "empty successful discovery must explain no active models and still permit Save");
        Check(saves == 0, "empty model tests must not write settings");
        Click(window, Save);
        Check(!IsWindow(window) && saves == 1 && committed.serverUrl == server.Url() && committed.chatFontSize == 20.0f, "a valid URL and font size must be saveable even if the server has no active models");

        window = CreateSettingsDialogWindow(nullptr, settings, save);
        server.SetHandler([](const Request&) { return Response{503, R"({"error":{"message":"temporary fixture unavailable"}})", 0, {}}; });
        Click(window, TestConnection); PollUntilIdle(window);
        Check(Read(GetDlgItem(window, Status)).find(L"temporary fixture unavailable") != std::wstring::npos && IsWindowEnabled(GetDlgItem(window, TestConnection)), "HTTP errors must remain inline and allow another connection test");
        Check(saves == 1, "HTTP errors must never call the save callback");

        MockConnectorServer replacement;
        server.SetHandler([](const Request&) { return Response{200, R"({"data":[{"id":"late-model"}]})", 450, {}}; });
        const auto requestCount = server.RequestCount(); Click(window, TestConnection);
        Check(server.WaitForRequests(requestCount + 1), "cancellation test must reach its owned HTTP server");
        SetDlgItemTextW(window, ServerUrl, replacement.Url().c_str());
        Check(IsWindowEnabled(GetDlgItem(window, TestConnection)), "editing the URL must cancel and release the old connection test");
        Click(window, TestConnection); PollUntilIdle(window);
        const auto replacementStatus = Read(GetDlgItem(window, Status));
        Check(replacementStatus.find(L"2 active models") != std::wstring::npos && replacement.WaitForRequests(1), "the edited URL must use its own server and result");
        Sleep(500); SendMessageW(window, WM_TIMER, PollTimer, 0);
        Check(Read(GetDlgItem(window, Status)) == replacementStatus && Read(GetDlgItem(window, ServerUrl)) == replacement.Url(), "a late canceled discovery must never overwrite the new address or status");
        Check(saves == 1 && committed.serverUrl == settings.serverUrl, "URL edits and connection tests must leave persisted settings untouched");

        replacement.SetHandler([](const Request&) { return Response{200, R"({"data":[]})", 1500, {}}; });
        const auto replacementCount = replacement.RequestCount(); Click(window, TestConnection);
        Check(replacement.WaitForRequests(replacementCount + 1), "close cancellation must begin a real pending request");
        const auto closing = GetTickCount64(); SendMessageW(window, WM_CLOSE, 0, 0);
        Check(!IsWindow(window) && GetTickCount64() - closing < 250, "closing a pending test must cancel safely without waiting on network response");
        Check(saves == 1, "closing a pending test must not save settings");
        window = CreateSettingsDialogWindow(nullptr, settings, save);
        const auto fresh = Read(GetDlgItem(window, Status));
        Sleep(100); SendMessageW(window, WM_TIMER, PollTimer, 0);
        Check(Read(GetDlgItem(window, Status)) == fresh && IsWindowEnabled(GetDlgItem(window, TestConnection)), "late worker completion must not access a closed or replacement HWND");
        DestroyWindow(window);
    }

    struct ModalScript
    {
        HWND owner = nullptr;
        bool accept = false, ownerDisabled = false, stayedHidden = false;
    };
    ModalScript* currentScript = nullptr;
    constexpr UINT ModalAction = WM_APP + 50;
    LRESULT CALLBACK ModalProbe(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto& script = *reinterpret_cast<ModalScript*>(data);
        if (message == WM_WINDOWPOSCHANGING)
        {
            auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
            if (position->flags & SWP_SHOWWINDOW) { position->flags &= ~SWP_SHOWWINDOW; position->flags |= SWP_HIDEWINDOW; }
        }
        if (message == ModalAction)
        {
            script.ownerDisabled = !IsWindowEnabled(script.owner); script.stayedHidden = !ShownStyle(window);
            if (script.accept) { SendDlgItemMessageW(window, ServerUrl, WM_KEYDOWN, VK_RETURN, 0); }
            else Click(window, Cancel);
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
    LRESULT CALLBACK ModalHook(int code, WPARAM wParam, LPARAM lParam)
    {
        wchar_t className[128]{};
        const HWND window = reinterpret_cast<HWND>(wParam);
        if (code >= 0 && currentScript && GetClassNameW(window, className, static_cast<int>(std::size(className))) &&
            std::wstring(className) == L"ConnectorStudioSettingsDialog")
        {
            if (code == HCBT_CREATEWND)
            {
                SetWindowSubclass(window, ModalProbe, 77, reinterpret_cast<DWORD_PTR>(currentScript));
                PostMessageW(window, ModalAction, 0, 0);
            }
            if (code == HCBT_ACTIVATE) return 1;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    void TestModalOwner(const std::filesystem::path& root)
    {
        const HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Own modal regression owner", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(owner != nullptr, "modal test owner must be created");
        Connector::Settings settings;
        (void)root;
        int calls = 0; const SaveConnectorSettings save = [&](const Connector::Settings&, std::wstring&) { ++calls; return true; };
        ModalScript script{owner}; currentScript = &script;
        const HHOOK hook = SetWindowsHookExW(WH_CBT, ModalHook, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "thread-local hidden modal probe must install");
        const HWND foreground = GetForegroundWindow();
        const bool canceled = ShowSettingsDialog(owner, settings, save);
        Check(!canceled && calls == 0 && script.ownerDisabled && script.stayedHidden, "Cancel must leave app state untouched while the modal owner was disabled");
        Check(IsWindowEnabled(owner) && GetForegroundWindow() == foreground, "modal cleanup must restore the owner without activating test windows");
        script.accept = true;
        Check(ShowSettingsDialog(owner, settings, save) && calls == 1 && script.ownerDisabled && script.stayedHidden, "modal success must reflect an accepted callback and remain hidden in tests");
        Check(IsWindowEnabled(owner), "successful modal closure must restore the owner");
        EnableWindow(owner, FALSE); script.accept = false;
        Check(!ShowSettingsDialog(owner, settings, save) && !IsWindowEnabled(owner), "an owner already disabled before opening must remain disabled");
        UnhookWindowsHookEx(hook); currentScript = nullptr; DestroyWindow(owner);
    }
}
int main()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    try
    {
        const auto root = TestRoot();
        TestValidationCaptureAndDpi(root); TestHttpDiscoveryAndCancellation(root); TestModalOwner(root);
        std::cout << checks << " native settings-dialog checks passed.\n";
        CoUninitialize(); return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Settings dialog regression: " << error.what() << '\n';
        CoUninitialize(); return 1;
    }
}
