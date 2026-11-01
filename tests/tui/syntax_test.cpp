// Tablo güdümlü sözdizimi vurgusu (T58): dil başına aralıklar, satırlar arası durum, kötü niyetli girdi.
#include "tui/syntax.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

using noteapp::tui::highlight_line;
using noteapp::tui::kInitialSyntaxState;
using noteapp::tui::kMaxHighlightLineBytes;
using noteapp::tui::SyntaxLanguage;
using noteapp::tui::SyntaxSpan;
using noteapp::tui::SyntaxState;
using noteapp::tui::SyntaxStateCache;
using noteapp::tui::TokenKind;

namespace {

struct Lined {
    std::vector<SyntaxSpan> spans;
    SyntaxState exit = kInitialSyntaxState;
};

Lined scan(SyntaxLanguage language, std::string_view line, SyntaxState entry = kInitialSyntaxState) {
    Lined out;
    out.exit = highlight_line(language, line, entry, &out.spans);
    return out;
}

// `needle`ın ilk baytının türü: testler bayt ofseti yerine metinle okunur.
TokenKind kind_of(const std::vector<SyntaxSpan>& spans, std::string_view line, std::string_view needle) {
    const std::size_t at = line.find(needle);
    REQUIRE_MESSAGE(at != std::string_view::npos, needle);
    const auto it =
        std::ranges::find_if(spans, [at](const SyntaxSpan& s) { return at >= s.begin && at < s.end; });
    return it == spans.end() ? TokenKind::Plain : it->kind;
}

TokenKind kind(SyntaxLanguage language, std::string_view line, std::string_view needle) {
    return kind_of(scan(language, line).spans, line, needle);
}

void check_well_formed(const std::vector<SyntaxSpan>& spans, std::size_t size) {
    std::size_t previous_end = 0;
    for (const SyntaxSpan& span : spans) {
        CHECK(span.begin >= previous_end);
        CHECK(span.begin < span.end);
        CHECK(span.end <= size);
        CHECK(span.kind != TokenKind::Plain);
        previous_end = span.end;
    }
}

constexpr SyntaxLanguage kAllLanguages[] = {
    SyntaxLanguage::Plain,      SyntaxLanguage::Java,       SyntaxLanguage::Cpp,
    SyntaxLanguage::C,          SyntaxLanguage::Python,     SyntaxLanguage::Shell,
    SyntaxLanguage::JavaScript, SyntaxLanguage::TypeScript, SyntaxLanguage::Go,
    SyntaxLanguage::Rust,       SyntaxLanguage::Sql,        SyntaxLanguage::Json,
    SyntaxLanguage::Css,        SyntaxLanguage::Html,       SyntaxLanguage::Xml,
    SyntaxLanguage::Yaml,       SyntaxLanguage::Toml,       SyntaxLanguage::Ini,
    SyntaxLanguage::Env,        SyntaxLanguage::Properties, SyntaxLanguage::Dockerfile,
    SyntaxLanguage::Makefile,   SyntaxLanguage::Markdown,
};

}  // namespace

TEST_CASE("syntax: the language comes from the extension, case-insensitively, unknown is plain") {
    using noteapp::tui::syntax_language_for;
    const std::pair<const char*, SyntaxLanguage> table[] = {
        {"java", SyntaxLanguage::Java},
        {"cpp", SyntaxLanguage::Cpp},
        {"hpp", SyntaxLanguage::Cpp},
        {"cc", SyntaxLanguage::Cpp},
        {"c", SyntaxLanguage::C},
        {"h", SyntaxLanguage::C},
        {"py", SyntaxLanguage::Python},
        {"sh", SyntaxLanguage::Shell},
        {"bash", SyntaxLanguage::Shell},
        {"js", SyntaxLanguage::JavaScript},
        {"mjs", SyntaxLanguage::JavaScript},
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
        {"conf", SyntaxLanguage::Ini},
        {"env", SyntaxLanguage::Env},
        {"properties", SyntaxLanguage::Properties},
        {"dockerfile", SyntaxLanguage::Dockerfile},
        {"makefile", SyntaxLanguage::Makefile},
        {"mk", SyntaxLanguage::Makefile},
        {"md", SyntaxLanguage::Markdown},
        {"markdown", SyntaxLanguage::Markdown},
        {"JAVA", SyntaxLanguage::Java},
        {"txt", SyntaxLanguage::Plain},
        {"csv", SyntaxLanguage::Plain},
        {"xyz", SyntaxLanguage::Plain},
        {"", SyntaxLanguage::Plain},
    };
    for (const auto& [extension, language] : table)
        CHECK_MESSAGE(syntax_language_for(extension) == language, extension);
}

