#include "app/data_preview.hpp"

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

// Liste imli satırda (`- `, `* `, `+ `, `12. `, `3) `; en çok 3 boşluk girinti) öğe içeriğinin başladığı sütun;
// değilse 0. İçerik aynı satırda çit olabilir (`- ```bash`).
std::size_t list_content_column(std::string_view row) {
    constexpr std::size_t kMaxIndent = 3;
    constexpr std::size_t kMaxDigits = 9;
    std::size_t at = row.find_first_not_of(' ');
    if (at == std::string_view::npos || at > kMaxIndent)
        return 0;
    if (row[at] == '-' || row[at] == '*' || row[at] == '+') {
        ++at;
    } else {
        const std::size_t digits_end = std::min(row.find_first_not_of("0123456789", at), row.size());
        if (digits_end == at || digits_end - at > kMaxDigits || digits_end >= row.size() ||
            (row[digits_end] != '.' && row[digits_end] != ')'))
            return 0;
        at = digits_end + 1;
    }
    if (at >= row.size() || (row[at] != ' ' && row[at] != '\t'))
        return 0;
    return std::min(row.find_first_not_of(" \t", at), row.size());
}

// Satırdaki çit: satır başında ya da liste iminden sonra. Girinti mutlak sütundur.
FenceRun fence_in(std::string_view row) {
    const char first = row.empty() ? '\0' : row.front();
    if (first == '`' || first == '~' || first == ' ')
        if (const FenceRun fence = fence_run(row); fence.mark != '\0')
            return fence;
    if (first != '-' && first != '*' && first != '+' && first != ' ' && (first < '0' || first > '9'))
        return {};
    const std::size_t content = list_content_column(row);
    if (content == 0)
        return {};
    FenceRun fence = fence_run(row.substr(content));
    fence.indent += content;
    fence.in_list = true;
    return fence;
}

// Boş satırdan sonra açık blokları (liste, alıntı, tablo, paragraf) kapatıp yenisini başlatan satır. Girinti
// (liste devamı, girintili kod), alıntı, liste imi ve sayıyla (sıralı liste) başlayanlar değil.
bool starts_top_level_block(std::string_view row) {
    constexpr std::string_view kContinuers = " \t>-+*0123456789";
    return !is_blank(row) && kContinuers.find(row.front()) == std::string_view::npos;
}

