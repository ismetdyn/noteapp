#include "detect/detect.hpp"

#include "detect/structure.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

namespace noteapp::detect {
namespace {

// Sıra sabittir: eşit puanlarda aday sırası (kararlı sıralama) buradan gelir.
enum Type : std::size_t {
    kMd,
    kTxt,
    kCpp,
    kJava,
    kPy,
    kJson,
    kCsv,
    kSh,
    kC,
    kCss,
    kDockerfile,
    kEnv,
    kGo,
    kHtml,
    kIni,
    kJs,
    kMakefile,
    kProperties,
    kRs,
    kSql,
    kToml,
    kTs,
    kXml,
    kYaml,
    kTypeCount
};
constexpr std::array<std::string_view, kTypeCount> kTypeNames{
    "md", "txt",  "cpp", "java", "py",       "json",       "csv", "sh",  "c",    "css", "dockerfile", "env",
    "go", "html", "ini", "js",   "makefile", "properties", "rs",  "sql", "toml", "ts",  "xml",        "yaml"};
using Scores = std::array<double, kTypeCount>;

// Ham puanı [0,1] aralığına sıkıştırır: puan / (toplam + kSmoothing). Hem payı (rakiplere oranı)
// hem de kanıt miktarını (tek zayıf sinyal yüksek güven vermez) tek formülde yakalar.
constexpr double kSmoothing = 2.0;
// Çok satırlı ama hiç sinyal taşımayan metin düz yazıdır; txt'nin taban puanı.
constexpr double kTxtBaseline = 1.0;
// Yapısal kararların (shebang/JSON/CSV) sabit güveni; shebang kesindir.
constexpr double kShebangConfidence = 1.0;
constexpr double kJsonConfidence = 0.95;
constexpr double kCsvConfidence = 0.9;
constexpr double kStructureConfidence = 0.9;  // satır biçimiyle karar: anahtar/değer, işaretleme, Dockerfile
// CSV: satırların en az %90'ı aynı ayırıcı sayısını taşımalı; alan başına ortalama uzunluk düz yazıdan
// ayırır.
constexpr std::size_t kCsvConsistencyPercent = 90;
constexpr double kCsvMaxAvgFieldLength = 40.0;
// CSV'yi engelleyen "kod/biçim sinyali" eşiği (SPEC: kod sinyali yok).
constexpr double kCsvBlockingScore = 3.0;

struct Signal {
    std::string_view needle;
    Type type;
    double weight;
};

// Satırın herhangi bir yerinde geçen basit sinyaller. Konuma bağlı olanlar kPrefixes'ta ve score_line içinde.
constexpr Signal kContains[] = {
    {"#include <", kCpp, 5},
    {"#include \"", kCpp, 5},
    {".h>", kC, 1},
    {"std::", kCpp, 2},
    {"::", kCpp, 1},
    {"template<", kCpp, 3},
    {"template <", kCpp, 3},
    {"->", kCpp, 1},
    {"nullptr", kCpp, 2},
    {"int main(", kCpp, 4},
    {"using namespace", kCpp, 3},
    {"#pragma", kCpp, 2},
    {"printf(", kC, 3},
    {"malloc(", kC, 3},
    {"typedef struct", kC, 4},
    {"(void)", kC, 2},
    {"public class", kJava, 4},
    {"public interface", kJava, 4},
    {"import java.", kJava, 4},
    {"import javax.", kJava, 4},
    {"System.out.", kJava, 3},
    {"@Override", kJava, 3},
    {"@Entity", kJava, 3},
    {"public static void main", kJava, 5},
    {"self.", kPy, 2},
    {"print(", kPy, 2},
    {"__name__", kPy, 3},
    {"range(", kPy, 1.5},
    {"; then", kSh, 3},
    {"; do", kSh, 2},
    {">&2", kSh, 3},
    {"](", kMd, 2},
    {"console.", kJs, 3},
    {"require(", kJs, 3},
    {"module.exports", kJs, 3},
    {"document.", kJs, 3},
    {"window.", kJs, 1.5},
    {"=>", kJs, 1.5},
    {"===", kJs, 1.5},
    {"!==", kJs, 1.5},
    {"await ", kJs, 1.5},
    {"constructor(", kJs, 3},
    {"this.", kJs, 1.5},
    {": string", kTs, 3},
    {": number", kTs, 3},
    {": boolean", kTs, 3},
    {": void", kTs, 3},
    {"<T>", kTs, 3},
    {":=", kGo, 1.5},
    {"fmt.", kGo, 3},
    {"err != nil", kGo, 3},
    {"go func", kGo, 3},
    {" struct {", kGo, 1},
    {"println!(", kRs, 3},
    {"&self", kRs, 3},
    {"$@", kMakefile, 3},
    {"$<", kMakefile, 3},
};

// Kırpılmış satırın başında aranan sinyaller; bir kısmı başka tipin satır içi kelimesidir, başı kesin ayırır.
constexpr Signal kPrefixes[] = {
    {"#ifndef ", kC, 2},       {"#define ", kC, 2},
    {"static int ", kC, 2},    {"void ", kC, 1},
    {"def ", kPy, 2},          {"elif ", kPy, 2},
    {"echo ", kSh, 2},         {"printf ", kSh, 2},
    {"set -e", kSh, 3},        {"if [", kSh, 3},
    {"[[ ", kSh, 3},           {"export default", kJs, 3},
    {"export const ", kJs, 3}, {"export function ", kJs, 3},
    {"export class ", kJs, 3}, {"async function ", kJs, 3},
    {"interface ", kTs, 3},    {"export interface ", kTs, 3},
    {"export enum ", kTs, 4},  {"export type ", kTs, 3},
    {"func ", kGo, 4},         {"import (", kGo, 3},
    {"import \"", kGo, 3},     {"defer ", kGo, 2},
    {"fn ", kRs, 4},           {"pub fn ", kRs, 4},
    {"pub struct ", kRs, 3},   {"pub enum ", kRs, 3},
    {"pub trait ", kRs, 4},    {"impl ", kRs, 4},
    {"use std::", kRs, 4},     {"let mut ", kRs, 3},
    {"#[derive", kRs, 4},      {"@media", kCss, 4},
    {"@import", kCss, 4},      {"@font-face", kCss, 4},
    {"@keyframes", kCss, 4},   {".PHONY", kMakefile, 5},
    {"ifeq ", kMakefile, 2},   {"ifneq ", kMakefile, 2},
    {"ifdef ", kMakefile, 2},  {"endif", kMakefile, 2},
};

// Satırın küçük harfli başında aranır: SQL anahtar sözcükleri büyük/küçük harf ayırmaz.
constexpr Signal kSqlPrefixes[] = {
    {"select ", kSql, 2},       {"where ", kSql, 2},       {"join ", kSql, 1},
    {"order by ", kSql, 2},     {"group by ", kSql, 2},    {"insert into ", kSql, 4},
    {"create table ", kSql, 4}, {"alter table ", kSql, 4}, {"drop table ", kSql, 4},
    {"delete from ", kSql, 4},  {"values ", kSql, 2},      {"-- ", kSql, 1},
};
constexpr std::size_t kSqlPrefixBytes = 16;

// `x {` satırı bu kelimelerle başlıyorsa seçici değil, dil bildirimidir.
constexpr std::string_view kDeclarationWords[] = {"abstract",  "class",   "do",        "else",   "enum",
                                                  "export",    "final",   "finally",   "impl",   "interface",
                                                  "namespace", "private", "protected", "public", "static",
                                                  "struct",    "trait",   "try",       "type",   "union"};

bool starts_with_any(std::string_view t, std::initializer_list<std::string_view> prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(),
                       [&](std::string_view p) { return t.starts_with(p); });
}

std::string_view trim(std::string_view s) {
    constexpr std::string_view kWs = " \t\r\n";
    const auto b = s.find_first_not_of(kWs);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(kWs) - b + 1);
}

