#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include "AppTypes.h"

#include <functional>
#include <wincodec.h>

namespace Lattice
{
    using Microsoft::WRL::ComPtr;

    class Direct2DContext
    {
    public:
        Direct2DContext();
        ~Direct2DContext();

        bool Initialize(HWND hwnd);
        void Resize(UINT width, UINT height);
        bool BeginDraw();
        void SetDpi(float dpi);
        float GetDpi() const { return m_dpi; }
        float GetChatFontSize() const { return m_chatFontSize; }
        void SetChatFontSize(float size);
        HRESULT EndDraw();

        bool SaveRenderToPng(const std::wstring& filePath, UINT width, UINT height,
            const std::function<void()>& renderCallback);

        ID2D1RenderTarget* GetRenderTarget() const { return m_activeRenderTarget ? m_activeRenderTarget : m_renderTarget.Get(); }
        IDWriteFactory* GetDWriteFactory() const { return m_dwriteFactory.Get(); }

        // Color brushes
        ID2D1SolidColorBrush* GetSolidBrush(D2D1_COLOR_F color);
        ID2D1LinearGradientBrush* GetAssistantAvatarBrush(const D2D1_RECT_F& rect);

        // Text Formats
        IDWriteTextFormat* FontHeading() const { return m_fontHeading.Get(); }
        IDWriteTextFormat* FontTitle() const { return m_fontTitle.Get(); }
        IDWriteTextFormat* FontBody() const { return m_fontBody.Get(); }
        IDWriteTextFormat* FontBodyBold() const { return m_fontBodyBold.Get(); }
        IDWriteTextFormat* FontSmall() const { return m_fontSmall.Get(); }
        IDWriteTextFormat* FontSmallBold() const { return m_fontSmallBold.Get(); }
        IDWriteTextFormat* FontTiny() const { return m_fontTiny.Get(); }
        IDWriteTextFormat* FontTinyBold() const { return m_fontTinyBold.Get(); }
        IDWriteTextFormat* FontEyebrow() const { return m_fontEyebrow.Get(); }
        IDWriteTextFormat* FontCode() const { return m_fontCode.Get(); }
        IDWriteTextFormat* FontChat() const { return m_fontChat.Get(); }
        IDWriteTextFormat* FontChatCode() const { return m_fontChatCode.Get(); }

