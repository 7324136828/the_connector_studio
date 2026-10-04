#define LATTICE_TESTING
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/main.cpp"
#include <cassert>
#include <iostream>

namespace
{
    void SetComposerText(const std::wstring& text)
    {
        SetWindowTextW(g_editControl, text.c_str());
        // Multiline EDIT's WM_SETTEXT omits EN_CHANGE, unlike user typing.
        SendMessageW(g_mainWindow, WM_COMMAND, MAKEWPARAM(201, EN_CHANGE),
            reinterpret_cast<LPARAM>(g_editControl));
    }
    void AssertCaptionlessClient(HWND window)
    {
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        assert((style & WS_CAPTION) == 0);
        assert((style & (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ==
            (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        RECT windowRect{}, clientRect{};
        assert(GetWindowRect(window, &windowRect));
        assert(GetClientRect(window, &clientRect));
        POINT clientOrigin{};
        assert(ClientToScreen(window, &clientOrigin));
        assert(clientOrigin.x == windowRect.left && clientOrigin.y == windowRect.top);
        assert(clientRect.right == windowRect.right - windowRect.left);
        assert(clientRect.bottom == windowRect.bottom - windowRect.top);
    }
    LRESULT FrameHit(HWND window, float x, float y)
    {
        POINT point{static_cast<LONG>(std::lround(x * Scale())), static_cast<LONG>(std::lround(y * Scale()))};
        assert(ClientToScreen(window, &point));
        return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    }
    LRESULT CALLBACK KeepFrameTestHidden(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR)
    {
        if (message == WM_WINDOWPOSCHANGING)
        {
            auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
            // Test actual Windows minimize/maximize transitions without displaying a surface.
            // WM_WINDOWPOSCHANGING permits changes to the visibility flags in WINDOWPOS.
            if (position->flags & SWP_SHOWWINDOW)
            {
                position->flags &= ~SWP_SHOWWINDOW;
                position->flags |= SWP_HIDEWINDOW;
            }
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
    void TestNativeFrame(HWND window)
    {
        AssertCaptionlessClient(window);
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        const RECT proposed{40, 50, 980, 790};
        RECT simple = proposed;
        assert(SendMessageW(window, WM_NCCALCSIZE, FALSE, reinterpret_cast<LPARAM>(&simple)) == 0);
        assert(EqualRect(&simple, &proposed));
        WINDOWPOS position{window, nullptr, 40, 50, 940, 740, SWP_NOZORDER | SWP_NOACTIVATE};
        NCCALCSIZE_PARAMS detailed{};
        detailed.rgrc[0] = proposed;
        detailed.rgrc[1] = RECT{10, 20, 900, 700};
        detailed.rgrc[2] = RECT{11, 21, 899, 699};
        detailed.lppos = &position;
        const RECT previousWindow = detailed.rgrc[1], previousClient = detailed.rgrc[2];
        assert(SendMessageW(window, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&detailed)) == 0);
        assert(EqualRect(&detailed.rgrc[0], &proposed));
        assert(EqualRect(&detailed.rgrc[1], &previousWindow));
        assert(EqualRect(&detailed.rgrc[2], &previousClient));

        assert(SetWindowTextW(window, L"Updated taskbar title without a native caption"));
        AssertCaptionlessClient(window);
        assert(SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        AssertCaptionlessClient(window);
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        assert(GetSystemMenu(window, FALSE));
        assert(GetMenuState(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND) != static_cast<UINT>(-1));
        assert(FrameHit(window, 260.0f, 18.0f) == HTCAPTION);
        assert(FrameHit(window, 260.0f, 1.0f) == HTTOP);
        assert(FrameHit(window, 1.0f, 1.0f) == HTTOPLEFT);
        assert(FrameHit(window, 100.0f, 18.0f) == HTCLIENT);
        assert(FrameHit(window, 16.0f, 18.0f) == HTCLIENT);
        const auto close = Lattice::UIComponents::HitTest(g_layoutMetrics, 16.0f, 18.0f,
            g_controller->GetSidebarMode(), Lattice::ActiveDropdown::None, {});
        assert(close.type == Lattice::HitTargetType::TrafficClose);
        assert(Lattice::UIComponents::HitTest(g_layoutMetrics, 34.0f, 18.0f,
            g_controller->GetSidebarMode(), Lattice::ActiveDropdown::None, {}).type == Lattice::HitTargetType::TrafficMinimize);
        assert(Lattice::UIComponents::HitTest(g_layoutMetrics, 52.0f, 18.0f,
            g_controller->GetSidebarMode(), Lattice::ActiveDropdown::None, {}).type == Lattice::HitTargetType::TrafficMaximize);

        MONITORINFO monitor{sizeof(monitor)};
        assert(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor));
        MINMAXINFO bounds{};
        SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&bounds));
        assert(bounds.ptMaxPosition.x == monitor.rcWork.left - monitor.rcMonitor.left);
        assert(bounds.ptMaxPosition.y == monitor.rcWork.top - monitor.rcMonitor.top);
        assert(bounds.ptMaxSize.x == monitor.rcWork.right - monitor.rcWork.left);
        assert(bounds.ptMaxSize.y == monitor.rcWork.bottom - monitor.rcWork.top);
        assert(bounds.ptMinTrackSize.x == static_cast<LONG>(860 * Scale()));
        assert(bounds.ptMinTrackSize.y == static_cast<LONG>(580 * Scale()));

        RECT restored{};
        assert(GetWindowRect(window, &restored));
        constexpr UINT_PTR hiddenSubclass = 1;
        assert(SetWindowSubclass(window, KeepFrameTestHidden, hiddenSubclass, 0));
        // Consume any process STARTUPINFO show-state override without displaying the window.
        ShowWindow(window, SW_HIDE);
        ShowWindow(window, SW_SHOWMINNOACTIVE);
        assert(IsIconic(window));
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        assert(GetForegroundWindow() != window);
        ShowWindow(window, SW_RESTORE);
        assert(!IsIconic(window) && !IsZoomed(window));
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        AssertCaptionlessClient(window);
        ShowWindow(window, SW_MAXIMIZE);
        assert(IsZoomed(window));
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        assert(GetForegroundWindow() != window);
        RECT maximized{};
        assert(GetWindowRect(window, &maximized));
        assert(EqualRect(&maximized, &monitor.rcWork));
        AssertCaptionlessClient(window);
        // Maximized windows retain caption dragging, but do not expose a resize edge.
        assert(FrameHit(window, 260.0f, 1.0f) == HTCAPTION);
        ShowWindow(window, SW_RESTORE);
        assert(!IsZoomed(window) && !IsIconic(window));
        assert((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == 0);
        assert(GetForegroundWindow() != window);
        RECT returned{};
        assert(GetWindowRect(window, &returned));
        assert(EqualRect(&returned, &restored));
        AssertCaptionlessClient(window);
        assert(RemoveWindowSubclass(window, KeepFrameTestHidden, hiddenSubclass));
        RefreshControls();
    }
    struct KeyboardState
    {
        BYTE original[256]{};
        KeyboardState(bool ctrl = false, bool shift = false)
        {
            GetKeyboardState(original);
            BYTE state[256]{};
            if (ctrl) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
            if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
            SetKeyboardState(state);
        }
        ~KeyboardState() { SetKeyboardState(original); }
    };
}

int main()
{
    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    cls.lpfnWndProc = WindowProc; cls.hInstance = instance; cls.lpszClassName = WindowClassName;
    assert(RegisterClassW(&cls));
    const float initialScale = GetDpiForSystem() / 96.0f;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WindowClassName, L"Interaction regression tests",
        MainWindowStyle, 0, 0, static_cast<int>(1140 * initialScale), static_cast<int>(760 * initialScale), nullptr, nullptr, instance, nullptr);
    assert(window && g_editControl && g_controller && g_d2dContext);
    // Check startup geometry before a title update or SWP_FRAMECHANGED can mask creation defects.
    AssertCaptionlessClient(window);
    assert(g_controller->GetTabs().empty() && g_controller->GetActiveTab() == -1);
    assert(g_controller->GetFiles().empty() && !ActiveSessionFor(*g_controller));
    assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
    KillTimer(window, AnimTimerId);
    // No user preference writes or modal save prompts from tests.
    g_controller = std::make_unique<Lattice::AppController>();
    g_controller->Initialize(nullptr);
    RefreshControls();
    TestNativeFrame(window);

    assert(g_controller->GetTabs().empty() && g_controller->GetActiveTab() == -1);
    assert(g_controller->GetFiles().empty() && g_controller->GetSkills().empty() && g_controller->GetPlugins().empty());
    assert(!ActiveSessionFor(*g_controller) && !g_layoutMetrics.hasActiveSession);
    assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
    assert(ReadEditText(window) == Lattice::AppIdentity::Name);
    SendMessageW(window, WM_PAINT, 0, 0);
    FocusComposer();
    {
        KeyboardState keys(true);
        assert(HandleNavigationKey(VK_TAB));
    }
    OpenMenu(Lattice::ActiveDropdown::Session);
    assert(HandleNavigationKey(VK_ESCAPE));
    assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
    wcscpy_s(g_findText, L"native-search-fixture-omega");
    assert(!FindNext(false));
    assert(g_controller->GetTabs().empty());
    assert(HandleCommand(NewSessionCommand));
    assert(g_controller->GetActiveTab() == 0 && g_controller->GetTabs().size() == 1);
    assert(g_controller->GetTabs()[0].messages.empty());
    assert(g_controller->GetTabs()[0].projectName.empty() && g_controller->GetTabs()[0].projectPath.empty());
    assert(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE);
    // Search owns an explicit fixture, independently of startup content.
    SetComposerText(L"native-search-fixture-omega");
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    for (int i = 0; i < 20; ++i) SendMessageW(window, WM_TIMER, AnimTimerId, 0);

    const std::wstring unicodeDraft = L"Draft \u03A9 \u4E2D \U0001F680\nsecond line";
    SetComposerText(unicodeDraft);
    assert(ReadEditText(g_editControl) == unicodeDraft);
    assert(g_controller->GetDraftText() == unicodeDraft);
    {
        KeyboardState keys(true);
        SendMessageW(g_editControl, WM_CHAR, 1, 0);
        DWORD start = 0, end = 0;
        SendMessageW(g_editControl, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        assert(start == 0 && end == unicodeDraft.size());
    }
    const int firstTab = g_controller->GetActiveTab();
    assert(HandleCommand(NewSessionCommand));
    assert(ReadEditText(g_editControl).empty());
    SetComposerText(L"Other tab's draft");
    g_controller->SelectSession(firstTab); RefreshControls();
    assert(ReadEditText(g_editControl) == unicodeDraft);
    g_controller->SelectSession(static_cast<int>(g_controller->GetTabs().size()) - 1); RefreshControls();
    assert(ReadEditText(g_editControl) == L"Other tab's draft");

    SetComposerText(L"   \r\n\t");
    auto messageCount = g_controller->GetTabs()[g_controller->GetActiveTab()].messages.size();
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    assert(g_controller->GetTabs()[g_controller->GetActiveTab()].messages.size() == messageCount);
    assert(ReadEditText(g_editControl) == L"   \r\n\t");

    SetComposerText(L"Send with Enter");
    SendMessageW(g_editControl, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    assert(g_controller->GetTabs()[g_controller->GetActiveTab()].messages.size() == messageCount + 1);
    assert(ReadEditText(g_editControl).empty());
    assert(g_controller->GetDraftText().empty());
    const int sentTab = g_controller->GetActiveTab();

    SetComposerText(L"line one");
    SendMessageW(g_editControl, EM_SETSEL, 8, 8);
    {
        KeyboardState keys(false, true);
        SendMessageW(g_editControl, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    }
    assert(ReadEditText(g_editControl).find(L'\n') != std::wstring::npos);
    assert(g_controller->GetTabs()[sentTab].messages.size() == messageCount + 1);

    const auto tabCount = g_controller->GetTabs().size();
    {
        KeyboardState keys(true);
        MSG msg{}; msg.hwnd = g_editControl; msg.message = WM_KEYDOWN; msg.wParam = 'N';
        assert(PreTranslateAppMessage(msg));
    }
    assert(g_controller->GetTabs().size() == tabCount + 1);
    assert(ReadEditText(g_editControl).empty());
    for (int i = 0; i < 20; ++i) SendMessageW(window, WM_TIMER, AnimTimerId, 0);
    assert(g_controller->GetTabs()[sentTab].messages.size() == messageCount + 2);
    assert(g_controller->GetTabs()[g_controller->GetActiveTab()].messages.empty());

    OpenMenu(Lattice::ActiveDropdown::Session);
    assert(g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::Session);
    assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
    assert(HandleNavigationKey(VK_DOWN)); assert(g_dropdownIndex == 1);
    assert(HandleNavigationKey(VK_ESCAPE));
    assert(g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None);
    assert(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE);
    NavigateFocus(false);
    assert(g_keyboardFocus && g_keyboardTarget.rect.right > g_keyboardTarget.rect.left);
    g_controller->HandleClick({Lattice::HitTargetType::ActivitySession});
    g_dropdownIndex = 0;
    g_controller->SetActiveDropdown(Lattice::ActiveDropdown::SidebarModel); RefreshControls();
    assert(HandleNavigationKey(VK_DOWN)); assert(HandleNavigationKey(VK_RETURN));
    assert(g_controller->GetSelectedModel() == L"GPT-4.1");
    assert(g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None);

    SetComposerText(L"Draft remains after search");
    wcscpy_s(g_findText, L"native-search-fixture-omega");
    assert(FindNext(false));
    assert(g_controller->GetActiveTab() == 0);
    assert(ReadEditText(g_editControl) == unicodeDraft);
    wcscpy_s(g_findText, L"unlikely-no-such-text-29");
    assert(!FindNext(false));
    assert(ReadEditText(g_editControl) == unicodeDraft);

    // Scrollbar position is derived from actual rendered content.
    g_controller->SetDraftText(std::wstring(8000, L'W')); SyncControllerToEdit();
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0); RefreshControls();
    const auto& tab = g_controller->GetTabs()[g_controller->GetActiveTab()];
    assert(tab.maxScroll > 0);
    g_controller->SetActiveScrollOffset(0); RefreshControls();
    const auto thumb = g_layoutMetrics.scrollbarThumb;
    const int x = static_cast<int>((thumb.left + thumb.right) * 0.5f * Scale());
    const int y = static_cast<int>((thumb.top + thumb.bottom) * 0.5f * Scale());
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    assert(g_dragConversation);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON,
        MAKELPARAM(x, static_cast<int>(g_layoutMetrics.scrollbarTrack.bottom * Scale())));
    assert(g_controller->GetTabs()[g_controller->GetActiveTab()].scrollOffset > 0);
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    assert(!g_dragConversation && !g_dragSidebar);

    // High-DPI layout and native editor use the same coordinate system.
    g_keyboardFocus = true;
    g_keyboardTarget = {Lattice::HitTargetType::ComposerSend, -1, g_layoutMetrics.composerSendBtn};
    RECT rect{0, 0, 1710, 1140};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&rect));
    assert(g_dpi == 144);
    AssertCaptionlessClient(window);
    assert(FrameHit(window, 260.0f, 18.0f) == HTCAPTION);
    assert(FrameHit(window, 260.0f, 1.0f) == HTTOP);
    assert(FrameHit(window, 16.0f, 18.0f) == HTCLIENT);
    assert(g_keyboardFocus && g_keyboardTarget.rect.left == g_layoutMetrics.composerSendBtn.left);
    RECT edit{}; GetWindowRect(g_editControl, &edit);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&edit), 2);
    assert(std::abs(edit.left - static_cast<int>(std::lround(g_layoutMetrics.composerEditArea.left * Scale()))) <= 1);
    assert(std::abs(edit.top - static_cast<int>(std::lround(g_layoutMetrics.composerEditArea.top * Scale()))) <= 1);
    assert(g_d2dContext->BeginDraw());
    assert(SUCCEEDED(g_d2dContext->EndDraw()));

