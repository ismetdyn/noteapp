#include "detect/detect.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

namespace noteapp::detect {
namespace {

// Sıra sabittir: eşit puanlarda aday sırası (kararlı sıralama) buradan gelir.
enum Type : std::size_t { kMd, kTxt, kCpp, kJava, kPy, kJson, kCsv, kSh, kTypeCount };
constexpr std::array<std::string_view, kTypeCount> kTypeNames{"md", "txt",  "cpp", "java",
                                                              "py", "json", "csv", "sh"};
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

// Satırın herhangi bir yerinde geçen basit sinyaller. Konuma bağlı olanlar score_line içinde.
constexpr Signal kContains[] = {
    {"#include <", kCpp, 5},
    {"#include \"", kCpp, 5},
    {"std::", kCpp, 2},
    {"::", kCpp, 1},
    {"template<", kCpp, 3},
    {"template <", kCpp, 3},
    {"->", kCpp, 1},
    {"nullptr", kCpp, 2},
    {"int main(", kCpp, 4},
    {"using namespace", kCpp, 3},
    {"#pragma", kCpp, 2},
    {"public class", kJava, 4},
    {"public interface", kJava, 4},
    {"import java.", kJava, 4},
    {"import javax.", kJava, 4},
    {"System.out.", kJava, 3},
    {"@Override", kJava, 3},
    {"public static void main", kJava, 5},
    {"self.", kPy, 2},
    {"print(", kPy, 2},
    {"__name__", kPy, 3},
    {"range(", kPy, 1.5},
    {"; then", kSh, 3},
    {"; do", kSh, 2},
    {"](", kMd, 2},
};

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

// Kod tipi sinyalleri (kod çiti dışındaki her satır için).
void score_code_line(std::string_view t, std::string_view next, std::size_t indent, Scores& s) {
    for (const auto& sig : kContains) {
        if (t.find(sig.needle) != std::string_view::npos)
            s[sig.type] += sig.weight;
    }
    const bool ends_semicolon = t.ends_with(';');
    const bool ends_colon = t.ends_with(':');

    if (t.starts_with("package ") && ends_semicolon)
        s[kJava] += 4;
    if (t.starts_with("import ") && ends_semicolon && t.find('.') != std::string_view::npos)
        s[kJava] += 2;

    if (t.starts_with("def ") && t.find('(') != std::string_view::npos)
        s[kPy] += 4;
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
    if (t.starts_with("elif "))
        s[kPy] += 2;
    if (t.starts_with("import ") && !ends_semicolon)
        s[kPy] += 2;
    if (t.starts_with("from ") && t.find(" import ") != std::string_view::npos)
        s[kPy] += 2;

    if (has_shell_variable(t))
        s[kSh] += 1.5;
    if (t == "fi" || t == "esac")
        s[kSh] += 3;
    if (t == "done" || t == "do")
        s[kSh] += 2;
    if (t == "then" || t.ends_with(" then"))
        s[kSh] += 3;
    if (t.starts_with("echo "))
        s[kSh] += 2;
    if (t.starts_with("export ") && t.find('=') != std::string_view::npos)
        s[kSh] += 3;
    if (t.starts_with("set -e"))
        s[kSh] += 3;
    if (t.starts_with("if [") || t.starts_with("[[ "))
        s[kSh] += 3;
}

// Markdown sinyalleri; h1 ayrı sayılır (Python/sh yorum satırları "# x" gibi görünür).
void score_markdown_line(std::string_view t, Scores& s, std::size_t& h1_count) {
    std::size_t hashes = 0;
    while (hashes < t.size() && t[hashes] == '#')
        ++hashes;
    if (hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ' &&
        !trim(t.substr(hashes)).empty()) {
        if (hashes == 1)
            ++h1_count;
        else
            s[kMd] += 3;
    }
    if (t.starts_with("- [ ]") || t.starts_with("- [x]") || t.starts_with("- [X]"))
        s[kMd] += 3;
    else if (t.starts_with("- ") || t.starts_with("* ") || is_numbered_item(t))
        s[kMd] += 1;
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
};

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

        const auto next = i + 1 < lines.size() ? lines[i + 1] : std::string_view{};
        score_code_line(t, next, indent_of(lines[i]), r.scores);
        if (!in_block_comment)
            score_markdown_line(t, r.scores, h1_count);

        if (t.find("#include") != std::string_view::npos)
            r.has_include = true;
        if (t.find("import java.") != std::string_view::npos ||
            t.find("import javax.") != std::string_view::npos ||
            (t.starts_with("package ") && t.ends_with(';'))) {
            r.has_java_marker = true;
        }
        if (const auto open = t.find("/*");
            open != std::string_view::npos && t.find("*/", open) == std::string_view::npos) {
            in_block_comment = true;
        } else if (in_block_comment && t.find("*/") != std::string_view::npos) {
            in_block_comment = false;
        }
    }
    // Tek başlık bile belgeyi md yapmaz ama kanıt sayılır; çoğu "# yorum" olan kodu ezmesin diye üst sınır 2.
    r.scores[kMd] += std::min<std::size_t>(h1_count, 1) * 1.5;

    // SPEC ayrım kuralları.
    if (r.has_include)
        r.scores[kJava] = 0;
    if (r.has_java_marker)
        r.scores[kCpp] = 0;
    return r;
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
        const char c = text[i];
        if (c == '"') {
            const auto start = i;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\')
                    ++i;
            }
            if (i >= text.size())
                return false;  // kapanmamış tırnak
            const auto after = text.find_first_not_of(" \t\r\n", i + 1);
            if (!stack.empty() && stack.back() == '{' && after != std::string_view::npos &&
                text[after] == ':' && start < i) {
                saw_key = true;
            }
        } else if (c == '{' || c == '[') {
            stack.push_back(c);
        } else if (c == '}' || c == ']') {
            if (stack.empty() || stack.back() != (c == '}' ? '{' : '['))
                return false;
            stack.pop_back();
            // Kök kapanışı metnin sonu dışında olmamalı ("{} {}" gibi).
            if (stack.empty() && i + 1 != text.size())
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

    // 5'in ön hazırlığı: ham puanlar CSV'nin "kod sinyali yok" koşulu için de gerekli.
    auto scan = score_lines(lines);
    auto& raw = scan.scores;

    // 4. CSV: tutarlı ayırıcı ve kod/biçim sinyali yok.
    const double strongest_other = std::max({raw[kMd], raw[kCpp], raw[kJava], raw[kPy], raw[kSh]});
    if (strongest_other < kCsvBlockingScore && is_consistent_table(lines))
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
