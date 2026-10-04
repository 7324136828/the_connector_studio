#include "UIComponents.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Lattice;
namespace
{
    int checks = 0;
    void Check(bool condition, const char* description)
    {
        ++checks;
        if (!condition) throw std::runtime_error(description);
    }
    bool Visible(const D2D1_RECT_F& rect) { return rect.right > rect.left && rect.bottom > rect.top; }
    HitTestResult HitCenter(const LayoutMetrics& layout, const D2D1_RECT_F& rect, SidebarMode mode,
        ActiveDropdown dropdown = ActiveDropdown::None, const std::vector<MenuItem>& items = {})
    {
        return UIComponents::HitTest(layout, (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f, mode, dropdown, items);
    }
    void Compute(LayoutMetrics& layout, const std::vector<SessionTab>& tabs, int active,
        const std::vector<FileItem>& files = {}, SidebarMode mode = SidebarMode::Files,
        ActiveDropdown dropdown = ActiveDropdown::None, const std::vector<MenuItem>& items = {},
        float scroll = 0.0f, bool expanded = true, float width = 1000.0f)
    {
        UIComponents::ComputeLayout(layout, width, 700.0f, mode, tabs, active, files, {}, {}, dropdown, items, scroll, expanded);
    }
    void CheckPngBlankArea(const std::wstring& path, const WICRect& area)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))), "empty render decoder must initialize");
        Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)), "empty render PNG must decode");
        Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)), "empty render frame must decode");
        Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "empty render pixels must decode");
        std::vector<BYTE> pixels(static_cast<size_t>(area.Width) * area.Height * 4);
        Check(SUCCEEDED(converter->CopyPixels(&area, area.Width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "blank workspace pixels must be readable");
        bool blank = true;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] != 0x1c || pixels[i + 1] != 0x17 || pixels[i + 2] != 0x15 || pixels[i + 3] != 0xff) { blank = false; break; }
        Check(blank, "empty workspace must paint only the preserved dark background without welcome text or composer");
    }
    void TestEmptyWorkspace()
    {
        LayoutMetrics layout;
        const std::vector<SessionTab> tabs;
        const std::vector<MenuItem> items = {{L"New Session", L"Ctrl+N"}};
        for (const auto mode : {SidebarMode::Files, SidebarMode::Session, SidebarMode::Plugins})
        {
            Compute(layout, tabs, -1, {}, mode);
            Check(!layout.hasActiveSession && !layout.hasProject, "startup must have no session or project");
            Check(layout.tabs.empty() && layout.tabCloses.empty(), "startup must have zero session tabs");
            Check(!Visible(layout.sidebarProjectRow) && layout.sidebarFileRows.empty() && !Visible(layout.sidebarContextCard), "startup must have no fabricated project or context");
            Check(layout.openFileEditorsCount == 0, "startup must report zero file editors");
            Check(!Visible(layout.chatHeading) && !Visible(layout.conversationArea) && !Visible(layout.composerBox) && !Visible(layout.composerEditArea), "no session must have no chat or input geometry");
            Check(!Visible(layout.sidebarModelSelect) && !Visible(layout.sidebarAgentSelect) && !Visible(layout.sidebarAddSkillBtn), "session controls must be absent without a session");
            Check(HitCenter(layout, layout.tabNewBtn, mode).type == HitTargetType::TabNew, "explicit new session action must remain available");
            Check(UIComponents::HitTest(layout, 500, 600, mode, ActiveDropdown::None, {}).type == HitTargetType::None, "blank workspace must not retain composer hit targets");
            Check(UIComponents::UpdateConversationScrollMetrics(layout, 9999, 9999) == 0 && !Visible(layout.scrollbarThumb), "no session must reject stale scroll geometry");
            Compute(layout, tabs, -1, {}, mode, ActiveDropdown::Session, items);
            Check(HitCenter(layout, layout.dropdownItems[0], mode, ActiveDropdown::Session, items).type == HitTargetType::DropdownItem, "session menu must work before creating a session");
            for (const auto dropdown : {ActiveDropdown::Model, ActiveDropdown::SidebarModel})
            {
                Compute(layout, tabs, -1, {}, mode, dropdown, items);
                Check(!Visible(layout.dropdownRect) && layout.dropdownItems.empty(), "model menus must not appear without an anchor");
            }
        }
        std::vector<SessionTab> explicitTabs(24);
        Compute(layout, explicitTabs, 23);
        Check(layout.hasActiveSession && Visible(layout.composerEditArea), "explicit sessions must expose the composer");
        Check(!layout.hasProject && !Visible(layout.sidebarProjectRow) && layout.openFileEditorsCount == 0, "sessions must not be counted as opened files or projects");
        const auto oldComposer = layout.composerSendBtn;
        Compute(layout, tabs, -1);
        Check(HitCenter(layout, oldComposer, SidebarMode::Files).type == HitTargetType::None, "closing the last session must clear composer hit targets");
        explicitTabs[23].projectPath = L"C:\\workspace";
        explicitTabs[23].projectName = L"workspace";
        Compute(layout, explicitTabs, 23);
        Check(layout.hasProject && Visible(layout.sidebarProjectRow), "a real empty directory must still expose its project row");
        UIComponents::ComputeLayout(layout, 1000, 700, SidebarMode::Files, {}, -1, {}, {}, {},
            ActiveDropdown::None, {}, 0, true, true);
        Check(layout.hasProject && !layout.hasActiveSession && Visible(layout.sidebarProjectRow), "an independently opened project must render without a dummy session");
        Check(!Visible(layout.composerBox) && layout.openFileEditorsCount == 0, "an independently opened project must not fabricate a chat or file editor");
    }
    void TestTreeAndSidebar()
    {
        LayoutMetrics layout;
        std::vector<SessionTab> tabs(1);
        std::vector<FileItem> files = {
            {L"closed", L"folder", 0, true, false},
            {L"hidden.txt", L"txt", 1, false, true},
            {L"hidden folder", L"folder", 1, true, true},
            {L"nested.txt", L"txt", 2, false, true},
            {L"open", L"folder", 0, true, true},
            {L"visible.txt", L"txt", 1, false, true}
        };
        Compute(layout, tabs, 0, files);
        Check(layout.sidebarFileRows.size() == files.size(), "file hit rows must preserve source indices");
        Check(!Visible(layout.sidebarFileRows[1]) && !Visible(layout.sidebarFileRows[2]) && !Visible(layout.sidebarFileRows[3]), "a collapsed ancestor must hide all descendants");
        Check(HitCenter(layout, layout.sidebarFileRows[4], SidebarMode::Files).index == 4, "visible sibling must keep its source index");
        Check(std::abs(layout.sidebarFileRows[4].top - layout.sidebarFileRows[0].bottom) < 0.01f, "collapsed descendants must not leave blank rows");
        Compute(layout, tabs, 0, files, SidebarMode::Files, ActiveDropdown::None, {}, 0.0f, false);
        for (const auto& row : layout.sidebarFileRows) Check(!Visible(row), "collapsed project must hide the tree");
        files.clear();
        for (int i = 0; i < 120; ++i) files.push_back({L"file.txt", L"txt", 0, false, true});
        Compute(layout, tabs, 0, files, SidebarMode::Files, ActiveDropdown::None, {}, 100000.0f);
        Check(layout.sidebarMaxScroll > 0.0f && layout.sidebarScrollOffset == layout.sidebarMaxScroll, "sidebar scrolling must clamp to real content");
        Check(Visible(layout.sidebarScrollbarThumb), "long trees need a draggable scrollbar");
        Check(HitCenter(layout, layout.sidebarScrollbarThumb, SidebarMode::Files).type == HitTargetType::SidebarScrollbarThumb, "sidebar scrollbar must own its hit area");
        Check(HitCenter(layout, layout.sidebarHeading, SidebarMode::Files).type == HitTargetType::None, "scrolled rows must not receive clicks through the fixed heading");
        Compute(layout, tabs, 0, files, SidebarMode::Session);
        Check(layout.sidebarFileRows.empty(), "switching panels must clear old row geometry");
        Check(HitCenter(layout, layout.sidebarModelSelect, SidebarMode::Session).type == HitTargetType::SidebarModelSelect, "model selector needs a hit target");
        Check(HitCenter(layout, layout.sidebarAgentSelect, SidebarMode::Session).type == HitTargetType::SidebarAgentSelect, "agent control must have a clickable hit area");
        Check(UIComponents::HitTest(layout, 90.0f, layout.sidebarTogglePlan.top + 5.0f, SidebarMode::Session, ActiveDropdown::None, {}).type == HitTargetType::SidebarTogglePlan, "toggle labels must be clickable");
    }
    void TestTabsAndPopups()
    {
        LayoutMetrics layout;
        std::vector<SessionTab> tabs(24);
        Compute(layout, tabs, 23, {}, SidebarMode::Files, ActiveDropdown::None, {}, 0.0f, true, 760.0f);
        Check(Visible(layout.tabs[23]) && layout.tabs[23].right <= layout.tabPreviousBtn.left + 0.01f, "active overflow tab must remain visible without covering navigation");
        Check(!Visible(layout.tabs[0]), "off-screen tabs must not retain hit targets");
        Check(layout.tabNewBtn.right <= layout.windowWidth, "new session button must remain on screen");
        Check(HitCenter(layout, layout.tabNewBtn, SidebarMode::Files).type == HitTargetType::TabNew, "overflow new session button must remain clickable");
        Check(HitCenter(layout, layout.tabPreviousBtn, SidebarMode::Files).type == HitTargetType::TabPrevious, "overflow previous tab navigation must remain clickable");
        Check(HitCenter(layout, layout.tabNextBtn, SidebarMode::Files).type == HitTargetType::TabNext, "overflow next tab navigation must remain clickable");
        std::vector<MenuItem> items = {{L"One", L""}, {L"Two", L""}, {L"Three", L"", true}};
        Compute(layout, tabs, 23, {}, SidebarMode::Session, ActiveDropdown::SidebarModel, items);
        Check(std::abs(layout.dropdownRect.left - layout.sidebarModelSelect.left) < 0.01f, "sidebar model popup must anchor to the sidebar");
        Check(layout.dropdownRect.top > layout.sidebarModelSelect.bottom, "sidebar model popup should open below its control when there is room");
        Check(layout.dropdownItems[2].top - layout.dropdownItems[1].top > 30.0f, "menu separators need their own spacing");
        Check(HitCenter(layout, layout.dropdownItems[1], SidebarMode::Session, ActiveDropdown::SidebarModel, items).index == 1, "popup item hit testing must match painted rows");
        Check(UIComponents::HitTest(layout, layout.dropdownRect.left + 1.0f, layout.dropdownRect.top + 1.0f, SidebarMode::Session, ActiveDropdown::SidebarModel, items).type == HitTargetType::None, "popup padding must never click through to controls underneath");
        Compute(layout, tabs, 23, {}, SidebarMode::Session, ActiveDropdown::Model, items, 0.0f, true, 760.0f);
        Check(layout.dropdownRect.bottom < layout.composerModelBtn.top, "composer model popup must open above its control");
        Check(layout.dropdownRect.left >= 0 && layout.dropdownRect.right <= layout.windowWidth, "popup must remain inside the window");
        Compute(layout, tabs, 23);
        Check(!Visible(layout.dropdownRect) && layout.dropdownItems.empty(), "closed menus must clear popup geometry");
    }
    void TestConversationAndRender()
    {
        Direct2DContext context;
        Check(context.Initialize(GetDesktopWindow()), "DirectWrite context must initialize");
        std::vector<SessionTab> tabs(1);
        LayoutMetrics wide, narrow;
        Compute(wide, tabs, 0);
        Compute(narrow, tabs, 0, {}, SidebarMode::Files, ActiveDropdown::None, {}, 0.0f, true, 760.0f);
        Check(UIComponents::MeasureConversationHeight(context, wide, tabs[0]) == 0.0f, "empty sessions must not invent scrollable history");
        MessageItem message;
        message.author = L"You";
        message.role = L"user";
        message.text = L"Short message.";
        tabs[0].messages.push_back(message);
        const float bodyHeight = context.MeasureTextHeight(message.text, context.FontBody(), wide.conversationInner.right - wide.conversationInner.left - 38.0f);
        Check(std::abs(UIComponents::MeasureConversationHeight(context, wide, tabs[0]) - (98.0f + bodyHeight)) < 0.01f, "conversation height must include exact body and all spacing");
        tabs[0].messages[0].text.clear();
        for (int i = 0; i < 100; ++i) tabs[0].messages[0].text += L"A message with several words to test wrapping and scroll bounds. ";
        const float wideHeight = UIComponents::MeasureConversationHeight(context, wide, tabs[0]);
        const float narrowHeight = UIComponents::MeasureConversationHeight(context, narrow, tabs[0]);
        Check(narrowHeight > wideHeight, "resizing narrower must measure newly wrapped message lines");
        const float maximum = UIComponents::UpdateConversationScrollMetrics(narrow, narrowHeight, 100000.0f);
        Check(maximum > 0.0f && Visible(narrow.scrollbarThumb), "long conversation must expose exact scroll range and thumb");
        Check(std::abs(narrow.scrollbarThumb.bottom - narrow.scrollbarTrack.bottom) < 0.01f, "overscrolling must clamp the thumb to the bottom");
        Check(HitCenter(narrow, narrow.scrollbarThumb, SidebarMode::Files).type == HitTargetType::ScrollbarThumb, "conversation thumb needs a hit target");
        tabs[0].messages[0].text = L"Plan";
        tabs[0].messages[0].planSteps.push_back({1, L"Step", L"Short description"});
        const float shortPlan = UIComponents::MeasureConversationHeight(context, narrow, tabs[0]);
        auto& step = tabs[0].messages[0].planSteps[0];
        for (int i = 0; i < 50; ++i) step.description += L" A longer description with words that wrap.";
        Check(UIComponents::MeasureConversationHeight(context, narrow, tabs[0]) > shortPlan + 100.0f, "long plan descriptions must grow their rows instead of truncating");
        UIComponents::UpdateConversationScrollMetrics(narrow, 0.0f, 100000.0f);
        Check(!Visible(narrow.scrollbarThumb) && narrow.conversationMaxScroll == 0.0f, "short histories must clear stale scrollbar geometry");

        const HWND window = CreateWindowExW(0, L"STATIC", L"UI regression test", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "hidden rendering test window must be created");
        Direct2DContext screen;
        Check(screen.Initialize(window), "HWND render target must initialize");
        screen.SetDpi(144.0f);
        Check(screen.GetDpi() == 144.0f, "renderer must retain per-monitor DPI");
        const auto base = std::filesystem::temp_directory_path() / (L"lattice-ui-test-" + std::to_wstring(GetCurrentProcessId()));
        const auto output = base.wstring() + L".png";
        const auto render = [&]() {
            UIComponents::Render(screen, narrow, SidebarMode::Files, ActiveDropdown::None, {}, tabs, 0, {}, {}, {}, L"Offline", true, true, false, L"", {});
        };
        Check(screen.SaveRenderToPng(output, 760, 700, render), "native PNG rendering must succeed with target-specific brushes");
        Check(std::filesystem::file_size(output) > 1000, "rendered PNG must contain content");
        LayoutMetrics empty;
        Compute(empty, {}, -1, {}, SidebarMode::Session, ActiveDropdown::Model, {{L"Offline", L""}}, 0.0f, true, 760.0f);
        Check(screen.SaveRenderToPng(output, 760, 700, [&] {
            UIComponents::Render(screen, empty, SidebarMode::Session, ActiveDropdown::Model, {{L"Offline", L""}}, {}, -1, {}, {}, {}, L"Offline", true, true, false, L"", {});
        }), "empty workspace must render safely with an unavailable model popup");
        CheckPngBlankArea(output, WICRect{300, 100, 400, 500});
        std::vector<SessionTab> blankSession(1);
        Compute(empty, blankSession, 0, {}, SidebarMode::Files, ActiveDropdown::None, {}, 0.0f, true, 760.0f);
        Check(screen.SaveRenderToPng(output, 760, 700, [&] {
            UIComponents::Render(screen, empty, SidebarMode::Files, ActiveDropdown::None, {}, blankSession, 0, {}, {}, {}, L"Offline", true, true, false, L"", {});
        }), "a newly created blank session must render safely");
        CheckPngBlankArea(output, WICRect{300, 160, 400, 380});
        Check(!screen.SaveRenderToPng(output + L".failed", 760, 700, [] { throw std::runtime_error("test callback failure"); }), "PNG callback failure must be handled safely");
        Check(screen.BeginDraw(), "render target must remain usable after PNG callback failure");
        screen.FillRect(D2D1::RectF(0, 0, 100, 100), Colors::ShellBg);
        Check(SUCCEEDED(screen.EndDraw()), "PNG rendering must restore HWND brush resources");
        screen.Resize(800, 600);
        Check(screen.BeginDraw(), "resized render target must remain usable");
        screen.FillRect(D2D1::RectF(0, 0, 100, 100), Colors::SidebarBg);
        Check(SUCCEEDED(screen.EndDraw()), "resized drawing must succeed");
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        std::filesystem::remove(output + L".failed", ignored);
        DestroyWindow(window);
    }
}

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    try
    {
        TestEmptyWorkspace();
        TestTreeAndSidebar();
        TestTabsAndPopups();
        TestConversationAndRender();
        std::cout << checks << " UI layout/rendering checks passed.\n";
        CoUninitialize();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "UI regression: " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
