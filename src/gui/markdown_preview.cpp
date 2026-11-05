#include "gui/markdown_preview.hpp"

#include "gui/theme.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>

namespace noteapp::gui {

namespace {

// Aralıklar gövde yazı boyu (em) birimindedir: arayüz ölçeğiyle birlikte büyür.
constexpr float kLineSpacing = 1.3F;      // satır yüksekliği / yazı boyu
constexpr float kBlockGapEms = 0.6F;      // bloklar arası (paragraf)
constexpr float kHeadingGapEms = 1.2F;    // başlığın üstünde: bölüm ayrımı okunsun
constexpr float kListItemGapEms = 0.15F;  // aynı listenin ardışık öğeleri arası
constexpr float kListIndentEms = 1.5F;    // liste derinliği başına
constexpr float kQuoteIndentEms = 1.0F;   // alıntı derinliği başına; çubuk bu payın başında
constexpr float kQuoteBarEms = 0.2F;
constexpr float kCodePadEms = 0.75F;  // gövde 16 px'te 12 px
constexpr float kInlineCodePadEms = 0.2F;
constexpr float kCellPadEms = 0.5F;
constexpr float kLineThickness = 1.0F;  // piksel: ayraç ve tablo çizgisi ölçekle kalınlaşmaz
// h1..h6; daha derin başlık yok (md4c 6'da keser). Ölçek SPEC-gui "Önizleme" (G10'da sıkılaştı).
constexpr std::array<float, 6> kHeadingScale = {1.6F, 1.35F, 1.2F, 1.05F, 1.0F, 1.0F};
// Köşe yarıçapı, ölçek 1 pikseli: kod bloğu çocuk pencere gibi, satır içi kod çip gibi.
constexpr float kCodeBlockRounding = 6.0F;
constexpr float kInlineCodeRounding = 4.0F;

std::size_t utf8_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    constexpr unsigned char kTwo = 0xC0;
    constexpr unsigned char kThree = 0xE0;
    constexpr unsigned char kFour = 0xF0;
    return byte >= kFour ? 4 : byte >= kThree ? 3 : byte >= kTwo ? 2 : 1;
}

float measure(ImFont* font, float size, std::string_view text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
}

ImFont* font_for(const PreviewFonts& fonts, const pdf::TextStyle& style) {
    if (style.mono)
        return fonts.mono;
    if (style.bold && style.italic)
        return fonts.bold_italic;
    if (style.bold)
        return fonts.bold;
    return style.italic ? fonts.italic : fonts.regular;
}

struct Look {
    ImFont* font = nullptr;
    float size = 0.0F;
    PreviewInk ink = PreviewInk::Text;
    bool code_background = false;
    std::string_view link;
    bool wiki = false;
};

Look look_of(const PreviewFonts& fonts, const pdf::Span& span, float size, bool bold, const LinkResolves& resolves) {
    pdf::TextStyle style = span.style;
    style.bold = style.bold || bold;
    Look look{font_for(fonts, style), size,  span.link.empty() ? PreviewInk::Text : PreviewInk::Link,
              span.style.mono,        span.link, span.wiki};
    // Çözülmeyen [[hedef]] bağlantı değildir: tıklama alanı ve alt çizgisi yok (bağlantı panelindeki "Eksik").
    if (span.wiki && !resolves(span.link)) {
        look.ink = PreviewInk::Missing;
        look.link = {};
    }
    return look;
}

// Sözcükler satıra dizilir: sığmayan sözcük alt satıra iner, satırdan geniş sözcük kod noktası sınırında
// bölünür. Sarma yalnız boşlukta; satır sonu '\n'.
class Flow {
public:
    Flow(PreviewLayout& out, float left, float right, float top, float line_height)
        : out_(out), left_(left), right_(right), x_(left), y_(top), line_height_(line_height) {}

    void add(std::string_view text, const Look& look) {
        while (!text.empty()) {
            if (text.front() == '\n') {
                new_line();
                text.remove_prefix(1);
            } else if (text.front() == ' ') {
                pending_space_ = x_ > left_;
                text.remove_prefix(1);
            } else {
                const std::size_t end = std::min(text.find_first_of(" \n"), text.size());
                add_word(text.substr(0, end), look);
                text.remove_prefix(end);
            }
        }
    }

    // Son satırın altı.
    float bottom() const { return y_ + line_height_; }
    float right_edge() const { return widest_; }

private:
    void new_line() {
        y_ += line_height_;
        x_ = left_;
        pending_space_ = false;
        last_hit_ = false;
    }

