#define LATTICE_TESTING
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "MockConnectorServer.h"
#include "../src/main.cpp"
#include "MarkdownRenderer.h"
#include "SessionPersistence.h"
#include <cassert>
#include <filesystem>
#include <iostream>

namespace
{
    struct InteractionFixture
    {
        std::filesystem::path base;
        std::filesystem::path root;
        bool owned = false;
        InteractionFixture()
        {
            wchar_t configured[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", configured, 32768);
            base = length && length < 32768 ? std::filesystem::path(configured) : std::filesystem::current_path();
            std::filesystem::create_directories(base);
            base = std::filesystem::weakly_canonical(base);
            root = base / (L"interaction-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            owned = std::filesystem::create_directory(root);
            assert(owned);
        }
        ~InteractionFixture()
        {
            if (owned && root.is_absolute() && root.parent_path() == base &&
                root.filename().wstring().find(L"interaction-fixture-") == 0)
            {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }
        }
    };
    template<class Predicate>
    void PumpUntil(HWND window, Predicate ready)
    {
        const ULONGLONG deadline = GetTickCount64() + 6000;
        while (!ready() && GetTickCount64() < deadline)
        {
            SendMessageW(window, WM_TIMER, AnimTimerId, 0);
            Sleep(5);
        }
        assert(ready());
    }
    void SetComposerText(const std::wstring& text)
    {
        SetWindowTextW(g_editControl, text.c_str());
        // Multiline EDIT's WM_SETTEXT omits EN_CHANGE, unlike user typing.
        SendMessageW(g_mainWindow, WM_COMMAND, MAKEWPARAM(201, EN_CHANGE),
            reinterpret_cast<LPARAM>(g_editControl));
    }
    void ClickAt(HWND window, float x, float y)
    {
        const LPARAM point = MAKELPARAM(static_cast<int>(std::lround(x * Scale())), static_cast<int>(std::lround(y * Scale())));
        SendMessageW(window, WM_MOUSEMOVE, 0, point);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(window, WM_LBUTTONUP, 0, point);
    }
    void ClickRect(HWND window, D2D1_RECT_F rect)
    {
        assert(rect.right > rect.left && rect.bottom > rect.top);
        ClickAt(window, (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
    }
    int MenuRow(const wchar_t* label)
    {
        const auto& items = g_controller->GetCurrentDropdownItems();
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].label == label) return static_cast<int>(i);
        assert(false); return -1;
    }
    void TestModelPickers(HWND window, ConnectorTest::MockConnectorServer& server)
    {
        using Lattice::ActiveDropdown;
        const auto draft = g_controller->GetDraftText();
        RefreshControls();
        ClickRect(window, g_layoutMetrics.composerModelBtn);
        assert(g_controller->GetActiveDropdown() == ActiveDropdown::Model);
        assert(g_layoutMetrics.dropdownRect.bottom > g_layoutMetrics.dropdownRect.top);
        assert(!(GetWindowLongPtrW(g_editControl, GWL_STYLE) & WS_VISIBLE));
        assert(g_controller->GetCurrentDropdownItems()[0].selected);
        ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"second-model")]);
        assert(g_controller->GetSelectedModel() == L"second-model");
        assert(g_controller->GetActiveDropdown() == ActiveDropdown::None);