std::size_t indent_of(std::string_view line) {
    const auto b = line.find_first_not_of(" \t");
    return b == std::string_view::npos ? 0 : b;
}

// Satır sonu / UTF-8 sınırını bölmeden, ilk max_bytes baytı (BOM'suz) döndürür.
std::string_view clip(std::string_view text, std::size_t max_bytes) {
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    if (text.size() <= max_bytes)
        return text;
    text = text.substr(0, max_bytes);
    // Satır ortasında kesmek sinyalleri bozar; son tam satıra geri dön.
    if (const auto nl = text.rfind('\n'); nl != std::string_view::npos)
        return text.substr(0, nl);
    // Tek uzun satır: yarım kalan UTF-8 dizisini at (devam baytları 10xxxxxx, ardından başlangıç baytı).
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
        text.remove_suffix(1);
    if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80) != 0)
        text.remove_suffix(1);
    return text;
}

std::vector<std::string_view> non_blank_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = text.substr(0, nl);
        if (!trim(line).empty())
            lines.push_back(line);
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return lines;
}

bool has_shell_variable(std::string_view t) {
    for (auto pos = t.find('$'); pos != std::string_view::npos; pos = t.find('$', pos + 1)) {
        if (pos + 1 >= t.size())
            break;
        const unsigned char c = static_cast<unsigned char>(t[pos + 1]);
        if (std::isalpha(c) || c == '_' || c == '{' || c == '(')
            return true;
    }
    return false;
}

