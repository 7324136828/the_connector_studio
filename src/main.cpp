#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <memory>
#include <algorithm>
#include <shellapi.h>
#include <commdlg.h>
#include <cmath>
#include <cwctype>
#include <fstream>
#include "AppTypes.h"
#include "Direct2DContext.h"
#include "UIComponents.h"
#include "AppController.h"
#include "AppIdentity.h"
#include "NewItemDialog.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    constexpr const wchar_t* WindowClassName = Lattice::AppIdentity::WindowClass;
    // WS_POPUP prevents Windows from adding an overlapped-window caption at creation.
    // Retain native resizing, taskbar, and minimize/maximize/system commands.
    constexpr DWORD MainWindowStyle = WS_POPUP | WS_THICKFRAME | WS_SYSMENU |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    constexpr UINT_PTR AnimTimerId = 1001;

    HWND g_mainWindow = nullptr;
    HWND g_editControl = nullptr;
    WNDPROC g_originalEditProc = nullptr;
    HBRUSH g_editBackgroundBrush = nullptr;
    HFONT g_editFont = nullptr;

    std::unique_ptr<Lattice::Direct2DContext> g_d2dContext;
    std::unique_ptr<Lattice::AppController> g_controller;
    Lattice::LayoutMetrics g_layoutMetrics;
    Lattice::HitTestResult g_hoveredTarget;
    float g_animTick = 0.0f;
    bool g_mouseTracking = false;
    bool g_syncingText = false;
    bool g_editComposing = false;
    float g_dpi = 96.0f;
    bool g_dragSidebar = false;
    bool g_dragConversation = false;
    float g_dragAnchor = 0.0f;
    int g_dropdownIndex = 0;
    HWND g_findDialog = nullptr;
    UINT g_findMessage = 0;
    FINDREPLACEW g_find{};
    wchar_t g_findText[512]{};
    int g_lastFoundSession = -1;
    int g_lastFoundMessage = -1;
    bool g_keyboardFocus = false;
    Lattice::HitTestResult g_keyboardTarget{};
    HACCEL g_accelerators = nullptr;
    enum Command : WORD { NewSessionCommand = 4001, OpenSessionCommand, SaveSessionCommand,
        CloseSessionCommand, NewProjectCommand, CloseProjectCommand, SearchCommand,
        ProjectMenuCommand, SessionMenuCommand, FocusComposerCommand, FilesCommand,
        SessionControlsCommand, PluginsCommand, FindNextCommand };

    float Scale() { return g_dpi / 96.0f; }
    bool Contains(const D2D1_RECT_F& r, float x, float y)
    {
        return r.right > r.left && r.bottom > r.top &&
            x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }
    const Lattice::SessionTab* ActiveSessionFor(const Lattice::AppController& controller)
    {
        const auto& tabs = controller.GetTabs();
        const int active = controller.GetActiveTab();
        return active >= 0 && active < static_cast<int>(tabs.size()) ? &tabs[active] : nullptr;
    }
    void FocusComposer()
    {
        SetFocus(g_controller && ActiveSessionFor(*g_controller) && g_editControl &&
            (GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE) ? g_editControl : g_mainWindow);
    }
    std::wstring ReadEditText(HWND hwnd)
    {
        const int length = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
        text.resize(static_cast<size_t>((std::max)(0, copied)));
        return text;
    }
    void SyncControllerToEdit()
    {
        if (!g_editControl || !g_controller ||
            ReadEditText(g_editControl) == g_controller->GetDraftText()) return;
        g_syncingText = true;
        SetWindowTextW(g_editControl, g_controller->GetDraftText().c_str());
        SendMessageW(g_editControl, EM_SETSEL, g_controller->GetDraftText().size(), g_controller->GetDraftText().size());
        g_syncingText = false;
    }
    void UpdateEditFont()
    {
        HFONT replacement = CreateFontW(-static_cast<int>(std::lround(12.0f * Scale())),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!replacement) return;
        if (g_editControl) SendMessageW(g_editControl, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
        if (g_editFont) DeleteObject(g_editFont);
        g_editFont = replacement;
    }
    void RefreshControls();
    bool HandleNavigationKey(WPARAM key);
    std::vector<Lattice::HitTestResult> FocusTargets();

    LRESULT CALLBACK SubclassedEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_KEYDOWN:
            if (HandleNavigationKey(wParam)) return 0;
            break;
        case WM_CHAR:
            if (wParam == 1 && (GetKeyState(VK_CONTROL) & 0x8000))
            { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
            if (wParam == 127 && (GetKeyState(VK_CONTROL) & 0x8000))
            {
                DWORD start = 0, end = 0;
                SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                const auto text = ReadEditText(hwnd);
                if (start == end)
                {
                    while (start > 0 && iswspace(text[start - 1])) --start;
                    while (start > 0 && !iswspace(text[start - 1])) --start;
                }
                SendMessageW(hwnd, EM_SETSEL, start, end);
                SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
                return 0;
            }
            // Consuming WM_CHAR prevents TranslateMessage inserting a stray newline after sending.
            if (wParam == VK_RETURN && !g_editComposing && !(GetKeyState(VK_SHIFT) & 0x8000))
            {
                if (g_controller)
                {
                    g_controller->SetDraftText(ReadEditText(hwnd));
                    g_controller->SendCurrentMessage();
                    RefreshControls();
                }
                return 0;
            }
            if (wParam == VK_TAB) return 0;
            break;
        case WM_IME_STARTCOMPOSITION:
            g_editComposing = true;
            break;
        case WM_IME_ENDCOMPOSITION:
            g_editComposing = false;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(g_mainWindow, nullptr, FALSE);
            break;
        case WM_PAINT:
        {
            const LRESULT result = CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);
            // Multiline EDIT does not support EM_SETCUEBANNER.
            if (GetWindowTextLengthW(hwnd) == 0)
            {
                HDC dc = GetDC(hwnd);
                if (dc)
                {
                    RECT r{}; GetClientRect(hwnd, &r); r.left += 2; r.top += 3;
                    auto oldFont = SelectObject(dc, g_editFont);
                    SetTextColor(dc, RGB(78, 85, 96)); SetBkMode(dc, TRANSPARENT);
                    DrawTextW(dc, L"Type a message or attach files", -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SelectObject(dc, oldFont); ReleaseDC(hwnd, dc);
                }
            }
            return result;
        }
        }
        return CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);
    }

    void SyncEditControlToController()
    {
        if (!g_editControl || !g_controller || g_syncingText) return;

        g_controller->SetDraftText(ReadEditText(g_editControl));
    }

    void UpdateLayoutAndControls(int width, int height)
    {
        if (!g_controller || !g_d2dContext || width <= 0 || height <= 0) return;

        Lattice::UIComponents::ComputeLayout(
            g_layoutMetrics,
            static_cast<float>(width) / Scale(),
            static_cast<float>(height) / Scale(),
            g_controller->GetSidebarMode(),
            g_controller->GetTabs(),
            g_controller->GetActiveTab(),
            g_controller->GetFiles(),
            g_controller->GetSkills(),
            g_controller->GetPlugins(),
            g_controller->GetActiveDropdown(),
            g_controller->GetCurrentDropdownItems(),
            g_controller->GetSidebarScrollOffset(), g_controller->GetProjectExpanded(),
            !g_controller->GetCurrentProjectPath().empty());

        g_controller->SetSidebarScrollMetrics(g_layoutMetrics.sidebarMaxScroll);
        const auto* session = ActiveSessionFor(*g_controller);
        if (session)
        {
            const float contentHeight = Lattice::UIComponents::MeasureConversationHeight(
                *g_d2dContext, g_layoutMetrics, *session);
            g_controller->SetActiveScrollMetrics(Lattice::UIComponents::UpdateConversationScrollMetrics(
                g_layoutMetrics, contentHeight, session->scrollOffset));
            Lattice::UIComponents::UpdateConversationScrollMetrics(g_layoutMetrics, contentHeight, session->scrollOffset);
        }
        else
        {
            Lattice::UIComponents::UpdateConversationScrollMetrics(g_layoutMetrics, 0.0f, 0.0f);
            if (g_dragConversation) { g_dragConversation = false; ReleaseCapture(); }
        }
        std::wstring title = Lattice::AppIdentity::Name;
        if (!g_controller->GetCurrentProjectName().empty()) title += L" \x2014 " + g_controller->GetCurrentProjectName();
        if (ReadEditText(g_mainWindow) != title) SetWindowTextW(g_mainWindow, title.c_str());

        if (g_editControl)
        {
            const auto& editR = g_layoutMetrics.composerEditArea;
            RECT wanted{ static_cast<LONG>(std::lround(editR.left * Scale())),
                static_cast<LONG>(std::lround(editR.top * Scale())),
                static_cast<LONG>(std::lround(editR.right * Scale())),
                static_cast<LONG>(std::lround(editR.bottom * Scale())) };
            RECT current{};
            GetWindowRect(g_editControl, &current);
            MapWindowPoints(nullptr, g_mainWindow, reinterpret_cast<POINT*>(&current), 2);
            if (session && !EqualRect(&wanted, &current))
                MoveWindow(g_editControl, wanted.left, wanted.top, wanted.right - wanted.left, wanted.bottom - wanted.top, TRUE);
            // Native child controls paint above Direct2D popups.
            const bool show = session && g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None;
            if (static_cast<bool>(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE) != show)
                ShowWindow(g_editControl, show ? SW_SHOWNA : SW_HIDE);
        }
        if (g_keyboardFocus)
        {
            bool found = false;
            for (const auto& target : FocusTargets())
                if (target.type == g_keyboardTarget.type && target.index == g_keyboardTarget.index)
                { g_keyboardTarget = target; found = true; break; }
            if (!found) g_keyboardFocus = false;
        }
    }

    void RefreshControls()
    {
        SyncControllerToEdit();
        RECT r{}; GetClientRect(g_mainWindow, &r);
        UpdateLayoutAndControls(r.right, r.bottom);
        InvalidateRect(g_mainWindow, nullptr, FALSE);
    }

    void OpenMenu(Lattice::ActiveDropdown menu)
    {
        SyncEditControlToController();
        g_controller->SetActiveDropdown(g_controller->GetActiveDropdown() == menu ? Lattice::ActiveDropdown::None : menu);
        g_dropdownIndex = 0; SetFocus(g_mainWindow); RefreshControls();
    }
    std::wstring Lower(std::wstring text)
    {
        for (auto& ch : text) ch = static_cast<wchar_t>(towlower(ch));
        return text;
    }
    bool FindNext(bool matchCase)
    {
        if (!g_controller || !*g_findText) return false;
        const auto query = matchCase ? std::wstring(g_findText) : Lower(g_findText);
        struct Location { int tab; int message; };
        std::vector<Location> locations;
        const auto& tabs = g_controller->GetTabs();
        for (size_t t = 0; t < tabs.size(); ++t)
            for (size_t m = 0; m < tabs[t].messages.size(); ++m)
                locations.push_back({static_cast<int>(t), static_cast<int>(m)});
        size_t start = 0;
        for (size_t i = 0; i < locations.size(); ++i)
            if (tabs[locations[i].tab].id == g_lastFoundSession && locations[i].message == g_lastFoundMessage) start = i + 1;
        for (size_t n = 0; n < locations.size(); ++n)
        {
            const auto loc = locations[(start + n) % locations.size()];
            const auto& msg = tabs[loc.tab].messages[loc.message];
            std::wstring text = msg.text;
            for (const auto& item : msg.items) text += L"\n" + item;
            for (const auto& step : msg.planSteps) text += L"\n" + step.title + L"\n" + step.description;
            if ((matchCase ? text : Lower(text)).find(query) == std::wstring::npos) continue;
            SyncEditControlToController(); g_controller->SelectSession(loc.tab); RefreshControls();
            Lattice::SessionTab prefix = tabs[loc.tab];
            prefix.messages.resize(static_cast<size_t>(loc.message));
            g_controller->SetActiveScrollOffset((std::max)(0.0f,
                Lattice::UIComponents::MeasureConversationHeight(*g_d2dContext, g_layoutMetrics, prefix) - 28.0f));
            g_lastFoundSession = tabs[loc.tab].id; g_lastFoundMessage = loc.message;
            RefreshControls(); return true;
        }
        return false;
    }
    void OpenSearch()
    {
        g_controller->CloseDropdown(); RefreshControls();
        if (g_findDialog) { SetForegroundWindow(g_findDialog); return; }
        g_find = {}; g_find.lStructSize = sizeof(g_find); g_find.hwndOwner = g_mainWindow;
        g_find.lpstrFindWhat = g_findText;
        g_find.wFindWhatLen = static_cast<WORD>(std::size(g_findText));
        g_find.Flags = FR_DOWN | FR_HIDEUPDOWN | FR_HIDEWHOLEWORD;
        g_findDialog = FindTextW(&g_find);
    }
    void ExecuteTarget(const Lattice::HitTestResult& target)
    {
        SyncEditControlToController();
        using Lattice::HitTargetType;
        if (target.type == HitTargetType::TitleSearch) OpenSearch();
        else if (target.type == HitTargetType::ComposerEdit) FocusComposer();
        else if (target.type == HitTargetType::TabPrevious || target.type == HitTargetType::TabNext)
            g_controller->SelectSession(g_controller->GetActiveTab() + (target.type == HitTargetType::TabPrevious ? -1 : 1));
        else g_controller->HandleClick(target);
        g_dropdownIndex = 0; RefreshControls();
    }
    std::vector<Lattice::HitTestResult> FocusTargets()
    {
        using Lattice::HitTargetType;
        std::vector<Lattice::HitTestResult> targets;
        const auto add = [&](HitTargetType type, D2D1_RECT_F r, int index = -1)
        { if (r.right > r.left && r.bottom > r.top) targets.push_back({type, index, r}); };
        const auto sidebar = [&](HitTargetType type, D2D1_RECT_F r, int index = -1)
        {
            r.top = (std::max)(r.top, g_layoutMetrics.sidebarViewport.top);
            r.bottom = (std::min)(r.bottom, g_layoutMetrics.sidebarViewport.bottom); add(type, r, index);
        };
        add(HitTargetType::MenuProject, g_layoutMetrics.menuProject); add(HitTargetType::MenuSession, g_layoutMetrics.menuSession);
        add(HitTargetType::TitleSearch, g_layoutMetrics.titleSearch);
        add(HitTargetType::ActivityFiles, g_layoutMetrics.actBtnFiles); add(HitTargetType::ActivitySession, g_layoutMetrics.actBtnSession);
        add(HitTargetType::ActivityPlugins, g_layoutMetrics.actBtnPlugins);
        if (g_controller->GetSidebarMode() == Lattice::SidebarMode::Files)
        {
            sidebar(HitTargetType::SidebarProjectRow, g_layoutMetrics.sidebarProjectRow);
            for (size_t i = 0; i < g_layoutMetrics.sidebarFileRows.size(); ++i)
                sidebar(HitTargetType::SidebarFileRow, g_layoutMetrics.sidebarFileRows[i], static_cast<int>(i));
        }
        else if (g_controller->GetSidebarMode() == Lattice::SidebarMode::Session)
        {
            sidebar(HitTargetType::SidebarModelSelect, g_layoutMetrics.sidebarModelSelect);
            sidebar(HitTargetType::SidebarAgentSelect, g_layoutMetrics.sidebarAgentSelect);
            sidebar(HitTargetType::SidebarTogglePlan, g_layoutMetrics.sidebarTogglePlan);
            sidebar(HitTargetType::SidebarToggleSafeTools, g_layoutMetrics.sidebarToggleSafeTools);
            for (size_t i = 0; i < g_layoutMetrics.sidebarSkillRows.size(); ++i)
                sidebar(HitTargetType::SidebarSkillItem, g_layoutMetrics.sidebarSkillRows[i], static_cast<int>(i));
            sidebar(HitTargetType::SidebarAddSkill, g_layoutMetrics.sidebarAddSkillBtn);
        }
        else
        {
            for (size_t i = 0; i < g_layoutMetrics.sidebarPluginCards.size(); ++i)
                sidebar(HitTargetType::SidebarPluginCard, g_layoutMetrics.sidebarPluginCards[i], static_cast<int>(i));
            sidebar(HitTargetType::SidebarBrowsePlugins, g_layoutMetrics.sidebarBrowsePluginsBtn);
        }
        add(HitTargetType::TabPrevious, g_layoutMetrics.tabPreviousBtn);
        for (size_t i = 0; i < g_layoutMetrics.tabs.size(); ++i)
        {
            add(HitTargetType::TabItem, g_layoutMetrics.tabs[i], static_cast<int>(i));
            add(HitTargetType::TabClose, g_layoutMetrics.tabCloses[i], static_cast<int>(i));
        }
        add(HitTargetType::TabNext, g_layoutMetrics.tabNextBtn); add(HitTargetType::TabNew, g_layoutMetrics.tabNewBtn);
        add(HitTargetType::ComposerEdit, g_layoutMetrics.composerEditArea); add(HitTargetType::ComposerAttach, g_layoutMetrics.composerAttachBtn);
        add(HitTargetType::ComposerModelSelect, g_layoutMetrics.composerModelBtn); add(HitTargetType::ComposerContextChip, g_layoutMetrics.composerContextChip);
        add(HitTargetType::ComposerMic, g_layoutMetrics.composerMicBtn); add(HitTargetType::ComposerSend, g_layoutMetrics.composerSendBtn);
        add(HitTargetType::ActivitySettings, g_layoutMetrics.actBtnSettings);
        return targets;
    }
    void NavigateFocus(bool backwards)
    {
        const auto targets = FocusTargets(); if (targets.empty()) return;
        int current = -1;
        for (size_t i = 0; i < targets.size(); ++i)
            if ((GetFocus() == g_editControl && targets[i].type == Lattice::HitTargetType::ComposerEdit) ||
                (g_keyboardFocus && targets[i].type == g_keyboardTarget.type && targets[i].index == g_keyboardTarget.index))
                current = static_cast<int>(i);
        const int count = static_cast<int>(targets.size());
        const int next = backwards ? (current < 0 ? count - 1 : (current + count - 1) % count) : (current + 1) % count;
        g_keyboardTarget = targets[next]; g_keyboardFocus = true;
        SetFocus(g_keyboardTarget.type == Lattice::HitTargetType::ComposerEdit ? g_editControl : g_mainWindow);
        InvalidateRect(g_mainWindow, nullptr, FALSE);
    }
    bool HandleNavigationKey(WPARAM key)
    {
        if (!g_controller) return false;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key == VK_TAB && ctrl)
        {
            const int count = static_cast<int>(g_controller->GetTabs().size());
            SyncEditControlToController(); g_controller->CloseDropdown();
            if (count > 0)
                g_controller->SelectSession((g_controller->GetActiveTab() + count + (shift ? -1 : 1)) % count);
            RefreshControls(); return true;
        }
        if (g_controller->GetActiveDropdown() != Lattice::ActiveDropdown::None)
        {
            const int count = static_cast<int>(g_controller->GetCurrentDropdownItems().size());
            if (key == VK_ESCAPE || key == VK_TAB)
            { g_keyboardFocus = false; g_controller->CloseDropdown(); RefreshControls(); FocusComposer(); return true; }
            if (count && (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END))
            {
                g_dropdownIndex = key == VK_HOME ? 0 : key == VK_END ? count - 1 :
                    (g_dropdownIndex + count + (key == VK_UP ? -1 : 1)) % count;
                InvalidateRect(g_mainWindow, nullptr, FALSE); return true;
            }
            if (key == VK_RETURN || key == VK_SPACE)
            { g_keyboardFocus = false; g_controller->HandleDropdownSelect(g_dropdownIndex); RefreshControls(); FocusComposer(); return true; }
        }
        if (key == VK_TAB) { NavigateFocus(shift); return true; }
        if (GetFocus() == g_mainWindow && g_keyboardFocus && (key == VK_RETURN || key == VK_SPACE))
        { ExecuteTarget(g_keyboardTarget); return true; }
        if (GetFocus() == g_mainWindow && (key == VK_PRIOR || key == VK_NEXT || key == VK_HOME || key == VK_END))
        {
            if (key == VK_HOME) g_controller->SetActiveScrollOffset(0);
            else if (key == VK_END)
            { if (const auto* session = ActiveSessionFor(*g_controller)) g_controller->SetActiveScrollOffset(session->maxScroll); }
            else g_controller->ScrollActiveTab(key == VK_PRIOR ? 4.0f : -4.0f);
            RefreshControls(); return true;
        }
        return false;
    }
    bool HandleCommand(WORD command)
    {
        if (!g_controller || command < NewSessionCommand || command > FindNextCommand) return false;
        SyncEditControlToController();
        if (command == ProjectMenuCommand) { OpenMenu(Lattice::ActiveDropdown::Project); return true; }
        if (command == SessionMenuCommand) { OpenMenu(Lattice::ActiveDropdown::Session); return true; }
        g_controller->CloseDropdown();
        switch (command)
        {
        case NewSessionCommand: g_controller->NewSession(); break;
        case OpenSessionCommand: g_controller->OpenSession(); break;
        case SaveSessionCommand: g_controller->SaveSession(); break;
        case CloseSessionCommand: g_controller->CloseSession(g_controller->GetActiveTab()); break;
        case NewProjectCommand: g_controller->NewProject(); break;
        case CloseProjectCommand: g_controller->CloseProject(); break;
        case SearchCommand: OpenSearch(); break;
        case FocusComposerCommand: RefreshControls(); FocusComposer(); break;
        case FilesCommand: g_controller->HandleClick({Lattice::HitTargetType::ActivityFiles}); break;
        case SessionControlsCommand: g_controller->HandleClick({Lattice::HitTargetType::ActivitySession}); break;
        case PluginsCommand: g_controller->HandleClick({Lattice::HitTargetType::ActivityPlugins}); break;
        case FindNextCommand:
            if (!*g_findText) OpenSearch();
            else if (!FindNext((g_find.Flags & FR_MATCHCASE) != 0)) MessageBeep(MB_ICONINFORMATION);
            break;
        default: return false;
        }
        g_keyboardFocus = false; RefreshControls();
        if (command == NewSessionCommand || command == OpenSessionCommand || command == CloseSessionCommand) FocusComposer();
        return true;
    }
    HACCEL CreateAppAccelerators()
    {
        ACCEL items[] = {
            {FVIRTKEY | FCONTROL, 'N', NewSessionCommand}, {FVIRTKEY | FCONTROL, 'O', OpenSessionCommand},
            {FVIRTKEY | FCONTROL, 'S', SaveSessionCommand}, {FVIRTKEY | FCONTROL, 'W', CloseSessionCommand},
            {FVIRTKEY | FCONTROL | FSHIFT, 'N', NewProjectCommand}, {FVIRTKEY | FCONTROL | FSHIFT, 'W', CloseProjectCommand},
            {FVIRTKEY | FCONTROL, 'F', SearchCommand}, {FVIRTKEY | FALT, 'P', ProjectMenuCommand},
            {FVIRTKEY | FALT, 'S', SessionMenuCommand}, {FVIRTKEY | FCONTROL, 'L', FocusComposerCommand},
            {FVIRTKEY | FCONTROL, '1', FilesCommand}, {FVIRTKEY | FCONTROL, '2', SessionControlsCommand},
            {FVIRTKEY | FCONTROL, '3', PluginsCommand}, {FVIRTKEY, VK_F3, FindNextCommand}
        };
        return CreateAcceleratorTableW(items, static_cast<int>(std::size(items)));
    }
    bool PreTranslateAppMessage(MSG& msg)
    {
        if (g_findDialog && IsDialogMessageW(g_findDialog, &msg)) return true;
        return g_accelerators && TranslateAcceleratorW(g_mainWindow, g_accelerators, &msg);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_findMessage && message == g_findMessage)
        {
            const auto* find = reinterpret_cast<const FINDREPLACEW*>(lParam);
            if (find->Flags & FR_DIALOGTERM) g_findDialog = nullptr;
            else if (find->Flags & FR_FINDNEXT)
            {
                if (!FindNext((find->Flags & FR_MATCHCASE) != 0))
                    MessageBoxW(g_findDialog, L"No matching conversation text was found.",
                        L"Find in conversations", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        switch (message)
        {
        case WM_NCCALCSIZE:
            // Cover initial creation (RECT) as well as later frame changes
            // (NCCALCSIZE_PARAMS) with the app's custom header and resize edges.
            return 0;
        case WM_NCHITTEST:
        {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &p);
            RECT r{}; GetClientRect(hwnd, &r);
            const int border = static_cast<int>(6 * Scale());
            if (!IsZoomed(hwnd))
            {
                const bool left = p.x < border, right = p.x >= r.right - border;
                const bool top = p.y < border, bottom = p.y >= r.bottom - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            if (g_controller && p.y / Scale() < g_layoutMetrics.titlebar.bottom &&
                g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None)
            {
                const auto hit = Lattice::UIComponents::HitTest(g_layoutMetrics, p.x / Scale(), p.y / Scale(),
                    g_controller->GetSidebarMode(), g_controller->GetActiveDropdown(), g_controller->GetCurrentDropdownItems());
                if (hit.type == Lattice::HitTargetType::None) return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { static_cast<LONG>(860 * Scale()), static_cast<LONG>(580 * Scale()) };
            MONITORINFO monitor{ sizeof(MONITORINFO) };
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                info->ptMaxPosition = { monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top };
                info->ptMaxSize = { monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top };
            }
            return 0;
        }
        case WM_DPICHANGED:
        {
            g_dpi = static_cast<float>(HIWORD(wParam));
            if (g_d2dContext) g_d2dContext->SetDpi(g_dpi);
            UpdateEditFont();
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshControls();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CREATE:
        {
            g_mainWindow = hwnd;
            g_dpi = static_cast<float>(GetDpiForWindow(hwnd));
            g_d2dContext = std::make_unique<Lattice::Direct2DContext>();
            if (!g_d2dContext->Initialize(hwnd))
            {
                MessageBoxW(hwnd, L"Direct2D initialization failed.", L"Error", MB_OK | MB_ICONERROR);
                return -1;
            }
            g_d2dContext->SetDpi(g_dpi);

            g_controller = std::make_unique<Lattice::AppController>();
            g_controller->Initialize(hwnd);

            // Edit control background brush (#202329 -> RGB 32, 35, 41)
            g_editBackgroundBrush = CreateSolidBrush(RGB(32, 35, 41));

            // Edit control font
            UpdateEditFont();

            // Create multi-line edit control inside composer
            g_editControl = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                0, 0, 100, 40,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(201)),
                reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance, nullptr);

            if (g_editControl)
            {
                SendMessageW(g_editControl, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
                SendMessageW(g_editControl, EM_SETLIMITTEXT, 1024 * 1024, 0);
                g_originalEditProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(g_editControl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassedEditProc)));
            }

            if (!g_editControl || !g_editBackgroundBrush) return -1;
            g_findMessage = RegisterWindowMessageW(FINDMSGSTRINGW);
            g_accelerators = CreateAppAccelerators();
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, AnimTimerId, 50, nullptr);
            return 0;
        }

        case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED) return 0;
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (g_d2dContext)
            {
                g_d2dContext->Resize(width, height);
            }
            UpdateLayoutAndControls(width, height);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CTLCOLOREDIT:
        {
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == g_editControl)
            {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdc, RGB(226, 229, 234)); // #e2e5ea
                SetBkColor(hdc, RGB(32, 35, 41));     // #202329
                return reinterpret_cast<INT_PTR>(g_editBackgroundBrush);
            }
            break;
        }

        case WM_COMMAND:
        {
            if (LOWORD(wParam) == 201 && HIWORD(wParam) == EN_CHANGE)
            {
                SyncEditControlToController();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (HandleCommand(LOWORD(wParam))) return 0;
            break;
        }

        case WM_MOUSEMOVE:
        {
            if (!g_mouseTracking)
            {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                g_mouseTracking = true;
            }

            float x = static_cast<float>(GET_X_LPARAM(lParam)) / Scale();
            float y = static_cast<float>(GET_Y_LPARAM(lParam)) / Scale();
            if (g_dragConversation || g_dragSidebar)
            {
                const auto track = g_dragSidebar ? g_layoutMetrics.sidebarViewport : g_layoutMetrics.scrollbarTrack;
                const auto thumb = g_dragSidebar ? g_layoutMetrics.sidebarScrollbarThumb : g_layoutMetrics.scrollbarThumb;
                const float travel = track.bottom - track.top - (thumb.bottom - thumb.top);
                const auto* session = ActiveSessionFor(*g_controller);
                const float maxScroll = g_dragSidebar ? g_layoutMetrics.sidebarMaxScroll :
                    (session ? session->maxScroll : 0.0f);
                const float offset = travel > 0 ?
                    (std::clamp)(y - g_dragAnchor - track.top, 0.0f, travel) / travel * maxScroll : 0.0f;
                if (g_dragSidebar) g_controller->SetSidebarScrollOffset(offset);
                else g_controller->SetActiveScrollOffset(offset);
                RefreshControls();
                return 0;
            }

            Lattice::HitTestResult target = Lattice::UIComponents::HitTest(
                g_layoutMetrics, x, y,
                g_controller->GetSidebarMode(),
                g_controller->GetActiveDropdown(),
                g_controller->GetCurrentDropdownItems());

            if (target.type != g_hoveredTarget.type || target.index != g_hoveredTarget.index ||
                (target.type == Lattice::HitTargetType::DropdownItem && target.index != g_dropdownIndex))
            {
                g_hoveredTarget = target;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (target.type == Lattice::HitTargetType::DropdownItem) g_dropdownIndex = target.index;

            if (target.type != Lattice::HitTargetType::None)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
            }
            else
            {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            return 0;
        }

        case WM_MOUSELEAVE:
        {
            g_mouseTracking = false;
            g_hoveredTarget = Lattice::HitTestResult{};
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            g_keyboardFocus = false;
            SyncEditControlToController();
            SetFocus(hwnd);

            float x = static_cast<float>(GET_X_LPARAM(lParam)) / Scale();
            float y = static_cast<float>(GET_Y_LPARAM(lParam)) / Scale();
            if (g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None)
            {
                if (Contains(g_layoutMetrics.scrollbarThumb, x, y) || Contains(g_layoutMetrics.sidebarScrollbarThumb, x, y))
                {
                    g_dragSidebar = Contains(g_layoutMetrics.sidebarScrollbarThumb, x, y);
                    g_dragConversation = !g_dragSidebar;
                    g_dragAnchor = y - (g_dragSidebar ? g_layoutMetrics.sidebarScrollbarThumb.top : g_layoutMetrics.scrollbarThumb.top);
                    SetCapture(hwnd); return 0;
                }
                if (Contains(g_layoutMetrics.scrollbarTrack, x, y))
                {
                    g_controller->ScrollActiveTab(y < g_layoutMetrics.scrollbarThumb.top ? 4.0f : -4.0f);
                    RefreshControls(); return 0;
                }
                if (Contains(g_layoutMetrics.sidebarScrollbarTrack, x, y))
                {
                    g_controller->ScrollSidebar(y < g_layoutMetrics.sidebarScrollbarThumb.top ? 4.0f : -4.0f);
                    RefreshControls(); return 0;
                }
            }

            Lattice::HitTestResult target = Lattice::UIComponents::HitTest(
                g_layoutMetrics, x, y,
                g_controller->GetSidebarMode(),
                g_controller->GetActiveDropdown(),
                g_controller->GetCurrentDropdownItems());

            ExecuteTarget(target);

            // If user clicked inside the composer box, focus the edit control
            if (target.type == Lattice::HitTargetType::None && Contains(g_layoutMetrics.composerEditArea, x, y))
            {
                FocusComposer();
            }

            return 0;
        }
        case WM_LBUTTONUP:
            if (g_dragSidebar || g_dragConversation)
            { g_dragSidebar = g_dragConversation = false; ReleaseCapture(); }
            return 0;
        case WM_CAPTURECHANGED:
            g_dragSidebar = g_dragConversation = false;
            return 0;

        case WM_MOUSEWHEEL:
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; ScreenToClient(hwnd, &point);
            const float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
            if (Contains(g_layoutMetrics.sidebar, point.x / Scale(), point.y / Scale())) g_controller->ScrollSidebar(delta);
            else if (Contains(g_layoutMetrics.conversationArea, point.x / Scale(), point.y / Scale())) g_controller->ScrollActiveTab(delta);
            RefreshControls();
            return 0;
        }

        case WM_KEYDOWN:
        {
            if (HandleNavigationKey(wParam)) return 0;
            break;
        }
        case WM_DROPFILES:
        {
            const auto drop = reinterpret_cast<HDROP>(wParam);
            std::vector<std::wstring> paths;
            const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
            for (UINT i = 0; i < count && i < 128; ++i)
            {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(static_cast<size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1); path.resize(length);
                paths.push_back(std::move(path));
            }
            DragFinish(drop); SyncEditControlToController();
            g_controller->AttachFilePaths(paths); RefreshControls(); FocusComposer();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && g_controller)
            { g_controller->CloseDropdown(); RefreshControls(); }
            return 0;

        case WM_TIMER:
        {
            if (wParam == AnimTimerId)
            {
                g_animTick += 0.05f;
                if (g_controller)
                {
                    g_controller->OnTimer();

                    SyncControllerToEdit();
                }
            }
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);

            if (g_d2dContext && g_controller)
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                UpdateLayoutAndControls(rc.right - rc.left, rc.bottom - rc.top);
                if (!g_d2dContext->BeginDraw())
                { EndPaint(hwnd, &ps); return 0; }
                auto hovered = g_keyboardFocus ? g_keyboardTarget : g_hoveredTarget;
                if (g_controller->GetActiveDropdown() != Lattice::ActiveDropdown::None &&
                    g_dropdownIndex >= 0 && g_dropdownIndex < static_cast<int>(g_layoutMetrics.dropdownItems.size()))
                    hovered = { Lattice::HitTargetType::DropdownItem, g_dropdownIndex, g_layoutMetrics.dropdownItems[g_dropdownIndex] };

                Lattice::UIComponents::Render(
                    *g_d2dContext,
                    g_layoutMetrics,
                    g_controller->GetSidebarMode(),
                    g_controller->GetActiveDropdown(),
                    g_controller->GetCurrentDropdownItems(),
                    g_controller->GetTabs(),
                    g_controller->GetActiveTab(),
                    g_controller->GetFiles(),
                    g_controller->GetSkills(),
                    g_controller->GetPlugins(),
                    g_controller->GetSelectedModel(),
                    g_controller->GetPlanBeforeEdits(),
                    g_controller->GetAutoRunSafeTools(),
                    g_controller->GetIsListening(),
                    g_controller->GetDraftText(),
                    hovered,
                    g_animTick,
                    g_controller->IsGeneratingReply(), g_controller->GetCurrentProjectName());

                if (g_keyboardFocus && g_keyboardTarget.type != Lattice::HitTargetType::ComposerEdit &&
                    g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None)
                    g_d2dContext->DrawRoundedRect(g_keyboardTarget.rect, 3.0f, Lattice::Colors::ActivityActivePill);
                if (GetFocus() == g_editControl && ActiveSessionFor(*g_controller))
                    g_d2dContext->DrawRoundedRect(g_layoutMetrics.composerBox, 7.0f, Lattice::Colors::ComposerBorderFocus);
                if (g_d2dContext->EndDraw() == D2DERR_RECREATE_TARGET) InvalidateRect(hwnd, nullptr, FALSE);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            SyncEditControlToController();
            if (!g_controller || g_controller->CanCloseApplication()) DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            SyncEditControlToController();
            return !g_controller || g_controller->CanCloseApplication();
        case WM_DESTROY:
        {
            KillTimer(hwnd, AnimTimerId);
            if (g_findDialog) DestroyWindow(g_findDialog);
            g_findDialog = nullptr;
            if (g_accelerators) DestroyAcceleratorTable(g_accelerators);
            g_accelerators = nullptr;
            if (g_editBackgroundBrush)
            {
                DeleteObject(g_editBackgroundBrush);
                g_editBackgroundBrush = nullptr;
            }
            if (g_editFont)
            {
                DeleteObject(g_editFont);
                g_editFont = nullptr;
            }
            g_d2dContext.reset();
            g_controller.reset();
            g_mainWindow = g_editControl = nullptr;
            g_originalEditProc = nullptr;
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

#ifndef LATTICE_TESTING
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    // Enable Per-Monitor v2 DPI awareness for crystal-clear text & icons
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    std::wstring outPath;
    std::wstring newDialogPath;
    for (int i = 1; argv && i < argc; ++i)
    {
        args.emplace_back(argv[i]);
        if (args.back().rfind(L"--screenshot=", 0) == 0) outPath = args.back().substr(13);
        else if (args.back() == L"--screenshot") outPath = L"screenshot_production.png";
        else if (args.back().rfind(L"--screenshot-new=", 0) == 0) newDialogPath = args.back().substr(17);
    }
    if (argv) LocalFree(argv);
    const auto hasArg = [&](const wchar_t* value)
    { return std::find(args.begin(), args.end(), value) != args.end(); };
    if (!newDialogPath.empty())
    {
        Lattice::ItemTypeRegistry registry;
        Lattice::NewItemDialogOptions options;
        options.types = registry.Types();
        if (hasArg(L"--category=files")) options.initialCategory = Lattice::ItemCategory::Files;
        else if (hasArg(L"--category=sessions")) options.initialCategory = Lattice::ItemCategory::Sessions;
        std::wstring error;
        const bool saved = Lattice::SaveNewItemDialogPreview(options, newDialogPath, error);
        if (!saved) OutputDebugStringW(error.c_str());
        CoUninitialize();
        return saved ? 0 : 1;
    }
    if (!outPath.empty())
    {
        auto d2d = std::make_unique<Lattice::Direct2DContext>();
        if (!d2d->Initialize(nullptr)) { CoUninitialize(); return 1; }

        auto ctrl = std::make_unique<Lattice::AppController>();
        ctrl->Initialize(nullptr);
        if (hasArg(L"--new-session")) ctrl->NewSession();

        UINT w = 1140;
        UINT h = 760;
        Lattice::LayoutMetrics layout;
        Lattice::UIComponents::ComputeLayout(
            layout, static_cast<float>(w), static_cast<float>(h),
            ctrl->GetSidebarMode(), ctrl->GetTabs(), ctrl->GetActiveTab(),
            ctrl->GetFiles(), ctrl->GetSkills(), ctrl->GetPlugins(),
            ctrl->GetActiveDropdown(), ctrl->GetCurrentDropdownItems(), 0.0f, true,
            !ctrl->GetCurrentProjectPath().empty());

        if (hasArg(L"--sidebar=session"))
        {
            ctrl->HandleClick({ Lattice::HitTargetType::ActivitySession, -1, {} });
        }
        else if (hasArg(L"--sidebar=plugins"))
        {
            ctrl->HandleClick({ Lattice::HitTargetType::ActivityPlugins, -1, {} });
        }

        if (hasArg(L"--menu=project"))
        {
            ctrl->SetActiveDropdown(Lattice::ActiveDropdown::Project);
        }
        else if (hasArg(L"--menu=session"))
        {
            ctrl->SetActiveDropdown(Lattice::ActiveDropdown::Session);
        }
        else if (hasArg(L"--menu=model"))
            ctrl->SetActiveDropdown(Lattice::ActiveDropdown::Model);
        else if (hasArg(L"--menu=sidebar-model"))
        {
            ctrl->HandleClick({Lattice::HitTargetType::ActivitySession});
            ctrl->SetActiveDropdown(Lattice::ActiveDropdown::SidebarModel);
        }

        Lattice::UIComponents::ComputeLayout(
            layout, static_cast<float>(w), static_cast<float>(h),
            ctrl->GetSidebarMode(), ctrl->GetTabs(), ctrl->GetActiveTab(),
            ctrl->GetFiles(), ctrl->GetSkills(), ctrl->GetPlugins(),
            ctrl->GetActiveDropdown(), ctrl->GetCurrentDropdownItems(), 0.0f, true,
            !ctrl->GetCurrentProjectPath().empty());
        if (const auto* session = ActiveSessionFor(*ctrl))
            Lattice::UIComponents::UpdateConversationScrollMetrics(layout,
                Lattice::UIComponents::MeasureConversationHeight(*d2d, layout, *session), session->scrollOffset);

        bool saved = d2d->SaveRenderToPng(outPath, w, h, [&]() {
            Lattice::UIComponents::Render(
                *d2d, layout,
                ctrl->GetSidebarMode(), ctrl->GetActiveDropdown(),
                ctrl->GetCurrentDropdownItems(), ctrl->GetTabs(),
                ctrl->GetActiveTab(), ctrl->GetFiles(), ctrl->GetSkills(),
                ctrl->GetPlugins(), ctrl->GetSelectedModel(),
                ctrl->GetPlanBeforeEdits(), ctrl->GetAutoRunSafeTools(),
                ctrl->GetIsListening(), ctrl->GetDraftText(),
                Lattice::HitTestResult{}, 0.0f, false, ctrl->GetCurrentProjectName());
        });

        ctrl.reset(); d2d.reset();
        CoUninitialize();
        return saved ? 0 : 1;
    }

    // Initialize common controls
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSW windowClass{};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr; // Direct2D handles drawing background
    windowClass.lpszClassName = WindowClassName;

    if (!RegisterClassW(&windowClass))
    {
        MessageBoxW(nullptr, L"Could not register the window class.", Lattice::AppIdentity::Name, MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    const float initialScale = GetDpiForSystem() / 96.0f;
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        WindowClassName,
        Lattice::AppIdentity::Name,
        MainWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(1140 * initialScale), static_cast<int>(760 * initialScale),
        nullptr, nullptr, instance, nullptr);

    if (!window)
    {
        MessageBoxW(nullptr, L"Could not create the main application window.", Lattice::AppIdentity::Name, MB_OK | MB_ICONERROR);
        g_d2dContext.reset(); g_controller.reset();
        CoUninitialize();
        return 1;
    }

    // Recalculate the initial native frame before the custom header is shown.
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Match remaining system frame accents to the native dark theme.
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(window, 19 /* fallback */, &darkMode, sizeof(darkMode));

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    FocusComposer();

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        if (!PreTranslateAppMessage(message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    g_d2dContext.reset(); g_controller.reset();
    CoUninitialize();
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
#endif
