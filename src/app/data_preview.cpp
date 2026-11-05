#include "app/data_preview.hpp"

#include "app/label_fold.hpp"
#include "detect/detect.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace noteapp::app {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::size_t kCsvSampleLines = 200;  // ayırıcı kararı için yeter; dev dosyada tüm satırları ayırmaz

std::size_t bom_size(std::string_view text) {
    return text.starts_with(kBom) ? kBom.size() : 0;
}

std::vector<std::string_view> csv_sample(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty() && lines.size() < kCsvSampleLines) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (!line.empty())
            lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return lines;
}

// `base`: metnin kaynaktaki ofseti (BOM), satır ofsetleri kaynağa göre verilsin.
class CsvParser {
public:
    CsvParser(std::string_view text, std::size_t base, char delimiter, const TableLimits& limits)
        : text_(text), base_(base), delimiter_(delimiter), limits_(limits) {}

    CsvTable parse() && {
        bool quoted = false;
        for (std::size_t at = 0; at < text_.size(); ++at)
            at = step(at, quoted);
        end_row();
        return std::move(table_);
    }

private:
    // Son işlenen baytın ofseti.
    std::size_t step(std::size_t at, bool& quoted) {
        const char byte = text_[at];
        if (byte == '"') {
            if (quoted && at + 1 < text_.size() && text_[at + 1] == '"') {
                field_ += '"';
                return at + 1;
            }
            quoted = !quoted;
        } else if (byte == delimiter_ && !quoted) {
            end_field();
        } else if (byte == '\n' && !quoted) {
            end_row();
            row_start_ = at + 1;
        } else {
            field_ += byte;
        }
        return at;
    }

    void end_field() {
        if (field_.ends_with('\r'))
            field_.pop_back();
        ++row_fields_;
        if (table_.rows.size() < limits_.rows && row_.size() < limits_.columns)
            row_.push_back(field_);
        row_blank_ = row_blank_ && field_.empty();
        field_.clear();
    }

    void end_row() {
        end_field();
        const bool blank = row_blank_ && row_fields_ == 1;
        if (!blank) {
            table_.column_count = std::max(table_.column_count, row_fields_);
            if (table_.rows.size() < limits_.rows) {
                table_.rows.push_back(std::move(row_));
                table_.row_offsets.push_back(base_ + row_start_);
            } else {
                ++table_.omitted_rows;
            }
        }
        row_.clear();
        row_fields_ = 0;
        row_blank_ = true;
    }

    std::string_view text_;
    std::size_t base_;
    char delimiter_;
    TableLimits limits_;
    CsvTable table_;
    std::vector<std::string> row_;
    std::string field_;
    std::size_t row_start_ = 0;
    std::size_t row_fields_ = 0;
    bool row_blank_ = true;
};

// Katı RFC 8259 denetleyicisi ve girintileyici. Özyinelemesiz: iç içelik `stack_`te (iç içe kap başına bir
// bayt), derinlik sınırı yok, çok derin not yığını taşırmaz. Bağımlılık yok: projede JSON kütüphanesi yok,
// yalnız bu görünüm için eklenmeye değmez.
class JsonPrinter {
public:
    JsonPrinter(std::string_view text, std::size_t base) : text_(text), base_(base) {}

    std::optional<std::vector<JsonLine>> print() && {
        if (!value())
            return std::nullopt;
        while (need_value_ || !stack_.empty()) {
            const bool ok = std::exchange(need_value_, false) ? value() : after_value();
            if (!ok)
                return std::nullopt;
        }
        flush();
        skip_space();
        if (pos_ != text_.size())
            return std::nullopt;
        return std::move(lines_);
    }

private:
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    void take(std::string_view shown, std::size_t source_offset) {
        if (line_.text.empty())
            line_.offset = base_ + source_offset;
        line_.text += shown;
    }

    void flush() {
        if (line_.text.empty())
            return;
        line_.depth = stack_.size();
        lines_.push_back(std::exchange(line_, {}));
    }

