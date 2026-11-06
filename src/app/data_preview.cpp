#include "app/data_preview.hpp"

#include "app/label_fold.hpp"
#include "detect/detect.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
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

// Bir satırın taraması: dizin ve hücre ayrıştırması aynı kural. Tırnak tırnaklı alanı açıp kapatır ("" tırnağın
// kendisi), tırnak dışındaki ayırıcı alanı, satır sonu satırı bitirir. Tek alanlı ve alanı boş (sondaki \r düşer)
// satır boştur, atlanır. `cells` doluysa hücreler kurulur (en çok `max_columns`); değilse yalnız sayılır.
struct RowScan {
    std::size_t next = 0;  // sonraki satırın başı
    std::size_t fields = 1;
    bool blank = false;
};

RowScan scan_row(std::string_view text, std::size_t at, char delimiter, std::vector<std::string>* cells,
                 std::size_t max_columns) {
    RowScan scan;
    std::string field;
    std::size_t content = 0;  // alan baytları (tırnaklar ve ayırıcılar hariç)
    char last = '\0';
    const auto end_field = [&] {
        if (cells == nullptr)
            return;
        if (field.ends_with('\r'))
            field.pop_back();
        if (cells->size() < max_columns)
            cells->push_back(std::move(field));
        field.clear();
    };
    bool quoted = false;
    for (; at < text.size(); ++at) {
        const char byte = text[at];
        if (byte == '"') {
            if (quoted && at + 1 < text.size() && text[at + 1] == '"') {
                ++content;
                if (cells != nullptr)
                    field += '"';
                ++at;
            } else {
                quoted = !quoted;
            }
        } else if (byte == delimiter && !quoted) {
            ++scan.fields;
            end_field();
        } else if (byte == '\n' && !quoted) {
            ++at;
            break;
        } else {
            ++content;
            last = byte;
            if (cells != nullptr)
                field += byte;
        }
    }
    end_field();
    scan.next = at;
    scan.blank = scan.fields == 1 && (content == 0 || (content == 1 && last == '\r'));
    return scan;
}

// Katı RFC 8259 denetleyicisi ve girintileyici. Özyinelemesiz: iç içelik `stack_`te (iç içe kap başına bir
// bayt), derinlik sınırı yok, çok derin not yığını taşırmaz. Bağımlılık yok: projede JSON kütüphanesi yok,
// yalnız bu görünüm için eklenmeye değmez.
class JsonPrinter {
public:
    // `index` verilirse metin kurulmaz: yalnız doğrulanır, satırlar sayılır, kontrol noktaları yazılır.
    JsonPrinter(std::string_view text, std::size_t base, JsonIndex* index = nullptr)
        : text_(text), base_(base), index_(index) {}

    std::optional<std::vector<JsonLine>> print() && {
        if (!value())
            return std::nullopt;
        while (need_value_ || !stack_.empty()) {
            const bool ok = std::exchange(need_value_, false) ? value() : after_value();
            if (!ok)
                return std::nullopt;
        }
        flush(pos_, 0);
        skip_space();
        if (pos_ != text_.size())
            return std::nullopt;
        if (index_ != nullptr)
            index_->line_count = count_;
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
        if (!has_line_)
            line_.offset = base_ + source_offset;
        has_line_ = true;
        if (index_ == nullptr)
            line_.text += shown;
    }

    // Satır biter; sıradaki satır `next_pos`ta (ya da sonrasındaki ilk belirteçte) `next_depth` iç içelikle başlar
    // (json_lines oradan sürdürür).
    void flush(std::size_t next_pos, std::size_t next_depth) {
        if (!has_line_)
            return;
        has_line_ = false;
        line_.depth = stack_.size();
        if (index_ == nullptr)
            lines_.push_back(std::exchange(line_, {}));
        ++count_;
        if (index_ == nullptr)
            return;
        // Nokta sıradaki belirtecin üstünde: araya giren boşluk pencere okumasında bir daha taranmaz.
        std::size_t next = next_pos;
        while (next < text_.size() && (text_[next] == ' ' || text_[next] == '\t' || text_[next] == '\n' ||
                                       text_[next] == '\r'))
            ++next;
        if (count_ % kJsonCheckpointLines == 0 || next - last_checkpoint_ >= kJsonCheckpointBytes) {
            index_->checkpoints.push_back({count_, base_ + next, next_depth});
            last_checkpoint_ = next;
        }
    }