// "1. " biçimli numaralı liste maddesi mi?
bool is_numbered_item(std::string_view t) {
    std::size_t i = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])))
        ++i;
    return i > 0 && i <= 3 && t.substr(i).starts_with(". ");
}

bool has_bold(std::string_view t) {
    const auto a = t.find("**");
    return a != std::string_view::npos && t.find("**", a + 2) != std::string_view::npos;
}

void score_table(std::string_view t, Scores& s) {
    for (const auto& sig : kContains)
        if (t.find(sig.needle) != std::string_view::npos)
            s[sig.type] += sig.weight;
    for (const auto& sig : kPrefixes)
        if (t.starts_with(sig.needle))
            s[sig.type] += sig.weight;
}

bool has_word_token(std::string_view t, std::string_view token) {
    return t.find(token) != std::string_view::npos;
}

void score_java_lines(std::string_view t, Scores& s) {
    if (!t.ends_with(';'))
        return;
    if (t.starts_with("package "))
        s[kJava] += 4;
    // TS/JS `import x from 'y';` de noktalı virgülle biter; Java'da tırnak olmaz.
    if (t.starts_with("import ") && has_word_token(t, ".") &&
        t.find_first_of("\"'") == std::string_view::npos)
        s[kJava] += 2;
}

void score_python_lines(std::string_view t, std::string_view next, std::size_t indent, Scores& s) {
    const bool ends_colon = t.ends_with(':');
    if (t.starts_with("def ") && has_word_token(t, "("))
        s[kPy] += 2;
    // Düz yazı "x:" ile bitebilir; yalnızca Python anahtar sözcüğüyle başlayan + altında girintili blok olan
    // satır sayılır.
    if (ends_colon &&
        starts_with_any(t, {"class ", "if ", "elif ", "else", "for ", "while ", "try", "except", "finally",
                            "with ", "def "}) &&
        !next.empty() && indent_of(next) > indent) {
        s[kPy] += 1.5;
    }
    if (t.starts_with("class ") && ends_colon)
        s[kPy] += 3;
    // Go `import "fmt"`, JS `import a from 'b'` ve Java `import a.b;` Python değildir.
    if (t.starts_with("import ") && !t.ends_with(';') && t.find_first_of("\"'(") == std::string_view::npos &&
        !has_word_token(t, " from "))
        s[kPy] += 2;
    if (t.starts_with("from ") && has_word_token(t, " import "))
        s[kPy] += 2;
}

void score_shell_lines(std::string_view t, Scores& s) {
    if (has_shell_variable(t))
        s[kSh] += 1.5;
    if (t == "fi" || t == "esac")
        s[kSh] += 3;
    if (t == "done" || t == "do")
        s[kSh] += 2;
    if (t == "then" || t.ends_with(" then"))
        s[kSh] += 3;
    if (t.starts_with("export ") && has_word_token(t, "="))
        s[kSh] += 3;
    // `ad() {`: C'de dönüş tipi önce gelir, boşluk içerir.
    if (t.ends_with("() {") && t.find(' ') + 1 == t.size() - 1)
        s[kSh] += 2;
}

