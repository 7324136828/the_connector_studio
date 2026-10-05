#include "MarkdownRenderer.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <list>
#include <memory>
#include <string_view>

namespace Lattice
{
    namespace
    {
        constexpr size_t MaxStructuredBlocks = 2048;
        constexpr size_t TextChunk = 16384;
        constexpr size_t MaxInlineSpans = 1024;
        constexpr size_t MaxTableColumns = 12;
        constexpr size_t MaxTableRows = 256;
        constexpr size_t MaxTableCells = 4096;
        constexpr float BlockGap = 9.0f;
        using View = std::wstring_view;

        struct Line { View text; size_t next = 0; };
        Line ReadLine(const std::wstring& source, size_t position)
        {
            const auto end = source.find(L'\n', position);
            const auto length = (end == std::wstring::npos ? source.size() : end) - position;
            View text(source.data() + position, length);
            if (!text.empty() && text.back() == L'\r') text.remove_suffix(1);
            return {text, end == std::wstring::npos ? source.size() : end + 1};
        }
        View Trim(View text)
        {
            while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
            return text;
        }
        bool IsPunctuation(wchar_t ch)
        {
            return (ch >= L'!' && ch <= L'/') || (ch >= L':' && ch <= L'@') ||
                (ch >= L'[' && ch <= L'`') || (ch >= L'{' && ch <= L'~');
        }
        bool IsWord(wchar_t ch) { return iswalnum(ch) != 0 || ch == L'_'; }
        void Append(MarkdownText& output, View text, unsigned style)
        {
            if (text.empty()) return;
            const auto start = static_cast<uint32_t>(output.text.size());
            output.text.append(text);
            if (style)
            {
                if (!output.spans.empty() && output.spans.back().style == style &&
                    output.spans.back().start + output.spans.back().length == start)
                    output.spans.back().length += static_cast<uint32_t>(text.size());
                else if (output.spans.size() < MaxInlineSpans)
                    output.spans.push_back({start, static_cast<uint32_t>(text.size()), style});
            }
        }
        size_t FindUnescaped(View text, View token, size_t start, size_t& work)
        {
            for (size_t position = start; position + token.size() <= text.size(); ++position)
            {
                if (!work) return View::npos;
                --work;
                if (text.substr(position, token.size()) != token) continue;
                size_t slashCount = 0;
                for (size_t prior = position; prior && text[prior - 1] == L'\\'; --prior) ++slashCount;
                if (!(slashCount % 2)) return position;
            }
            return View::npos;
        }
        size_t FindEmphasisClosing(View text, View token, size_t start, size_t& work)
        {
            // Resolve the common nested forms *italic **bold*** and **bold *italic***
            // without treating the inner strong/emphasis delimiter as the outer close.
            bool innerStrong = false;
            bool innerEmphasis = false;
            const auto marker = token.front();
            for (size_t position = start; position < text.size(); ++position)
            {
                if (!work) return View::npos;
                --work;
                if (text[position] == L'\\' && position + 1 < text.size()) { ++position; continue; }
                if (text[position] == L'`')
                {
                    const auto close = text.find(L'`', position + 1);
                    if (close != View::npos) { position = close; continue; }
                }
                if (text[position] != marker) continue;
                size_t run = 1;
                while (position + run < text.size() && text[position + run] == marker)
                {
                    if (!work) return View::npos;
                    --work;
                    ++run;
                }
                const bool canOpen = position + run < text.size() && !iswspace(text[position + run]);
                const bool canClose = position > start && !iswspace(text[position - 1]);
                if (token.size() == 1 && run == 2)
                {
                    if (!innerStrong && canOpen) innerStrong = true;
                    else if (innerStrong && canClose) innerStrong = false;
                }
                else if (token.size() == 2 && run == 1)
                {
                    if (!innerEmphasis && canOpen) innerEmphasis = true;
                    else if (innerEmphasis && canClose) innerEmphasis = false;
                }
                else if (run >= token.size() && canClose)
                {
                    if (token.size() == 1 && run >= 3 && innerStrong) return position + 2;
                    if (token.size() == 2 && run >= 3 && innerEmphasis) return position + 1;
                    return position;
                }
                position += run - 1;
            }
            return View::npos;
        }
        void ParseInlineInto(View text, MarkdownText& output, unsigned style = 0, unsigned depth = 0)
        {
            if (depth >= 6 || output.spans.size() >= MaxInlineSpans) { Append(output, text, style); return; }
            const bool hasLabelEnd = text.find(L']') != View::npos;
            size_t work = text.size() * 8 + 1024;
            size_t position = 0;
            size_t plainStart = 0;
            const auto flush = [&](size_t end) { Append(output, text.substr(plainStart, end - plainStart), style); };
            while (position < text.size())
            {
                if (!work) { flush(position); Append(output, text.substr(position), style); return; }
                if (text[position] == L'\\' && position + 1 < text.size() && IsPunctuation(text[position + 1]))
                {
                    flush(position);
                    Append(output, text.substr(position + 1, 1), style);
                    position += 2;
                    plainStart = position;
                    continue;
                }
                if (text[position] == L'`')
                {
                    size_t count = 1;
                    while (position + count < text.size() && text[position + count] == L'`') ++count;
                    const auto token = text.substr(position, count);
                    auto closing = text.find(token, position + count);
                    while (closing != View::npos && ((closing && text[closing - 1] == L'`') ||
                        (closing + count < text.size() && text[closing + count] == L'`')))
                        closing = text.find(token, closing + count);
                    if (closing != View::npos)
                    {
                        flush(position);
                        std::wstring code(text.substr(position + count, closing - position - count));
                        std::replace(code.begin(), code.end(), L'\n', L' ');
                        if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ' &&
                            code.find_first_not_of(L' ') != std::wstring::npos)
                            code = code.substr(1, code.size() - 2);
                        Append(output, code, style | MarkdownCode);
                        position = closing + count;
                        plainStart = position;
                        continue;
                    }
                    position += count;
                    continue; // Keep an unmatched backtick run literal without rescanning its suffixes.
                }
                const bool image = text[position] == L'!' && position + 1 < text.size() && text[position + 1] == L'[';
                if (hasLabelEnd && (text[position] == L'[' || image))
                {
                    const auto labelStart = position + (image ? 2 : 1);
                    const auto labelEnd = FindUnescaped(text, L"]", labelStart, work);
                    if (labelEnd != View::npos && labelEnd + 1 < text.size() && text[labelEnd + 1] == L'(')
                    {
                        // Parentheses inside a URL are balanced; destinations remain data only.
                        size_t end = labelEnd + 2;
                        unsigned nesting = 1;
                        for (; end < text.size() && nesting; ++end)
                        {
                            if (text[end] == L'\\' && end + 1 < text.size()) { ++end; continue; }
                            if (text[end] == L'(') ++nesting;
                            else if (text[end] == L')') --nesting;
                        }
                        if (!nesting)
                        {
                            flush(position);
                            if (image) Append(output, L"[Image: ", style);
                            ParseInlineInto(text.substr(labelStart, labelEnd - labelStart), output,
                                style | (image ? 0 : MarkdownLink), depth + 1);
                            if (image) Append(output, L"]", style);
                            position = end;
                            plainStart = position;
                            continue;
                        }
                    }
                }
                View token;
                unsigned added = 0;
                if (text.substr(position, 3) == L"***" || text.substr(position, 3) == L"___") { token = text.substr(position, 3); added = MarkdownBold | MarkdownItalic; }
                else if (text.substr(position, 2) == L"**" || text.substr(position, 2) == L"__") { token = text.substr(position, 2); added = MarkdownBold; }
                else if (text.substr(position, 2) == L"~~") { token = text.substr(position, 2); added = MarkdownStrike; }
                else if (text[position] == L'*' || text[position] == L'_') { token = text.substr(position, 1); added = MarkdownItalic; }
                if (!token.empty() && position + token.size() < text.size() && !iswspace(text[position + token.size()]) &&
                    !(token.front() == L'_' && position && IsWord(text[position - 1])))
                {
                    auto closing = FindEmphasisClosing(text, token, position + token.size(), work);
                    while (closing != View::npos && (closing == position + token.size() || iswspace(text[closing - 1]) ||
                        (token.front() == L'_' && closing + token.size() < text.size() && IsWord(text[closing + token.size()]))))
                        closing = FindEmphasisClosing(text, token, closing + token.size(), work);
                    if (closing != View::npos)
                    {
                        flush(position);
                        ParseInlineInto(text.substr(position + token.size(), closing - position - token.size()), output, style | added, depth + 1);
                        position = closing + token.size();
                        plainStart = position;
                        continue;
                    }
                }
                ++position;
            }
            flush(text.size());
        }
        MarkdownText Inline(View text)
        {
            MarkdownText output;
            output.text.reserve(text.size());
            ParseInlineInto(text, output);
            return output;
        }
        bool Fence(View line, wchar_t& marker, size_t& length, View& language)
        {
            line = Trim(line);
            if (line.empty() || (line.front() != L'`' && line.front() != L'~')) return false;
            marker = line.front();
            length = 0;
            while (length < line.size() && line[length] == marker) ++length;
            if (length < 3) return false;
            language = Trim(line.substr(length));
            return marker != L'`' || language.find(L'`') == View::npos;
        }
        unsigned Heading(View line, View& body)
        {
            line = Trim(line);
            unsigned level = 0;
            while (level < line.size() && level < 6 && line[level] == L'#') ++level;
            if (!level || (level < line.size() && line[level] != L' ' && line[level] != L'\t')) return 0;
            body = Trim(line.substr(level));
            auto contentEnd = body.size();
            while (contentEnd && body[contentEnd - 1] == L'#') --contentEnd;
            if (contentEnd < body.size() && (!contentEnd || iswspace(body[contentEnd - 1]))) body = Trim(body.substr(0, contentEnd));
            return level;
        }
        bool Rule(View line)
        {
            line = Trim(line);
            if (line.empty() || (line.front() != L'*' && line.front() != L'-' && line.front() != L'_')) return false;
            const auto marker = line.front();
            size_t count = 0;
            for (auto ch : line) { if (ch == marker) ++count; else if (ch != L' ' && ch != L'\t') return false; }
            return count >= 3;
        }
        unsigned Setext(View line)
        {
            line = Trim(line);
            if (line.empty() || (line.front() != L'=' && line.front() != L'-')) return 0;
            const auto marker = line.front();
            for (auto ch : line) if (ch != marker) return 0;
            return marker == L'=' ? 1 : 2;
        }
        bool List(View line, std::wstring& marker, unsigned& indent, View& body)
        {
            size_t start = 0;
            while (start < line.size() && (line[start] == L' ' || line[start] == L'\t')) ++start;
            indent = static_cast<unsigned>((std::min)(size_t(8), start / 2));
            auto trimmed = line.substr(start);
            if (trimmed.size() >= 2 && (trimmed[0] == L'-' || trimmed[0] == L'*' || trimmed[0] == L'+') && iswspace(trimmed[1]))
            {
                marker = L"\x2022";
                body = Trim(trimmed.substr(2));
                return true;
            }
            size_t digits = 0;
            while (digits < trimmed.size() && digits < 9 && trimmed[digits] >= L'0' && trimmed[digits] <= L'9') ++digits;
            if (digits && digits + 1 < trimmed.size() && (trimmed[digits] == L'.' || trimmed[digits] == L')') && iswspace(trimmed[digits + 1]))
            {
                marker = std::wstring(trimmed.substr(0, digits)) + L".";
                body = Trim(trimmed.substr(digits + 2));
                return true;
            }
            return false;
        }
        bool Quote(View line, View& body)
        {
            line = Trim(line);
            if (line.empty() || line.front() != L'>') return false;
            line.remove_prefix(1);
            if (!line.empty() && line.front() == L' ') line.remove_prefix(1);
            body = line;
            return true;
        }
        bool StartsBlock(View line)
        {
            wchar_t fence{}; size_t count{}; View content;
            std::wstring marker; unsigned indent{};
            return Trim(line).empty() || Fence(line, fence, count, content) || Heading(line, content) || Rule(line) ||
                List(line, marker, indent, content) || Quote(line, content);
        }
        std::vector<View> Cells(View line)
        {
            line = Trim(line);
            if (!line.empty() && line.front() == L'|') line.remove_prefix(1);
            if (!line.empty() && line.back() == L'|' && (line.size() < 2 || line[line.size() - 2] != L'\\')) line.remove_suffix(1);
            std::vector<View> result;
            size_t start = 0;
            size_t codeRun = 0;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == L'\\' && i + 1 < line.size()) { ++i; continue; }
                if (line[i] == L'`')
                {
                    size_t run = 1;
                    while (i + run < line.size() && line[i + run] == L'`') ++run;
                    if (!codeRun) codeRun = run; else if (codeRun == run) codeRun = 0;
                    i += run - 1;
                }
                else if (line[i] == L'|' && !codeRun)
                {
                    result.push_back(Trim(line.substr(start, i - start)));
                    start = i + 1;
                    if (result.size() > MaxTableColumns) return {};
                }
            }
            result.push_back(Trim(line.substr(start)));
            return result.size() <= MaxTableColumns ? result : std::vector<View>{};
        }
        std::vector<MarkdownAlignment> TableDelimiter(View line)
        {
            if (line.find(L'|') == View::npos) return {};
            const auto cells = Cells(line);
            std::vector<MarkdownAlignment> alignment;
            for (auto cell : cells)
            {
                const bool left = !cell.empty() && cell.front() == L':';
                const bool right = !cell.empty() && cell.back() == L':';
                if (left) cell.remove_prefix(1);
                if (right && !cell.empty()) cell.remove_suffix(1);
                if (cell.size() < 3) return {};
                for (auto ch : cell) if (ch != L'-') return {};
                alignment.push_back(left && right ? MarkdownAlignment::Center : right ? MarkdownAlignment::Right : MarkdownAlignment::Left);
            }
            return alignment;
        }
        size_t ChunkEnd(View text, size_t position)
        {
            auto end = (std::min)(position + TextChunk, text.size());
            if (end < text.size() && end > position && text[end - 1] >= 0xd800 && text[end - 1] <= 0xdbff) --end;
            return end;
        }
        void AppendBlock(MarkdownDocument& document, MarkdownBlock block, View text, bool literal = false)
        {
            // Chunk exceptionally large paragraphs/code without truncating their trailing content.
            if (text.empty())
            {
                block.content.text.clear();
                document.blocks.push_back(std::move(block));
                return;
            }
            for (size_t position = 0; position < text.size();)
            {
                const auto end = ChunkEnd(text, position);
                auto part = block;
                const auto chunk = text.substr(position, end - position);
                if (literal) part.content.text.assign(chunk); else part.content = Inline(chunk);
                document.blocks.push_back(std::move(part));
                position = end;
            }
        }

        struct CellLayout
        {
            ComPtr<IDWriteTextLayout> text;
            float x = 0;
            float y = 0;
            float width = 0;
            float height = 0;
        };
        struct BlockLayout
        {
            MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
            float y = 0;
            float height = 0;
            float contentX = 0;
            ComPtr<IDWriteTextLayout> text;
            ComPtr<IDWriteTextLayout> marker;
            std::vector<CellLayout> cells;
            std::vector<float> rowEdges;
            std::vector<float> columnEdges;
        };
        struct Layout
        {
            std::vector<BlockLayout> blocks;
            float height = 0;
        };
        struct CacheEntry
        {
            std::wstring source;
            MarkdownDocument document;
            ComPtr<IDWriteFactory> factory;
            float width = 0;
            float fontSize = 0;
            Layout layout;
        };
        thread_local std::list<CacheEntry> cache;

        ComPtr<IDWriteTextLayout> TextLayout(Direct2DContext& context, const MarkdownText& text, float width,
            MarkdownBlockKind kind, unsigned headingLevel = 0, MarkdownAlignment alignment = MarkdownAlignment::Left, bool tableHeader = false)
        {
            ComPtr<IDWriteTextLayout> layout;
            if (!context.GetDWriteFactory()) return layout;
            auto format = kind == MarkdownBlockKind::Code ? context.FontChatCode() : context.FontChat();
            const float scale = context.GetChatFontSize() / 11.5f;
            if (!format) return layout;
            if (FAILED(context.GetDWriteFactory()->CreateTextLayout(text.text.c_str(), static_cast<UINT32>(text.text.size()),
                format, (std::max)(1.0f, width), 1000000.0f, layout.GetAddressOf()))) return {};
            layout->SetTextAlignment(alignment == MarkdownAlignment::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING :
                alignment == MarkdownAlignment::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.text.size())};
            if (kind == MarkdownBlockKind::Heading)
            {
                const float size = (headingLevel == 1 ? 20.0f : headingLevel == 2 ? 17.0f : headingLevel == 3 ? 14.5f : 12.5f) * scale;
                layout->SetFontSize(size, all);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
                layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.45f, size * 1.08f);
            }
            else if (kind == MarkdownBlockKind::Code)
                layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f * scale, 13.0f * scale);
            if (tableHeader) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
            for (const auto& span : text.spans)
            {
                const DWRITE_TEXT_RANGE range{span.start, span.length};
                if (span.style & MarkdownBold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                if (span.style & MarkdownItalic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                if (span.style & MarkdownCode) { layout->SetFontFamilyName(L"Consolas", range); layout->SetFontSize(11.0f * scale, range); }
                if (span.style & MarkdownStrike) layout->SetStrikethrough(TRUE, range);
                if (span.style & MarkdownLink) layout->SetUnderline(TRUE, range);
            }
            return layout;
        }
        float TextHeight(IDWriteTextLayout* layout, float scale)
        {
            DWRITE_TEXT_METRICS metrics{};
            return layout && SUCCEEDED(layout->GetMetrics(&metrics)) ? (std::max)(18.5f * scale, metrics.height) : 18.5f * scale;
        }
        Layout BuildLayout(Direct2DContext& context, const MarkdownDocument& document, float width)
        {
            Layout output;
            const float scale = context.GetChatFontSize() / 11.5f;
            for (const auto& block : document.blocks)
            {
                BlockLayout placed;
                placed.kind = block.kind;
                placed.y = output.height;
                if (block.kind == MarkdownBlockKind::Rule) placed.height = 12.0f * scale;
                else if (block.kind == MarkdownBlockKind::Table)
                {
                    const auto columns = block.columns.size();
                    std::vector<float> weights(columns, 80.0f * scale);
                    for (size_t column = 0; column < columns; ++column)
                    {
                        for (size_t row = 0; row < (std::min)(block.rows.size(), size_t(16)); ++row)
                        {
                            const auto& cell = block.rows[row][column];
                            // A long unbroken cell does not monopolize the table width.
                            const auto sample = cell.text.substr(0, 160);
                            weights[column] = (std::max)(weights[column], (std::min)(240.0f * scale, context.MeasureTextWidth(sample, context.FontChat()) + 20.0f * scale));
                        }
                    }
                    float sum = 0;
                    for (const auto weight : weights) sum += weight;
                    placed.columnEdges.push_back(0);
                    for (const auto weight : weights) placed.columnEdges.push_back(placed.columnEdges.back() + width * weight / sum);
                    placed.columnEdges.back() = width;
                    placed.rowEdges.push_back(0);
                    for (size_t row = 0; row < block.rows.size(); ++row)
                    {
                        float height = 0;
                        for (size_t column = 0; column < columns; ++column)
                        {
                            CellLayout cell;
                            cell.x = placed.columnEdges[column] + 9.0f * scale;
                            cell.y = placed.rowEdges.back() + 8.0f * scale;
                            cell.width = (std::max)(1.0f, placed.columnEdges[column + 1] - placed.columnEdges[column] - 18.0f * scale);
                            cell.text = TextLayout(context, block.rows[row][column], cell.width, MarkdownBlockKind::Paragraph, 0, block.columns[column], row == 0);
                            cell.height = TextHeight(cell.text.Get(), scale);
                            height = (std::max)(height, cell.height);
                            placed.cells.push_back(std::move(cell));
                        }
                        placed.rowEdges.push_back(placed.rowEdges.back() + height + 16.0f * scale);
                    }
                    placed.height = placed.rowEdges.back();
                }
                else
                {
                    float padding = 0;
                    if (block.kind == MarkdownBlockKind::Code) { placed.contentX = 12.0f * scale; padding = 12.0f * scale; }
                    else if (block.kind == MarkdownBlockKind::Quote) { placed.contentX = 14.0f * scale; padding = 6.0f * scale; }
                    else if (block.kind == MarkdownBlockKind::ListItem)
                    {
                        placed.contentX = (22.0f + block.indent * 14.0f) * scale;
                        placed.contentX = (std::min)(placed.contentX, width * 0.4f);
                        MarkdownText marker; marker.text = block.marker;
                        placed.marker = TextLayout(context, marker, (std::max)(1.0f, placed.contentX - 5.0f * scale), MarkdownBlockKind::Paragraph, 0, MarkdownAlignment::Right);
                    }
                    const auto textWidth = (std::max)(1.0f, width - placed.contentX - padding);
                    placed.text = TextLayout(context, block.content, textWidth, block.kind, block.headingLevel);
                    placed.height = TextHeight(placed.text.Get(), scale) + (block.kind == MarkdownBlockKind::Code ? 20.0f * scale : 0.0f);
                }
                output.height += placed.height + (block.kind == MarkdownBlockKind::ListItem ? 3.0f : BlockGap) * scale;
                output.blocks.push_back(std::move(placed));
            }
            if (!output.blocks.empty()) output.height -= (document.blocks.back().kind == MarkdownBlockKind::ListItem ? 3.0f : BlockGap) * scale;
            return output;
        }
        CacheEntry& GetLayout(Direct2DContext& context, const std::wstring& source, float width)
        {
            width = (std::max)(1.0f, width);
            auto found = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& entry) { return entry.source == source; });
            if (found == cache.end())
            {
                CacheEntry entry;
                entry.source = source;
                entry.document = ParseMarkdown(source);
                cache.push_front(std::move(entry));
            }
            else cache.splice(cache.begin(), cache, found);
            auto& entry = cache.front();
            if (entry.factory.Get() != context.GetDWriteFactory() || entry.width != width || entry.fontSize != context.GetChatFontSize())
            {
                entry.factory = context.GetDWriteFactory();
                entry.width = width;
                entry.fontSize = context.GetChatFontSize();
                entry.layout = BuildLayout(context, entry.document, width);
            }
            // Keep parse/layout caches bounded across long conversations and project changes.
            size_t characters = 0;
            size_t count = 0;
            auto remove = cache.end();
            for (auto it = cache.begin(); it != cache.end(); ++it)
            {
                characters += it->source.size();
                if (++count > 512 || (count > 1 && characters > 8 * 1024 * 1024)) { remove = it; break; }
            }
            if (remove != cache.end()) cache.erase(remove, cache.end());
            return entry;
        }
    }

    MarkdownDocument ParseMarkdown(const std::wstring& source)
    {
        MarkdownDocument document;
        size_t position = 0;
        size_t tableCells = 0;
        while (position < source.size())
        {
            if (document.blocks.size() >= MaxStructuredBlocks || tableCells >= MaxTableCells)
            {
                document.usedPlainFallback = true;
                MarkdownBlock fallback;
                AppendBlock(document, std::move(fallback), View(source).substr(position), true);
                break;
            }
            const auto line = ReadLine(source, position);
            if (Trim(line.text).empty()) { position = line.next; continue; }
            wchar_t fenceMarker{}; size_t fenceLength{}; View language;
            if (Fence(line.text, fenceMarker, fenceLength, language))
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Code;
                block.language.assign(language.substr(0, 80));
                std::wstring code;
                auto next = line.next;
                bool first = true;
                while (next < source.size())
                {
                    const auto codeLine = ReadLine(source, next);
                    wchar_t closingMarker{}; size_t closingLength{}; View remainder;
                    if (Fence(codeLine.text, closingMarker, closingLength, remainder) && closingMarker == fenceMarker && closingLength >= fenceLength && remainder.empty())
                    { next = codeLine.next; break; }
                    if (!first) code.push_back(L'\n');
                    code.append(codeLine.text);
                    first = false;
                    next = codeLine.next;
                }
                AppendBlock(document, std::move(block), code, true);
                position = next;
                continue;
            }
            View body;
            const auto headingLevel = Heading(line.text, body);
            if (headingLevel)
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Heading;
                block.headingLevel = headingLevel;
                AppendBlock(document, std::move(block), body);
                position = line.next;
                continue;
            }
            if (line.next < source.size())
            {
                const auto following = ReadLine(source, line.next);
                const auto level = Setext(following.text);
                if (level)
                {
                    MarkdownBlock block;
                    block.kind = MarkdownBlockKind::Heading;
                    block.headingLevel = level;
                    AppendBlock(document, std::move(block), Trim(line.text));
                    position = following.next;
                    continue;
                }
                const auto alignment = TableDelimiter(following.text);
                const auto header = Cells(line.text);
                const auto boundedCells = [](const std::vector<View>& cells) {
                    return std::all_of(cells.begin(), cells.end(), [](View cell) { return cell.size() <= TextChunk; });
                };
                if (!alignment.empty() && header.size() == alignment.size() && line.text.find(L'|') != View::npos && boundedCells(header))
                {
                    MarkdownBlock block;
                    block.kind = MarkdownBlockKind::Table;
                    block.columns = alignment;
                    const auto addRow = [&](const std::vector<View>& cells) {
                        std::vector<MarkdownText> row;
                        for (const auto cell : cells) row.push_back(Inline(cell));
                        block.rows.push_back(std::move(row));
                    };
                    addRow(header);
                    position = following.next;
                    while (position < source.size() && block.rows.size() < MaxTableRows &&
                        tableCells + (block.rows.size() + 1) * alignment.size() <= MaxTableCells)
                    {
                        const auto row = ReadLine(source, position);
                        if (row.text.find(L'|') == View::npos || Trim(row.text).empty()) break;
                        const auto cells = Cells(row.text);
                        if (cells.size() != alignment.size() || !boundedCells(cells)) break;
                        addRow(cells);
                        position = row.next;
                    }
                    tableCells += block.rows.size() * alignment.size();
                    document.blocks.push_back(std::move(block));
                    continue;
                }
            }
            if (Rule(line.text))
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Rule;
                document.blocks.push_back(std::move(block));
                position = line.next;
                continue;
            }
            std::wstring marker; unsigned indent{};
            if (List(line.text, marker, indent, body))
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::ListItem;
                block.marker = marker;
                block.indent = indent;
                std::wstring item(body);
                position = line.next;
                // Preserve indented continuation lines under their original list marker.
                while (position < source.size())
                {
                    const auto continuation = ReadLine(source, position);
                    if (StartsBlock(continuation.text) || continuation.text.empty() ||
                        (continuation.text.front() != L' ' && continuation.text.front() != L'\t')) break;
                    item.push_back(L'\n');
                    item.append(Trim(continuation.text));
                    position = continuation.next;
                }
                AppendBlock(document, std::move(block), item);
                continue;
            }
            if (Quote(line.text, body))
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Quote;
                std::wstring quote(body);
                position = line.next;
                while (position < source.size())
                {
                    const auto continuation = ReadLine(source, position);
                    if (!Quote(continuation.text, body)) break;
                    quote.push_back(L'\n');
                    quote.append(body);
                    position = continuation.next;
                }
                AppendBlock(document, std::move(block), quote);
                continue;
            }
            std::wstring paragraph(line.text);
            position = line.next;
            while (position < source.size())
            {
                const auto continuation = ReadLine(source, position);
                if (StartsBlock(continuation.text)) break;
                // Do not consume the header of a following table/Setext heading.
                if (continuation.next < source.size())
                {
                    const auto following = ReadLine(source, continuation.next);
                    if (Setext(following.text) || !TableDelimiter(following.text).empty()) break;
                }
                paragraph.push_back(L'\n');
                paragraph.append(continuation.text);
                position = continuation.next;
                if (paragraph.size() >= TextChunk) break;
            }
            MarkdownBlock block;
            AppendBlock(document, std::move(block), paragraph);
        }
        return document;
    }

    float MeasureMarkdown(Direct2DContext& context, const std::wstring& source, float width)
    {
        if (source.empty() || !std::isfinite(width) || width <= 0 || !context.GetDWriteFactory()) return 0;
        return GetLayout(context, source, width).layout.height;
    }

    void RenderMarkdown(Direct2DContext& context, const std::wstring& source,
        const D2D1_RECT_F& bounds, D2D1_COLOR_F color)
    {
        const auto width = bounds.right - bounds.left;
        if (source.empty() || !std::isfinite(width) || width <= 0 || bounds.bottom <= bounds.top || !context.GetRenderTarget()) return;
        const auto& entry = GetLayout(context, source, width);
        auto target = context.GetRenderTarget();
        context.PushClip(bounds);
        for (const auto& block : entry.layout.blocks)
        {
            const auto y = bounds.top + block.y;
            if (y >= bounds.bottom || y + block.height < bounds.top) continue;
            const auto drawText = [&](IDWriteTextLayout* text, float x, float top, D2D1_COLOR_F textColor) {
                if (text) target->DrawTextLayout(D2D1::Point2F(x, top), text, context.GetSolidBrush(textColor), D2D1_DRAW_TEXT_OPTIONS_NONE);
            };
            if (block.kind == MarkdownBlockKind::Rule)
                context.DrawLine(D2D1::Point2F(bounds.left, y + 6), D2D1::Point2F(bounds.right, y + 6), Colors::PlanCardDivider);
            else if (block.kind == MarkdownBlockKind::Table)
            {
                context.FillRect(D2D1::RectF(bounds.left, y, bounds.right, y + block.height), Colors::PlanCardBg);
                if (block.rowEdges.size() > 1)
                    context.FillRect(D2D1::RectF(bounds.left, y, bounds.right, y + block.rowEdges[1]), Colors::ComposerBg);
                context.DrawRect(D2D1::RectF(bounds.left, y, bounds.right, y + block.height), Colors::PlanCardBorder);
                for (size_t row = 1; row + 1 < block.rowEdges.size(); ++row)
                    context.DrawLine(D2D1::Point2F(bounds.left, y + block.rowEdges[row]), D2D1::Point2F(bounds.right, y + block.rowEdges[row]), Colors::PlanCardBorder);
                for (size_t column = 1; column + 1 < block.columnEdges.size(); ++column)
                    context.DrawLine(D2D1::Point2F(bounds.left + block.columnEdges[column], y), D2D1::Point2F(bounds.left + block.columnEdges[column], y + block.height), Colors::PlanCardBorder);
                for (const auto& cell : block.cells)
                {
                    context.PushClip(D2D1::RectF(bounds.left + cell.x, y + cell.y, bounds.left + cell.x + cell.width, y + cell.y + cell.height));
                    drawText(cell.text.Get(), bounds.left + cell.x, y + cell.y, color);
                    context.PopClip();
                }
            }
            else
            {
                float insetY = 0;
                if (block.kind == MarkdownBlockKind::Code)
                {
                    context.FillRoundedRect(D2D1::RectF(bounds.left, y, bounds.right, y + block.height), 5.0f, Colors::PlanCardBg);
                    context.DrawRoundedRect(D2D1::RectF(bounds.left, y, bounds.right, y + block.height), 5.0f, Colors::PlanCardBorder);
                    insetY = 10.0f * context.GetChatFontSize() / 11.5f;
                }
                else if (block.kind == MarkdownBlockKind::Quote)
                    context.FillRect(D2D1::RectF(bounds.left + 1, y, bounds.left + 3, y + block.height), Colors::Eyebrow);
                drawText(block.marker.Get(), bounds.left, y, Colors::TextMuted);
                drawText(block.text.Get(), bounds.left + block.contentX, y + insetY,
                    block.kind == MarkdownBlockKind::Heading ? Colors::TextHeading : color);
            }
        }
        context.PopClip();
    }
}
