#include "tui/syntax.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace noteapp::tui {

namespace {

// Durum kodlaması (kod ailesi): 0 normal, 1 blok yorum, 2+i `strings[i]` çok satırlı dizgesi.
constexpr SyntaxState kInBlockComment = 1;
constexpr SyntaxState kFirstStringState = 2;
constexpr std::size_t npos = std::string_view::npos;
constexpr std::size_t kMaxFoldedKeyword = 32;  // SQL'de en uzun anahtar sözcüğü fazlasıyla aşar

constexpr std::string_view kJavaKeywords[] = {
    "abstract",   "assert",    "break",        "case",      "catch",  "class",   "continue",  "default",
    "do",         "else",      "enum",         "extends",   "final",  "finally", "for",       "if",
    "implements", "import",    "instanceof",   "interface", "native", "new",     "package",   "permits",
    "private",    "protected", "public",       "record",    "return", "sealed",  "static",    "strictfp",
    "super",      "switch",    "synchronized", "this",      "throw",  "throws",  "transient", "try",
    "var",        "volatile",  "while",        "yield"};
static_assert(std::ranges::is_sorted(kJavaKeywords));

constexpr std::string_view kJavaTypes[] = {"boolean", "byte", "char",  "double", "float",
                                           "int",     "long", "short", "void"};
static_assert(std::ranges::is_sorted(kJavaTypes));

constexpr std::string_view kJavaConstants[] = {"false", "null", "true"};
static_assert(std::ranges::is_sorted(kJavaConstants));

constexpr std::string_view kCppKeywords[] = {
    "alignas",   "alignof",   "auto",     "break",        "case",          "catch",       "class",
    "co_await",  "co_return", "co_yield", "concept",      "const",         "consteval",   "constexpr",
    "constinit", "continue",  "decltype", "default",      "delete",        "do",          "dynamic_cast",
    "else",      "enum",      "explicit", "export",       "extern",        "final",       "for",
    "friend",    "goto",      "if",       "inline",       "mutable",       "namespace",   "new",
    "noexcept",  "operator",  "override", "private",      "protected",     "public",      "reinterpret_cast",
    "requires",  "return",    "sizeof",   "static",       "static_assert", "static_cast", "struct",
    "switch",    "template",  "this",     "thread_local", "throw",         "try",         "typedef",
    "typeid",    "typename",  "union",    "using",        "virtual",       "volatile",    "while"};
static_assert(std::ranges::is_sorted(kCppKeywords));

constexpr std::string_view kCTypes[] = {"bool",     "char",    "double",   "float",    "int",
                                        "int16_t",  "int32_t", "int64_t",  "int8_t",   "long",
                                        "short",    "signed",  "size_t",   "uint16_t", "uint32_t",
                                        "uint64_t", "uint8_t", "unsigned", "void",     "wchar_t"};
static_assert(std::ranges::is_sorted(kCTypes));

constexpr std::string_view kCppConstants[] = {"NULL", "false", "nullptr", "true"};
static_assert(std::ranges::is_sorted(kCppConstants));

constexpr std::string_view kCKeywords[] = {
    "auto",   "break",  "case",   "const",   "continue", "default",  "do",       "else",   "enum",
    "extern", "for",    "goto",   "if",      "inline",   "register", "restrict", "return", "sizeof",
    "static", "struct", "switch", "typedef", "union",    "volatile", "while"};
static_assert(std::ranges::is_sorted(kCKeywords));

constexpr std::string_view kCConstants[] = {"NULL", "false", "true"};
static_assert(std::ranges::is_sorted(kCConstants));

constexpr std::string_view kPyKeywords[] = {
    "and",    "as",     "assert", "async",  "await",   "break",    "class", "continue", "def",
    "del",    "elif",   "else",   "except", "finally", "for",      "from",  "global",   "if",
    "import", "in",     "is",     "lambda", "match",   "nonlocal", "not",   "or",       "pass",
    "raise",  "return", "try",    "while",  "with",    "yield"};
static_assert(std::ranges::is_sorted(kPyKeywords));

constexpr std::string_view kPyTypes[] = {"bool", "bytes",  "dict", "float", "int",
                                         "list", "object", "set",  "str",   "tuple"};
static_assert(std::ranges::is_sorted(kPyTypes));

constexpr std::string_view kPyConstants[] = {"False", "None", "True"};
static_assert(std::ranges::is_sorted(kPyConstants));

constexpr std::string_view kShKeywords[] = {"case",   "do",     "done",     "elif",   "else",     "esac",
                                            "exec",   "export", "fi",       "for",    "function", "if",
                                            "in",     "local",  "readonly", "return", "set",      "shift",
                                            "source", "then",   "trap",     "unset",  "until",    "while"};
static_assert(std::ranges::is_sorted(kShKeywords));

constexpr std::string_view kJsKeywords[] = {
    "async",    "await",   "break",    "case",   "catch",  "class",  "const",      "continue",
    "debugger", "default", "delete",   "do",     "else",   "export", "extends",    "finally",
    "for",      "from",    "function", "if",     "import", "in",     "instanceof", "let",
    "new",      "of",      "return",   "static", "super",  "switch", "this",       "throw",
    "try",      "typeof",  "var",      "void",   "while",  "yield"};
static_assert(std::ranges::is_sorted(kJsKeywords));

constexpr std::string_view kJsConstants[] = {"Infinity", "NaN", "false", "null", "true", "undefined"};
static_assert(std::ranges::is_sorted(kJsConstants));

constexpr std::string_view kTsKeywords[] = {
    "abstract",   "as",        "async",     "await",   "break",      "case",      "catch",     "class",
    "const",      "continue",  "debugger",  "declare", "default",    "delete",    "do",        "else",
    "enum",       "export",    "extends",   "finally", "for",        "from",      "function",  "if",
    "implements", "import",    "in",        "infer",   "instanceof", "interface", "is",        "keyof",
    "let",        "namespace", "new",       "of",      "override",   "private",   "protected", "public",
    "readonly",   "return",    "satisfies", "static",  "super",      "switch",    "this",      "throw",
    "try",        "type",      "typeof",    "var",     "void",       "while",     "yield"};
static_assert(std::ranges::is_sorted(kTsKeywords));

constexpr std::string_view kTsTypes[] = {"any",    "bigint", "boolean", "never",  "number",
                                         "object", "string", "symbol",  "unknown"};
static_assert(std::ranges::is_sorted(kTsTypes));

constexpr std::string_view kGoKeywords[] = {
    "break", "case",   "chan",   "const",  "continue", "default", "defer",     "else", "fallthrough",
    "for",   "func",   "go",     "goto",   "if",       "import",  "interface", "map",  "package",
    "range", "return", "select", "struct", "switch",   "type",    "var"};
static_assert(std::ranges::is_sorted(kGoKeywords));

constexpr std::string_view kGoTypes[] = {"any",     "bool",    "byte",   "complex128", "complex64", "error",
                                         "float32", "float64", "int",    "int16",      "int32",     "int64",
                                         "int8",    "rune",    "string", "uint",       "uint16",    "uint32",
                                         "uint64",  "uint8",   "uintptr"};
static_assert(std::ranges::is_sorted(kGoTypes));

constexpr std::string_view kGoConstants[] = {"false", "iota", "nil", "true"};
static_assert(std::ranges::is_sorted(kGoConstants));

constexpr std::string_view kRustKeywords[] = {
    "Self",   "as",     "async",  "await", "break", "const",  "continue", "crate",  "dyn",
    "else",   "enum",   "extern", "fn",    "for",   "if",     "impl",     "in",     "let",
    "loop",   "match",  "mod",    "move",  "mut",   "pub",    "ref",      "return", "self",
    "static", "struct", "super",  "trait", "type",  "unsafe", "use",      "where",  "while"};
static_assert(std::ranges::is_sorted(kRustKeywords));

constexpr std::string_view kRustTypes[] = {"bool",  "char", "f32",  "f64", "i128", "i16", "i32", "i64",  "i8",
                                           "isize", "str",  "u128", "u16", "u32",  "u64", "u8",  "usize"};
static_assert(std::ranges::is_sorted(kRustTypes));

constexpr std::string_view kRustConstants[] = {"None", "false", "true"};
static_assert(std::ranges::is_sorted(kRustConstants));

constexpr std::string_view kSqlKeywords[] = {
    "add",    "all",      "alter",       "and",     "as",         "asc",    "begin",    "between", "by",
    "case",   "check",    "column",      "commit",  "constraint", "create", "cross",    "default", "delete",
    "desc",   "distinct", "drop",        "else",    "end",        "exists", "foreign",  "from",    "group",
    "having", "if",       "in",          "index",   "inner",      "insert", "into",     "is",      "join",
    "key",    "left",     "like",        "limit",   "not",        "offset", "on",       "or",      "order",
    "outer",  "primary",  "references",  "replace", "returning",  "right",  "rollback", "select",  "set",
    "table",  "then",     "transaction", "trigger", "union",      "unique", "update",   "values",  "view",
    "when",   "where",    "with"};
static_assert(std::ranges::is_sorted(kSqlKeywords));

constexpr std::string_view kSqlTypes[] = {"bigint",   "blob",    "boolean",   "char",   "date",
                                          "datetime", "decimal", "double",    "float",  "int",
                                          "integer",  "numeric", "real",      "serial", "smallint",
                                          "text",     "time",    "timestamp", "varchar"};
static_assert(std::ranges::is_sorted(kSqlTypes));

constexpr std::string_view kSqlConstants[] = {"false", "null", "true"};
static_assert(std::ranges::is_sorted(kSqlConstants));

constexpr std::string_view kJsonConstants[] = {"false", "null", "true"};
static_assert(std::ranges::is_sorted(kJsonConstants));

constexpr std::string_view kDockerKeywords[] = {
    "ADD",   "ARG",        "AS",      "CMD", "COPY",  "ENTRYPOINT", "ENV",  "EXPOSE", "FROM",   "HEALTHCHECK",
    "LABEL", "MAINTAINER", "ONBUILD", "RUN", "SHELL", "STOPSIGNAL", "USER", "VOLUME", "WORKDIR"};
static_assert(std::ranges::is_sorted(kDockerKeywords));

constexpr std::string_view kMakeKeywords[] = {"define", "else",   "endef", "endif",   "export",  "ifdef",
                                              "ifeq",   "ifndef", "ifneq", "include", "override"};
static_assert(std::ranges::is_sorted(kMakeKeywords));

constexpr std::string_view kTomlConstants[] = {"false", "inf", "nan", "true"};
static_assert(std::ranges::is_sorted(kTomlConstants));

constexpr std::string_view kYamlConstants[] = {"False", "No",   "Null", "True", "Yes",  "false",
                                               "no",    "null", "off",  "on",   "true", "yes"};
static_assert(std::ranges::is_sorted(kYamlConstants));

// Dosyadaki bir dizge biçimi; uzun açılış (`"""`) kısadan önce gelmelidir.
struct StringRule {
    std::string_view open;
    std::string_view close;
    bool multiline = false;
    bool escapes = true;
};

// Tek karakterle başlayan belirteç: `$x`, `@Override`.
struct PrefixMark {
    char mark = 0;
    TokenKind kind = TokenKind::Plain;
};

// Bir dilin bütün tanımı: yeni dil çoğunlukla yalnız bir satır tablodur.
struct CodeSpec {
    std::span<const std::string_view> line_comments = {};
    std::string_view block_open = {};
    std::string_view block_close = {};
    std::span<const StringRule> strings = {};
    std::span<const std::string_view> keywords = {};
    std::span<const std::string_view> types = {};
    std::span<const std::string_view> constants = {};
    std::span<const PrefixMark> marks = {};
    std::string_view ident_extra = {};
    bool ignore_case = false;
    bool comment_needs_boundary = false;  // `a#b` yorum değil
    bool directives = false;              // satır başında `#include`
    bool capitalized_types = false;       // `Foo` tiptir
    bool keys_before_colon = false;       // `"a":` ve `color:` anahtardır
    bool char_literals = false;           // Rust: `'a` ömürdür, yalnız `'a'` dizgedir
};

constexpr std::string_view kSlashComment[] = {"//"};
constexpr std::string_view kHashComment[] = {"#"};
constexpr std::string_view kDashComment[] = {"--"};

constexpr StringRule kCStrings[] = {{"\"", "\"", false, true}, {"'", "'", false, true}};
constexpr StringRule kPyStrings[] = {{"\"\"\"", "\"\"\"", true, true},
                                     {"'''", "'''", true, true},
                                     {"\"", "\"", false, true},
                                     {"'", "'", false, true}};
constexpr StringRule kShStrings[] = {{"\"", "\"", true, true}, {"'", "'", true, false}};
constexpr StringRule kJsStrings[] = {
    {"`", "`", true, true}, {"\"", "\"", false, true}, {"'", "'", false, true}};
constexpr StringRule kGoStrings[] = {
    {"`", "`", true, false}, {"\"", "\"", false, true}, {"'", "'", false, true}};
constexpr StringRule kRustStrings[] = {{"\"", "\"", true, true}};
constexpr StringRule kSqlStrings[] = {{"'", "'", true, false}, {"\"", "\"", false, false}};
constexpr StringRule kDoubleQuoteString[] = {{"\"", "\"", false, true}};
constexpr StringRule kTomlStrings[] = {{"\"\"\"", "\"\"\"", true, true},
                                       {"'''", "'''", true, false},
                                       {"\"", "\"", false, true},
                                       {"'", "'", false, false}};

constexpr PrefixMark kDollarMark[] = {{'$', TokenKind::Constant}};
constexpr PrefixMark kAnnotationMark[] = {{'@', TokenKind::Type}};
constexpr PrefixMark kAtRuleMark[] = {{'@', TokenKind::Keyword}};

constexpr CodeSpec kJava{.line_comments = kSlashComment,
                         .block_open = "/*",
                         .block_close = "*/",
                         .strings = kCStrings,
                         .keywords = kJavaKeywords,
                         .types = kJavaTypes,
                         .constants = kJavaConstants,
                         .marks = kAnnotationMark,
                         .capitalized_types = true};
constexpr CodeSpec kCpp{.line_comments = kSlashComment,
                        .block_open = "/*",
                        .block_close = "*/",
                        .strings = kCStrings,
                        .keywords = kCppKeywords,
                        .types = kCTypes,
                        .constants = kCppConstants,
                        .directives = true};
constexpr CodeSpec kC{.line_comments = kSlashComment,
                      .block_open = "/*",
                      .block_close = "*/",
                      .strings = kCStrings,
                      .keywords = kCKeywords,
                      .types = kCTypes,
                      .constants = kCConstants,
                      .directives = true};
constexpr CodeSpec kPython{.line_comments = kHashComment,
                           .strings = kPyStrings,
                           .keywords = kPyKeywords,
                           .types = kPyTypes,
                           .constants = kPyConstants,
                           .marks = kAnnotationMark,
                           .capitalized_types = true};
constexpr CodeSpec kShell{.line_comments = kHashComment,
                          .strings = kShStrings,
                          .keywords = kShKeywords,
                          .marks = kDollarMark,
                          .comment_needs_boundary = true};
constexpr CodeSpec kJavaScript{.line_comments = kSlashComment,
                               .block_open = "/*",
                               .block_close = "*/",
                               .strings = kJsStrings,
                               .keywords = kJsKeywords,
                               .constants = kJsConstants,
                               .capitalized_types = true};
constexpr CodeSpec kTypeScript{.line_comments = kSlashComment,
                               .block_open = "/*",
                               .block_close = "*/",
                               .strings = kJsStrings,
                               .keywords = kTsKeywords,
                               .types = kTsTypes,
                               .constants = kJsConstants,
                               .marks = kAnnotationMark,
                               .capitalized_types = true};
constexpr CodeSpec kGo{.line_comments = kSlashComment,
                       .block_open = "/*",
                       .block_close = "*/",
                       .strings = kGoStrings,
                       .keywords = kGoKeywords,
                       .types = kGoTypes,
                       .constants = kGoConstants};
constexpr CodeSpec kRust{.line_comments = kSlashComment,
                         .block_open = "/*",
                         .block_close = "*/",
                         .strings = kRustStrings,
                         .keywords = kRustKeywords,
                         .types = kRustTypes,
                         .constants = kRustConstants,
                         .capitalized_types = true,
                         .char_literals = true};
constexpr CodeSpec kSql{.line_comments = kDashComment,
                        .block_open = "/*",
                        .block_close = "*/",
                        .strings = kSqlStrings,
                        .keywords = kSqlKeywords,
                        .types = kSqlTypes,
                        .constants = kSqlConstants,
                        .ignore_case = true};
constexpr CodeSpec kJson{
    .strings = kDoubleQuoteString, .constants = kJsonConstants, .keys_before_colon = true};
constexpr CodeSpec kCss{.block_open = "/*",
                        .block_close = "*/",
                        .strings = kCStrings,
                        .marks = kAtRuleMark,
                        .ident_extra = "-",
                        .keys_before_colon = true};
constexpr CodeSpec kToml{.line_comments = kHashComment, .strings = kTomlStrings, .constants = kTomlConstants};
constexpr CodeSpec kYamlFlow{.line_comments = kHashComment,
                             .strings = kCStrings,
                             .constants = kYamlConstants,
                             .comment_needs_boundary = true};
constexpr CodeSpec kDockerfile{.line_comments = kHashComment,
                               .strings = kCStrings,
                               .keywords = kDockerKeywords,
                               .marks = kDollarMark,
                               .comment_needs_boundary = true};
constexpr CodeSpec kMakefile{.line_comments = kHashComment,
                             .strings = kCStrings,
                             .keywords = kMakeKeywords,
                             .marks = kDollarMark,
                             .comment_needs_boundary = true};

// Yalnız nullptr olmayan çıktıya yazar: görünmeyen satırda durum ilerletmenin bedeli düşük kalsın.
class Sink {
public:
    explicit Sink(std::vector<SyntaxSpan>* out) : out_(out) {}
    bool enabled() const { return out_ != nullptr; }
    void add(std::size_t begin, std::size_t end, TokenKind kind) const {
        if (out_ && begin < end && kind != TokenKind::Plain)
            out_->push_back({begin, end, kind});
    }

private:
    std::vector<SyntaxSpan>* out_;
};

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_ascii_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool is_ascii_lower(char c) {
    return c >= 'a' && c <= 'z';
}

// ASCII olmayan bayt tanımlayıcı sayılır: Türkçe ad bölünmesin, geçersiz bayt da zararsız kalsın.
bool is_word_char(char c) {
    return is_ascii_lower(c) || is_ascii_upper(c) || is_ascii_digit(c) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

char folded(char c) {
    return is_ascii_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

std::size_t skip_spaces(std::string_view line, std::size_t at) {
    while (at < line.size() && is_space(line[at]))
        ++at;
    return at;
}

// Sondaki boşlukları atar.
std::size_t trim_end(std::string_view line, std::size_t begin, std::size_t end) {
    while (end > begin && is_space(line[end - 1]))
        --end;
    return end;
}

bool has_word(std::span<const std::string_view> words, std::string_view word) {
    return std::ranges::binary_search(words, word);
}

// Kod ailesinin tek geçişli taraması: her dal en az bir bayt tüketir ve geri dönmez; bu yüzden süre
// doğrusaldır.
class CodeScanner {
public:
    CodeScanner(const CodeSpec& spec, std::string_view line, Sink sink)
        : spec_(spec), line_(line), sink_(sink) {}

    SyntaxState run(SyntaxState entry, std::size_t from = 0) {
        pos_ = from;
        first_code_ = skip_spaces(line_, from);
        resume(entry);
        while (pos_ < line_.size() && state_ == kInitialSyntaxState)
            step();
        return state_;
    }

private:
    bool is_ident(char c) const {
        return is_word_char(c) || spec_.ident_extra.find(c) != std::string_view::npos;
    }
    void resume(SyntaxState entry);
    void step();
    void scan_number();
    void scan_ident();
    TokenKind classify(std::string_view word) const;
    TokenKind lookup(std::string_view word) const;
    bool is_key_colon_at(std::size_t at) const;
    bool scan_line_comment();
    bool scan_block_comment();
    void finish_block_comment(std::size_t begin, std::size_t search_from);
    bool scan_string();
    void finish_string(std::size_t index, std::size_t begin, std::size_t search_from);
    std::size_t string_end(const StringRule& rule, std::size_t from) const;
    bool scan_mark();
    std::size_t mark_end(std::size_t at);
    std::size_t closing_of(char close, std::size_t at, bool& known_missing) const;
    bool scan_directive();
    bool scan_char_literal();

    const CodeSpec& spec_;
    std::string_view line_;
    Sink sink_;
    std::size_t pos_ = 0;
    std::size_t first_code_ = 0;
    SyntaxState state_ = kInitialSyntaxState;
    bool no_closing_brace_ = false;  // aynı aramayı tekrarlamak `${${${…` satırını karesel yapardı
    bool no_closing_paren_ = false;
};

void CodeScanner::resume(SyntaxState entry) {
    if (entry == kInBlockComment && !spec_.block_close.empty()) {
        finish_block_comment(pos_, pos_);
    } else if (entry >= kFirstStringState) {
        const auto index = static_cast<std::size_t>(entry - kFirstStringState);
        if (index < spec_.strings.size() && spec_.strings[index].multiline)
            finish_string(index, pos_, pos_);
    }
}

void CodeScanner::step() {
    const char c = line_[pos_];
    if (is_space(c)) {  // NOLINT(bugprone-branch-clone) -- boşluk ve tanınmayan bayt aynı atlar; sıra anlamlı
        ++pos_;
    } else if (is_ascii_digit(c)) {
        scan_number();
    } else if (is_ident(c)) {
        scan_ident();
    } else if (!(scan_line_comment() || scan_block_comment() || scan_string() || scan_mark() ||
                 scan_directive() || scan_char_literal())) {
        ++pos_;
    }
}

void CodeScanner::scan_number() {
    const std::size_t begin = pos_;
    while (pos_ < line_.size() && (is_word_char(line_[pos_]) || line_[pos_] == '.'))
        ++pos_;
    sink_.add(begin, pos_, TokenKind::Number);
}

bool CodeScanner::is_key_colon_at(std::size_t at) const {
    if (!spec_.keys_before_colon || at >= line_.size() || line_[at] != ':')
        return false;
    return at + 1 == line_.size() || !is_ident(line_[at + 1]);
}

void CodeScanner::scan_ident() {
    const std::size_t begin = pos_;
    while (pos_ < line_.size() && is_ident(line_[pos_]))
        ++pos_;
    if (sink_.enabled())
        sink_.add(begin, pos_,
                  is_key_colon_at(pos_) ? TokenKind::Key : classify(line_.substr(begin, pos_ - begin)));
}

TokenKind CodeScanner::classify(std::string_view word) const {
    if (!spec_.ignore_case)
        return lookup(word);
    if (word.size() > kMaxFoldedKeyword)
        return TokenKind::Plain;
    std::array<char, kMaxFoldedKeyword> buffer{};
    std::ranges::transform(word, buffer.begin(), folded);
    return lookup(std::string_view(buffer.data(), word.size()));
}

TokenKind CodeScanner::lookup(std::string_view word) const {
    if (has_word(spec_.keywords, word))
        return TokenKind::Keyword;
    if (has_word(spec_.types, word))
        return TokenKind::Type;
    if (has_word(spec_.constants, word))
        return TokenKind::Constant;
    const bool capitalized = is_ascii_upper(word.front()) && std::ranges::any_of(word, is_ascii_lower);
    return spec_.capitalized_types && capitalized ? TokenKind::Type : TokenKind::Plain;
}

bool CodeScanner::scan_line_comment() {
    if (spec_.comment_needs_boundary && pos_ > 0 && !is_space(line_[pos_ - 1]))
        return false;
    const std::string_view rest = line_.substr(pos_);
    if (!std::ranges::any_of(spec_.line_comments,
                             [rest](std::string_view open) { return rest.starts_with(open); }))
        return false;
    sink_.add(pos_, line_.size(), TokenKind::Comment);
    pos_ = line_.size();
    return true;
}

bool CodeScanner::scan_block_comment() {
    if (spec_.block_open.empty() || !line_.substr(pos_).starts_with(spec_.block_open))
        return false;
    finish_block_comment(pos_, pos_ + spec_.block_open.size());
    return true;
}

void CodeScanner::finish_block_comment(std::size_t begin, std::size_t search_from) {
    const std::size_t close = line_.find(spec_.block_close, search_from);
    if (close == npos) {
        sink_.add(begin, line_.size(), TokenKind::Comment);
        pos_ = line_.size();
        state_ = kInBlockComment;
        return;
    }
    pos_ = close + spec_.block_close.size();
    sink_.add(begin, pos_, TokenKind::Comment);
}

bool CodeScanner::scan_string() {
    const std::string_view rest = line_.substr(pos_);
    for (std::size_t i = 0; i < spec_.strings.size(); ++i) {
        if (rest.starts_with(spec_.strings[i].open)) {
            finish_string(i, pos_, pos_ + spec_.strings[i].open.size());
            return true;
        }
    }
    return false;
}

void CodeScanner::finish_string(std::size_t index, std::size_t begin, std::size_t search_from) {
    const StringRule& rule = spec_.strings[index];
    const std::size_t end = string_end(rule, search_from);
    if (end == npos) {
        sink_.add(begin, line_.size(), TokenKind::String);
        pos_ = line_.size();
        if (rule.multiline)
            state_ = static_cast<SyntaxState>(kFirstStringState + index);
        return;
    }
    pos_ = end;
    const bool is_key = spec_.keys_before_colon && skip_spaces(line_, end) < line_.size() &&
                        line_[skip_spaces(line_, end)] == ':';
    sink_.add(begin, end, is_key ? TokenKind::Key : TokenKind::String);
}

std::size_t CodeScanner::string_end(const StringRule& rule, std::size_t from) const {
    for (std::size_t at = from; at < line_.size();) {
        if (rule.escapes && line_[at] == '\\') {
            at += 2;
        } else if (line_.compare(at, rule.close.size(), rule.close) == 0) {
            return at + rule.close.size();
        } else {
            ++at;
        }
    }
    return npos;
}

bool CodeScanner::scan_mark() {
    const char c = line_[pos_];
    for (const PrefixMark& mark : spec_.marks) {
        if (mark.mark == c) {
            const std::size_t end = mark_end(pos_ + 1);
            sink_.add(pos_, end, mark.kind);
            pos_ = end;
            return true;
        }
    }
    return false;
}

std::size_t CodeScanner::closing_of(char close, std::size_t at, bool& known_missing) const {
    if (known_missing)
        return npos;
    const std::size_t found = line_.find(close, at);
    known_missing = found == npos;
    return found;
}

// `$x`, `${x}`, `$(x)`, `$@`, `@Override`: kapanış yoksa açılışın kendisi.
std::size_t CodeScanner::mark_end(std::size_t at) {
    if (at >= line_.size())
        return line_.size();
    if (line_[at] == '{' || line_[at] == '(') {
        const std::size_t close = line_[at] == '{' ? closing_of('}', at, no_closing_brace_)
                                                   : closing_of(')', at, no_closing_paren_);
        return close == npos ? at + 1 : close + 1;
    }
    if (!is_ident(line_[at]))
        return is_space(line_[at]) ? at : at + 1;
    while (at < line_.size() && is_ident(line_[at]))
        ++at;
    return at;
}

bool CodeScanner::scan_directive() {
    if (!spec_.directives || line_[pos_] != '#' || pos_ != first_code_)
        return false;
    std::size_t end = pos_ + 1;
    while (end < line_.size() && is_word_char(line_[end]))
        ++end;
    sink_.add(pos_, end, TokenKind::Keyword);
    pos_ = end;
    return true;
}

// Rust: `'a'` ve `'\n'` karakterdir; `'a` ömür belirtecidir, satırın geri kalanını dizge yapmamalı.
bool CodeScanner::scan_char_literal() {
    if (!spec_.char_literals || line_[pos_] != '\'')
        return false;
    constexpr std::size_t kMaxCharLiteralBytes = 8;  // `'\u{10FFFF}'` dışında 4 bayt + kaçış
    const std::size_t limit = std::min(line_.size(), pos_ + kMaxCharLiteralBytes);
    for (std::size_t at = pos_ + 1; at < limit; ++at) {
        if (line_[at] == '\\') {
            ++at;
        } else if (line_[at] == '\'' && at > pos_ + 1) {
            sink_.add(pos_, at + 1, TokenKind::String);
            pos_ = at + 1;
            return true;
        } else if (is_ascii_lower(line_[at]) && at > pos_ + 1) {
            break;  // `'ab`: ömür
        }
    }
    ++pos_;
    return true;
}

SyntaxState scan_code(const CodeSpec& spec, std::string_view line, SyntaxState entry, Sink sink,
                      std::size_t from = 0) {
    return CodeScanner(spec, line, sink).run(entry, from);
}

// `A="b" # c`: tırnaklı değer dizge, kalan yorum; değilse satır sonu yorumuna dek değer. Uzun tek satırda
// geri dönüş yok: kapanış aranır, bulunmazsa satır biter.
void scan_scalar(std::string_view line, std::size_t at, Sink sink, TokenKind bare) {
    at = skip_spaces(line, at);
    if (at >= line.size())
        return;
    std::size_t rest = at;
    if (line[at] == '"' || line[at] == '\'') {
        const char quote = line[at];
        rest = at + 1;
        while (rest < line.size() && line[rest] != quote)
            rest += line[rest] == '\\' && quote == '"' ? 2 : 1;
        rest = std::min(rest + 1, line.size());
        sink.add(at, rest, TokenKind::String);
    }
    const std::size_t comment = line.find(" #", rest);
    const std::size_t end = trim_end(line, rest, comment == npos ? line.size() : comment);
    if (rest == at)
        sink.add(at, end, bare);
    if (comment != npos)
        sink.add(comment + 1, line.size(), TokenKind::Comment);
}

SyntaxState scan_env(std::string_view line, SyntaxState, Sink sink) {
    constexpr std::string_view kExport = "export ";
    std::size_t at = skip_spaces(line, 0);
    if (at < line.size() && line[at] == '#') {
        sink.add(at, line.size(), TokenKind::Comment);
        return kInitialSyntaxState;
    }
    if (line.substr(at).starts_with(kExport)) {
        sink.add(at, at + kExport.size() - 1, TokenKind::Keyword);
        at = skip_spaces(line, at + kExport.size());
    }
    const std::size_t eq = line.find('=', at);
    if (eq == npos)
        return kInitialSyntaxState;
    sink.add(at, trim_end(line, at, eq), TokenKind::Key);
    scan_scalar(line, eq + 1, sink, TokenKind::Value);
    return kInitialSyntaxState;
}

// `anahtar = değer` ve `anahtar: değer`; değerin tamamı metindir (java properties'te `#` değere dahildir).
void scan_key_value(std::string_view line, std::size_t at, Sink sink) {
    const std::size_t separator = line.find_first_of("=:", at);
    if (separator == npos)
        return;
    sink.add(at, trim_end(line, at, separator), TokenKind::Key);
    const std::size_t value = skip_spaces(line, separator + 1);
    sink.add(value, trim_end(line, value, line.size()), TokenKind::Value);
}

SyntaxState scan_properties(std::string_view line, SyntaxState, Sink sink) {
    const std::size_t at = skip_spaces(line, 0);
    if (at < line.size() && (line[at] == '#' || line[at] == '!'))
        sink.add(at, line.size(), TokenKind::Comment);
    else
        scan_key_value(line, at, sink);
    return kInitialSyntaxState;
}

SyntaxState scan_ini(std::string_view line, SyntaxState, Sink sink) {
    const std::size_t at = skip_spaces(line, 0);
    if (at >= line.size())
        return kInitialSyntaxState;
    if (line[at] == ';' || line[at] == '#')
        sink.add(at, line.size(), TokenKind::Comment);
    else if (line[at] == '[')
        sink.add(at, trim_end(line, at, line.size()), TokenKind::Tag);
    else
        scan_key_value(line, at, sink);
    return kInitialSyntaxState;
}

bool is_toml_key(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](char c) {
        return is_word_char(c) || is_space(c) || c == '.' || c == '-' || c == '"' || c == '\'';
    });
}

SyntaxState scan_toml(std::string_view line, SyntaxState entry, Sink sink) {
    if (entry != kInitialSyntaxState)
        return scan_code(kToml, line, entry, sink);
    const std::size_t at = skip_spaces(line, 0);
    if (at >= line.size())
        return kInitialSyntaxState;
    // `[a.b]` tablo başlığıdır; virgülle süren satır çok satırlı dizinin parçasıdır.
    const std::size_t comment = line.find(" #");
    const std::size_t end = trim_end(line, at, comment == npos ? line.size() : comment);
    if (line[at] == '[' && line[end - 1] == ']' && line.find(',', at) == npos) {
        sink.add(at, end, TokenKind::Tag);
        sink.add(comment == npos ? line.size() : comment + 1, line.size(), TokenKind::Comment);
        return kInitialSyntaxState;
    }
    const std::size_t eq = line.find('=', at);
    if (eq != npos && line[at] != '#' && is_toml_key(line.substr(at, eq - at))) {
        sink.add(at, trim_end(line, at, eq), TokenKind::Key);
        return scan_code(kToml, line, kInitialSyntaxState, sink, eq + 1);
    }
    return scan_code(kToml, line, kInitialSyntaxState, sink);
}

bool is_number_text(std::string_view text) {
    return !text.empty() && is_ascii_digit(text.front()) &&
           std::ranges::all_of(text, [](char c) { return is_ascii_digit(c) || c == '.' || c == '_'; });
}

// `key: don't stop`: tırnak ortadaysa dizge açmaz, kod tablosu burada kullanılamaz.
void scan_yaml_value(std::string_view line, std::size_t at, Sink sink) {
    at = skip_spaces(line, at);
    if (at >= line.size())
        return;
    if (line[at] == '[' || line[at] == '{') {
        scan_code(kYamlFlow, line, kInitialSyntaxState, sink, at);
        return;
    }
    if (line[at] == '#') {
        sink.add(at, line.size(), TokenKind::Comment);
        return;
    }
    const std::size_t comment = line.find(" #", at);
    const std::size_t end = trim_end(line, at, comment == npos ? line.size() : comment);
    const std::string_view text = line.substr(at, end - at);
    TokenKind bare = TokenKind::Value;
    if (is_number_text(text))
        bare = TokenKind::Number;
    else if (has_word(kYamlConstants, text))
        bare = TokenKind::Constant;
    scan_scalar(line, at, sink, bare);
}

// `anahtar:` satırın neresinde biter; `http://a` ve yorumdaki iki nokta anahtar açmaz.
std::size_t yaml_key_end(std::string_view line, std::size_t at) {
    for (std::size_t colon = line.find(':', at); colon != npos; colon = line.find(':', colon + 1)) {
        if (colon + 1 == line.size() || is_space(line[colon + 1]))
            return colon;
    }
    return npos;
}

SyntaxState scan_yaml(std::string_view line, SyntaxState, Sink sink) {
    std::size_t at = skip_spaces(line, 0);
    if (at >= line.size())
        return kInitialSyntaxState;
    if (line[at] == '#') {
        sink.add(at, line.size(), TokenKind::Comment);
        return kInitialSyntaxState;
    }
    const std::string_view trimmed = line.substr(at, trim_end(line, at, line.size()) - at);
    if (trimmed == "---" || trimmed == "...") {
        sink.add(at, at + trimmed.size(), TokenKind::Keyword);
        return kInitialSyntaxState;
    }
    while (line.substr(at).starts_with("- "))
        at = skip_spaces(line, at + 2);
    const std::size_t comment = line.find(" #", at);
    const std::size_t colon = yaml_key_end(line, at);
    if (colon != npos && (comment == npos || colon < comment)) {
        sink.add(at, trim_end(line, at, colon), TokenKind::Key);
        at = colon + 1;
    }
    scan_yaml_value(line, at, sink);
    return kInitialSyntaxState;
}

// Satır başında `ifeq` ya da `#`; sekmeyle başlayan tarif; `AD := değer`; `hedef: bağımlılık`.
void scan_make_declaration(std::string_view line, std::size_t at, Sink sink) {
    const std::size_t separator = line.find_first_of(":=", at);
    if (separator == npos) {
        scan_code(kMakefile, line, kInitialSyntaxState, sink, at);
        return;
    }
    const bool assigns = line[separator] == '=' || line.substr(separator).starts_with(":=");
    if (!assigns) {
        sink.add(at, trim_end(line, at, separator), TokenKind::Tag);
        scan_code(kMakefile, line, kInitialSyntaxState, sink, separator + 1);
        return;
    }
    std::size_t key_end = separator;
    if (key_end > at && (line[key_end - 1] == '?' || line[key_end - 1] == '+'))
        --key_end;
    sink.add(at, trim_end(line, at, key_end), TokenKind::Key);
    const std::size_t eq = line.find('=', separator);
    const std::size_t value = skip_spaces(line, eq + 1);
    sink.add(value, trim_end(line, value, line.size()), TokenKind::Value);
}

SyntaxState scan_makefile(std::string_view line, SyntaxState, Sink sink) {
    if (line.starts_with('\t')) {
        scan_code(kMakefile, line, kInitialSyntaxState, sink, 1);
        return kInitialSyntaxState;
    }
    const std::size_t at = skip_spaces(line, 0);
    if (at >= line.size())
        return kInitialSyntaxState;
    std::size_t word_end = at;
    while (word_end < line.size() && is_word_char(line[word_end]))
        ++word_end;
    if (line[at] == '#' || has_word(kMakeKeywords, line.substr(at, word_end - at)))
        scan_code(kMakefile, line, kInitialSyntaxState, sink, at);
    else
        scan_make_declaration(line, at, sink);
    return kInitialSyntaxState;
}

// İşaretleme durumu: 0 metin, sonrası etiket içi; `Etiket` satır sonunu aşabilir (çok satırlı öznitelik).
enum MarkupState : SyntaxState {
    kMarkupText,
    kMarkupComment,
    kMarkupTag,
    kMarkupDouble,
    kMarkupSingle,
    kMarkupCdata
};

class MarkupScanner {
public:
    MarkupScanner(std::string_view line, Sink sink) : line_(line), sink_(sink) {}