TEST_CASE("syntax: java keywords, types, strings, numbers, comments") {
    const std::string_view line = "public final Foo x = \"s\" + 42; // not";
    const auto language = SyntaxLanguage::Java;
    CHECK(kind(language, line, "public") == TokenKind::Keyword);
    CHECK(kind(language, line, "Foo") == TokenKind::Type);
    CHECK(kind(language, line, " x ") == TokenKind::Plain);
    CHECK(kind(language, line, "\"s\"") == TokenKind::String);
    CHECK(kind(language, line, "42") == TokenKind::Number);
    CHECK(kind(language, line, "// not") == TokenKind::Comment);
    CHECK(kind(language, "@Override", "@Override") == TokenKind::Type);
    CHECK(kind(language, "return true;", "true") == TokenKind::Constant);
    CHECK(kind(language, "a1 = b2;", "a1") == TokenKind::Plain);
}

TEST_CASE("syntax: cpp and c directives, keywords and types") {
    CHECK(kind(SyntaxLanguage::Cpp, "#include <vector>", "#include") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Cpp, "template <class T> T f();", "template") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Cpp, "int x = nullptr;", "int") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::Cpp, "int x = nullptr;", "nullptr") == TokenKind::Constant);
    CHECK(kind(SyntaxLanguage::Cpp, "char c = 'x';", "'x'") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::C, "#define N 10", "#define") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::C, "struct node *p = NULL;", "struct") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::C, "struct node *p = NULL;", "NULL") == TokenKind::Constant);
    CHECK(kind(SyntaxLanguage::C, "/* c */ int", "/* c */") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::C, "/* c */ int", "int") == TokenKind::Type);
}

TEST_CASE("syntax: python") {
    const auto language = SyntaxLanguage::Python;
    CHECK(kind(language, "def f(x):  # yorum", "def") == TokenKind::Keyword);
    CHECK(kind(language, "def f(x):  # yorum", "# yorum") == TokenKind::Comment);
    CHECK(kind(language, "x = 'a' + \"b\"", "'a'") == TokenKind::String);
    CHECK(kind(language, "x = 'a' + \"b\"", "\"b\"") == TokenKind::String);
    CHECK(kind(language, "return None", "None") == TokenKind::Constant);
    CHECK(kind(language, "@staticmethod", "@staticmethod") == TokenKind::Type);
    CHECK(kind(language, "s = str(1.5)", "str") == TokenKind::Type);
    CHECK(kind(language, "s = str(1.5)", "1.5") == TokenKind::Number);
}

TEST_CASE("syntax: shell comments need a word boundary, variables are constants") {
    const auto language = SyntaxLanguage::Shell;
    CHECK(kind(language, "if [ -f a ]; then # not", "if") == TokenKind::Keyword);
    CHECK(kind(language, "if [ -f a ]; then # not", "# not") == TokenKind::Comment);
    CHECK(kind(language, "echo a#b", "#b") == TokenKind::Plain);
    CHECK(kind(language, "cp $src ${dst}", "$src") == TokenKind::Constant);
    CHECK(kind(language, "cp $src ${dst}", "${dst}") == TokenKind::Constant);
    CHECK(kind(language, "x=\"a b\"", "\"a b\"") == TokenKind::String);
}

TEST_CASE("syntax: javascript and typescript") {
    CHECK(kind(SyntaxLanguage::JavaScript, "const a = `t`; // c", "const") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::JavaScript, "const a = `t`; // c", "`t`") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::JavaScript, "const a = `t`; // c", "// c") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::TypeScript, "interface A { id: number; }", "interface") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::TypeScript, "interface A { id: number; }", "number") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::TypeScript, "let u = undefined;", "undefined") == TokenKind::Constant);
}

TEST_CASE("syntax: go and rust") {
    CHECK(kind(SyntaxLanguage::Go, "func f(a int) string {", "func") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Go, "func f(a int) string {", "int") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::Go, "x := `raw`", "`raw`") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::Go, "return nil", "nil") == TokenKind::Constant);
    CHECK(kind(SyntaxLanguage::Rust, "pub fn f(a: u32) -> String {", "pub") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Rust, "pub fn f(a: u32) -> String {", "u32") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::Rust, "pub fn f(a: u32) -> String {", "String") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::Rust, "let c = 'x'; let v = 1;", "'x'") == TokenKind::String);
    // Ömür belirteci `'a` dizge açmaz; ardındaki kod rengini korur.
    CHECK(kind(SyntaxLanguage::Rust, "fn f<'a>(s: &'a str) -> u8 { 1 }", "str") == TokenKind::Type);
    CHECK(kind(SyntaxLanguage::Rust, "fn f<'a>(s: &'a str) -> u8 { 1 }", "u8") == TokenKind::Type);
}