void score_script_lines(std::string_view t, Scores& s) {
    const bool assigns = has_word_token(t, " = ");
    if (assigns && starts_with_any(t, {"const ", "let ", "var "}))
        s[kJs] += 1.5;
    if (t.starts_with("function ") && has_word_token(t, "("))
        s[kJs] += 4;
    if (t.starts_with("import ") && has_word_token(t, " from ") &&
        t.find_first_of("\"'") != std::string_view::npos)
        s[kJs] += 3;
    if (assigns && t.starts_with("type "))
        s[kTs] += 4;
    if (t.starts_with("class ") && t.ends_with('{'))
        s[kJs] += 1;
}

void score_go_package(std::string_view t, Scores& s) {
    if (t.starts_with("package ") && !t.ends_with(';') && t.find(' ', 8) == std::string_view::npos)
        s[kGo] += 4;
}

void score_sql_line(std::string_view t, std::string_view next, Scores& s) {
    char lowered[kSqlPrefixBytes];
    const std::size_t size = std::min(t.size(), kSqlPrefixBytes);
    std::transform(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(size), lowered,
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string_view head(lowered, size);
    for (const auto& sig : kSqlPrefixes)
        if (head.starts_with(sig.needle))
            s[sig.type] += sig.weight;
    // Python `from a import b`; İngilizce "Update the list" `set` olmadan SQL değildir.
    if (head.starts_with("from ") && !has_word_token(t, " import "))
        s[kSql] += 2;
    if (head.starts_with("set ") && has_word_token(t, "="))
        s[kSql] += 1;
    if (head.starts_with("update ") && (has_word_token(t, " SET ") || has_word_token(t, " set ") ||
                                        starts_with_any(trim(next), {"SET ", "set "})))
        s[kSql] += 2;
}

bool is_css_declaration(std::string_view t) {
    const auto colon = t.find(':');
    if (colon == std::string_view::npos || colon == 0 || !t.ends_with(';'))
        return false;
    const std::string_view name = t.substr(0, colon);
    const bool name_ok =
        std::ranges::all_of(name, [](unsigned char c) { return std::islower(c) != 0 || c == '-'; });
    const std::string_view value = trim(t.substr(colon + 1));
    return name_ok && !starts_with_any(value, {"string", "number", "boolean"});
}

bool is_css_selector(std::string_view t) {
    if (!t.ends_with('{') || t.find_first_of("(=") != std::string_view::npos)
        return false;
    const std::string_view word = t.substr(0, t.find(' '));
    return std::ranges::find(kDeclarationWords, word) == std::ranges::end(kDeclarationWords);
}

void score_css_line(std::string_view t, Scores& s) {
    if (is_css_selector(t) || is_css_declaration(t))
        s[kCss] += 1.5;
    // Tek satırlık kural: `a { color: blue; }`.
    if (t.ends_with("; }") && has_word_token(t, "{"))
        s[kCss] += 3;
}

// Hedef satırının altındaki sekmeyle girintili satır Makefile tarifidir; `prev` tam haliyle gelir.
void score_makefile_line(std::string_view raw, std::string_view prev, std::string_view t, Scores& s) {
    const bool recipe = raw.starts_with('\t') && !prev.empty() && prev.front() != '\t' &&
                        prev.front() != '#' && prev.find(':') != std::string_view::npos &&
                        prev.find("::") == std::string_view::npos;
    if (recipe)
        s[kMakefile] += 6;
    if (raw.front() != ' ' && raw.front() != '\t' && (has_word_token(t, " := ") || has_word_token(t, " ?= ")))
        s[kMakefile] += 2;
}

// Kod tipi sinyalleri (kod çiti dışındaki her satır için).
void score_code_line(std::string_view raw, std::string_view prev, std::string_view next, Scores& s) {
    const std::string_view t = trim(raw);
    const std::size_t indent = indent_of(raw);
    score_table(t, s);
    score_java_lines(t, s);
    score_python_lines(t, next, indent, s);
    score_shell_lines(t, s);
    score_script_lines(t, s);
    score_go_package(t, s);
    score_sql_line(t, next, s);
    score_css_line(t, s);
    score_makefile_line(raw, prev, t, s);
}

// 1..6 diyez + boşluk + metin: başlık düzeyi; değilse 0.
std::size_t heading_level(std::string_view t) {
    std::size_t hashes = 0;
    while (hashes < t.size() && t[hashes] == '#')
        ++hashes;
    const bool valid = hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ' &&
                       !trim(t.substr(hashes)).empty();
    return valid ? hashes : 0;
}

void score_markdown_list(std::string_view t, Scores& s) {
    if (t.starts_with("- [ ]") || t.starts_with("- [x]") || t.starts_with("- [X]"))
        s[kMd] += 3;
    else if (t.starts_with("- ") || t.starts_with("* ") || is_numbered_item(t))
        s[kMd] += 1;
}

// Markdown sinyalleri; h1 ayrı sayılır (Python/sh yorum satırları "# x" gibi görünür).
void score_markdown_line(std::string_view t, Scores& s, std::size_t& h1_count) {
    if (const std::size_t level = heading_level(t); level == 1)
        ++h1_count;
    else if (level > 1)
        s[kMd] += 3;
    score_markdown_list(t, s);
    if (t.starts_with("> "))
        s[kMd] += 1;
    if (has_bold(t))
        s[kMd] += 2;
    if (t.front() == '|' && std::count(t.begin(), t.end(), '|') >= 2)
        s[kMd] += t.find("---") != std::string_view::npos ? 3 : 2;
}

struct LineScan {
    Scores scores{};
    bool has_include = false;
    bool has_java_marker = false;
    bool has_cpp_marker = false;  // C'de olamayan sözdizimi: std::, şablon, başlıksız #include <...>
    bool has_class = false;       // `class` C'de yok ama JS'te de var: yalnız C/C++ ayrımında sayılır
    bool has_go_marker = false;
    bool has_rust_marker = false;
};

bool is_cpp_only(std::string_view t) {
    for (const std::string_view token : {"std::", "template", "namespace ", "nullptr", "cout"})
        if (t.find(token) != std::string_view::npos)
            return true;
    return t.starts_with("#include <") && t.find(".h>") == std::string_view::npos;
}

void note_markers(std::string_view t, LineScan& r) {
    r.has_include = r.has_include || t.find("#include") != std::string_view::npos;
    r.has_java_marker = r.has_java_marker || t.find("import java.") != std::string_view::npos ||
                        t.find("import javax.") != std::string_view::npos ||
                        (t.starts_with("package ") && t.ends_with(';'));
    r.has_cpp_marker = r.has_cpp_marker || is_cpp_only(t);
    r.has_class = r.has_class || t.starts_with("class ") || t == "public:" || t == "private:";
    r.has_go_marker =
        r.has_go_marker || t.starts_with("func ") || (t.starts_with("package ") && !t.ends_with(';'));
    r.has_rust_marker =
        r.has_rust_marker || starts_with_any(t, {"fn ", "pub fn ", "impl ", "use std::", "let mut ",
                                                 "#[derive", "pub struct ", "pub enum ", "pub trait "});
}

void clear_scores(Scores& s, std::initializer_list<Type> types) {
    for (const Type type : types)
        s[type] = 0;
}

// Akraba diller aynı sinyalleri paylaşır; ayırt edici tek işaret tüm aileyi karara bağlar.
void resolve_families(LineScan& r) {
    Scores& s = r.scores;
    if (r.has_include)
        s[kJava] = 0;
    if (r.has_java_marker)
        clear_scores(s, {kCpp, kC, kJs, kTs, kCss, kGo});
    if (r.has_cpp_marker)
        clear_scores(s, {kC, kJs, kTs, kCss});
    else if (r.has_class)
        s[kC] = 0;
    else if (s[kC] > 0) {
        s[kC] += s[kCpp];
        s[kCpp] = 0;
    }
    if (r.has_go_marker)
        clear_scores(s, {kCpp, kC, kJs, kTs, kCss, kPy});
    if (r.has_rust_marker)
        clear_scores(s, {kCpp, kC, kJs, kTs, kCss, kPy, kGo});
    if (s[kTs] > 0) {
        s[kTs] += s[kJs];
        s[kJs] = 0;
    }
}

LineScan score_lines(const std::vector<std::string_view>& lines) {
    LineScan r;
    std::size_t h1_count = 0;
    bool in_fence = false;
    bool in_block_comment = false;  // javadoc satırlarındaki " * x" markdown listesi sanılmasın

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto t = trim(lines[i]);
        if (t.starts_with("```")) {
            in_fence = !in_fence;
            r.scores[kMd] += 2;
            continue;
        }
        if (in_fence)
            continue;  // çit içi kod hiçbir tipe sayılmaz

        const auto prev = i > 0 ? lines[i - 1] : std::string_view{};
        const auto next = i + 1 < lines.size() ? lines[i + 1] : std::string_view{};
        score_code_line(lines[i], prev, next, r.scores);
        if (!in_block_comment)
            score_markdown_line(t, r.scores, h1_count);
        note_markers(t, r);
        if (const auto open = t.find("/*");
            open != std::string_view::npos && t.find("*/", open) == std::string_view::npos) {
            in_block_comment = true;
        } else if (in_block_comment && t.find("*/") != std::string_view::npos) {
            in_block_comment = false;
        }
    }
    // Tek başlık bile belgeyi md yapmaz ama kanıt sayılır; çoğu "# yorum" olan kodu ezmesin diye üst sınır 2.
    r.scores[kMd] += std::min<std::size_t>(h1_count, 1) * 1.5;
    resolve_families(r);
    return r;
}