    bool after_value() {
        skip_space();
        const char opener = stack_.back();
        const char closer = opener == '{' ? '}' : ']';
        if (peek() == ',') {
            take(",", pos_);
            flush();
            ++pos_;
            need_value_ = true;
            return opener != '{' || key();
        }
        if (peek() != closer)
            return false;
        flush();
        stack_.pop_back();
        take(std::string_view(&closer, 1), pos_);
        ++pos_;
        return true;
    }

    bool value() {
        skip_space();
        switch (peek()) {
        case '{':
            return open('{', '}');
        case '[':
            return open('[', ']');
        case '"':
            return string();
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default:
            return number();
        }
    }

    // Boş olmayan kabın ilk değerini print() döngüsü okur (need_value_): iç içelik özyineleme değildir.
    bool open(char opener, char closer) {
        take(std::string_view(&opener, 1), pos_);
        ++pos_;
        skip_space();
        if (peek() == closer) {
            take(std::string_view(&closer, 1), pos_);
            ++pos_;
            return true;
        }
        flush();
        stack_.push_back(opener);
        need_value_ = true;
        return opener != '{' || key();
    }

    bool key() {
        skip_space();
        if (peek() != '"' || !string())
            return false;
        skip_space();
        if (peek() != ':')
            return false;
        take(": ", pos_);
        ++pos_;
        return true;
    }

    bool literal(std::string_view word) {
        if (!text_.substr(pos_).starts_with(word))
            return false;
        take(word, pos_);
        pos_ += word.size();
        return true;
    }

    bool escape_at(std::size_t at) const {
        const char kind = at < text_.size() ? text_[at] : '\0';
        if (kind != 'u')
            return std::string_view("\"\\/bfnrt").find(kind) != std::string_view::npos;
        constexpr std::size_t kHexDigits = 4;
        if (at + kHexDigits >= text_.size())
            return false;
        const std::string_view digits = text_.substr(at + 1, kHexDigits);
        return std::ranges::all_of(digits, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    }

    bool string() {
        constexpr unsigned char kFirstPrintable = 0x20;
        const std::size_t begin = pos_++;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            if (static_cast<unsigned char>(text_[pos_]) < kFirstPrintable)
                return false;
            if (text_[pos_] == '\\') {
                if (!escape_at(pos_ + 1))
                    return false;
                pos_ += text_[pos_ + 1] == 'u' ? 5 : 1;
            }
            ++pos_;
        }
        if (pos_ >= text_.size())
            return false;
        ++pos_;
        take(text_.substr(begin, pos_ - begin), begin);
        return true;
    }

    bool digits() {
        const std::size_t begin = pos_;
        while (peek() >= '0' && peek() <= '9')
            ++pos_;
        return pos_ > begin;
    }

    bool number() {
        const std::size_t begin = pos_;
        if (peek() == '-')
            ++pos_;
        if (peek() == '0')
            ++pos_;
        else if (!digits())
            return false;
        if (peek() == '.') {
            ++pos_;
            if (!digits())
                return false;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-')
                ++pos_;
            if (!digits())
                return false;
        }
        take(text_.substr(begin, pos_ - begin), begin);
        return true;
    }

    std::string_view text_;
    std::size_t base_;
    std::size_t pos_ = 0;
    std::vector<char> stack_;
    bool need_value_ = false;  // kap açıldı ya da virgül okundu: sıradaki değer okunacak
    JsonLine line_;
    std::vector<JsonLine> lines_;
};

}  // namespace

PreviewKind preview_kind_for(std::string_view extension) {
    std::string lowered(extension);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "md" || lowered == "markdown")
        return PreviewKind::Markdown;
    if (lowered == "json")
        return PreviewKind::Json;
    return lowered == "csv" ? PreviewKind::Csv : PreviewKind::None;
}

std::optional<CsvTable> parse_csv(std::string_view source, const TableLimits& limits) {
    const std::size_t base = bom_size(source);
    const std::string_view text = source.substr(base);
    const std::optional<char> delimiter = detect::table_delimiter(csv_sample(text));
    if (!delimiter)
        return std::nullopt;
    return CsvParser(text, base, *delimiter, limits).parse();
}