    void add_word(std::string_view word, const Look& look) {
        const float width = measure(look.font, look.size, word);
        const float space = pending_space_ ? measure(look.font, look.size, " ") : 0.0F;
        pending_space_ = false;
        if (x_ + space + width > right_ && x_ > left_)
            new_line();
        else
            x_ += space;
        if (x_ + width <= right_) {
            place(word, look);
            return;
        }
        // Satırdan geniş sözcük: her parça için ölçüm satır genişliğinde durur (CalcTextSizeA max_width), kalan
        // sözcük yeniden ölçülmez. Toplam iş sözcük boyuyla doğrusal; 256 KiB'lık tek satır (base64, URL) da.
        while (!word.empty()) {
            const char* rest = nullptr;
            look.font->CalcTextSizeA(look.size, right_ - x_, 0.0F, word.data(), word.data() + word.size(), &rest);
            auto fit = static_cast<std::size_t>(rest - word.data());
            if (fit >= word.size()) {
                place(word, look);
                return;
            }
            if (fit == 0 && x_ > left_) {
                new_line();
                continue;
            }
            fit = std::max(fit, std::min(utf8_length(word.front()), word.size()));  // tek glif sığmasa da ilerle
            place(word.substr(0, fit), look);
            if (fit == word.size())
                return;
            new_line();
            word.remove_prefix(fit);
        }
    }

    void place(std::string_view text, const Look& look) {
        const float width = measure(look.font, look.size, text);
        const ImVec2 pos(x_, y_ + ((line_height_ - look.size) / 2.0F));
        out_.runs.push_back({look.font, look.size, pos, width, text, look.ink, !look.link.empty(),
                             look.code_background, y_, y_ + line_height_});
        if (!look.link.empty()) {
            // Aynı bağlantının aynı satırdaki ardışık sözcükleri (aradaki boşluk dahil) tek tıklama alanı.
            if (last_hit_ && out_.hits.back().target == look.link && out_.hits.back().min.y == y_)
                out_.hits.back().max.x = x_ + width;
            else
                out_.hits.push_back({ImVec2(x_, y_), ImVec2(x_ + width, y_ + line_height_), look.link, look.wiki});
        }
        last_hit_ = !look.link.empty();
        x_ += width;
        widest_ = std::max(widest_, x_);
    }

    PreviewLayout& out_;
    float left_;
    float right_;
    float x_;
    float y_;
    float line_height_;
    float widest_ = 0.0F;
    bool pending_space_ = false;
    bool last_hit_ = false;
};

class Layouter {
public:
    Layouter(PreviewLayout& out, const PreviewFonts& fonts, float em, float width, const LinkResolves& resolves)
        : out_(out), fonts_(fonts), resolves_(resolves), em_(em), width_(width), line_height_(em * kLineSpacing) {}

    // Bloğun altını döner; sağ kenarı `right_`a işler.
    float add(const pdf::Block& block, float top) {
        quoted_ = block.quote_depth > 0;
        const float quote_left = static_cast<float>(block.quote_depth) * kQuoteIndentEms * em_;
        const float list_indent = kListIndentEms * em_;
        const float marker_left = quote_left + (static_cast<float>(std::max(block.list_depth - 1, 0)) * list_indent);
        float left = block.list_depth > 0 ? marker_left + list_indent : quote_left;
        if (!block.marker.empty()) {
            const Look muted{fonts_.regular, em_, PreviewInk::Muted, false, {}, false};
            // Geniş im ("10. [x] ") metni sağa iter: üst üste binmesin.
            left = std::max(left, marker_left + measure(fonts_.regular, em_, block.marker));
            Flow marker(out_, marker_left, FLT_MAX, top, block_line_height(block));
            marker.add(block.marker, muted);
        }
        switch (block.kind) {
        case pdf::Block::Kind::Heading:
            return add_text(block.spans, left, top, heading_size(block.heading_level), true);
        case pdf::Block::Kind::Code:
            return add_code(block, left, top);
        case pdf::Block::Kind::Rule:
            out_.boxes.push_back({PreviewBox::Kind::Line, ImVec2(left, top + (line_height_ / 2.0F)),
                                  ImVec2(width_, top + (line_height_ / 2.0F) + kLineThickness)});
            return top + line_height_;
        case pdf::Block::Kind::Table:
            return add_table(block, left, top);
        case pdf::Block::Kind::Paragraph:
            break;
        }
        return add_text(block.spans, left, top, em_, false);
    }