    SyntaxState run(SyntaxState entry) {
        state_ = entry <= kMarkupCdata ? entry : static_cast<SyntaxState>(kMarkupText);
        while (pos_ < line_.size()) {
            switch (state_) {
            case kMarkupText:
                scan_text();
                break;
            case kMarkupComment:
                scan_until("-->", TokenKind::Comment);
                break;
            case kMarkupCdata:
                scan_until("]]>", TokenKind::String);
                break;
            case kMarkupDouble:
                scan_until("\"", TokenKind::String);
                break;
            case kMarkupSingle:
                scan_until("'", TokenKind::String);
                break;
            default:
                scan_tag();
                break;
            }
        }
        return state_;
    }

private:
    void scan_text();
    void scan_tag();
    void scan_until(std::string_view close, TokenKind kind);

    std::string_view line_;
    Sink sink_;
    std::size_t pos_ = 0;
    SyntaxState state_ = kMarkupText;
};

void MarkupScanner::scan_text() {
    constexpr std::string_view kCommentOpen = "<!--";
    constexpr std::string_view kCdataOpen = "<![CDATA[";
    const std::size_t open = line_.find('<', pos_);
    if (open == npos) {
        pos_ = line_.size();
        return;
    }
    const std::string_view rest = line_.substr(open);
    pos_ = open;
    if (rest.starts_with(kCommentOpen)) {
        state_ = kMarkupComment;
        return;
    }
    if (rest.starts_with(kCdataOpen)) {
        sink_.add(open, open + kCdataOpen.size(), TokenKind::Tag);
        pos_ = open + kCdataOpen.size();
        state_ = kMarkupCdata;
        return;
    }
    std::size_t end = open + 1;
    while (end < line_.size() && (line_[end] == '/' || line_[end] == '?' || line_[end] == '!'))
        ++end;
    const std::size_t name_begin = end;
    while (end < line_.size() &&
           (is_word_char(line_[end]) || line_[end] == ':' || line_[end] == '-' || line_[end] == '.'))
        ++end;
    if (end == name_begin) {
        pos_ = open + 1;  // `a < b`
        return;
    }
    sink_.add(open, end, TokenKind::Tag);
    pos_ = end;
    state_ = kMarkupTag;
}

static bool is_attribute_char(char c) {
    return is_word_char(c) || c == ':' || c == '-';
}

void MarkupScanner::scan_tag() {
    const char c = line_[pos_];
    if (c == '>') {
        sink_.add(pos_, pos_ + 1, TokenKind::Tag);
        ++pos_;
        state_ = kMarkupText;
    } else if (c == '"' || c == '\'') {
        state_ = c == '"' ? kMarkupDouble : kMarkupSingle;
        sink_.add(pos_, pos_ + 1, TokenKind::String);
        ++pos_;
    } else if ((c == '/' || c == '?') && pos_ + 1 < line_.size() && line_[pos_ + 1] == '>') {
        sink_.add(pos_, pos_ + 2, TokenKind::Tag);
        pos_ += 2;
        state_ = kMarkupText;
    } else if (is_attribute_char(c)) {
        const std::size_t begin = pos_;
        while (pos_ < line_.size() && is_attribute_char(line_[pos_]))
            ++pos_;
        sink_.add(begin, pos_, TokenKind::Key);
    } else {
        ++pos_;
    }
}

void MarkupScanner::scan_until(std::string_view close, TokenKind kind) {
    const std::size_t found = line_.find(close, pos_);
    if (found == npos) {
        sink_.add(pos_, line_.size(), kind);
        pos_ = line_.size();
        return;
    }
    const std::size_t end = found + close.size();
    sink_.add(pos_, end, kind);
    pos_ = end;
    state_ = state_ == kMarkupDouble || state_ == kMarkupSingle ? kMarkupTag : kMarkupText;
}

SyntaxState scan_markup(std::string_view line, SyntaxState entry, Sink sink) {
    return MarkupScanner(line, sink).run(entry);
}

// Durum: 0 metin, 1 ``` çiti, 2 ~~~ çiti.
SyntaxState fence_of(std::string_view trimmed) {
    if (trimmed.starts_with("```"))
        return 1;
    return trimmed.starts_with("~~~") ? 2 : 0;
}

bool is_heading(std::string_view line) {
    const std::size_t hashes = line.find_first_not_of('#');
    constexpr std::size_t kMaxHeadingLevel = 6;
    return hashes != npos && hashes >= 1 && hashes <= kMaxHeadingLevel && line[hashes] == ' ';
}

class MarkdownInline {
public:
    MarkdownInline(std::string_view line, Sink sink) : line_(line), sink_(sink) {}
    void run() {
        while (pos_ < line_.size())
            step();
    }

private:
    enum Delimiter { kBacktick, kStar, kDoubleStar, kUnderscore, kDoubleUnderscore, kDelimiterCount };
    static constexpr std::string_view kDelimiters[kDelimiterCount] = {"`", "*", "**", "_", "__"};