TEST_CASE("syntax: sql keywords ignore case") {
    const auto language = SyntaxLanguage::Sql;
    CHECK(kind(language, "SELECT a FROM b -- c", "SELECT") == TokenKind::Keyword);
    CHECK(kind(language, "select a from b -- c", "select") == TokenKind::Keyword);
    CHECK(kind(language, "select a from b -- c", "-- c") == TokenKind::Comment);
    CHECK(kind(language, "WHERE x = 'a''b'", "'a") == TokenKind::String);
    CHECK(kind(language, "id INTEGER PRIMARY KEY", "INTEGER") == TokenKind::Type);
    CHECK(kind(language, "a = NULL", "NULL") == TokenKind::Constant);
}

TEST_CASE("syntax: json keys differ from string values") {
    const std::string_view line = "\"ad\": \"x\", \"n\": 1.5, \"b\": true, \"c\": null";
    const auto language = SyntaxLanguage::Json;
    CHECK(kind(language, line, "\"ad\"") == TokenKind::Key);
    CHECK(kind(language, line, "\"x\"") == TokenKind::String);
    CHECK(kind(language, line, "1.5") == TokenKind::Number);
    CHECK(kind(language, line, "true") == TokenKind::Constant);
    CHECK(kind(language, line, "null") == TokenKind::Constant);
}

TEST_CASE("syntax: css property names, at-rules, comments, strings") {
    const auto language = SyntaxLanguage::Css;
    CHECK(kind(language, "  color: red; /* c */", "color") == TokenKind::Key);
    CHECK(kind(language, "  color: red; /* c */", "/* c */") == TokenKind::Comment);
    CHECK(kind(language, "@media (x) {", "@media") == TokenKind::Keyword);
    CHECK(kind(language, "a:hover {", "a:hover") == TokenKind::Plain);
    CHECK(kind(language, "--fg: 12px;", "--fg") == TokenKind::Key);
    CHECK(kind(language, "--fg: 12px;", "12px") == TokenKind::Number);
    CHECK(kind(language, "url(\"a\")", "\"a\"") == TokenKind::String);
}

TEST_CASE("syntax: html and xml tags, attributes, comments") {
    for (const auto language : {SyntaxLanguage::Html, SyntaxLanguage::Xml}) {
        const std::string_view line = "<a href=\"x\">metin</a> <!-- c -->";
        CHECK(kind(language, line, "<a") == TokenKind::Tag);
        CHECK(kind(language, line, "href") == TokenKind::Key);
        CHECK(kind(language, line, "\"x\"") == TokenKind::String);
        CHECK(kind(language, line, "metin") == TokenKind::Plain);
        CHECK(kind(language, line, "</a>") == TokenKind::Tag);
        CHECK(kind(language, line, "<!-- c -->") == TokenKind::Comment);
    }
    CHECK(kind(SyntaxLanguage::Xml, "<?xml version=\"1.0\"?>", "<?xml") == TokenKind::Tag);
    CHECK(kind(SyntaxLanguage::Xml, "<![CDATA[ x ]]>", "x") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::Html, "a < b", "<") == TokenKind::Plain);
}

TEST_CASE("syntax: yaml keys, values, comments, list items") {
    const auto language = SyntaxLanguage::Yaml;
    CHECK(kind(language, "name: abc", "name") == TokenKind::Key);
    CHECK(kind(language, "name: abc", "abc") == TokenKind::Value);
    CHECK(kind(language, "  - item: 'q'  # c", "item") == TokenKind::Key);
    CHECK(kind(language, "  - item: 'q'  # c", "'q'") == TokenKind::String);
    CHECK(kind(language, "  - item: 'q'  # c", "# c") == TokenKind::Comment);
    CHECK(kind(language, "port: 80", "80") == TokenKind::Number);
    CHECK(kind(language, "on: true", "true") == TokenKind::Constant);
    CHECK(kind(language, "text: don't stop", "don't") == TokenKind::Value);
    CHECK(kind(language, "---", "---") == TokenKind::Keyword);
    CHECK(kind(language, "# yorum: x", "yorum") == TokenKind::Comment);
    CHECK(kind(language, "url: http://a:1/b", "http") == TokenKind::Value);
}

