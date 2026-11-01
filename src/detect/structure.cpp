#include "detect/structure.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace noteapp::detect {

namespace {

// Yaml dışı anahtar/değer tiplerinde her satır anahtar, bölüm ya da yorum olmalıdır; yaml'da blok metinleri
// nedeniyle satırların bu oranı yeter.
constexpr std::size_t kYamlLinePercent = 80;
// Yapısal kanıtı olmayan düz "a: b" satırları (Türkçe not dahil) en az bu kadar olmalı.
constexpr std::size_t kYamlMinPlainKeys = 3;
// Değeri bu kadar kelimeyi aşan "anahtar: değer" satırı düz yazıdır.
constexpr std::size_t kProseValueWords = 5;
constexpr std::size_t kDockerfileMinInstructions = 2;

constexpr std::string_view kDockerInstructions[] = {
    "ADD",   "ARG",        "CMD",     "COPY", "ENTRYPOINT", "ENV",        "EXPOSE", "FROM",   "HEALTHCHECK",
    "LABEL", "MAINTAINER", "ONBUILD", "RUN",  "SHELL",      "STOPSIGNAL", "USER",   "VOLUME", "WORKDIR"};

constexpr std::string_view kHtmlTags[] = {
    "a",       "article", "aside",  "body",  "br",    "button", "div",  "em",     "footer", "form",
    "h1",      "h2",      "h3",     "h4",    "h5",    "h6",     "head", "header", "hr",     "html",
    "img",     "input",   "li",     "link",  "main",  "meta",   "nav",  "ol",     "p",      "script",
    "section", "span",    "strong", "style", "table", "td",     "tr",   "ul"};

std::string_view trimmed(std::string_view s) {
    constexpr std::string_view kWs = " \t\r\n";
    const auto b = s.find_first_not_of(kWs);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(kWs) - b + 1);
}

bool is_key_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '.' || c == '-';
}

bool is_key(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, is_key_char);
}

bool is_upper_identifier(std::string_view key) {
    return !key.empty() && std::isdigit(static_cast<unsigned char>(key.front())) == 0 &&
           std::ranges::all_of(key, [](char c) {
               const auto u = static_cast<unsigned char>(c);
               return std::isupper(u) != 0 || std::isdigit(u) != 0 || c == '_';
           });
}

bool is_number(std::string_view v) {
    if (v.empty())
        return false;
    bool digit = false;
    for (const char c : v) {
        const bool numeric = std::isdigit(static_cast<unsigned char>(c)) != 0;
        digit = digit || numeric;
        if (!numeric && c != '.' && c != '-' && c != '+' && c != '_')
            return false;
    }
    return digit;
}

// TOML'da değer bu biçimlerden biri olmak zorundadır; ini ve properties'te çıplak metin de olur.
bool is_toml_literal(std::string_view v) {
    if (v.empty())
        return false;
    const char first = v.front();
    return first == '"' || first == '\'' || first == '[' || first == '{' || v == "true" || v == "false" ||
           is_number(v);
}

// Satır sonu yorumu değere dahil değildir: `a = 1  # açıklama`.
std::string_view without_inline_comment(std::string_view v) {
    const auto hash = v.find(" #");
    return hash == std::string_view::npos ? v : trimmed(v.substr(0, hash));
}

std::size_t word_count(std::string_view v) {
    std::size_t words = 0;
    bool in_word = false;
    for (const char c : v) {
        const bool space = c == ' ' || c == '\t';
        words += !space && !in_word ? 1 : 0;
        in_word = !space;
    }
    return words;
}

bool has_shell_substitution(std::string_view v) {
    return v.find("$(") != std::string_view::npos || v.find('`') != std::string_view::npos;
}

