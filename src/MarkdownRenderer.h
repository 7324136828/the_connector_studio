#pragma once
#include "Direct2DContext.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Lattice
{
    enum class MarkdownBlockKind { Paragraph, Heading, ListItem, Quote, Code, Table, Rule };
    enum class MarkdownAlignment { Left, Center, Right };
    enum MarkdownStyle : unsigned { MarkdownBold = 1, MarkdownItalic = 2, MarkdownCode = 4, MarkdownStrike = 8, MarkdownLink = 16 };

    struct MarkdownSpan
    {
        uint32_t start = 0;
        uint32_t length = 0;
        unsigned style = 0;
    };
    struct MarkdownText
    {
        std::wstring text;
        std::vector<MarkdownSpan> spans;
    };
    struct MarkdownBlock
    {
        MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
        MarkdownText content;
        std::wstring marker;
        std::wstring language;
        unsigned headingLevel = 0;
        unsigned indent = 0;
        std::vector<std::vector<MarkdownText>> rows;
        std::vector<MarkdownAlignment> columns;
    };
    struct MarkdownDocument
    {
        std::vector<MarkdownBlock> blocks;
        bool usedPlainFallback = false;
    };

    // Parsing never changes the source transcript. Unsupported syntax stays readable.
    // Bounded blocks/cells/spans prevent pathological Markdown from exhausting native layouts.
    MarkdownDocument ParseMarkdown(const std::wstring& source);

    // Measurement and drawing share cached DirectWrite layouts at the exact supplied width.
    // No HTML, external images, browser engine, or link execution is involved.
    float MeasureMarkdown(Direct2DContext& context, const std::wstring& source, float width);
    void RenderMarkdown(Direct2DContext& context, const std::wstring& source,
        const D2D1_RECT_F& bounds, D2D1_COLOR_F color = Colors::TextSub);
}
