#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include "AppTypes.h"
#include "Direct2DContext.h"

namespace Lattice
{
    enum class HitTargetType
    {
        None,
        TrafficClose,
        TrafficMinimize,
        TrafficMaximize,
        MenuProject,
        MenuSession,
        TitleSearch,
        ActivityFiles,
        ActivitySession,
        ActivityPlugins,
        ActivitySettings,
        TabItem,
        TabClose,
        TabNew,
        TabPrevious,
        TabNext,
        SidebarProjectRow,
        SidebarFileRow,
        SidebarModelSelect,
        SidebarAgentSelect,
        SidebarTogglePlan,
        SidebarToggleSafeTools,
        SidebarSkillItem,
        SidebarAddSkill,
        SidebarPluginCard,
        SidebarBrowsePlugins,
        ComposerEdit,
        ComposerAttach,
        ComposerModelSelect,
        ComposerContextChip,
        ComposerMic,
        ComposerSend,
        DropdownItem,
        ScrollbarThumb,
        ScrollbarTrack,
        SidebarScrollbarThumb,
        SidebarScrollbarTrack
    };

    struct HitTestResult
    {
        HitTargetType type = HitTargetType::None;
        int index = -1;
        D2D1_RECT_F rect{};
    };

    struct LayoutMetrics
    {
        float windowWidth = 1000.0f;
        float windowHeight = 700.0f;

        D2D1_RECT_F titlebar{};
        D2D1_RECT_F trafficLights{};
        D2D1_RECT_F menuProject{};
        D2D1_RECT_F menuSession{};
        D2D1_RECT_F windowTitle{};
        D2D1_RECT_F titleSearch{};

        D2D1_RECT_F activityBar{};
        D2D1_RECT_F actBtnFiles{};
        D2D1_RECT_F actBtnSession{};
        D2D1_RECT_F actBtnPlugins{};
        D2D1_RECT_F actBtnSettings{};

        D2D1_RECT_F sidebar{};
        D2D1_RECT_F sidebarHeading{};
        D2D1_RECT_F sidebarViewport{};
        D2D1_RECT_F sidebarScrollbarTrack{};
        D2D1_RECT_F sidebarScrollbarThumb{};
        float sidebarScrollOffset = 0.0f;
        float sidebarMaxScroll = 0.0f;
        float sidebarContentHeight = 0.0f;
        bool projectExpanded = true;
        bool hasActiveSession = false;
        bool hasProject = false;
        D2D1_RECT_F sidebarProjectRow{};
        D2D1_RECT_F sidebarOpenEditorsRow{};
        D2D1_RECT_F sidebarContextCard{};
        int openFileEditorsCount = 0;
        std::vector<D2D1_RECT_F> sidebarFileRows;
        D2D1_RECT_F sidebarModelSelect{};
        D2D1_RECT_F sidebarAgentSelect{};
        D2D1_RECT_F sidebarTogglePlan{};
        D2D1_RECT_F sidebarToggleSafeTools{};
        std::vector<D2D1_RECT_F> sidebarSkillRows;
        D2D1_RECT_F sidebarAddSkillBtn{};
        std::vector<D2D1_RECT_F> sidebarPluginCards;
        D2D1_RECT_F sidebarBrowsePluginsBtn{};

        D2D1_RECT_F editorArea{};
        D2D1_RECT_F tabsBar{};
        std::vector<D2D1_RECT_F> tabs;
        std::vector<D2D1_RECT_F> tabCloses;
        D2D1_RECT_F tabNewBtn{};
        D2D1_RECT_F tabPreviousBtn{};
        D2D1_RECT_F tabNextBtn{};
        int firstVisibleTab = 0;
        int visibleTabCount = 0;

        D2D1_RECT_F chatHeading{};
        D2D1_RECT_F conversationArea{};
        D2D1_RECT_F conversationInner{};
        D2D1_RECT_F scrollbarTrack{};
        D2D1_RECT_F scrollbarThumb{};
        float conversationContentHeight = 0.0f;
        float conversationMaxScroll = 0.0f;