// Her tipin kanıtı; satır başına tek sınıf sayılır.
struct KeyValueTally {
    std::size_t content_lines = 0;  // yorum ve boş dışı
    std::size_t sections = 0;
    std::size_t equals_keys = 0;
    std::size_t env_keys = 0;      // BÜYÜK_HARF ya da export
    std::size_t dotted_keys = 0;   // a.b.c
    std::size_t bare_values = 0;   // TOML olamayan değer
    std::size_t shell_values = 0;  // $(...) ya da küçük harfli anahtarda $x: kabuk betiği
    std::size_t colon_keys = 0;
    std::size_t dotted_colon_keys = 0;
    std::size_t lowercase_colon_keys = 0;
    std::size_t prose_values = 0;
    std::size_t list_items = 0;
    std::size_t other = 0;
    std::size_t markdown_headings = 0;  // `## x`: yaml yorumu `# x` olur, iki diyez belge başlığıdır
    bool has_yaml_evidence = false;     // ---, iç içe anahtar, liste, sayı/boole değer, a_b / a-b anahtar
    bool semicolon_comment = false;
    bool tab_indented = false;
};

class KeyValueScanner {
public:
    KeyValueTally scan(const std::vector<std::string_view>& lines);

private:
    void scan_line(std::string_view raw);
    bool skip_toml_block(std::string_view text);
    bool skip_yaml_block(std::string_view raw);
    bool scan_comment(std::string_view text);
    bool scan_section(std::string_view text);
    bool scan_list_item(std::string_view text, std::size_t indent);
    bool scan_equals(std::string_view text);
    bool scan_colon(std::string_view text, std::size_t indent);
    void note_equals_value(std::string_view value, bool env_style);
    void note_colon_value(std::string_view value, std::size_t indent);

    KeyValueTally tally_;
    std::string_view toml_closer_;  // boş değilse çok satırlı dizge ya da dizi içindeyiz
    bool in_yaml_block_ = false;    // `key: |` ardındaki blok metin
    std::size_t yaml_block_indent_ = 0;
};

KeyValueTally KeyValueScanner::scan(const std::vector<std::string_view>& lines) {
    for (const std::string_view line : lines)
        scan_line(line);
    return tally_;
}

void KeyValueScanner::scan_line(std::string_view raw) {
    const std::string_view text = trimmed(raw);
    if (skip_toml_block(text) || skip_yaml_block(raw) || scan_comment(text))
        return;
    if (raw.front() == '\t')
        tally_.tab_indented = true;
    ++tally_.content_lines;
    const std::size_t indent = raw.find_first_not_of(" \t");
    if (scan_section(text) || scan_list_item(text, indent) || scan_equals(text) || scan_colon(text, indent))
        return;
    ++tally_.other;
}

bool KeyValueScanner::skip_toml_block(std::string_view text) {
    if (toml_closer_.empty())
        return false;
    if (text.find(toml_closer_) != std::string_view::npos)
        toml_closer_ = {};
    return true;
}

bool KeyValueScanner::skip_yaml_block(std::string_view raw) {
    if (!in_yaml_block_)
        return false;
    if (raw.find_first_not_of(" \t") > yaml_block_indent_)
        return true;
    in_yaml_block_ = false;
    return false;
}

bool KeyValueScanner::scan_comment(std::string_view text) {
    if (text == "---") {
        tally_.has_yaml_evidence = true;
        return true;
    }
    if (text.front() == ';')
        tally_.semicolon_comment = true;
    if (text.starts_with("## "))
        ++tally_.markdown_headings;
    return text.front() == '#' || text.front() == ';' || text.front() == '!';
}

bool KeyValueScanner::scan_section(std::string_view text) {
    if (text.front() != '[' || text.back() != ']' || text.size() < 3)
        return false;
    ++tally_.sections;
    return true;
}

bool KeyValueScanner::scan_list_item(std::string_view text, std::size_t indent) {
    if (!text.starts_with("- ") && text != "-")
        return false;
    ++tally_.list_items;
    tally_.has_yaml_evidence = true;
    // `- name: a` aynı zamanda anahtardır; blok metin başlatabilir.
    if (const std::string_view item = trimmed(text.substr(1)); !item.empty())
        scan_colon(item, indent + 1);
    return true;
}

