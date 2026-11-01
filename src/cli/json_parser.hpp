#pragma once

// `note graph import`'un JSON ayrıştırıcısı. Girdi güvenilmez: derinlik ve düğüm sayısı sınırlı, her sapma
// nullopt.
#include <charconv>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace noteapp::cli {

inline constexpr int kMaxJsonDepth = 6;
// Satır başına değer sayısı. Sınır olmasa 128 MiB'lik "[1,1,…]" yaklaşık 7 GiB ayırırdı. Bir düğümün gerçek
// satırı (etiketler + özellikler) bunun çok altındadır.
inline constexpr std::size_t kMaxJsonValues = 100000;

struct Json {
    enum class Kind { String, Number, Bool, Array, Object } kind = Kind::String;
    std::string text;  // String, Number; Bool'da "true"/"false"
    std::vector<Json> items;
    std::map<std::string, Json> fields;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    std::optional<Json> parse() {
        auto value = parse_value(0);
        skip_space();
        if (at_ != text_.size())
            return std::nullopt;
        return value;
    }

private:
    void skip_space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t'))
            ++at_;
    }
    bool take(char c) {
        skip_space();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }
    std::optional<Json> parse_value(int depth) {
        if (depth > kMaxJsonDepth || ++values_ > kMaxJsonValues)
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
            return Json{Json::Kind::String, std::move(*text), {}, {}};
        }
        return parse_scalar();
    }
    std::optional<Json> parse_scalar() {
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' && text_[at_] != ']' &&
               text_[at_] != ' ')
            ++at_;
        const std::string_view token = text_.substr(start, at_ - start);
        if (token == "true" || token == "false")
            return Json{Json::Kind::Bool, std::string(token), {}, {}};
        if (token.empty() || token.find_first_not_of("-0123456789") != std::string_view::npos)
            return std::nullopt;
        return Json{Json::Kind::Number, std::string(token), {}, {}};
    }
    std::optional<Json> parse_array(int depth) {
        ++at_;
        Json out{Json::Kind::Array, {}, {}, {}};
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
        Json out{Json::Kind::Object, {}, {}, {}};
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
        } else {
            out += static_cast<char>(0xE0U | (code >> 12U));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
    }
    bool parse_unicode_escape(std::string& out) {
        if (at_ + 4 > text_.size())
            return false;
        unsigned code = 0;
        const auto* begin = text_.data() + at_;
        const auto [end, error] = std::from_chars(begin, begin + 4, code, 16);
        if (error != std::errc{} || end != begin + 4)
            return false;
        at_ += 4;
        append_utf8(out, code);
        return true;
    }
    bool parse_escape(std::string& out) {
        if (at_ >= text_.size())
            return false;
        const char escape = text_[at_++];
        constexpr std::string_view kFrom = "\"\\/ntr";
        constexpr std::string_view kTo = "\"\\/\n\t\r";
        if (const auto simple = kFrom.find(escape); simple != std::string_view::npos) {
            out += kTo[simple];
            return true;
        }
        return escape == 'u' && parse_unicode_escape(out);
    }
    std::optional<std::string> parse_string() {
        if (at_ >= text_.size() || text_[at_] != '"')
            return std::nullopt;
        ++at_;
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"')
                return out;
            if (c != '\\')
                out += c;
            else if (!parse_escape(out))
                return std::nullopt;
        }
        return std::nullopt;
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::size_t values_ = 0;
};

}  // namespace noteapp::cli
