#include "pdf/pdf_export.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "pdf/fonts.hpp"
#include "pdf/markdown_blocks.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <hpdf.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace noteapp::pdf {

namespace {

constexpr float kPointsPerMm = 72.0F / 25.4F;
constexpr float kMargin = 15.0F * kPointsPerMm;
constexpr float kBodySize = 11.0F;
constexpr float kCodeSize = 9.5F;
constexpr std::array<float, 6> kHeadingSizes = {20.0F, 17.0F, 15.0F, 13.0F, 12.0F, 11.0F};
constexpr float kLineHeight = 1.35F;
constexpr float kBlockGap = 6.0F;
constexpr float kListIndent = 14.0F;
constexpr float kQuoteIndent = 12.0F;
constexpr float kQuoteBarOffset = 6.0F;
constexpr float kRuleWidth = 0.5F;
constexpr float kCellGap = 8.0F;
constexpr float kFontUnitsPerEm = 1000.0F;
// Girinti alanı yutamasın: derin iç içe yapıda bile metin en az bu kadar genişlikte akar.
constexpr float kMinTextWidth = 120.0F;
// Bu sütun sayısının üstünde hücreler okunamayacak kadar daralır; satır düz metne döner, veri kaybolmaz.
constexpr std::size_t kMaxTableColumns = 12;
constexpr std::string_view kTabReplacement = "    ";
constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::string_view kColumnSeparator = " | ";

std::size_t utf8_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return lead >= 0xC0 ? 2 : 1;
}

// libharu'nun UTF-8 kodlayıcısı durumludur ve 16 bitlik kod kullanır: bozuk dizi sonraki karakterleri
// kaydırır, BMP dışı kırpılır. Bu yüzden ona yalnız geçerli BMP metni verilir.
std::string sanitize_text(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            if (lead == '\t')
                clean += kTabReplacement;
            else if (lead == '\n' || lead >= 0x20)
                clean += static_cast<char>(lead);
            ++i;  // diğer denetim karakterleri (CR dahil) atılır: görünür karşılıkları yok
            continue;
        }
        const std::size_t length = utf8_length(lead);
        const std::string_view sequence = text.substr(i, length);
        if (sequence.size() == length && length > 1 && length < 4 && core::is_valid_utf8(sequence)) {
            clean += sequence;
            i += length;
        } else {
            clean += kReplacementCharacter;
            // Geçerli 4 baytlık dizi tek karakterdir; bozuk bayt tek tek değiştirilir.
            i += length == 4 && sequence.size() == 4 && core::is_valid_utf8(sequence) ? 4 : 1;
        }
    }
    return clean;
}

core::Error render_error(HPDF_STATUS status) {
    return {core::Error::Code::Io, "pdf.render_failed", {{"detail", std::to_string(status)}}};
}

struct DocDeleter {
    void operator()(HPDF_Doc doc) const { HPDF_Free(doc); }
};
using DocPtr = std::unique_ptr<std::remove_pointer_t<HPDF_Doc>, DocDeleter>;

// Tek satırın çizilecek parçası.
struct Fragment {
    std::string text;
    TextStyle style;
    float width = 0;
};

struct Line {
    std::vector<Fragment> fragments;
    float width = 0;
};

// Fontlar ilk kullanımda yüklenir: kullanılmayan her font PDF'i büyütürdü (sıkıştırma yok).
class FontCache {
public:
    explicit FontCache(HPDF_Doc doc) : doc_(doc) {}

    HPDF_Font get(TextStyle style) {
        const std::size_t slot = style.mono ? 4 : (style.bold ? 1U : 0U) + (style.italic ? 2U : 0U);
        HPDF_Font& font = fonts_[slot];
        if (font == nullptr) {
            static constexpr std::array<const char*, 5> kFiles = {
                "fonts/DejaVuSans.ttf", "fonts/DejaVuSans-Bold.ttf", "fonts/DejaVuSans-Oblique.ttf",
                "fonts/DejaVuSans-BoldOblique.ttf", "fonts/DejaVuSansMono.ttf"};
            const std::optional<std::string_view> data = embedded_font(kFiles[slot]);
            if (!data)
                return nullptr;
            const auto* bytes = reinterpret_cast<const HPDF_BYTE*>(data->data());
            const char* name =
                HPDF_LoadTTFontFromMemory(doc_, bytes, static_cast<HPDF_UINT>(data->size()), HPDF_TRUE);
            if (name != nullptr)
                font = HPDF_GetFont(doc_, name, "UTF-8");
        }
        return font;
    }