    void step();
    void scan_emphasis(Delimiter delimiter);
    bool span_to_closer(Delimiter delimiter, TokenKind kind, std::size_t search_from);

    std::string_view line_;
    Sink sink_;
    std::size_t pos_ = 0;
    // Kapanışı olmayan ayraç bir daha aranmaz: `*a *a *a` değil ama `**a` dizisi karesele dönerdi.
    std::array<bool, kDelimiterCount> no_closer_{};
};

void MarkdownInline::step() {
    switch (line_[pos_]) {
    case '\\':
        pos_ += 2;
        break;
    case '`':
        if (!span_to_closer(kBacktick, TokenKind::Code, pos_ + 1))
            ++pos_;
        break;
    case '*':
        scan_emphasis(line_.substr(pos_).starts_with("**") ? kDoubleStar : kStar);
        break;
    case '_':
        scan_emphasis(line_.substr(pos_).starts_with("__") ? kDoubleUnderscore : kUnderscore);
        break;
    default:
        ++pos_;
        break;
    }
}

void MarkdownInline::scan_emphasis(Delimiter delimiter) {
    const std::size_t size = kDelimiters[delimiter].size();
    const std::size_t inner = pos_ + size;
    const bool underscore = delimiter == kUnderscore || delimiter == kDoubleUnderscore;
    // `2 * 3`, `* madde` ve `snake_case` vurgu değildir.
    const bool opens = inner < line_.size() && !is_space(line_[inner]) &&
                       !(underscore && pos_ > 0 && is_word_char(line_[pos_ - 1]));
    if (!opens || !span_to_closer(delimiter, TokenKind::Emphasis, inner))
        pos_ = inner;
}

bool MarkdownInline::span_to_closer(Delimiter delimiter, TokenKind kind, std::size_t search_from) {
    if (no_closer_[delimiter])
        return false;
    const std::string_view text = kDelimiters[delimiter];
    const std::size_t close = line_.find(text, search_from);
    if (close == npos) {
        no_closer_[delimiter] = true;
        return false;
    }
    const std::size_t end = close + text.size();
    sink_.add(pos_, end, kind);
    pos_ = end;
    return true;
}

SyntaxState scan_markdown(std::string_view line, SyntaxState entry, Sink sink) {
    const std::size_t at = skip_spaces(line, 0);
    const SyntaxState fence = fence_of(line.substr(at));
    const std::size_t end = trim_end(line, 0, line.size());
    if (entry == 1 || entry == 2) {
        sink.add(0, end, TokenKind::Code);
        return fence == entry ? kInitialSyntaxState : entry;
    }
    if (fence != kInitialSyntaxState) {
        sink.add(0, end, TokenKind::Code);
        return fence;
    }
    if (is_heading(line.substr(at)))
        sink.add(0, end, TokenKind::Heading);
    else
        MarkdownInline(line, sink).run();
    return kInitialSyntaxState;
}

using SpecialScanner = SyntaxState (*)(std::string_view, SyntaxState, Sink);

// Dil başına tek satır: kod tablosu ya da kendi tarayıcısı. Sıra SyntaxLanguage ile aynıdır.
struct LanguageRule {
    SyntaxLanguage language;
    const CodeSpec* code;
    SpecialScanner special;
};

constexpr LanguageRule kLanguageRules[] = {
    {SyntaxLanguage::Plain, nullptr, nullptr},
    {SyntaxLanguage::Java, &kJava, nullptr},
    {SyntaxLanguage::Cpp, &kCpp, nullptr},
    {SyntaxLanguage::C, &kC, nullptr},
    {SyntaxLanguage::Python, &kPython, nullptr},
    {SyntaxLanguage::Shell, &kShell, nullptr},
    {SyntaxLanguage::JavaScript, &kJavaScript, nullptr},
    {SyntaxLanguage::TypeScript, &kTypeScript, nullptr},
    {SyntaxLanguage::Go, &kGo, nullptr},
    {SyntaxLanguage::Rust, &kRust, nullptr},
    {SyntaxLanguage::Sql, &kSql, nullptr},
    {SyntaxLanguage::Json, &kJson, nullptr},
    {SyntaxLanguage::Css, &kCss, nullptr},
    {SyntaxLanguage::Html, nullptr, scan_markup},
    {SyntaxLanguage::Xml, nullptr, scan_markup},
    {SyntaxLanguage::Yaml, nullptr, scan_yaml},
    {SyntaxLanguage::Toml, nullptr, scan_toml},
    {SyntaxLanguage::Ini, nullptr, scan_ini},
    {SyntaxLanguage::Env, nullptr, scan_env},
    {SyntaxLanguage::Properties, nullptr, scan_properties},
    {SyntaxLanguage::Dockerfile, &kDockerfile, nullptr},
    {SyntaxLanguage::Makefile, nullptr, scan_makefile},
    {SyntaxLanguage::Markdown, nullptr, scan_markdown},
};

struct ExtensionRule {
    std::string_view extension;
    SyntaxLanguage language{};
};

constexpr ExtensionRule kExtensions[] = {
    {"java", SyntaxLanguage::Java},
    {"cpp", SyntaxLanguage::Cpp},
    {"cc", SyntaxLanguage::Cpp},
    {"cxx", SyntaxLanguage::Cpp},
    {"hpp", SyntaxLanguage::Cpp},
    {"hh", SyntaxLanguage::Cpp},
    {"hxx", SyntaxLanguage::Cpp},
    {"c", SyntaxLanguage::C},
    {"h", SyntaxLanguage::C},
    {"py", SyntaxLanguage::Python},
    {"sh", SyntaxLanguage::Shell},
    {"bash", SyntaxLanguage::Shell},
    {"zsh", SyntaxLanguage::Shell},
    {"js", SyntaxLanguage::JavaScript},
    {"mjs", SyntaxLanguage::JavaScript},
    {"cjs", SyntaxLanguage::JavaScript},
    {"jsx", SyntaxLanguage::JavaScript},
    {"ts", SyntaxLanguage::TypeScript},
    {"tsx", SyntaxLanguage::TypeScript},
    {"go", SyntaxLanguage::Go},
    {"rs", SyntaxLanguage::Rust},
    {"sql", SyntaxLanguage::Sql},
    {"json", SyntaxLanguage::Json},
    {"css", SyntaxLanguage::Css},
    {"html", SyntaxLanguage::Html},
    {"htm", SyntaxLanguage::Html},
    {"xml", SyntaxLanguage::Xml},
    {"svg", SyntaxLanguage::Xml},
    {"yaml", SyntaxLanguage::Yaml},
    {"yml", SyntaxLanguage::Yaml},
    {"toml", SyntaxLanguage::Toml},
    {"ini", SyntaxLanguage::Ini},
    {"cfg", SyntaxLanguage::Ini},
    {"conf", SyntaxLanguage::Ini},
    {"env", SyntaxLanguage::Env},
    {"properties", SyntaxLanguage::Properties},
    {"dockerfile", SyntaxLanguage::Dockerfile},
    {"makefile", SyntaxLanguage::Makefile},
    {"mk", SyntaxLanguage::Makefile},
    {"md", SyntaxLanguage::Markdown},
    {"markdown", SyntaxLanguage::Markdown},
};

}  // namespace

SyntaxLanguage syntax_language_for(std::string_view extension) {
    std::string lowered(extension);
    std::ranges::transform(lowered, lowered.begin(), folded);
    const auto* rule = std::ranges::find(kExtensions, std::string_view(lowered), &ExtensionRule::extension);
    return rule == std::ranges::end(kExtensions) ? SyntaxLanguage::Plain : rule->language;
}

SyntaxState highlight_line(SyntaxLanguage language, std::string_view line, SyntaxState entry,
                           std::vector<SyntaxSpan>* spans) {
    if (line.size() > kMaxHighlightLineBytes)
        return entry;
    const LanguageRule& rule = kLanguageRules[static_cast<std::size_t>(language)];
    const Sink sink(spans);
    if (rule.code)
        return scan_code(*rule.code, line, entry, sink);
    return rule.special ? rule.special(line, entry, sink) : kInitialSyntaxState;
}

}  // namespace noteapp::tui
