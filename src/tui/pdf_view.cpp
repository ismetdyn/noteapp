#include "tui/pdf_view.hpp"

#include <algorithm>
#include <string>

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

}  // namespace noteapp::tui
