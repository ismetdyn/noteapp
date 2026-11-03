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

}  // namespace noteapp::app
