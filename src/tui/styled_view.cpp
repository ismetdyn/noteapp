#include "tui/styled_view.hpp"

#include "core/i18n.hpp"
#include "edit/display_text.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace noteapp::tui {

namespace {

using ByteSpan = std::pair<std::size_t, std::size_t>;

constexpr int kMinContentCells = 1;
constexpr int kMinColumnCells = 3;     // daha darı sütun değil, harf yığını olurdu
constexpr int kColumnBorderCells = 3;  // " │ "
constexpr TextLook kBorderLook{.dim = true};

constexpr bool is_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Metin terminal_safe'ten geçmiştir (geçerli UTF-8); ASCII kısayolu hücre genişliği çağrısının bedelini
// önler.
std::pair<std::size_t, int> code_point_cells(std::string_view text, std::size_t at, CellWidthFn cell_width) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80)
        return {1, 1};
    std::size_t length = 1;
    while (at + length < text.size() && is_continuation(text[at + length]))
        ++length;
    constexpr unsigned kPayloadMask = 0x3F;
    constexpr int kPayloadBits = 6;
    char32_t code_point = lead >= 0xF0 ? lead & 0x07U : (lead >= 0xE0 ? lead & 0x0FU : lead & 0x1FU);
    for (std::size_t i = 1; i < length; ++i)
        code_point = (code_point << kPayloadBits) | (static_cast<unsigned char>(text[at + i]) & kPayloadMask);
    return {length, cell_width(code_point)};
}

// Açgözlü sarma: boşlukta kırar, boşluksuz sözcüğü hücrede keser. Her bayta bir kez bakar (doğrusal).
template <typename Emit>
void for_each_wrapped(std::string_view text, int width, CellWidthFn cell_width, const Emit& emit) {
    std::size_t begin = 0;
    std::size_t at = 0;
    std::optional<std::size_t> space;
    int cells = 0;
    int cells_after_space = 0;
    const auto start_line = [&](std::size_t next, int used) {
        begin = next;
        cells = used;
        space.reset();
    };
    while (at < text.size()) {
        const char byte = text[at];
        if (byte == '\n') {
            emit(begin, at);
            start_line(++at, 0);
            continue;
        }
        const auto [length, cell] = code_point_cells(text, at, cell_width);
        if (cells + cell > width && cells > 0) {
            if (byte == ' ') {
                emit(begin, at);
                start_line(++at, 0);
                continue;
            }
            if (space) {
                emit(begin, *space);
                start_line(*space + 1, cells - cells_after_space);
            } else {
                emit(begin, at);
                start_line(at, 0);
            }
        }
        if (byte == ' ' && at > begin) {
            space = at;
            cells_after_space = cells + 1;
        }
        cells += cell;
        at += length;
    }
    emit(begin, text.size());
}

// `src`nin [begin, end) dilimini `out`a ekler; `cursor` aynı kaynakta sıralı dilimlerde geri dönmez.
void append_slice(InlineText& out, const InlineText& src, ByteSpan slice, std::size_t& cursor) {
    const auto [begin, end] = slice;
    while (cursor < src.spans.size() && src.spans[cursor].end <= begin)
        ++cursor;
    std::size_t at = begin;
    for (std::size_t i = cursor; i < src.spans.size() && src.spans[i].begin < end; ++i) {
        const StyledSpan& span = src.spans[i];
        const std::size_t from = std::max(span.begin, begin);
        const std::size_t to = std::min(span.end, end);
        out.append(std::string_view(src.text).substr(at, from - at));
        out.append(std::string_view(src.text).substr(from, to - from), span.look);
        at = to;
    }
    out.append(std::string_view(src.text).substr(at, end - at));
}

std::vector<ByteSpan> wrapped_spans(const InlineText& cell, int width, CellWidthFn cell_width) {
    std::vector<ByteSpan> spans;
    for_each_wrapped(cell.text, width, cell_width,
                     [&](std::size_t begin, std::size_t end) { spans.emplace_back(begin, end); });
    return spans;
}

int longest_line_cells(std::string_view text, CellWidthFn cell_width) {
    int longest = 0;
    int cells = 0;
    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == '\n') {
            cells = 0;
            ++at;
            continue;
        }
        const auto [length, cell] = code_point_cells(text, at, cell_width);
        cells += cell;
        at += length;
        longest = std::max(longest, cells);
    }
    return longest;
}

// Sütun genişlikleri: doğal genişlik `cap`te kesilir, `cap` sığan en büyük değerdir (ikili arama, O(n log
// w)); artan hücreler kesilen sütunlara dağılır. En geniş sütunu birer azaltmak sütun*hücre işlem isterdi.
std::vector<int> fit_columns(const std::vector<int>& natural, int room) {
    const auto used = [&](int cap) {
        long total = 0;
        for (const int width : natural)
            total += std::min(width, cap);
        return total;
    };
    int low = 1;
    int high = std::max(room, 1);
    while (low < high) {
        const int middle = low + (high - low + 1) / 2;
        if (used(middle) <= room)
            low = middle;
        else
            high = middle - 1;
    }
    std::vector<int> widths;
    widths.reserve(natural.size());
    long spare = room - used(low);
    for (const int width : natural) {
        const bool capped = width > low && spare > 0;
        widths.push_back(std::max(std::min(width, low) + (capped ? 1 : 0), 1));
        spare -= capped ? 1 : 0;
    }
    return widths;
}

