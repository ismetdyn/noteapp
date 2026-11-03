#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Notun biçimli görünümü (TUI REVIEW, GUI önizleme); uzantıdan belirlenir, None ise yalnız ham metin vardır.
enum class PreviewKind { None, Markdown, Csv, Json };
PreviewKind preview_kind_for(std::string_view extension);  // büyük/küçük harf duyarsız, noktasız

// limits.tui.table_rows ve table_columns: iki arayüzde aynı anahtarlar (varsayılanlar defaults.toml ile
// aynı).
struct TableLimits {
    std::size_t rows = 5000;
    std::size_t columns = 128;

    bool operator==(const TableLimits&) const = default;
};

// Ayrıştırılmış CSV: hücreler kaynaktaki baytlardır (görüntü arındırması arayüzün işi), ilk satır başlıktır.
struct CsvTable {
    // En çok limits.rows satır, satırda en çok limits.columns hücre.
    std::vector<std::vector<std::string>> rows;
    std::vector<std::size_t> row_offsets;  // satırın kaynaktaki (BOM dahil) bayt ofseti
    std::size_t column_count = 0;          // kaynaktaki en geniş satır; saklanandan fazla olabilir
    std::size_t omitted_rows = 0;          // limits.rows'u aşan satırlar
};

// Ayırıcı `detect` kuralıyla bulunur; tutarlı tablo değilse nullopt. RFC 4180'e yakın: tırnaklı alanda
// ayırıcı ve satır sonu metindir, "" tek tırnaktır; bozuk tırnak hata değildir, metin görünür kalır. Boş
// satır atlanır.
std::optional<CsvTable> parse_csv(std::string_view source, const TableLimits& limits = {});

// Girintilenmiş JSON'un bir satırı: `depth` iç içelik, `text` boşluksuz belirteçler (`"ad": "x",`), `offset`
// satırın ilk belirtecinin kaynaktaki (BOM dahil) bayt ofseti.
struct JsonLine {
    std::size_t depth = 0;
    std::string text;
    std::size_t offset = 0;
};

// Katı RFC 8259: geçersiz JSON nullopt. Özyinelemesiz, iç içelik sınırı yok; satır sayısını çağıran sınırlar
// (kaynak zaten limits.tui.view_bytes altındadır).
std::optional<std::vector<JsonLine>> pretty_json(std::string_view source);

}  // namespace noteapp::app
