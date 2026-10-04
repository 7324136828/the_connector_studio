#include "NewItemDialog.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <utility>

namespace Lattice
{
    namespace
    {
        using namespace NewItemDialogControls;
        using Microsoft::WRL::ComPtr;
        constexpr wchar_t DialogClass[] = L"ConnectorStudioNewItemDialog";
        constexpr COLORREF Background = RGB(21, 23, 28);
        constexpr COLORREF Panel = RGB(27, 29, 35);
        constexpr COLORREF Field = RGB(32, 35, 41);
        constexpr COLORREF Border = RGB(58, 62, 72);
        constexpr COLORREF Text = RGB(216, 222, 233);
        constexpr COLORREF Muted = RGB(137, 145, 159);
        constexpr COLORREF Accent = RGB(112, 123, 237);
        constexpr COLORREF Selection = RGB(48, 57, 92);
        constexpr COLORREF ErrorColor = RGB(244, 151, 151);
        struct Completion { bool succeeded = false; };
        struct DialogState
        {
            HWND window = nullptr;
            HWND composingEdit = nullptr;
            NewItemDialogOptions options;
            NewItemDialogCallbacks callbacks;
            std::shared_ptr<Completion> completion;
            ItemCategory category = ItemCategory::Projects;
            std::vector<size_t> typeIndices;
            std::wstring error, parentLocation, projectLocation;
            UINT dpi = 96;
            bool owned = false;
            bool busy = false;
            bool compact = false;
            HFONT bodyFont = nullptr, boldFont = nullptr, titleFont = nullptr, smallFont = nullptr;
            HBRUSH backgroundBrush = nullptr, fieldBrush = nullptr, panelBrush = nullptr;
            ~DialogState()
            {
                for (auto font : {bodyFont, boldFont, titleFont, smallFont}) if (font) DeleteObject(font);
                for (auto brush : {backgroundBrush, fieldBrush, panelBrush}) if (brush) DeleteObject(brush);
            }
            int Px(float dip) const { return static_cast<int>(std::lround(dip * dpi / 96.0f)); }
            HWND Control(int id) const { return GetDlgItem(window, id); }
        };
        RECT R(DialogState& state, float x, float y, float width, float height)
        { return RECT{state.Px(x), state.Px(y), state.Px(x + width), state.Px(y + height)}; }
        void Fill(HDC dc, const RECT& rect, COLORREF color)
        {
            const HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &rect, brush); DeleteObject(brush);
        }
        void Outline(HDC dc, RECT rect, COLORREF color, int thickness = 1)
        {
            const HBRUSH brush = CreateSolidBrush(color);
            for (int i = 0; i < thickness; ++i) { FrameRect(dc, &rect, brush); InflateRect(&rect, -1, -1); }
            DeleteObject(brush);
        }
        void Label(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
            UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX)
        {
            const auto previous = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
            SelectObject(dc, previous);
        }
        std::wstring Read(HWND window)
        {
            const int length = GetWindowTextLengthW(window);
            std::wstring result(static_cast<size_t>(length) + 1, L'\0');
            const int copied = GetWindowTextW(window, result.data(), length + 1);
            result.resize(static_cast<size_t>((std::max)(0, copied)));
            return result;
        }
        std::wstring Trim(std::wstring value)
        {
            const auto begin = std::find_if_not(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(c) != 0; });
            const auto end = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t c) { return std::iswspace(c) != 0; }).base();
            return begin < end ? std::wstring(begin, end) : std::wstring{};
        }
        const ItemType* SelectedType(DialogState& state)
        {
            const auto selected = SendMessageW(state.Control(Types), LB_GETCURSEL, 0, 0);
            return selected >= 0 && static_cast<size_t>(selected) < state.typeIndices.size() ?
                &state.options.types[state.typeIndices[static_cast<size_t>(selected)]] : nullptr;
        }
        void ShowError(DialogState& state, std::wstring message, HWND focus = nullptr)
        {
            state.error = std::move(message);
            InvalidateRect(state.window, nullptr, FALSE);
            if (focus) SetFocus(focus);
        }
        void UpdateButtons(DialogState& state)
        {
            EnableWindow(state.Control(Create), !state.busy && SelectedType(state) && static_cast<bool>(state.callbacks.create));
            EnableWindow(state.Control(OpenSelected), !state.busy && static_cast<bool>(state.callbacks.openProject) &&
                SendMessageW(state.Control(RecentProjects), LB_GETCURSEL, 0, 0) != LB_ERR);
            EnableWindow(state.Control(OpenExisting), !state.busy && static_cast<bool>(state.callbacks.openProject));
            EnableWindow(state.Control(Browse), !state.busy);
        }
        void MakeFonts(DialogState& state)
        {
            const auto make = [&](int size, int weight) {
                return CreateFontW(-state.Px(static_cast<float>(size)), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            };
            for (auto font : {state.bodyFont, state.boldFont, state.titleFont, state.smallFont}) if (font) DeleteObject(font);
            state.bodyFont = make(13, FW_NORMAL); state.boldFont = make(13, FW_SEMIBOLD);
            state.titleFont = make(21, FW_SEMIBOLD); state.smallFont = make(11, FW_NORMAL);
            for (HWND child = GetWindow(state.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.bodyFont), TRUE);
            SendMessageW(state.Control(Types), LB_SETITEMHEIGHT, 0, state.Px(54));
            SendMessageW(state.Control(RecentProjects), LB_SETITEMHEIGHT, 0, state.Px(42));
        }
        void Layout(DialogState& state)
        {
            RECT client{}; GetClientRect(state.window, &client);
            const float width = client.right * 96.0f / state.dpi;
            const float height = client.bottom * 96.0f / state.dpi;
            state.compact = height < 620.0f;
            const float typeWidth = (std::min)(248.0f, (std::max)(160.0f, (width - 48.0f) * 0.36f));
            const float right = 48.0f + typeWidth;
            const float typeTop = state.compact ? 118.0f : 138.0f;
            const auto move = [&](int id, float x, float y, float w, float h) {
                const auto rect = R(state, x, y, w, h);
                MoveWindow(state.Control(id), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
            };
            move(Close, width - 44, 6, 36, 32);
            const float tabTop = state.compact ? 48.0f : 62.0f;
            move(FilesTab, 24, tabTop, 84, 34); move(SessionsTab, 108, tabTop, 106, 34); move(ProjectsTab, 214, tabTop, 102, 34);
            move(Types, 24, typeTop, typeWidth, state.compact ? 126.0f : 184.0f);
            move(Name, right + 10, typeTop + 7, width - right - 48, 20);
            const float locationTop = state.compact ? 186.0f : 222.0f;
            move(Location, right + 10, locationTop + 7, width - right - 142, 20);
            move(Browse, width - 116, locationTop, 92, 34);
            move(RecentProjects, 24, state.compact ? 286.0f : 402.0f, width - 48,
                (std::max)(state.compact ? 32.0f : 42.0f, height - (state.compact ? 428.0f : 578.0f)));
            move(OpenSelected, 24, height - (state.compact ? 130.0f : 164.0f), 124, state.compact ? 28.0f : 32.0f);
            move(OpenExisting, 160, height - (state.compact ? 130.0f : 164.0f), 164, state.compact ? 28.0f : 32.0f);
            move(Cancel, width - 224, height - (state.compact ? 50.0f : 60.0f), 92, state.compact ? 32.0f : 36.0f);
            move(Create, width - 120, height - (state.compact ? 50.0f : 60.0f), 96, state.compact ? 32.0f : 36.0f);
            SendMessageW(state.Control(Types), LB_SETITEMHEIGHT, 0, state.Px(state.compact ? 44.0f : 54.0f));
            SendMessageW(state.Control(RecentProjects), LB_SETITEMHEIGHT, 0, state.Px(state.compact ? 32.0f : 42.0f));
            const bool projects = state.category == ItemCategory::Projects;
            for (const int id : {RecentProjects, OpenSelected, OpenExisting}) ShowWindow(state.Control(id), projects ? SW_SHOWNA : SW_HIDE);
            UpdateButtons(state);
            InvalidateRect(state.window, nullptr, FALSE);
        }
        void SelectCategory(DialogState& state, ItemCategory category)
        {
            if (state.category == ItemCategory::Projects) state.parentLocation = Read(state.Control(Location));
            else state.projectLocation = Read(state.Control(Location));
            state.category = category; state.error.clear(); state.typeIndices.clear();
            SetWindowTextW(state.Control(Location), (category == ItemCategory::Projects ? state.parentLocation : state.projectLocation).c_str());
            SendMessageW(state.Control(Types), LB_RESETCONTENT, 0, 0);
            for (size_t i = 0; i < state.options.types.size(); ++i)
            {
                const auto& type = state.options.types[i];
                if (type.category != category) continue;
                state.typeIndices.push_back(i);
                SendMessageW(state.Control(Types), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(type.name.c_str()));
            }
            if (!state.typeIndices.empty()) SendMessageW(state.Control(Types), LB_SETCURSEL, 0, 0);
            for (const int id : {FilesTab, SessionsTab, ProjectsTab}) InvalidateRect(state.Control(id), nullptr, FALSE);
            Layout(state);
        }
        bool PickFolder(HWND owner, const std::wstring& initial, std::wstring& result, std::wstring& error)
        {
            ComPtr<IFileOpenDialog> dialog;
            HRESULT hr = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
            if (FAILED(hr)) { error = L"Windows could not open the folder picker."; return false; }
            FILEOPENDIALOGOPTIONS flags{};
            hr = dialog->GetOptions(&flags);
            if (SUCCEEDED(hr)) hr = dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
            if (FAILED(hr)) { error = L"Windows could not configure the folder picker."; return false; }
            dialog->SetTitle(L"Select a folder");
            if (!initial.empty())
            {
                ComPtr<IShellItem> folder;
                if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
            }
            hr = dialog->Show(owner);
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
            if (FAILED(hr)) { error = L"Windows could not display the folder picker."; return false; }
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            hr = dialog->GetResult(&item);
            if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (FAILED(hr) || !path) { if (path) CoTaskMemFree(path); error = L"The selection must be a local folder."; return false; }
            result = path; CoTaskMemFree(path); return true;
        }
        void CreateItem(DialogState& state)
        {
            if (state.busy) return;
            const auto* type = SelectedType(state);
            if (!type) { ShowError(state, L"Select an item type.", state.Control(Types)); return; }
            NewItemRequest request{};
            request.category = state.category; request.typeId = type->id;
            request.name = Trim(Read(state.Control(Name))); request.location = Trim(Read(state.Control(Location)));
            if (request.name.empty()) { ShowError(state, L"Enter a name.", state.Control(Name)); return; }
            if (request.location.empty()) { ShowError(state, state.category == ItemCategory::Projects ?
                L"Choose a parent folder for the project." : L"Choose a work project folder.", state.Control(Location)); return; }
            if (!state.callbacks.create) { ShowError(state, L"Creation is unavailable."); return; }
            state.busy = true; UpdateButtons(state);
            std::wstring error; bool succeeded = false;
            try { succeeded = state.callbacks.create(request, error); }
            catch (...) { error = L"The item could not be created. Try again."; }
            state.busy = false;
            if (succeeded) { state.completion->succeeded = true; DestroyWindow(state.window); return; }
            UpdateButtons(state); ShowError(state, error.empty() ? L"The item could not be created. Try again." : std::move(error));
        }
        void OpenProject(DialogState& state, const std::wstring& path)
        {
            if (state.busy || !state.callbacks.openProject) return;
            state.busy = true; UpdateButtons(state);
            std::wstring error; bool succeeded = false;
            try { succeeded = state.callbacks.openProject(path, error); }
            catch (...) { error = L"The project could not be opened. Try again."; }
            state.busy = false;
            if (succeeded) { state.completion->succeeded = true; DestroyWindow(state.window); return; }
            UpdateButtons(state); ShowError(state, error.empty() ? L"The project could not be opened. Try again." : std::move(error));
        }
        void OpenRecent(DialogState& state)
        {
            const auto index = SendMessageW(state.Control(RecentProjects), LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < state.options.recentProjects.size())
                OpenProject(state, state.options.recentProjects[static_cast<size_t>(index)]);
        }
        void Paint(DialogState& state, HDC dc)
        {
            RECT client{}; GetClientRect(state.window, &client);
            const float width = client.right * 96.0f / state.dpi;
            const float height = client.bottom * 96.0f / state.dpi;
            const float typeWidth = (std::min)(248.0f, (std::max)(160.0f, (width - 48.0f) * 0.36f));
            const float right = 48.0f + typeWidth;
            const float typeTop = state.compact ? 118.0f : 138.0f;
            const float locationTop = state.compact ? 186.0f : 222.0f;
            Fill(dc, client, Background);
            Fill(dc, R(state, 0, 0, width, state.compact ? 40.0f : 48.0f), RGB(25, 27, 32));
            Label(dc, L"New", R(state, 24, 8, width - 100, 32), state.titleFont, Text);
            Fill(dc, R(state, 24, state.compact ? 82.0f : 96.0f, width - 48, 1), Border);
            Label(dc, L"Item type", R(state, 24, state.compact ? 92.0f : 109.0f, typeWidth, 22), state.boldFont, Muted);
            Outline(dc, R(state, 23, typeTop - 1, typeWidth + 2, state.compact ? 128.0f : 186.0f), Border);
            Label(dc, L"Name", R(state, right, state.compact ? 92.0f : 109.0f, width - right - 24, 22), state.boldFont, Muted);
            Label(dc, state.category == ItemCategory::Projects ? L"Parent folder" : L"Work project folder",
                R(state, right, locationTop - 29, width - right - 24, 22), state.boldFont, Muted);
            for (const auto field : {std::pair<int, RECT>{Name, R(state, right, typeTop, width - right - 24, 34)},
                std::pair<int, RECT>{Location, R(state, right, locationTop, width - right - 128, 34)}})
            {
                Fill(dc, field.second, Field);
                Outline(dc, field.second, GetFocus() == state.Control(field.first) ? Accent : Border, state.Px(1));
            }
            if (const auto* type = SelectedType(state))
            {
                Label(dc, type->description, R(state, right, state.compact ? 234.0f : 277.0f, width - right - 24, state.compact ? 22.0f : 45.0f), state.bodyFont, Muted,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            const wchar_t* help = state.category == ItemCategory::Projects ?
                L"Creates a project folder for your files and saved work sessions." :
                state.category == ItemCategory::Sessions ? L"Saved in the selected work project's sessions folder." :
                L"Saved in the selected work project's files folder.";
            if (!state.compact)
                Label(dc, help, R(state, 24, 340, width - 48, 32), state.smallFont, Muted,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            if (state.category == ItemCategory::Projects)
            {
                Label(dc, L"Recent projects", R(state, 24, state.compact ? 260.0f : 373.0f, width - 48, 23), state.boldFont, Muted);
                Outline(dc, R(state, 23, state.compact ? 285.0f : 401.0f, width - 46,
                    (std::max)(state.compact ? 32.0f : 42.0f, height - (state.compact ? 428.0f : 578.0f)) + 2), Border);
            }
            Label(dc, state.error, R(state, 24, height - (state.compact ? 91.0f : 119.0f), width - 48, state.compact ? 24.0f : 43.0f), state.bodyFont, ErrorColor,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
            Fill(dc, R(state, 24, height - (state.compact ? 62.0f : 72.0f), width - 48, 1), Border);
            Outline(dc, client, Border);
        }
        void DrawControl(DialogState& state, const DRAWITEMSTRUCT& draw)
        {
            RECT rect = draw.rcItem;
            const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
            if (draw.CtlType == ODT_BUTTON)
            {
                const int id = static_cast<int>(draw.CtlID);
                const bool tab = id >= FilesTab && id <= ProjectsTab;
                const bool active = tab && (id - FilesTab) == static_cast<int>(state.category);
                const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
                const bool focused = (draw.itemState & ODS_FOCUS) != 0;
                COLORREF color = id == Create ? (disabled ? Border : Accent) : (tab ? Background : Field);
                if (pressed) color = Selection;
                Fill(draw.hDC, rect, color);
                if (tab && active) { RECT line = rect; line.top = line.bottom - state.Px(2); Fill(draw.hDC, line, Accent); }
                else if (!tab && id != Close) Outline(draw.hDC, rect, Border);
                Label(draw.hDC, id == Close ? L"\x00d7" : Read(draw.hwndItem), rect,
                    active || id == Create ? state.boldFont : state.bodyFont,
                    disabled ? Muted : active || id == Create ? RGB(255, 255, 255) : Text,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (focused) { InflateRect(&rect, -state.Px(4), -state.Px(4)); Outline(draw.hDC, rect, RGB(151, 162, 231)); }
                return;
            }
            if (draw.CtlType != ODT_LISTBOX) return;
            Fill(draw.hDC, rect, Panel);
            if (draw.itemID == static_cast<UINT>(-1)) return;
            const bool selected = (draw.itemState & ODS_SELECTED) != 0;
            if (selected) Fill(draw.hDC, rect, Selection);
            RECT title = rect, detail = rect;
            title.left += state.Px(12); title.right -= state.Px(10); title.top += state.Px(6); title.bottom = title.top + state.Px(21);
            detail.left += state.Px(12); detail.right -= state.Px(10); detail.top += state.Px(28); detail.bottom -= state.Px(4);
            if (draw.CtlID == Types && draw.itemID < state.typeIndices.size())
            {
                const auto& type = state.options.types[state.typeIndices[draw.itemID]];
                Label(draw.hDC, type.name, title, state.boldFont, Text);
                Label(draw.hDC, type.extension.empty() ? L"Project folder" : type.extension + L" file", detail, state.smallFont, Muted);
            }
            else if (draw.CtlID == RecentProjects && draw.itemID < state.options.recentProjects.size())
            {
                const auto& path = state.options.recentProjects[draw.itemID];
                if (state.compact)
                {
                    RECT line = rect; line.left += state.Px(12); line.right -= state.Px(10);
                    Label(draw.hDC, path, line, state.bodyFont, Text);
                    if (draw.itemState & ODS_FOCUS) { InflateRect(&rect, -2, -2); Outline(draw.hDC, rect, RGB(151, 162, 231)); }
                    return;
                }
                std::wstring name;
                try { name = std::filesystem::path(path).filename().wstring(); } catch (...) {}
                Label(draw.hDC, name.empty() ? path : name, title, state.boldFont, Text);
                detail.top = rect.top + state.Px(25);
                Label(draw.hDC, path, detail, state.smallFont, Muted);
            }
            if (draw.itemState & ODS_FOCUS) { InflateRect(&rect, -2, -2); Outline(draw.hDC, rect, RGB(151, 162, 231)); }
        }
        LRESULT CALLBACK ChildProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
            UINT_PTR, DWORD_PTR data)
        {
            auto& state = *reinterpret_cast<DialogState*>(data);
            if (message == WM_IME_STARTCOMPOSITION) state.composingEdit = window;
            if ((message == WM_IME_ENDCOMPOSITION || message == WM_NCDESTROY) && state.composingEdit == window)
                state.composingEdit = nullptr;
            if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(state.window, nullptr, FALSE);
            if (message == WM_GETDLGCODE)
            {
                const auto code = DefSubclassProc(window, message, wParam, lParam);
                const auto* key = reinterpret_cast<const MSG*>(lParam);
                const int id = GetDlgCtrlID(window);
                if (key && (key->wParam == VK_ESCAPE || key->wParam == VK_RETURN ||
                    (key->wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) ||
                    (id >= FilesTab && id <= ProjectsTab && (key->wParam == VK_LEFT || key->wParam == VK_RIGHT))))
                    return code | DLGC_WANTMESSAGE;
                return code;
            }
            if (message == WM_KEYDOWN)
            {
                // Enter selects an IME candidate and Escape dismisses composition;
                // neither should create an item or close the dialog at that point.
                if (state.composingEdit == window && (wParam == VK_RETURN || wParam == VK_ESCAPE))
                    return DefSubclassProc(window, message, wParam, lParam);
                if (wParam == VK_ESCAPE) { if (!state.busy) DestroyWindow(state.window); return 0; }
                if (wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 1;
                    SelectCategory(state, static_cast<ItemCategory>((static_cast<int>(state.category) + step) % 3)); return 0;
                }
                const int id = GetDlgCtrlID(window);
                if ((id == FilesTab || id == SessionsTab || id == ProjectsTab) && (wParam == VK_LEFT || wParam == VK_RIGHT))
                {
                    const int next = (id - FilesTab + (wParam == VK_LEFT ? 2 : 1)) % 3;
                    SelectCategory(state, static_cast<ItemCategory>(next)); SetFocus(state.Control(FilesTab + next)); return 0;
                }
                if (wParam == VK_RETURN)
                {
                    if (id == RecentProjects) OpenRecent(state);
                    else if (id == Name || id == Location || id == Types) CreateItem(state);
                    else SendMessageW(window, BM_CLICK, 0, 0);
                    return 0;
                }
            }
            if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE) && state.composingEdit != window) return 0;
            if ((message == WM_PRINTCLIENT || message == WM_PRINT) && (GetDlgCtrlID(window) == Name || GetDlgCtrlID(window) == Location))
            {
                // Windows' native EDIT skips its text when the parent is hidden. Print
                // the real control's current text, font, format rectangle and scroll
                // position; normal WM_PAINT and all editing remain native.
                const HDC dc = reinterpret_cast<HDC>(wParam);
                RECT client{}, format{}; GetClientRect(window, &client);
                Fill(dc, client, Field);
                SendMessageW(window, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
                auto text = Read(window);
                const auto first = SendMessageW(window, EM_GETFIRSTVISIBLELINE, 0, 0);
                if (first > 0 && static_cast<size_t>(first) < text.size()) text.erase(0, static_cast<size_t>(first));
                const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
                Label(dc, text, format, font ? font : state.bodyFont, Text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                return 0;
            }
            if ((message == WM_PAINT || message == WM_PRINTCLIENT) &&
                (GetDlgCtrlID(window) == RecentProjects || GetDlgCtrlID(window) == Types))
            {
                const auto result = DefSubclassProc(window, message, wParam, lParam);
                if (SendMessageW(window, LB_GETCOUNT, 0, 0) == 0)
                {
                    const HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wParam) : GetDC(window);
                    RECT rect{}; GetClientRect(window, &rect);
                    rect.left += state.Px(12); rect.top += state.Px(8); rect.bottom = rect.top + state.Px(24);
                    Label(dc, GetDlgCtrlID(window) == RecentProjects ? L"No recent projects" : L"No types available", rect, state.bodyFont, Muted);
                    if (message == WM_PAINT) ReleaseDC(window, dc);
                }
                return result;
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
                state->dpi = GetDpiForWindow(window);
                state->backgroundBrush = CreateSolidBrush(Background); state->fieldBrush = CreateSolidBrush(Field); state->panelBrush = CreateSolidBrush(Panel);
                const auto child = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
                    const HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                    if (control) SetWindowSubclass(control, ChildProc, 1, reinterpret_cast<DWORD_PTR>(state));
                    return control;
                };
                for (const auto pair : {std::pair<int, const wchar_t*>{FilesTab, L"Files"}, {SessionsTab, L"Sessions"}, {ProjectsTab, L"Projects"}})
                    if (!child(L"BUTTON", pair.second, pair.first, WS_TABSTOP | BS_OWNERDRAW)) return -1;
                if (!child(L"LISTBOX", L"", Types, WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT)) return -1;
                if (!child(L"EDIT", L"", Name, WS_TABSTOP | ES_AUTOHSCROLL)) return -1;
                if (!child(L"EDIT", (state->category == ItemCategory::Projects ? state->parentLocation : state->projectLocation).c_str(), Location, WS_TABSTOP | ES_AUTOHSCROLL)) return -1;
                if (!child(L"BUTTON", L"Browse...", Browse, WS_TABSTOP | BS_OWNERDRAW)) return -1;
                if (!child(L"LISTBOX", L"", RecentProjects, WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT)) return -1;
                for (const auto pair : {std::pair<int, const wchar_t*>{OpenSelected, L"Open Selected"}, {OpenExisting, L"Open Existing..."}, {Cancel, L"Cancel"}, {Create, L"Create"}, {Close, L"Close"}})
                    if (!child(L"BUTTON", pair.second, pair.first, (pair.first == Close ? 0 : WS_TABSTOP) | BS_OWNERDRAW)) return -1;
                SendMessageW(state->Control(Name), EM_SETLIMITTEXT, 255, 0);
                SendMessageW(state->Control(Location), EM_SETLIMITTEXT, 32767, 0);
                for (const auto& recent : state->options.recentProjects)
                    SendMessageW(state->Control(RecentProjects), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(recent.c_str()));
                MakeFonts(*state); SelectCategory(*state, state->options.initialCategory); return 0;
            }
            case WM_NCCALCSIZE: return 0;
            case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(window, &point);
                RECT rect{}; GetClientRect(window, &rect); const int edge = state->Px(5);
                const bool left = point.x < edge, right = point.x >= rect.right - edge;
                const bool top = point.y < edge, bottom = point.y >= rect.bottom - edge;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (point.y < state->Px(state->compact ? 40.0f : 48.0f) && point.x < rect.right - state->Px(48)) return HTCAPTION;
                return HTCLIENT;
            }
            case WM_GETMINMAXINFO:
            {
                auto* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                const LONG availableWidth = (std::max)(1L, monitor.rcWork.right - monitor.rcWork.left - state->Px(24));
                const LONG availableHeight = (std::max)(1L, monitor.rcWork.bottom - monitor.rcWork.top - state->Px(24));
                bounds->ptMinTrackSize = POINT{(std::min)(static_cast<LONG>(state->Px(720)), availableWidth),
                    (std::min)(static_cast<LONG>(state->Px(460)), availableHeight)}; return 0;
            }
            case WM_DPICHANGED:
                state->dpi = HIWORD(wParam) ? HIWORD(wParam) : 96; MakeFonts(*state);
                if (lParam)
                {
                    const auto rect = *reinterpret_cast<RECT*>(lParam);
                    MONITORINFO monitor{sizeof(monitor)};
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
            {
                PAINTSTRUCT ps{}; const HDC dc = BeginPaint(window, &ps); Paint(*state, dc); EndPaint(window, &ps); return 0;
            }
            case WM_PRINTCLIENT: Paint(*state, reinterpret_cast<HDC>(wParam)); return 0;
            case WM_DRAWITEM: DrawControl(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
            case WM_MEASUREITEM:
            {
                auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
                item->itemHeight = state->Px(item->CtlID == RecentProjects ? 42.0f : 54.0f); return TRUE;
            }
            case WM_CTLCOLORLISTBOX:
                SetBkColor(reinterpret_cast<HDC>(wParam), Panel); SetTextColor(reinterpret_cast<HDC>(wParam), Text);
                return reinterpret_cast<LRESULT>(state->panelBrush);
            case WM_CTLCOLOREDIT:
                SetBkColor(reinterpret_cast<HDC>(wParam), Field); SetTextColor(reinterpret_cast<HDC>(wParam), Text);
                return reinterpret_cast<LRESULT>(state->fieldBrush);
            case WM_CTLCOLORSTATIC:
                SetBkColor(reinterpret_cast<HDC>(wParam), Background); SetTextColor(reinterpret_cast<HDC>(wParam), Text);
                return reinterpret_cast<LRESULT>(state->backgroundBrush);
            case DM_GETDEFID: return MAKELRESULT(Create, DC_HASDEFID);
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) { if (!state->busy) DestroyWindow(window); return 0; }
                if (wParam == VK_RETURN) { CreateItem(*state); return 0; }
                break;
            case WM_COMMAND:
            {
                const int id = LOWORD(wParam), code = HIWORD(wParam);
                if (id == Cancel || id == Close) { if (!state->busy) DestroyWindow(window); return 0; }
                if (id >= FilesTab && id <= ProjectsTab && code == BN_CLICKED) { SelectCategory(*state, static_cast<ItemCategory>(id - FilesTab)); return 0; }
                if ((id == Name || id == Location) && code == EN_CHANGE) { state->error.clear(); InvalidateRect(window, nullptr, FALSE); return 0; }
                if (id == Create && code == BN_CLICKED) { CreateItem(*state); return 0; }
                if (id == Types && code == LBN_SELCHANGE) { state->error.clear(); UpdateButtons(*state); InvalidateRect(window, nullptr, FALSE); return 0; }
                if (id == RecentProjects && (code == LBN_SELCHANGE || code == LBN_DBLCLK))
                { UpdateButtons(*state); if (code == LBN_DBLCLK) OpenRecent(*state); return 0; }
                if (id == OpenSelected && code == BN_CLICKED) { OpenRecent(*state); return 0; }
                if ((id == Browse || id == OpenExisting) && code == BN_CLICKED)
                {
                    std::wstring selected, error;
                    if (PickFolder(window, Read(state->Control(Location)), selected, error))
                    {
                        if (id == Browse) { SetWindowTextW(state->Control(Location), selected.c_str()); ShowError(*state, L""); }
                        else OpenProject(*state, selected);
                    }
                    else if (!error.empty()) ShowError(*state, std::move(error));
                    return 0;
                }
                break;
            }
            case WM_CLOSE: if (!state->busy) DestroyWindow(window); return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                if (state->owned) delete state;
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
        HWND CreateWindowForDialog(HWND owner, const NewItemDialogOptions& options,
            const NewItemDialogCallbacks& callbacks, const std::shared_ptr<Completion>& completion, bool visible)
        {
            WNDCLASSEXW cls{sizeof(cls)};
            cls.lpfnWndProc = DialogProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = DialogClass;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
            auto state = std::make_unique<DialogState>(); state->options = options; state->callbacks = callbacks; state->completion = completion;
            if (state->options.types.empty()) { ItemTypeRegistry registry; state->options.types = registry.Types(); }
            state->category = options.initialCategory;
            state->projectLocation = options.projectLocation;
            state->parentLocation = options.location;
            state->dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
            const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
            RECT anchor = info.rcWork;
            if (owner) GetWindowRect(owner, &anchor);
            const LONG width = (std::min)(static_cast<LONG>(state->Px(800)), (std::max)(1L, info.rcWork.right - info.rcWork.left - state->Px(24)));
            const LONG height = (std::min)(static_cast<LONG>(state->Px(640)), (std::max)(1L, info.rcWork.bottom - info.rcWork.top - state->Px(24)));
            const int x = (std::clamp)(anchor.left + (anchor.right - anchor.left - width) / 2,
                info.rcWork.left, (std::max)(info.rcWork.left, info.rcWork.right - width));
            const int y = (std::clamp)(anchor.top + (anchor.bottom - anchor.top - height) / 2,
                info.rcWork.top, (std::max)(info.rcWork.top, info.rcWork.bottom - height));
            const DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_CLIPCHILDREN;
            const HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, DialogClass, L"New - Connector Studio",
                style, x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), state.get());
            if (!window) return nullptr;
            state->owned = true; state.release();
            if (visible) { ShowWindow(window, SW_SHOW); UpdateWindow(window); SetFocus(GetDlgItem(window, Name)); }
            return window;
        }
    }
    HWND CreateNewItemDialogWindow(HWND owner, const NewItemDialogOptions& options,
        const NewItemDialogCallbacks& callbacks, bool visible)
    {
        return CreateWindowForDialog(owner, options, callbacks, std::make_shared<Completion>(), visible);
    }
    bool ShowNewItemDialog(HWND owner, const NewItemDialogOptions& options, const NewItemDialogCallbacks& callbacks)
    {
        const auto completion = std::make_shared<Completion>();
        const HWND previousFocus = GetFocus();
        const HWND window = CreateWindowForDialog(owner, options, callbacks, completion, false);
        if (!window) return false;
        const bool ownerWasEnabled = owner && IsWindowEnabled(owner);
        if (ownerWasEnabled) EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW); UpdateWindow(window); SetFocus(GetDlgItem(window, Name));
        MSG message{}; BOOL result = 1;
        while (IsWindow(window) && (result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (IsWindow(window)) DestroyWindow(window);
        if (ownerWasEnabled && IsWindow(owner))
        {
            EnableWindow(owner, TRUE); SetActiveWindow(owner);
            SetFocus(previousFocus && IsWindow(previousFocus) ? previousFocus : owner);
        }
        if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
        return completion->succeeded;
    }
    bool CaptureNewItemDialog(HWND window, const std::wstring& path, std::wstring& error)
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
    bool SaveNewItemDialogPreview(const NewItemDialogOptions& options, const std::wstring& path, std::wstring& error)
    {
        NewItemDialogCallbacks callbacks;
        callbacks.create = [](const NewItemRequest&, std::wstring&) { return false; };
        callbacks.openProject = [](const std::wstring&, std::wstring&) { return false; };
        const HWND window = CreateNewItemDialogWindow(nullptr, options, callbacks, false);
        if (!window) { error = L"The dialog preview could not be created."; return false; }
        const bool saved = CaptureNewItemDialog(window, path, error);
        DestroyWindow(window); return saved;
    }
}
