#include "detect/detect.hpp"
#include "tui/markdown_view.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::size_t kCsvSampleLines = 200;  // ayırıcı kararı için yeter; dev dosyada tüm satırları ayırmaz
constexpr int kJsonIndentCells = 2;

std::string_view without_bom(std::string_view text) {
    return text.starts_with(kBom) ? text.substr(kBom.size()) : text;
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

// RFC 4180'e yakın ayrıştırıcı: tırnaklı alanda ayırıcı ve satır sonu metindir, "" tek tırnaktır. Bozuk
// tırnak hata değildir: metin görünür kalır.
class CsvTable {
public:
    CsvTable(std::string_view text, char delimiter) : text_(text), delimiter_(delimiter), lines_(text) {}

    TableData parse() && {
        bool quoted = false;
        for (std::size_t at = 0; at < text_.size(); ++at)
            at = step(at, quoted);
        end_row();
        table_.header_rows = table_.rows.empty() ? 0 : 1;
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
        const bool keep = table_.rows.size() < kMaxTableRows && row_.size() < kMaxTableColumns;
        if (keep) {
            TextLook look;
            look.bold = table_.rows.empty();
            row_.emplace_back().append(display_safe(field_), look);
        }
        row_blank_ = row_blank_ && field_.empty();
        field_.clear();
    }

    void end_row() {
        end_field();
        const bool blank = row_blank_ && row_fields_ == 1;
        if (!blank) {
            table_.column_count = std::max(table_.column_count, row_fields_);
            if (table_.rows.size() < kMaxTableRows) {
                table_.rows.push_back(std::move(row_));
                table_.source_lines.push_back(lines_.line_of(row_start_));
            } else {
                ++table_.omitted_rows;
            }
        }
        row_.clear();
        row_fields_ = 0;
        row_blank_ = true;
    }

    std::string_view text_;
    char delimiter_;
    SourceLines lines_;
    TableData table_;
    std::vector<InlineText> row_;
    std::string field_;
    std::size_t row_start_ = 0;
    std::size_t row_fields_ = 0;
    bool row_blank_ = true;
};

// Katı RFC 8259 denetleyicisi ve girintileyici; yığın derinliği kMaxJsonDepth ile sınırlıdır. Bağımlılık yok:
// projede JSON kütüphanesi yok, yalnız bu görünüm için eklenmeye değmez.
class JsonPrinter {
public:
    JsonPrinter(std::string_view text, ViewBuilder& view) : text_(text), lines_(text), view_(view) {}

    bool print() {
        if (!value())
            return false;
        while (!stack_.empty())
            if (!after_value())
                return false;
        flush();
        skip_space();
        return pos_ == text_.size();
    }

private:
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    void take(std::string_view shown, std::size_t source_offset) {
        if (line_.empty())
            line_source_ = lines_.line_of(source_offset);
        line_ += shown;
    }

    void flush() {
        if (line_.empty())
            return;
        const int cells = std::min(static_cast<int>(stack_.size()) * kJsonIndentCells, view_.width() / 2);
        InlineText indent;
        indent.append(std::string(static_cast<std::size_t>(cells), ' '));
        SyntaxState state = kInitialSyntaxState;
        view_.add(indent, indent, highlighted_line(SyntaxLanguage::Json, line_, state, {}), line_source_);
        line_.clear();
    }

    bool after_value() {
        skip_space();
        const char opener = stack_.back();
        const char closer = opener == '{' ? '}' : ']';
        if (peek() == ',') {
            take(",", pos_);
            flush();
            ++pos_;
            return opener == '{' ? key() && value() : value();
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

    bool open(char opener, char closer) {
        if (static_cast<int>(stack_.size()) >= kMaxJsonDepth)
            return false;
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
        return opener == '{' ? key() && value() : value();
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
        take(display_safe(text_.substr(begin, pos_ - begin)), begin);
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
    SourceLines lines_;
    ViewBuilder& view_;
    std::size_t pos_ = 0;
    std::vector<char> stack_;
    std::string line_;
    std::size_t line_source_ = 0;
};

}  // namespace

std::optional<RenderedView> render_csv(std::string_view source, int width, CellWidthFn cell_width) {
    const std::string_view text = without_bom(source);
    const std::optional<char> delimiter = detect::table_delimiter(csv_sample(text));
    if (!delimiter)
        return std::nullopt;
    ViewBuilder view(width, cell_width);
    add_table(view, CsvTable(text, *delimiter).parse(), {});
    return std::move(view).finish();
}

std::optional<RenderedView> render_json(std::string_view source, int width, CellWidthFn cell_width) {
    ViewBuilder view(width, cell_width);
    if (!JsonPrinter(without_bom(source), view).print())
        return std::nullopt;
    return std::move(view).finish();
}

}  // namespace noteapp::tui