TEST_CASE("syntax: toml tables, keys, literals") {
    const auto language = SyntaxLanguage::Toml;
    CHECK(kind(language, "[tool.poetry]", "tool") == TokenKind::Tag);
    CHECK(kind(language, "[[bin]]", "bin") == TokenKind::Tag);
    CHECK(kind(language, "name = \"a\" # c", "name") == TokenKind::Key);
    CHECK(kind(language, "name = \"a\" # c", "\"a\"") == TokenKind::String);
    CHECK(kind(language, "name = \"a\" # c", "# c") == TokenKind::Comment);
    CHECK(kind(language, "n = 3", "3") == TokenKind::Number);
    CHECK(kind(language, "b = false", "false") == TokenKind::Constant);
    CHECK(kind(language, "# x = 1", "x") == TokenKind::Comment);
}

TEST_CASE("syntax: ini, env, properties") {
    CHECK(kind(SyntaxLanguage::Ini, "[core]", "core") == TokenKind::Tag);
    CHECK(kind(SyntaxLanguage::Ini, "; c", "c") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Ini, "# c", "c") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Ini, "key = some value", "key") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Ini, "key = some value", "some") == TokenKind::Value);
    CHECK(kind(SyntaxLanguage::Env, "DB_HOST=localhost", "DB_HOST") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Env, "DB_HOST=localhost", "localhost") == TokenKind::Value);
    CHECK(kind(SyntaxLanguage::Env, "export A=1", "export") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Env, "export A=1", "A") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Env, "A=\"b c\" # d", "\"b c\"") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::Env, "A=\"b c\" # d", "# d") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Env, "# A=1", "A") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Properties, "a.b.c=value", "a.b.c") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Properties, "a.b.c=value", "value") == TokenKind::Value);
    CHECK(kind(SyntaxLanguage::Properties, "key: value", "key") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Properties, "key: value", "value") == TokenKind::Value);
    CHECK(kind(SyntaxLanguage::Properties, "! c", "c") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Properties, "# c", "c") == TokenKind::Comment);
}

TEST_CASE("syntax: dockerfile and makefile") {
    CHECK(kind(SyntaxLanguage::Dockerfile, "FROM alpine AS b", "FROM") == TokenKind::Keyword);
    CHECK(kind(SyntaxLanguage::Dockerfile, "RUN echo \"a\"", "\"a\"") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::Dockerfile, "# not", "not") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Dockerfile, "ENV A=$B", "$B") == TokenKind::Constant);
    CHECK(kind(SyntaxLanguage::Makefile, "CC := gcc", "CC") == TokenKind::Key);
    CHECK(kind(SyntaxLanguage::Makefile, "CC := gcc", "gcc") == TokenKind::Value);
    CHECK(kind(SyntaxLanguage::Makefile, "all: a.o", "all") == TokenKind::Tag);
    CHECK(kind(SyntaxLanguage::Makefile, "\tcc $(CFLAGS) -c $<", "$(CFLAGS)") == TokenKind::Constant);
    CHECK(kind(SyntaxLanguage::Makefile, "# not", "not") == TokenKind::Comment);
    CHECK(kind(SyntaxLanguage::Makefile, "ifeq ($(A),1)", "ifeq") == TokenKind::Keyword);
}

TEST_CASE("syntax: markdown headings, code, emphasis") {
    const auto language = SyntaxLanguage::Markdown;
    CHECK(kind(language, "## Başlık", "Başlık") == TokenKind::Heading);
    CHECK(kind(language, "#etiket", "etiket") == TokenKind::Plain);
    CHECK(kind(language, "a `kod` b", "`kod`") == TokenKind::Code);
    CHECK(kind(language, "a **kalın** b", "kalın") == TokenKind::Emphasis);
    CHECK(kind(language, "a *ince* b", "ince") == TokenKind::Emphasis);
    CHECK(kind(language, "a _ince_ b", "ince") == TokenKind::Emphasis);
    CHECK(kind(language, "snake_case_name", "case") == TokenKind::Plain);
    CHECK(kind(language, "* madde", "madde") == TokenKind::Plain);
    CHECK(kind(language, "2 * 3 * 4", "3") == TokenKind::Plain);
}