    float right() const { return right_; }

private:
    float heading_size(int level) const {
        const auto index = static_cast<std::size_t>(std::clamp(level, 1, static_cast<int>(kHeadingScale.size())) - 1);
        return em_ * kHeadingScale[index];
    }

    float block_line_height(const pdf::Block& block) const {
        return block.kind == pdf::Block::Kind::Heading ? heading_size(block.heading_level) * kLineSpacing
                                                       : line_height_;
    }

    float add_text(const std::vector<pdf::Span>& spans, float left, float top, float size, bool bold) {
        Flow flow(out_, left, std::max(width_, left + em_), top, size * kLineSpacing);
        for (const pdf::Span& span : spans) {
            Look look = look_of(fonts_, span, size, bold, resolves_);
            if (quoted_ && look.ink == PreviewInk::Text)  // alıntı soluk; bağlantı rengi kalır
                look.ink = PreviewInk::Muted;
            flow.add(span.text, look);
        }
        right_ = std::max(right_, flow.right_edge());
        return flow.bottom();
    }

    // Boşluklar korunur, sarma yok: uzun satır yatay kaydırılır.
    float add_code(const pdf::Block& block, float left, float top) {
        const float pad = kCodePadEms * em_;
        float x = left + pad;
        float y = top + pad;
        float widest = x;
        for (std::size_t index = 0; index < block.spans.size(); ++index) {
            std::string_view text = block.spans[index].text;
            if (index + 1 == block.spans.size() && text.ends_with('\n'))
                text.remove_suffix(1);  // son satır sonu boş satır değildir
            while (true) {
                const std::size_t end = std::min(text.find('\n'), text.size());
                const std::string_view line = text.substr(0, end);
                if (!line.empty()) {
                    const float width = measure(fonts_.mono, em_, line);
                    out_.runs.push_back({fonts_.mono, em_, ImVec2(x, y + ((line_height_ - em_) / 2.0F)), width, line,
                                         PreviewInk::Text, false, false, y, y + line_height_});
                    x += width;
                    widest = std::max(widest, x);
                }
                if (end == text.size())
                    break;
                text.remove_prefix(end + 1);
                x = left + pad;
                y += line_height_;
            }
        }
        const float bottom = y + line_height_ + pad;
        const float right = std::max(width_, widest + pad);
        out_.boxes.push_back({PreviewBox::Kind::CodeBlock, ImVec2(left, top), ImVec2(right, bottom)});
        right_ = std::max(right_, right);
        return bottom;
    }

    // Hücreler tek satır, sütun en geniş hücresi kadar; başlık satırı kalın. Sığmayan tablo yatay kaydırılır.
    float add_table(const pdf::Block& block, float left, float top) {
        const float pad = kCellPadEms * em_;
        std::vector<float> widths;
        for (const pdf::TableRow& row : block.rows) {
            widths.resize(std::max(widths.size(), row.cells.size()), 0.0F);
            for (std::size_t column = 0; column < row.cells.size(); ++column) {
                float width = 0.0F;
                for (const pdf::Span& span : row.cells[column])
                    width += measure(look_of(fonts_, span, em_, row.is_header, resolves_).font, em_, span.text);
                widths[column] = std::max(widths[column], width);
            }
        }
        std::vector<float> edges{left};
        for (const float width : widths)
            edges.push_back(edges.back() + width + (2.0F * pad));
        float y = top;
        out_.boxes.push_back({PreviewBox::Kind::Line, ImVec2(left, y), ImVec2(edges.back(), y + kLineThickness)});
        for (const pdf::TableRow& row : block.rows) {
            for (std::size_t column = 0; column < row.cells.size(); ++column) {
                Flow cell(out_, edges[column] + pad, FLT_MAX, y, line_height_);
                for (const pdf::Span& span : row.cells[column])
                    cell.add(span.text, look_of(fonts_, span, em_, row.is_header, resolves_));
            }
            y += line_height_;
            out_.boxes.push_back({PreviewBox::Kind::Line, ImVec2(left, y), ImVec2(edges.back(), y + kLineThickness)});
        }
        for (const float edge : edges)
            out_.boxes.push_back({PreviewBox::Kind::Line, ImVec2(edge, top), ImVec2(edge + kLineThickness, y)});
        right_ = std::max(right_, edges.back() + kLineThickness);
        return y + kLineThickness;
    }

    PreviewLayout& out_;
    const PreviewFonts& fonts_;
    const LinkResolves& resolves_;
    float em_;
    float width_;
    float line_height_;
    float right_ = 0.0F;
    bool quoted_ = false;  // eklenen blok alıntıda
};

bool is_list_item(const pdf::Block& block) {
    return block.list_depth > 0;
}

}  // namespace