    float measure(std::string_view text, TextStyle style, float size) {
        HPDF_Font font = get(style);
        if (font == nullptr || text.empty())
            return 0;
        const auto* bytes = reinterpret_cast<const HPDF_BYTE*>(text.data());
        const HPDF_TextWidth width = HPDF_Font_TextWidth(font, bytes, static_cast<HPDF_UINT>(text.size()));
        return static_cast<float>(width.width) * size / kFontUnitsPerEm;
    }

private:
    HPDF_Doc doc_;
    std::array<HPDF_Font, 5> fonts_{};
};

// Açgözlü satır kırıcı: boşlukta kırar; satırdan uzun sözcüğü kod noktası sınırında böler. Her zaman ilerler:
// dar alanda bile satıra en az bir karakter koyar, sonsuz döngü olmaz.
class LineWrapper {
public:
    LineWrapper(FontCache& fonts, float max_width, float size)
        : fonts_(fonts), max_width_(max_width), size_(size) {}

    void add(const std::vector<Span>& spans) {
        for (const Span& span : spans)
            add_span(span);
    }

    // Boşlukları korur (kod bloğu, düz metin): yalnız satır sonunda ve alan bitince kırar.
    void add_preformatted(std::string_view text, TextStyle style) {
        for (std::size_t start = 0; start <= text.size();) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            add_broken(text.substr(start, end - start), style);
            end_line();
            start = end + 1;
        }
    }

    std::vector<Line> finish() && {
        if (!current_.fragments.empty())
            end_line();
        return std::move(lines_);
    }

private:
    void add_span(const Span& span) {
        std::string_view rest = span.text;
        while (!rest.empty()) {
            if (rest.front() == '\n') {
                end_line();
                pending_space_ = false;
                rest.remove_prefix(1);
            } else if (rest.front() == ' ') {
                pending_space_ = !current_.fragments.empty();
                rest.remove_prefix(1);
            } else {
                const std::size_t end = std::min(rest.find_first_of(" \n"), rest.size());
                add_word(rest.substr(0, end), span.style);
                rest.remove_prefix(end);
            }
        }
    }

    void add_word(std::string_view word, TextStyle style) {
        const float word_width = fonts_.measure(word, style, size_);
        const float space_width = pending_space_ ? fonts_.measure(" ", style, size_) : 0;
        if (current_.width + space_width + word_width > max_width_) {
            if (!current_.fragments.empty())
                end_line();
            if (word_width > max_width_) {
                add_broken(word, style);
                return;
            }
        } else if (pending_space_) {
            place(" ", style, space_width);
        }
        pending_space_ = false;
        place(word, style, word_width);
    }

    // Kod noktası başına ölçülür: dosya boyunda doğrusal.
    void add_broken(std::string_view text, TextStyle style) {
        pending_space_ = false;
        std::size_t run_start = 0;
        float run_width = 0;
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t length = utf8_length(static_cast<unsigned char>(text[i]));
            const float width = fonts_.measure(text.substr(i, length), style, size_);
            if (current_.width + run_width + width > max_width_ &&
                (run_width > 0 || !current_.fragments.empty())) {
                place(text.substr(run_start, i - run_start), style, run_width);
                end_line();
                run_start = i;
                run_width = 0;
            }
            run_width += width;
            i += length;
        }
        place(text.substr(run_start), style, run_width);
    }

    void place(std::string_view text, TextStyle style, float width) {
        if (text.empty())
            return;
        if (!current_.fragments.empty() && current_.fragments.back().style == style) {
            current_.fragments.back().text += text;
            current_.fragments.back().width += width;
        } else {
            current_.fragments.push_back({std::string(text), style, width});
        }
        current_.width += width;
    }

    void end_line() {
        lines_.push_back(std::move(current_));
        current_ = Line{};
    }

    FontCache& fonts_;
    float max_width_;
    float size_;
    std::vector<Line> lines_;
    Line current_;
    bool pending_space_ = false;
};

class PageWriter {
public:
    explicit PageWriter(HPDF_Doc doc) : doc_(doc), fonts_(doc) { new_page(); }

    FontCache& fonts() { return fonts_; }
    float text_width() const { return page_width_ - 2 * kMargin; }

    void gap(float height) { cursor_ -= height; }

    // Taşan satır yeni sayfaya geçer; alt kenar boşluğu satır kutusunu da kapsar.
    void reserve_line(float leading) {
        if (cursor_ - leading < kMargin)
            new_page();
    }

