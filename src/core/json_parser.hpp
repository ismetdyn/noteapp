#pragma once

// RFC 8259 JSON ayrıştırıcısı (`note graph import`, LSP). Girdi güvenilmez: derinlik, değer sayısı ve boyut
// sınırlı, her sapma nullopt. Başlık yalnız standart kütüphaneye dayanır: fuzz hedefi core'a bağlanmadan
// derlenir.
#include <charconv>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace noteapp::core {

inline constexpr int kMaxJsonDepth = 6;
// Satır başına değer sayısı. Sınır olmasa 128 MiB'lik "[1,1,…]" yaklaşık 7 GiB ayırırdı. Bir düğümün gerçek
// satırı (etiketler + özellikler) bunun çok altındadır.
inline constexpr std::size_t kMaxJsonValues = 100000;

// Varsayılanlar import'unkidir; başka çağıran kendi sınırını verir.
struct JsonLimits {
    int max_depth = kMaxJsonDepth;  // kök 0
    std::size_t max_values = kMaxJsonValues;
    // Import satırı zaten okunurken sınırlanır (kMaxLineBytes).
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
};

struct Json {
    enum class Kind { String, Number, Bool, Array, Object, Null } kind = Kind::String;
    std::string text{};  // String: çözülmüş baytlar; Number: ham belirteç; Bool: "true"/"false"
    std::vector<Json> items{};
    std::map<std::string, Json> fields{};
    double number = 0;  // Number

    bool operator==(const Json&) const = default;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text, JsonLimits limits = {}) : text_(text), limits_(limits) {}
    std::optional<Json> parse() {
        if (text_.size() > limits_.max_bytes)
            return std::nullopt;
        auto value = parse_value(0);
        skip_space();
        if (!value || at_ != text_.size())
            return std::nullopt;
        return value;
    }