PreviewLayout layout_preview(const std::vector<pdf::Block>& blocks, const PreviewFonts& fonts, float font_size,
                             float width, const LinkResolves& resolves) {
    PreviewLayout out;
    Layouter layouter(out, fonts, font_size, width, resolves);
    const float bar_width = kQuoteBarEms * font_size;
    float y = 0.0F;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const pdf::Block& block = blocks[index];
        const float bottom = layouter.add(block, y);
        out.block_spans.emplace_back(y, bottom);
        const pdf::Block* next = index + 1 < blocks.size() ? &blocks[index + 1] : nullptr;
        float gap_ems = kBlockGapEms;
        if (next != nullptr && next->kind == pdf::Block::Kind::Heading)
            gap_ems = kHeadingGapEms;
        else if (next != nullptr && is_list_item(block) && is_list_item(*next))
            gap_ems = kListItemGapEms;
        const float gap = gap_ems * font_size;
        // Alıntı çubuğu aynı alıntının sonraki bloğuna aradaki boşluk boyunca uzar: kesik çizgi olmasın.
        for (int depth = 0; depth < block.quote_depth; ++depth) {
            const float x = static_cast<float>(depth) * kQuoteIndentEms * font_size;
            const bool continues = next != nullptr && next->quote_depth > depth;
            out.boxes.push_back(
                {PreviewBox::Kind::QuoteBar, ImVec2(x, y), ImVec2(x + bar_width, continues ? bottom + gap : bottom)});
        }
        y = bottom + gap;
    }
    out.size = ImVec2(layouter.right(), y);
    return out;
}

std::span<const PreviewRun> visible_runs(const PreviewLayout& layout, float top, float bottom) {
    // Satırı görünen alana değen koşular; iki anahtar da koşu sırasıyla azalmaz, ikisi de ikili aramayla.
    const auto first = std::ranges::lower_bound(layout.runs, top, {}, &PreviewRun::line_bottom);
    const auto last = std::ranges::upper_bound(first, layout.runs.end(), bottom, {}, &PreviewRun::line_top);
    return {first, last};
}

void draw_preview_layout(const PreviewLayout& layout, ImVec2 origin, float top, float bottom) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    // Kod bloğu ve satır içi kod aynı zemin: tuvale karşı ≥ 1,5, metne karşı ≥ 4,5 (theme_test).
    const ImU32 background = theme::current().code_bg;
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_Separator);
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const auto at = [origin](ImVec2 point) { return ImVec2(origin.x + point.x, origin.y + point.y); };
    for (const PreviewBox& box : layout.boxes) {
        if (box.max.y < top || box.min.y > bottom)
            continue;
        if (box.kind == PreviewBox::Kind::CodeBlock)
            draw_list->AddRectFilled(at(box.min), at(box.max), background, kCodeBlockRounding * scale);
        else
            draw_list->AddRectFilled(at(box.min), at(box.max), line);
    }
    for (const PreviewRun& run : visible_runs(layout, top, bottom)) {
        const bool dim = run.ink == PreviewInk::Muted || run.ink == PreviewInk::Missing;
        const ImU32 color = ImGui::GetColorU32(run.ink == PreviewInk::Link ? ImGuiCol_TextLink
                                               : dim                       ? ImGuiCol_TextDisabled
                                                                           : ImGuiCol_Text);
        const ImVec2 pos = at(run.pos);
        if (run.code_background) {
            const float pad = run.size * kInlineCodePadEms;
            draw_list->AddRectFilled(ImVec2(pos.x - pad, pos.y),
                                     ImVec2(pos.x + run.width + pad, pos.y + run.size), background,
                                     kInlineCodeRounding * scale);
        }
        draw_list->AddText(run.font, run.size, pos, color, run.text.data(), run.text.data() + run.text.size());
        if (run.underline)
            draw_list->AddLine(ImVec2(pos.x, pos.y + run.size), ImVec2(pos.x + run.width, pos.y + run.size), color);
        if (run.ink == PreviewInk::Missing) {
            const float middle = pos.y + (run.size / 2.0F);
            draw_list->AddLine(ImVec2(pos.x, middle), ImVec2(pos.x + run.width, middle), color);
        }
        // Çizim listesine yazılan metin ImGui günlüğüne (testler) de girsin; günlük kapalıyken iş yapmaz.
        ImGui::LogText("%.*s ", static_cast<int>(run.text.size()), run.text.data());
    }
}

}  // namespace noteapp::gui