bool KeyValueScanner::scan_equals(std::string_view text) {
    constexpr std::string_view kExport = "export ";
    const bool exported = text.starts_with(kExport);
    if (exported)
        text.remove_prefix(kExport.size());
    const auto eq = text.find('=');
    if (eq == std::string_view::npos)
        return false;
    const std::string_view key = trimmed(text.substr(0, eq));
    if (!is_key(key))
        return false;
    ++tally_.equals_keys;
    if (key.find('.') != std::string_view::npos)
        ++tally_.dotted_keys;
    const bool env_style = exported || is_upper_identifier(key);
    if (env_style)
        ++tally_.env_keys;
    note_equals_value(without_inline_comment(trimmed(text.substr(eq + 1))), env_style);
    return true;
}

void KeyValueScanner::note_equals_value(std::string_view value, bool env_style) {
    if (!is_toml_literal(value))
        ++tally_.bare_values;
    if (has_shell_substitution(value) || (!env_style && value.find('$') != std::string_view::npos))
        ++tally_.shell_values;
    if (value.starts_with("\"\"\"") && value.find("\"\"\"", 3) == std::string_view::npos)
        toml_closer_ = "\"\"\"";
    else if (value.starts_with("'''") && value.find("'''", 3) == std::string_view::npos)
        toml_closer_ = "'''";
    else if (value.starts_with('[') && value.find(']') == std::string_view::npos)
        toml_closer_ = "]";
}

bool KeyValueScanner::scan_colon(std::string_view text, std::size_t indent) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos)
        return false;
    const std::string_view key = text.substr(0, colon);
    const std::string_view rest = text.substr(colon + 1);
    if (!is_key(key) || (!rest.empty() && rest.front() != ' ') || text.back() == ';' || text.back() == '{')
        return false;
    ++tally_.colon_keys;
    if (key.find('.') != std::string_view::npos)
        ++tally_.dotted_colon_keys;
    if (std::islower(static_cast<unsigned char>(key.front())) != 0 || indent > 0)
        ++tally_.lowercase_colon_keys;
    if (indent > 0 || key.find_first_of("_-") != std::string_view::npos)
        tally_.has_yaml_evidence = true;
    note_colon_value(trimmed(rest), indent);
    return true;
}

void KeyValueScanner::note_colon_value(std::string_view value, std::size_t indent) {
    if (word_count(value) > kProseValueWords || value.ends_with('.'))
        ++tally_.prose_values;
    if (is_number(value) || value == "true" || value == "false")
        tally_.has_yaml_evidence = true;
    if (value.starts_with('|') || value.starts_with('>')) {
        in_yaml_block_ = true;
        yaml_block_indent_ = indent;
    }
}

bool all_lines_are_keys(const KeyValueTally& t) {
    return t.other == 0 && t.colon_keys == 0 && t.list_items == 0;
}

bool is_env(const KeyValueTally& t) {
    return all_lines_are_keys(t) && t.sections == 0 && t.equals_keys > 0 && t.env_keys == t.equals_keys &&
           t.shell_values == 0;
}

bool is_toml(const KeyValueTally& t) {
    return all_lines_are_keys(t) && t.shell_values == 0 && t.equals_keys > 0 && t.bare_values == 0;
}

bool is_ini(const KeyValueTally& t) {
    if (t.other > 0 || t.list_items > 0 || t.shell_values > 0 || t.equals_keys + t.colon_keys == 0)
        return false;
    return t.sections > 0 || (t.semicolon_comment && t.colon_keys == 0);
}

bool is_properties(const KeyValueTally& t) {
    if (t.other > 0 || t.list_items > 0 || t.sections > 0 || t.shell_values > 0)
        return false;
    if (t.colon_keys > 0)
        return t.equals_keys == 0 && t.dotted_colon_keys == t.colon_keys;
    return t.equals_keys >= 2 || t.dotted_keys > 0;
}