std::optional<std::vector<JsonLine>> pretty_json(std::string_view source) {
    const std::size_t base = bom_size(source);
    return JsonPrinter(source.substr(base), base).print();
}

namespace {

// Kuşatma satırı (CommonMark): en çok 3 boşluk girinti, aynı karakterden (` ya da ~) en az 3. `rest` koşudan
// sonrası.
struct FenceRun {
    char mark = '\0';
    std::size_t length = 0;
    std::string_view rest;
    std::size_t indent = 0;
    bool in_list = false;  // liste imli satırda açıldı: girinti öğe içeriğine göre, kabı kesin
};

FenceRun fence_run(std::string_view row) {
    constexpr std::size_t kMaxIndent = 3;
    constexpr std::size_t kMinRun = 3;
    const std::size_t indent = row.find_first_not_of(' ');
    if (indent == std::string_view::npos || indent > kMaxIndent || (row[indent] != '`' && row[indent] != '~'))
        return {};
    const char mark = row[indent];
    const std::size_t end = std::min(row.find_first_not_of(mark, indent), row.size());
    if (end - indent < kMinRun)
        return {};
    return {mark, end - indent, row.substr(end), indent};
}

bool is_blank(std::string_view row) {
    return row.find_first_not_of(" \t\r") == std::string_view::npos;
}

// Satırın `at`ten başlayan boşluğunun bittiği bayt ve ulaştığı sütun (`column`dan sayılır). Sekme bir sonraki 4'ün
// katına ilerler (CommonMark sekme durağı, md4c md_line_indentation): girinti bayt değil sütun karşılaştırılır.
struct Indent {
    std::size_t bytes = 0;
    std::size_t columns = 0;
};

Indent indentation(std::string_view row, std::size_t at = 0, std::size_t column = 0) {
    constexpr std::size_t kTabStop = 4;
    for (; at < row.size() && (row[at] == ' ' || row[at] == '\t'); ++at)
        column = row[at] == '\t' ? (column / kTabStop + 1) * kTabStop : column + 1;
    return {at, column};
}

// Liste imli satırda (`- `, `* `, `+ `, `12. `, `3) `; en çok 3 boşluk girinti) aynı satırda başlayan öğe içeriğinin
// yeri; değilse nullopt. İçerik çit olabilir (`- ```bash`). İmden sonra 5 ya da daha çok sütun boşluk varsa içerik
// imden bir sütun sonradır ve satırın kalanı girintili koddur: çit olamaz, nullopt.
std::optional<Indent> list_item_content(std::string_view row) {
    constexpr std::size_t kMaxIndent = 3;
    constexpr std::size_t kMaxDigits = 9;
    constexpr std::size_t kMaxGap = 4;
    std::size_t at = row.find_first_not_of(' ');
    if (at == std::string_view::npos || at > kMaxIndent)
        return std::nullopt;
    if (row[at] == '-' || row[at] == '*' || row[at] == '+') {
        ++at;
    } else {
        const std::size_t digits_end = std::min(row.find_first_not_of("0123456789", at), row.size());
        if (digits_end == at || digits_end - at > kMaxDigits || digits_end >= row.size() ||
            (row[digits_end] != '.' && row[digits_end] != ')'))
            return std::nullopt;
        at = digits_end + 1;
    }
    if (at >= row.size() || (row[at] != ' ' && row[at] != '\t'))
        return std::nullopt;
    // İme dek boşluk ve ASCII: imin bittiği bayt, bittiği sütundur.
    const Indent content = indentation(row, at, at);
    if (content.columns - at > kMaxGap)
        return std::nullopt;
    return content;
}

// Satırdaki çit: satır başında ya da liste iminden sonra. Girinti mutlak sütundur.
FenceRun fence_in(std::string_view row) {
    const char first = row.empty() ? '\0' : row.front();
    if (first == '`' || first == '~' || first == ' ')
        if (const FenceRun fence = fence_run(row); fence.mark != '\0')
            return fence;
    if (first != '-' && first != '*' && first != '+' && first != ' ' && (first < '0' || first > '9'))
        return {};
    const std::optional<Indent> content = list_item_content(row);
    if (!content)
        return {};
    FenceRun fence = fence_run(row.substr(content->bytes));
    fence.indent += content->columns;
    fence.in_list = true;
    return fence;
}

// Boş satırdan sonra açık blokları (liste, alıntı, tablo, paragraf) kapatıp yenisini başlatan satır. Girinti
// (liste devamı, girintili kod), alıntı, liste imi ve sayıyla (sıralı liste) başlayanlar değil.
bool starts_top_level_block(std::string_view row) {
    constexpr std::string_view kContinuers = " \t>-+*0123456789";
    return !is_blank(row) && kContinuers.find(row.front()) == std::string_view::npos;
}

// Etiket kuralları md4c 0.6.0'ınkiyle aynı (md4c.c: md_is_link_label, md_decode_utf8__, ISUNICODEWHITESPACE_):
// parça ile bütün belgenin aynı bağlantıyı çözmesi buna bağlı.
bool is_ascii_punct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

// Geçersiz dizide tek bayt, değeri md4c gibi `char`ın işaretiyle: geçersiz bayt geçerli bir harfle eşleşmez.
char32_t decode_utf8(std::string_view text, std::size_t at, std::size_t& size) {
    const auto byte = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(text[at + i])); };
    const auto tails = [&](std::size_t count) {
        for (std::size_t i = 1; i <= count; ++i)
            if (at + i >= text.size() || (byte(i) & 0xC0) != 0x80)
                return false;
        return true;
    };
    const char32_t lead = byte(0);
    if ((lead & 0xE0) == 0xC0 && tails(1)) {
        size = 2;
        return ((lead & 0x1F) << 6) | (byte(1) & 0x3F);
    }
    if ((lead & 0xF0) == 0xE0 && tails(2)) {
        size = 3;
        return ((lead & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
    }
    if ((lead & 0xF8) == 0xF0 && tails(3)) {
        size = 4;
        return ((lead & 0x07) << 18) | ((byte(1) & 0x3F) << 12) | ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
    }
    size = 1;
    return static_cast<char32_t>(static_cast<unsigned>(text[at]));
}

// ASCII boşluk, sekme, \v, \f ve Unicode Zs.
bool is_unicode_whitespace(char32_t c) {
    if (c <= 0x7F)
        return c == ' ' || c == '\t' || c == '\v' || c == '\f';
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool is_line_break(char c) {
    return c == '\n' || c == '\r';
}

// `[`ten sonraki `at`ten başlayan etiketi kapatan `]`in konumu; değilse npos. Kaçışlı ayraç (`\]`) etikettendir,
// kaçışsız `[` etiketi bozar, içerik boşluktan ibaret olamaz. Etiket paragraf içinde satır sonuyla sürer, boş
// satırda biter. md4c en çok 999 birim sayar (karakter, kaçış çifti, satır sonu); devam satırının girintisi ve
// alıntı imleri (`>`) sayılmaz, md4c satırı onlardan sonra başlatır.
std::size_t label_close(std::string_view text, std::size_t at) {
    constexpr std::size_t kMaxUnits = 999;
    std::size_t units = 0;
    bool blank = true;
    while (at < text.size()) {
        const char c = text[at];
        if (c == ']')
            return blank ? std::string_view::npos : at;
        if (c == '[')
            return std::string_view::npos;
        if (is_line_break(c)) {
            at += c == '\r' && at + 1 < text.size() && text[at + 1] == '\n' ? 2 : 1;
            ++units;  // md4c sınırı satır sonunda değil, sonraki karakterde denetler
            at = std::min(text.find_first_not_of(" \t>", at), text.size());
            if (at < text.size() && is_line_break(text[at]))
                return std::string_view::npos;
            continue;
        }
        std::size_t size = 1;
        if (c == '\\' && at + 1 < text.size() && is_ascii_punct(text[at + 1])) {
            size = 2;
            blank = false;
        } else {
            const char32_t codepoint =
                static_cast<unsigned char>(c) < 0x80 ? static_cast<char32_t>(c) : decode_utf8(text, at, size);
            blank = blank && is_unicode_whitespace(codepoint);
        }
        at += size;
        if (++units > kMaxUnits)
            return std::string_view::npos;
    }
    return std::string_view::npos;
}

// Alıntıdaki çok satırlı etiket: md4c satırları alıntı imleri olmadan birleştirir.
std::string without_quote_marks(std::string_view label) {
    std::string out;
    for (std::size_t at = 0; at < label.size();) {
        const std::size_t eol = std::min(label.find('\n', at), label.size());
        out += label.substr(at, eol - at);
        if (eol == label.size())
            break;
        out += '\n';
        at = std::min(label.find_first_not_of(" \t>", eol + 1), label.size());
    }
    return out;
}

// Hedef: `<...>` ya da boşluksuz, parantezleri dengeli, denetim karakteri olmayan dizi. Sonrası; değilse npos.
std::size_t destination_end(std::string_view row, std::size_t at) {
    if (row[at] == '<') {
        for (++at; at < row.size() && row[at] != '>'; ++at) {
            if (row[at] == '<')
                return std::string_view::npos;
            if (row[at] == '\\' && at + 1 < row.size())
                ++at;
        }
        return at < row.size() ? at + 1 : std::string_view::npos;
    }
    int depth = 0;
    for (; at < row.size() && row[at] != ' ' && row[at] != '\t'; ++at) {
        const char c = row[at];
        if (c == '\\' && at + 1 < row.size()) {
            ++at;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20)
            return std::string_view::npos;
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        if (depth < 0)
            return std::string_view::npos;
    }
    return depth == 0 ? at : std::string_view::npos;
}

// Başlık "..." '...' ya da (...), satır sonuna dek; ardında yalnız boşluk.
bool is_title(std::string_view rest) {
    const char open = rest.front();
    if (open != '"' && open != '\'' && open != '(')
        return false;
    const char close = open == '(' ? ')' : open;
    std::size_t at = 1;
    for (; at < rest.size() && rest[at] != close; ++at) {
        if (rest[at] == '\\' && at + 1 < rest.size())
            ++at;
        else if (open == '(' && rest[at] == '(')
            return false;
    }
    return at < rest.size() && is_blank(rest.substr(at + 1));
}

// Etiketin karşılaştırma biçimi, kod noktası kod noktası `emit`e. md4c gibi (md_label_hash, md_label_cmp) baştaki
// ve sondaki boşluk atılır, iç boşluk dizisi (Unicode boşluk ve satır sonu) tek boşluk olur, harfler md4c'nin
// tablosuyla katlanır (label_fold.hpp). ASCII yerel ayara bakmadan, tablo aramasız katlanır: çoğu etiket ASCII.
template <typename Emit>
void for_each_normalized(std::string_view label, const Emit& emit) {
    bool started = false;
    bool space = false;
    for (std::size_t at = 0; at < label.size();) {
        std::size_t size = 1;
        const char c = label[at];
        const bool ascii = static_cast<unsigned char>(c) < 0x80;
        const char32_t codepoint = ascii ? static_cast<char32_t>(c) : decode_utf8(label, at, size);
        at += size;
        if (is_unicode_whitespace(codepoint) || is_line_break(c)) {
            space = started;
            continue;
        }
        if (space)
            emit(U' ');
        space = false;
        started = true;
        if (ascii) {
            emit(c >= 'A' && c <= 'Z' ? codepoint + ('a' - 'A') : codepoint);
            continue;
        }
        const FoldedCodepoints folded = fold_codepoint(codepoint);
        for (std::size_t i = 0; i < folded.count; ++i)
            emit(folded.codepoints[i]);
    }
}

std::u32string normalized_label(std::string_view label) {
    std::u32string key;
    key.reserve(label.size());
    for_each_normalized(label, [&](char32_t c) { key += c; });
    return key;
}

// Normalleşmiş etiketin FNV-1a özeti (kod noktası başına bir adım), ayırma yapmadan; boş etiket 0 (aranmaz).
std::uint64_t label_hash(std::string_view label) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    bool any = false;
    for_each_normalized(label, [&](char32_t c) {
        hash = (hash ^ c) * kPrime;
        any = true;
    });
    return any ? hash : 0;
}

// Tek satırlık bağlantı başvuru tanımı (CommonMark): en çok 3 boşluk girinti, [etiket]: hedef, isteğe bağlı başlık.
// Şüphede tanım sayılmaz: eklenen satır tanım olmasaydı parçanın sonunda metin olarak görünürdü.
// Tanımsa etiketi (ayraçsız), değilse nullopt.
std::optional<std::string_view> link_definition_label(std::string_view row) {
    constexpr std::size_t kMaxIndent = 3;
    if (row.ends_with('\r'))
        row.remove_suffix(1);
    const std::size_t indent = row.find_first_not_of(' ');
    if (indent == std::string_view::npos || indent > kMaxIndent || row[indent] != '[')
        return std::nullopt;
    const std::size_t close = label_close(row, indent + 1);
    if (close == std::string_view::npos || close + 1 >= row.size() || row[close + 1] != ':')
        return std::nullopt;
    const std::string_view label = row.substr(indent + 1, close - indent - 1);
    std::size_t at = row.find_first_not_of(" \t", close + 2);
    if (at == std::string_view::npos)
        return std::nullopt;  // hedef sonraki satırda: çok satırlı tanım
    const std::size_t after = destination_end(row, at);
    if (after == std::string_view::npos)
        return std::nullopt;
    const std::size_t title = row.find_first_not_of(" \t", after);
    if (title == std::string_view::npos || (title > after && is_title(row.substr(title))))
        return label;
    return std::nullopt;
}

// Satırları sırayla gezer: `visit(row, at, line, after_blank, in_fence)`; in_fence satırdan önceki durumdur (satır
// açık kuşatılmış kodun içinde mi, ya da bilinmiyor mu). Metin açık kuşatılmış kodla bitiyorsa (ya da bilinmiyorsa)
// true. Kap (liste, alıntı) izlenmez: girintili çitin içinde ondan az girintili dolu satır, çit liste öğesindeyse
// öğeyi ve çiti kapatır, üst düzeydeyse içeriktir. Bundan sonra durum bilinemez; izleyici kesimi bir daha
// önermez (yanlış kesim kodu bölerdi, kesmemek yalnız parçayı büyütür).
template <typename Visit>
bool walk_lines(std::string_view text, const Visit& visit) {
    FenceRun open;  // açık kuşatılmış kod; mark '\0' ise yok
    bool unsure = false;
    bool after_blank = false;
    std::size_t line = 0;
    for (std::size_t at = 0; at < text.size(); ++line) {
        const std::size_t eol = std::min(text.find('\n', at), text.size());
        const std::string_view row = text.substr(at, eol - at);
        visit(row, at, line, after_blank, unsure || open.mark != '\0');
        // Satır başına sabit iş: çoğu satır harfle başlar, ne çit ne boş satırdır (20 MiB'ta ~1 milyon satır).
        const char first = row.empty() ? '\0' : row.front();
        const bool indented = first == ' ' || first == '\t' || first == '\r';
        const auto next_line = [&] {
            after_blank = row.empty() || (indented && is_blank(row));
            at = eol + 1;
        };
        const auto open_if_fence = [&] {
            const FenceRun fence = fence_in(row);
            // Ters tırnak çitinin bilgi dizgesinde ters tırnak olamaz (CommonMark): o satır satır içi koddur.
            if (fence.mark == '~' || (fence.mark == '`' && fence.rest.find('`') == std::string_view::npos))
                open = fence;
        };
        if (open.mark == '\0') {
            open_if_fence();
            next_line();
            continue;
        }
        // Kapanış en çok 3 sütun girintilidir: liste öğesindeki çitte öğe içeriğine göre, içerikten az girintili
        // satırda satır başına göre (md4c kapanışı kaba bakmadan önce dener).
        constexpr std::size_t kMaxFenceIndent = 3;
        const Indent leading = indentation(row);
        const std::size_t relative =
            open.in_list && leading.columns >= open.indent ? leading.columns - open.indent : leading.columns;
        const FenceRun fence = relative <= kMaxFenceIndent ? fence_run(row.substr(leading.bytes)) : FenceRun{};
        if (fence.mark == open.mark && fence.length >= open.length && is_blank(fence.rest)) {
            open = {};
        } else if (open.indent > 0 && !is_blank(row) && leading.columns < open.indent) {
            // Öğe içeriğinden az girintili dolu satır öğeyi ve çiti kapatır. Liste imli satırda açılan çitin kabı
            // kesindir; girintili çitin kabı bilinmez (üst düzeyde içerik olurdu).
            if (open.in_list) {
                open = {};
                open_if_fence();  // aynı satır yeni bir çit açabilir
            } else {
                unsure = true;
            }
        }
        next_line();
    }
    return unsure || open.mark != '\0';
}

// Tanım satırının normalleşmiş etiketi (satır toplanırken tanım olduğu doğrulandı).
std::u32string definition_label(std::string_view text, const MarkdownDefinition& definition) {
    return normalized_label(*link_definition_label(text.substr(definition.begin, definition.end - definition.begin)));
}

}  // namespace