    // A clean explicit session closes back to a truly empty surface without a save dialog.
    g_controller = std::make_unique<Lattice::AppController>();
    g_controller->Initialize(nullptr);
    g_keyboardFocus = false;
    RefreshControls();
    const auto create = g_layoutMetrics.tabNewBtn;
    const int createX = static_cast<int>((create.left + create.right) * 0.5f * Scale());
    const int createY = static_cast<int>((create.top + create.bottom) * 0.5f * Scale());
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(createX, createY));
    assert(g_controller->GetTabs().size() == 1 && g_controller->GetTabs()[0].messages.empty());
    assert(g_controller->GetTabs()[0].draftText.empty() && !g_controller->GetTabs()[0].dirty);
    assert(HandleCommand(CloseSessionCommand));
    assert(g_controller->GetTabs().empty() && g_controller->GetActiveTab() == -1);
    assert(!ActiveSessionFor(*g_controller) && !g_layoutMetrics.hasActiveSession);
    assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
    assert(ReadEditText(g_editControl).empty() && ReadEditText(window) == Lattice::AppIdentity::Name);
    SendMessageW(window, WM_PAINT, 0, 0);
    AssertCaptionlessClient(window);

    DestroyWindow(window);
    UnregisterClassW(WindowClassName, instance);
    CoUninitialize();
    std::cout << "Native interaction regressions passed.\n";
}
