#include "graph/cypher/lexer.hpp"

#include "graph/cypher/runtime.hpp"
#include "graph/op.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace noteapp::graph::detail::cypher {

namespace {

constexpr std::array<std::string_view, 3> kTwoByteSymbols = {"<>", "<=", ">="};
constexpr std::string_view kOneByteSymbols = "()[]{}:,.|=<>+-*/%;";

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// UTF-8 baytları ad sayılır: şemasız etiket ve anahtar Türkçe olabilir; geçerlilik belirteç bitince sınanır.
bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool is_name_part(char c) {
    return is_name_start(c) || is_digit(c);
}

void append_code_point(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::optional<std::uint32_t> hex_value(char c) {
    if (is_digit(c))
        return static_cast<std::uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint32_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<std::uint32_t>(c - 'A' + 10);
    return std::nullopt;
}

std::optional<char> simple_escape(char c) {
    switch (c) {
    case '\\':
    case '\'':
    case '"':
        return c;
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    default:
        return std::nullopt;
    }
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Result<std::vector<Token>> run() {
        while (true) {
            CYPHER_TRY(skip_trivia());
            if (at_ >= source_.size())
                break;
            CYPHER_TRY(next_token());
        }
        Token end;
        end.pos = position();
        end.begin = end.end = at_;
        tokens_.push_back(std::move(end));
        return std::move(tokens_);
    }

private:
    Position position() const { return Position{line_, col_}; }
    char peek(std::size_t offset = 0) const {
        return at_ + offset < source_.size() ? source_[at_ + offset] : '\0';
    }

    void advance(std::size_t count = 1) {
        for (std::size_t i = 0; i < count && at_ < source_.size(); ++i, ++at_) {
            const char c = source_[at_];
            if (c == '\n') {
                ++line_;
                col_ = 1;
            } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                ++col_;  // sütun kod noktası sayar: devam baytı ilerletmez
            }
        }
    }

    Error syntax(std::string what, Position pos) const {
        return located_error(Error::Code::Syntax, "graph.cypher_syntax", pos, {{"what", std::move(what)}});
    }

    Result<void> skip_trivia() {
        while (at_ < source_.size()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (at_ < source_.size() && peek() != '\n')
                    advance();
            } else if (c == '/' && peek(1) == '*') {
                CYPHER_TRY(skip_block_comment());
            } else {
                break;
            }
        }
        return {};
    }

    Result<void> skip_block_comment() {
        const Position start = position();
        advance(2);
        while (at_ < source_.size()) {
            if (peek() == '*' && peek(1) == '/') {
                advance(2);
                return {};
            }
            advance();
        }
        return std::unexpected(syntax("unterminated_comment", start));
    }

    void push(TokenKind kind, std::string text, Position pos, std::size_t begin) {
        Token token;
        token.kind = kind;
        token.text = std::move(text);
        token.pos = pos;
        token.begin = begin;
        token.end = at_;
        tokens_.push_back(std::move(token));
    }

    Result<void> next_token() {
        const char c = peek();
        if (is_digit(c))
            return number();
        if (is_name_start(c))
            return word(TokenKind::Word);
        if (c == '\'' || c == '"')
            return string_literal(c);
        if (c == '`')
            return quoted_name();
        if (c == '$')
            return parameter();
        return symbol();
    }

    Result<void> word(TokenKind kind) {
        const Position start = position();
        const std::size_t begin = at_;
        while (at_ < source_.size() && is_name_part(peek()))
            advance();
        const std::string_view text = source_.substr(begin, at_ - begin);
        if (!is_valid_utf8(text))
            return std::unexpected(syntax("invalid_utf8", start));
        push(kind, std::string(text), start, begin);
        return {};
    }

    Result<void> parameter() {
        const Position start = position();
        const std::size_t begin = at_;
        advance();
        if (!is_name_part(peek()))
            return std::unexpected(syntax("expected_parameter_name", start));
        const std::size_t name_begin = at_;
        while (at_ < source_.size() && is_name_part(peek()))
            advance();
        const std::string_view text = source_.substr(name_begin, at_ - name_begin);
        if (!is_valid_utf8(text))
            return std::unexpected(syntax("invalid_utf8", start));
        push(TokenKind::Param, std::string(text), start, begin);
        return {};
    }