TEST_CASE("syntax: plain language has no spans; spans are ordered, in range and never Plain") {
    CHECK(scan(SyntaxLanguage::Plain, "int x = 1; // c").spans.empty());
    const char* samples[] = {
        "public class A { /* c */ int x = 1; String s = \"t\"; }",
        "def f(): return 'x' # c",
        "<a b=\"c\" d='e'>f</a><!-- g -->",
        "key: \"value\" # c",
        "[section]",
        "a = [1, 'x', true] # c",
        "# h **b** `c`",
        "**b** _c_ `d` *e*",
        "CC := a $(B) # c",
        "\tcc -o $@ $<",
    };
    for (const auto language : kAllLanguages)
        for (const std::string_view sample : samples)
            check_well_formed(scan(language, sample).spans, sample.size());
}

TEST_CASE("syntax state: block comments and multi-line strings carry across lines") {
    struct Case {
        SyntaxLanguage language;
        std::string_view open;
        std::string_view middle;
        std::string_view close;
        std::string_view after;
        TokenKind inside;
    };
    const Case cases[] = {
        {SyntaxLanguage::Java, "int a; /* b", "c d", "e */ int f;", "int", TokenKind::Comment},
        {SyntaxLanguage::Cpp, "/* b", "c", "e */ int", "int", TokenKind::Comment},
        {SyntaxLanguage::C, "/* b", "c", "e */ int", "int", TokenKind::Comment},
        {SyntaxLanguage::Python, "x = \"\"\"b", "c", "e\"\"\" + def", "def", TokenKind::String},
        {SyntaxLanguage::Python, "x = '''b", "c", "e''' + def", "def", TokenKind::String},
        {SyntaxLanguage::JavaScript, "a = `b", "c", "e` + const", "const", TokenKind::String},
        {SyntaxLanguage::TypeScript, "/* b", "c", "e */ const", "const", TokenKind::Comment},
        {SyntaxLanguage::Go, "a := `b", "c", "e` + func", "func", TokenKind::String},
        {SyntaxLanguage::Rust, "/* b", "c", "e */ fn", "fn", TokenKind::Comment},
        {SyntaxLanguage::Rust, "let a = \"b", "c", "e\"; fn", "fn", TokenKind::String},
        {SyntaxLanguage::Sql, "/* b", "c", "e */ SELECT", "SELECT", TokenKind::Comment},
        {SyntaxLanguage::Sql, "x = 'b", "c", "e' SELECT", "SELECT", TokenKind::String},
        {SyntaxLanguage::Css, "/* b", "c", "e */ @media", "@media", TokenKind::Comment},
        {SyntaxLanguage::Html, "<!-- b", "c", "e --><a>", "<a", TokenKind::Comment},
        {SyntaxLanguage::Xml, "<!-- b", "c", "e --><a>", "<a", TokenKind::Comment},
        {SyntaxLanguage::Xml, "<a b=\"c", "d", "e\" f>", "f", TokenKind::String},
        {SyntaxLanguage::Toml, "a = \"\"\"b", "c", "e\"\"\" # d", "# d", TokenKind::String},
        {SyntaxLanguage::Shell, "a=\"b", "c", "e\" # d", "# d", TokenKind::String},
        {SyntaxLanguage::Markdown, "```cpp", "int a;", "```", "```", TokenKind::Code},
    };
    for (const Case& c : cases) {
        const Lined first = scan(c.language, c.open);
        CHECK_MESSAGE(first.exit != kInitialSyntaxState, c.open);
        const Lined middle = scan(c.language, c.middle, first.exit);
        CHECK_MESSAGE(middle.exit == first.exit, c.middle);
        CHECK_MESSAGE(kind_of(middle.spans, c.middle, c.middle.substr(0, 1)) == c.inside, c.middle);
        const Lined last = scan(c.language, c.close, middle.exit);
        CHECK_MESSAGE(last.exit == kInitialSyntaxState, c.close);
        CHECK_MESSAGE(kind_of(last.spans, c.close, c.close.substr(0, 1)) == c.inside, c.close);
        if (c.language != SyntaxLanguage::Markdown) {
            const TokenKind after = kind_of(last.spans, c.close, c.after);
            CHECK_MESSAGE(after != c.inside, c.after);
        }
    }
}

