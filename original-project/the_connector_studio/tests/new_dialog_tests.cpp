#include "NewItemDialog.h"
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
    using namespace NewItemDialogControls;
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
    std::wstring ListText(HWND list, int index)
    {
        const auto length = SendMessageW(list, LB_GETTEXTLEN, index, 0);
        Check(length != LB_ERR, "type list must contain a real item");
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const auto copied = SendMessageW(list, LB_GETTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(copied)); return text;
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
            Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)), "native dialog capture must decode");
            Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)), "native dialog capture must have dimensions");
            Check(SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "native dialog capture must have readable pixels");
            pixels.resize(static_cast<size_t>(width) * height * 4);
            Check(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "native dialog pixels must copy");
        }
        int TextPixels(RECT area) const
        {
            int count = 0;
            for (LONG y = area.top; y < area.bottom; ++y)
                for (LONG x = area.left; x < area.right; ++x)
                {
                    const auto at = (static_cast<size_t>(y) * width + x) * 4;
                    if (pixels[at] > 110 && pixels[at + 1] > 110 && pixels[at + 2] > 110) ++count;
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
            std::filesystem::current_path() / L"test-workspaces" / L"new_dialog_tests";
        std::filesystem::create_directories(root); return std::filesystem::absolute(root);
    }
    void TestControlsAndRetry(const std::filesystem::path& root)
    {
        ItemTypeRegistry registry;
        NewItemDialogOptions options;
        options.types = registry.Types(); options.location = root.wstring();
        options.projectLocation = (root / L"chosen-project").wstring();
        int calls = 0; bool accept = false; NewItemRequest received{};
        NewItemDialogCallbacks callbacks;
        callbacks.create = [&](const NewItemRequest& request, std::wstring& error) {
            ++calls; received = request; error = L"This folder cannot be used. Choose another location."; return accept;
        };
        callbacks.openProject = [](const std::wstring&, std::wstring&) { return false; };
        const HWND owner = CreateWindowExW(0, L"STATIC", L"Native dialog test owner", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(owner != nullptr, "hidden owner must be created");
        const HWND window = CreateNewItemDialogWindow(owner, options, callbacks);
        Check(window != nullptr && !ShownStyle(window), "native dialog tests must stay hidden");
        Check(GetWindow(window, GW_OWNER) == owner, "new dialog must have its app owner");
        Check((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == 0, "native dialog must use the custom dark header");
        Check(Read(GetDlgItem(window, FilesTab)) == L"Files" && Read(GetDlgItem(window, SessionsTab)) == L"Sessions" && Read(GetDlgItem(window, ProjectsTab)) == L"Projects", "dialog must expose exactly the three category tabs");
        Check(SendDlgItemMessageW(window, Types, LB_GETCOUNT, 0, 0) == 1 && ListText(GetDlgItem(window, Types), 0) == L"Work Project", "new project must select the registered project type");
        Check(Read(GetDlgItem(window, Name)).empty(), "new dialog must not inject a sample name");
        Check(SendDlgItemMessageW(window, RecentProjects, LB_GETCOUNT, 0, 0) == 0 && !IsWindowEnabled(GetDlgItem(window, OpenSelected)), "recent projects must be empty until real projects exist");
        Click(window, Create);
        Check(calls == 0 && IsWindow(window), "missing names must validate before invoking creation");
        std::wstring error;
        const auto baseline = root / L"projects-blank.png";
        Check(CaptureNewItemDialog(window, baseline.wstring(), error), "real HWND project dialog must capture");
        const Raster before(baseline.wstring());
        Check(before.TextPixels(ChildRect(window, ProjectsTab)) > 20 && before.TextPixels(ChildRect(window, Types)) > 20, "capture must include native tab and list text");
        const std::wstring name = L"  Work \u03a9 \u4e2d  ";
        SetDlgItemTextW(window, Name, name.c_str());
        const auto typed = root / L"projects-typed.png";
        Check(CaptureNewItemDialog(window, typed.wstring(), error), "native edit content must capture");
        const Raster after(typed.wstring());
        Check(after.TextPixels(ChildRect(window, Name)) > before.TextPixels(ChildRect(window, Name)) + 10, "capture must include actual native name edit text");
        SendDlgItemMessageW(window, Name, WM_IME_STARTCOMPOSITION, 0, 0);
        SendDlgItemMessageW(window, Name, WM_KEYDOWN, VK_RETURN, 0);
        Check(calls == 0 && IsWindow(window), "Enter must select an IME candidate without invoking item creation");
        SendDlgItemMessageW(window, Name, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(calls == 0 && IsWindow(window), "Escape must dismiss IME composition without canceling the dialog");
        SendDlgItemMessageW(window, Name, WM_IME_ENDCOMPOSITION, 0, 0);
        SendDlgItemMessageW(window, Name, WM_KEYDOWN, VK_RETURN, 0);
        Check(calls == 1 && IsWindow(window) && received.name == L"Work \u03a9 \u4e2d" && received.location == root.wstring(), "Enter must submit trimmed Unicode input and retain a failed dialog");
        Check(Read(GetDlgItem(window, Name)) == name, "failed creation must preserve the user's entered name");
        const auto failed = root / L"projects-validation.png";
        Check(CaptureNewItemDialog(window, failed.wstring(), error), "callback errors must remain visible");
        Check(Raster(failed.wstring()).ErrorPixels(GetDpiForWindow(window) / 96.0f) > 20, "callback error must be painted inline in the actual HWND");
        Click(window, FilesTab);
        Check(ListText(GetDlgItem(window, Types), 0) == L"Text File", "Files tab must select Text File");
        Check(!ShownStyle(GetDlgItem(window, RecentProjects)) && !ShownStyle(GetDlgItem(window, OpenExisting)), "file creation must hide project-only controls");
        Check(Read(GetDlgItem(window, Location)) == options.projectLocation, "project parent folder must never become a file location");
        Check(GetNextDlgTabItem(window, GetDlgItem(window, Location), FALSE) == GetDlgItem(window, Browse), "keyboard navigation must reach Browse");
        Check(GetNextDlgTabItem(window, GetDlgItem(window, Browse), FALSE) == GetDlgItem(window, Cancel), "keyboard navigation must skip hidden project actions");
        Click(window, SessionsTab);
        Check(ListText(GetDlgItem(window, Types), 0) == L"Work Session", "Sessions tab must select Work Session");
        Click(window, ProjectsTab);
        Check(Read(GetDlgItem(window, Location)) == root.wstring(), "switching categories must retain the project parent location");
        Click(window, FilesTab);
        SendDlgItemMessageW(window, FilesTab, WM_KEYDOWN, VK_RIGHT, 0);
        Check(ListText(GetDlgItem(window, Types), 0) == L"Work Session", "keyboard tab arrows must switch categories");
        accept = true; Click(window, Create);
        Check(calls == 2 && !IsWindow(window) && received.category == ItemCategory::Sessions && received.typeId == L"work-session", "successful retry must close the dialog with the selected registered type");
        Check(IsWindowEnabled(owner), "a hidden test dialog must not disable its owner");
        DestroyWindow(owner);
    }
    void TestRegistryRecentsAndDpi(const std::filesystem::path& root)
    {
        ItemTypeRegistry registry;
        std::wstring error;
        Check(registry.RegisterType({L"markdown", L"Markdown File", L"A plain Markdown document.", L".md", ItemCategory::Files}, error), "future registered types must be accepted");
        NewItemDialogOptions options;
        options.initialCategory = ItemCategory::Files; options.types = registry.Types(); options.location = root.wstring();
        options.projectLocation = (root / L"chosen-project").wstring();
        NewItemRequest received{}; int calls = 0;
        NewItemDialogCallbacks callbacks;
        callbacks.create = [&](const NewItemRequest& request, std::wstring& callbackError) { received = request; ++calls; callbackError = L"Retryable failure."; return false; };
        HWND window = CreateNewItemDialogWindow(nullptr, options, callbacks);
        Check(window != nullptr, "registered-type test dialog must be created");
        Check(Read(GetDlgItem(window, Location)) == options.projectLocation, "direct Files opening must use the project root rather than its parent");
        Check(SendDlgItemMessageW(window, Types, LB_GETCOUNT, 0, 0) == 2, "dialog must enumerate registry types dynamically");
        SendDlgItemMessageW(window, Types, LB_SETCURSEL, 1, 0);
        SetDlgItemTextW(window, Name, L"notes"); Click(window, Create);
        Check(calls == 1 && received.typeId == L"markdown" && IsWindow(window), "custom registry types must reach the same callback and retry path");
        RECT dpiRect{0, 0, 1200, 960};
        SendMessageW(window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&dpiRect));
        const auto nameRect = ChildRect(window, Name);
        Check(nameRect.left == 459 && nameRect.top == 218, "native fields must scale to the dialog's current DPI");
        Check(SendDlgItemMessageW(window, Types, LB_GETITEMHEIGHT, 0, 0) == 81, "native list rows must scale with DPI");
        const auto font = reinterpret_cast<HFONT>(SendDlgItemMessageW(window, Name, WM_GETFONT, 0, 0));
        LOGFONTW logical{}; Check(GetObjectW(font, sizeof(logical), &logical) && logical.lfHeight == -20, "native edit text must use a DPI-scaled font");
        MINMAXINFO bounds{}; SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&bounds));
        Check(bounds.ptMinTrackSize.x > 0 && bounds.ptMinTrackSize.x <= 1080 && bounds.ptMinTrackSize.y > 0 && bounds.ptMinTrackSize.y <= 690, "dialog minimum size must scale with DPI and fit the work area");
        SetWindowPos(window, nullptr, 0, 0, 1380, 1050, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{}; GetClientRect(window, &client);
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!ShownStyle(child)) continue;
            const auto rect = ChildRect(window, GetDlgCtrlID(child));
            Check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom, "resized native controls must remain inside the dialog");
        }
        MSG enter{}; enter.hwnd = GetDlgItem(window, Name); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
        Check(SendMessageW(enter.hwnd, WM_GETDLGCODE, VK_RETURN, reinterpret_cast<LPARAM>(&enter)) & DLGC_WANTMESSAGE, "Enter must route to the dialog submit handler during modal keyboard translation");
        const auto screenshot = root / L"files-dpi.png";
        Check(CaptureNewItemDialog(window, screenshot.wstring(), error), "resized DPI-scaled native dialog must capture");
        const Raster image(screenshot.wstring());
        Check(image.width == 1380 && image.height == 1050, "capture must use the actual current HWND dimensions");
        SetWindowPos(window, nullptr, 0, 0, 1080, 690, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Click(window, ProjectsTab);
        GetClientRect(window, &client);
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!ShownStyle(child)) continue;
            const auto rect = ChildRect(window, GetDlgCtrlID(child));
            Check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom, "compact native controls must remain accessible inside a small dialog");
        }
        Check(ChildRect(window, RecentProjects).bottom < ChildRect(window, OpenSelected).top &&
            ChildRect(window, OpenSelected).bottom < ChildRect(window, Create).top, "compact recent actions must not overlap list or footer controls");
        Check(CaptureNewItemDialog(window, (root / L"projects-compact.png").wstring(), error), "compact native layout must capture for visual review");
        SendDlgItemMessageW(window, Name, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!IsWindow(window) && calls == 1, "Escape must cancel from a native edit without invoking creation again");

        options = NewItemDialogOptions{}; options.types = registry.Types(); options.location = root.wstring();
        options.recentProjects = {(root / L"own-project").wstring(), (root / L"missing-project").wstring()};
        int opens = 0; bool openSucceeds = false; std::wstring opened;
        callbacks.openProject = [&](const std::wstring& path, std::wstring& callbackError) { ++opens; opened = path; callbackError = L"This project is missing. Choose another project."; return openSucceeds; };
        window = CreateNewItemDialogWindow(nullptr, options, callbacks);
        Check(window != nullptr && SendDlgItemMessageW(window, RecentProjects, LB_GETCOUNT, 0, 0) == 2, "recent list must contain only supplied real paths");
        SendDlgItemMessageW(window, RecentProjects, LB_SETCURSEL, 1, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RecentProjects, LBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(window, RecentProjects)));
        Check(IsWindowEnabled(GetDlgItem(window, OpenSelected)), "selecting a recent project must enable Open Selected");
        Click(window, OpenSelected);
        Check(opens == 1 && opened == options.recentProjects[1] && IsWindow(window), "failed recent-project opening must leave the dialog available for retry");
        SendDlgItemMessageW(window, RecentProjects, LB_SETCURSEL, 0, 0); openSucceeds = true;
        SendDlgItemMessageW(window, RecentProjects, WM_KEYDOWN, VK_RETURN, 0);
        Check(opens == 2 && opened == options.recentProjects[0] && !IsWindow(window), "Enter on a recent project must open the chosen folder and close only on success");
        Check(!CaptureNewItemDialog(window, screenshot.wstring(), error) && !error.empty(), "capturing a closed dialog must report a useful error");
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
            if (script.accept) { SetDlgItemTextW(window, Name, L"Own modal fixture"); SendDlgItemMessageW(window, Name, WM_KEYDOWN, VK_RETURN, 0); }
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
            std::wstring(className) == L"ConnectorStudioNewItemDialog")
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
        NewItemDialogOptions options; options.location = root.wstring();
        int calls = 0; NewItemDialogCallbacks callbacks;
        callbacks.create = [&](const NewItemRequest&, std::wstring&) { ++calls; return true; };
        ModalScript script{owner}; currentScript = &script;
        const HHOOK hook = SetWindowsHookExW(WH_CBT, ModalHook, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "thread-local hidden modal probe must install");
        const HWND foreground = GetForegroundWindow();
        const bool canceled = ShowNewItemDialog(owner, options, callbacks);
        Check(!canceled && calls == 0 && script.ownerDisabled && script.stayedHidden, "Cancel must leave app state untouched while the modal owner was disabled");
        Check(IsWindowEnabled(owner) && GetForegroundWindow() == foreground, "modal cleanup must restore the owner without activating test windows");
        script.accept = true;
        Check(ShowNewItemDialog(owner, options, callbacks) && calls == 1 && script.ownerDisabled && script.stayedHidden, "modal success must reflect an accepted callback and remain hidden in tests");
        Check(IsWindowEnabled(owner), "successful modal closure must restore the owner");
        EnableWindow(owner, FALSE); script.accept = false;
        Check(!ShowNewItemDialog(owner, options, callbacks) && !IsWindowEnabled(owner), "an owner already disabled before opening must remain disabled");
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
        TestControlsAndRetry(root); TestRegistryRecentsAndDpi(root); TestModalOwner(root);
        std::cout << checks << " native new-dialog checks passed.\n";
        CoUninitialize(); return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "New dialog regression: " << error.what() << '\n';
        CoUninitialize(); return 1;
    }
}
