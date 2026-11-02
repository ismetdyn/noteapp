#pragma once

#include "tui/syntax.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// REVIEW'da biçimli görünümün (md, csv, json) üst sınırları: not güvenilmezdir, bellek ve süre girdiyle
// doğrusal ve sınırlı kalmalı. Aşan not ham metne düşer ya da kesildiğini soluk satırla söyler.
// Üretim UI iş parçacığındadır, bütçe 50 ms: bu sınır ölçümle seçildi (SPEC-tui "REVIEW biçimli görünüm").
inline constexpr std::size_t kMaxRenderedViewBytes = std::size_t{256} * 1024;
inline constexpr std::size_t kMaxViewLines = 100000;
inline constexpr std::size_t kMaxTableRows = 5000;
inline constexpr std::size_t kMaxTableColumns = 128;

// Hücrenin görünüşü: yalnız yazı biçimi ve 16 ANSI renklerine inen sözdizimi türü (terminal bağımsız,
// FTXUI'siz).
struct TextLook {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool dim = false;
    bool underline = false;
    TokenKind token = TokenKind::Plain;
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
struct RenderedView {
    std::vector<std::string> lines;
    std::vector<std::vector<StyledSpan>> spans;
    std::vector<std::size_t> source_lines;
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
                            TextLook fallback);

class ViewBuilder {
public:
    ViewBuilder(int width, CellWidthFn cell_width);

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
    RenderedView view_;
    bool truncated_ = false;
};

// Tablo verisi: hücreler sarılmamış; kısa satırlar boş hücreyle tamamlanır.
struct TableData {
    std::vector<std::vector<InlineText>> rows;
    std::vector<std::size_t> source_lines;
    std::size_t header_rows = 0;
    std::size_t omitted_rows = 0;  // kMaxTableRows'u aşan, gösterilmeyen satırlar
    std::size_t column_count = 0;  // kaynaktaki sütun sayısı; kMaxTableColumns'tan fazlası saklanmaz
};

// Hücreler sütun genişliğinde sarılır, tablo `prefix` dahil genişliğe sığar; sığmayan sütunlar düşer ve
// soluk satırla söylenir.
void add_table(ViewBuilder& view, const TableData& table, const InlineText& prefix);

}  // namespace noteapp::tui
