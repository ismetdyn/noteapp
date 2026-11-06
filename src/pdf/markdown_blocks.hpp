#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace noteapp::pdf {

struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool strike = false;  // ~~üstü çizili~~
    bool operator==(const TextStyle&) const = default;
};

// Aynı biçimli metin parçası; '\n' zorunlu satır sonudur.
struct Span {
    std::string text;
    TextStyle style;
    // Yalnız MarkdownOptions::links ile dolar: bağlantının ham hedefi (URL ya da [[wiki]] hedefi, güvenilmez).
    std::string link = {};
    bool wiki = false;
};

struct TableRow {
    std::vector<std::vector<Span>> cells;
    bool is_header = false;
};

// Tablo sütununun hizası (GFM `:--`, `:-:`, `--:`); Default sola yaslıdır.
enum class Align { Default, Left, Center, Right };

// Madde imi iç içe liste derinliğine göre döner (HTML'deki disc, circle, square); TUI aynı tabloyu kullanır.
inline constexpr std::string_view kListBullets[] = {"• ", "◦ ", "▪ "};

struct Block {
    enum class Kind { Paragraph, Heading, Code, Rule, Table };
    Kind kind = Kind::Paragraph;
    int heading_level = 0;
    int list_depth = 0;
    int quote_depth = 0;
    // Kaplar dıştan içe: 'Q' alıntı, 'L' liste ("LQ": liste öğesindeki alıntı); derinlikler bunun sayımıdır.
    std::string nesting;
    // Liste imi: "• ", "2. ", "[x] "; ilk satırda asılı durur. Blok açmadan kap (iç liste, alıntı) açan ya da boş
    // öğenin imi kendi boş paragrafındadır.
    std::string marker;
    std::vector<Span> spans;
    std::vector<TableRow> rows;
    std::vector<Align> aligns;  // tablo: sütun başına
    bool list_start = false;    // listenin ilk bloğu: önceki ayrı listeden aralıkla ayrılır
    std::string language;       // kuşatılmış kod: dil etiketi (bilgi dizgesinin ilk sözcüğü); renk GUI'de
};

struct MarkdownOptions {
    // GUI önizlemesi: bağlantı hedefleri ve [[wiki]] bağlantıları (md4c MD_FLAG_WIKILINKS) taşınır. PDF
    // istemez: orada [[...]] düz metin kalır, hedef hiç bulunmaz.
    bool links = false;
};

// `text` geçerli UTF-8 olmalıdır (sanitize_text). HTML kapalıdır: ham etiketler düz metin kalır. Görseller
// alt metnine (yoksa pdf.image_placeholder; HTML'deki alt gibi süssüz, bağlantısız), bağlantılar metnine iner;
// hedef yalnız MarkdownOptions::links ile, varlıkları çözülmüş bulunur.
std::vector<Block> parse_markdown(std::string_view text, MarkdownOptions options = {});

}  // namespace noteapp::pdf
