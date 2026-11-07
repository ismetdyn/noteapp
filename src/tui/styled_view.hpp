#pragma once

#include "app/data_preview.hpp"
#include "core/limits.hpp"
#include "edit/syntax.hpp"
#include "pdf/markdown_blocks.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// REVIEW'da biçimli görünümün (md, csv, json) sınırları: `limits.tui.*` (view_limits_of; sınırsız SIZE_MAX).
// Bellek ve süre girdiyle doğrusaldır; aşan not ham metne düşer ya da kesildiğini soluk satırla söyler.
// Üretim UI iş parçacığındadır, bütçe 50 ms: varsayılanlar ölçümle seçildi (SPEC-tui "REVIEW biçimli
// görünüm"). Üye varsayılanları defaults.toml ile aynıdır, bir test denetler.
struct ViewLimits {
    // Biçimli görünüme giren en büyük not (PreviewCache); varsayılan sınırsız: büyük not parça parça üretilir.
    std::size_t source_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t lines = 100000;
    std::size_t table_rows = std::numeric_limits<std::size_t>::max();  // satırlar dizinli: varsayılan sınırsız
    std::size_t table_columns = 128;
    std::size_t highlight_line_bytes = kDefaultHighlightLineBytes;  // markdown kod bloğu
    app::MarkdownChunkLimits markdown = {};  // büyük md parçalarının düşman girdi korumaları (limits.tui.preview_*)

    bool operator==(const ViewLimits&) const = default;
};
ViewLimits view_limits_of(const core::TuiLimits& limits);
SyntaxLimits syntax_limits_of(const core::TuiLimits& limits);

// Hücrenin görünüşü: yalnız yazı biçimi ve 16 ANSI renklerine inen sözdizimi türü (terminal bağımsız,
// FTXUI'siz).
struct TextLook {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool dim = false;
    bool underline = false;
    TokenKind token = TokenKind::Plain;
    // Biçimli markdown'daki [[bağlantı]]: RenderedView::wiki_targets'ta 1 tabanlı sıra; 0 bağlantı değil.
    std::size_t wiki_link = 0;
    bool operator==(const TextLook&) const = default;
};

// Satır içi bayt aralığı [begin, end); sıralı ve örtüşmesiz, düz metin için aralık üretilmez.
struct StyledSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    TextLook look;
};

// Görüntülenecek satırlar; üç dizi aynı uzunluktadır. Her satır genişliğe sığar (sarma burada biter), metin
// terminal_safe'ten geçmiştir. `source_lines` kaynakta o satırı üreten bloğun satırıdır (0 tabanlı, azalmaz).
// `wiki_targets`: [[bağlantı]] hedefleri (core::link_target anahtarı) metindeki sırayla; TextLook::wiki_link
// bunlara bakar. Bağlantı ayraçsız, GUI gibi metniyle çizilir.
struct RenderedView {
    std::vector<std::string> lines;
    std::vector<std::vector<StyledSpan>> spans;
    std::vector<std::size_t> source_lines;
    std::vector<std::string> wiki_targets;
};

using CellWidthFn = int (*)(char32_t);

// Sarılmayı bekleyen biçimli metin; '\n' zorunlu satır sonudur.
struct InlineText {
    std::string text;
    std::vector<StyledSpan> spans;
    // `safe` önceden display_safe'ten geçmiş olmalı.
    void append(std::string_view safe, TextLook look = {});
};

// Boşluk sayılan denetim baytları (sekme, satır sonu) boşluğa iner, kalanı terminal_safe kuralıyla arınır.
std::string display_safe(std::string_view raw);

// Kaynak bayt ofseti -> satır numarası (0 tabanlı).
class SourceLines {
public:
    explicit SourceLines(std::string_view text);
    std::size_t line_of(std::size_t offset) const;
    std::size_t count() const { return starts_.size(); }
    std::string_view line_text(std::size_t line) const;

private:
    std::string_view text_;
    std::vector<std::size_t> starts_;
};

// `line` yalnız görüntülenecek metindir; dil Plain ise tüm satır `fallback` görünüşünü alır.
InlineText highlighted_line(SyntaxLanguage language, std::string_view line, SyntaxState& state,
                            TextLook fallback, std::size_t max_line_bytes = kDefaultHighlightLineBytes);

class ViewBuilder {
public:
    // `max_lines` aşılınca kalan satırlar düşer, sonda soluk satır söyler.
    ViewBuilder(int width, CellWidthFn cell_width, std::size_t max_lines = ViewLimits{}.lines);

    // İçeriği sarar: ilk satırın önünde `first`, devamında `next` (iki önek aynı genişlikte).
    void add(const InlineText& first, const InlineText& next, const InlineText& content,
             std::size_t source_line);
    // Genişliğe sığdığı bilinen hazır satır.
    void add_line(const InlineText& line, std::size_t source_line);
    // Sınırı aşan kısmın söylenmesi: satır sınırından muaf, soluk.
    void add_note(const std::string& note, const InlineText& prefix, std::size_t source_line);

    int width() const { return width_; }
    CellWidthFn cell_width() const { return cell_width_; }
    int cells(std::string_view text) const;
    bool empty() const { return view_.lines.empty(); }
    std::size_t last_source_line() const;

    RenderedView finish() &&;

private:
    void push(const InlineText& prefix, const InlineText& content, std::size_t begin, std::size_t end,
              std::size_t& span_cursor, std::size_t source_line);

    int width_;
    CellWidthFn cell_width_;
    std::size_t max_lines_;
    RenderedView view_;
    bool truncated_ = false;
};

// Tablo verisi: hücreler sarılmamış; kısa satırlar boş hücreyle tamamlanır.
struct TableData {
    std::vector<std::vector<InlineText>> rows;
    std::vector<pdf::Align> aligns;  // sütun başına (markdown `:-:`, `--:`); eksik sütun sola yaslı
    std::vector<std::size_t> source_lines;
    std::size_t header_rows = 0;
    std::size_t omitted_rows = 0;  // ViewLimits::table_rows'u aşan, gösterilmeyen satırlar
    std::size_t column_count = 0;  // kaynaktaki sütun sayısı; ViewLimits::table_columns'tan fazlası saklanmaz
};

// Tablonun sütun genişlikleri ve sığmayan sütun sayısı. Parça parça çizilen büyük tabloda ilk parçadan alınır, sonraki
// parçalar aynı sütunlarla çizilir (geniş hücre sütununda sarılır).
struct TableColumns {
    std::vector<int> widths;
    std::size_t hidden = 0;
};
TableColumns table_columns(const ViewBuilder& view, const TableData& table, const InlineText& prefix);

// Hücreler sütun genişliğinde sarılır, tablo `prefix` dahil genişliğe sığar; sığmayan sütunlar düşer ve
// soluk satırla söylenir.
void add_table(ViewBuilder& view, const TableData& table, const InlineText& prefix);
void add_table(ViewBuilder& view, const TableData& table, const InlineText& prefix, const TableColumns& columns);

}  // namespace noteapp::tui
