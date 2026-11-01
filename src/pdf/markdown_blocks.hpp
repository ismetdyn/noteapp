#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace noteapp::pdf {

struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool operator==(const TextStyle&) const = default;
};

// Aynı biçimli metin parçası; '\n' zorunlu satır sonudur.
struct Span {
    std::string text;
    TextStyle style;
};

struct TableRow {
    std::vector<std::vector<Span>> cells;
    bool is_header = false;
};

struct Block {
    enum class Kind { Paragraph, Heading, Code, Rule, Table };
    Kind kind = Kind::Paragraph;
    int heading_level = 0;
    int list_depth = 0;
    int quote_depth = 0;
    std::string marker;  // liste imi: "• ", "2. ", "[x] "; ilk satırda asılı durur
    std::vector<Span> spans;
    std::vector<TableRow> rows;
};

// `text` geçerli UTF-8 olmalıdır (sanitize_text). HTML kapalıdır: ham etiketler düz metin kalır. Görseller
// alt metnine (yoksa pdf.image_placeholder), bağlantılar metnine iner; hedef URL'ler sonuçta hiç bulunmaz.
std::vector<Block> parse_markdown(std::string_view text);

}  // namespace noteapp::pdf
