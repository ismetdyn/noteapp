#include "app/data_preview.hpp"
#include "tui/markdown_view.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr int kJsonIndentCells = 2;

// Ayrıştırma app::index_csv/csv_rows'tadır (GUI ile ortak); burada yalnız görüntü: hücre arındırılır, başlık kalın.
TableData table_of(std::string_view source, const app::CsvIndex& index, const SourceLines& lines,
                   std::size_t first, std::size_t count, std::size_t max_columns) {
    TableData table;
    table.rows.reserve(count);
    std::size_t row = first;
    for (const app::CsvRow& csv : app::csv_rows(source, index, first, count, max_columns)) {
        TextLook look;
        look.bold = row++ == 0;
        std::vector<InlineText>& cells = table.rows.emplace_back();
        for (const std::string& cell : csv.cells)
            cells.emplace_back().append(display_safe(cell), look);
        table.source_lines.push_back(lines.line_of(csv.offset));
    }
    table.header_rows = first == 0 && count > 0 ? 1 : 0;
    table.column_count = index.column_count;
    return table;
}

}  // namespace

RenderedView render_csv_rows(std::string_view source, const app::CsvIndex& index, const SourceLines& lines,
                             std::size_t first, std::size_t count, bool last, TableColumns& columns, int width,
                             CellWidthFn cell_width, const ViewLimits& limits) {
    const std::size_t shown = std::min(index.row_count, limits.table_rows);
    first = std::min(first, shown);
    count = std::min(count, shown - first);
    TableData table = table_of(source, index, lines, first, count, limits.table_columns);
    table.omitted_rows = last ? index.row_count - shown : 0;
    ViewBuilder view(width, cell_width, limits.lines);
    if (columns.widths.empty())
        columns = table_columns(view, table, {});
    TableColumns drawn = columns;
    drawn.hidden = last ? columns.hidden : 0;
    add_table(view, table, {}, drawn);
    return std::move(view).finish();
}

std::optional<RenderedView> render_csv(std::string_view source, int width, CellWidthFn cell_width,
                                       const ViewLimits& limits) {
    const std::optional<app::CsvIndex> index = app::index_csv(source);
    if (!index)
        return std::nullopt;
    TableColumns columns;
    return render_csv_rows(source, *index, SourceLines(source), 0, index->row_count, true, columns, width,
                           cell_width, limits);
}

// Girinti genişliğin yarısını geçmez: derin iç içelikte içerik görünür kalır.
RenderedView render_json_lines(std::string_view source, const app::JsonIndex& index, const SourceLines& lines,
                               std::size_t first, std::size_t count, int width, CellWidthFn cell_width,
                               const ViewLimits& limits) {
    ViewBuilder view(width, cell_width, limits.lines);
    for (const app::JsonLine& line : app::json_lines(source, index, first, count)) {
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

std::optional<RenderedView> render_json(std::string_view source, int width, CellWidthFn cell_width,
                                        const ViewLimits& limits) {
    const std::optional<app::JsonIndex> index = app::index_json(source);
    if (!index)
        return std::nullopt;
    return render_json_lines(source, *index, SourceLines(source), 0, index->line_count, width, cell_width, limits);
}

}  // namespace noteapp::tui