    void draw_text(float x, float size, std::span<const Fragment> fragments) {
        const float baseline = cursor_ - size;
        HPDF_Page_BeginText(page_);
        for (const Fragment& fragment : fragments) {
            HPDF_Font font = fonts_.get(fragment.style);
            if (font == nullptr)
                continue;
            HPDF_Page_SetFontAndSize(page_, font, size);
            HPDF_Page_TextOut(page_, x, baseline, fragment.text.c_str());
            x += fragment.width;
        }
        HPDF_Page_EndText(page_);
    }

    void advance(float leading) { cursor_ -= leading; }

    void draw_vertical_bar(float x, float leading) {
        HPDF_Page_SetLineWidth(page_, kRuleWidth);
        HPDF_Page_MoveTo(page_, x, cursor_);
        HPDF_Page_LineTo(page_, x, cursor_ - leading);
        HPDF_Page_Stroke(page_);
    }

    void draw_rule() {
        reserve_line(kBlockGap);
        HPDF_Page_SetLineWidth(page_, kRuleWidth);
        HPDF_Page_MoveTo(page_, kMargin, cursor_);
        HPDF_Page_LineTo(page_, page_width_ - kMargin, cursor_);
        HPDF_Page_Stroke(page_);
    }

private:
    void new_page() {
        page_ = HPDF_AddPage(doc_);
        HPDF_Page_SetSize(page_, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
        page_width_ = HPDF_Page_GetWidth(page_);
        cursor_ = HPDF_Page_GetHeight(page_) - kMargin;
    }

    HPDF_Doc doc_;
    FontCache fonts_;
    HPDF_Page page_ = nullptr;
    float page_width_ = 0;
    float cursor_ = 0;
};

float heading_size(int level) {
    const auto index =
        static_cast<std::size_t>(std::clamp(level, 1, static_cast<int>(kHeadingSizes.size())) - 1);
    return kHeadingSizes[index];
}

float block_size(const Block& block) {
    switch (block.kind) {
    case Block::Kind::Heading:
        return heading_size(block.heading_level);
    case Block::Kind::Code:
        return kCodeSize;
    default:
        return kBodySize;
    }
}

std::vector<Span> styled_for(const Block& block) {
    std::vector<Span> spans = block.spans;
    if (block.kind == Block::Kind::Heading) {
        for (Span& span : spans)
            span.style.bold = true;
    }
    return spans;
}

void draw_lines(PageWriter& writer, const std::vector<Line>& lines, const Block& block, float x,
                float marker_x, float size) {
    const float leading = size * kLineHeight;
    const Fragment marker{block.marker, TextStyle{}, 0};
    bool is_first = true;
    for (const Line& line : lines) {
        writer.reserve_line(leading);
        for (int depth = 0; depth < block.quote_depth; ++depth) {
            const float bar_x = kMargin + static_cast<float>(depth + 1) * kQuoteIndent - kQuoteBarOffset;
            // Girinti tavanda: sonraki barlar da sığmaz. Döngü sürerse satır başına derinlik kadar boş adım
            // olur.
            if (bar_x >= marker_x)
                break;
            writer.draw_vertical_bar(bar_x, leading);
        }
        if (is_first && !block.marker.empty())
            writer.draw_text(marker_x, size, std::span(&marker, 1));
        writer.draw_text(x, size, line.fragments);
        writer.advance(leading);
        is_first = false;
    }
    writer.gap(kBlockGap);
}

std::vector<Span> flattened(const TableRow& row) {
    std::vector<Span> spans;
    for (std::size_t i = 0; i < row.cells.size(); ++i) {
        if (i > 0)
            spans.push_back({std::string(kColumnSeparator), TextStyle{}});
        spans.insert(spans.end(), row.cells[i].begin(), row.cells[i].end());
    }
    return spans;
}

void draw_table(PageWriter& writer, const Block& block, float left, float width) {
    std::size_t columns = 1;
    for (const TableRow& row : block.rows)
        columns = std::max(columns, row.cells.size());
    const float leading = kBodySize * kLineHeight;

    if (columns > kMaxTableColumns) {
        for (const TableRow& row : block.rows) {
            LineWrapper wrapper(writer.fonts(), width, kBodySize);
            wrapper.add(flattened(row));
            draw_lines(writer, std::move(wrapper).finish(), Block{}, left, left, kBodySize);
        }
        return;
    }

    const float column_width = width / static_cast<float>(columns);
    for (const TableRow& row : block.rows) {
        std::vector<std::vector<Line>> cells;
        std::size_t height = 1;
        for (const std::vector<Span>& cell : row.cells) {
            LineWrapper wrapper(writer.fonts(), column_width - kCellGap, kBodySize);
            if (row.is_header) {
                std::vector<Span> bold = cell;
                for (Span& span : bold)
                    span.style.bold = true;
                wrapper.add(bold);
            } else {
                wrapper.add(cell);
            }
            cells.push_back(std::move(wrapper).finish());
            height = std::max(height, cells.back().size());
        }
        // Satır satır çizilir: sayfadan uzun hücre de sayfa kırılımında kaybolmaz.
        for (std::size_t line = 0; line < height; ++line) {
            writer.reserve_line(leading);
            for (std::size_t column = 0; column < cells.size(); ++column) {
                if (line < cells[column].size())
                    writer.draw_text(left + static_cast<float>(column) * column_width, kBodySize,
                                     cells[column][line].fragments);
            }
            writer.advance(leading);
        }
    }
    writer.gap(kBlockGap);
}

void draw_block(PageWriter& writer, const Block& block) {
    const float requested_indent = static_cast<float>(block.list_depth) * kListIndent +
                                   static_cast<float>(block.quote_depth) * kQuoteIndent;
    const float indent = std::min(requested_indent, writer.text_width() - kMinTextWidth);
    const float marker_x = kMargin + indent;

    if (block.kind == Block::Kind::Rule) {
        writer.draw_rule();
        writer.gap(kBlockGap);
        return;
    }
    if (block.kind == Block::Kind::Table) {
        draw_table(writer, block, marker_x, writer.text_width() - indent);
        return;
    }
    const float size = block_size(block);
    const float marker_width = writer.fonts().measure(block.marker, TextStyle{}, size);
    const float x = marker_x + marker_width;
    LineWrapper wrapper(writer.fonts(), writer.text_width() - indent - marker_width, size);
    if (block.kind == Block::Kind::Code) {
        std::string code;
        for (const Span& span : block.spans)
            code += span.text;
        if (code.ends_with('\n'))
            code.pop_back();
        wrapper.add_preformatted(code, TextStyle{.mono = true});
    } else {
        wrapper.add(styled_for(block));
    }
    if (block.kind == Block::Kind::Heading)
        writer.gap(kBlockGap);
    draw_lines(writer, std::move(wrapper).finish(), block, x, marker_x, size);
}

std::expected<std::string, core::Error> save_to_bytes(HPDF_Doc doc) {
    const HPDF_STATUS saved = HPDF_SaveToStream(doc);
    if (saved != HPDF_OK)
        return std::unexpected(render_error(saved));
    std::string bytes(HPDF_GetStreamSize(doc), '\0');
    auto read = static_cast<HPDF_UINT32>(bytes.size());
    const HPDF_STATUS status = HPDF_ReadFromStream(doc, reinterpret_cast<HPDF_BYTE*>(bytes.data()), &read);
    // Akış sonu okuma bitince HPDF_STREAM_EOF döner; hata değildir.
    if (status != HPDF_OK && status != HPDF_STREAM_EOF)
        return std::unexpected(render_error(status));
    bytes.resize(read);
    return bytes;
}

}  // namespace