        // Drawing primitives
        void FillRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color);
        void DrawRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color, float strokeWidth = 1.0f);
        void FillRoundedRect(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color);
        void DrawRoundedRect(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color, float strokeWidth = 1.0f);
        void FillCircle(float cx, float cy, float radius, D2D1_COLOR_F color);
        void DrawCircle(float cx, float cy, float radius, D2D1_COLOR_F color, float strokeWidth = 1.0f);
        void DrawLine(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_COLOR_F color, float strokeWidth = 1.0f);
        void DrawTextW(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        
        void DrawTextSingleLine(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
            D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

        // Measure text height with wrapping
        float MeasureTextHeight(const std::wstring& text, IDWriteTextFormat* format, float maxWidth);
        float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format);

        // Vector Icon Rendering
        void DrawIcon(IconType type, const D2D1_RECT_F& rect, D2D1_COLOR_F color, float strokeWidth = 1.6f);

        void PushClip(const D2D1_RECT_F& rect);
        void PopClip();

    private:
        HWND m_hwnd = nullptr;
        float m_dpi = 96.0f;
        float m_chatFontSize = 11.5f;
        ComPtr<ID2D1Factory> m_d2dFactory;
        ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
        ID2D1RenderTarget* m_activeRenderTarget = nullptr;
        ComPtr<IDWriteFactory> m_dwriteFactory;
        ComPtr<ID2D1SolidColorBrush> m_tempBrush;
        ComPtr<ID2D1LinearGradientBrush> m_avatarBrush;

        ComPtr<IDWriteTextFormat> m_fontHeading;
        ComPtr<IDWriteTextFormat> m_fontTitle;
        ComPtr<IDWriteTextFormat> m_fontBody;
        ComPtr<IDWriteTextFormat> m_fontBodyBold;
        ComPtr<IDWriteTextFormat> m_fontSmall;
        ComPtr<IDWriteTextFormat> m_fontSmallBold;
        ComPtr<IDWriteTextFormat> m_fontTiny;
        ComPtr<IDWriteTextFormat> m_fontTinyBold;
        ComPtr<IDWriteTextFormat> m_fontEyebrow;
        ComPtr<IDWriteTextFormat> m_fontCode;
        ComPtr<IDWriteTextFormat> m_fontChat;
        ComPtr<IDWriteTextFormat> m_fontChatCode;

        bool CreateDeviceIndependentResources();
        bool CreateChatFonts(float size);
        bool CreateDeviceResources();
        bool CreateTargetBrushes(ID2D1RenderTarget* target);
        void DiscardDeviceResources();
    };

    // Color definitions
    namespace Colors
    {
        inline D2D1_COLOR_F Rgb(UINT32 hex, float alpha = 1.0f)
        {
            float r = ((hex >> 16) & 0xFF) / 255.0f;
            float g = ((hex >> 8) & 0xFF) / 255.0f;
            float b = (hex & 0xFF) / 255.0f;
            return D2D1::ColorF(r, g, b, alpha);
        }

        const auto ShellBg          = Rgb(0x15171c);
        const auto CanvasBg         = Rgb(0x111318);
        const auto TitlebarBg       = Rgb(0x191b20);
        const auto TitlebarBorder   = Rgb(0x272a31);
        const auto ActivityBarBg    = Rgb(0x181a1f);
        const auto ActivityBarBorder= Rgb(0x262930);
        const auto SidebarBg        = Rgb(0x1b1d23);
        const auto SidebarBorder    = Rgb(0x292c33);
        const auto TabsBarBg        = Rgb(0x181a1f);
        const auto TabInactiveBg    = Rgb(0x1b1d22);
        const auto TabActiveBg      = Rgb(0x15171c);
        const auto TabActiveTopLine = Rgb(0x828df5);
        const auto TabBorder        = Rgb(0x282b32);
        const auto TabStatusGreen   = Rgb(0x6ec18b);
        const auto TabStatusAmber   = Rgb(0xd9a85d);
        const auto StatusDotRingGreen = Rgb(0x294a35);
        const auto StatusDotRingAmber = Rgb(0x4d3b22);
        const auto TrafficRed       = Rgb(0xff5f57);
        const auto TrafficYellow    = Rgb(0xfebc2e);
        const auto TrafficGreen     = Rgb(0x28c840);
        const auto StatusbarBg      = Rgb(0x191b20);
        const auto StatusbarBorder  = Rgb(0x292c33);
        const auto ComposerBg       = Rgb(0x202329);
        const auto ComposerBorder   = Rgb(0x3a3e48);
        const auto ComposerBorderFocus = Rgb(0x6069b9);
        const auto SendButton       = Rgb(0x707bed);
        const auto SendButtonDisabled = Rgb(0x353942);
        const auto SendTextDisabled = Rgb(0x626975);
        const auto DropdownBg       = Rgb(0x21242b);
        const auto DropdownBorder   = Rgb(0x3a3e49);
        const auto DropdownHover    = Rgb(0x344081);
        const auto PlanCardBg       = Rgb(0x191b20);
        const auto PlanCardBorder   = Rgb(0x2d3038);
        const auto PlanCardDivider  = Rgb(0x292c32);
        const auto PlanIconBg       = Rgb(0x272b36);
        const auto PlanIconText     = Rgb(0x8d97ed);
        const auto TextMain         = Rgb(0xd8dee9);
        const auto TextSub          = Rgb(0xaeb4bf);
        const auto TextMuted        = Rgb(0x777f8d);
        const auto TextDark         = Rgb(0x4e5560);
        const auto TextHeading      = Rgb(0xeef1f6);
        const auto Eyebrow          = Rgb(0x747ff0);
        const auto ActivityActivePill= Rgb(0x8591ff);
        const auto ActivityIcon     = Rgb(0x777f8d);
        const auto ActivityIconHover= Rgb(0xccd2dc);
        const auto ActivityIconActive= Rgb(0xf1f3f7);
        const auto AvatarUser       = Rgb(0x3c424e);
        const auto AvatarUserSmall  = Rgb(0x4e5665);
        const auto AvatarAssistantStart = Rgb(0x717bec);
        const auto AvatarAssistantEnd   = Rgb(0x4f57a9);
        const auto AppMarkBg        = Rgb(0x6b72de);
        const auto MicListeningBg   = Rgb(0x41292b);
        const auto MicListeningText = Rgb(0xf08484);
        const auto TokenBarBg       = Rgb(0x343840);
        const auto TokenBarFill     = Rgb(0x7783f1);
        const auto PluginTealBg     = Rgb(0x335b67);
        const auto PluginTealIcon   = Rgb(0x8fe1db);
        const auto PluginVioletBg   = Rgb(0x3c3760);
        const auto PluginVioletIcon = Rgb(0xaeb5ff);
        const auto EnabledGreen     = Rgb(0x67bc85);
        const auto SkillBadgeBg     = Rgb(0x2e333d);
        const auto SkillBadgeText   = Rgb(0x8da4ee);
        const auto CheckmarkGreen   = Rgb(0x72b88e);
        const auto FileReact        = Rgb(0x5bc9e8);
        const auto FileTs           = Rgb(0x5baee5);
        const auto FileMd           = Rgb(0x9ba8ff);
        const auto FileEnv          = Rgb(0xdfbf68);
        const auto HoverBg          = Rgb(0x262932);
        const auto ButtonHover      = Rgb(0x2b2e36);
        const auto ToggleBgOff      = Rgb(0x393d45);
        const auto ToggleBgOn       = Rgb(0x6570d9);
        const auto ToggleThumb      = Rgb(0xcbd0d8);
        const auto SelectBg         = Rgb(0x22252c);
        const auto SelectBorder     = Rgb(0x343842);
        const auto DateRuleLine     = Rgb(0x25282e);
        const auto DateRuleText     = Rgb(0x5f6672);
        const auto White            = Rgb(0xffffff);
    }
}