        D2D1_RECT_F composerWrap{};
        D2D1_RECT_F composerBox{};
        D2D1_RECT_F composerEditArea{};
        D2D1_RECT_F composerAttachBtn{};
        D2D1_RECT_F composerModelBtn{};
        D2D1_RECT_F composerContextChip{};
        D2D1_RECT_F composerMicBtn{};
        D2D1_RECT_F composerSendBtn{};

        D2D1_RECT_F statusbar{};

        D2D1_RECT_F dropdownRect{};
        std::vector<D2D1_RECT_F> dropdownItems;
    };

    class UIComponents
    {
    public:
        static void ComputeLayout(LayoutMetrics& layout, float width, float height,
            SidebarMode sidebarMode, const std::vector<SessionTab>& tabs, int activeTab,
            const std::vector<FileItem>& files, const std::vector<SkillItem>& skills,
            const std::vector<PluginItem>& plugins, ActiveDropdown activeDropdown,
            const std::vector<MenuItem>& dropdownItems,
            float sidebarScrollOffset = 0.0f, bool projectExpanded = true, bool hasCurrentProject = false);

        static float MeasureConversationHeight(Direct2DContext& ctx, const LayoutMetrics& layout,
            const SessionTab& session);

        static float UpdateConversationScrollMetrics(LayoutMetrics& layout, float contentHeight, float scrollOffset);

        static HitTestResult HitTest(const LayoutMetrics& layout, float x, float y,
            SidebarMode sidebarMode, ActiveDropdown activeDropdown,
            const std::vector<MenuItem>& dropdownItems);

        static void Render(Direct2DContext& ctx, const LayoutMetrics& layout,
            SidebarMode sidebarMode, ActiveDropdown activeDropdown,
            const std::vector<MenuItem>& currentDropdownItems,
            const std::vector<SessionTab>& tabs, int activeTab,
            const std::vector<FileItem>& files, const std::vector<SkillItem>& skills,
            const std::vector<PluginItem>& plugins, const std::wstring& selectedModel,
            bool planBeforeEdits, bool autoRunSafeTools, bool isListening,
            const std::wstring& draftText, HitTestResult hoveredTarget,
            float animTick = 0.0f, bool isGeneratingReply = false, const std::wstring& currentProjectName = {});

    private:
        static void RenderTitlebar(Direct2DContext& ctx, const LayoutMetrics& layout,
            ActiveDropdown activeDropdown, const std::wstring& projectName, HitTestResult hoveredTarget);

        static void RenderActivityBar(Direct2DContext& ctx, const LayoutMetrics& layout,
            SidebarMode sidebarMode, HitTestResult hoveredTarget);

        static void RenderSidebar(Direct2DContext& ctx, const LayoutMetrics& layout,
            SidebarMode sidebarMode, const std::vector<FileItem>& files,
            const std::vector<SkillItem>& skills, const std::vector<PluginItem>& plugins,
            const std::wstring& selectedModel, bool planBeforeEdits, bool autoRunSafeTools,
            int attachedFilesCount, const std::wstring& projectName, HitTestResult hoveredTarget);

        static void RenderTabsBar(Direct2DContext& ctx, const LayoutMetrics& layout,
            const std::vector<SessionTab>& tabs, int activeTab, HitTestResult hoveredTarget);

        static void RenderChatHeading(Direct2DContext& ctx, const LayoutMetrics& layout,
            const SessionTab& session);

        static void RenderConversation(Direct2DContext& ctx, const LayoutMetrics& layout,
            const SessionTab& session, float& outTotalContentHeight);

        static void RenderComposer(Direct2DContext& ctx, const LayoutMetrics& layout,
            const std::wstring& selectedModel, int attachedFilesCount, bool isListening,
            const std::wstring& draftText, HitTestResult hoveredTarget, float animTick, bool isGeneratingReply);

        static void RenderStatusbar(Direct2DContext& ctx, const LayoutMetrics& layout, float animTick);

        static void RenderDropdown(Direct2DContext& ctx, const LayoutMetrics& layout,
            const std::vector<MenuItem>& items, HitTestResult hoveredTarget);
    };
}