NoteFormat format_for(const std::filesystem::path& note_path) {
    std::string extension = note_path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
    return extension == ".md" || extension == ".markdown" ? NoteFormat::Markdown : NoteFormat::PlainText;
}

std::expected<void, core::Error> export_pdf(std::string_view text, NoteFormat format,
                                            const std::filesystem::path& output) {
    // NoteStore BOM'u korur; md4c'ye giderse ilk satırdaki başlık başlık sayılmazdı.
    if (text.starts_with(kByteOrderMark))
        text.remove_prefix(kByteOrderMark.size());
    const std::string clean = sanitize_text(text);
    const DocPtr doc(HPDF_New(nullptr, nullptr));
    if (!doc)
        return std::unexpected(render_error(HPDF_FAILD_TO_ALLOC_MEM));
    HPDF_UseUTFEncodings(doc.get());
    HPDF_SetCurrentEncoder(doc.get(), "UTF-8");

    PageWriter writer(doc.get());
    if (format == NoteFormat::Markdown) {
        for (const Block& block : parse_markdown(clean))
            draw_block(writer, block);
    } else {
        Block plain;
        plain.kind = Block::Kind::Code;
        plain.spans.push_back({clean, TextStyle{.mono = true}});
        draw_block(writer, plain);
    }
    // Çizim çağrıları hatayı belgede biriktirir; tek tek denetlemek yerine sonda bakılır.
    if (const HPDF_STATUS status = HPDF_GetError(doc.get()); status != HPDF_OK)
        return std::unexpected(render_error(status));

    const auto bytes = save_to_bytes(doc.get());
    if (!bytes)
        return std::unexpected(bytes.error());
    return core::write_file_atomically(output, *bytes);
}

}  // namespace noteapp::pdf
