#include "app/data_preview.hpp"
#include "tui/markdown_view.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr int kJsonIndentCells = 2;

// Ayrıştırma app::parse_csv'dedir (GUI ile ortak); burada yalnız görüntü: hücre arındırılır, başlık kalın.
TableData table_of(app::CsvTable csv, std::string_view source) {
    const SourceLines lines(source);
    TableData table;
    table.rows.reserve(csv.rows.size());
    for (const std::vector<std::string>& cells : csv.rows) {
        TextLook look;
        look.bold = table.rows.empty();
        std::vector<InlineText>& row = table.rows.emplace_back();
        for (const std::string& cell : cells)
            row.emplace_back().append(display_safe(cell), look);
    }
    for (const std::size_t offset : csv.row_offsets)
        table.source_lines.push_back(lines.line_of(offset));
    table.header_rows = table.rows.empty() ? 0 : 1;
    table.omitted_rows = csv.omitted_rows;
    table.column_count = csv.column_count;
    return table;
}

}  // namespace

std::optional<RenderedView> render_csv(std::string_view source, int width, CellWidthFn cell_width,
                                       const ViewLimits& limits) {
    std::optional<app::CsvTable> csv =
        app::parse_csv(source, {.rows = limits.table_rows, .columns = limits.table_columns});
    if (!csv)
        return std::nullopt;
    ViewBuilder view(width, cell_width, limits.lines);
    add_table(view, table_of(std::move(*csv), source), {});
    return std::move(view).finish();
}

// Girinti genişliğin yarısını geçmez: derin iç içelikte içerik görünür kalır.
std::optional<RenderedView> render_json(std::string_view source, int width, CellWidthFn cell_width,
                                        const ViewLimits& limits) {
    const std::optional<std::vector<app::JsonLine>> json = app::pretty_json(source);
    if (!json)
        return std::nullopt;
    const SourceLines lines(source);
    ViewBuilder view(width, cell_width, limits.lines);
    for (const app::JsonLine& line : *json) {
        const int cells = std::min(static_cast<int>(line.depth) * kJsonIndentCells, view.width() / 2);
        InlineText indent;
        indent.append(std::string(static_cast<std::size_t>(cells), ' '));
        SyntaxState state = kInitialSyntaxState;
        view.add(indent, indent,
                 highlighted_line(SyntaxLanguage::Json, display_safe(line.text), state, {},
                                  limits.highlight_line_bytes),
                 lines.line_of(line.offset));
    }
    return std::move(view).finish();
}

}  // namespace noteapp::tui
