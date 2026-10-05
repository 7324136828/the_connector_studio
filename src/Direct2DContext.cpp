#include "Direct2DContext.h"
#include <cmath>
#include <algorithm>

namespace Lattice
{
    Direct2DContext::Direct2DContext()
    {
    }

    Direct2DContext::~Direct2DContext()
    {
        DiscardDeviceResources();
    }

    bool Direct2DContext::Initialize(HWND hwnd)
    {
        m_hwnd = hwnd;
        if (hwnd && hwnd != GetDesktopWindow()) SetDpi(static_cast<float>(GetDpiForWindow(hwnd)));
        if (!CreateDeviceIndependentResources())
            return false;
        if (hwnd && hwnd != GetDesktopWindow())
        {
            return CreateDeviceResources();
        }
        return true;
    }

    bool Direct2DContext::CreateDeviceIndependentResources()
    {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (FAILED(hr)) return false;

        const auto createFont = [&](const wchar_t* name, DWRITE_FONT_WEIGHT weight, float size, ComPtr<IDWriteTextFormat>& font) {
            return SUCCEEDED(m_dwriteFactory->CreateTextFormat(name, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", font.ReleaseAndGetAddressOf()));
        };
        const wchar_t* fontName = L"Segoe UI";
        if (!createFont(fontName, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f, m_fontHeading) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_MEDIUM, 13.0f, m_fontTitle) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, m_fontBody) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.5f, m_fontBodyBold) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_NORMAL, 10.0f, m_fontSmall) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, m_fontSmallBold) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_NORMAL, 8.5f, m_fontTiny) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_BOLD, 8.5f, m_fontTinyBold) ||
            !createFont(fontName, DWRITE_FONT_WEIGHT_ULTRA_BOLD, 9.0f, m_fontEyebrow) ||
            !createFont(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 11.0f, m_fontCode)) return false;
        m_fontBody->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.5f, 14.0f);
        return CreateChatFonts(m_chatFontSize);
    }
    bool Direct2DContext::CreateChatFonts(float size)
    {
        if (!m_dwriteFactory) return false;
        ComPtr<IDWriteTextFormat> body, code;
        const float scale = size / 11.5f;
        if (FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", body.GetAddressOf())) ||
            FAILED(m_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale, L"en-US", code.GetAddressOf()))) return false;
        body->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.5f * scale, 14.0f * scale);
        code->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f * scale, 13.0f * scale);
        m_fontChat = std::move(body); m_fontChatCode = std::move(code);
        return true;
    }
    void Direct2DContext::SetChatFontSize(float size)
    {
        if (!std::isfinite(size) || size < 10.0f || size > 24.0f || size == m_chatFontSize) return;
        if (m_dwriteFactory && !CreateChatFonts(size)) return;
        m_chatFontSize = size;
    }

    bool Direct2DContext::CreateTargetBrushes(ID2D1RenderTarget* target)
    {
        if (!target) return false;
        ComPtr<ID2D1SolidColorBrush> solid;
        HRESULT hr = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), solid.GetAddressOf());
        if (FAILED(hr)) return false;
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, Colors::AvatarAssistantStart }, { 1.0f, Colors::AvatarAssistantEnd }
        };
        ComPtr<ID2D1GradientStopCollection> stopCollection;
        hr = target->CreateGradientStopCollection(stops, 2, stopCollection.GetAddressOf());
        if (FAILED(hr)) return false;
        ComPtr<ID2D1LinearGradientBrush> avatar;
        hr = target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(27, 27)),
            stopCollection.Get(), avatar.GetAddressOf());
        if (FAILED(hr)) return false;
        m_tempBrush = solid;
        m_avatarBrush = avatar;
        return true;
    }

    bool Direct2DContext::CreateDeviceResources()
    {
        if (m_renderTarget) return m_tempBrush.Get() != nullptr && m_avatarBrush.Get() != nullptr;
        if (!m_d2dFactory || !m_hwnd || m_hwnd == GetDesktopWindow()) return false;
        RECT client{};
        if (!GetClientRect(m_hwnd, &client)) return false;
        const auto size = D2D1::SizeU(static_cast<UINT32>((std::max)(1L, client.right - client.left)), static_cast<UINT32>((std::max)(1L, client.bottom - client.top)));
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), m_dpi, m_dpi);
        HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(properties,
            D2D1::HwndRenderTargetProperties(m_hwnd, size), m_renderTarget.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        m_renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (!CreateTargetBrushes(m_renderTarget.Get()))
        {
            DiscardDeviceResources();
            return false;
        }
        return true;
    }

    void Direct2DContext::DiscardDeviceResources()
    {
        m_activeRenderTarget = nullptr;
        m_avatarBrush.Reset();
        m_tempBrush.Reset();
        m_renderTarget.Reset();
    }

    void Direct2DContext::SetDpi(float dpi)
    {
        m_dpi = std::isfinite(dpi) && dpi > 0.0f ? dpi : 96.0f;
        if (m_renderTarget) m_renderTarget->SetDpi(m_dpi, m_dpi);
    }

    void Direct2DContext::Resize(UINT width, UINT height)
    {
        if (m_renderTarget && width > 0 && height > 0 && FAILED(m_renderTarget->Resize(D2D1::SizeU(width, height))))
            DiscardDeviceResources();
    }

    bool Direct2DContext::BeginDraw()
    {
        if (!CreateDeviceResources()) return false;
        m_renderTarget->BeginDraw();
        return true;
    }

    HRESULT Direct2DContext::EndDraw()
    {
        if (!m_renderTarget) return E_FAIL;
        const HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
        return hr;
    }
    bool Direct2DContext::SaveRenderToPng(const std::wstring& filePath, UINT width, UINT height,
        const std::function<void()>& renderCallback)
    {
        if (!m_d2dFactory || !width || !height || !renderCallback) return false;
        ComPtr<IWICImagingFactory> wicFactory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) return false;

        ComPtr<IWICBitmap> wicBitmap;
        hr = wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnDemand, &wicBitmap);
        if (FAILED(hr)) return false;

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

        ComPtr<ID2D1RenderTarget> wicTarget;
        hr = m_d2dFactory->CreateWicBitmapRenderTarget(wicBitmap.Get(), props, &wicTarget);
        if (FAILED(hr)) return false;

        wicTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        wicTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        ID2D1RenderTarget* oldTarget = m_activeRenderTarget;
        ComPtr<ID2D1SolidColorBrush> oldBrush = m_tempBrush;
        ComPtr<ID2D1LinearGradientBrush> oldAvatar = m_avatarBrush;
        // Brushes belong to a render target; never reuse the HWND brushes on a WIC target.
        if (!CreateTargetBrushes(wicTarget.Get())) return false;
        m_activeRenderTarget = wicTarget.Get();
        wicTarget->BeginDraw();
        try
        {
            renderCallback();
            hr = wicTarget->EndDraw();
        }
        catch (...)
        {
            wicTarget->EndDraw();
            hr = E_FAIL;
        }
        m_activeRenderTarget = oldTarget;
        m_tempBrush = oldBrush;
        m_avatarBrush = oldAvatar;
        if (FAILED(hr)) return false;

        ComPtr<IWICBitmapEncoder> encoder;
        hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) return false;

        ComPtr<IWICStream> stream;
        hr = wicFactory->CreateStream(&stream);
        if (FAILED(hr)) return false;

        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        CloseHandle(hFile);

        hr = stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return false;

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return false;

        ComPtr<IWICBitmapFrameEncode> frame;
        hr = encoder->CreateNewFrame(&frame, nullptr);
        if (FAILED(hr)) return false;

        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) return false;

        hr = frame->SetSize(width, height);
        if (FAILED(hr)) return false;

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr)) return false;

        hr = frame->WriteSource(wicBitmap.Get(), nullptr);
        if (FAILED(hr)) return false;

        hr = frame->Commit();
        if (FAILED(hr)) return false;

        hr = encoder->Commit();
        return SUCCEEDED(hr);
    }

    ID2D1SolidColorBrush* Direct2DContext::GetSolidBrush(D2D1_COLOR_F color)
    {
        if (m_tempBrush)
        {
            m_tempBrush->SetColor(color);
            return m_tempBrush.Get();
        }
        return nullptr;
    }

    ID2D1LinearGradientBrush* Direct2DContext::GetAssistantAvatarBrush(const D2D1_RECT_F& rect)
    {
        if (m_avatarBrush)
        {
            m_avatarBrush->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
            m_avatarBrush->SetEndPoint(D2D1::Point2F(rect.right, rect.bottom));
            return m_avatarBrush.Get();
        }
        return nullptr;
    }

    void Direct2DContext::FillRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        rt->FillRectangle(rect, GetSolidBrush(color));
    }

    void Direct2DContext::DrawRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color, float strokeWidth)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        rt->DrawRectangle(rect, GetSolidBrush(color), strokeWidth);
    }

    void Direct2DContext::FillRoundedRect(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);
        rt->FillRoundedRectangle(rrect, GetSolidBrush(color));
    }

    void Direct2DContext::DrawRoundedRect(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color, float strokeWidth)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);
        rt->DrawRoundedRectangle(rrect, GetSolidBrush(color), strokeWidth);
    }

    void Direct2DContext::FillCircle(float cx, float cy, float radius, D2D1_COLOR_F color)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        rt->FillEllipse(ellipse, GetSolidBrush(color));
    }

    void Direct2DContext::DrawCircle(float cx, float cy, float radius, D2D1_COLOR_F color, float strokeWidth)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        rt->DrawEllipse(ellipse, GetSolidBrush(color), strokeWidth);
    }

    void Direct2DContext::DrawLine(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_COLOR_F color, float strokeWidth)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;
        rt->DrawLine(p0, p1, GetSolidBrush(color), strokeWidth);
    }

    void Direct2DContext::DrawTextW(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush || !format || text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        rt->DrawText(text.c_str(), static_cast<UINT32>(text.length()), format, rect, GetSolidBrush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Direct2DContext::DrawTextSingleLine(const std::wstring& text, IDWriteTextFormat* format,
        const D2D1_RECT_F& rect, D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align)
    {
        auto target = GetRenderTarget();
        if (!target || !m_tempBrush || !m_dwriteFactory || !format || text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
            rect.right - rect.left, rect.bottom - rect.top, layout.GetAddressOf()))) return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(align);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf())))
            layout->SetTrimming(&trimming, ellipsis.Get());
        target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), GetSolidBrush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float Direct2DContext::MeasureTextHeight(const std::wstring& text, IDWriteTextFormat* format, float maxWidth)
    {
        if (!m_dwriteFactory || !format || text.empty() || maxWidth <= 0.0f) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()),
            format, maxWidth, 10000.0f, layout.GetAddressOf());
        if (FAILED(hr)) return 18.0f;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
        return metrics.height;
    }

    float Direct2DContext::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format)
    {
        if (!m_dwriteFactory || !format || text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()),
            format, 10000.0f, 1000.0f, layout.GetAddressOf());
        if (FAILED(hr)) return 0.0f;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    void Direct2DContext::PushClip(const D2D1_RECT_F& rect)
    {
        auto rt = GetRenderTarget();
        if (rt)
        {
            rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
    }

    void Direct2DContext::PopClip()
    {
        auto rt = GetRenderTarget();
        if (rt)
        {
            rt->PopAxisAlignedClip();
        }
    }

    void Direct2DContext::DrawIcon(IconType type, const D2D1_RECT_F& rect, D2D1_COLOR_F color, float strokeWidth)
    {
        auto rt = GetRenderTarget();
        if (!rt || !m_tempBrush) return;

        float width = rect.right - rect.left;
        float height = rect.bottom - rect.top;
        float sx = width / 24.0f;
        float sy = height / 24.0f;

        auto P = [&](float x, float y) -> D2D1_POINT_2F {
            return D2D1::Point2F(rect.left + x * sx, rect.top + y * sy);
        };

        auto R = [&](float x, float y, float w, float h) -> D2D1_RECT_F {
            return D2D1::RectF(rect.left + x * sx, rect.top + y * sy,
                rect.left + (x + w) * sx, rect.top + (y + h) * sy);
        };

        switch (type)
        {
        case IconType::Agent:
        {
            // rect x="5" y="7" width="14" height="12" rx="3"
            DrawRoundedRect(R(5.0f, 7.0f, 14.0f, 12.0f), 3.0f * sx, color, strokeWidth);
            // M12 3v4
            DrawLine(P(12.0f, 3.0f), P(12.0f, 7.0f), color, strokeWidth);
            // Eyes
            FillCircle(P(8.5f, 11.5f).x, P(8.5f, 11.5f).y, 1.0f * sx, color);
            FillCircle(P(15.5f, 11.5f).x, P(15.5f, 11.5f).y, 1.0f * sx, color);
            // Mouth M9 15h6
            DrawLine(P(9.0f, 15.0f), P(15.0f, 15.0f), color, strokeWidth);
            break;
        }
        case IconType::Branch:
        {
            // circles (6,5,2), (18,6,2), (6,19,2)
            DrawCircle(P(6.0f, 5.0f).x, P(6.0f, 5.0f).y, 2.0f * sx, color, strokeWidth);
            DrawCircle(P(18.0f, 6.0f).x, P(18.0f, 6.0f).y, 2.0f * sx, color, strokeWidth);
            DrawCircle(P(6.0f, 19.0f).x, P(6.0f, 19.0f).y, 2.0f * sx, color, strokeWidth);
            // vertical line M6 7v10
            DrawLine(P(6.0f, 7.0f), P(6.0f, 17.0f), color, strokeWidth);
            // branch curve approx
            DrawLine(P(8.0f, 8.0f), P(11.0f, 11.0f), color, strokeWidth);
            DrawLine(P(11.0f, 11.0f), P(15.0f, 11.0f), color, strokeWidth);
            DrawLine(P(15.0f, 11.0f), P(18.0f, 8.0f), color, strokeWidth);
            break;
        }
        case IconType::Chevron:
        {
            DrawLine(P(9.0f, 6.0f), P(15.0f, 12.0f), color, strokeWidth);
            DrawLine(P(15.0f, 12.0f), P(9.0f, 18.0f), color, strokeWidth);
            break;
        }
        case IconType::Close:
        {
            DrawLine(P(7.0f, 7.0f), P(17.0f, 17.0f), color, strokeWidth);
            DrawLine(P(17.0f, 7.0f), P(7.0f, 17.0f), color, strokeWidth);
            break;
        }
        case IconType::File:
        {
            // M6 2h8l4 4v16H6z
            DrawLine(P(6.0f, 2.0f), P(14.0f, 2.0f), color, strokeWidth);
            DrawLine(P(14.0f, 2.0f), P(18.0f, 6.0f), color, strokeWidth);
            DrawLine(P(18.0f, 6.0f), P(18.0f, 22.0f), color, strokeWidth);
            DrawLine(P(18.0f, 22.0f), P(6.0f, 22.0f), color, strokeWidth);
            DrawLine(P(6.0f, 22.0f), P(6.0f, 2.0f), color, strokeWidth);
            // M14 2v5h5
            DrawLine(P(14.0f, 2.0f), P(14.0f, 7.0f), color, strokeWidth);
            DrawLine(P(14.0f, 7.0f), P(18.0f, 7.0f), color, strokeWidth);
            break;
        }
        case IconType::Folder:
        {
            // M3 6h6l2 2h10v11H3z
            DrawLine(P(3.0f, 6.0f), P(9.0f, 6.0f), color, strokeWidth);
            DrawLine(P(9.0f, 6.0f), P(11.0f, 8.0f), color, strokeWidth);
            DrawLine(P(11.0f, 8.0f), P(21.0f, 8.0f), color, strokeWidth);
            DrawLine(P(21.0f, 8.0f), P(21.0f, 19.0f), color, strokeWidth);
            DrawLine(P(21.0f, 19.0f), P(3.0f, 19.0f), color, strokeWidth);
            DrawLine(P(3.0f, 19.0f), P(3.0f, 6.0f), color, strokeWidth);
            break;
        }
        case IconType::Mic:
        {
            // capsule
            DrawRoundedRect(R(9.0f, 3.0f, 6.0f, 11.0f), 3.0f * sx, color, strokeWidth);
            // cradle arc
            DrawLine(P(5.5f, 11.0f), P(5.5f, 13.0f), color, strokeWidth);
            DrawLine(P(5.5f, 13.0f), P(12.0f, 17.5f), color, strokeWidth);
            DrawLine(P(12.0f, 17.5f), P(18.5f, 13.0f), color, strokeWidth);
            DrawLine(P(18.5f, 13.0f), P(18.5f, 11.0f), color, strokeWidth);
            // stand
            DrawLine(P(12.0f, 17.5f), P(12.0f, 21.0f), color, strokeWidth);
            DrawLine(P(9.0f, 21.0f), P(15.0f, 21.0f), color, strokeWidth);
            break;
        }
        case IconType::More:
        {
            FillCircle(P(5.0f, 12.0f).x, P(5.0f, 12.0f).y, 1.4f * sx, color);
            FillCircle(P(12.0f, 12.0f).x, P(12.0f, 12.0f).y, 1.4f * sx, color);
            FillCircle(P(19.0f, 12.0f).x, P(19.0f, 12.0f).y, 1.4f * sx, color);
            break;
        }
        case IconType::Paperclip:
        {
            DrawLine(P(17.0f, 8.0f), P(9.0f, 16.0f), color, strokeWidth);
            DrawLine(P(9.0f, 16.0f), P(7.0f, 14.0f), color, strokeWidth);
            DrawLine(P(7.0f, 14.0f), P(15.0f, 6.0f), color, strokeWidth);
            DrawLine(P(15.0f, 6.0f), P(18.0f, 9.0f), color, strokeWidth);
            DrawLine(P(18.0f, 9.0f), P(10.0f, 17.0f), color, strokeWidth);
            DrawLine(P(10.0f, 17.0f), P(6.0f, 13.0f), color, strokeWidth);
            DrawLine(P(6.0f, 13.0f), P(12.0f, 7.0f), color, strokeWidth);
            break;
        }
        case IconType::Plugin:
        {
            DrawLine(P(8.0f, 3.0f), P(8.0f, 7.0f), color, strokeWidth);
            DrawLine(P(16.0f, 3.0f), P(16.0f, 7.0f), color, strokeWidth);
            DrawLine(P(6.0f, 7.0f), P(18.0f, 7.0f), color, strokeWidth);
            DrawLine(P(6.0f, 7.0f), P(6.0f, 12.0f), color, strokeWidth);
            DrawLine(P(18.0f, 7.0f), P(18.0f, 12.0f), color, strokeWidth);
            DrawLine(P(6.0f, 12.0f), P(12.0f, 16.0f), color, strokeWidth);
            DrawLine(P(18.0f, 12.0f), P(12.0f, 16.0f), color, strokeWidth);
            DrawLine(P(12.0f, 16.0f), P(12.0f, 21.0f), color, strokeWidth);
            break;
        }
        case IconType::Search:
        {
            DrawCircle(P(11.0f, 11.0f).x, P(11.0f, 11.0f).y, 6.5f * sx, color, strokeWidth);
            DrawLine(P(16.0f, 16.0f), P(20.5f, 20.5f), color, strokeWidth);
            break;
        }
        case IconType::Send:
        {
            // M21 3L12.5 21 10.2 13.8 3 11.5z
            DrawLine(P(21.0f, 3.0f), P(12.5f, 21.0f), color, strokeWidth);
            DrawLine(P(12.5f, 21.0f), P(10.2f, 13.8f), color, strokeWidth);
            DrawLine(P(10.2f, 13.8f), P(3.0f, 11.5f), color, strokeWidth);
            DrawLine(P(3.0f, 11.5f), P(21.0f, 3.0f), color, strokeWidth);
            DrawLine(P(10.2f, 13.8f), P(21.0f, 3.0f), color, strokeWidth);
            break;
        }
        case IconType::Settings:
        {
            DrawCircle(P(12.0f, 12.0f).x, P(12.0f, 12.0f).y, 3.5f * sx, color, strokeWidth);
            DrawCircle(P(12.0f, 12.0f).x, P(12.0f, 12.0f).y, 7.5f * sx, color, strokeWidth * 0.8f);
            // 8 spokes/teeth
            for (int i = 0; i < 8; ++i)
            {
                float angle = i * 3.14159f / 4.0f;
                float c = cosf(angle);
                float s = sinf(angle);
                DrawLine(P(12.0f + 6.5f * c, 12.0f + 6.5f * s),
                         P(12.0f + 9.0f * c, 12.0f + 9.0f * s), color, strokeWidth * 1.2f);
            }
            break;
        }
        case IconType::Sparkles:
        {
            // Center large sparkle (12, 8)
            auto DrawStar = [&](float cx, float cy, float r) {
                DrawLine(P(cx, cy - r), P(cx, cy + r), color, strokeWidth);
                DrawLine(P(cx - r, cy), P(cx + r, cy), color, strokeWidth);
                DrawLine(P(cx - r * 0.45f, cy - r * 0.45f), P(cx + r * 0.45f, cy + r * 0.45f), color, strokeWidth * 0.8f);
                DrawLine(P(cx + r * 0.45f, cy - r * 0.45f), P(cx - r * 0.45f, cy + r * 0.45f), color, strokeWidth * 0.8f);
            };
            DrawStar(12.0f, 8.0f, 5.0f);
            DrawStar(19.0f, 16.0f, 3.2f);
            DrawStar(5.0f, 15.0f, 3.0f);
            break;
        }
        }
    }
}
