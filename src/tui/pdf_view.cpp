#include "tui/pdf_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace noteapp::tui {

namespace {

TextLook dim_look() {
    TextLook look;
    look.dim = true;
    return look;
}

// markdown_view.cpp'nin başlık görünüşüyle aynı: iki görünüm aynı görünsün.
TextLook heading_look(int level) {
    TextLook look;
    look.bold = true;
    look.token = level <= 2 ? TokenKind::Heading : TokenKind::Type;
    look.underline = level == 1;
    return look;
}

TextLook span_look(const pdf::TextStyle& style, int heading_level) {
    TextLook look = heading_level > 0 ? heading_look(heading_level) : TextLook{};
    look.bold = look.bold || style.bold;
    look.italic = style.italic;
    if (style.mono && heading_level == 0)
        look.token = TokenKind::Code;
    return look;
}

InlineText spaces(int cells) {
    InlineText prefix;
    prefix.append(std::string(static_cast<std::size_t>(std::max(cells, 0)), ' '));
    return prefix;
}

void add_dim(ViewBuilder& view, std::string_view text, std::size_t source_line) {
    InlineText line;
    line.append(display_safe(text), dim_look());
    view.add({}, {}, line, source_line);
}

}  // namespace

RenderedView render_pdf_page(std::string_view separator, const std::vector<pdf::Block>& blocks,
                             std::string_view error, int width, CellWidthFn cell_width, std::size_t max_lines) {
    ViewBuilder view(width, cell_width, max_lines);
    add_dim(view, separator, 0);
    if (!error.empty()) {
        add_dim(view, error, 0);
    } else {
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const pdf::Block& block = blocks[index];
            const std::size_t source_line = index + 1;
            if (index > 0)
                view.add_line({}, source_line);
            InlineText content;
            for (const pdf::Span& span : block.spans)
                content.append(display_safe(span.text), span_look(span.style, block.heading_level));
            if (block.kind != pdf::Block::Kind::Heading) {
                view.add({}, {}, content, source_line);
                continue;
            }
            const std::string marker = std::string(static_cast<std::size_t>(block.heading_level), '#') + " ";
            InlineText first;
            first.append(marker, dim_look());
            view.add(first, spaces(view.cells(marker)), content, source_line);
        }
    }
    view.add_line({}, blocks.size() + 1);  // sonraki sayfanın ayırıcısından önce boş satır
    return std::move(view).finish();
}

RenderedView render_pdf_note(std::string_view note, int width, CellWidthFn cell_width) {
    ViewBuilder view(width, cell_width);
    add_dim(view, note, 0);
    return std::move(view).finish();
}

int page_block_rows(float width, float height, int columns, CellPixels cell) {
    if (width <= 0.0F || height <= 0.0F || columns <= 0 || cell.width <= 0 || cell.height <= 0)
        return 1;
    const double pixels = static_cast<double>(height) / static_cast<double>(width) * columns * cell.width;
    return std::max(1, static_cast<int>(std::ceil(pixels / cell.height)));
}

std::vector<BlockCell> page_block_row(const pdfview::RenderedPage& page, int row, int columns) {
    constexpr int kChannels = 4;  // RGBA
    std::vector<BlockCell> cells(static_cast<std::size_t>(std::max(columns, 0)));
    if (page.width <= 0 || page.height <= 0 || columns <= 0)
        return cells;
    const double step = static_cast<double>(page.width) / columns;  // hedef piksel başına kaynak piksel
    // Hedef pikselin kaynak aralığı [begin, end): en az bir piksel, sayfa kenarında kırpılır.
    const auto source = [step](int target, int limit) {
        const int begin = std::min(static_cast<int>(target * step), limit);
        const int end = std::min(std::max(begin + 1, static_cast<int>((target + 1) * step)), limit);
        return std::pair{begin, end};
    };
    const auto sample = [&](int x, int y) -> std::optional<Rgb> {
        const auto [top, bottom] = source(y, page.height);
        if (top >= bottom)
            return std::nullopt;
        const auto [left, right] = source(x, page.width);
        std::uint64_t sums[3] = {};
        for (int sy = top; sy < bottom; ++sy)
            for (int sx = left; sx < right; ++sx) {
                const std::size_t at = ((static_cast<std::size_t>(sy) * static_cast<std::size_t>(page.width)) +
                                        static_cast<std::size_t>(sx)) *
                                       kChannels;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    sums[channel] += page.rgba[at + channel];
            }
        const auto count = static_cast<std::uint64_t>(bottom - top) * static_cast<std::uint64_t>(right - left);
        const auto mean = [count](std::uint64_t sum) { return static_cast<std::uint8_t>((sum + (count / 2)) / count); };
        return Rgb{mean(sums[0]), mean(sums[1]), mean(sums[2])};
    };
    for (int x = 0; x < columns; ++x)
        cells[static_cast<std::size_t>(x)] = {sample(x, 2 * row), sample(x, (2 * row) + 1)};
    return cells;
}

}  // namespace noteapp::tui