bool is_yaml(const KeyValueTally& t) {
    if (t.tab_indented || t.equals_keys > 0 || t.sections > 0 || t.colon_keys == 0 || t.markdown_headings > 0)
        return false;
    if ((t.content_lines - t.other) * 100 < t.content_lines * kYamlLinePercent)
        return false;
    // Düz yazı `Not: ...`: büyük harfli anahtar ya da cümle uzunluğunda değer.
    if (t.prose_values * 5 > t.colon_keys || t.lowercase_colon_keys * 2 < t.colon_keys)
        return false;
    return t.has_yaml_evidence || t.colon_keys >= kYamlMinPlainKeys;
}

std::optional<Structured> classify_key_values(const std::vector<std::string_view>& lines) {
    const KeyValueTally tally = KeyValueScanner{}.scan(lines);
    if (is_env(tally))
        return Structured::Env;
    if (is_toml(tally))
        return Structured::Toml;
    if (is_ini(tally))
        return Structured::Ini;
    if (is_properties(tally))
        return Structured::Properties;
    if (is_yaml(tally))
        return Structured::Yaml;
    return std::nullopt;
}

std::string lowered_prefix(std::string_view text, std::size_t size) {
    std::string out(text.substr(0, size));
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// İlk etiket adı: `<a href` -> "a"; `<soap:Body` iki noktayı içerir. Etiket değilse boş.
std::string first_tag_name(std::string_view text) {
    std::size_t i = 1;
    while (i < text.size() && (is_key_char(text[i]) || text[i] == ':'))
        ++i;
    const bool closes =
        i >= text.size() || text[i] == ' ' || text[i] == '>' || text[i] == '/' || text[i] == '\t';
    if (i == 1 || !closes || std::isalpha(static_cast<unsigned char>(text[1])) == 0)
        return {};
    return lowered_prefix(text.substr(1), i - 1);
}

std::optional<Structured> classify_markup(const std::vector<std::string_view>& lines) {
    std::size_t first = 0;
    while (first < lines.size() && trimmed(lines[first]).starts_with("<!--"))
        ++first;
    if (first == lines.size() || !trimmed(lines.back()).ends_with('>'))
        return std::nullopt;
    const std::string_view text = trimmed(lines[first]);
    if (text.starts_with("<?xml"))
        return Structured::Xml;
    if (lowered_prefix(text, 14) == "<!doctype html")
        return Structured::Html;
    if (!text.starts_with('<'))
        return std::nullopt;
    const std::string tag = first_tag_name(text);
    if (tag.empty())
        return std::nullopt;
    const bool html = std::ranges::find(kHtmlTags, std::string_view(tag)) != std::ranges::end(kHtmlTags);
    return html ? Structured::Html : Structured::Xml;
}

bool is_docker_instruction(std::string_view text) {
    const std::string_view word = text.substr(0, text.find(' '));
    return std::ranges::find(kDockerInstructions, word) != std::ranges::end(kDockerInstructions);
}

std::optional<Structured> classify_dockerfile(const std::vector<std::string_view>& lines) {
    std::size_t instructions = 0;
    bool continued = false;
    bool has_from = false;
    for (const std::string_view raw : lines) {
        const std::string_view text = trimmed(raw);
        const bool was_continued = continued;
        continued = text.ends_with('\\');
        if (was_continued || text.starts_with('#'))
            continue;
        if (!is_docker_instruction(text))
            return std::nullopt;
        has_from = has_from || text.starts_with("FROM ");
        ++instructions;
    }
    return has_from && instructions >= kDockerfileMinInstructions ? std::optional(Structured::Dockerfile)
                                                                  : std::nullopt;
}

}  // namespace

std::optional<Structured> classify_structure(const std::vector<std::string_view>& lines) {
    if (auto markup = classify_markup(lines))
        return markup;
    if (auto dockerfile = classify_dockerfile(lines))
        return dockerfile;
    return classify_key_values(lines);
}

}  // namespace noteapp::detect