struct TableLayout {
    std::vector<int> widths;
    std::size_t hidden_columns = 0;
};

TableLayout layout_table(const ViewBuilder& view, const TableData& table, int room) {
    std::size_t columns = table.column_count;
    for (const auto& row : table.rows)
        columns = std::max(columns, row.size());
    const auto fitting =
        static_cast<std::size_t>(std::max((room - 1) / (kMinColumnCells + kColumnBorderCells), 1));
    const std::size_t visible = std::min(columns, fitting);
    std::vector<int> natural(visible, 1);
    for (const auto& row : table.rows)
        for (std::size_t c = 0; c < visible && c < row.size(); ++c)
            natural[c] = std::max(natural[c], longest_line_cells(row[c].text, view.cell_width()));
    const int available = room - kColumnBorderCells * static_cast<int>(visible) - 1;
    return {fit_columns(natural, available), columns - visible};
}

void add_table_row(ViewBuilder& view, const std::vector<InlineText>& row, const std::vector<int>& widths,
                   const std::vector<pdf::Align>& aligns, const InlineText& prefix, std::size_t source_line) {
    static const InlineText kEmptyCell;
    const std::size_t columns = widths.size();
    std::vector<std::vector<ByteSpan>> wrapped(columns);
    std::size_t height = 1;
    for (std::size_t c = 0; c < columns; ++c) {
        wrapped[c] = wrapped_spans(c < row.size() ? row[c] : kEmptyCell, widths[c], view.cell_width());
        height = std::max(height, wrapped[c].size());
    }
    std::vector<std::size_t> cursors(columns, 0);
    for (std::size_t line_index = 0; line_index < height; ++line_index) {
        InlineText line = prefix;
        line.append("│ ", kBorderLook);
        for (std::size_t c = 0; c < columns; ++c) {
            const InlineText& cell = c < row.size() ? row[c] : kEmptyCell;
            int spare = widths[c];
            int before = 0;
            if (line_index < wrapped[c].size()) {
                const ByteSpan slice = wrapped[c][line_index];
                spare = std::max(
                    widths[c] - view.cells(std::string_view(cell.text).substr(slice.first, slice.second - slice.first)),
                    0);
                const pdf::Align align = c < aligns.size() ? aligns[c] : pdf::Align::Default;
                before = align == pdf::Align::Right ? spare : align == pdf::Align::Center ? spare / 2 : 0;
                line.append(std::string(static_cast<std::size_t>(before), ' '));
                append_slice(line, cell, slice, cursors[c]);
            }
            line.append(std::string(static_cast<std::size_t>(spare - before), ' '));
            line.append(c + 1 < columns ? " │ " : " │", kBorderLook);
        }
        view.add_line(line, source_line);
    }
}

void add_header_rule(ViewBuilder& view, const std::vector<int>& widths, const InlineText& prefix,
                     std::size_t source_line) {
    std::string rule = "├";
    for (std::size_t c = 0; c < widths.size(); ++c) {
        for (int i = 0; i < widths[c] + 2; ++i)
            rule += "─";
        rule += c + 1 < widths.size() ? "┼" : "┤";
    }
    InlineText line = prefix;
    line.append(rule, kBorderLook);
    view.add_line(line, source_line);
}

}  // namespace

ViewLimits view_limits_of(const core::TuiLimits& limits) {
    return {.source_bytes = limits.view_bytes.cap(),
            .lines = limits.view_lines.cap(),
            .table_rows = limits.table_rows.cap(),
            .table_columns = limits.table_columns.cap(),
            .highlight_line_bytes = limits.highlight_line_bytes.cap(),
            .markdown = app::markdown_chunk_limits_of(limits)};
}

SyntaxLimits syntax_limits_of(const core::TuiLimits& limits) {
    return {.note_bytes = limits.highlight_bytes.cap(), .line_bytes = limits.highlight_line_bytes.cap()};
}

void InlineText::append(std::string_view safe, TextLook look) {
    if (safe.empty())
        return;
    const std::size_t begin = text.size();
    text += safe;
    if (look == TextLook{})
        return;
    if (!spans.empty() && spans.back().end == begin && spans.back().look == look)
        spans.back().end = text.size();
    else
        spans.push_back({begin, text.size(), look});
}