// Satır başında `[`ten sonra etiket: kaçışlı karakter dahil, iç içe `[` yok, boş değil, en çok 999 karakter.
// Etiketin `]`inden sonraki konum; değilse npos.
std::size_t label_end(std::string_view row, std::size_t at) {
    constexpr std::size_t kMaxLabel = 999;
    const std::size_t begin = at;
    bool blank = true;
    while (at < row.size() && row[at] != ']') {
        if (row[at] == '[')
            return std::string_view::npos;
        if (row[at] == '\\' && at + 1 < row.size()) {
            blank = false;
            at += 2;
            continue;
        }
        blank = blank && (row[at] == ' ' || row[at] == '\t');
        ++at;
    }
    if (at >= row.size() || blank || at - begin > kMaxLabel)
        return std::string_view::npos;
    return at + 1;
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

// Etiketin karşılaştırma biçimi (CommonMark: büyük/küçük harf ve boşluk dizisi fark etmez), karakter karakter
// `emit`e. Yalnız ASCII katlanır (yerel ayara bakan std::tolower yavaştı): md4c Unicode katlar, ASCII dışı harfte
// büyük/küçük farklı yazılmış tekrar ilk tanımı ezebilir.
template <typename Emit>
void for_each_normalized(std::string_view label, const Emit& emit) {
    bool started = false;
    bool space = false;
    for (const char c : label) {
        if (c == ' ' || c == '\t') {
            space = started;
            continue;
        }
        if (space)
            emit(' ');
        space = false;
        started = true;
        emit(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
}

std::string normalized_label(std::string_view label) {
    std::string key;
    key.reserve(label.size());
    for_each_normalized(label, [&](char c) { key += c; });
    return key;
}

// Normalleşmiş etiketin FNV-1a özeti, ayırma yapmadan; boş etiket 0 (aranmaz).
std::uint64_t label_hash(std::string_view label) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    bool any = false;
    for_each_normalized(label, [&](char c) {
        hash = (hash ^ static_cast<unsigned char>(c)) * kPrime;
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
    std::size_t at = label_end(row, indent + 1);
    if (at == std::string_view::npos || at >= row.size() || row[at] != ':')
        return std::nullopt;
    const std::string_view label = row.substr(indent + 1, at - indent - 2);
    at = row.find_first_not_of(" \t", at + 1);
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
        // Liste öğesindeki çitin kapanışı öğe içeriğine göre en çok 3 boşluk girintilidir.
        const std::size_t leading = std::min(row.find_first_not_of(' '), row.size());
        const std::string_view relative = open.in_list ? row.substr(std::min(leading, open.indent)) : row;
        const char lead = relative.empty() ? '\0' : relative.front();
        const FenceRun fence = lead == '`' || lead == '~' || lead == ' ' ? fence_run(relative) : FenceRun{};
        if (fence.mark == open.mark && fence.length >= open.length && is_blank(fence.rest)) {
            open = {};
        } else if (open.indent > 0 && !is_blank(row) && leading < open.indent) {
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

// Tanımlar belge sırasıyla gelir: özete göre kararlı sıralanır, aynı etiketin ilki kalır, sonrakiler tekrar olur.
// Aynı özetin farklı etiketleri (çakışma) metinle ayrılır.
void keep_first_definitions(std::string_view text, MarkdownDefinitions& definitions) {
    std::vector<MarkdownDefinition>& all = definitions.first;
    std::ranges::stable_sort(all, {}, &MarkdownDefinition::hash);
    std::vector<MarkdownDefinition> kept;
    kept.reserve(all.size());
    for (std::size_t group = 0; group < all.size();) {
        std::size_t next = group;
        while (next < all.size() && all[next].hash == all[group].hash)
            ++next;
        if (next == group + 1) {  // tek tanımlı etiket (çoğu): metne bakılmaz
            kept.push_back(all[group]);
            group = next;
            continue;
        }
        const std::size_t first_kept = kept.size();
        for (std::size_t i = group; i < next; ++i) {
            const MarkdownDefinition& entry = all[i];
            const std::string label = normalized_label(*link_definition_label(text.substr(entry.begin, entry.end - entry.begin)));
            const bool repeat = std::any_of(kept.begin() + static_cast<std::ptrdiff_t>(first_kept), kept.end(),
                                            [&](const MarkdownDefinition& other) {
                                                return normalized_label(*link_definition_label(
                                                           text.substr(other.begin, other.end - other.begin))) == label;
                                            });
            if (repeat)
                definitions.repeats.emplace_back(entry.begin, entry.end);
            else
                kept.push_back(entry);
        }
        group = next;
    }
    all = std::move(kept);
    std::ranges::sort(definitions.repeats);
}

}  // namespace

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
    for (std::size_t open = source.find('['); open != std::string::npos; open = source.find('[', open + 1)) {
        const std::size_t close = source.find_first_of("[]\n", open + 1);
        if (close == std::string::npos || source[close] != ']')
            continue;
        const std::string_view label = std::string_view(source).substr(open + 1, close - open - 1);
        const std::uint64_t hash = label_hash(label);
        if (hash == 0)
            continue;
        const auto [from, to] = std::ranges::equal_range(definitions.first, hash, {}, &MarkdownDefinition::hash);
        for (auto entry = from; entry != to; ++entry) {
            if (entry->begin >= chunk.begin && entry->begin < chunk.end)
                continue;  // kendi parçasında: md4c yerinde çözer
            const std::string_view row = text.substr(entry->begin, entry->end - entry->begin);
            // Tek aday metinle doğrulanmaz: özet çakışmasında eklenen, parçanın anmadığı tanım etkisizdir.
            if (from + 1 == to || normalized_label(*link_definition_label(row)) == normalized_label(label))
                wanted.emplace_back(entry->begin, entry->end);
        }
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
