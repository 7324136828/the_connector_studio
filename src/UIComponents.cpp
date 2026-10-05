#include "UIComponents.h"
#include "AppIdentity.h"
#include "MarkdownRenderer.h"
#include "ChatCopy.h"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace Lattice
{
    static inline bool PointInRect(const D2D1_RECT_F& r, float x, float y)
    {
        return r.right > r.left && r.bottom > r.top && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }

    void UIComponents::ComputeLayout(LayoutMetrics& layout, float width, float height,
        SidebarMode sidebarMode, const std::vector<SessionTab>& tabs, int activeTab,
        const std::vector<FileItem>& files, const std::vector<SkillItem>& skills,
        const std::vector<PluginItem>& plugins, ActiveDropdown activeDropdown,
        const std::vector<MenuItem>& dropdownItems, float sidebarScrollOffset, bool projectExpanded, bool hasCurrentProject,
        float dropdownScrollOffset, const std::vector<AgentItem>& agents,
        const std::vector<McpServerItem>& mcpServers, float chatFontSize)
    {
        // Start from fresh geometry so hidden controls never retain stale hit targets.
        layout = LayoutMetrics{};
        width = (std::max)(1.0f, width);
        height = (std::max)(1.0f, height);
        layout.windowWidth = width;
        layout.windowHeight = height;
        layout.projectExpanded = projectExpanded;
        layout.hasActiveSession = activeTab >= 0 && activeTab < static_cast<int>(tabs.size());
        layout.hasProject = hasCurrentProject || !files.empty() || (layout.hasActiveSession && !tabs[activeTab].projectPath.empty());
        layout.titlebar = D2D1::RectF(0, 0, width, 38.0f);
        layout.trafficLights = D2D1::RectF(0, 0, 72.0f, 38.0f);
        layout.menuProject = D2D1::RectF(76.0f, 7.0f, 130.0f, 31.0f);
        layout.menuSession = D2D1::RectF(134.0f, 7.0f, 194.0f, 31.0f);
        const float center = width * 0.5f;
        layout.windowTitle = D2D1::RectF(center - 110.0f, 7.0f, center + 110.0f, 31.0f);

        const float contentTop = 38.0f;
        const float contentBottom = (std::max)(contentTop, height - 24.0f);
        layout.statusbar = D2D1::RectF(0, contentBottom, width, height);
        layout.activityBar = D2D1::RectF(0, contentTop, 46.0f, contentBottom);
        layout.actBtnFiles = D2D1::RectF(0, contentTop + 7.0f, 45.0f, contentTop + 49.0f);
        layout.actBtnSession = D2D1::RectF(0, contentTop + 51.0f, 45.0f, contentTop + 93.0f);
        layout.actBtnPlugins = D2D1::RectF(0, contentTop + 95.0f, 45.0f, contentTop + 137.0f);
        layout.actBtnSettings = D2D1::RectF(0, contentBottom - 76.0f, 45.0f, contentBottom - 34.0f);

        layout.sidebar = D2D1::RectF(46.0f, contentTop, 276.0f, contentBottom);
        layout.sidebarHeading = D2D1::RectF(46.0f, contentTop, 276.0f, contentTop + 43.0f);
        layout.sidebarViewport = D2D1::RectF(46.0f, contentTop + 43.0f, 276.0f, contentBottom);
        float curY = layout.sidebarViewport.top;
        if (sidebarMode == SidebarMode::Files)
        {
            if (layout.hasProject)
            {
                layout.sidebarProjectRow = D2D1::RectF(46.0f, curY, 276.0f, curY + 30.0f);
                curY += 35.0f;
            }
            layout.sidebarFileRows.resize(files.size());
            int collapsedIndent = -1;
            for (size_t i = 0; i < files.size(); ++i)
            {
                if (!projectExpanded) break;
                const auto& file = files[i];
                if (collapsedIndent >= 0 && file.indent > collapsedIndent) continue;
                collapsedIndent = -1;
                layout.sidebarFileRows[i] = D2D1::RectF(46.0f, curY, 276.0f, curY + 25.0f);
                curY += 25.0f;
                if (file.isFolder && !file.isExpanded) collapsedIndent = file.indent;
            }
            layout.sidebarOpenEditorsRow = D2D1::RectF(46.0f, curY + 16.0f, 276.0f, curY + 47.0f);
            curY += 47.0f;
            if (layout.hasActiveSession && tabs[activeTab].filesCount > 0)
            {
                layout.sidebarContextCard = D2D1::RectF(56.0f, curY + 14.0f, 266.0f, curY + 106.0f);
                curY += 106.0f;
            }
            curY += 14.0f;
        }
        else if (sidebarMode == SidebarMode::Session)
        {
            curY += 12.0f;
            layout.sidebarModelSelect = D2D1::RectF(58.0f, curY + 20.0f, 264.0f, curY + 53.0f);
            curY = layout.sidebarModelSelect.bottom + 34.0f;
            const auto beginSection = [&](D2D1_RECT_F& heading) {
                heading = D2D1::RectF(58.0f, curY, 264.0f, curY + 18.0f);
                curY += 26.0f;
            };
            beginSection(layout.sidebarAgentsHeading);
            layout.sidebarAgentMoveUpBtns.resize(agents.size());
            layout.sidebarAgentMoveDownBtns.resize(agents.size());
            for (size_t i = 0; i < agents.size(); ++i)
            {
                layout.sidebarAgentRows.push_back(D2D1::RectF(58.0f, curY, 264.0f, curY + 48.0f));
                layout.sidebarAgentToggleBtns.push_back(D2D1::RectF(235.0f, curY + 23.0f, 263.0f, curY + 45.0f));
                if (i > 0) layout.sidebarAgentMoveUpBtns[i] = D2D1::RectF(182.0f, curY + 23.0f, 204.0f, curY + 45.0f);
                if (i + 1 < agents.size()) layout.sidebarAgentMoveDownBtns[i] = D2D1::RectF(206.0f, curY + 23.0f, 228.0f, curY + 45.0f);
                curY += 50.0f;
            }
            if (agents.empty()) curY += 30.0f;
            curY += 12.0f;
            beginSection(layout.sidebarSkillsHeading);
            for (size_t i = 0; i < skills.size(); ++i)
            {
                layout.sidebarSkillRows.push_back(D2D1::RectF(58.0f, curY, 264.0f, curY + 40.0f));
                curY += 42.0f;
            }
            if (skills.empty()) curY += 30.0f;
            curY += 12.0f;
            beginSection(layout.sidebarMcpServersHeading);
            for (size_t i = 0; i < mcpServers.size(); ++i)
            {
                layout.sidebarMcpServerRows.push_back(D2D1::RectF(58.0f, curY, 264.0f, curY + 40.0f));
                curY += 42.0f;
            }
            if (mcpServers.empty()) curY += 30.0f;
            curY += 14.0f;
        }
        else if (sidebarMode == SidebarMode::Plugins)
        {
            for (size_t i = 0; i < plugins.size(); ++i)
            {
                layout.sidebarPluginCards.push_back(D2D1::RectF(46.0f, curY, 276.0f, curY + 68.0f));
                curY += 68.0f;
            }
            layout.sidebarBrowsePluginsBtn = D2D1::RectF(56.0f, curY + 10.0f, 266.0f, curY + 40.0f);
            curY += 52.0f;
        }
        layout.sidebarContentHeight = curY - layout.sidebarViewport.top;
        const float sidebarHeight = (std::max)(0.0f, contentBottom - layout.sidebarViewport.top);
        layout.sidebarMaxScroll = (std::max)(0.0f, layout.sidebarContentHeight - sidebarHeight);
        layout.sidebarScrollOffset = (std::clamp)(sidebarScrollOffset, 0.0f, layout.sidebarMaxScroll);
        const auto offsetRect = [&](D2D1_RECT_F& rect) {
            if (rect.right <= rect.left || rect.bottom <= rect.top) return;
            rect.top -= layout.sidebarScrollOffset;
            rect.bottom -= layout.sidebarScrollOffset;
        };
        offsetRect(layout.sidebarProjectRow);
        offsetRect(layout.sidebarOpenEditorsRow);
        offsetRect(layout.sidebarContextCard);
        for (auto& rect : layout.sidebarFileRows) offsetRect(rect);
        offsetRect(layout.sidebarModelSelect);
        offsetRect(layout.sidebarAgentsHeading);
        offsetRect(layout.sidebarSkillsHeading);
        offsetRect(layout.sidebarMcpServersHeading);
        for (auto& rect : layout.sidebarAgentRows) offsetRect(rect);
        for (auto& rect : layout.sidebarAgentToggleBtns) offsetRect(rect);
        for (auto& rect : layout.sidebarAgentMoveUpBtns) offsetRect(rect);
        for (auto& rect : layout.sidebarAgentMoveDownBtns) offsetRect(rect);
        for (auto& rect : layout.sidebarSkillRows) offsetRect(rect);
        for (auto& rect : layout.sidebarMcpServerRows) offsetRect(rect);
        for (auto& rect : layout.sidebarPluginCards) offsetRect(rect);
        offsetRect(layout.sidebarBrowsePluginsBtn);
        if (layout.sidebarMaxScroll > 0.0f && sidebarHeight > 0.0f)
        {
            layout.sidebarScrollbarTrack = D2D1::RectF(270.0f, layout.sidebarViewport.top, 275.0f, contentBottom);
            const float thumbHeight = (std::min)(sidebarHeight, (std::max)(24.0f, sidebarHeight * sidebarHeight / layout.sidebarContentHeight));
            const float thumbTop = layout.sidebarViewport.top + layout.sidebarScrollOffset / layout.sidebarMaxScroll * (sidebarHeight - thumbHeight);
            layout.sidebarScrollbarThumb = D2D1::RectF(270.0f, thumbTop, 275.0f, thumbTop + thumbHeight);
        }

        layout.editorArea = D2D1::RectF(276.0f, contentTop, width, contentBottom);
        layout.tabsBar = D2D1::RectF(276.0f, contentTop, width, contentTop + 36.0f);
        layout.tabs.resize(tabs.size());
        layout.tabCloses.resize(tabs.size());
        const float editorWidth = (std::max)(1.0f, width - 276.0f);
        const bool tabOverflow = static_cast<float>(tabs.size()) * 160.0f + 36.0f > editorWidth;
        float tabsRight = width - 36.0f;
        float tabX = 276.0f;
        if (tabOverflow)
        {
            layout.tabNewBtn = D2D1::RectF(width - 36.0f, contentTop, width, contentTop + 36.0f);
            layout.tabPreviousBtn = D2D1::RectF(width - 92.0f, contentTop, width - 64.0f, contentTop + 36.0f);
            layout.tabNextBtn = D2D1::RectF(width - 64.0f, contentTop, width - 36.0f, contentTop + 36.0f);
            tabsRight = width - 92.0f;
            const int capacity = (std::max)(1, static_cast<int>((tabsRight - tabX) / 120.0f));
            layout.visibleTabCount = (std::min)(capacity, static_cast<int>(tabs.size()));
            layout.firstVisibleTab = (std::clamp)(activeTab - capacity + 1, 0, (std::max)(0, static_cast<int>(tabs.size()) - capacity));
        }
        else
        {
            layout.visibleTabCount = static_cast<int>(tabs.size());
        }
        const float tabWidth = layout.visibleTabCount > 0 ? (std::min)(160.0f, (tabsRight - tabX) / layout.visibleTabCount) : 160.0f;
        for (int i = layout.firstVisibleTab; i < layout.firstVisibleTab + layout.visibleTabCount; ++i)
        {
            layout.tabs[i] = D2D1::RectF(tabX, contentTop, tabX + tabWidth, contentTop + 36.0f);
            layout.tabCloses[i] = D2D1::RectF(tabX + tabWidth - 25.0f, contentTop + 7.0f, tabX + tabWidth - 3.0f, contentTop + 29.0f);
            tabX += tabWidth;
        }
        if (!tabOverflow) layout.tabNewBtn = D2D1::RectF(tabX, contentTop, tabX + 36.0f, contentTop + 36.0f);

        if (layout.hasActiveSession)
        {
            const float headingTop = contentTop + 36.0f;
            layout.chatHeading = D2D1::RectF(276.0f, headingTop, width, headingTop + 72.0f);
            chatFontSize = std::isfinite(chatFontSize) ? (std::clamp)(chatFontSize, 10.0f, 24.0f) : 11.5f;
            const float editHeight = (std::max)(40.0f, 40.0f * chatFontSize / 11.5f);
            const float composerTop = (std::max)(headingTop + 72.0f, contentBottom - editHeight - 66.0f);
            layout.composerWrap = D2D1::RectF(276.0f, composerTop, width, contentBottom);
            const float composerWidth = (std::max)(1.0f, (std::min)(editorWidth - 64.0f, 760.0f));
            const float composerLeft = 276.0f + (editorWidth - composerWidth) * 0.5f;
            layout.composerBox = D2D1::RectF(composerLeft, composerTop + 6.0f, composerLeft + composerWidth, composerTop + editHeight + 52.0f);
            layout.composerEditArea = D2D1::RectF(composerLeft + 14.0f, composerTop + 14.0f, composerLeft + composerWidth - 14.0f, composerTop + editHeight + 14.0f);
            const float toolbarY = composerTop + editHeight + 16.0f;
            layout.composerAttachBtn = D2D1::RectF(composerLeft + 8.0f, toolbarY, composerLeft + 37.0f, toolbarY + 28.0f);
            layout.composerModelBtn = D2D1::RectF(composerLeft + 42.0f, toolbarY, composerLeft + 172.0f, toolbarY + 28.0f);
            layout.composerContextChip = D2D1::RectF(composerLeft + 178.0f, toolbarY, composerLeft + 242.0f, toolbarY + 28.0f);
            layout.composerSendBtn = D2D1::RectF(composerLeft + composerWidth - 37.0f, toolbarY, composerLeft + composerWidth - 8.0f, toolbarY + 28.0f);
            layout.composerMicBtn = D2D1::RectF(composerLeft + composerWidth - 72.0f, toolbarY, composerLeft + composerWidth - 43.0f, toolbarY + 28.0f);
            layout.conversationArea = D2D1::RectF(276.0f, headingTop + 72.0f, width, composerTop);
            layout.conversationInner = D2D1::RectF(composerLeft, headingTop + 72.0f, composerLeft + composerWidth, composerTop);
            layout.chatCopyButton = D2D1::RectF(layout.conversationInner.right - 78.0f, headingTop + 24.0f, layout.conversationInner.right, headingTop + 45.0f);
            layout.chatHasCopyableMessages = std::any_of(tabs[activeTab].messages.begin(), tabs[activeTab].messages.end(), ChatCopy::HasContent);
            layout.scrollbarTrack = D2D1::RectF(width - 10.0f, headingTop + 72.0f, width - 2.0f, composerTop);

        }

        if (activeDropdown != ActiveDropdown::None && !dropdownItems.empty())
        {
            D2D1_RECT_F anchor = layout.menuProject;
            float dropdownWidth = 224.0f;
            bool preferAbove = false;
            if (activeDropdown == ActiveDropdown::Session) anchor = layout.menuSession;
            else if (activeDropdown == ActiveDropdown::SidebarModel) { anchor = layout.sidebarModelSelect; layout.dropdownIsModel = true; }
            else if (activeDropdown == ActiveDropdown::Model) { anchor = layout.composerModelBtn; layout.dropdownIsModel = true; preferAbove = true; }
            if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) return;
            const auto rowHeight = [&](const MenuItem& item) {
                return layout.dropdownIsModel && !item.shortcut.empty() && item.shortcut != item.label ? 46.0f : 30.0f;
            };
            for (const auto& item : dropdownItems)
            {
                layout.dropdownContentHeight += rowHeight(item) + (item.hasDivider ? 6.0f : 0.0f);
                if (layout.dropdownIsModel)
                {
                    // Layout has no text renderer. Give long server IDs room and let
                    // DirectWrite ellipsize the remaining text without wrapping rows.
                    const float labelWidth = static_cast<float>(item.label.size()) * 7.0f + 54.0f;
                    const float nameWidth = static_cast<float>(item.shortcut.size()) * 6.0f + 54.0f;
                    dropdownWidth = (std::max)(dropdownWidth, (std::min)(640.0f, (std::max)(labelWidth, nameWidth)));
                }
            }
            if (layout.dropdownIsModel) dropdownWidth = (std::max)(320.0f, dropdownWidth);
            dropdownWidth = (std::max)(1.0f, (std::min)(dropdownWidth, width - 8.0f));
            const float desiredHeight = layout.dropdownContentHeight + 12.0f;
            const float above = (std::max)(0.0f, anchor.top - 7.0f);
            const float below = (std::max)(0.0f, height - anchor.bottom - 7.0f);
            const float maximumHeight = (std::min)(440.0f, (std::max)(1.0f, height - 8.0f));
            const float targetHeight = (std::min)(desiredHeight, maximumHeight);
            const bool openAbove = preferAbove ? (above >= targetHeight || above >= below) :
                (below < targetHeight && above > below);
            const float availableHeight = openAbove ? above : below;
            const float dropdownHeight = (std::min)(targetHeight, (std::max)(12.0f, availableHeight));
            const float dropdownX = (std::clamp)(anchor.left, 4.0f, (std::max)(4.0f, width - dropdownWidth - 4.0f));
            float dropdownY = openAbove ? anchor.top - dropdownHeight - 3.0f : anchor.bottom + 3.0f;
            dropdownY = (std::clamp)(dropdownY, 4.0f, (std::max)(4.0f, height - dropdownHeight - 4.0f));
            layout.dropdownRect = D2D1::RectF(dropdownX, dropdownY, dropdownX + dropdownWidth, dropdownY + dropdownHeight);
            layout.dropdownViewport = D2D1::RectF(dropdownX + 5.0f, dropdownY + 6.0f,
                dropdownX + dropdownWidth - 5.0f, dropdownY + dropdownHeight - 6.0f);
            const float viewportHeight = (std::max)(0.0f, layout.dropdownViewport.bottom - layout.dropdownViewport.top);
            layout.dropdownMaxScroll = (std::max)(0.0f, layout.dropdownContentHeight - viewportHeight);
            layout.dropdownScrollOffset = std::isfinite(dropdownScrollOffset) ?
                (std::clamp)(dropdownScrollOffset, 0.0f, layout.dropdownMaxScroll) : 0.0f;
            float itemY = layout.dropdownViewport.top - layout.dropdownScrollOffset;
            const float rowRight = layout.dropdownViewport.right - (layout.dropdownMaxScroll > 0.0f ? 9.0f : 0.0f);
            for (const auto& item : dropdownItems)
            {
                if (item.hasDivider) itemY += 6.0f;
                const float itemHeight = rowHeight(item);
                layout.dropdownItems.push_back(D2D1::RectF(layout.dropdownViewport.left, itemY, rowRight, itemY + itemHeight - 2.0f));
                itemY += itemHeight;
            }
            if (layout.dropdownMaxScroll > 0.0f && viewportHeight > 0.0f)
            {
                layout.dropdownScrollTrack = D2D1::RectF(layout.dropdownViewport.right - 5.0f, layout.dropdownViewport.top,
                    layout.dropdownViewport.right, layout.dropdownViewport.bottom);
                const float thumbHeight = (std::min)(viewportHeight,
                    (std::max)(22.0f, viewportHeight * viewportHeight / layout.dropdownContentHeight));
                const float thumbY = layout.dropdownViewport.top + layout.dropdownScrollOffset / layout.dropdownMaxScroll *
                    (viewportHeight - thumbHeight);
                layout.dropdownScrollThumb = D2D1::RectF(layout.dropdownScrollTrack.left, thumbY,
                    layout.dropdownScrollTrack.right, thumbY + thumbHeight);
            }
        }
    }

    float UIComponents::EnsureDropdownItemVisible(const LayoutMetrics& layout, int index)
    {
        if (index < 0 || index >= static_cast<int>(layout.dropdownItems.size())) return layout.dropdownScrollOffset;
        const auto& row = layout.dropdownItems[index];
        float offset = layout.dropdownScrollOffset;
        if (row.top < layout.dropdownViewport.top) offset += row.top - layout.dropdownViewport.top;
        else if (row.bottom > layout.dropdownViewport.bottom) offset += row.bottom - layout.dropdownViewport.bottom;
        return (std::clamp)(offset, 0.0f, layout.dropdownMaxScroll);
    }
    HitTestResult UIComponents::HitTest(const LayoutMetrics& layout, float x, float y,
        SidebarMode sidebarMode, ActiveDropdown activeDropdown,
        const std::vector<MenuItem>& dropdownItems)
    {
        HitTestResult res;

        // 1. Check open dropdown first
        if (activeDropdown != ActiveDropdown::None)
        {
            if (PointInRect(layout.dropdownRect, x, y))
            {
                if (PointInRect(layout.dropdownScrollThumb, x, y))
                    return {HitTargetType::DropdownScrollbarThumb, -1, layout.dropdownScrollThumb};
                if (PointInRect(layout.dropdownScrollTrack, x, y))
                    return {HitTargetType::DropdownScrollbarTrack, -1, layout.dropdownScrollTrack};
                if (!PointInRect(layout.dropdownViewport, x, y)) return res;
                for (size_t i = 0; i < layout.dropdownItems.size(); ++i)
                {
                    if (PointInRect(layout.dropdownItems[i], x, y))
                    {
                        if (i >= dropdownItems.size() || !dropdownItems[i].enabled) return res;
                        res.type = HitTargetType::DropdownItem;
                        res.index = static_cast<int>(i);
                        res.rect = layout.dropdownItems[i];
                        return res;
                    }
                }
            }
        }

        // The padding/border of a popup owns its pixels too; never click through it.
        if (activeDropdown != ActiveDropdown::None && PointInRect(layout.dropdownRect, x, y)) return res;

        // 2. Titlebar buttons
        if (PointInRect(layout.titlebar, x, y))
        {
            if (x >= 8.0f && x < 25.0f && y >= 7.0f && y < 30.0f)
            {
                res.type = HitTargetType::TrafficClose;
                return res;
            }
            if (x >= 26.0f && x < 43.0f && y >= 7.0f && y < 30.0f)
            {
                res.type = HitTargetType::TrafficMinimize;
                return res;
            }
            if (x >= 44.0f && x < 61.0f && y >= 7.0f && y < 30.0f)
            {
                res.type = HitTargetType::TrafficMaximize;
                return res;
            }
            if (PointInRect(layout.menuProject, x, y))
            {
                res.type = HitTargetType::MenuProject;
                res.rect = layout.menuProject;
                return res;
            }
            if (PointInRect(layout.menuSession, x, y))
            {
                res.type = HitTargetType::MenuSession;
                res.rect = layout.menuSession;
                return res;
            }
            return res;
        }

        // 3. Activity Bar
        if (PointInRect(layout.activityBar, x, y))
        {
            if (PointInRect(layout.actBtnFiles, x, y)) { res.type = HitTargetType::ActivityFiles; return res; }
            if (PointInRect(layout.actBtnSession, x, y)) { res.type = HitTargetType::ActivitySession; return res; }
            if (PointInRect(layout.actBtnPlugins, x, y)) { res.type = HitTargetType::ActivityPlugins; return res; }
            if (PointInRect(layout.actBtnSettings, x, y)) { res.type = HitTargetType::ActivitySettings; return res; }
            return res;
        }

        // 4. Sidebar
        if (PointInRect(layout.sidebar, x, y))
        {
            if (!PointInRect(layout.sidebarViewport, x, y)) return res;
            if (PointInRect(layout.sidebarScrollbarThumb, x, y)) { res.type = HitTargetType::SidebarScrollbarThumb; res.rect = layout.sidebarScrollbarThumb; return res; }
            if (PointInRect(layout.sidebarScrollbarTrack, x, y)) { res.type = HitTargetType::SidebarScrollbarTrack; res.rect = layout.sidebarScrollbarTrack; return res; }
            if (sidebarMode == SidebarMode::Files)
            {
                if (PointInRect(layout.sidebarProjectRow, x, y))
                {
                    res.type = HitTargetType::SidebarProjectRow;
                    return res;
                }
                for (size_t i = 0; i < layout.sidebarFileRows.size(); ++i)
                {
                    if (PointInRect(layout.sidebarFileRows[i], x, y))
                    {
                        res.type = HitTargetType::SidebarFileRow;
                        res.index = static_cast<int>(i);
                        return res;
                    }
                }
            }
            else if (sidebarMode == SidebarMode::Session)
            {
                if (PointInRect(layout.sidebarModelSelect, x, y))
                {
                    res.type = HitTargetType::SidebarModelSelect;
                    res.rect = layout.sidebarModelSelect;
                    return res;
                }
                for (size_t i = 0; i < layout.sidebarAgentMoveUpBtns.size(); ++i)
                {
                    if (PointInRect(layout.sidebarAgentMoveUpBtns[i], x, y))
                        return {HitTargetType::SidebarAgentMoveUp, static_cast<int>(i), layout.sidebarAgentMoveUpBtns[i]};
                }
                for (size_t i = 0; i < layout.sidebarAgentToggleBtns.size(); ++i)
                    if (PointInRect(layout.sidebarAgentToggleBtns[i], x, y))
                        return {HitTargetType::SidebarAgentItem, static_cast<int>(i), layout.sidebarAgentToggleBtns[i]};
                for (size_t i = 0; i < layout.sidebarAgentMoveDownBtns.size(); ++i)
                {
                    if (PointInRect(layout.sidebarAgentMoveDownBtns[i], x, y))
                        return {HitTargetType::SidebarAgentMoveDown, static_cast<int>(i), layout.sidebarAgentMoveDownBtns[i]};
                }
                for (size_t i = 0; i < layout.sidebarSkillRows.size(); ++i)
                {
                    if (PointInRect(layout.sidebarSkillRows[i], x, y))
                    {
                        res.type = HitTargetType::SidebarSkillItem;
                        res.index = static_cast<int>(i);
                        res.rect = layout.sidebarSkillRows[i];
                        return res;
                    }
                }
                for (size_t i = 0; i < layout.sidebarMcpServerRows.size(); ++i)
                {
                    if (PointInRect(layout.sidebarMcpServerRows[i], x, y))
                        return {HitTargetType::SidebarMcpServerItem, static_cast<int>(i), layout.sidebarMcpServerRows[i]};
                }
            }
            else if (sidebarMode == SidebarMode::Plugins)
            {
                for (size_t i = 0; i < layout.sidebarPluginCards.size(); ++i)
                {
                    if (PointInRect(layout.sidebarPluginCards[i], x, y))
                    {
                        res.type = HitTargetType::SidebarPluginCard;
                        res.index = static_cast<int>(i);
                        return res;
                    }
                }
                if (PointInRect(layout.sidebarBrowsePluginsBtn, x, y))
                {
                    res.type = HitTargetType::SidebarBrowsePlugins;
                    return res;
                }
            }
            return res;
        }

        // 5. Tabs
        if (PointInRect(layout.tabsBar, x, y))
        {
            if (PointInRect(layout.tabNewBtn, x, y)) { res.type = HitTargetType::TabNew; res.rect = layout.tabNewBtn; return res; }
            if (PointInRect(layout.tabPreviousBtn, x, y)) { res.type = HitTargetType::TabPrevious; res.rect = layout.tabPreviousBtn; return res; }
            if (PointInRect(layout.tabNextBtn, x, y)) { res.type = HitTargetType::TabNext; res.rect = layout.tabNextBtn; return res; }
            for (size_t i = 0; i < layout.tabCloses.size(); ++i)
            {
                if (PointInRect(layout.tabCloses[i], x, y))
                {
                    res.type = HitTargetType::TabClose;
                    res.index = static_cast<int>(i);
                    return res;
                }
            }
            for (size_t i = 0; i < layout.tabs.size(); ++i)
            {
                if (PointInRect(layout.tabs[i], x, y))
                {
                    res.type = HitTargetType::TabItem;
                    res.index = static_cast<int>(i);
                    return res;
                }
            }
            if (PointInRect(layout.tabNewBtn, x, y))
            {
                res.type = HitTargetType::TabNew;
                return res;
            }
            return res;
        }

        if (PointInRect(layout.scrollbarThumb, x, y)) { res.type = HitTargetType::ScrollbarThumb; res.rect = layout.scrollbarThumb; return res; }
        if (layout.conversationMaxScroll > 0.0f && PointInRect(layout.scrollbarTrack, x, y)) { res.type = HitTargetType::ScrollbarTrack; res.rect = layout.scrollbarTrack; return res; }

        if (layout.chatHasCopyableMessages && PointInRect(layout.chatCopyButton, x, y))
            return {HitTargetType::CopyChat, -1, layout.chatCopyButton};
        if (PointInRect(layout.conversationArea, x, y))
            for (size_t i = 0; i < layout.messageCopyButtons.size(); ++i)
                if (PointInRect(layout.messageCopyButtons[i], x, y))
                    return {HitTargetType::CopyMessage, static_cast<int>(i), layout.messageCopyButtons[i]};

        // 6. Composer buttons
        if (PointInRect(layout.composerWrap, x, y))
        {
            if (PointInRect(layout.composerEditArea, x, y)) { res.type = HitTargetType::ComposerEdit; res.rect = layout.composerEditArea; return res; }
            if (PointInRect(layout.composerAttachBtn, x, y)) { res.type = HitTargetType::ComposerAttach; return res; }
            if (PointInRect(layout.composerModelBtn, x, y)) { res.type = HitTargetType::ComposerModelSelect; return res; }
            if (PointInRect(layout.composerContextChip, x, y)) { res.type = HitTargetType::ComposerContextChip; return res; }
            if (PointInRect(layout.composerMicBtn, x, y)) { res.type = HitTargetType::ComposerMic; return res; }
            if (PointInRect(layout.composerSendBtn, x, y)) { res.type = HitTargetType::ComposerSend; return res; }
            return res;
        }

        return res;
    }

    void UIComponents::Render(Direct2DContext& ctx, const LayoutMetrics& layout,
        SidebarMode sidebarMode, ActiveDropdown activeDropdown,
        const std::vector<MenuItem>& currentDropdownItems,
        const std::vector<SessionTab>& tabs, int activeTab,
        const std::vector<FileItem>& files, const std::vector<SkillItem>& skills,
        const std::vector<PluginItem>& plugins, const std::wstring& selectedModel,
        bool /*planBeforeEdits*/, bool /*autoRunSafeTools*/, bool isListening,
        const std::wstring& draftText, HitTestResult hoveredTarget, float animTick, bool isGeneratingReply,
        const std::wstring& currentProjectName, const std::wstring& connectionStatus, bool connected,
        const std::wstring& requestError, const std::vector<AgentItem>& agents,
        const std::vector<McpServerItem>& mcpServers, const std::wstring& resourcePreferencesError,
        const std::wstring& copyFeedback, bool copyFeedbackError)
    {
        // 1. Background fill
        ctx.FillRect(D2D1::RectF(0, 0, layout.windowWidth, layout.windowHeight), Colors::ShellBg);

        // 2. Titlebar
        std::wstring projectName = (activeTab >= 0 && activeTab < static_cast<int>(tabs.size())) ?
            tabs[activeTab].projectName : std::wstring{};
        if (!currentProjectName.empty()) projectName = currentProjectName;
        RenderTitlebar(ctx, layout, activeDropdown, projectName, hoveredTarget);

        // 3. Activity Bar
        RenderActivityBar(ctx, layout, sidebarMode, hoveredTarget);

        // 4. Sidebar
        int attachedFilesCount = (activeTab >= 0 && activeTab < static_cast<int>(tabs.size())) ?
            tabs[activeTab].filesCount : 0;
        RenderSidebar(ctx, layout, sidebarMode, files, skills, plugins, selectedModel,
            attachedFilesCount, projectName, hoveredTarget, agents, mcpServers, resourcePreferencesError);

        // 5. Tabs
        RenderTabsBar(ctx, layout, tabs, activeTab, hoveredTarget);

        // 6. Active Tab Content
        if (layout.hasActiveSession && activeTab >= 0 && activeTab < static_cast<int>(tabs.size()))
        {
            const auto& curSession = tabs[activeTab];
            RenderChatHeading(ctx, layout, curSession, connected, requestError, hoveredTarget);
            float totalH = 0.0f;
            RenderConversation(ctx, layout, curSession, totalH, hoveredTarget);
        }

        // 7. Composer
        if (layout.hasActiveSession)
            RenderComposer(ctx, layout, selectedModel, attachedFilesCount, isListening,
                draftText, hoveredTarget, animTick, isGeneratingReply);

        // 8. Statusbar
        RenderStatusbar(ctx, layout, animTick, connectionStatus, connected, copyFeedback, copyFeedbackError);

        // 9. Dropdown overlay if open
        if (activeDropdown != ActiveDropdown::None)
        {
            RenderDropdown(ctx, layout, currentDropdownItems, hoveredTarget);
        }
    }

    void UIComponents::RenderTitlebar(Direct2DContext& ctx, const LayoutMetrics& layout,
        ActiveDropdown activeDropdown, const std::wstring& projectName, HitTestResult hoveredTarget)
    {
        ctx.FillRect(layout.titlebar, Colors::TitlebarBg);
        ctx.DrawLine(D2D1::Point2F(layout.titlebar.left, layout.titlebar.bottom),
            D2D1::Point2F(layout.titlebar.right, layout.titlebar.bottom), Colors::TitlebarBorder, 1.0f);

        // Traffic lights
        ctx.FillCircle(16.0f, 18.0f, 5.5f, Colors::TrafficRed);
        ctx.FillCircle(34.0f, 18.0f, 5.5f, Colors::TrafficYellow);
        ctx.FillCircle(52.0f, 18.0f, 5.5f, Colors::TrafficGreen);

        // Project menu button
        bool projectActive = (activeDropdown == ActiveDropdown::Project) ||
            (hoveredTarget.type == HitTargetType::MenuProject);
        if (projectActive)
        {
            ctx.FillRoundedRect(layout.menuProject, 4.0f, Colors::ButtonHover);
        }
        ctx.DrawTextW(L"Project", ctx.FontTitle(),
            D2D1::RectF(layout.menuProject.left, layout.menuProject.top + 3.0f, layout.menuProject.right, layout.menuProject.bottom),
            projectActive ? Colors::White : Colors::TextMain, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Session menu button
        bool sessionActive = (activeDropdown == ActiveDropdown::Session) ||
            (hoveredTarget.type == HitTargetType::MenuSession);
        if (sessionActive)
        {
            ctx.FillRoundedRect(layout.menuSession, 4.0f, Colors::ButtonHover);
        }
        ctx.DrawTextW(L"Session", ctx.FontTitle(),
            D2D1::RectF(layout.menuSession.left, layout.menuSession.top + 3.0f, layout.menuSession.right, layout.menuSession.bottom),
            sessionActive ? Colors::White : Colors::TextMain, DWRITE_TEXT_ALIGNMENT_CENTER);

        if (layout.windowWidth >= 900.0f)
        {
        // Centered application and project identity.
        float titleY = layout.windowTitle.top + 3.0f;
        std::wstring fullTitle = AppIdentity::Name;
        if (!projectName.empty()) fullTitle += L" \x2014 " + projectName;
        const float titleWidth = (std::min)(layout.windowWidth - 430.0f,
            27.0f + ctx.MeasureTextWidth(fullTitle, ctx.FontBody()));
        const float titleLeft = (layout.windowWidth - titleWidth) * 0.5f;
        D2D1_RECT_F markRect = D2D1::RectF(titleLeft, titleY, titleLeft + 20.0f, titleY + 20.0f);
        ctx.FillRoundedRect(markRect, 5.0f, Colors::AppMarkBg);
        ctx.DrawIcon(IconType::Sparkles, D2D1::RectF(markRect.left + 3.0f, markRect.top + 3.0f, markRect.right - 3.0f, markRect.bottom - 3.0f), Colors::White, 1.4f);

        D2D1_RECT_F textRect = D2D1::RectF(markRect.right + 7.0f, titleY + 2.0f, titleLeft + titleWidth + 1.0f, titleY + 20.0f);
        ctx.DrawTextSingleLine(fullTitle, ctx.FontBody(), textRect, Colors::TextSub);

        }

    }

    void UIComponents::RenderActivityBar(Direct2DContext& ctx, const LayoutMetrics& layout,
        SidebarMode sidebarMode, HitTestResult hoveredTarget)
    {
        ctx.FillRect(layout.activityBar, Colors::ActivityBarBg);
        ctx.DrawLine(D2D1::Point2F(layout.activityBar.right, layout.activityBar.top),
            D2D1::Point2F(layout.activityBar.right, layout.activityBar.bottom), Colors::ActivityBarBorder, 1.0f);

        auto DrawActButton = [&](const D2D1_RECT_F& r, IconType icon, bool active, bool hover) {
            D2D1_COLOR_F clr = active ? Colors::ActivityIconActive : (hover ? Colors::ActivityIconHover : Colors::ActivityIcon);
            if (active)
            {
                ctx.FillRoundedRect(D2D1::RectF(r.left, r.top + 9.0f, r.left + 2.5f, r.top + 33.0f), 1.0f, Colors::ActivityActivePill);
            }
            D2D1_RECT_F iconRect = D2D1::RectF(r.left + 12.0f, r.top + 10.0f, r.right - 12.0f, r.bottom - 10.0f);
            ctx.DrawIcon(icon, iconRect, clr, 1.7f);
        };

        DrawActButton(layout.actBtnFiles, IconType::File, sidebarMode == SidebarMode::Files, hoveredTarget.type == HitTargetType::ActivityFiles);
        DrawActButton(layout.actBtnSession, IconType::Agent, sidebarMode == SidebarMode::Session, hoveredTarget.type == HitTargetType::ActivitySession);
        DrawActButton(layout.actBtnPlugins, IconType::Plugin, sidebarMode == SidebarMode::Plugins, hoveredTarget.type == HitTargetType::ActivityPlugins);

        DrawActButton(layout.actBtnSettings, IconType::Settings, false, hoveredTarget.type == HitTargetType::ActivitySettings);
    }

    void UIComponents::RenderSidebar(Direct2DContext& ctx, const LayoutMetrics& layout,
        SidebarMode sidebarMode, const std::vector<FileItem>& files,
        const std::vector<SkillItem>& skills, const std::vector<PluginItem>& plugins,
        const std::wstring& selectedModel,
        int attachedFilesCount, const std::wstring& projectName, HitTestResult hoveredTarget,
        const std::vector<AgentItem>& agents, const std::vector<McpServerItem>& mcpServers,
        const std::wstring& resourcePreferencesError)
    {
        ctx.FillRect(layout.sidebar, Colors::SidebarBg);
        ctx.DrawLine(D2D1::Point2F(layout.sidebar.right, layout.sidebar.top),
            D2D1::Point2F(layout.sidebar.right, layout.sidebar.bottom), Colors::SidebarBorder, 1.0f);

        // Heading
        std::wstring headingText = L"EXPLORER";
        if (sidebarMode == SidebarMode::Session) headingText = L"SESSION";
        else if (sidebarMode == SidebarMode::Plugins) headingText = L"PLUGINS";

        ctx.DrawTextW(headingText, ctx.FontSmallBold(),
            D2D1::RectF(layout.sidebarHeading.left + 14.0f, layout.sidebarHeading.top + 14.0f, layout.sidebarHeading.right - 35.0f, layout.sidebarHeading.bottom),
            Colors::TextMuted);

        D2D1_RECT_F moreRect = D2D1::RectF(layout.sidebarHeading.right - 32.0f, layout.sidebarHeading.top + 8.0f, layout.sidebarHeading.right - 5.0f, layout.sidebarHeading.top + 35.0f);
        ctx.DrawIcon(IconType::More, D2D1::RectF(moreRect.left + 5.0f, moreRect.top + 5.0f, moreRect.right - 5.0f, moreRect.bottom - 5.0f), Colors::TextMuted, 1.5f);

        ctx.PushClip(layout.sidebarViewport);
        if (sidebarMode == SidebarMode::Files)
        {
            if (layout.hasProject)
            {
                // Real project root
                ctx.DrawLine(D2D1::Point2F(layout.sidebarProjectRow.left, layout.sidebarProjectRow.top),
                    D2D1::Point2F(layout.sidebarProjectRow.right, layout.sidebarProjectRow.top), Colors::ActivityBarBorder, 1.0f);
                ctx.DrawLine(D2D1::Point2F(layout.sidebarProjectRow.left, layout.sidebarProjectRow.bottom),
                    D2D1::Point2F(layout.sidebarProjectRow.right, layout.sidebarProjectRow.bottom), Colors::ActivityBarBorder, 1.0f);
    
                ctx.DrawTextW(layout.projectExpanded ? L"\u2304" : L"\u203a", ctx.FontSmall(),
                    D2D1::RectF(layout.sidebarProjectRow.left + 8.0f, layout.sidebarProjectRow.top + 7.0f, layout.sidebarProjectRow.left + 22.0f, layout.sidebarProjectRow.bottom),
                    Colors::TextMuted);
                ctx.DrawTextSingleLine(projectName, ctx.FontSmallBold(),
                    D2D1::RectF(layout.sidebarProjectRow.left + 24.0f, layout.sidebarProjectRow.top + 7.0f, layout.sidebarProjectRow.right, layout.sidebarProjectRow.bottom),
                    Colors::TextMain);

            }

            // Files Tree
            for (size_t i = 0; i < files.size() && i < layout.sidebarFileRows.size(); ++i)
            {
                const auto& f = files[i];
                const auto& r = layout.sidebarFileRows[i];
                if (r.right <= r.left || r.bottom <= r.top) continue;

                if (hoveredTarget.type == HitTargetType::SidebarFileRow && hoveredTarget.index == static_cast<int>(i))
                {
                    ctx.FillRect(r, Colors::HoverBg);
                }

                float leftX = r.left + 12.0f + f.indent * 15.0f;
                if (f.isFolder)
                {
                    ctx.DrawTextW(f.isExpanded ? L"\u2304" : L"\u203a", ctx.FontSmall(), D2D1::RectF(leftX, r.top + 4.0f, leftX + 12.0f, r.bottom), Colors::TextMuted);
                    ctx.DrawIcon(IconType::Folder, D2D1::RectF(leftX + 13.0f, r.top + 5.0f, leftX + 28.0f, r.bottom - 5.0f), Colors::TextMuted, 1.5f);
                    ctx.DrawTextSingleLine(f.name, ctx.FontSmall(), D2D1::RectF(leftX + 32.0f, r.top + 5.0f, r.right, r.bottom), Colors::TextMain);
                }
                else
                {
                    D2D1_COLOR_F symColor = Colors::FileTs;
                    std::wstring sym;
                    if (f.type == L"ts") sym = L"TS";
                    else if (f.type == L"react") { sym = L"\u269b"; symColor = Colors::FileReact; }
                    else if (f.type == L"md") { sym = L"M\u2193"; symColor = Colors::FileMd; }
                    else if (f.type == L"env") { sym = L"\u25c7"; symColor = Colors::FileEnv; }
                    else if (f.type == L"cpp" || f.type == L"h") sym = L"C++";
                    if (sym.empty()) ctx.DrawIcon(IconType::File, D2D1::RectF(leftX + 12.0f, r.top + 5.0f, leftX + 26.0f, r.bottom - 5.0f), Colors::TextMuted, 1.2f);
                    else ctx.DrawTextW(sym, ctx.FontTinyBold(), D2D1::RectF(leftX + 10.0f, r.top + 5.0f, leftX + 28.0f, r.bottom), symColor);
                    ctx.DrawTextSingleLine(f.name, ctx.FontSmall(), D2D1::RectF(leftX + 32.0f, r.top + 5.0f, r.right, r.bottom), Colors::TextMain);
                }
            }

            // Open Editors
            const auto& oeRect = layout.sidebarOpenEditorsRow;
            ctx.DrawLine(D2D1::Point2F(oeRect.left, oeRect.top), D2D1::Point2F(oeRect.right, oeRect.top), Colors::SidebarBorder, 1.0f);
            ctx.DrawLine(D2D1::Point2F(oeRect.left, oeRect.bottom), D2D1::Point2F(oeRect.right, oeRect.bottom), Colors::SidebarBorder, 1.0f);
            ctx.DrawTextW(L"OPEN EDITORS", ctx.FontTinyBold(), D2D1::RectF(oeRect.left + 14.0f, oeRect.top + 8.0f, oeRect.right - 40.0f, oeRect.bottom), Colors::TextMuted);

            ctx.FillCircle(oeRect.right - 22.0f, oeRect.top + 15.5f, 8.0f, Colors::TokenBarBg);
            ctx.DrawTextW(std::to_wstring(layout.openFileEditorsCount), ctx.FontTinyBold(), D2D1::RectF(oeRect.right - 30.0f, oeRect.top + 8.0f, oeRect.right - 14.0f, oeRect.bottom), Colors::TextSub, DWRITE_TEXT_ALIGNMENT_CENTER);

            if (layout.sidebarContextCard.right > layout.sidebarContextCard.left)
            {
                // Context Card
                const auto& cardRect = layout.sidebarContextCard;
                ctx.FillRoundedRect(cardRect, 6.0f, Colors::ComposerBg);
                ctx.DrawRoundedRect(cardRect, 6.0f, Colors::PlanCardBorder, 1.0f);
    
                ctx.DrawIcon(IconType::Sparkles, D2D1::RectF(cardRect.left + 10.0f, cardRect.top + 10.0f, cardRect.left + 24.0f, cardRect.top + 24.0f), Colors::Eyebrow, 1.4f);
                ctx.DrawTextW(L"CONTEXT", ctx.FontTinyBold(), D2D1::RectF(cardRect.left + 28.0f, cardRect.top + 10.0f, cardRect.right, cardRect.top + 24.0f), Colors::Eyebrow);
    
                std::wstring attStr = std::to_wstring(attachedFilesCount) + L" files attached";
                ctx.DrawTextW(attStr, ctx.FontSmallBold(), D2D1::RectF(cardRect.left + 10.0f, cardRect.top + 30.0f, cardRect.right, cardRect.top + 46.0f), Colors::TextMain);
                ctx.DrawTextW(L"Local session context", ctx.FontTiny(), D2D1::RectF(cardRect.left + 10.0f, cardRect.top + 48.0f, cardRect.right, cardRect.top + 64.0f), Colors::TextMuted);
    
                D2D1_RECT_F barBg = D2D1::RectF(cardRect.left + 10.0f, cardRect.top + 70.0f, cardRect.right - 10.0f, cardRect.top + 74.0f);
                ctx.FillRoundedRect(barBg, 2.0f, Colors::TokenBarBg);
                D2D1_RECT_F barFill = D2D1::RectF(barBg.left, barBg.top, barBg.left + (barBg.right - barBg.left) * (attachedFilesCount > 0 ? 1.0f : 0.0f), barBg.bottom);
                ctx.FillRoundedRect(barFill, 2.0f, Colors::TokenBarFill);

            }
        }
        else if (sidebarMode == SidebarMode::Session)
        {
            const float modelY = layout.sidebarModelSelect.top - 20.0f;
            ctx.DrawTextW(L"MODEL", ctx.FontTinyBold(), D2D1::RectF(58.0f, modelY, 264.0f, modelY + 16.0f), Colors::TextMuted);
            ctx.FillRoundedRect(layout.sidebarModelSelect, 5.0f, hoveredTarget.type == HitTargetType::SidebarModelSelect ? Colors::ButtonHover : Colors::SelectBg);
            ctx.DrawRoundedRect(layout.sidebarModelSelect, 5.0f, Colors::SelectBorder, 1.0f);
            ctx.DrawTextSingleLine(selectedModel.empty() ? L"Select model" : selectedModel, ctx.FontSmall(),
                D2D1::RectF(layout.sidebarModelSelect.left + 10.0f, layout.sidebarModelSelect.top + 7.0f, layout.sidebarModelSelect.right - 25.0f, layout.sidebarModelSelect.bottom), Colors::TextMain);
            ctx.DrawTextW(L"\u2304", ctx.FontSmall(), D2D1::RectF(layout.sidebarModelSelect.right - 22.0f, layout.sidebarModelSelect.top + 7.0f, layout.sidebarModelSelect.right - 5.0f, layout.sidebarModelSelect.bottom), Colors::TextMuted);
            ctx.DrawTextSingleLine(resourcePreferencesError.empty() ? (layout.hasActiveSession ? L"Used for this session" : L"Default for new sessions") : resourcePreferencesError, ctx.FontTiny(),
                D2D1::RectF(58.0f, layout.sidebarModelSelect.bottom + 6.0f, 264.0f, layout.sidebarModelSelect.bottom + 22.0f), resourcePreferencesError.empty() ? Colors::TextMuted : Colors::TrafficRed);

            const auto drawSection = [&](const D2D1_RECT_F& heading, const std::wstring& title, size_t count, const std::wstring& emptyText) {
                ctx.DrawLine(D2D1::Point2F(layout.sidebar.left, heading.top - 8.0f), D2D1::Point2F(layout.sidebar.right, heading.top - 8.0f), Colors::SidebarBorder);
                ctx.DrawTextW(title, ctx.FontTinyBold(), D2D1::RectF(heading.left, heading.top, heading.right - 30.0f, heading.bottom), Colors::TextMuted);
                ctx.DrawTextW(std::to_wstring(count), ctx.FontTiny(), D2D1::RectF(heading.right - 28.0f, heading.top, heading.right, heading.bottom), Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                if (!count) ctx.DrawTextW(emptyText, ctx.FontSmall(), D2D1::RectF(heading.left, heading.bottom + 8.0f, heading.right, heading.bottom + 28.0f), Colors::TextDark);
            };
            const auto drawMoveButton = [&](const D2D1_RECT_F& rect, bool up, bool enabled, int index) {
                const auto target = up ? HitTargetType::SidebarAgentMoveUp : HitTargetType::SidebarAgentMoveDown;
                const bool hovered = enabled && hoveredTarget.type == target && hoveredTarget.index == index;
                if (hovered) ctx.FillRoundedRect(rect, 4.0f, Colors::ButtonHover);
                const auto color = enabled ? Colors::TextMuted : Colors::TextDark;
                const float midX = (rect.left + rect.right) * 0.5f;
                const float midY = (rect.top + rect.bottom) * 0.5f;
                const float tipY = midY + (up ? -3.0f : 3.0f), tailY = midY + (up ? 2.0f : -2.0f);
                ctx.DrawLine(D2D1::Point2F(midX - 4.0f, tailY), D2D1::Point2F(midX, tipY), color, 1.5f);
                ctx.DrawLine(D2D1::Point2F(midX, tipY), D2D1::Point2F(midX + 4.0f, tailY), color, 1.5f);
            };
            const auto drawFolderToggle = [&](const D2D1_RECT_F& row, const std::wstring& name, const std::wstring& description, bool enabled, HitTargetType type, int index, D2D1_COLOR_F detailColor) {
                if (row.bottom <= layout.sidebarViewport.top || row.top >= layout.sidebarViewport.bottom) return;
                if (hoveredTarget.type == type && hoveredTarget.index == index) ctx.FillRoundedRect(row, 4.0f, Colors::HoverBg);
                ctx.DrawIcon(IconType::Folder, D2D1::RectF(row.left + 2.0f, row.top + 5.0f, row.left + 17.0f, row.top + 18.0f), Colors::TextMuted, 1.2f);
                ctx.DrawTextSingleLine(name, ctx.FontSmallBold(), D2D1::RectF(row.left + 24.0f, row.top + 3.0f, row.right - 36.0f, row.top + 19.0f), Colors::TextMain);
                ctx.DrawTextSingleLine(description, ctx.FontTiny(), D2D1::RectF(row.left + 24.0f, row.top + 20.0f, row.right - 36.0f, row.bottom), detailColor);
                const auto toggle = D2D1::RectF(row.right - 29.0f, row.top + 12.5f, row.right - 1.0f, row.top + 27.5f);
                ctx.FillRoundedRect(toggle, 7.5f, enabled ? Colors::ToggleBgOn : Colors::ToggleBgOff);
                ctx.FillCircle(enabled ? toggle.right - 7.5f : toggle.left + 7.5f, (toggle.top + toggle.bottom) * 0.5f, 5.5f, enabled ? Colors::White : Colors::ToggleThumb);
            };

            drawSection(layout.sidebarAgentsHeading, L"AGENTS", agents.size(), L"No agents found");
            for (size_t i = 0; i < agents.size() && i < layout.sidebarAgentRows.size(); ++i)
            {
                const auto& row = layout.sidebarAgentRows[i];
                if (row.bottom <= layout.sidebarViewport.top || row.top >= layout.sidebarViewport.bottom) continue;
                const auto badge = D2D1::RectF(row.left + 1.0f, row.top + 23.0f, row.left + 23.0f, row.top + 45.0f);
                ctx.FillRoundedRect(badge, 5.0f, Colors::PlanIconBg);
                ctx.DrawTextW(std::to_wstring(i + 1), ctx.FontTinyBold(), D2D1::RectF(badge.left, badge.top + 4.0f, badge.right, badge.bottom), Colors::PlanIconText, DWRITE_TEXT_ALIGNMENT_CENTER);
                ctx.DrawTextSingleLine(agents[i].name, ctx.FontSmallBold(), D2D1::RectF(row.left + 1.0f, row.top + 3.0f, row.right - 1.0f, row.top + 20.0f), Colors::TextMain);
                ctx.DrawTextSingleLine(L"Priority " + std::to_wstring(i + 1), ctx.FontTiny(), D2D1::RectF(row.left + 30.0f, row.top + 27.0f, 176.0f, row.bottom), Colors::TextMuted);
                drawMoveButton(D2D1::RectF(182.0f, row.top + 23.0f, 204.0f, row.top + 45.0f), true, i > 0, static_cast<int>(i));
                drawMoveButton(D2D1::RectF(206.0f, row.top + 23.0f, 228.0f, row.top + 45.0f), false, i + 1 < agents.size(), static_cast<int>(i));
                const auto toggleHit = layout.sidebarAgentToggleBtns[i];
                if (hoveredTarget.type == HitTargetType::SidebarAgentItem && hoveredTarget.index == static_cast<int>(i)) ctx.FillRoundedRect(toggleHit, 4.0f, Colors::ButtonHover);
                const auto toggle = D2D1::RectF(toggleHit.left, row.top + 26.5f, toggleHit.right, row.top + 41.5f);
                ctx.FillRoundedRect(toggle, 7.5f, agents[i].isEnabled ? Colors::ToggleBgOn : Colors::ToggleBgOff);
                ctx.FillCircle(agents[i].isEnabled ? toggle.right - 7.5f : toggle.left + 7.5f, row.top + 34.0f, 5.5f, agents[i].isEnabled ? Colors::White : Colors::ToggleThumb);
            }
            drawSection(layout.sidebarSkillsHeading, L"SKILLS", skills.size(), L"No skills found");
            for (size_t i = 0; i < skills.size() && i < layout.sidebarSkillRows.size(); ++i)
                drawFolderToggle(layout.sidebarSkillRows[i], skills[i].name,
                    skills[i].description.empty() ? L"Skill folder" : skills[i].description, skills[i].isEnabled, HitTargetType::SidebarSkillItem, static_cast<int>(i), Colors::TextMuted);
            drawSection(layout.sidebarMcpServersHeading, L"MCP SERVERS", mcpServers.size(), L"No MCP servers found");
            for (size_t i = 0; i < mcpServers.size() && i < layout.sidebarMcpServerRows.size(); ++i)
            {
                const auto& row = layout.sidebarMcpServerRows[i];
                if (row.bottom <= layout.sidebarViewport.top || row.top >= layout.sidebarViewport.bottom) continue;
                const auto& server = mcpServers[i];
                const bool connecting = server.connectionState == McpConnectionState::Connecting;
                const auto detail = !server.connectionError.empty() ? L"Off: " + server.connectionError :
                    (connecting ? L"Connecting..." : (server.connectionState == McpConnectionState::Connected || server.isEnabled ? L"Connected" : L"Disabled"));
                drawFolderToggle(layout.sidebarMcpServerRows[i], server.name, detail,
                    server.isEnabled || connecting, HitTargetType::SidebarMcpServerItem, static_cast<int>(i),
                    server.connectionError.empty() ? Colors::TextMuted : Colors::TrafficRed);
            }
        }
        else if (sidebarMode == SidebarMode::Plugins)
        {
            for (size_t i = 0; i < plugins.size() && i < layout.sidebarPluginCards.size(); ++i)
            {
                const auto& p = plugins[i];
                const auto& r = layout.sidebarPluginCards[i];
                if (hoveredTarget.type == HitTargetType::SidebarPluginCard && hoveredTarget.index == static_cast<int>(i)) ctx.FillRect(r, Colors::HoverBg);

                ctx.DrawLine(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.top), Colors::SidebarBorder, 1.0f);

                D2D1_RECT_F logoR = D2D1::RectF(r.left + 12.0f, r.top + 14.0f, r.left + 45.0f, r.top + 47.0f);
                ctx.FillRoundedRect(logoR, 7.0f, p.isViolet ? Colors::PluginVioletBg : Colors::PluginTealBg);
                ctx.DrawIcon(p.isViolet ? IconType::Sparkles : IconType::Plugin,
                    D2D1::RectF(logoR.left + 6.0f, logoR.top + 6.0f, logoR.right - 6.0f, logoR.bottom - 6.0f),
                    p.isViolet ? Colors::PluginVioletIcon : Colors::PluginTealIcon, 1.6f);

                ctx.DrawTextW(p.name, ctx.FontSmallBold(), D2D1::RectF(logoR.right + 10.0f, r.top + 12.0f, r.right - 35.0f, r.top + 28.0f), Colors::TextMain);
                ctx.DrawTextW(p.description, ctx.FontTiny(), D2D1::RectF(logoR.right + 10.0f, r.top + 28.0f, r.right - 10.0f, r.bottom), Colors::TextMuted);

                ctx.DrawTextW(p.isEnabled ? L"ON" : L"OFF", ctx.FontTinyBold(),
                    D2D1::RectF(r.right - 32.0f, r.top + 12.0f, r.right - 8.0f, r.top + 28.0f),
                    p.isEnabled ? Colors::EnabledGreen : Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }

            // Browse plugins button
            if (hoveredTarget.type == HitTargetType::SidebarBrowsePlugins) ctx.FillRoundedRect(layout.sidebarBrowsePluginsBtn, 5.0f, Colors::HoverBg);
            ctx.DrawRoundedRect(layout.sidebarBrowsePluginsBtn, 5.0f, Colors::PlanCardBorder, 1.0f);
            ctx.DrawTextW(L"Browse plugins", ctx.FontSmall(), layout.sidebarBrowsePluginsBtn, Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        ctx.PopClip();
        if (layout.sidebarMaxScroll > 0.0f) ctx.FillRoundedRect(layout.sidebarScrollbarThumb, 2.5f, Colors::PlanCardBorder);
    }

    void UIComponents::RenderTabsBar(Direct2DContext& ctx, const LayoutMetrics& layout,
        const std::vector<SessionTab>& tabs, int activeTab, HitTestResult hoveredTarget)
    {
        ctx.FillRect(layout.tabsBar, Colors::TabsBarBg);
        ctx.DrawLine(D2D1::Point2F(layout.tabsBar.left, layout.tabsBar.bottom),
            D2D1::Point2F(layout.tabsBar.right, layout.tabsBar.bottom), Colors::SidebarBorder, 1.0f);

        ctx.PushClip(layout.tabsBar);
        for (size_t i = 0; i < tabs.size() && i < layout.tabs.size(); ++i)
        {
            const auto& t = tabs[i];
            const auto& r = layout.tabs[i];
            if (r.right <= r.left) continue;
            bool isActive = (static_cast<int>(i) == activeTab);

            ctx.FillRect(r, isActive ? Colors::TabActiveBg : Colors::TabInactiveBg);
            ctx.DrawLine(D2D1::Point2F(r.right, r.top), D2D1::Point2F(r.right, r.bottom), Colors::TabBorder, 1.0f);

            if (isActive)
            {
                ctx.FillRect(D2D1::RectF(r.left, r.top, r.right, r.top + 2.0f), Colors::TabActiveTopLine);
            }

            // Status dot
            D2D1_COLOR_F dotColor = t.isAmber ? Colors::TabStatusAmber : Colors::TabStatusGreen;
            D2D1_COLOR_F ringColor = t.isAmber ? Colors::StatusDotRingAmber : Colors::StatusDotRingGreen;
            float dotX = r.left + 16.0f;
            float dotY = r.top + 18.0f;
            ctx.DrawCircle(dotX, dotY, 4.5f, ringColor, 1.5f);
            ctx.FillCircle(dotX, dotY, 3.5f, dotColor);

            // Tab title
            D2D1_COLOR_F textColor = isActive ? Colors::TextHeading : Colors::TextMuted;
            ctx.DrawTextSingleLine(t.title, ctx.FontSmall(), D2D1::RectF(dotX + 10.0f, r.top + 10.0f, r.right - 24.0f, r.bottom), textColor);

            // Close button
            const auto& closeR = layout.tabCloses[i];
            bool closeHover = (hoveredTarget.type == HitTargetType::TabClose && hoveredTarget.index == static_cast<int>(i));
            if (isActive || closeHover || (hoveredTarget.type == HitTargetType::TabItem && hoveredTarget.index == static_cast<int>(i)))
            {
                ctx.DrawIcon(IconType::Close, D2D1::RectF(closeR.left + 2.0f, closeR.top + 2.0f, closeR.right - 2.0f, closeR.bottom - 2.0f),
                    closeHover ? Colors::White : Colors::TextMuted, 1.3f);
            }
        }

        // New Tab button '+'
        bool newTabHover = (hoveredTarget.type == HitTargetType::TabNew);
        if (newTabHover)
            ctx.FillRect(layout.tabNewBtn, Colors::HoverBg);
        ctx.DrawTextW(L"+", ctx.FontTitle(), layout.tabNewBtn, Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        const auto drawNavigation = [&](const D2D1_RECT_F& rect, const wchar_t* label, HitTargetType type, bool enabled) {
            if (rect.right <= rect.left) return;
            if (enabled && hoveredTarget.type == type) ctx.FillRect(rect, Colors::ButtonHover);
            ctx.DrawTextW(label, ctx.FontTitle(), D2D1::RectF(rect.left, rect.top + 8.0f, rect.right, rect.bottom), enabled ? Colors::TextSub : Colors::TextDark, DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        drawNavigation(layout.tabPreviousBtn, L"\u2039", HitTargetType::TabPrevious, activeTab > 0);
        drawNavigation(layout.tabNextBtn, L"\u203a", HitTargetType::TabNext, activeTab + 1 < static_cast<int>(tabs.size()));
        ctx.PopClip();
    }

    void UIComponents::RenderChatHeading(Direct2DContext& ctx, const LayoutMetrics& layout,
        const SessionTab& session, bool connected, const std::wstring& requestError, HitTestResult hoveredTarget)
    {
        ctx.FillRect(layout.chatHeading, Colors::ShellBg);
        ctx.DrawLine(D2D1::Point2F(layout.chatHeading.left, layout.chatHeading.bottom),
            D2D1::Point2F(layout.chatHeading.right, layout.chatHeading.bottom), Colors::PlanCardBorder, 1.0f);

        ctx.PushClip(layout.chatHeading);
        float paddingLeft = layout.conversationInner.left;
        float paddingRight = layout.conversationInner.right;

        // Eyebrow
        ctx.DrawTextW(session.eyebrow, ctx.FontEyebrow(),
            D2D1::RectF(paddingLeft, layout.chatHeading.top + 10.0f, paddingLeft + 200.0f, layout.chatHeading.top + 22.0f),
            Colors::Eyebrow);

        // Heading title
        ctx.DrawTextSingleLine(session.title, ctx.FontHeading(),
            D2D1::RectF(paddingLeft, layout.chatHeading.top + 22.0f, paddingRight - 90.0f, layout.chatHeading.top + 48.0f),
            Colors::TextHeading);

        // Subtitle
        std::wstring sub = std::to_wstring(session.filesCount) + L" files in context";
        if (!session.projectName.empty()) sub = L"Working in " + session.projectName + L" \x00b7 " + sub;
        if (!requestError.empty()) sub = requestError;
        ctx.DrawTextSingleLine(sub, ctx.FontSmall(),
            D2D1::RectF(paddingLeft, layout.chatHeading.top + 48.0f, paddingRight, layout.chatHeading.top + 64.0f),
            requestError.empty() ? Colors::TextMuted : Colors::TrafficRed);

        // Session State badge: ● Synced
        float badgeRight = paddingRight;
        float badgeTop = layout.chatHeading.top + 7.0f;
        ctx.FillCircle(badgeRight - 84.0f, badgeTop + 7.0f, 3.0f, connected ? Colors::TabStatusGreen : Colors::TextMuted);
        ctx.DrawTextW(connected ? L"Connected" : L"Local", ctx.FontSmall(),
            D2D1::RectF(badgeRight - 75.0f, badgeTop, badgeRight, badgeTop + 18.0f),
            Colors::TextMuted);
        if (hoveredTarget.type == HitTargetType::CopyChat && layout.chatHasCopyableMessages) ctx.FillRoundedRect(layout.chatCopyButton, 4.0f, Colors::ButtonHover);
        ctx.DrawRoundedRect(layout.chatCopyButton, 4.0f, Colors::PlanCardBorder);
        ctx.DrawTextW(L"Copy chat", ctx.FontTiny(), D2D1::RectF(layout.chatCopyButton.left, layout.chatCopyButton.top + 4.0f,
            layout.chatCopyButton.right, layout.chatCopyButton.bottom), layout.chatHasCopyableMessages ? Colors::TextSub : Colors::TextDark, DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx.PopClip();
    }

    namespace
    {
        struct MessageMeasurements
        {
            float bodyHeight = 0.0f;
            std::vector<float> itemHeights;
            std::vector<float> planTitleHeights;
            std::vector<float> planDescriptionHeights;
            std::vector<float> planRowHeights;
            float planHeight = 0.0f;
            float height = 0.0f;
        };

        MessageMeasurements MeasureMessage(Direct2DContext& ctx, const MessageItem& message, float textWidth)
        {
            MessageMeasurements result;
            textWidth = (std::max)(1.0f, textWidth);
            result.bodyHeight = MeasureMarkdown(ctx, message.text, textWidth);
            result.height = 18.0f + result.bodyHeight + 8.0f;
            for (const auto& item : message.items)
            {
                const float itemHeight = ctx.MeasureTextHeight(item, ctx.FontSmall(), (std::max)(1.0f, textWidth - 28.0f));
                result.itemHeights.push_back(itemHeight);
                result.height += (std::max)(22.0f, itemHeight + 8.0f);
            }
            for (const auto& step : message.planSteps)
            {
                const float planTextWidth = (std::max)(1.0f, textWidth - 50.0f);
                const float titleHeight = ctx.MeasureTextHeight(step.title, ctx.FontSmallBold(), planTextWidth);
                const float descriptionHeight = ctx.MeasureTextHeight(step.description, ctx.FontTiny(), planTextWidth);
                const float rowHeight = (std::max)(52.0f, titleHeight + descriptionHeight + 22.0f);
                result.planTitleHeights.push_back(titleHeight);
                result.planDescriptionHeights.push_back(descriptionHeight);
                result.planRowHeights.push_back(rowHeight);
                result.planHeight += rowHeight;
            }
            if (!message.planSteps.empty()) result.height += result.planHeight + 12.0f;
            result.height += 16.0f;
            return result;
        }
    }

    float UIComponents::MeasureConversationHeight(Direct2DContext& ctx, const LayoutMetrics& layout,
        const SessionTab& session)
    {
        if (!layout.hasActiveSession || session.messages.empty()) return 0.0f;
        float height = 16.0f + 28.0f + 12.0f;
        const float textWidth = layout.conversationInner.right - layout.conversationInner.left - 38.0f;
        for (const auto& message : session.messages) height += MeasureMessage(ctx, message, textWidth).height;
        return height;
    }

    float UIComponents::UpdateConversationScrollMetrics(LayoutMetrics& layout, float contentHeight, float scrollOffset)
    {
        if (!layout.hasActiveSession) contentHeight = 0.0f;
        layout.conversationContentHeight = (std::max)(0.0f, contentHeight);
        const float viewportHeight = (std::max)(0.0f, layout.conversationArea.bottom - layout.conversationArea.top);
        layout.conversationMaxScroll = (std::max)(0.0f, contentHeight - viewportHeight);
        layout.scrollbarThumb = D2D1::RectF();
        if (layout.conversationMaxScroll > 0.0f && viewportHeight > 0.0f)
        {
            const float thumbHeight = (std::min)(viewportHeight, (std::max)(24.0f, viewportHeight * viewportHeight / contentHeight));
            const float scrollRatio = (std::clamp)(scrollOffset, 0.0f, layout.conversationMaxScroll) / layout.conversationMaxScroll;
            const float thumbTop = layout.scrollbarTrack.top + scrollRatio * (viewportHeight - thumbHeight);
            layout.scrollbarThumb = D2D1::RectF(layout.scrollbarTrack.left, thumbTop, layout.scrollbarTrack.right, thumbTop + thumbHeight);
        }
        return layout.conversationMaxScroll;
    }
    void UIComponents::UpdateConversationCopyTargets(Direct2DContext& ctx, LayoutMetrics& layout, const SessionTab& session)
    {
        layout.messageCopyButtons.assign(session.messages.size(), D2D1_RECT_F{});
        if (!layout.hasActiveSession) return;
        float y = layout.conversationInner.top + 44.0f - (std::clamp)(session.scrollOffset, 0.0f, layout.conversationMaxScroll);
        const float width = (std::max)(1.0f, layout.conversationInner.right - layout.conversationInner.left - 38.0f);
        for (size_t i = 0; i < session.messages.size(); ++i)
        {
            if (ChatCopy::HasContent(session.messages[i]))
                layout.messageCopyButtons[i] = D2D1::RectF(layout.conversationInner.right - 46.0f, y - 1.0f, layout.conversationInner.right, y + 16.0f);
            y += MeasureMessage(ctx, session.messages[i], width).height;
        }
    }

    void UIComponents::RenderConversation(Direct2DContext& ctx, const LayoutMetrics& layout,
        const SessionTab& session, float& outTotalContentHeight, HitTestResult hoveredTarget)
    {
        ctx.FillRect(layout.conversationArea, Colors::ShellBg);
        ctx.PushClip(layout.conversationArea);
        outTotalContentHeight = MeasureConversationHeight(ctx, layout, session);
        if (session.messages.empty())
        {
            ctx.PopClip();
            return;
        }
        const float viewportHeight = (std::max)(0.0f, layout.conversationArea.bottom - layout.conversationArea.top);
        const float maxScroll = (std::max)(0.0f, outTotalContentHeight - viewportHeight);
        const float scrollOffset = (std::clamp)(session.scrollOffset, 0.0f, maxScroll);
        float contentY = layout.conversationInner.top + 16.0f - scrollOffset;
        const float innerLeft = layout.conversationInner.left;
        const float innerWidth = layout.conversationInner.right - innerLeft;
        const float textLeft = innerLeft + 38.0f;
        const float textWidth = (std::max)(1.0f, innerWidth - 38.0f);
        ctx.DrawLine(D2D1::Point2F(innerLeft, contentY + 6.0f), D2D1::Point2F(innerLeft + innerWidth * 0.33f, contentY + 6.0f), Colors::DateRuleLine);
        ctx.DrawTextW(L"Conversation history", ctx.FontTiny(),
            D2D1::RectF(innerLeft + innerWidth * 0.33f, contentY, innerLeft + innerWidth * 0.67f, contentY + 16.0f),
            Colors::DateRuleText, DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx.DrawLine(D2D1::Point2F(innerLeft + innerWidth * 0.67f, contentY + 6.0f), D2D1::Point2F(innerLeft + innerWidth, contentY + 6.0f), Colors::DateRuleLine);
        contentY += 28.0f;
        for (size_t messageIndex = 0; messageIndex < session.messages.size(); ++messageIndex)
        {
            const auto& message = session.messages[messageIndex];
            const auto measured = MeasureMessage(ctx, message, textWidth);
            // Keep measuring every row for exact bounds, but skip painting off-screen messages.
            if (contentY + measured.height < layout.conversationArea.top || contentY > layout.conversationArea.bottom)
            {
                contentY += measured.height;
                continue;
            }
            const bool isAssistant = message.role == L"assistant";
            const auto avatar = D2D1::RectF(innerLeft, contentY, innerLeft + 27.0f, contentY + 27.0f);
            ctx.FillRoundedRect(avatar, 7.0f, isAssistant ? Colors::AvatarAssistantStart : Colors::AvatarUser);
            if (isAssistant)
            {
                if (auto brush = ctx.GetAssistantAvatarBrush(avatar)) ctx.GetRenderTarget()->FillRoundedRectangle(D2D1::RoundedRect(avatar, 7.0f, 7.0f), brush);
                ctx.DrawIcon(IconType::Sparkles, D2D1::RectF(avatar.left + 5.0f, avatar.top + 5.0f, avatar.right - 5.0f, avatar.bottom - 5.0f), Colors::White, 1.5f);
            }
            else ctx.DrawTextW(L"You", ctx.FontTinyBold(), D2D1::RectF(avatar.left, avatar.top + 6.0f, avatar.right, avatar.bottom), Colors::White, DWRITE_TEXT_ALIGNMENT_CENTER);
            const float authorWidth = (std::min)(textWidth * 0.5f, ctx.MeasureTextWidth(message.author, ctx.FontBodyBold()));
            ctx.DrawTextW(message.author, ctx.FontBodyBold(), D2D1::RectF(textLeft, contentY, textLeft + authorWidth + 1.0f, contentY + 16.0f), Colors::TextMain);
            ctx.DrawTextW(message.time, ctx.FontTiny(), D2D1::RectF(textLeft + authorWidth + 10.0f, contentY + 2.0f, textLeft + textWidth - 54.0f, contentY + 16.0f), Colors::TextDark);
            if (ChatCopy::HasContent(message))
            {
                const auto copy = D2D1::RectF(textLeft + textWidth - 46.0f, contentY - 1.0f, textLeft + textWidth, contentY + 16.0f);
                if (hoveredTarget.type == HitTargetType::CopyMessage && hoveredTarget.index == static_cast<int>(messageIndex)) ctx.FillRoundedRect(copy, 4.0f, Colors::ButtonHover);
                ctx.DrawTextW(L"Copy", ctx.FontTiny(), D2D1::RectF(copy.left, copy.top + 3.0f, copy.right, copy.bottom), Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            float curY = contentY + 18.0f;
            RenderMarkdown(ctx, message.text, D2D1::RectF(textLeft, curY, textLeft + textWidth, curY + measured.bodyHeight + 5.0f), Colors::TextSub);
            curY += measured.bodyHeight + 8.0f;
            for (size_t i = 0; i < message.items.size(); ++i)
            {
                const auto badge = D2D1::RectF(textLeft + 4.0f, curY + 2.0f, textLeft + 21.0f, curY + 19.0f);
                ctx.FillRoundedRect(badge, 5.0f, Colors::PlanIconBg);
                ctx.DrawTextW(std::to_wstring(i + 1), ctx.FontTinyBold(), badge, Colors::PlanIconText, DWRITE_TEXT_ALIGNMENT_CENTER);
                ctx.DrawTextW(message.items[i], ctx.FontSmall(), D2D1::RectF(textLeft + 28.0f, curY + 2.0f, textLeft + textWidth, curY + measured.itemHeights[i] + 6.0f), Colors::TextSub);
                curY += (std::max)(22.0f, measured.itemHeights[i] + 8.0f);
            }
            if (!message.planSteps.empty())
            {
                const auto card = D2D1::RectF(textLeft, curY + 4.0f, textLeft + textWidth, curY + 4.0f + measured.planHeight);
                ctx.FillRoundedRect(card, 7.0f, Colors::PlanCardBg);
                ctx.DrawRoundedRect(card, 7.0f, Colors::PlanCardBorder);
                float stepY = card.top;
                for (size_t i = 0; i < message.planSteps.size(); ++i)
                {
                    const auto& step = message.planSteps[i];
                    const auto badge = D2D1::RectF(card.left + 10.0f, stepY + 15.0f, card.left + 30.0f, stepY + 35.0f);
                    ctx.FillRoundedRect(badge, 5.0f, Colors::PlanIconBg);
                    ctx.DrawTextW(std::to_wstring(step.number), ctx.FontTinyBold(), badge, Colors::PlanIconText, DWRITE_TEXT_ALIGNMENT_CENTER);
                    const float titleY = stepY + 10.0f;
                    const float descriptionY = titleY + measured.planTitleHeights[i] + 2.0f;
                    ctx.DrawTextW(step.title, ctx.FontSmallBold(), D2D1::RectF(badge.right + 10.0f, titleY, card.right - 10.0f, titleY + measured.planTitleHeights[i] + 1.0f), Colors::TextHeading);
                    ctx.DrawTextW(step.description, ctx.FontTiny(), D2D1::RectF(badge.right + 10.0f, descriptionY, card.right - 10.0f, descriptionY + measured.planDescriptionHeights[i] + 1.0f), Colors::TextMuted);
                    stepY += measured.planRowHeights[i];
                    if (i + 1 < message.planSteps.size()) ctx.DrawLine(D2D1::Point2F(card.left, stepY), D2D1::Point2F(card.right, stepY), Colors::PlanCardDivider);
                }
            }
            contentY += measured.height;
        }
        ctx.PopClip();
        if (maxScroll > 0.0f)
        {
            const float thumbHeight = (std::min)(viewportHeight, (std::max)(24.0f, viewportHeight * viewportHeight / outTotalContentHeight));
            const float thumbY = layout.scrollbarTrack.top + scrollOffset / maxScroll * (viewportHeight - thumbHeight);
            ctx.FillRoundedRect(D2D1::RectF(layout.scrollbarTrack.left, thumbY, layout.scrollbarTrack.right, thumbY + thumbHeight), 3.0f, Colors::PlanCardBorder);
        }
    }
    void UIComponents::RenderComposer(Direct2DContext& ctx, const LayoutMetrics& layout,
        const std::wstring& selectedModel, int attachedFilesCount, bool isListening,
        const std::wstring& draftText, HitTestResult hoveredTarget, float animTick, bool isGeneratingReply)
    {
        // Composer container box
        ctx.FillRoundedRect(layout.composerBox, 9.0f, Colors::ComposerBg);
        ctx.DrawRoundedRect(layout.composerBox, 9.0f, Colors::ComposerBorder, 1.0f);

        // Placeholder if empty
        if (draftText.empty())
        {
            ctx.DrawTextW(L"Type a message or attach files", ctx.FontChat(),
                D2D1::RectF(layout.composerEditArea.left + 2.0f, layout.composerEditArea.top + 4.0f, layout.composerEditArea.right, layout.composerEditArea.bottom),
                Colors::TextDark);
        }

        if (!draftText.empty())
        {
            // The native editor is hidden while a popup is open; retain the draft in the composed surface.
            ctx.PushClip(layout.composerEditArea);
            ctx.DrawTextW(draftText, ctx.FontBody(), layout.composerEditArea, Colors::TextMain);
            ctx.PopClip();
        }
        // Toolbar: Attach File
        bool attachHover = (hoveredTarget.type == HitTargetType::ComposerAttach);
        if (attachHover)
            ctx.FillRoundedRect(layout.composerAttachBtn, 5.0f, Colors::HoverBg);
        ctx.DrawIcon(IconType::Paperclip, D2D1::RectF(layout.composerAttachBtn.left + 5.0f, layout.composerAttachBtn.top + 5.0f, layout.composerAttachBtn.right - 5.0f, layout.composerAttachBtn.bottom - 5.0f),
            attachHover ? Colors::White : Colors::TextMuted, 1.5f);

        // Model selector button
        bool modelHover = (hoveredTarget.type == HitTargetType::ComposerModelSelect);
        ctx.FillRoundedRect(layout.composerModelBtn, 5.0f, modelHover ? Colors::ButtonHover : Colors::SelectBg);
        ctx.DrawTextSingleLine((selectedModel.empty() ? L"Select model" : selectedModel) + L" ⌄", ctx.FontTiny(),
            D2D1::RectF(layout.composerModelBtn.left + 8.0f, layout.composerModelBtn.top + 7.0f, layout.composerModelBtn.right - 6.0f, layout.composerModelBtn.bottom),
            Colors::TextSub);

        // Context chip: ● N files
        bool contextHover = (hoveredTarget.type == HitTargetType::ComposerContextChip);
        if (contextHover)
            ctx.FillRoundedRect(layout.composerContextChip, 5.0f, Colors::HoverBg);
        ctx.FillCircle(layout.composerContextChip.left + 8.0f, layout.composerContextChip.top + 14.0f, 3.0f, Colors::Eyebrow);
        std::wstring chipText = std::to_wstring(attachedFilesCount) + L" files";
        ctx.DrawTextW(chipText, ctx.FontTiny(),
            D2D1::RectF(layout.composerContextChip.left + 16.0f, layout.composerContextChip.top + 7.0f, layout.composerContextChip.right, layout.composerContextChip.bottom),
            Colors::TextMuted);

        // Mic button
        if (isListening)
        {
            ctx.FillRoundedRect(layout.composerMicBtn, 5.0f, Colors::MicListeningBg);
            float pulse = 0.5f + 0.5f * sinf(animTick * 6.0f);
            ctx.DrawIcon(IconType::Mic, D2D1::RectF(layout.composerMicBtn.left + 5.0f, layout.composerMicBtn.top + 5.0f, layout.composerMicBtn.right - 5.0f, layout.composerMicBtn.bottom - 5.0f),
                Colors::MicListeningText, 1.6f + pulse * 0.4f);
        }
        else
        {
            bool micHover = (hoveredTarget.type == HitTargetType::ComposerMic);
            if (micHover)
                ctx.FillRoundedRect(layout.composerMicBtn, 5.0f, Colors::HoverBg);
            ctx.DrawIcon(IconType::Mic, D2D1::RectF(layout.composerMicBtn.left + 5.0f, layout.composerMicBtn.top + 5.0f, layout.composerMicBtn.right - 5.0f, layout.composerMicBtn.bottom - 5.0f),
                micHover ? Colors::White : Colors::TextMuted, 1.5f);
        }

        // Send button
        const bool canSend = !isGeneratingReply && std::any_of(draftText.begin(), draftText.end(),
            [](wchar_t character) { return !std::iswspace(character); });
        D2D1_COLOR_F sendBg = (canSend || isGeneratingReply) ? Colors::SendButton : Colors::SendButtonDisabled;
        D2D1_COLOR_F sendIconColor = (canSend || isGeneratingReply) ? Colors::White : Colors::SendTextDisabled;

        ctx.FillRoundedRect(layout.composerSendBtn, 6.0f, sendBg);
        if (isGeneratingReply)
            ctx.FillRoundedRect(D2D1::RectF(layout.composerSendBtn.left + 10.0f, layout.composerSendBtn.top + 10.0f,
                layout.composerSendBtn.right - 10.0f, layout.composerSendBtn.bottom - 10.0f), 1.5f, sendIconColor);
        else
            ctx.DrawIcon(IconType::Send, D2D1::RectF(layout.composerSendBtn.left + 6.0f, layout.composerSendBtn.top + 6.0f, layout.composerSendBtn.right - 6.0f, layout.composerSendBtn.bottom - 6.0f),
                sendIconColor, 1.5f);

        // Hint below
        ctx.DrawTextW(L"Enter to send · Shift + Enter for new line", ctx.FontTiny(),
            D2D1::RectF(layout.composerBox.left, layout.composerBox.bottom + 2.0f, layout.composerBox.right, layout.composerBox.bottom + 14.0f),
            Colors::TextDark, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    void UIComponents::RenderStatusbar(Direct2DContext& ctx, const LayoutMetrics& layout, float animTick,
        const std::wstring& connectionStatus, bool connected, const std::wstring& copyFeedback, bool copyFeedbackError)
    {
        ctx.FillRect(layout.statusbar, Colors::StatusbarBg);
        ctx.DrawLine(D2D1::Point2F(layout.statusbar.left, layout.statusbar.top),
            D2D1::Point2F(layout.statusbar.right, layout.statusbar.top), Colors::StatusbarBorder, 1.0f);

        // Local session label.
        ctx.DrawTextW(L"⌁", ctx.FontTitle(), D2D1::RectF(12.0f, layout.statusbar.top + 2.0f, 26.0f, layout.statusbar.bottom), Colors::Eyebrow);
        ctx.DrawTextW(L"local", ctx.FontTiny(), D2D1::RectF(28.0f, layout.statusbar.top + 5.0f, 65.0f, layout.statusbar.bottom), Colors::TextMuted);
        ctx.DrawTextSingleLine(copyFeedback, ctx.FontTiny(), D2D1::RectF(80.0f, layout.statusbar.top + 5.0f, layout.statusbar.right - 220.0f, layout.statusbar.bottom),
            copyFeedbackError ? Colors::TrafficRed : Colors::CheckmarkGreen);

        // Offline and text encoding status.
        float rightX = layout.statusbar.right - 14.0f;

        ctx.DrawTextW(L"UTF-8", ctx.FontTiny(), D2D1::RectF(rightX - 45.0f, layout.statusbar.top + 5.0f, rightX, layout.statusbar.bottom), Colors::TextMuted);
        rightX -= 60.0f;

        // Offline status dot
        (void)animTick;
        D2D1_COLOR_F dotColor = connected ? Colors::TabStatusGreen : Colors::TextMuted;
        ctx.FillCircle(rightX - 130.0f, layout.statusbar.top + 12.0f, 3.0f, dotColor);
        ctx.DrawTextSingleLine(connectionStatus, ctx.FontTiny(), D2D1::RectF(rightX - 122.0f, layout.statusbar.top + 5.0f, rightX, layout.statusbar.bottom), Colors::TextMuted);
    }

    void UIComponents::RenderDropdown(Direct2DContext& ctx, const LayoutMetrics& layout,
        const std::vector<MenuItem>& items, HitTestResult hoveredTarget)
    {
        if (layout.dropdownRect.right <= layout.dropdownRect.left || layout.dropdownRect.bottom <= layout.dropdownRect.top) return;
        ctx.FillRoundedRect(layout.dropdownRect, 7.0f, Colors::DropdownBg);
        ctx.DrawRoundedRect(layout.dropdownRect, 7.0f, Colors::DropdownBorder, 1.0f);

        ctx.PushClip(layout.dropdownViewport);
        for (size_t i = 0; i < items.size() && i < layout.dropdownItems.size(); ++i)
        {
            const auto& item = items[i];
            const auto& r = layout.dropdownItems[i];
            if (r.bottom <= layout.dropdownViewport.top || r.top >= layout.dropdownViewport.bottom) continue;

            bool hover = item.enabled && (hoveredTarget.type == HitTargetType::DropdownItem && hoveredTarget.index == static_cast<int>(i));
            if (hover)
            {
                ctx.FillRoundedRect(r, 4.0f, Colors::DropdownHover);
            }

            if (item.hasDivider)
            {
                ctx.DrawLine(D2D1::Point2F(r.left, r.top - 2.0f), D2D1::Point2F(r.right, r.top - 2.0f), Colors::PlanCardBorder, 1.0f);
            }

            const auto textColor = !item.enabled ? Colors::TextMuted : (hover ? Colors::White : Colors::TextMain);
            if (layout.dropdownIsModel)
            {
                const float textLeft = r.left + 25.0f;
                if (item.selected)
                    ctx.DrawTextSingleLine(L"\u2713", ctx.FontSmall(), D2D1::RectF(r.left + 6.0f, r.top + 5.0f, textLeft - 2.0f, r.bottom), Colors::CheckmarkGreen);
                ctx.DrawTextSingleLine(item.label, ctx.FontSmall(),
                    D2D1::RectF(textLeft, r.top + 5.0f, r.right - 9.0f, r.bottom), textColor);
                if (!item.shortcut.empty() && item.shortcut != item.label)
                    ctx.DrawTextSingleLine(item.shortcut, ctx.FontTiny(),
                        D2D1::RectF(textLeft, r.top + 23.0f, r.right - 9.0f, r.bottom), Colors::TextMuted);
            }
            else
            {
                ctx.DrawTextSingleLine(item.label, ctx.FontSmall(), D2D1::RectF(r.left + 9.0f, r.top + 5.0f,
                    r.right - (item.shortcut.empty() ? 9.0f : 70.0f), r.bottom), textColor);
                if (!item.shortcut.empty())
                    ctx.DrawTextW(item.shortcut, ctx.FontTiny(), D2D1::RectF(r.right - 70.0f, r.top + 6.0f, r.right - 9.0f, r.bottom),
                        Colors::TextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
        ctx.PopClip();
        if (layout.dropdownMaxScroll > 0.0f)
        {
            ctx.FillRoundedRect(layout.dropdownScrollTrack, 2.5f, Colors::SelectBg);
            ctx.FillRoundedRect(layout.dropdownScrollThumb, 2.5f, Colors::PlanCardBorder);
        }
    }
}