// Kapanış tırnağının konumu; yoksa npos. `open` açılış tırnağıdır.
std::size_t json_string_end(std::string_view text, std::size_t open) {
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '"')
            return i;
        if (text[i] == '\\')
            ++i;
    }
    return std::string_view::npos;
}

bool is_json_key_end(std::string_view text, std::size_t close, const std::vector<char>& stack) {
    const auto after = text.find_first_not_of(" \t\r\n", close + 1);
    return !stack.empty() && stack.back() == '{' && after != std::string_view::npos && text[after] == ':';
}

// Parantezi yığına işler; dengesizlikte false. Kök kapanışı metnin sonu dışında olmamalı ("{} {}" gibi).
bool track_json_bracket(std::string_view text, std::size_t i, std::vector<char>& stack) {
    const char c = text[i];
    if (c == '{' || c == '[') {
        stack.push_back(c);
    } else if (c == '}' || c == ']') {
        if (stack.empty() || stack.back() != (c == '}' ? '{' : '['))
            return false;
        stack.pop_back();
        if (stack.empty() && i + 1 != text.size())
            return false;
    }
    return true;
}

bool is_structural_json(std::string_view text) {
    text = trim(text);
    if (text.size() < 2)
        return false;
    const char open = text.front();
    if (!((open == '{' && text.back() == '}') || (open == '[' && text.back() == ']')))
        return false;

    std::vector<char> stack;
    bool saw_key = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {
            const std::size_t close = json_string_end(text, i);
            if (close == std::string_view::npos)
                return false;  // kapanmamış tırnak
            saw_key = saw_key || is_json_key_end(text, close, stack);
            i = close;
        } else if (!track_json_bracket(text, i, stack)) {
            return false;
        }
    }
    return stack.empty() && (open == '[' || saw_key);
}