private:
    static constexpr unsigned kFirstPrintable = 0x20;
    static constexpr unsigned kHighSurrogateFirst = 0xD800;
    static constexpr unsigned kLowSurrogateFirst = 0xDC00;
    static constexpr unsigned kSurrogateEnd = 0xE000;
    static constexpr unsigned kSupplementaryFirst = 0x10000;
    static constexpr unsigned kSurrogateBits = 10;
    static constexpr std::size_t kHexDigits = 4;
    static constexpr int kHexBase = 16;

    static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    bool at(char c) const { return at_ < text_.size() && text_[at_] == c; }
    void skip_space() {
        while (at_ < text_.size() && is_space(text_[at_]))
            ++at_;
    }
    bool take(char c) {
        skip_space();
        if (!at(c))
            return false;
        ++at_;
        return true;
    }
    std::optional<Json> parse_value(int depth) {
        if (depth > limits_.max_depth || ++values_ > limits_.max_values)
            return std::nullopt;
        skip_space();
        if (at_ >= text_.size())
            return std::nullopt;
        const char c = text_[at_];
        if (c == '{')
            return parse_object(depth);
        if (c == '[')
            return parse_array(depth);
        if (c == '"') {
            auto text = parse_string();
            if (!text)
                return std::nullopt;
            return Json{.kind = Json::Kind::String, .text = std::move(*text)};
        }
        if (c == '-' || is_digit(c))
            return parse_number();
        return parse_literal();
    }
    std::optional<Json> parse_literal() {
        const std::string_view rest = text_.substr(at_);
        for (const std::string_view word : {"true", "false"})
            if (rest.starts_with(word)) {
                at_ += word.size();
                return Json{.kind = Json::Kind::Bool, .text = std::string(word)};
            }
        if (constexpr std::string_view kNull = "null"; rest.starts_with(kNull)) {
            at_ += kNull.size();
            return Json{.kind = Json::Kind::Null};
        }
        return std::nullopt;
    }
    bool skip_digits() {
        const std::size_t start = at_;
        while (at_ < text_.size() && is_digit(text_[at_]))
            ++at_;
        return at_ > start;
    }
    // -? (0 | [1-9][0-9]*) (.[0-9]+)? ([eE][+-]?[0-9]+)?  Ham belirteç text'te kalır: double'a sığmayan
    // tamsayıyı import int64 olarak kayıpsız okur.
    std::optional<Json> parse_number() {
        const std::size_t start = at_;
        if (at('-'))
            ++at_;
        if (at('0'))
            ++at_;
        else if (!skip_digits())
            return std::nullopt;
        if (at('.')) {
            ++at_;
            if (!skip_digits())
                return std::nullopt;
        }
        if (at('e') || at('E')) {
            ++at_;
            if (at('+') || at('-'))
                ++at_;
            if (!skip_digits())
                return std::nullopt;
        }
        const std::string_view token = text_.substr(start, at_ - start);
        double number = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), number);
        // Aralık dışı (1e999) reddedilir: RFC 8259 §6 aralığı uygulamaya bırakır.
        if (error != std::errc{} || end != token.data() + token.size())
            return std::nullopt;
        return Json{.kind = Json::Kind::Number, .text = std::string(token), .number = number};
    }
    std::optional<Json> parse_array(int depth) {
        ++at_;
        Json out{.kind = Json::Kind::Array};
        if (take(']'))
            return out;
        while (true) {
            auto item = parse_value(depth + 1);
            if (!item)
                return std::nullopt;
            out.items.push_back(std::move(*item));
            if (take(']'))
                return out;
            if (!take(','))
                return std::nullopt;
        }
    }
    std::optional<Json> parse_object(int depth) {
        ++at_;
        Json out{.kind = Json::Kind::Object};
        if (take('}'))
            return out;
        while (true) {
            skip_space();
            auto key = parse_string();
            if (!key || !take(':'))
                return std::nullopt;
            auto value = parse_value(depth + 1);
            if (!value)
                return std::nullopt;
            out.fields.insert_or_assign(std::move(*key), std::move(*value));
            if (take('}'))
                return out;
            if (!take(','))
                return std::nullopt;
        }
    }
    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0U | (code >> 6U));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        } else if (code < kSupplementaryFirst) {
            out += static_cast<char>(0xE0U | (code >> 12U));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        } else {
            out += static_cast<char>(0xF0U | (code >> 18U));
            out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
    }
    std::optional<unsigned> parse_hex4() {
        if (at_ + kHexDigits > text_.size())
            return std::nullopt;
        unsigned code = 0;
        const auto* begin = text_.data() + at_;
        const auto [end, error] = std::from_chars(begin, begin + kHexDigits, code, kHexBase);
        if (error != std::errc{} || end != begin + kHexDigits)
            return std::nullopt;
        at_ += kHexDigits;
        return code;
    }
    // Yalnız kalan vekil reddedilir: UTF-8'de karşılığı yoktur.
    bool parse_unicode_escape(std::string& out) {
        auto code = parse_hex4();
        if (!code || (*code >= kLowSurrogateFirst && *code < kSurrogateEnd))
            return false;
        if (*code >= kHighSurrogateFirst && *code < kLowSurrogateFirst) {
            if (!text_.substr(at_).starts_with("\\u"))
                return false;
            at_ += 2;
            const auto low = parse_hex4();
            if (!low || *low < kLowSurrogateFirst || *low >= kSurrogateEnd)
                return false;
            *code = kSupplementaryFirst + ((*code - kHighSurrogateFirst) << kSurrogateBits) +
                    (*low - kLowSurrogateFirst);
        }
        append_utf8(out, *code);
        return true;
    }
    bool parse_escape(std::string& out) {
        if (at_ >= text_.size())
            return false;
        const char escape = text_[at_++];
        constexpr std::string_view kFrom = "\"\\/bfnrt";
        constexpr std::string_view kTo = "\"\\/\b\f\n\r\t";
        if (const auto simple = kFrom.find(escape); simple != std::string_view::npos) {
            out += kTo[simple];
            return true;
        }
        return escape == 'u' && parse_unicode_escape(out);
    }
    std::optional<std::string> parse_string() {
        if (!at('"'))
            return std::nullopt;
        ++at_;
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < kFirstPrintable)
                return std::nullopt;
            if (c != '\\')
                out += c;
            else if (!parse_escape(out))
                return std::nullopt;
        }
        return std::nullopt;
    }

    std::string_view text_;
    JsonLimits limits_;
    std::size_t at_ = 0;
    std::size_t values_ = 0;
};

}  // namespace noteapp::core