// Tanımlar belge sırasıyla gelir: özete göre kararlı sıralanır. Tek tanımlı özet (çoğu) metne bakmaz, ayırma
// yapmaz. Aynı özetli grup (tekrar ya da çakışma) normalleşmiş etiketiyle bir kez kararlı sıralanır: aynı etiketin
// ilki (belge sırası korunur) kalır, sonrakiler tekrar olur; grup etikete göre sıralı kalır, arama ikili yapılır.
// Grup başına O(k log k): her tanımı öncekilerle karşılaştırmak, özeti çakışacak biçimde üretilmiş notta O(k²) idi.
void keep_first_definitions(std::string_view text, MarkdownDefinitions& definitions) {
    std::vector<MarkdownDefinition>& all = definitions.first;
    std::ranges::stable_sort(all, {}, &MarkdownDefinition::hash);
    std::vector<MarkdownDefinition> kept;
    kept.reserve(all.size());
    std::vector<std::pair<std::u32string, std::size_t>> group_labels;  // (etiket, all içindeki yer)
    for (std::size_t group = 0; group < all.size();) {
        std::size_t next = group + 1;
        while (next < all.size() && all[next].hash == all[group].hash)
            ++next;
        if (next == group + 1) {
            kept.push_back(all[group]);
            group = next;
            continue;
        }
        group_labels.clear();
        for (std::size_t i = group; i < next; ++i)
            group_labels.emplace_back(definition_label(text, all[i]), i);
        std::ranges::stable_sort(group_labels, {}, &std::pair<std::u32string, std::size_t>::first);
        for (std::size_t i = 0; i < group_labels.size(); ++i) {
            const MarkdownDefinition& entry = all[group_labels[i].second];
            if (i > 0 && group_labels[i].first == group_labels[i - 1].first)
                definitions.repeats.emplace_back(entry.begin, entry.end);
            else
                kept.push_back(entry);
        }
        group = next;
    }
    all = std::move(kept);
    std::ranges::sort(definitions.repeats);
}