// Tırnak dışındaki ayırıcıları sayar (alıntılı alandaki virgül sayılmaz).
std::size_t count_delimiter(std::string_view line, char delim) {
    std::size_t n = 0;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"')
            quoted = !quoted;
        else if (c == delim && !quoted)
            ++n;
    }
    return n;
}

bool is_consistent_table(const std::vector<std::string_view>& lines) {
    for (const char delim : {',', ';', '\t'}) {
        std::vector<std::size_t> counts;
        counts.reserve(lines.size());
        std::size_t chars = 0;
        for (const auto l : lines) {
            counts.push_back(count_delimiter(l, delim));
            chars += l.size();
        }
        // En sık görülen ayırıcı sayısı (mod); kararlılık için sıralayıp en uzun koşuyu bul.
        auto sorted = counts;
        std::sort(sorted.begin(), sorted.end());
        std::size_t mode = sorted.front(), best_run = 0;
        for (std::size_t i = 0, run = 0; i < sorted.size(); ++i) {
            run = (i > 0 && sorted[i] == sorted[i - 1]) ? run + 1 : 1;
            if (run > best_run) {
                best_run = run;
                mode = sorted[i];
            }
        }
        if (mode == 0 || best_run * 100 < lines.size() * kCsvConsistencyPercent)
            continue;
        const double avg_field =
            static_cast<double>(chars) / (static_cast<double>(lines.size()) * (mode + 1));
        if (avg_field <= kCsvMaxAvgFieldLength)
            return true;
    }
    return false;
}