std::string display_safe(std::string_view raw) {
    std::string spaced(raw);
    std::ranges::replace_if(spaced, [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return terminal_safe(spaced);
}

SourceLines::SourceLines(std::string_view text) : text_(text) {
    starts_.push_back(0);
    for (std::size_t at = text.find('\n'); at != std::string_view::npos; at = text.find('\n', at + 1))
        starts_.push_back(at + 1);
}

std::size_t SourceLines::line_of(std::size_t offset) const {
    return static_cast<std::size_t>(std::ranges::upper_bound(starts_, offset) - starts_.begin()) - 1;
}

std::string_view SourceLines::line_text(std::size_t line) const {
    const std::size_t end = line + 1 < starts_.size() ? starts_[line + 1] : text_.size();
    return text_.substr(starts_[line], end - starts_[line]);
}

InlineText highlighted_line(SyntaxLanguage language, std::string_view line, SyntaxState& state,
                            TextLook fallback, std::size_t max_line_bytes) {
    InlineText out;
    if (language == SyntaxLanguage::Plain) {
        out.append(line, fallback);
        return out;
    }
    std::vector<SyntaxSpan> tokens;
    state = highlight_line(language, line, state, &tokens, max_line_bytes);
    std::size_t at = 0;
    const auto boundary = [&](std::size_t offset) {
        while (offset < line.size() && is_continuation(line[offset]))
            ++offset;
        return std::min(offset, line.size());
    };
    for (const SyntaxSpan& token : tokens) {
        // Belirteç sınırı UTF-8 dizisinin ortasına düşebilir; satırın parçalanması geçersiz bayt üretirdi.
        const std::size_t begin = std::max(boundary(token.begin), at);
        const std::size_t end = std::max(boundary(token.end), begin);
        out.append(line.substr(at, begin - at));
        TextLook look;
        look.token = token.kind;
        out.append(line.substr(begin, end - begin), look);
        at = end;
    }
    out.append(line.substr(at));
    return out;
}

ViewBuilder::ViewBuilder(int width, CellWidthFn cell_width, std::size_t max_lines)
    : width_(std::max(width, 1)), cell_width_(cell_width), max_lines_(max_lines) {}

int ViewBuilder::cells(std::string_view text) const {
    int total = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto [length, cell] = code_point_cells(text, at, cell_width_);
        total += cell;
        at += length;
    }
    return total;
}

std::size_t ViewBuilder::last_source_line() const {
    return view_.source_lines.empty() ? 0 : view_.source_lines.back();
}

void ViewBuilder::push(const InlineText& prefix, const InlineText& content, std::size_t begin,
                       std::size_t end, std::size_t& span_cursor, std::size_t source_line) {
    if (view_.lines.size() >= max_lines_) {
        truncated_ = true;
        return;
    }
    InlineText line = prefix;
    append_slice(line, content, {begin, end}, span_cursor);
    view_.lines.push_back(std::move(line.text));
    view_.spans.push_back(std::move(line.spans));
    view_.source_lines.push_back(std::max(source_line, last_source_line()));
}

void ViewBuilder::add(const InlineText& first, const InlineText& next, const InlineText& content,
                      std::size_t source_line) {
    const int room = std::max(width_ - cells(first.text), kMinContentCells);
    const InlineText* prefix = &first;
    std::size_t cursor = 0;
    for_each_wrapped(content.text, room, cell_width_, [&](std::size_t begin, std::size_t end) {
        push(*prefix, content, begin, end, cursor, source_line);
        prefix = &next;
    });
}

void ViewBuilder::add_line(const InlineText& line, std::size_t source_line) {
    std::size_t cursor = 0;
    push(line, InlineText{}, 0, 0, cursor, source_line);
}

void ViewBuilder::add_note(const std::string& note, const InlineText& prefix, std::size_t source_line) {
    InlineText line = prefix;
    TextLook look;
    look.dim = true;
    line.append(note, look);
    view_.lines.push_back(std::move(line.text));
    view_.spans.push_back(std::move(line.spans));
    view_.source_lines.push_back(std::max(source_line, last_source_line()));
}

RenderedView ViewBuilder::finish() && {
    if (truncated_)
        add_note(core::tr("tui.view_truncated"), {}, last_source_line());
    if (view_.lines.empty())
        add_line({}, 0);
    return std::move(view_);
}

TableColumns table_columns(const ViewBuilder& view, const TableData& table, const InlineText& prefix) {
    TableLayout layout = layout_table(view, table, view.width() - view.cells(prefix.text));
    return {std::move(layout.widths), layout.hidden_columns};
}

void add_table(ViewBuilder& view, const TableData& table, const InlineText& prefix) {
    add_table(view, table, prefix, table_columns(view, table, prefix));
}

void add_table(ViewBuilder& view, const TableData& table, const InlineText& prefix, const TableColumns& layout) {
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        add_table_row(view, table.rows[r], layout.widths, table.aligns, prefix, table.source_lines[r]);
        if (r + 1 == table.header_rows)
            add_header_rule(view, layout.widths, prefix, table.source_lines[r]);
    }
    const std::size_t last = view.last_source_line();
    if (table.omitted_rows > 0)
        view.add_note(core::tr("tui.view_rows_cut", {{"count", std::to_string(table.omitted_rows)}}), prefix,
                      last);
    if (layout.hidden > 0)
        view.add_note(core::tr("tui.view_columns_cut", {{"count", std::to_string(layout.hidden)}}), prefix, last);
}

}  // namespace noteapp::tui