MarkdownScan scan_markdown(std::string_view text, std::size_t target_bytes) {
    MarkdownScan scan{{{0, text.size(), 0}}, {}};
    std::vector<MarkdownChunk>& chunks = scan.chunks;
    MarkdownDefinitions& definitions = scan.definitions;
    bool previous_definition = false;
    const bool open_fence = walk_lines(text, [&](std::string_view row, std::size_t at, std::size_t line,
                                                 bool after_blank, bool in_fence) {
        if (!in_fence && after_blank && at - chunks.back().begin >= target_bytes && starts_top_level_block(row)) {
            chunks.back().end = at;
            chunks.push_back({at, text.size(), line});
        }
        // Hızlı ret: tanım satırı `[` ya da girintiyle başlar. Paragraf devamı tanım değildir.
        const std::optional<std::string_view> label =
            !in_fence && (line == 0 || after_blank || previous_definition) && !row.empty() &&
                    (row.front() == '[' || row.front() == ' ')
                ? link_definition_label(row)
                : std::nullopt;
        previous_definition = label.has_value();
        if (label)
            definitions.first.push_back({label_hash(*label), at, at + row.size()});
    });
    chunks.back().open_fence = open_fence;
    keep_first_definitions(text, definitions);
    return scan;
}

std::vector<MarkdownChunk> markdown_chunks(std::string_view text, std::size_t target_bytes) {
    return scan_markdown(text, target_bytes).chunks;
}

