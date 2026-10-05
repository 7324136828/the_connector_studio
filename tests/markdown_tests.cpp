#include "MarkdownRenderer.h"
#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Lattice;
namespace
{
    int checks = 0;
    void Check(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }
    bool HasStyle(const MarkdownText& text, const std::wstring& needle, unsigned style)
    {
        const auto start = text.text.find(needle);
        if (start == std::wstring::npos) return false;
        auto covered = start;
        const auto end = start + needle.size();
        for (const auto& span : text.spans)
        {
            if (span.start + span.length <= covered) continue;
            if (span.start > covered || (span.style & style) != style) return false;
            covered = span.start + span.length;
            if (covered >= end) return true;
        }
        return false;
    }
    void TestInlineAndBlocks()
    {
        const std::wstring source = L"# Native **Markdown**\r\n\r\n"
            L"Plain **bold** and *italic*, ***both***, `APC_LEVEL` and foo_bar_baz. \\*literal\\* and ~~obsolete~~.\n\n"
            L"- first **item**\n  continuation\n  - nested\n12. ordered item\n\n"
            L"> quoted *content*\n> another line\n\n"
            L"```cpp\nstd::string raw = \"**literal** | value\";\n// `code` stays literal\n```\n\n"
            L"---\n\n[Microsoft](https://example.invalid) ![diagram](https://example.invalid/image.png)\n";
        const auto original = source;
        const auto parsed = ParseMarkdown(source);
        Check(source == original, "Markdown parsing must not mutate the raw transcript");
        Check(parsed.blocks.size() == 9, "block structure must include heading, paragraphs, lists, quote, code, and rule");
        Check(parsed.blocks[0].kind == MarkdownBlockKind::Heading && parsed.blocks[0].headingLevel == 1,
            "ATX headings must use a heading block");
        const auto& text = parsed.blocks[1].content;
        Check(text.text.find(L"**") == std::wstring::npos && text.text.find(L"*literal*") != std::wstring::npos,
            "matched formatting must hide delimiters while escaped Markdown stays literal");
        Check(HasStyle(text, L"bold", MarkdownBold) && HasStyle(text, L"italic", MarkdownItalic) &&
            HasStyle(text, L"both", MarkdownBold | MarkdownItalic), "emphasis spans must preserve bold and italic combinations");
        Check(HasStyle(text, L"APC_LEVEL", MarkdownCode) && HasStyle(text, L"obsolete", MarkdownStrike),
            "inline code and strike spans must retain their own native styling");
        Check(text.text.find(L"foo_bar_baz") != std::wstring::npos, "underscores in identifiers must remain literal");
        Check(parsed.blocks[2].kind == MarkdownBlockKind::ListItem && parsed.blocks[2].content.text.find(L"continuation") != std::wstring::npos,
            "indented list continuation must remain under its list marker");
        Check(parsed.blocks[3].indent == 1 && parsed.blocks[4].marker == L"12.", "nested bullets and numbered lists must retain their depth and numbering");
        Check(parsed.blocks[5].kind == MarkdownBlockKind::Quote && parsed.blocks[5].content.text.find(L"another line") != std::wstring::npos,
            "blockquote lines must form a readable quote");
        Check(parsed.blocks[6].kind == MarkdownBlockKind::Code && parsed.blocks[6].content.spans.empty() &&
            parsed.blocks[6].content.text.find(L"**literal** | value") != std::wstring::npos,
            "fenced code must remain literal rather than parse Markdown inside it");
        Check(parsed.blocks[7].kind == MarkdownBlockKind::Rule, "horizontal rules must be rendered as a separator");
        Check(HasStyle(parsed.blocks[8].content, L"Microsoft", MarkdownLink) &&
            parsed.blocks[8].content.text.find(L"[Image: diagram]") != std::wstring::npos,
            "links and remote image descriptions must be readable without executing links or loading images");
        const auto edge = ParseMarkdown(L"Title\n=====\n\n## Second ##\n\nUnmatched **open and `code; <script> stays literal.\n\n~~~\nunfinished fence **raw**\n");
        Check(edge.blocks.size() == 4 && edge.blocks[0].headingLevel == 1 && edge.blocks[1].content.text == L"Second",
            "Setext and closing-hash headings must preserve their content");
        Check(edge.blocks[2].content.text.find(L"**open") != std::wstring::npos && edge.blocks[2].content.text.find(L"<script>") != std::wstring::npos,
            "unmatched Markdown and HTML must remain literal text");
        Check(edge.blocks[3].kind == MarkdownBlockKind::Code && edge.blocks[3].content.text == L"unfinished fence **raw**",
            "an unfinished fence must retain the final code line");
        const auto ticks = ParseMarkdown(L"Use `` `literal` | pipe `` and snake_case_name.\n");
        Check(ticks.blocks[0].content.text == L"Use `literal` | pipe and snake_case_name.", "multi-backtick inline code must preserve embedded backticks");
        const auto nested = ParseMarkdown(L"*italic **strong*** and **strong *italic***");
        Check(nested.blocks[0].content.text == L"italic strong and strong italic" &&
            HasStyle(nested.blocks[0].content, L"italic strong", MarkdownItalic) &&
            HasStyle(nested.blocks[0].content, L"strong italic", MarkdownBold),
            "nested emphasis must use the correct closing run without leaving stray delimiters");
    }
    void TestTablesAndBounds()
    {
        const auto parsed = ParseMarkdown(L"| Feature | APC | DPC |\n| :--- | :---: | ---: |\n"
            L"| Full name | **Asynchronous** Procedure Call | Deferred Procedure Call |\n"
            L"| Literal pipes | escaped \\| content | `a|b` |\n\nTrailing text\n");
        Check(parsed.blocks.size() == 2 && parsed.blocks[0].kind == MarkdownBlockKind::Table, "GFM table delimiters must produce a native table");
        const auto& table = parsed.blocks[0];
        Check(table.rows.size() == 3 && table.columns.size() == 3, "table must preserve its header and complete body rows");
        Check(table.columns[0] == MarkdownAlignment::Left && table.columns[1] == MarkdownAlignment::Center && table.columns[2] == MarkdownAlignment::Right,
            "table delimiter alignment must be preserved for native text layouts");
        Check(table.rows[2][1].text == L"escaped | content" && table.rows[2][2].text == L"a|b", "escaped and inline-code pipes must not split cells");
        Check(HasStyle(table.rows[1][1], L"Asynchronous", MarkdownBold), "table cells must use the same inline Markdown parser");
        Check(parsed.blocks[1].content.text == L"Trailing text", "text after a table must remain visible");
        const auto invalid = ParseMarkdown(L"| A | B |\n| - | --- |\n| first | second |\n");
        Check(invalid.blocks[0].kind != MarkdownBlockKind::Table && invalid.blocks[0].content.text.find(L"first") != std::wstring::npos,
            "malformed delimiters must remain readable without dropping table-like text");
        std::wstring dense;
        for (int i = 0; i < 2300; ++i) dense += L"- item\n";
        dense += L"FINAL TRAILING CONTENT";
        const auto bounded = ParseMarkdown(dense);
        Check(bounded.usedPlainFallback && bounded.blocks.size() <= 2050,
            "pathological block counts must switch to bounded plain chunks");
        Check(bounded.blocks.back().content.text.find(L"FINAL TRAILING CONTENT") != std::wstring::npos,
            "bounded fallback must preserve final content instead of truncating the transcript");
        std::wstring longText(65500, L'x');
        longText += L"\xd83d\xde00 FINAL";
        const auto chunks = ParseMarkdown(longText);
        std::wstring joined;
        for (const auto& block : chunks.blocks) joined += block.content.text;
        Check(joined == longText && chunks.blocks.size() > 1, "large paragraphs must be chunked without losing Unicode or trailing text");
        std::wstring tricky;
        for (int index = 0; index < 5000; ++index) tricky += L"*word ";
        tricky.append(5000, L'[');
        tricky += L"] FINAL TEXT";
        const auto start = std::chrono::steady_clock::now();
        const auto boundedInline = ParseMarkdown(tricky);
        joined.clear();
        for (const auto& block : boundedInline.blocks) joined += block.content.text;
        Check(joined == tricky && std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
            "unmatched inline delimiters must use bounded search work without dropping trailing text");
        std::wstring hugeCell(30000, L'z');
        const auto oversize = ParseMarkdown(L"| Header | Other |\n| --- | --- |\n| " + hugeCell + L" | final cell |\nEND");
        joined.clear();
        for (const auto& block : oversize.blocks) joined += block.content.text;
        Check(joined.find(hugeCell) != std::wstring::npos && joined.find(L"END") != std::wstring::npos,
            "oversized table cells must fall back to complete readable text");
    }
    std::wstring Sample()
    {
        return L"# APC and DPC in Windows\n\n"
            L"**APCs** and **DPCs** are mechanisms for deferring work. They differ in *where* and at what privilege level they execute.\n\n"
            L"| Feature | APC | DPC |\n| :--- | :--- | :--- |\n"
            L"| Full name | Asynchronous Procedure Call | Deferred Procedure Call |\n"
            L"| Typical IRQL | `APC_LEVEL` | `DISPATCH_LEVEL` |\n"
            L"| Execution context | Context of a specific thread | Interrupt context on an available CPU |\n"
            L"| Scheduling | Queued until a thread reaches a safe point | Runs soon after interrupt processing |\n\n"
            L"## APC: Asynchronous Procedure Call\n\n"
            L"An APC is queued for execution in the context of a particular thread.\n\n"
            L"1. Check the thread context.\n2. Keep callbacks short.\n   - Avoid blocking work.\n\n"
            L"> **Note:** Kernel mode and user mode APCs have different delivery requirements.\n\n"
            L"```cpp\n// Literal Markdown: **bold** | text\nvoid WorkItem() {\n    ProcessQueuedWork();\n}\n```\n\n"
            L"---\n\nUnicode: \x03a9 \x4e2d \xd83d\xde00. Inline `code`, *emphasis*, and **strong text**.\n";
    }
    size_t CountTextPixels(const std::wstring& path)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "PNG decoder must initialize");
        Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)), "Markdown PNG must decode");
        Check(SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)), "Markdown PNG frame must decode");
        Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom)), "Markdown PNG pixels must decode");
        UINT width = 0, height = 0;
        converter->GetSize(&width, &height);
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        Check(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "Markdown rendered pixels must be readable");
        size_t count = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] > 100 && pixels[i + 1] > 100 && pixels[i + 2] > 100) ++count;
        return count;
    }
    void TestNativeLayoutAndRendering()
    {
        Direct2DContext context;
        Check(context.Initialize(nullptr), "native DirectWrite context must initialize without a browser or window");
        const auto source = Sample();
        const auto wide = MeasureMarkdown(context, source, 820);
        const auto narrow = MeasureMarkdown(context, source, 310);
        Check(wide > 350 && narrow > wide && std::isfinite(narrow), "table cells and paragraphs must wrap into a taller narrow layout");
        Check(MeasureMarkdown(context, source, 820) == wide, "returning to the same width must preserve exact measured geometry");
        Check(MeasureMarkdown(context, L"", 500) == 0 && MeasureMarkdown(context, source, 0) == 0 &&
            MeasureMarkdown(context, source, std::numeric_limits<float>::quiet_NaN()) == 0,
            "empty text and invalid widths must not create bogus conversation height");
        context.SetDpi(192);
        Check(MeasureMarkdown(context, source, 820) == wide, "Markdown layout must stay in device-independent units at high DPI");
        context.SetDpi(96);
        std::wstring mutableSource = L"one line";
        const auto shortHeight = MeasureMarkdown(context, mutableSource, 100);
        mutableSource.assign(L"many words that wrap into several lines for this same object");
        Check(MeasureMarkdown(context, mutableSource, 100) > shortHeight, "cache keys must detect text changes in an existing string object");
        const auto wideTable = MeasureMarkdown(context, L"| A | B |\n| --- | --- |\n| brief | short |", 400);
        const auto longTable = MeasureMarkdown(context, L"| A | B |\n| --- | --- |\n| a much longer sentence that needs wrapping inside this native cell | short |", 150);
        Check(longTable > wideTable, "a wrapped table cell must expand its complete row rather than overlap the next row");
        std::vector<wchar_t> environment(32768);
        const auto environmentSize = GetEnvironmentVariableW(L"CONNECTOR_STUDIO_TEST_ROOT", environment.data(), static_cast<DWORD>(environment.size()));
        const auto root = environmentSize && environmentSize < environment.size() ?
            std::filesystem::path(std::wstring(environment.data(), environmentSize)) : std::filesystem::current_path();
        Check(std::filesystem::exists(root), "Markdown screenshots must stay in an existing test workspace");
        const auto screenshot = (root / L"markdown-render.png").wstring();
        const auto height = static_cast<UINT>(std::ceil(wide + 48));
        Check(context.SaveRenderToPng(screenshot, 868, height, [&] {
            context.FillRect(D2D1::RectF(0, 0, 868, static_cast<float>(height)), Colors::ShellBg);
            RenderMarkdown(context, source, D2D1::RectF(24, 24, 844, 24 + wide));
        }), "Markdown must render to a native PNG with its measured height");
        Check(CountTextPixels(screenshot) > 3000, "Markdown screenshot must contain real native styled text");
        const auto compact = (root / L"markdown-compact.png").wstring();
        Check(context.SaveRenderToPng(compact, 358, static_cast<UINT>(std::ceil(narrow + 48)), [&] {
            context.FillRect(D2D1::RectF(0, 0, 358, narrow + 48), Colors::ShellBg);
            RenderMarkdown(context, source, D2D1::RectF(24, 24, 334, narrow + 24));
        }), "narrow native Markdown must render its entire measured table and trailing text");
        Check(MeasureMarkdown(context, source, 820) == wide, "rendering must use the same geometry as measurement");
        const auto chromeBody = context.FontBody();
        context.SetChatFontSize(24.0f);
        const auto large = MeasureMarkdown(context, source, 820);
        Check(context.GetChatFontSize() == 24.0f && context.FontChat()->GetFontSize() == 24.0f &&
            std::abs(context.FontChatCode()->GetFontSize() - 11.0f * 24.0f / 11.5f) < 0.001f,
            "chat body and code formats must honor the selected font size");
        Check(context.FontBody() == chromeBody && context.FontBody()->GetFontSize() == 11.5f && context.FontTitle()->GetFontSize() == 13.0f,
            "chat font changes must preserve the application's chrome fonts");
        Check(large > wide * 1.5f && MeasureMarkdown(context, source, 310) > narrow,
            "changing chat font size must invalidate cached Markdown layouts and expand wrapped tables and text");
        context.SetChatFontSize(9.0f);
        context.SetChatFontSize(std::numeric_limits<float>::quiet_NaN());
        Check(context.GetChatFontSize() == 24.0f, "invalid runtime font sizes must preserve the existing chat format");
        context.SetDpi(144);
        Check(MeasureMarkdown(context, source, 820) == large, "chosen chat font geometry must remain in device-independent units at high DPI");
        context.SetDpi(96);
        const auto largeScreenshot = (root / L"markdown-font-24.png").wstring();
        Check(context.SaveRenderToPng(largeScreenshot, 868, static_cast<UINT>(std::ceil(large + 48)), [&] {
            context.FillRect(D2D1::RectF(0, 0, 868, large + 48), Colors::ShellBg);
            RenderMarkdown(context, source, D2D1::RectF(24, 24, 844, 24 + large));
        }), "largest chat font must render complete native Markdown using its remeasured height");
        context.SetChatFontSize(11.5f);
        Check(MeasureMarkdown(context, source, 820) == wide, "returning to the original font size must rebuild the original exact geometry");
        Direct2DContext startupContext;
        startupContext.SetChatFontSize(18.0f);
        Check(startupContext.Initialize(nullptr) && startupContext.FontChat()->GetFontSize() == 18.0f,
            "a persisted chat font choice must apply before native graphics initialization");

        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"ConnectorMarkdownTestWindow";
        RegisterClassW(&windowClass);
        const auto hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Markdown regression", WS_POPUP,
            0, 0, 500, 400, nullptr, nullptr, instance, nullptr);
        Check(hwnd != nullptr, "hidden native render target must initialize");
        {
            Direct2DContext windowContext;
            Check(windowContext.Initialize(hwnd), "HWND Direct2D target must initialize");
            const auto targetSwap = (root / L"markdown-target-swap.png").wstring();
            Check(windowContext.SaveRenderToPng(targetSwap, 480, 360, [&] {
                windowContext.FillRect(D2D1::RectF(0, 0, 480, 360), Colors::ShellBg);
                RenderMarkdown(windowContext, source, D2D1::RectF(10, 10, 470, 350));
            }), "cached text layouts must render on a WIC target without retaining HWND brushes");
            Check(windowContext.BeginDraw(), "HWND target must draw after a PNG target swap");
            windowContext.FillRect(D2D1::RectF(0, 0, 500, 400), Colors::ShellBg);
            RenderMarkdown(windowContext, source, D2D1::RectF(10, 10, 470, 350));
            Check(SUCCEEDED(windowContext.EndDraw()), "cached Markdown layouts must use the current target brush after a PNG swap");
        }
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, instance);
    }
}
int main()
{
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        TestInlineAndBlocks();
        TestTablesAndBounds();
        TestNativeLayoutAndRendering();
        std::cout << "Markdown tests passed (" << checks << " checks)\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Markdown tests failed after " << checks << " checks: " << error.what() << '\n';
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
}