    bool after_value() {
        skip_space();
        const char opener = stack_.back();
        const char closer = opener == '{' ? '}' : ']';
        if (peek() == ',') {
            take(",", pos_);
            flush(pos_ + 1, stack_.size());
            ++pos_;
            need_value_ = true;
            return opener != '{' || key();
        }
        if (peek() != closer)
            return false;
        flush(pos_, stack_.size());
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
        flush(pos_, stack_.size() + 1);
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
    JsonIndex* index_;
    std::size_t pos_ = 0;
    std::vector<char> stack_;
    bool need_value_ = false;  // kap açıldı ya da virgül okundu: sıradaki değer okunacak
    JsonLine line_;
    bool has_line_ = false;
    std::size_t count_ = 0;  // biten satırlar
    std::size_t last_checkpoint_ = 0;  // son kontrol noktasının yeri (text_ içinde)
    std::vector<JsonLine> lines_;
};

// Doğrulanmış JSON'un satırları: JsonPrinter'ın satır kuralı (kap açılınca ve virgülde satır biter, kapanış yeni
// satırda başlar, boş kap tek belirteç) yığınsız, yalnız iç içelik sayısıyla. Metin yalnız istenen satırlar için
// kurulur; öncekiler sayılıp geçilir.
class JsonLineReader {
public:
    JsonLineReader(std::string_view source, const JsonIndex::Checkpoint& from)
        : source_(source), pos_(from.pos), depth_(from.depth), line_(from.line) {}

    std::vector<JsonLine> read(std::size_t first, std::size_t count) && {
        first_ = first;
        const std::size_t last = first + std::min(count, std::numeric_limits<std::size_t>::max() - first);
        while (line_ < last) {
            skip_space();
            if (pos_ >= source_.size()) {
                end_line();
                break;
            }
            step();
        }
        return std::move(out_);
    }

private:
    void skip_space() {
        while (pos_ < source_.size() && (source_[pos_] == ' ' || source_[pos_] == '\t' || source_[pos_] == '\n' ||
                                         source_[pos_] == '\r'))
            ++pos_;
    }

    void put(std::string_view shown, std::size_t at) {
        if (!has_line_) {
            current_.offset = at;
            current_.depth = depth_;
            has_line_ = true;
        }
        if (line_ >= first_)
            current_.text += shown;
    }

    void end_line() {
        if (!has_line_)
            return;
        if (line_ >= first_)
            out_.push_back(std::exchange(current_, {}));
        else
            current_ = {};
        has_line_ = false;
        ++line_;
    }

    void step() {
        const char c = source_[pos_];
        switch (c) {
        case '{':
        case '[': {
            const char closer = c == '{' ? '}' : ']';
            const std::size_t at = pos_++;
            skip_space();
            if (pos_ < source_.size() && source_[pos_] == closer) {
                const char pair[] = {c, closer};
                put(std::string_view(pair, 2), at);
                ++pos_;
                return;
            }
            put(std::string_view(&source_[at], 1), at);
            end_line();
            ++depth_;
            return;
        }
        case '}':
        case ']':
            end_line();
            --depth_;
            put(std::string_view(&source_[pos_], 1), pos_);
            ++pos_;
            return;
        case ',':
            put(",", pos_++);
            end_line();
            return;
        case ':':
            put(": ", pos_++);
            return;
        case '"': {
            const std::size_t begin = pos_++;
            while (pos_ < source_.size() && source_[pos_] != '"')
                pos_ += source_[pos_] == '\\' ? 2 : 1;
            pos_ = std::min(pos_ + 1, source_.size());
            put(source_.substr(begin, pos_ - begin), begin);
            return;
        }
        default: {  // sayı ya da true/false/null
            const std::size_t begin = pos_;
            while (pos_ < source_.size() && std::string_view(" \t\n\r,:]}").find(source_[pos_]) == std::string_view::npos)
                ++pos_;
            put(source_.substr(begin, pos_ - begin), begin);
            return;
        }
        }
    }