MarkdownDefinitions markdown_link_definitions(std::string_view text) {
    return scan_markdown(text).definitions;
}

std::string markdown_chunk_source(std::string_view text, const MarkdownChunk& chunk,
                                  const MarkdownDefinitions& definitions) {
    std::string source(text.substr(chunk.begin, chunk.end - chunk.begin));
    const auto first = std::ranges::lower_bound(definitions.repeats, chunk.begin, {},
                                                &std::pair<std::size_t, std::size_t>::first);
    for (auto repeat = first; repeat != definitions.repeats.end() && repeat->first < chunk.end; ++repeat)
        std::fill(source.begin() + static_cast<std::ptrdiff_t>(repeat->first - chunk.begin),
                  source.begin() + static_cast<std::ptrdiff_t>(repeat->second - chunk.begin), ' ');
    if (definitions.empty() || chunk.open_fence)
        return source;
    // Başvurunun her biçimi ([metin][etiket], [etiket][], [etiket]) köşeli ayraç içinde etiketi taşır: parçadaki
    // ayraç içleri aranır, başka parçada tanımlı olanların ilk tanımı belge sırasıyla eklenir.
    std::vector<std::pair<std::size_t, std::size_t>> wanted;
    const auto want = [&](std::string_view label) {
        const std::uint64_t hash = label_hash(label);
        if (hash == 0)
            return;
        auto [entry, to] = std::ranges::equal_range(definitions.first, hash, {}, &MarkdownDefinition::hash);
        if (entry == to)
            return;
        // Tek aday metinle doğrulanmaz: özet çakışmasında eklenen, parçanın anmadığı tanım etkisizdir. Çakışan grup
        // etikete göre sıralıdır (keep_first_definitions): ikili arama.
        if (to - entry > 1) {
            const std::u32string key = normalized_label(label);
            const auto label_less = [&](const MarkdownDefinition& definition, const std::u32string& wanted_label) {
                return definition_label(text, definition) < wanted_label;
            };
            entry = std::lower_bound(entry, to, key, label_less);
            if (entry == to || definition_label(text, *entry) != key)
                return;
        }
        if (entry->begin < chunk.begin || entry->begin >= chunk.end)  // kendi parçasındakini md4c yerinde çözer
            wanted.emplace_back(entry->begin, entry->end);
    };
    for (std::size_t open = source.find('['); open != std::string::npos; open = source.find('[', open + 1)) {
        // Tek sayıda ters bölüden sonraki `[` kaçışlıdır, etiket açmaz (md4c). Atlanmazsa `\[` dolu satırda her biri
        // 999 birimlik tarama başlatırdı; kaçışsız `[` taramaları birbirini aşmaz, parça başına doğrusal kalır.
        if (open > 0) {
            const std::size_t run_start = source.find_last_not_of('\\', open - 1);  // npos: satır başına dek ters bölü
            const std::size_t backslashes = run_start == std::string::npos ? open : open - run_start - 1;
            if (backslashes % 2 == 1)
                continue;
        }
        const std::size_t close = label_close(source, open + 1);
        if (close == std::string::npos)
            continue;
        const std::string_view label = std::string_view(source).substr(open + 1, close - open - 1);
        want(label);
        // Fazladan eklenen tanım çıktı üretmez: alıntı imli ve imsiz biçimin ikisi de aranır.
        if (label.find('\n') != std::string_view::npos)
            want(without_quote_marks(label));
    }
    std::ranges::sort(wanted);
    const auto [last, end] = std::ranges::unique(wanted);
    wanted.erase(last, end);
    if (wanted.empty())
        return source;
    source += "\n\n";
    for (const auto& [begin, end] : wanted) {
        std::string_view line = text.substr(begin, end - begin);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        source += line;
        source += '\n';
    }
    return source;
}

}  // namespace noteapp::app
