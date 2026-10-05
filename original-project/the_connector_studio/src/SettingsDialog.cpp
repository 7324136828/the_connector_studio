#include "SettingsDialog.h"
#include <windowsx.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace Lattice
{
    namespace
    {
        using namespace SettingsDialogControls;
        using Microsoft::WRL::ComPtr;
        constexpr wchar_t DialogClass[] = L"ConnectorStudioSettingsDialog";
        constexpr COLORREF Background = RGB(21, 23, 28), Field = RGB(32, 35, 41), Border = RGB(58, 62, 72);
        constexpr COLORREF Text = RGB(216, 222, 233), Muted = RGB(137, 145, 159), Accent = RGB(112, 123, 237);
        constexpr COLORREF ErrorColor = RGB(244, 151, 151), SuccessColor = RGB(110, 193, 139), WarningColor = RGB(217, 168, 93);
        struct Completion { bool saved = false; };
        struct TestJob
        {
            std::shared_ptr<Connector::Cancellation> cancel = std::make_shared<Connector::Cancellation>();
            Connector::DiscoveryResult result;
            std::atomic<bool> done{false};
        };
        struct DialogState
        {
            HWND window = nullptr, composingEdit = nullptr;
            Connector::Settings settings;
            SaveConnectorSettings save;
            std::shared_ptr<Completion> completion;
            std::shared_ptr<TestJob> job;
            std::thread worker;
            COLORREF statusColor = Muted;
            UINT dpi = 96;
            bool owned = false, saving = false, compact = false;
            HFONT bodyFont = nullptr, boldFont = nullptr, titleFont = nullptr, smallFont = nullptr;
            HBRUSH backgroundBrush = nullptr, fieldBrush = nullptr;
            std::vector<float> fontSizes{11.5f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f, 24.0f};
            HWND Control(int id) const { return GetDlgItem(window, id); }
            int Px(float dip) const { return static_cast<int>(std::lround(dip * dpi / 96.0f)); }
            void CancelTest()
            {
                if (job) job->cancel->Cancel();
                // Workers own their result/cancellation through shared_ptr, never this
                // state or any HWND. Closing or editing the URL cannot wait on HTTP.
                if (worker.joinable()) worker.detach();
                job.reset();
                if (window) KillTimer(window, PollTimer);
            }
            ~DialogState()
            {
                CancelTest();
                for (auto font : {bodyFont, boldFont, titleFont, smallFont}) if (font) DeleteObject(font);
                for (auto brush : {backgroundBrush, fieldBrush}) if (brush) DeleteObject(brush);
            }
        };
        RECT R(DialogState& state, float x, float y, float width, float height)
        { return RECT{state.Px(x), state.Px(y), state.Px(x + width), state.Px(y + height)}; }
        void Fill(HDC dc, const RECT& rect, COLORREF color)
        { const HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush); }
        void Outline(HDC dc, RECT rect, COLORREF color, int thickness = 1)
        {
            const HBRUSH brush = CreateSolidBrush(color);
            for (int i = 0; i < thickness; ++i) { FrameRect(dc, &rect, brush); InflateRect(&rect, -1, -1); }
            DeleteObject(brush);
        }
        void Label(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT format)
        {
            const auto previous = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format); SelectObject(dc, previous);
        }
        std::wstring Read(HWND window)
        {
            const int length = GetWindowTextLengthW(window);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            text.resize(static_cast<size_t>((std::max)(0, GetWindowTextW(window, text.data(), length + 1)))); return text;
        }
        std::wstring FontLabel(float size)
        {
            std::wostringstream value; value << std::setprecision(4) << size;
            return value.str() + (size == 11.5f ? L" (Original)" : L"");
        }
        void UpdateButtons(DialogState& state)
        {
            EnableWindow(state.Control(TestConnection), !state.saving && !state.job);
            EnableWindow(state.Control(ServerUrl), !state.saving);
            EnableWindow(state.Control(ChatFontSize), !state.saving);
            EnableWindow(state.Control(Save), !state.saving && static_cast<bool>(state.save));
        }
        void ShowStatus(DialogState& state, const std::wstring& text, COLORREF color = Muted)
        {
            state.statusColor = color; SetWindowTextW(state.Control(Status), text.c_str());
            InvalidateRect(state.Control(Status), nullptr, TRUE); InvalidateRect(state.window, nullptr, FALSE);
        }
        bool ReadSettings(DialogState& state, Connector::Settings& settings)
        {
            settings = state.settings;
            std::wstring error;
            if (!Connector::NormalizeServerUrl(Read(state.Control(ServerUrl)), settings.serverUrl, error))
            { ShowStatus(state, error.empty() ? L"Enter an HTTP or HTTPS connector server URL." : error, ErrorColor); SetFocus(state.Control(ServerUrl)); return false; }
            const auto selection = SendMessageW(state.Control(ChatFontSize), CB_GETCURSEL, 0, 0);
            if (selection == CB_ERR || selection < 0 || static_cast<size_t>(selection) >= state.fontSizes.size())
            { ShowStatus(state, L"Choose a chat font size.", ErrorColor); SetFocus(state.Control(ChatFontSize)); return false; }
            settings.chatFontSize = state.fontSizes[static_cast<size_t>(selection)];
            return true;
        }
        void StartTest(DialogState& state)
        {
            if (state.saving || state.job) return;
            Connector::Settings settings;
            if (!ReadSettings(state, settings)) return;
            try
            {
                state.job = std::make_shared<TestJob>(); const auto job = state.job;
                state.worker = std::thread([job, settings] {
                    try { job->result = Connector::DiscoverModels(settings, job->cancel); }
                    catch (...) { job->result.error = L"The connection test could not be completed."; }
                    job->done.store(true, std::memory_order_release);
                });
                if (!SetTimer(state.window, PollTimer, 30, nullptr))
                { state.CancelTest(); ShowStatus(state, L"Windows could not start the connection test.", ErrorColor); UpdateButtons(state); return; }
                ShowStatus(state, L"Testing connection..."); UpdateButtons(state);
            }
            catch (...) { state.CancelTest(); ShowStatus(state, L"The connection test could not be started.", ErrorColor); UpdateButtons(state); }
        }
        void PollTest(DialogState& state)
        {
            if (!state.job || !state.job->done.load(std::memory_order_acquire)) return;
            if (state.worker.joinable()) state.worker.join();
            const auto job = state.job; state.job.reset(); KillTimer(state.window, PollTimer); UpdateButtons(state);
            if (job->result.cancelled) { ShowStatus(state, L"Connection test canceled."); return; }
            if (!job->result.success)
            { ShowStatus(state, job->result.error.empty() ? L"The server could not be reached." : job->result.error, ErrorColor); return; }
            if (job->result.models.empty())
                ShowStatus(state, L"Connected. The server reports no active models. You can still save this URL.", WarningColor);
            else
                ShowStatus(state, L"Connected. " + std::to_wstring(job->result.models.size()) +
                    (job->result.models.size() == 1 ? L" active model is available." : L" active models are available.") +
                    L" Save, then choose a model in Studio.", SuccessColor);
        }
        void SaveSettings(DialogState& state)
        {
            if (state.saving) return;
            Connector::Settings settings;
            if (!ReadSettings(state, settings)) return;
            if (!state.save) { ShowStatus(state, L"Saving settings is unavailable.", ErrorColor); return; }
            state.CancelTest();
            state.saving = true; UpdateButtons(state);
            bool saved = false; std::wstring error;
            try { saved = state.save(settings, error); }
            catch (...) { error = L"The settings could not be saved. Try again."; }
            state.saving = false;
            if (saved) { state.completion->saved = true; state.CancelTest(); DestroyWindow(state.window); return; }
            UpdateButtons(state); ShowStatus(state, error.empty() ? L"The settings could not be saved. Try again." : error, ErrorColor);
        }
        void MakeFonts(DialogState& state)
        {
            const auto make = [&](int size, int weight) { return CreateFontW(-state.Px(static_cast<float>(size)), 0, 0, 0, weight,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"); };
            for (auto font : {state.bodyFont, state.boldFont, state.titleFont, state.smallFont}) if (font) DeleteObject(font);
            state.bodyFont = make(13, FW_NORMAL); state.boldFont = make(13, FW_SEMIBOLD);
            state.titleFont = make(21, FW_SEMIBOLD); state.smallFont = make(11, FW_NORMAL);
            for (HWND child = GetWindow(state.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.bodyFont), TRUE);
            SendMessageW(state.Control(ServerUrlLabel), WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
            SendMessageW(state.Control(ChatFontSizeLabel), WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
            SendMessageW(state.Control(ChatFontSize), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), state.Px(26));
            SendMessageW(state.Control(ChatFontSize), CB_SETITEMHEIGHT, 0, state.Px(26));
            for (int id : {Example, ModelHelp}) SendMessageW(state.Control(id), WM_SETFONT, reinterpret_cast<WPARAM>(state.smallFont), TRUE);
        }
        void Layout(DialogState& state)
        {
            RECT client{}; GetClientRect(state.window, &client);
            const float width = client.right * 96.0f / state.dpi, height = client.bottom * 96.0f / state.dpi;
            state.compact = height < 400;
            const auto move = [&](int id, float x, float y, float w, float h) {
                const auto rect = R(state, x, y, w, h);
                MoveWindow(state.Control(id), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
            };
            const float urlTop = state.compact ? 74.0f : 106.0f;
            move(Close, width - 44, 6, 36, 32);
            move(ServerUrlLabel, 24, state.compact ? 48.0f : 76.0f, width - 48, 22);
            move(ServerUrl, 34, urlTop + 8, width - 68, 20);
            move(Example, 24, urlTop + (state.compact ? 40.0f : 48.0f), width - 48, state.compact ? 20.0f : 30.0f);
            move(ChatFontSizeLabel, 24, state.compact ? 144.0f : 194.0f, 140, 24);
            move(ChatFontSize, state.compact ? 176.0f : 24.0f, state.compact ? 140.0f : 222.0f, 160, 220);
            move(TestConnection, 24, state.compact ? 178.0f : 266.0f, 158, state.compact ? 30.0f : 34.0f);
            const float statusTop = state.compact ? 216.0f : 314.0f;
            move(Status, 24, statusTop, width - 48, (std::max)(30.0f, height - statusTop - (state.compact ? 82.0f : 112.0f)));
            move(ModelHelp, 24, height - 101, width - 48, 24);
            ShowWindow(state.Control(ModelHelp), height < 460.0f ? SW_HIDE : SW_SHOWNA);
            move(Cancel, width - 224, height - 54, 92, 32); move(Save, width - 120, height - 54, 96, 32);
            UpdateButtons(state); InvalidateRect(state.window, nullptr, FALSE);
        }
        void Paint(DialogState& state, HDC dc)
        {
            RECT client{}; GetClientRect(state.window, &client);
            const float width = client.right * 96.0f / state.dpi, height = client.bottom * 96.0f / state.dpi;
            Fill(dc, client, Background); Fill(dc, R(state, 0, 0, width, state.compact ? 40.0f : 48.0f), RGB(25, 27, 32));
            Label(dc, L"Settings", R(state, 24, 8, width - 100, 32), state.titleFont, Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            const auto field = R(state, 24, state.compact ? 74.0f : 106.0f, width - 48, 36);
            Fill(dc, field, Field); Outline(dc, field, GetFocus() == state.Control(ServerUrl) ? Accent : Border, state.Px(1));
            Fill(dc, R(state, 24, height - 66, width - 48, 1), Border); Outline(dc, client, Border);
        }
        void DrawButton(DialogState& state, const DRAWITEMSTRUCT& draw)
        {
            RECT rect = draw.rcItem; const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
            if (draw.CtlType == ODT_COMBOBOX)
            {
                Fill(draw.hDC, rect, (draw.itemState & ODS_SELECTED) ? RGB(48, 57, 92) : Field);
                if (draw.itemID < state.fontSizes.size())
                {
                    rect.left += state.Px(8);
                    Label(draw.hDC, FontLabel(state.fontSizes[draw.itemID]), rect, state.bodyFont, Text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                return;
            }
            COLORREF color = draw.CtlID == Save ? (disabled ? Border : Accent) : Field;
            if (draw.itemState & ODS_SELECTED) color = RGB(48, 57, 92);
            Fill(draw.hDC, rect, color);
            if (draw.CtlID != Close) Outline(draw.hDC, rect, Border);
            Label(draw.hDC, draw.CtlID == Close ? L"\x00d7" : Read(draw.hwndItem), rect,
                draw.CtlID == Save ? state.boldFont : state.bodyFont, disabled ? Muted : draw.CtlID == Save ? RGB(255, 255, 255) : Text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (draw.itemState & ODS_FOCUS) { InflateRect(&rect, -state.Px(4), -state.Px(4)); Outline(draw.hDC, rect, RGB(151, 162, 231)); }
        }
        void DrawFontSelector(DialogState& state, HWND window, HDC dc)
        {
            RECT rect{}; GetClientRect(window, &rect);
            Fill(dc, rect, Field); Outline(dc, rect, GetFocus() == window ? Accent : Border, state.Px(1));
            const auto selection = SendMessageW(window, CB_GETCURSEL, 0, 0);
            auto textRect = rect; textRect.left += state.Px(8); textRect.right -= state.Px(30);
            if (selection >= 0 && static_cast<size_t>(selection) < state.fontSizes.size())
                Label(dc, FontLabel(state.fontSizes[static_cast<size_t>(selection)]), textRect, state.bodyFont,
                    IsWindowEnabled(window) ? Text : Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            auto arrow = rect; arrow.left = arrow.right - state.Px(28);
            Label(dc, L"\x2304", arrow, state.bodyFont, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        LRESULT CALLBACK ChildProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
        {
            auto& state = *reinterpret_cast<DialogState*>(data);
            const auto id = GetDlgCtrlID(window);
            if (id == ChatFontSize && (message == WM_PRINT || message == WM_PRINTCLIENT))
            { DrawFontSelector(state, window, reinterpret_cast<HDC>(wParam)); return 0; }
            if (id == ChatFontSize && message == WM_PAINT)
            {
                PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
                DrawFontSelector(state, window, dc); EndPaint(window, &paint); return 0;
            }
            if (message == WM_IME_STARTCOMPOSITION) state.composingEdit = window;
            if ((message == WM_IME_ENDCOMPOSITION || message == WM_NCDESTROY) && state.composingEdit == window) state.composingEdit = nullptr;
            if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(state.window, nullptr, FALSE);
            if (message == WM_GETDLGCODE)
            {
                const auto code = DefSubclassProc(window, message, wParam, lParam);
                const auto* key = reinterpret_cast<const MSG*>(lParam);
                if (key && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE)) return code | DLGC_WANTMESSAGE;
                return code;
            }
            if (message == WM_KEYDOWN)
            {
                if (state.composingEdit == window && (wParam == VK_RETURN || wParam == VK_ESCAPE)) return DefSubclassProc(window, message, wParam, lParam);
                if (id == ChatFontSize && (wParam == VK_RETURN || wParam == VK_ESCAPE) && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0))
                    return DefSubclassProc(window, message, wParam, lParam);
                if (wParam == VK_ESCAPE) { if (!state.saving) DestroyWindow(state.window); return 0; }
                if (wParam == VK_RETURN)
                { if (id == ServerUrl || id == ChatFontSize) SaveSettings(state); else SendMessageW(window, BM_CLICK, 0, 0); return 0; }
            }
            if (message == WM_CHAR && state.composingEdit != window && (wParam == VK_RETURN || wParam == VK_ESCAPE)) return 0;
            if ((message == WM_PRINT || message == WM_PRINTCLIENT) && GetDlgCtrlID(window) == ServerUrl)
            {
                const HDC dc = reinterpret_cast<HDC>(wParam); RECT client{}, format{};
                GetClientRect(window, &client); Fill(dc, client, Field);
                SendMessageW(window, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
                auto text = Read(window); const auto first = SendMessageW(window, EM_GETFIRSTVISIBLELINE, 0, 0);
                if (first > 0 && static_cast<size_t>(first) < text.size()) text.erase(0, static_cast<size_t>(first));
                const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
                Label(dc, text, format, font ? font : state.bodyFont, Text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX); return 0;
            }
            return DefSubclassProc(window, message, wParam, lParam);
        }
        LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                state->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            if (!state) return DefWindowProcW(window, message, wParam, lParam);
            switch (message)
            {
            case WM_CREATE:
            {
                state->dpi = GetDpiForWindow(window); state->backgroundBrush = CreateSolidBrush(Background); state->fieldBrush = CreateSolidBrush(Field);
                const auto child = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
                    const HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                    if (control) SetWindowSubclass(control, ChildProc, 1, reinterpret_cast<DWORD_PTR>(state)); return control;
                };
                if (!child(L"STATIC", L"Connector server URL", ServerUrlLabel, SS_LEFT | SS_NOPREFIX)) return -1;
                if (!child(L"EDIT", state->settings.serverUrl.c_str(), ServerUrl, ES_AUTOHSCROLL | WS_TABSTOP)) return -1;
                if (!child(L"STATIC", L"Server root, for example http://127.0.0.1:8301", Example, SS_LEFT | SS_NOPREFIX)) return -1;
                if (!child(L"STATIC", L"Chat font size", ChatFontSizeLabel, SS_LEFT | SS_NOPREFIX)) return -1;
                if (!child(L"COMBOBOX", L"", ChatFontSize, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL)) return -1;
                if (std::isfinite(state->settings.chatFontSize) && state->settings.chatFontSize >= 10.0f && state->settings.chatFontSize <= 24.0f &&
                    std::find(state->fontSizes.begin(), state->fontSizes.end(), state->settings.chatFontSize) == state->fontSizes.end())
                    state->fontSizes.push_back(state->settings.chatFontSize);
                size_t selected = 0;
                for (size_t index = 0; index < state->fontSizes.size(); ++index)
                {
                    const auto label = FontLabel(state->fontSizes[index]);
                    SendMessageW(state->Control(ChatFontSize), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                    if (state->fontSizes[index] == state->settings.chatFontSize) selected = index;
                }
                SendMessageW(state->Control(ChatFontSize), CB_SETCURSEL, selected, 0);
                if (!child(L"BUTTON", L"Test Connection", TestConnection, BS_OWNERDRAW | WS_TABSTOP)) return -1;
                if (!child(L"STATIC", L"Test checks the server's currently enabled models.", Status, SS_LEFT | SS_NOPREFIX)) return -1;
                if (!child(L"STATIC", L"After saving, select an active model in the Studio model menu.", ModelHelp, SS_LEFT | SS_NOPREFIX)) return -1;
                for (const auto pair : {std::pair<int, const wchar_t*>{Cancel, L"Cancel"}, {Save, L"Save"}, {Close, L"Close"}})
                    if (!child(L"BUTTON", pair.second, pair.first, BS_OWNERDRAW | (pair.first == Close ? 0 : WS_TABSTOP))) return -1;
                SendMessageW(state->Control(ServerUrl), EM_SETLIMITTEXT, 32767, 0);
                MakeFonts(*state); Layout(*state); return 0;
            }
            case WM_NCCALCSIZE: return 0;
            case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(window, &point);
                RECT client{}; GetClientRect(window, &client); const int edge = state->Px(5);
                const bool left = point.x < edge, right = point.x >= client.right - edge, top = point.y < edge, bottom = point.y >= client.bottom - edge;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (point.y < state->Px(state->compact ? 40.0f : 48.0f) && point.x < client.right - state->Px(48)) return HTCAPTION;
                return HTCLIENT;
            }
            case WM_GETMINMAXINFO:
            {
                MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                const LONG width = (std::max)(1L, monitor.rcWork.right - monitor.rcWork.left - state->Px(24));
                const LONG height = (std::max)(1L, monitor.rcWork.bottom - monitor.rcWork.top - state->Px(24));
                auto* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
                bounds->ptMinTrackSize = POINT{(std::min)(static_cast<LONG>(state->Px(520)), width), (std::min)(static_cast<LONG>(state->Px(320)), height)}; return 0;
            }
            case WM_DPICHANGED:
                state->dpi = HIWORD(wParam) ? HIWORD(wParam) : 96; MakeFonts(*state);
                if (lParam)
                {
                    const auto rect = *reinterpret_cast<RECT*>(lParam); MONITORINFO monitor{sizeof(monitor)};
                    GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor);
                    const LONG width = (std::min)(rect.right - rect.left, (std::max)(1L, monitor.rcWork.right - monitor.rcWork.left - state->Px(24)));
                    const LONG height = (std::min)(rect.bottom - rect.top, (std::max)(1L, monitor.rcWork.bottom - monitor.rcWork.top - state->Px(24)));
                    const LONG x = (std::clamp)(rect.left, monitor.rcWork.left, (std::max)(monitor.rcWork.left, monitor.rcWork.right - width));
                    const LONG y = (std::clamp)(rect.top, monitor.rcWork.top, (std::max)(monitor.rcWork.top, monitor.rcWork.bottom - height));
                    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                Layout(*state); return 0;
            case WM_SIZE: Layout(*state); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            { PAINTSTRUCT ps{}; const HDC dc = BeginPaint(window, &ps); Paint(*state, dc); EndPaint(window, &ps); return 0; }
            case WM_PRINTCLIENT: Paint(*state, reinterpret_cast<HDC>(wParam)); return 0;
            case WM_DRAWITEM: DrawButton(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
            case WM_MEASUREITEM:
                if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType == ODT_COMBOBOX)
                { reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = state->Px(26); return TRUE; }
                break;
            case WM_CTLCOLOREDIT:
                SetBkColor(reinterpret_cast<HDC>(wParam), Field); SetTextColor(reinterpret_cast<HDC>(wParam), Text); return reinterpret_cast<LRESULT>(state->fieldBrush);
            case WM_CTLCOLORSTATIC:
                SetBkColor(reinterpret_cast<HDC>(wParam), Background);
                SetTextColor(reinterpret_cast<HDC>(wParam), GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == Status ? state->statusColor : Muted);
                return reinterpret_cast<LRESULT>(state->backgroundBrush);
            case WM_CTLCOLORLISTBOX:
                SetBkColor(reinterpret_cast<HDC>(wParam), Field); SetTextColor(reinterpret_cast<HDC>(wParam), Text);
                return reinterpret_cast<LRESULT>(state->fieldBrush);
            case DM_GETDEFID: return MAKELRESULT(Save, DC_HASDEFID);
            case WM_TIMER: if (wParam == PollTimer) PollTest(*state); return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) { if (!state->saving) DestroyWindow(window); return 0; }
                if (wParam == VK_RETURN) { SaveSettings(*state); return 0; }
                break;
            case WM_COMMAND:
            {
                const int id = LOWORD(wParam), code = HIWORD(wParam);
                if (id == Cancel || id == Close) { if (!state->saving) DestroyWindow(window); return 0; }
                if (id == ServerUrl && code == EN_CHANGE)
                { state->CancelTest(); ShowStatus(*state, L"Test checks the server's currently enabled models."); UpdateButtons(*state); return 0; }
                if (id == TestConnection && code == BN_CLICKED) { StartTest(*state); return 0; }
                if (id == ChatFontSize && (code == CBN_SELCHANGE || code == CBN_SELENDOK))
                { InvalidateRect(state->Control(ChatFontSize), nullptr, TRUE); return 0; }
                if (id == Save && code == BN_CLICKED) { SaveSettings(*state); return 0; }
                break;
            }
            case WM_CLOSE: if (!state->saving) DestroyWindow(window); return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                if (state->owned) delete state;
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
        HWND CreateWindowForDialog(HWND owner, const Connector::Settings& settings,
            const SaveConnectorSettings& save, const std::shared_ptr<Completion>& completion, bool visible)
        {
            WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = DialogProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = DialogClass;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
            auto state = std::make_unique<DialogState>(); state->settings = settings; state->save = save; state->completion = completion;
            state->dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
            RECT anchor = monitor.rcWork; if (owner) GetWindowRect(owner, &anchor);
            const LONG width = (std::min)(static_cast<LONG>(state->Px(640)), (std::max)(1L, monitor.rcWork.right - monitor.rcWork.left - state->Px(24)));
            const LONG height = (std::min)(static_cast<LONG>(state->Px(480)), (std::max)(1L, monitor.rcWork.bottom - monitor.rcWork.top - state->Px(24)));
            const LONG x = (std::clamp)(anchor.left + (anchor.right - anchor.left - width) / 2, monitor.rcWork.left, (std::max)(monitor.rcWork.left, monitor.rcWork.right - width));
            const LONG y = (std::clamp)(anchor.top + (anchor.bottom - anchor.top - height) / 2, monitor.rcWork.top, (std::max)(monitor.rcWork.top, monitor.rcWork.bottom - height));
            const HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, DialogClass, L"Settings - Connector Studio",
                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), state.get());
            if (!window) return nullptr;
            state->owned = true; state.release();
            if (visible) { ShowWindow(window, SW_SHOW); UpdateWindow(window); SetFocus(GetDlgItem(window, ServerUrl)); }
            return window;
        }
    }
    HWND CreateSettingsDialogWindow(HWND owner, const Connector::Settings& settings,
        const SaveConnectorSettings& save, bool visible)
    { return CreateWindowForDialog(owner, settings, save, std::make_shared<Completion>(), visible); }
    bool ShowSettingsDialog(HWND owner, const Connector::Settings& settings, const SaveConnectorSettings& save)
    {
        const auto completion = std::make_shared<Completion>(); const HWND previousFocus = GetFocus();
        const HWND window = CreateWindowForDialog(owner, settings, save, completion, false);
        if (!window) return false;
        const bool ownerWasEnabled = owner && IsWindowEnabled(owner); if (ownerWasEnabled) EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW); UpdateWindow(window); SetFocus(GetDlgItem(window, ServerUrl));
        MSG message{}; BOOL result = 1;
        while (IsWindow(window) && (result = GetMessageW(&message, nullptr, 0, 0)) > 0)
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (IsWindow(window)) DestroyWindow(window);
        if (ownerWasEnabled && IsWindow(owner))
        { EnableWindow(owner, TRUE); SetActiveWindow(owner); SetFocus(previousFocus && IsWindow(previousFocus) ? previousFocus : owner); }
        if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
        return completion->saved;
    }

    bool CaptureSettingsDialog(HWND window, const std::wstring& path, std::wstring& error)
    {
        error.clear(); RECT client{};
        if (!IsWindow(window) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0 ||
            client.right > 10000 || client.bottom > 10000) { error = L"The dialog window is unavailable."; return false; }
        const UINT width = static_cast<UINT>(client.right), height = static_cast<UINT>(client.bottom);
        BITMAPINFO bitmapInfo{}; bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width); bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmapInfo.bmiHeader.biPlanes = 1; bitmapInfo.bmiHeader.biBitCount = 32; bitmapInfo.bmiHeader.biCompression = BI_RGB;
        const HDC dc = GetDC(window); const HDC memory = CreateCompatibleDC(dc); void* pixels = nullptr;
        const HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!memory || !bitmap || !pixels)
        {
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (dc) ReleaseDC(window, dc);
            error = L"Windows could not capture the dialog."; return false;
        }
        const auto old = SelectObject(memory, bitmap);
        // Print the actual HWND and native EDIT, BUTTON and LISTBOX children, including
        // current text and selection. There is no separate mockup rendering path.
        SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        GdiFlush();
        ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmap> source; ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR, width * 4, width * height * 4, static_cast<BYTE*>(pixels), &source);
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
        if (FAILED(hr)) { error = L"The dialog preview could not be saved."; return false; }
        return true;
    }
    bool SaveSettingsDialogPreview(const Connector::Settings& settings, const std::wstring& path, std::wstring& error)
    {
        const HWND window = CreateSettingsDialogWindow(nullptr, settings, [](const Connector::Settings&, std::wstring&) { return false; }, false);
        if (!window) { error = L"The settings preview could not be created."; return false; }
        const bool saved = CaptureSettingsDialog(window, path, error); DestroyWindow(window); return saved;
    }
}