    std::string_view source_;
    std::size_t pos_;
    std::size_t depth_;
    std::size_t line_;
    std::size_t first_ = 0;
    JsonLine current_;
    bool has_line_ = false;
    std::vector<JsonLine> out_;
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

std::optional<CsvIndex> index_csv(std::string_view source) {
    const std::size_t base = bom_size(source);
    const std::optional<char> delimiter = detect::table_delimiter(csv_sample(source.substr(base)));
    if (!delimiter)
        return std::nullopt;
    CsvIndex index;
    index.delimiter = *delimiter;
    for (std::size_t at = base; at < source.size();) {
        if (index.checkpoints.empty() || index.row_count - index.checkpoints.back().row >= kCsvBlockRows ||
            at - index.checkpoints.back().offset >= kCsvCheckpointBytes)
            index.checkpoints.push_back({index.row_count, at});
        const RowScan row = scan_row(source, at, index.delimiter, nullptr, 0);
        if (!row.blank) {
            ++index.row_count;
            index.column_count = std::max(index.column_count, row.fields);
        }
        at = row.next;
    }
    return index;
}

std::vector<CsvRow> csv_rows(std::string_view source, const CsvIndex& index, std::size_t first, std::size_t count,
                             std::size_t max_columns) {
    std::vector<CsvRow> rows;
    if (first >= index.row_count)
        return rows;
    count = std::min(count, index.row_count - first);
    rows.reserve(count);
    const auto after = std::ranges::upper_bound(index.checkpoints, first, {}, &CsvCheckpoint::row);
    std::size_t at = std::prev(after)->offset;
    std::size_t row = std::prev(after)->row;
    while (rows.size() < count && at < source.size()) {
        CsvRow out{at, {}};
        const RowScan scan = scan_row(source, at, index.delimiter, row >= first ? &out.cells : nullptr, max_columns);
        at = scan.next;
        if (scan.blank)
            continue;
        if (row++ >= first)
            rows.push_back(std::move(out));
    }
    return rows;
}

std::optional<CsvTable> parse_csv(std::string_view source, const TableLimits& limits) {
    std::optional<CsvIndex> index = index_csv(source);
    if (!index)
        return std::nullopt;
    CsvTable table;
    for (CsvRow& row : csv_rows(source, *index, 0, limits.rows, limits.columns)) {
        table.rows.push_back(std::move(row.cells));
        table.row_offsets.push_back(row.offset);
    }
    table.column_count = index->column_count;
    table.omitted_rows = index->row_count - table.rows.size();
    return table;
}

std::optional<std::vector<JsonLine>> pretty_json(std::string_view source) {
    const std::size_t base = bom_size(source);
    return JsonPrinter(source.substr(base), base).print();
}

std::optional<JsonIndex> index_json(std::string_view source) {
    const std::size_t base = bom_size(source);
    JsonIndex index;
    index.checkpoints.push_back({0, base, 0});
    if (!JsonPrinter(source.substr(base), base, &index).print())
        return std::nullopt;
    return index;
}

std::vector<JsonLine> json_lines(std::string_view source, const JsonIndex& index, std::size_t first,
                                 std::size_t count) {
    if (index.checkpoints.empty() || count == 0 || first >= index.line_count)
        return {};
    const auto after = std::ranges::upper_bound(index.checkpoints, first, {}, &JsonIndex::Checkpoint::line);
    return JsonLineReader(source, *std::prev(after)).read(first, count);
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

// Zorla kesilen kodun parçasına taşınan açılış satırı öneki: girinti ve (liste imli satırda) im, çit ve dil sözcüğü
// (renk için). Önekin TAMAMI `max_prefix_bytes`a sığmalı: sözcükle sığmazsa yalnız çit, çit de sığmazsa hiçbir şey
// (her parçaya dev satır kopyalanmasın; yarım çit taşınmaz).
std::size_t carried_fence_bytes(std::string_view row, const FenceRun& fence, std::size_t max_prefix_bytes) {
    const auto run_end = static_cast<std::size_t>(fence.rest.data() - row.data());
    if (run_end > max_prefix_bytes)
        return 0;  // dev çit koşusu taşınmaz; kısaltılmış çit kodu erken kapatırdı
    const std::size_t word = std::min(row.find_first_not_of(" \t", run_end), row.size());
    if (word > max_prefix_bytes)
        return run_end;  // sözcükten önceki dev boşluk taşınmaz, yalnız çit
    const std::size_t word_end = std::min(row.find_first_of(" \t\r", word), row.size());
    return word_end <= max_prefix_bytes ? word_end : run_end;
}

// GFM tablo ayraç satırı: `|`ler arasında yalnız `-` (en az bir), iki yanda isteğe bağlı `:` ve boşluk.
bool is_table_delimiter(std::string_view row) {
    if (row.ends_with('\r'))
        row.remove_suffix(1);
    bool dash = false;
    for (const char c : row) {
        if (c == '-')
            dash = true;
        else if (c != '|' && c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return dash && row.find('|') != std::string_view::npos;
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
// `opener`: açık çitin taşınacak öneki [başı, sonu) (carried_fence_bytes); kod bilinmiyorsa ya da yoksa boş.
template <typename Visit>
bool walk_lines(std::string_view text, std::size_t max_info_bytes, const Visit& visit) {
    FenceRun open;  // açık kuşatılmış kod; mark '\0' ise yok
    std::pair<std::size_t, std::size_t> opener;
    bool unsure = false;
    bool after_blank = false;
    std::size_t line = 0;
    for (std::size_t at = 0; at < text.size(); ++line) {
        const std::size_t eol = std::min(text.find('\n', at), text.size());
        const std::string_view row = text.substr(at, eol - at);
        visit(row, at, line, after_blank, unsure || open.mark != '\0',
              !unsure && open.mark != '\0' ? opener : std::pair<std::size_t, std::size_t>{});
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
            if (fence.mark == '~' || (fence.mark == '`' && fence.rest.find('`') == std::string_view::npos)) {
                open = fence;
                opener = {at, at + carried_fence_bytes(row, fence, max_info_bytes)};
            }
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
// Satır toplanırken tanım olduğu doğrulandı: yalnız etiket ayrılır (label_close, en çok 999 birim). Satırın tamamını
// yeniden doğrulamak özeti çakışan grupta başvuru başına uzun tanım satırını baştan sona tarardı.
std::u32string definition_label(std::string_view text, const MarkdownDefinition& definition) {
    const std::string_view row = text.substr(definition.begin, definition.end - definition.begin);
    const std::size_t open = row.find('[');
    const std::size_t close = label_close(row, open + 1);
    return normalized_label(row.substr(open + 1, close - open - 1));
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

MarkdownScan scan_markdown(std::string_view text, std::size_t target_bytes, std::size_t max_bytes,
                           const MarkdownChunkLimits& limits) {
    MarkdownScan scan{{{0, text.size(), 0}}, {}};
    std::vector<MarkdownChunk>& chunks = scan.chunks;
    MarkdownDefinitions& definitions = scan.definitions;
    bool previous_definition = false;
    std::size_t block_row = 0;                    // satırın bloğundaki sırası (boş satırdan sonra 0)
    std::pair<std::size_t, std::size_t> table{};  // bloğun tablo başlığı ve ayraç satırı (son satır sonu hariç)
    // Taşınan satır sayısı bir kez bilinir: çit öneki tek satır, tablo iki.
    const auto cut = [&](std::size_t at, std::size_t line, bool in_fence, std::pair<std::size_t, std::size_t> carry) {
        chunks.back().end = at;
        chunks.back().open_fence = in_fence;
        const std::size_t lines = carry.second == carry.first ? 0 : in_fence ? 1 : 2;
        chunks.push_back({at, text.size(), line, false, carry.first, carry.second, lines});
    };
    const bool open_fence = walk_lines(text, limits.fence_info_bytes, [&](std::string_view row, std::size_t at, std::size_t line,
                                                 bool after_blank, bool in_fence,
                                                 std::pair<std::size_t, std::size_t> opener) {
        block_row = after_blank || line == 0 ? 0 : block_row + 1;
        if (block_row == 0 || in_fence)
            table = {};  // koddaki tablo görünümlü satır tablo değildir
        if (block_row == 1 && !in_fence && is_table_delimiter(row) && line > 0) {
            const std::size_t header = text.rfind('\n', at - 1);
            const std::size_t header_begin = header == 0 ? 0 : text.rfind('\n', header - 1) + 1;  // npos + 1 == 0
            const std::size_t delimiter_end = at + row.size();
            if (text.substr(header_begin, header - header_begin).find('|') != std::string_view::npos &&
                delimiter_end - header_begin <= limits.table_header_bytes)  // dev başlık her parçaya kopyalanmasın
                table = {header_begin, delimiter_end};
        }
        const auto carry = in_fence ? opener : table;
        const std::size_t row_end = std::min(at + row.size() + 1, text.size());  // satır sonu dahil
        if (!in_fence && after_blank && at - chunks.back().begin >= target_bytes && starts_top_level_block(row))
            cut(at, line, false, {});
        else if (row_end - chunks.back().begin > max_bytes && at > chunks.back().begin && at > carry.second)
            cut(at, line, in_fence, carry);
        // Satırın kendisi sınırdan uzunsa içinden kesilir (son boşlukta, yoksa kod noktası sınırında).
        while (row_end - chunks.back().begin > max_bytes) {
            std::size_t split = chunks.back().begin + max_bytes;
            // Boşluk yalnız kesimin son yarısında aranır: boşluksuz dev satırda geri arama notun başına dek gitmesin.
            const std::size_t lower = std::max(at, chunks.back().begin + (max_bytes / 2));
            const std::size_t space = text.substr(lower, split - lower).rfind(' ');
            if (space != std::string_view::npos)
                split = lower + space + 1;
            while (split > chunks.back().begin + 1 && (static_cast<unsigned char>(text[split]) & 0xC0) == 0x80)
                --split;
            cut(split, line, in_fence, at > carry.second ? carry : std::pair<std::size_t, std::size_t>{});
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

std::vector<MarkdownChunk> markdown_chunks(std::string_view text, std::size_t target_bytes, std::size_t max_bytes) {
    return scan_markdown(text, target_bytes, max_bytes).chunks;
}

MarkdownDefinitions markdown_link_definitions(std::string_view text) {
    return scan_markdown(text).definitions;
}

MarkdownChunkLimits markdown_chunk_limits_of(const core::TuiLimits& limits) {
    return {.definition_bytes = limits.preview_definition_bytes.cap(),
            .fence_info_bytes = limits.preview_fence_info_bytes.cap(),
            .table_header_bytes = limits.preview_table_header_bytes.cap()};
}

std::string markdown_chunk_source(std::string_view text, const MarkdownChunk& chunk,
                                  const MarkdownDefinitions& definitions, const MarkdownChunkLimits& limits) {
    std::string source(text.substr(chunk.carry_begin, chunk.carry_end - chunk.carry_begin));
    if (!source.empty())
        source += '\n';
    const std::size_t carried = source.size();
    source += text.substr(chunk.begin, chunk.end - chunk.begin);
    const auto first = std::ranges::lower_bound(definitions.repeats, chunk.begin, {},
                                                &std::pair<std::size_t, std::size_t>::first);
    for (auto repeat = first; repeat != definitions.repeats.end() && repeat->first < chunk.end; ++repeat)
        std::fill(source.begin() + static_cast<std::ptrdiff_t>(carried + repeat->first - chunk.begin),
                  source.begin() + static_cast<std::ptrdiff_t>(carried + std::min(repeat->second, chunk.end) -
                                                               chunk.begin),
                  ' ');
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
    // İş bütçesi: tanımlar belge sırasıyla, bütçe dolana dek (sığmayan uzun tanım atlanır, sonrakiler denenir).
    // Parçanın md4c'ye verilen metni en çok parça + bütçe; bütçeyi aşan başvuru çözülmez.
    source += "\n\n";
    std::size_t budget = limits.definition_bytes;
    for (const auto& [begin, end] : wanted) {
        std::string_view line = text.substr(begin, end - begin);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.size() + 1 > budget)
            continue;
        budget -= line.size() + 1;
        source += line;
        source += '\n';
    }
    return source;
}

}  // namespace noteapp::app