// Sıralı aday listesi; `chosen` (varsa) başa alınır ki type == ranked[0].type kalsın.
Suggestion finish(const Scores& scores, std::string type, double confidence, bool detected) {
    Suggestion out{std::move(type), confidence, {}, detected};
    out.ranked.reserve(kTypeCount);
    for (std::size_t i = 0; i < kTypeCount; ++i)
        out.ranked.push_back({std::string(kTypeNames[i]), scores[i]});
    std::stable_sort(out.ranked.begin(), out.ranked.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const auto it = std::find_if(out.ranked.begin(), out.ranked.end(),
                                 [&](const Candidate& c) { return c.type == out.type; });
    if (it != out.ranked.end())
        std::rotate(out.ranked.begin(), it, it + 1);
    return out;
}

Suggestion decided(Type type, double confidence) {
    Scores s{};
    s[type] = confidence;
    return finish(s, std::string(kTypeNames[type]), confidence, true);
}

Type type_of(Structured structured) {
    switch (structured) {
    case Structured::Env:
        return kEnv;
    case Structured::Properties:
        return kProperties;
    case Structured::Ini:
        return kIni;
    case Structured::Toml:
        return kToml;
    case Structured::Yaml:
        return kYaml;
    case Structured::Xml:
        return kXml;
    case Structured::Html:
        return kHtml;
    case Structured::Dockerfile:
        return kDockerfile;
    }
    return kTxt;
}

// Bu tiplerin satır sinyali CSV'nin "kod sinyali yok" koşulunu bozar.
double strongest_signal(const Scores& raw) {
    double strongest = 0;
    for (std::size_t i = 0; i < kTypeCount; ++i)
        if (i != kTxt && i != kCsv && i != kJson)
            strongest = std::max(strongest, raw[i]);
    return strongest;
}

}  // namespace

Suggestion suggest(std::string_view content, const Options& options) {
    const auto text = clip(content, options.max_scan_bytes);
    const auto lines = non_blank_lines(text);

    // 1. Boş / tek satır: hızlı notlar algılamaya girmez.
    if (lines.size() < 2)
        return finish(Scores{}, options.default_type, 0.0, false);

    // 2. Shebang.
    if (trim(lines.front()).starts_with("#!")) {
        return decided(lines.front().find("python") != std::string_view::npos ? kPy : kSh,
                       kShebangConfidence);
    }

    // 3. JSON.
    if (is_structural_json(text))
        return decided(kJson, kJsonConfidence);

    // 3b. Satır biçimi kesin: anahtar/değer ailesi, işaretleme, Dockerfile.
    if (const auto structured = classify_structure(lines))
        return decided(type_of(*structured), kStructureConfidence);

    // 5'in ön hazırlığı: ham puanlar CSV'nin "kod sinyali yok" koşulu için de gerekli.
    auto scan = score_lines(lines);
    auto& raw = scan.scores;

    // 4. CSV: tutarlı ayırıcı ve kod/biçim sinyali yok.
    if (strongest_signal(raw) < kCsvBlockingScore && is_consistent_table(lines))
        return decided(kCsv, kCsvConfidence);

    // 5. Puanlama.
    raw[kTxt] = kTxtBaseline;
    double total = 0;
    for (const double v : raw)
        total += v;
    Scores normalized{};
    for (std::size_t i = 0; i < kTypeCount; ++i)
        normalized[i] = raw[i] / (total + kSmoothing);

    const auto top =
        static_cast<std::size_t>(std::max_element(normalized.begin(), normalized.end()) - normalized.begin());
    if (normalized[top] >= options.min_confidence) {
        return finish(normalized, std::string(kTypeNames[top]), normalized[top], true);
    }

    // 6. Düşük güven: markdown sinyali varsa md, yoksa düz yazı. (Çok satırlı metin için default_type
    // kullanılmaz.)
    const Type fallback = raw[kMd] > 0 ? kMd : kTxt;
    return finish(normalized, std::string(kTypeNames[fallback]), normalized[fallback], true);
}

}  // namespace noteapp::detect