        ClickRect(window, g_layoutMetrics.actBtnSession);
        ClickRect(window, g_layoutMetrics.sidebarModelSelect);
        assert(g_controller->GetActiveDropdown() == ActiveDropdown::SidebarModel);
        ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"studio-test")]);
        assert(g_controller->GetSelectedModel() == L"studio-test");

        server.SetHandler([](const ConnectorTest::Request& request) {
            auto response = ConnectorTest::MockConnectorServer::Default(request);
            if (request.path == "/v1/models") response.delayMs = 300;
            return response;
        });
        ClickRect(window, g_layoutMetrics.composerModelBtn);
        ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"Refresh models")]);
        assert(g_controller->IsDiscoveringModels() && g_controller->GetActiveDropdown() == ActiveDropdown::Model);
        const int loading = MenuRow(L"Refreshing Connector models...");
        assert(!g_controller->GetCurrentDropdownItems()[loading].enabled);
        ClickRect(window, g_layoutMetrics.dropdownItems[loading]);
        assert(g_controller->GetActiveDropdown() == ActiveDropdown::Model && g_controller->GetSelectedModel() == L"studio-test");
        PumpUntil(window, [] { return g_controller->IsConnectorConnected() && !g_controller->IsDiscoveringModels(); });
        RefreshControls();
        assert(g_controller->GetCurrentDropdownItems()[0].enabled);

        server.SetHandler([](const ConnectorTest::Request& request) {
            if (request.path != "/v1/models") return ConnectorTest::MockConnectorServer::Default(request);
            std::string json = "{\"data\":[";
            for (int i = 0; i < 40; ++i)
            {
                if (i) json += ',';
                json += "{\"id\":\"model-" + std::to_string(i) + "\",\"name\":\"Retrieved model " + std::to_string(i) + "\"}";
            }
            return ConnectorTest::Response{200, json + "]}", 300};
        });
        g_controller->RefreshConnectorModels();
        PumpUntil(window, [] { return g_controller->IsConnectorConnected() && !g_controller->IsDiscoveringModels(); });
        RefreshControls();
        assert(MenuRow(L"model-39") == 39 && g_layoutMetrics.dropdownMaxScroll > 0);
        assert(g_layoutMetrics.dropdownRect.bottom <= g_layoutMetrics.windowHeight);
        POINT wheelPoint{static_cast<LONG>(std::lround((g_layoutMetrics.dropdownRect.left + 20) * Scale())),
            static_cast<LONG>(std::lround((g_layoutMetrics.dropdownRect.top + 30) * Scale()))};
        assert(ClientToScreen(window, &wheelPoint));
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), MAKELPARAM(wheelPoint.x, wheelPoint.y));
        assert(g_layoutMetrics.dropdownScrollOffset > 0);
        assert(HandleNavigationKey(VK_HOME) && g_dropdownIndex == 0 && g_layoutMetrics.dropdownScrollOffset == 0);
        assert(HandleNavigationKey(VK_END) && g_layoutMetrics.dropdownScrollOffset > 0);
        assert(HandleNavigationKey(VK_UP) && HandleNavigationKey(VK_UP) && g_dropdownIndex == 39);
        assert(Contains(g_layoutMetrics.dropdownViewport, g_layoutMetrics.dropdownItems[39].left + 5,
            (g_layoutMetrics.dropdownItems[39].top + g_layoutMetrics.dropdownItems[39].bottom) / 2));
        assert(HandleNavigationKey(VK_RETURN) && g_controller->GetSelectedModel() == L"model-39");

        ClickRect(window, g_layoutMetrics.composerModelBtn);
        assert(g_layoutMetrics.dropdownScrollOffset == 0);
        const auto thumb = g_layoutMetrics.dropdownScrollThumb;
        const float x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(std::lround(x * Scale())), static_cast<int>(std::lround(y * Scale()))));
        assert(g_dragDropdown && GetCapture() == window);
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(std::lround(x * Scale())),
            static_cast<int>(std::lround((g_layoutMetrics.dropdownScrollTrack.bottom - 1) * Scale()))));
        assert(g_layoutMetrics.dropdownScrollOffset > 0);
        SendMessageW(window, WM_LBUTTONUP, 0, 0);
        assert(!g_dragDropdown && GetCapture() != window);
        ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"Refresh models")]);
        assert(g_controller->IsDiscoveringModels() && g_dropdownIndex == -1);
        assert(HandleNavigationKey(VK_RETURN) && g_controller->GetSelectedModel() == L"model-39"
            && g_controller->GetActiveDropdown() == ActiveDropdown::Model);
        PumpUntil(window, [] { return g_controller->IsConnectorConnected() && !g_controller->IsDiscoveringModels(); });
        RefreshControls();
        assert(g_dropdownIndex == -1 && HandleNavigationKey(VK_RETURN) && g_controller->GetSelectedModel() == L"model-39"
            && g_controller->GetActiveDropdown() == ActiveDropdown::Model);
        ClickRect(window, g_layoutMetrics.dropdownItems[39]);
        assert(g_controller->GetSelectedModel() == L"model-39" && g_controller->GetActiveDropdown() == ActiveDropdown::None);

        server.SetHandler(ConnectorTest::MockConnectorServer::Default);
        g_controller->RefreshConnectorModels();
        PumpUntil(window, [] { return g_controller->IsConnectorConnected() && !g_controller->IsDiscoveringModels(); });
        RefreshControls(); ClickRect(window, g_layoutMetrics.composerModelBtn);
        ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"studio-test")]);
        ClickRect(window, g_layoutMetrics.actBtnFiles);
        assert(g_controller->GetDraftText() == draft && g_controller->GetSelectedModel() == L"studio-test");
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
    struct ClipboardFixture
    {
        struct Format { UINT id; HGLOBAL memory; };
        std::vector<Format> original;
        bool enabled = false;
        DWORD lastWrite = 0;
        explicit ClipboardFixture(HWND owner)
        {
            if (!OpenClipboard(owner)) return;
            bool safe = true; SetLastError(ERROR_SUCCESS);
            for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format))
            {
                if (format != CF_UNICODETEXT && format != CF_TEXT && format != CF_OEMTEXT && format != CF_LOCALE)
                { safe = false; break; }
                const auto data = GetClipboardData(format);
                const auto size = data ? GlobalSize(data) : 0;
                if (!size || size > 16 * 1024 * 1024) { safe = false; break; }
                const auto source = GlobalLock(data);
                const auto saved = GlobalAlloc(GMEM_MOVEABLE, size);
                const auto destination = saved ? GlobalLock(saved) : nullptr;
                if (!source || !destination)
                { if (source) GlobalUnlock(data); if (destination) GlobalUnlock(saved); if (saved) GlobalFree(saved); safe = false; break; }
                memcpy(destination, source, size); GlobalUnlock(data); GlobalUnlock(saved);
                original.push_back({format, saved});
            }
            if (GetLastError() != ERROR_SUCCESS) safe = false;
            CloseClipboard(); enabled = safe;
        }
        void RecordWrite() { lastWrite = g_lastCopyClipboardSequence; }
        ~ClipboardFixture()
        {
            if (enabled && lastWrite && GetClipboardSequenceNumber() == lastWrite && OpenClipboard(g_mainWindow))
            {
                if (GetClipboardSequenceNumber() == lastWrite && EmptyClipboard())
                    for (auto& format : original)
                        if (SetClipboardData(format.id, format.memory)) format.memory = nullptr;
                CloseClipboard();
            }
            for (const auto& format : original) if (format.memory) GlobalFree(format.memory);
        }
    };
    std::wstring ReadClipboardUnicode()
    {
        assert(OpenClipboard(g_mainWindow));
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        const auto size = memory ? GlobalSize(memory) : 0;
        assert(size >= sizeof(wchar_t) && size <= 64 * 1024 * 1024);
        const auto data = static_cast<const wchar_t*>(GlobalLock(memory));
        assert(data);
        const auto count = size / sizeof(wchar_t);
        size_t length = 0; while (length < count && data[length]) ++length;
        assert(length < count);
        const std::wstring result(data, length);
        GlobalUnlock(memory); CloseClipboard(); return result;
    }
    void TestCopyControls(HWND window, const std::filesystem::path& fixtureRoot)
    {
        using Lattice::HitTargetType;
        ClipboardFixture clipboard(window);
        g_controller = std::make_unique<Lattice::AppController>(); g_controller->Initialize(nullptr);
        g_keyboardFocus = false; RefreshControls();
        const auto sequence = GetClipboardSequenceNumber();
        if (clipboard.enabled)
        {
            assert(HandleCommand(CopyChatCommand));
            assert(g_copyFeedbackError && GetClipboardSequenceNumber() == sequence);
        }
        g_controller->NewSession(); RefreshControls();
        assert(!g_layoutMetrics.chatHasCopyableMessages);
        assert(Lattice::UIComponents::HitTest(g_layoutMetrics,
            (g_layoutMetrics.chatCopyButton.left + g_layoutMetrics.chatCopyButton.right) / 2,
            (g_layoutMetrics.chatCopyButton.top + g_layoutMetrics.chatCopyButton.bottom) / 2,
            g_controller->GetSidebarMode(), Lattice::ActiveDropdown::None, {}).type == HitTargetType::None);
        OpenMenu(Lattice::ActiveDropdown::Session);
        assert(!g_controller->GetCurrentDropdownItems()[MenuRow(L"Copy chat")].enabled);
        g_controller->CloseDropdown();
        Lattice::SessionTab session; session.title = L"Copy chat fixture"; session.draftText = L"COMPOSER_DRAFT_EXCLUDED";
        session.messages = {{L"user", L"You", L"", L"Unicode \u03A9 \u4E2D \U0001F680", {}, {}},
            {L"assistant", L"model", L"", L"", {}, {}},
            {L"assistant", L"model", L"", L"**Raw Markdown**\n`code`", {L"Structured item"}, {{1, L"Plan title", L"Plan detail"}}}};
        std::wstring error; const auto path = fixtureRoot / L"copy-chat.lattice";
        assert(Lattice::SessionPersistence::Save(path.wstring(), session, error) && g_controller->OpenSessionFromPath(path.wstring(), error));
        RefreshControls();
        assert(g_layoutMetrics.chatHasCopyableMessages && g_layoutMetrics.messageCopyButtons.size() == session.messages.size());
        assert(g_layoutMetrics.messageCopyButtons[1].right == g_layoutMetrics.messageCopyButtons[1].left);
        const auto targets = FocusTargets();
        assert(std::any_of(targets.begin(), targets.end(), [](const auto& target) { return target.type == HitTargetType::CopyChat; }));
        assert(std::any_of(targets.begin(), targets.end(), [](const auto& target) { return target.type == HitTargetType::CopyMessage && target.index == 2; }));
        if (clipboard.enabled)
        {
            ClickRect(window, g_layoutMetrics.messageCopyButtons[2]); clipboard.RecordWrite();
            assert(!g_copyFeedbackError && ReadClipboardUnicode() == Lattice::ChatCopy::Message(session.messages[2]).text);
            assert(g_copyFeedback == L"Message copied" && ReadEditText(g_editControl) == session.draftText);
            ClickRect(window, g_layoutMetrics.chatCopyButton); clipboard.RecordWrite();
            const auto transcript = Lattice::ChatCopy::Transcript(session);
            assert(transcript.success && ReadClipboardUnicode() == transcript.text && transcript.text.find(session.draftText) == std::wstring::npos);
            OpenMenu(Lattice::ActiveDropdown::Session);
            ClickRect(window, g_layoutMetrics.dropdownItems[MenuRow(L"Copy chat")]); clipboard.RecordWrite();
            assert(!g_copyFeedbackError && ReadClipboardUnicode() == transcript.text && g_controller->GetActiveDropdown() == Lattice::ActiveDropdown::None);
            {
                KeyboardState keys(true, true);
                MSG message{}; message.hwnd = g_editControl; message.message = WM_KEYDOWN; message.wParam = 'C';
                assert(PreTranslateAppMessage(message)); clipboard.RecordWrite();
            }
            assert(ReadClipboardUnicode() == transcript.text && ReadEditText(g_editControl) == session.draftText);
            SetFocus(window); g_keyboardFocus = true;
            g_keyboardTarget = {HitTargetType::CopyMessage, 0, g_layoutMetrics.messageCopyButtons[0]};
            assert(HandleNavigationKey(VK_RETURN)); clipboard.RecordWrite();
            assert(ReadClipboardUnicode() == Lattice::ChatCopy::Message(session.messages[0]).text);
            g_copyFeedbackUntil = GetTickCount64() - 1; SendMessageW(window, WM_TIMER, AnimTimerId, 0);
            assert(g_copyFeedback.empty());
        }
        else std::cout << "Clipboard mutation checks skipped to preserve unsupported or busy user clipboard data.\n";
        g_controller = std::make_unique<Lattice::AppController>(); g_controller->Initialize(nullptr);
        g_keyboardFocus = false; RefreshControls();
    }
    void TestChatFontControls(HWND window, const std::filesystem::path& fixtureRoot,
        ConnectorTest::MockConnectorServer& connector)
    {
        assert(g_dpi == 144);
        g_controller = std::make_unique<Lattice::AppController>(); g_controller->Initialize(nullptr);
        g_controller->ConfigureSettingsPathForTesting((fixtureRoot / L"chat-font-settings.json").wstring());
        g_controller->NewSession(); g_keyboardFocus = false; RefreshControls();
        const auto originalHeading = g_layoutMetrics.chatHeading;
        const auto originalSidebar = g_layoutMetrics.sidebar;
        const float originalEditHeight = g_layoutMetrics.composerEditArea.bottom - g_layoutMetrics.composerEditArea.top;
        const std::wstring draft = L"Preserve \u03A9 draft and selection\nsecond line";
        SetComposerText(draft); SendMessageW(g_editControl, EM_SETSEL, 3, 8);
        std::wstring error;
        assert(g_controller->ApplyConnectorSettings(Lattice::Connector::Settings{connector.Url(), 24.0f}, error));
        RefreshControls();
        LOGFONTW font{};
        assert(GetObjectW(g_editFont, sizeof(font), &font) == sizeof(font));
        assert(font.lfHeight == -static_cast<LONG>(std::lround(24.0f * Scale())));
        assert(g_d2dContext->GetChatFontSize() == 24.0f && g_editChatFontSize == 24.0f);
        assert(g_layoutMetrics.composerEditArea.bottom - g_layoutMetrics.composerEditArea.top > originalEditHeight * 2);
        assert(g_layoutMetrics.composerEditArea.bottom < g_layoutMetrics.composerSendBtn.top);
        assert(g_layoutMetrics.chatHeading.top == originalHeading.top && g_layoutMetrics.chatHeading.bottom == originalHeading.bottom);
        assert(g_layoutMetrics.sidebar.top == originalSidebar.top && g_layoutMetrics.sidebar.bottom == originalSidebar.bottom);
        DWORD start = 0, end = 0;
        SendMessageW(g_editControl, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        assert(start == 3 && end == 8 && ReadEditText(g_editControl) == draft && g_controller->GetDraftText() == draft);
        RECT edit{}; assert(GetWindowRect(g_editControl, &edit));
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&edit), 2);
        assert(std::abs((edit.bottom - edit.top) - static_cast<int>(std::lround(
            (g_layoutMetrics.composerEditArea.bottom - g_layoutMetrics.composerEditArea.top) * Scale()))) <= 1);
        assert(g_controller->ApplyConnectorSettings(Lattice::Connector::Settings{connector.Url(), 11.5f}, error));
        RefreshControls();
        assert(GetObjectW(g_editFont, sizeof(font), &font) == sizeof(font));
        assert(font.lfHeight == -static_cast<LONG>(std::lround(11.5f * Scale())) && g_d2dContext->GetChatFontSize() == 11.5f);
        SendMessageW(g_editControl, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        assert(start == 3 && end == 8 && ReadEditText(g_editControl) == draft && g_controller->GetDraftText() == draft);
        assert(g_layoutMetrics.composerEditArea.bottom - g_layoutMetrics.composerEditArea.top == originalEditHeight);
        g_controller = std::make_unique<Lattice::AppController>(); g_controller->Initialize(nullptr);
        g_keyboardFocus = false; RefreshControls();
    }
    void TestProjectResourceControls(HWND window, const std::filesystem::path& fixtureRoot,
        ConnectorTest::MockConnectorServer& connector)
    {
        namespace fs = std::filesystem;
        using Lattice::HitTargetType;
        g_controller = std::make_unique<Lattice::AppController>();
        g_controller->Initialize(nullptr);
        g_controller->ConfigureResourcePreferencesPathForTesting((fixtureRoot / L"resource-preferences").wstring());
        g_keyboardFocus = false;
        std::wstring error;
        assert(g_controller->CreateNewItem({Lattice::ItemCategory::Projects, L"work-project", L"Resource controls", fixtureRoot.wstring()}, error));
        const auto project = fixtureRoot / L"Resource controls";
        fs::create_directories(project / L"agents" / L"Alpha");
        fs::create_directories(project / L"agents" / L"Beta");
        fs::create_directories(project / L"skills" / L"Writing");
        fs::create_directories(project / L"mcp" / L"Shared");
        {
            std::ofstream configuration(project / L"mcp" / L"Shared" / L"mcp.json", std::ios::binary);
            configuration << R"({"servers":{"github":{"type":"http","url":"http://127.0.0.1:1/mcp"},"playwright":{"command":"connector-test-command-never-run","args":[]}}})";
            assert(configuration.good());
        }
        const auto requestsBeforeResources = connector.RequestCount();
        g_controller->RefreshProjectResources();
        RefreshControls(); ClickRect(window, g_layoutMetrics.actBtnSession);
        assert(g_controller->GetTabs().empty() && !g_layoutMetrics.hasActiveSession);
        assert(g_controller->GetAgents().size() == 2 && g_controller->GetSkills().size() == 1 && g_controller->GetMcpServers().size() == 2);
        assert(!g_controller->GetSkills()[0].isEnabled);
        assert(!g_controller->GetAgents()[0].isEnabled && !g_controller->GetAgents()[1].isEnabled);
        for (const auto& server : g_controller->GetMcpServers())
            assert(!server.isEnabled && server.connectionState == Lattice::McpConnectionState::Disabled && server.connectionError.empty());
        assert(g_controller->GetMcpServers()[0].fullPath == g_controller->GetMcpServers()[1].fullPath
            && g_controller->GetMcpServers()[0].serverKey != g_controller->GetMcpServers()[1].serverKey);
        assert(g_layoutMetrics.titleSearch.right == g_layoutMetrics.titleSearch.left);
        assert(g_layoutMetrics.sidebarAgentSelect.right == g_layoutMetrics.sidebarAgentSelect.left);
        assert(g_layoutMetrics.sidebarTogglePlan.right == g_layoutMetrics.sidebarTogglePlan.left);
        assert(g_layoutMetrics.sidebarToggleSafeTools.right == g_layoutMetrics.sidebarToggleSafeTools.left);
        assert(g_layoutMetrics.sidebarAddSkillBtn.right == g_layoutMetrics.sidebarAddSkillBtn.left);
        const auto targets = FocusTargets();
        for (const auto& target : targets)
            assert(target.type != HitTargetType::TitleSearch && target.type != HitTargetType::SidebarAgentSelect
                && target.type != HitTargetType::SidebarTogglePlan && target.type != HitTargetType::SidebarToggleSafeTools
                && target.type != HitTargetType::SidebarAddSkill);
        assert(std::count_if(targets.begin(), targets.end(), [](const auto& target) { return target.type == HitTargetType::SidebarMcpServerItem; }) == 2);
        assert(std::none_of(targets.begin(), targets.end(), [](const auto& target) {
            return (target.type == HitTargetType::SidebarAgentMoveUp && target.index == 0)
                || (target.type == HitTargetType::SidebarAgentMoveDown && target.index == 1);
        }));
        ClickRect(window, g_layoutMetrics.sidebarSkillRows[0]);
        assert(g_controller->GetSkills()[0].isEnabled && g_controller->GetResourcePreferencesError().empty());
        ClickRect(window, g_layoutMetrics.sidebarAgentMoveDownBtns[0]);
        assert(g_controller->GetAgents()[0].name == L"Beta" && g_controller->GetAgents()[1].name == L"Alpha");
        assert(g_controller->GetTabs().empty() && connector.RequestCount() == requestsBeforeResources);
        ClickRect(window, g_layoutMetrics.sidebarAgentToggleBtns[1]);
        assert(g_controller->GetAgents()[1].name == L"Alpha" && g_controller->GetAgents()[1].isEnabled && !g_controller->GetAgents()[0].isEnabled);
        {
            KeyboardState keys;
            SetFocus(window);
            g_keyboardFocus = true; g_keyboardTarget = {HitTargetType::SidebarAgentMoveUp, 1, g_layoutMetrics.sidebarAgentMoveUpBtns[1]};
            assert(HandleNavigationKey(VK_RETURN));
            assert(g_controller->GetAgents()[0].name == L"Alpha" && g_keyboardFocus
                && g_keyboardTarget.type == HitTargetType::SidebarAgentMoveDown && g_keyboardTarget.index == 0);
            assert(g_controller->GetAgents()[0].isEnabled && !g_controller->GetAgents()[1].isEnabled);
            assert(HandleNavigationKey(VK_RETURN));
            assert(g_controller->GetAgents()[1].name == L"Alpha" && g_keyboardFocus
                && g_keyboardTarget.type == HitTargetType::SidebarAgentMoveUp && g_keyboardTarget.index == 1);
            g_keyboardTarget = {HitTargetType::SidebarAgentItem, 1, g_layoutMetrics.sidebarAgentToggleBtns[1]};
            assert(HandleNavigationKey(VK_RETURN) && !g_controller->GetAgents()[1].isEnabled);
        }
        {
            KeyboardState keys;
            SetFocus(window);
            g_keyboardFocus = true; g_keyboardTarget = {HitTargetType::SidebarSkillItem, 0, g_layoutMetrics.sidebarSkillRows[0]};
            assert(HandleNavigationKey(VK_RETURN));
            assert(!g_controller->GetSkills()[0].isEnabled);
        }
        // New folders change source indices. WM_TIMER must update geometry before
        // a click can arrive, even if WM_PAINT has not run yet.
        for (int i = 0; i < 80; ++i) fs::create_directories(project / L"skills" / (L"Extra " + std::to_wstring(100 + i)));
        const auto previousRows = g_layoutMetrics.sidebarSkillRows.size();
        g_controller->RefreshProjectResources();
        assert(g_controller->GetSkills().size() == 81 && g_layoutMetrics.sidebarSkillRows.size() == previousRows);
        SendMessageW(window, WM_TIMER, AnimTimerId, 0);
        assert(g_layoutMetrics.sidebarSkillRows.size() == g_controller->GetSkills().size());
        ClickRect(window, g_layoutMetrics.sidebarSkillRows[0]);
        assert(g_controller->GetSkills()[0].name == L"Extra 100" && g_controller->GetSkills()[0].isEnabled);
        {
            KeyboardState keys;
            SetFocus(window);
            g_keyboardFocus = true; g_keyboardTarget = {HitTargetType::SidebarSkillItem, 0, g_layoutMetrics.sidebarSkillRows[0]};
            assert(HandleNavigationKey(VK_END));
            assert(g_controller->GetSidebarScrollOffset() == g_layoutMetrics.sidebarMaxScroll && g_layoutMetrics.sidebarMaxScroll > 0);
            assert(g_keyboardFocus && g_keyboardTarget.rect.top >= g_layoutMetrics.sidebarViewport.top);
            assert(HandleNavigationKey(VK_HOME) && g_controller->GetSidebarScrollOffset() == 0);
            assert(HandleNavigationKey(VK_NEXT) && g_controller->GetSidebarScrollOffset() > 0);
            assert(HandleNavigationKey(VK_PRIOR) && g_controller->GetSidebarScrollOffset() == 0);
        }
        Lattice::SessionTab markdown;
        markdown.title = L"Markdown native UI"; markdown.projectPath = project.wstring(); markdown.projectName = L"Resource controls";
        markdown.draftText = L"Editable draft is preserved";
        const std::wstring raw = L"# Native Markdown\n\n**Bold** and *italic* with `inline code`.\n\n- One\n- Two\n\n> Quoted note\n\n```cpp\nint value = 42;\n```\n\n| Server | State |\n| --- | --- |\n| github | Off |";
        markdown.messages.push_back({L"assistant", L"studio-test", L"", raw, {}, {}});
        const auto saved = project / L"sessions" / L"Markdown.lattice";
        assert(Lattice::SessionPersistence::Save(saved.wstring(), markdown, error));
        assert(g_controller->OpenSessionFromPath(saved.wstring(), error));
        g_keyboardFocus = false; RefreshControls();
        const auto bodyWidth = g_layoutMetrics.conversationInner.right - g_layoutMetrics.conversationInner.left - 38;
        assert(std::abs(g_layoutMetrics.conversationContentHeight - (98 + Lattice::MeasureMarkdown(*g_d2dContext, raw, bodyWidth))) < 0.01f);
        SendMessageW(window, WM_PAINT, 0, 0);
        assert(g_controller->GetTabs()[0].messages[0].text == raw && ReadEditText(g_editControl) == markdown.draftText);
        assert(g_controller->SaveSessionToPath(0, saved.wstring(), error));
        Lattice::SessionTab reopened;
        assert(Lattice::SessionPersistence::Load(saved.wstring(), reopened, error) && reopened.messages[0].text == raw);
        g_controller = std::make_unique<Lattice::AppController>();
        g_controller->Initialize(nullptr); g_keyboardFocus = false; RefreshControls();
    }
}