    void skip_digits() {
        while (is_digit(peek()))
            advance();
    }

    Result<void> number() {
        const Position start = position();
        const std::size_t begin = at_;
        skip_digits();
        bool is_float = false;
        // "1..2" aralıktır, kesir değil: noktadan sonra rakam gerekir.
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            skip_digits();
        }
        const std::size_t sign = peek(1) == '+' || peek(1) == '-' ? 1 : 0;
        if ((peek() == 'e' || peek() == 'E') && is_digit(peek(1 + sign))) {
            is_float = true;
            advance(1 + sign);
            skip_digits();
        }
        push(is_float ? TokenKind::Float : TokenKind::Integer,
             std::string(source_.substr(begin, at_ - begin)), start, begin);
        return {};
    }

    Result<std::uint32_t> unicode_escape(Position pos) {
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const std::optional<std::uint32_t> digit = hex_value(peek());
            if (!digit)
                return std::unexpected(syntax("bad_unicode_escape", pos));
            code = code * 16 + *digit;
            advance();
        }
        // Vekil kod noktası UTF-8 olamaz; çifti birleştirmek de gerekmiyor: BMP dışı metin doğrudan yazılır.
        if (code >= 0xD800 && code <= 0xDFFF)
            return std::unexpected(syntax("bad_unicode_escape", pos));
        return code;
    }

    Result<void> escape_into(std::string& out) {
        const Position pos = position();
        advance();  // ters bölü
        const char c = peek();
        if (c == 'u') {
            advance();
            const Result<std::uint32_t> code = unicode_escape(pos);
            if (!code)
                return std::unexpected(code.error());
            append_code_point(out, *code);
            return {};
        }
        const std::optional<char> simple = simple_escape(c);
        if (!simple)
            return std::unexpected(syntax("bad_escape", pos));
        out += *simple;
        advance();
        return {};
    }

    Result<void> string_literal(char quote) {
        const Position start = position();
        const std::size_t begin = at_;
        advance();
        std::string text;
        while (at_ < source_.size() && peek() != quote) {
            if (peek() == '\\') {
                CYPHER_TRY(escape_into(text));
            } else {
                text += peek();
                advance();
            }
        }
        if (at_ >= source_.size())
            return std::unexpected(syntax("unterminated_string", start));
        advance();
        if (!is_valid_utf8(text))
            return std::unexpected(syntax("invalid_utf8", start));
        push(TokenKind::String, std::move(text), start, begin);
        return {};
    }

    Result<void> quoted_name() {
        const Position start = position();
        const std::size_t begin = at_;
        advance();
        const std::size_t name_begin = at_;
        while (at_ < source_.size() && peek() != '`')
            advance();
        if (at_ >= source_.size())
            return std::unexpected(syntax("unterminated_name", start));
        const std::string_view text = source_.substr(name_begin, at_ - name_begin);
        advance();
        if (text.empty() || !is_valid_utf8(text) || text.find('\0') != std::string_view::npos)
            return std::unexpected(syntax("bad_name", start));
        push(TokenKind::QuotedName, std::string(text), start, begin);
        return {};
    }

    Result<void> symbol() {
        const Position start = position();
        const std::size_t begin = at_;
        const std::string_view rest = source_.substr(at_, 2);
        if (rest == "..") {
            advance(2);
            push(TokenKind::Symbol, "..", start, begin);
            return {};
        }
        for (const std::string_view two : kTwoByteSymbols) {
            if (rest == two) {
                advance(2);
                push(TokenKind::Symbol, std::string(two), start, begin);
                return {};
            }
        }
        if (kOneByteSymbols.find(peek()) == std::string_view::npos)
            return std::unexpected(syntax("unexpected_character", start));
        const std::string text(1, peek());
        advance();
        push(TokenKind::Symbol, text, start, begin);
        return {};
    }

    std::string_view source_;
    std::size_t at_ = 0;
    int line_ = 1;
    int col_ = 1;
    std::vector<Token> tokens_;
};

}  // namespace

Result<std::vector<Token>> tokenize(std::string_view source) {
    return Lexer(source).run();
}

}  // namespace noteapp::graph::detail::cypher