TEST_CASE("syntax state: a single-line string does not leak and a closed comment leaves normal state") {
    CHECK(scan(SyntaxLanguage::Java, "String s = \"unterminated").exit == kInitialSyntaxState);
    CHECK(scan(SyntaxLanguage::Python, "x = 'unterminated").exit == kInitialSyntaxState);
    CHECK(scan(SyntaxLanguage::Json, "\"unterminated").exit == kInitialSyntaxState);
    CHECK(scan(SyntaxLanguage::Yaml, "a: \"unterminated").exit == kInitialSyntaxState);
    CHECK(scan(SyntaxLanguage::Java, "/* a */ int").exit == kInitialSyntaxState);
    // Kaçışlı kapanış tırnağı dizgeyi bitirmez.
    const std::string_view escaped = "\"a\\\"b\" + 1";
    CHECK(kind(SyntaxLanguage::Java, escaped, "b") == TokenKind::String);
    CHECK(kind(SyntaxLanguage::Java, escaped, "1") == TokenKind::Number);
}

TEST_CASE(
    "syntax state cache: computes only up to the asked line and an edit invalidates from that line on") {
    const std::vector<std::string> original = {"int a;", "/* b", "c", "*/", "int d;"};
    std::vector<std::string> lines = original;
    int calls = 0;
    const auto line_at = [&](std::size_t i) {
        ++calls;
        return std::string_view(lines[i]);
    };
    SyntaxStateCache cache;
    const auto language = SyntaxLanguage::Java;
    CHECK(cache.entry_state(language, 0, line_at) == kInitialSyntaxState);
    CHECK(calls == 0);
    const SyntaxState in_comment = cache.entry_state(language, 2, line_at);
    CHECK(in_comment != kInitialSyntaxState);
    CHECK(calls == 2);
    CHECK(cache.known_lines() == 2);
    CHECK(cache.entry_state(language, 2, line_at) == in_comment);  // önbellekte
    CHECK(calls == 2);

    CHECK(cache.entry_state(language, 4, line_at) == kInitialSyntaxState);
    CHECK(calls == 4);

    // 1. satırdaki açılış silinir: 2 ve sonrası yeniden hesaplanır, 0. satır dokunulmaz.
    lines[1] = "b";
    cache.invalidate_from(1);
    CHECK(cache.known_lines() == 1);
    calls = 0;
    CHECK(cache.entry_state(language, 3, line_at) == kInitialSyntaxState);
    CHECK(calls == 2);
}

TEST_CASE("syntax hostile: unterminated constructs, invalid UTF-8 and control bytes never crash") {
    const std::string samples[] = {
        "\"",
        "'",
        "`",
        "/*",
        "*/",
        "<",
        "<!--",
        "<a",
        "<a b=\"",
        "<![CDATA[",
        "[",
        "[[",
        "\"\"\"",
        "'''",
        "```",
        "*",
        "**",
        "_",
        "__",
        "${",
        "$(",
        ":",
        "=",
        "- ",
        "#",
        "!",
        ";",
        "-",
        "\\",
        "\"\\",
        "\xFF\xFE\xFD",
        "\xC3",
        "\xE2\x82",
        "a\xC3",
        "\"\xC3",
        std::string(1, '\0'),
        "\x1B[31m",
    };
    for (const auto language : kAllLanguages) {
        for (const std::string& sample : samples) {
            // Her durumdan giriş: bozuk önbellek ya da başka dilin durumu da çökertmemeli.
            for (SyntaxState entry = 0; entry < 8; ++entry) {
                const Lined result = scan(language, sample, entry);
                check_well_formed(result.spans, sample.size());
            }
        }
    }
}

TEST_CASE("syntax hostile: a line over the limit is plain and passes the state through") {
    const std::string huge(kMaxHighlightLineBytes + 1, '"');
    for (const auto language : kAllLanguages) {
        const Lined result = scan(language, huge, 1);
        CHECK(result.spans.empty());
        CHECK(result.exit == 1);
    }
}

TEST_CASE("syntax hostile: pathological lines at the line limit stay well formed") {
    // Süre ölçümü Release perf paketinde (`editor latency: pathological`): burada paralel koşuda oynardı.
    const std::string_view units[] = {"\"", "'",  "`",  "*a ", "_a ", "**a ", "`a", "<",  "<a ",  "<!--",
                                      "a=", "/*", "*/", "\\",  "${",  "[",    ": ", "- ", "\xFF", "'a"};
    for (const auto language : kAllLanguages) {
        for (const std::string_view unit : units) {
            std::string line;
            while (line.size() < kMaxHighlightLineBytes)
                line += unit;
            line.resize(kMaxHighlightLineBytes);
            check_well_formed(scan(language, line).spans, line.size());
        }
    }
}
