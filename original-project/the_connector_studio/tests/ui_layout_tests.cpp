#include "UIComponents.h"
#include "MarkdownRenderer.h"
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
        float scroll = 0.0f, bool expanded = true, float width = 1000.0f, float dropdownScroll = 0.0f)
    {
        UIComponents::ComputeLayout(layout, width, 700.0f, mode, tabs, active, files, {}, {}, dropdown, items, scroll, expanded, false, dropdownScroll);
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
    void CheckPngModelText(const std::wstring& path, const D2D1_RECT_F& row)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))), "model render decoder must initialize");
        Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)), "model menu PNG must decode");
        Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)), "model menu frame must decode");
        Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "model menu pixels must decode");
        const WICRect area{static_cast<INT>(row.left + 25.0f), static_cast<INT>(row.top + 3.0f),
            static_cast<INT>(row.right - row.left - 34.0f), static_cast<INT>(row.bottom - row.top - 6.0f)};
        std::vector<BYTE> pixels(static_cast<size_t>(area.Width) * area.Height * 4);
        Check(SUCCEEDED(converter->CopyPixels(&area, area.Width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "model row pixels must be readable");
        size_t textPixels = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] > 150 && pixels[i + 1] > 150 && pixels[i + 2] > 150) ++textPixels;
        Check(textPixels > 20, "an open model dropdown must paint readable model text above the composer");
    }
    void CheckPngResourceToggle(const std::wstring& path, const D2D1_RECT_F& row, bool enabled, float centerY = 20)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))), "resource render decoder must initialize");
        Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)), "resource render PNG must decode");
        Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)), "resource render frame must decode");
        Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "resource render pixels must decode");
        WICRect sample{static_cast<INT>(row.right - 15), static_cast<INT>(row.top + centerY), 1, 1};
        BYTE pixel[4]{};
        Check(SUCCEEDED(converter->CopyPixels(&sample, 4, 4, pixel)), "resource toggle pixel must be readable");
        const auto expected = enabled ? Colors::ToggleBgOn : Colors::ToggleBgOff;
        const auto channel = [](float value) { return static_cast<int>(std::lround(value * 255)); };
        Check(std::abs(static_cast<int>(pixel[0]) - channel(expected.b)) <= 1
            && std::abs(static_cast<int>(pixel[1]) - channel(expected.g)) <= 1
            && std::abs(static_cast<int>(pixel[2]) - channel(expected.r)) <= 1,
            "resource toggles must paint the persisted enabled/disabled state");
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
            Check(Visible(layout.sidebarModelSelect) == (mode == SidebarMode::Session), "session panel must allow choosing a default model without creating a chat");
            Check(!Visible(layout.titleSearch) && !Visible(layout.sidebarAgentSelect) && !Visible(layout.sidebarTogglePlan)
                && !Visible(layout.sidebarToggleSafeTools) && !Visible(layout.sidebarAddSkillBtn), "removed controls must have no geometry");
            Check(HitCenter(layout, layout.tabNewBtn, mode).type == HitTargetType::TabNew, "explicit new session action must remain available");
            Check(UIComponents::HitTest(layout, 500, 600, mode, ActiveDropdown::None, {}).type == HitTargetType::None, "blank workspace must not retain composer hit targets");
            Check(UIComponents::UpdateConversationScrollMetrics(layout, 9999, 9999) == 0 && !Visible(layout.scrollbarThumb), "no session must reject stale scroll geometry");
            Compute(layout, tabs, -1, {}, mode, ActiveDropdown::Session, items);
            Check(HitCenter(layout, layout.dropdownItems[0], mode, ActiveDropdown::Session, items).type == HitTargetType::DropdownItem, "session menu must work before creating a session");
            for (const auto dropdown : {ActiveDropdown::Model, ActiveDropdown::SidebarModel})
            {
                Compute(layout, tabs, -1, {}, mode, dropdown, items);
                const bool anchored = dropdown == ActiveDropdown::SidebarModel && mode == SidebarMode::Session;
                Check(Visible(layout.dropdownRect) == anchored && (anchored || layout.dropdownItems.empty()), "model menus must follow the available selector without creating a session");
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
        Check(!Visible(layout.sidebarAgentSelect) && !Visible(layout.sidebarTogglePlan) && !Visible(layout.sidebarToggleSafeTools)
            && !Visible(layout.sidebarAddSkillBtn), "old agent, planning, tools, and Add skill controls must not be interactive");
        Check(UIComponents::HitTest(layout, 945.0f, 19.0f, SidebarMode::Session, ActiveDropdown::None, {}).type == HitTargetType::None,
            "removed title search button must not retain a hit target");
    }
    void TestProjectResources()
    {
        std::vector<AgentItem> agents = {{L"Research", L"C:\\project\\agents\\Research"},
            {L"Review", L"C:\\project\\agents\\Review"}, {L"Plan", L"C:\\project\\agents\\Plan"}};
        agents[1].isEnabled = true;
        std::vector<SkillItem> skills = {{L"MD", L"Writing", L"Writing instructions", false, L"C:\\project\\skills\\Writing"},
            {L"MD", L"Checking", L"Checking instructions", true, L"C:\\project\\skills\\Checking"}};
        std::vector<McpServerItem> servers = {{L"github", L"C:\\project\\mcp_servers\\Shared", false, L"", McpConnectionState::Disabled, L"", L"github"},
            {L"playwright", L"C:\\project\\mcp_servers\\Shared", true, L"", McpConnectionState::Connected, L"", L"playwright"}};
        LayoutMetrics layout;
        const auto compute = [&](float scroll = 0.0f, float height = 700.0f) {
            UIComponents::ComputeLayout(layout, 1000, height, SidebarMode::Session, {}, -1, {}, skills, {},
                ActiveDropdown::None, {}, scroll, true, true, 0, agents, servers);
        };
        compute();
        Check(!layout.hasActiveSession && layout.hasProject && Visible(layout.sidebarModelSelect), "project resources must not create a session");
        Check(layout.sidebarAgentRows.size() == agents.size() && layout.sidebarSkillRows.size() == skills.size()
            && layout.sidebarMcpServerRows.size() == servers.size(), "resource rows must preserve every discovered source index");
        for (int i = 0; i < static_cast<int>(agents.size()); ++i)
        {
            const auto toggle = HitCenter(layout, layout.sidebarAgentToggleBtns[i], SidebarMode::Session);
            Check(toggle.type == HitTargetType::SidebarAgentItem && toggle.index == i, "agent toggles must be independent of priority arrows and keep source indices");
            Check(layout.sidebarAgentToggleBtns[i].left > 228, "agent toggle hit area must not overlap priority arrows");
        }
        Check(!Visible(layout.sidebarAgentMoveUpBtns.front()) && !Visible(layout.sidebarAgentMoveDownBtns.back()),
            "unavailable first-up and last-down priority buttons must have no hit or keyboard target");
        const auto up = HitCenter(layout, layout.sidebarAgentMoveUpBtns[1], SidebarMode::Session);
        const auto down = HitCenter(layout, layout.sidebarAgentMoveDownBtns[1], SidebarMode::Session);
        Check(up.type == HitTargetType::SidebarAgentMoveUp && up.index == 1 && down.type == HitTargetType::SidebarAgentMoveDown && down.index == 1,
            "priority buttons must report the correct ranked agent index");
        Check(UIComponents::HitTest(layout, 193, layout.sidebarAgentRows.front().top + 34, SidebarMode::Session, ActiveDropdown::None, {}).type == HitTargetType::None,
            "a disabled priority arrow must not fall through to another agent action");
        for (int i = 0; i < 2; ++i)
        {
            const auto skill = HitCenter(layout, layout.sidebarSkillRows[i], SidebarMode::Session);
            const auto server = HitCenter(layout, layout.sidebarMcpServerRows[i], SidebarMode::Session);
            Check(skill.type == HitTargetType::SidebarSkillItem && skill.index == i && server.type == HitTargetType::SidebarMcpServerItem && server.index == i,
                "enabled and disabled resources, including named servers sharing a folder, must remain separately toggleable by source index");
        }
        for (float y = layout.sidebarViewport.top + 2; y < layout.sidebarViewport.bottom; y += 8)
            for (float x = 60; x < 264; x += 8)
            {
                const auto target = UIComponents::HitTest(layout, x, y, SidebarMode::Session, ActiveDropdown::None, {}).type;
                Check(target != HitTargetType::SidebarAgentSelect && target != HitTargetType::SidebarTogglePlan
                    && target != HitTargetType::SidebarToggleSafeTools && target != HitTargetType::SidebarAddSkill,
                    "removed session controls must never be hit-tested over resource content");
            }
        for (int i = 0; i < 100; ++i)
        {
            agents.push_back({L"Agent " + std::to_wstring(i), L"C:\\project\\agents"});
            skills.push_back({L"MD", L"Skill " + std::to_wstring(i), L"Discovered folder", i % 2 != 0, L"C:\\project\\skills"});
            servers.push_back({L"Server " + std::to_wstring(i), L"C:\\project\\mcp_servers", i % 2 != 0});
        }
        compute(1e9f, 420);
        Check(layout.sidebarMaxScroll > 0 && layout.sidebarScrollOffset == layout.sidebarMaxScroll && Visible(layout.sidebarScrollbarThumb),
            "long resource lists must clamp scrolling and expose a scrollbar");
        Check(HitCenter(layout, layout.sidebarMcpServerRows.back(), SidebarMode::Session).index == static_cast<int>(servers.size()) - 1,
            "the last MCP folder must retain its source index after scrolling all resource sections");
        Check(HitCenter(layout, layout.sidebarHeading, SidebarMode::Session).type == HitTargetType::None,
            "offscreen resources must not receive clicks through the fixed session heading");
        const auto revealAgent = layout.sidebarAgentRows[50].top + layout.sidebarScrollOffset - layout.sidebarViewport.top;
        compute(revealAgent, 420);
        Check(HitCenter(layout, layout.sidebarAgentMoveDownBtns[50], SidebarMode::Session).index == 50,
            "a scrolled agent priority button must preserve the original index");
        const auto cutSkill = layout.sidebarSkillRows[50].top + layout.sidebarScrollOffset - layout.sidebarViewport.top + 35;
        compute(cutSkill, 420);
        Check(UIComponents::HitTest(layout, 90, layout.sidebarViewport.top - 1, SidebarMode::Session, ActiveDropdown::None, {}).type == HitTargetType::None,
            "a partially clipped skill row must not be interactive above the viewport");
        const auto clipped = UIComponents::HitTest(layout, 90, layout.sidebarViewport.top + 2, SidebarMode::Session, ActiveDropdown::None, {});
        Check(clipped.type == HitTargetType::SidebarSkillItem && clipped.index == 50,
            "the visible remainder of a clipped skill row must use its source index");
        UIComponents::ComputeLayout(layout, 1000, 700, SidebarMode::Files, {}, -1, {}, skills, {}, ActiveDropdown::None, {}, 0, true, true, 0, agents, servers);
        Check(layout.sidebarAgentRows.empty() && layout.sidebarSkillRows.empty() && layout.sidebarMcpServerRows.empty(),
            "switching to Explorer must clear all resource interaction geometry");
        UIComponents::ComputeLayout(layout, 1000, 700, SidebarMode::Session, {}, -1, {}, {}, {}, ActiveDropdown::None, {}, 0, true, false);
        Check(layout.sidebarAgentRows.empty() && layout.sidebarSkillRows.empty() && layout.sidebarMcpServerRows.empty()
            && Visible(layout.sidebarAgentsHeading) && Visible(layout.sidebarSkillsHeading) && Visible(layout.sidebarMcpServersHeading),
            "empty resource folders must render section empty states without placeholder rows");
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
    void TestModelPopups()
    {
        LayoutMetrics layout;
        const std::vector<SessionTab> tabs(1);
        const std::vector<MenuItem> pending = {
            {L"Retrieving Connector models...", L"", false, false},
            {L"Refresh models", L"", true},
            {L"Connection settings", L""}
        };
        for (const auto dropdown : {ActiveDropdown::Model, ActiveDropdown::SidebarModel})
        {
            Compute(layout, tabs, 0, {}, SidebarMode::Session, dropdown, pending);
            Check(Visible(layout.dropdownRect), "model controls must show connection state and actions while discovery is pending");
            Check(HitCenter(layout, layout.dropdownItems[0], SidebarMode::Session, dropdown, pending).type == HitTargetType::None,
                "model discovery status must never be selectable as a model");
            const auto refresh = HitCenter(layout, layout.dropdownItems[1], SidebarMode::Session, dropdown, pending);
            Check(refresh.type == HitTargetType::DropdownItem && refresh.index == 1, "refresh action must remain clickable after a disabled status row");
        }

        std::vector<MenuItem> models;
        for (int i = 0; i < 100; ++i)
            models.push_back({L"connector_model_" + std::to_wstring(i) + L"_" + std::wstring(80, L'x'),
                L"Server model display name " + std::to_wstring(i) + L" " + std::wstring(120, L'y'), false, true, i == 75});
        models.push_back({L"Refresh models", L"", true});
        models.push_back({L"Connection settings", L""});
        for (const auto dropdown : {ActiveDropdown::Model, ActiveDropdown::SidebarModel})
        {
            Compute(layout, tabs, 0, {}, SidebarMode::Session, dropdown, models, 0.0f, true, 760.0f);
            Check(layout.dropdownItems.size() == models.size(), "a long model library must retain source indices for every retrieved model and action");
            Check(layout.dropdownRect.left >= 0 && layout.dropdownRect.right <= layout.windowWidth &&
                layout.dropdownRect.top >= 0 && layout.dropdownRect.bottom <= layout.windowHeight,
                "a large model library must fit completely within the application window");
            Check(layout.dropdownRect.right - layout.dropdownRect.left > 300.0f,
                "long Connector model IDs must receive more space than the compact selector button");
            Check(layout.dropdownMaxScroll > 0 && Visible(layout.dropdownScrollThumb), "a large model library must expose a scrollable viewport");
            Check(layout.dropdownItems[0].bottom - layout.dropdownItems[0].top > 30.0f,
                "server display names must have a separate line without competing with model IDs");
            Check(HitCenter(layout, layout.dropdownScrollThumb, SidebarMode::Session, dropdown, models).type == HitTargetType::DropdownScrollbarThumb,
                "model scrollbar thumb must own its hit area");
            Check(UIComponents::HitTest(layout, layout.dropdownRect.left + 15.0f, layout.dropdownRect.bottom - 1.0f,
                SidebarMode::Session, dropdown, models).type == HitTargetType::None, "model popup borders must not select offscreen rows or controls below them");
            const auto selectedOffset = UIComponents::EnsureDropdownItemVisible(layout, 75);
            Check(selectedOffset > 0, "keyboard navigation must be able to reveal a model beyond the first viewport");
            Compute(layout, tabs, 0, {}, SidebarMode::Session, dropdown, models, 0.0f, true, 760.0f, selectedOffset);
            Check(layout.dropdownItems[75].top >= layout.dropdownViewport.top - 0.01f &&
                layout.dropdownItems[75].bottom <= layout.dropdownViewport.bottom + 0.01f,
                "revealing a selected model must keep its entire row visible");
            const auto selected = HitCenter(layout, layout.dropdownItems[75], SidebarMode::Session, dropdown, models);
            Check(selected.type == HitTargetType::DropdownItem && selected.index == 75,
                "scrolled model selection must use the original retrieved model index");
            const auto lastOffset = UIComponents::EnsureDropdownItemVisible(layout, static_cast<int>(models.size()) - 1);
            Compute(layout, tabs, 0, {}, SidebarMode::Session, dropdown, models, 0.0f, true, 760.0f, lastOffset);
            Check(HitCenter(layout, layout.dropdownItems.back(), SidebarMode::Session, dropdown, models).index == static_cast<int>(models.size()) - 1,
                "connection settings action must remain reachable below a large model library");
            Check(UIComponents::EnsureDropdownItemVisible(layout, 0) == 0.0f, "keyboard Home must be able to return to the first retrieved model");
            Compute(layout, tabs, 0, {}, SidebarMode::Session, dropdown, models, 0.0f, true, 760.0f, 1e9f);
            Check(layout.dropdownScrollOffset == layout.dropdownMaxScroll, "model scrolling must clamp to the actual library size");
            Check(std::abs(layout.dropdownScrollThumb.bottom - layout.dropdownScrollTrack.bottom) < 0.01f,
                "model scrollbar must reach the bottom when the library reaches its end");
        }
    }
    void TestCopyGeometryAndChatFont()
    {
        Direct2DContext context;
        Check(context.Initialize(GetDesktopWindow()), "copy geometry context must initialize");
        std::vector<SessionTab> tabs(1);
        LayoutMetrics layout;
        Compute(layout, tabs, 0);
        UIComponents::UpdateConversationCopyTargets(context, layout, tabs[0]);
        Check(!layout.chatHasCopyableMessages && layout.messageCopyButtons.empty(), "empty chat must disable all copy targets");
        Check(HitCenter(layout, layout.chatCopyButton, SidebarMode::Files).type == HitTargetType::None,
            "disabled Copy chat must not receive a mouse hit");
        for (int i = 0; i < 50; ++i)
            tabs[0].messages.push_back({L"user", L"You", L"", L"Message " + std::to_wstring(i), {}, {}});
        tabs[0].messages[1].text.clear();
        Compute(layout, tabs, 0);
        UIComponents::UpdateConversationScrollMetrics(layout, UIComponents::MeasureConversationHeight(context, layout, tabs[0]), 0);
        UIComponents::UpdateConversationCopyTargets(context, layout, tabs[0]);
        Check(layout.chatHasCopyableMessages && HitCenter(layout, layout.chatCopyButton, SidebarMode::Files).type == HitTargetType::CopyChat,
            "nonempty chat must expose its whole transcript copy button");
        Check(layout.messageCopyButtons.size() == 50 && !Visible(layout.messageCopyButtons[1]),
            "empty messages must preserve source indices without a copy target");
        const auto first = HitCenter(layout, layout.messageCopyButtons[0], SidebarMode::Files);
        Check(first.type == HitTargetType::CopyMessage && first.index == 0, "first message copy must use its original source index");
        tabs[0].scrollOffset = layout.messageCopyButtons[38].top - layout.conversationArea.top - 40;
        UIComponents::UpdateConversationScrollMetrics(layout, layout.conversationContentHeight, tabs[0].scrollOffset);
        UIComponents::UpdateConversationCopyTargets(context, layout, tabs[0]);
        const auto later = HitCenter(layout, layout.messageCopyButtons[38], SidebarMode::Files);
        Check(later.type == HitTargetType::CopyMessage && later.index == 38, "scrolling must keep message copy source indices stable");
        Check(HitCenter(layout, layout.messageCopyButtons[0], SidebarMode::Files).type != HitTargetType::CopyMessage,
            "copy buttons outside the conversation clip must not be interactive");
        Check(HitCenter(layout, layout.scrollbarThumb, SidebarMode::Files).type == HitTargetType::ScrollbarThumb,
            "the scrollbar must retain hit priority over copy controls");
        LayoutMetrics original, larger;
        Compute(original, tabs, 0);
        UIComponents::ComputeLayout(larger, 1000, 700, SidebarMode::Files, tabs, 0, {}, {}, {}, ActiveDropdown::None, {},
            0, true, false, 0, {}, {}, 24);
        Check(larger.composerEditArea.bottom - larger.composerEditArea.top > 2 * (original.composerEditArea.bottom - original.composerEditArea.top),
            "a large chat font must reserve room for two editable lines");
        Check(larger.composerEditArea.bottom < larger.composerSendBtn.top && larger.composerSendBtn.bottom <= larger.composerBox.bottom,
            "large composer text must remain separate from its toolbar");
        Check(larger.chatHeading.top == original.chatHeading.top && larger.chatHeading.bottom == original.chatHeading.bottom &&
            larger.titlebar.bottom == original.titlebar.bottom && larger.sidebar.bottom == original.sidebar.bottom,
            "chat font size must keep chrome geometry unchanged");
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
        const float bodyHeight = MeasureMarkdown(context, message.text, wide.conversationInner.right - wide.conversationInner.left - 38.0f);
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
        const std::wstring markdown = L"# Review\n\n**Strong** and *emphasized* text with `inline code`.\n\n- First item\n- Second item\n\n> A quoted note\n\n```cpp\nint value = 42;\n```\n\n| Name | State |\n| --- | --- |\n| File | Ready |";
        tabs[0].messages[0].text = markdown; tabs[0].messages[0].planSteps.clear();
        const auto markdownHeight = MeasureMarkdown(context, markdown, narrow.conversationInner.right - narrow.conversationInner.left - 38);
        Check(std::abs(UIComponents::MeasureConversationHeight(context, narrow, tabs[0]) - (98 + markdownHeight)) < 0.01f,
            "conversation scroll measurements must use the exact Markdown body layout");

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
        Check(tabs[0].messages[0].text == markdown, "native Markdown rendering must preserve the raw saved/Connector message text");
        Check(std::filesystem::file_size(output) > 1000, "rendered PNG must contain content");
        const std::vector<MenuItem> discoveredModels = {
            {L"openrouter-free-tier-probabilistic-adjusted", L"Connector model display name", false, true, true},
            {L"gpt-6-luna", L"", false},
            {L"Refresh models", L"", true},
            {L"Connection settings", L""}
        };
        LayoutMetrics modelMenu;
        Compute(modelMenu, tabs, 0, {}, SidebarMode::Session, ActiveDropdown::Model, discoveredModels, 0.0f, true, 760.0f);
        Check(screen.SaveRenderToPng(output, 760, 700, [&] {
            UIComponents::Render(screen, modelMenu, SidebarMode::Session, ActiveDropdown::Model, discoveredModels,
                tabs, 0, {}, {}, {}, discoveredModels[0].label, true, true, false, L"", {});
        }), "discovered model IDs, display names, and selected checkmark must render in the native popup");
        CheckPngModelText(output, modelMenu.dropdownItems[0]);
        wchar_t testRoot[32768]{};
        const DWORD testRootLength = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", testRoot, 32768);
        if (testRootLength)
        {
            Check(testRootLength < 32768, "configured model preview fixture root must fit the native path buffer");
            std::error_code previewError;
            const auto previewRoot = std::filesystem::path(testRoot);
            Check(std::filesystem::is_directory(previewRoot, previewError) && !previewError,
                "configured model preview fixture root must already exist");
            const bool copied = std::filesystem::copy_file(output, previewRoot / L"model-picker-discovered.png",
                std::filesystem::copy_options::overwrite_existing, previewError);
            Check(copied && !previewError, "discovered model preview must be preserved inside its isolated test workspace");
        }
        std::vector<SessionTab> copyPreview(1);
        copyPreview[0].title = L"Markdown review";
        copyPreview[0].messages = {{L"user", L"You", L"10:42 AM", L"Review these **Markdown** notes.", {}, {}},
            {L"assistant", L"Connector model", L"10:43 AM", L"## Review\n\n**Bold**, *italic*, and `inline code` stay formatted.\n\n- Files stay local\n- Agents have separate toggles\n\n```cpp\nint priority = 1;\n```", {}, {}}};
        const auto copyOutput = base.wstring() + L"-copy.png";
        for (const float size : {11.5f, 24.0f})
        {
            screen.SetChatFontSize(size);
            LayoutMetrics copyLayout;
            UIComponents::ComputeLayout(copyLayout, 760, 700, SidebarMode::Files, copyPreview, 0, {}, {}, {},
                ActiveDropdown::None, {}, 0, true, false, 0, {}, {}, size);
            UIComponents::UpdateConversationScrollMetrics(copyLayout,
                UIComponents::MeasureConversationHeight(screen, copyLayout, copyPreview[0]), 0);
            UIComponents::UpdateConversationCopyTargets(screen, copyLayout, copyPreview[0]);
            Check(HitCenter(copyLayout, copyLayout.chatCopyButton, SidebarMode::Files).type == HitTargetType::CopyChat &&
                HitCenter(copyLayout, copyLayout.messageCopyButtons[0], SidebarMode::Files).type == HitTargetType::CopyMessage,
                "chat preview must expose the native whole chat and individual copy controls");
            Check(screen.SaveRenderToPng(copyOutput, 760, 700, [&] {
                UIComponents::Render(screen, copyLayout, SidebarMode::Files, ActiveDropdown::None, {}, copyPreview, 0, {}, {}, {},
                    L"studio-test", true, true, false, L"", {}, 0, false, L"", L"Connected", true, L"", {}, {}, L"", L"Chat copied");
            }), "chat copy controls and selected chat font must render to a native PNG");
            if (testRootLength)
            {
                std::error_code previewError;
                Check(std::filesystem::copy_file(copyOutput, std::filesystem::path(testRoot) /
                    (size == 24 ? L"chat-copy-font-24.png" : L"chat-copy-default.png"),
                    std::filesystem::copy_options::overwrite_existing, previewError) && !previewError,
                    "chat copy preview must be preserved inside its isolated test workspace");
            }
        }
        screen.SetChatFontSize(11.5f);
        { std::error_code ignored; std::filesystem::remove(copyOutput, ignored); }
        const std::vector<AgentItem> resourceAgents = {{L"Research", L"C:\\project\\agents\\Research", 1, false}, {L"Review", L"C:\\project\\agents\\Review", 2, true}};
        const std::vector<SkillItem> resourceSkills = {{L"MD", L"Writing", L"Writing instructions", false, L"C:\\project\\skills\\Writing"},
            {L"MD", L"Checking", L"Checking instructions", true, L"C:\\project\\skills\\Checking"}};
        const std::vector<McpServerItem> resourceServers = {{L"github", L"C:\\project\\mcp_servers\\Shared", false, L"", McpConnectionState::Disabled, L"Server unavailable", L"github"},
            {L"playwright", L"C:\\project\\mcp_servers\\Shared", true, L"", McpConnectionState::Connected, L"", L"playwright"},
            {L"Pending", L"C:\\project\\mcp_servers\\Pending", false, L"", McpConnectionState::Connecting}};
        LayoutMetrics resources;
        UIComponents::ComputeLayout(resources, 760, 700, SidebarMode::Session, {}, -1, {}, resourceSkills, {},
            ActiveDropdown::None, {}, 0, true, true, 0, resourceAgents, resourceServers);
        Check(screen.SaveRenderToPng(output, 760, 700, [&] {
            UIComponents::Render(screen, resources, SidebarMode::Session, ActiveDropdown::None, {}, {}, -1, {}, resourceSkills, {},
                L"studio-test", true, true, false, L"", {}, 0, false, L"Research project", L"Connected", true, L"", resourceAgents, resourceServers);
        }), "ranked agents, skill toggles, and MCP connection states must render without a chat session");
        CheckPngResourceToggle(output, resources.sidebarSkillRows[0], false);
        CheckPngResourceToggle(output, resources.sidebarAgentRows[0], false, 34);
        CheckPngResourceToggle(output, resources.sidebarAgentRows[1], true, 34);
        CheckPngResourceToggle(output, resources.sidebarSkillRows[1], true);
        CheckPngResourceToggle(output, resources.sidebarMcpServerRows[0], false);
        CheckPngResourceToggle(output, resources.sidebarMcpServerRows[1], true);
        CheckPngResourceToggle(output, resources.sidebarMcpServerRows[2], true);
        if (testRootLength)
        {
            std::error_code previewError;
            Check(std::filesystem::copy_file(output, std::filesystem::path(testRoot) / L"resource-sidebar.png",
                std::filesystem::copy_options::overwrite_existing, previewError) && !previewError,
                "resource sidebar preview must be preserved inside the isolated test workspace");
        }
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
        TestProjectResources();
        TestTabsAndPopups();
        TestModelPopups();
        TestCopyGeometryAndChatFont();
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