int main()
{
    InteractionFixture fixture;
    ConnectorTest::MockConnectorServer server;
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
    g_controller->ConfigureSettingsPathForTesting((fixture.root / L"connector-settings.json").wstring());
    std::wstring connectionError;
    assert(g_controller->ApplyConnectorSettings(Lattice::Connector::Settings{server.Url()}, connectionError));
    PumpUntil(window, [] { return g_controller->IsConnectorConnected(); });
    assert(g_controller->GetSelectedModel() == L"studio-test");
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
    TestModelPickers(window, server);
    // Search owns an explicit fixture, independently of startup content.
    SetComposerText(L"native-search-fixture-omega");
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    PumpUntil(window, [] { return g_controller->GetTabs()[0].messages.size() == 2 && !g_controller->IsGeneratingReply(); });
    assert(g_controller->GetTabs()[0].messages.back().text == L"Connector test reply");

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
    PumpUntil(window, [sentTab, messageCount] { return g_controller->GetTabs()[sentTab].messages.size() == messageCount + 2; });
    assert(g_controller->GetTabs()[sentTab].messages.size() == messageCount + 2);
    assert(g_controller->GetTabs()[sentTab].messages.back().text == L"Connector test reply");
    assert(g_controller->GetTabs()[g_controller->GetActiveTab()].messages.empty());
    const auto requests = server.Requests();
    assert(std::count_if(requests.begin(), requests.end(), [](const auto& request) {
        return request.method == "POST" && request.path == "/v1/chat/completions";
    }) >= 2);

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
    assert(g_controller->GetSelectedModel() == L"second-model");
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

    TestProjectResourceControls(window, fixture.root, server);
    TestCopyControls(window, fixture.root);
    TestChatFontControls(window, fixture.root, server);

    // Windows may cancel an accepted shutdown query. Only a confirmed
    // WM_ENDSESSION(TRUE) must stop the actual main-window transport.
    g_controller->ConfigureSettingsPathForTesting((fixture.root / L"shutdown-settings.json").wstring());
    assert(g_controller->ApplyConnectorSettings(Lattice::Connector::Settings{server.Url()}, connectionError));
    PumpUntil(window, [] { return g_controller->IsConnectorConnected(); });
    server.SetHandler([](const ConnectorTest::Request& request) {
        auto response = ConnectorTest::MockConnectorServer::Default(request);
        if (request.method == "POST") response.delayMs = 5000;
        return response;
    });
    g_controller->NewSession(); RefreshControls();
    SetComposerText(L"native shutdown lifecycle fixture");
    const auto requestsBeforeShutdown = server.RequestCount();
    SendMessageW(g_editControl, WM_CHAR, VK_RETURN, 0);
    assert(server.WaitForRequests(requestsBeforeShutdown + 1));
    assert(g_controller->IsGeneratingReply());
    assert(g_controller->SaveSessionToPath(0, (fixture.root / L"shutdown.lattice").wstring(), connectionError));
    assert(SendMessageW(window, WM_QUERYENDSESSION, 0, 0) == TRUE);
    assert(g_controller->IsGeneratingReply() && g_controller->IsConnectorConnected());
    SendMessageW(window, WM_ENDSESSION, FALSE, 0);
    assert(IsWindow(window) && g_controller->IsGeneratingReply() && g_controller->IsConnectorConnected());
    SendMessageW(window, WM_ENDSESSION, TRUE, 0);
    assert(IsWindow(window) && !g_controller->IsGeneratingReply());
    assert(g_controller->GetTabs()[0].messages.size() == 1);

    DestroyWindow(window);
    UnregisterClassW(WindowClassName, instance);
    CoUninitialize();
    std::cout << "Native interaction regressions passed.\n";
}
