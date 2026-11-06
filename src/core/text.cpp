#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <system_error>
#include <utility>

// md4c'nin C başlığı C++ bağlama bildirmez.
extern "C" {
#include <entity.h>
}

namespace noteapp::core {

namespace {

constexpr std::string_view kStampPrefix = "note-";
constexpr const char* kStampFormat = "%Y%m%d-%H%M%S";

bool is_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// Boşluk dahil kırpılacak baştaki işaretler: markdown başlık/liste/alıntı belirteçleri.
bool is_leading_marker(char c) {
    return c == '#' || c == '-' || c == '*' || c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string timestamp_name(std::chrono::system_clock::time_point now) {
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::array<char, 32> buffer{};
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), kStampFormat, &local);
    return std::string(kStampPrefix) + std::string(buffer.data(), length);
}

constexpr std::string_view kBlank = " \t\r";
constexpr std::string_view kExportPrefix = "export ";

std::string_view trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(kBlank);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(kBlank) - first + 1);
}

struct Assignment {
    std::string_view key;
    char separator{};
    std::string_view value;
};

// `[export ]anahtar<ayraç>değer`: anahtar ilk `=` ya da `:` öncesidir. Ayraç yoksa ya da anahtar boşsa
// nullopt.
std::optional<Assignment> split_assignment(std::string_view line) {
    line = trimmed(line);
    if (line.starts_with(kExportPrefix))
        line = trimmed(line.substr(kExportPrefix.size()));
    const std::size_t at = line.find_first_of("=:");
    if (at == std::string_view::npos || trimmed(line.substr(0, at)).empty())
        return std::nullopt;
    return Assignment{trimmed(line.substr(0, at)), line[at], trimmed(line.substr(at + 1))};
}

// Tırnak ve Türkçe bağımsız: düzyazı başlığı (`Toplantı: bütçe`, `int x`) anahtar sayılmasın diye yalnız
// ASCII.
bool is_ascii_identifier(std::string_view text) {
    const auto is_letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (text.empty() || !is_letter(text.front()))
        return false;
    return std::ranges::all_of(
        text, [&](char c) { return is_letter(c) || (c >= '0' && c <= '9') || c == '.' || c == '-'; });
}

// `:` düzyazıda da geçer: yalnız tek sözcüklük ya da tırnaklı değer anahtar sayılır. `::` kod, `://`
// adrestir; adresteki kimlik bilgisi (`@`) yine ada girmesin.
bool is_value_assignment(const Assignment& assignment) {
    const std::string_view value = assignment.value;
    if (!is_ascii_identifier(assignment.key) || value.empty())
        return false;
    if (assignment.separator == '=')
        return value.front() != '=';
    if (value.starts_with("//"))
        return value.find('@') != std::string_view::npos;
    if (value.front() == ':')
        return false;
    return value.front() == '"' || value.front() == '\'' ||
           value.find_first_of(kBlank) == std::string_view::npos;
}

// Sezgi: kısa belirteç ve rakamsız uzun sözcük kaçar; uzun camelCase kimlik öneriden düşer. Yalnız öneri
// içindir, sır tarayıcısı değildir.
bool is_token_like(std::string_view word) {
    constexpr std::array<std::string_view, 11> kPrefixes = {
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "sk-", "AKIA", "xoxb-", "xoxp-", "glpat-"};
    constexpr std::size_t kMinPrefixedBytes = 16;
    constexpr std::size_t kMinBareBytes = 32;
    if (word.size() >= kMinPrefixedBytes &&
        std::ranges::any_of(kPrefixes, [&](std::string_view prefix) { return word.starts_with(prefix); }))
        return true;
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto is_letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    return word.size() >= kMinBareBytes && std::ranges::any_of(word, is_digit) &&
           std::ranges::any_of(word, is_letter);
}

bool is_comment_or_structure(std::string_view line) {
    constexpr std::string_view kLineMarkers = "#;![-";
    return line.empty() || kLineMarkers.find(line.front()) != std::string_view::npos;
}

}  // namespace

namespace {

struct CodePoint {
    char32_t value = 0;
    std::size_t length = 0;
};

// `at`taki ASCII olmayan kod noktası; geçersizse (fazla uzun kodlama, vekil, > U+10FFFF, kesik) nullopt.
std::optional<CodePoint> multibyte_at(std::string_view utf8, std::size_t at) {
    const auto lead = static_cast<unsigned char>(utf8[at]);
    std::size_t extra = 0;
    char32_t cp = 0;
    char32_t min_cp = 0;  // fazla uzun kodlamayı yakalamak için alt sınır
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1, cp = lead & 0x1Fu, min_cp = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2, cp = lead & 0x0Fu, min_cp = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3, cp = lead & 0x07u, min_cp = 0x10000;
    } else {
        return std::nullopt;
    }
    if (at + extra >= utf8.size()) {
        return std::nullopt;  // kesik dizi
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const auto next = static_cast<unsigned char>(utf8[at + k]);
        if (!is_continuation(next))
            return std::nullopt;
        cp = (cp << 6) | (next & 0x3Fu);
    }
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (cp < min_cp || cp > 0x10FFFF || surrogate)
        return std::nullopt;
    return CodePoint{cp, extra + 1};
}

}  // namespace

std::optional<std::u32string> decode_utf8(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        const auto cp = multibyte_at(utf8, i);
        if (!cp)
            return std::nullopt;
        out.push_back(cp->value);
        i += cp->length;
    }
    return out;
}

namespace {

// Varlığın tek kod noktası metne nasıl girer (adlı ve sayısal biçim aynı kuraldan geçer: `&NewLine;` = `&#10;`).
// HTML'de boşluk varlığı boşluktur; satır sonu burada zorunlu satır sonu olurdu. Vekil, 0, U+10FFFF üstü ve
// denetim karakteri (C0, DEL, C1: tek baytlık CSI U+009B dahil) metne girmez.
std::string entity_code_point(char32_t value) {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    constexpr char32_t kMaxCodePoint = 0x10FFFF;
    if (value == '\t')
        return "\t";
    if (value == '\n' || value == '\r')
        return " ";
    const bool is_control = value < 0x20 || (value >= 0x7F && value <= 0x9F);
    const bool is_valid = !is_control && value <= kMaxCodePoint && !(value >= 0xD800 && value <= 0xDFFF);
    return is_valid ? encode_utf8(std::u32string(1, value)) : std::string(kReplacement);
}

}  // namespace

std::string decode_html_entity(std::string_view entity) {
    if (entity.size() <= 3 || entity[1] != '#') {
        // entity_lookup önek karşılaştırır (strncmp, anahtar boyunda): yalnız `&ad;` biçimi aranır.
        const bool is_named = entity.starts_with('&') && entity.ends_with(';');
        const ENTITY* named = is_named ? entity_lookup(entity.data(), entity.size()) : nullptr;
        if (named == nullptr)
            return std::string(entity);
        std::string out = entity_code_point(static_cast<char32_t>(named->codepoints[0]));
        if (named->codepoints[1] != 0)
            out += entity_code_point(static_cast<char32_t>(named->codepoints[1]));
        return out;
    }
    const bool is_hex = entity[2] == 'x' || entity[2] == 'X';
    const std::string_view digits = entity.substr(is_hex ? 3 : 2, entity.size() - (is_hex ? 4 : 3));
    std::uint32_t value = 0;
    const auto [end, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, is_hex ? 16 : 10);
    if (ec != std::errc{} || end != digits.data() + digits.size())
        return "\xEF\xBF\xBD";
    return entity_code_point(static_cast<char32_t>(value));
}

std::string encode_utf8(std::u32string_view codepoints) {
    std::string out;
    out.reserve(codepoints.size());
    for (const char32_t cp : codepoints) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Kod noktaları üretilmez: 20 MiB'lık notta 80 MiB'lık geçici dizi gerekmesin. ASCII sekizer bayt atlanır (not
// açılışında tüm metin denetlenir).
bool is_valid_utf8(std::string_view utf8) {
    constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
    std::size_t i = 0;
    while (i < utf8.size()) {
        std::uint64_t word = 0;
        if (i + sizeof(word) <= utf8.size()) {
            std::memcpy(&word, utf8.data() + i, sizeof(word));
            if ((word & kHighBits) == 0) {
                i += sizeof(word);
                continue;
            }
        }
        if (static_cast<unsigned char>(utf8[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto cp = multibyte_at(utf8, i);
        if (!cp)
            return false;
        i += cp->length;
    }
    return true;
}

namespace {

// Kod noktasına göre sıralı.
constexpr std::array<HiddenControl, 24> kHiddenControls = {{
    {0x061C, "ARABIC LETTER MARK"},
    {0x115F, "HANGUL CHOSEONG FILLER"},
    {0x1160, "HANGUL JUNGSEONG FILLER"},
    {0x180E, "MONGOLIAN VOWEL SEPARATOR"},
    {0x200B, "ZERO WIDTH SPACE"},
    {0x200E, "LEFT-TO-RIGHT MARK"},
    {0x200F, "RIGHT-TO-LEFT MARK"},
    {0x202A, "LEFT-TO-RIGHT EMBEDDING"},
    {0x202B, "RIGHT-TO-LEFT EMBEDDING"},
    {0x202C, "POP DIRECTIONAL FORMATTING"},
    {0x202D, "LEFT-TO-RIGHT OVERRIDE"},
    {0x202E, "RIGHT-TO-LEFT OVERRIDE"},
    {0x2060, "WORD JOINER"},
    {0x2061, "FUNCTION APPLICATION"},
    {0x2062, "INVISIBLE TIMES"},
    {0x2063, "INVISIBLE SEPARATOR"},
    {0x2064, "INVISIBLE PLUS"},
    {0x2066, "LEFT-TO-RIGHT ISOLATE"},
    {0x2067, "RIGHT-TO-LEFT ISOLATE"},
    {0x2068, "FIRST STRONG ISOLATE"},
    {0x2069, "POP DIRECTIONAL ISOLATE"},
    {0x3164, "HANGUL FILLER"},
    {0xFEFF, "ZERO WIDTH NO-BREAK SPACE"},
    {0xFFA0, "HALFWIDTH HANGUL FILLER"},
}};

}  // namespace

std::span<const HiddenControl> hidden_controls() {
    return kHiddenControls;
}

const HiddenControl* find_hidden_control(char32_t cp) {
    // Editör her ASCII dışı kod noktasında sorar: aralık dışı (Türkçe harfler dahil) taramaya girmez.
    if (cp < kHiddenControls.front().code_point || cp > kHiddenControls.back().code_point)
        return nullptr;
    const auto* found = std::ranges::find(kHiddenControls, cp, &HiddenControl::code_point);
    return found == kHiddenControls.end() ? nullptr : found;
}

namespace {

constexpr char32_t kTwoByteLimit = 0x800;
constexpr char32_t kThreeByteLimit = 0x10000;
static_assert(std::ranges::all_of(kHiddenControls,
                                  [](const HiddenControl& hidden) {
                                      return hidden.code_point < kThreeByteLimit;
                                  }),
              "count_hidden_controls decodes at most three bytes");

// Listedeki kod noktalarının UTF-8 öncü baytları, listeden derleme anında türetilir: listeye eklenen kod
// noktası ön süzgeçten kaçamaz.
constexpr std::array<bool, 256> kHiddenLeadBytes = [] {
    std::array<bool, 256> leads{};
    for (const HiddenControl& hidden : kHiddenControls) {
        const char32_t cp = hidden.code_point;
        leads[cp < kTwoByteLimit ? (0xC0U | (cp >> 6U)) : (0xE0U | (cp >> 12U))] = true;
    }
    return leads;
}();

}  // namespace

std::size_t count_hidden_controls(std::string_view utf8) {
    // Yalnız listedeki öncü baytlarda çözülür; öncü bayt devam baytı olamaz, her bayttan başlamak güvenli.
    constexpr unsigned char kThreeByteLead = 0xE0;
    std::size_t count = 0;
    for (std::size_t at = 0; at < utf8.size(); ++at) {
        const auto lead = static_cast<unsigned char>(utf8[at]);
        if (!kHiddenLeadBytes[lead])
            continue;
        const std::size_t length = lead >= kThreeByteLead ? 3 : 2;
        const auto decoded = decode_utf8(utf8.substr(at, length));
        if (decoded && decoded->size() == 1 && find_hidden_control(decoded->front())) {
            ++count;
            at += length - 1;
        }
    }
    return count;
}

std::string transliterate_turkish(std::string_view utf8) {
    auto decoded = decode_utf8(utf8);
    if (!decoded)
        return std::string(utf8);
    for (char32_t& cp : *decoded) {
        switch (cp) {
        case U'ç':
            cp = U'c';
            break;
        case U'Ç':
            cp = U'C';
            break;
        case U'ğ':
            cp = U'g';
            break;
        case U'Ğ':
            cp = U'G';
            break;
        case U'ı':
        case U'İ':
            cp = U'i';
            break;
        case U'ö':
            cp = U'o';
            break;
        case U'Ö':
            cp = U'O';
            break;
        case U'ş':
            cp = U's';
            break;
        case U'Ş':
            cp = U'S';
            break;
        case U'ü':
            cp = U'u';
            break;
        case U'Ü':
            cp = U'U';
            break;
        default:
            break;
        }
    }
    return encode_utf8(*decoded);
}

namespace {

std::string slug_of(std::string_view first_line, std::size_t max_length) {
    while (!first_line.empty() && is_leading_marker(first_line.front()))
        first_line.remove_prefix(1);

    // Geçersiz UTF-8 dönüştürülmeden gelir; ASCII olmayan baytlar zaten ayırıcı olacağından zararsız.
    const std::string ascii = transliterate_turkish(first_line);

    std::string slug;
    for (const char raw : ascii) {
        const char c = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            slug.push_back(c);
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();

    if (slug.size() > max_length) {
        // rfind(pos) pos dahil arar: tam max_length'teki tire de temiz bir kesim noktasıdır.
        const std::size_t dash = slug.rfind('-', max_length);
        slug.resize(dash == std::string::npos ? max_length : dash);
        while (!slug.empty() && slug.back() == '-')
            slug.pop_back();
    }
    return slug;
}

}  // namespace

std::string slugify(std::string_view first_line, std::chrono::system_clock::time_point now,
                    std::size_t max_length) {
    std::string slug = slug_of(first_line, max_length);
    return slug.empty() ? timestamp_name(now) : slug;
}

TypedName split_typed_name(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size() ||
        name.find_first_of(kBlank, dot) != std::string_view::npos)
        return {name, {}};
    return {name.substr(0, dot), name.substr(dot + 1)};
}

std::string slugify_stem(std::string_view stem, std::chrono::system_clock::time_point now,
                         std::size_t max_length) {
    std::string joined;
    while (true) {
        const std::size_t dot = stem.find('.');
        const std::string part = slug_of(stem.substr(0, dot), max_length);
        if (!part.empty())
            joined += (joined.empty() ? "" : ".") + part;
        if (dot == std::string_view::npos)
            break;
        stem.remove_prefix(dot + 1);
    }
    return joined.empty() ? timestamp_name(now) : joined;
}

std::string normalize_line_endings(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;  // CR atlanır, LF kalır
        out += text[i];
    }
    return out;
}

std::string first_non_empty_line(std::string_view content) {
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos)
            end = content.size();
        const std::string_view line = content.substr(start, end - start);
        if (line.find_first_not_of(" \t\r") != std::string_view::npos)
            return std::string(line);
        start = end + 1;
    }
    return {};
}

bool type_has_secret_values(std::string_view type) {
    return type == "env" || type == "properties" || type == "ini" || type == "toml" || type == "yaml";
}

std::string_view secret_line_key(std::string_view line) {
    line = trimmed(line);
    if (is_comment_or_structure(line))
        return {};
    const auto assignment = split_assignment(line);
    // Boşluk içeren "anahtar" düzyazı ya da çıplak değerdir.
    return assignment && assignment->key.find_first_of(kBlank) == std::string_view::npos ? assignment->key
                                                                                         : std::string_view{};
}

std::string_view assignment_line_key(std::string_view line) {
    const auto assignment = split_assignment(line);
    return assignment && is_value_assignment(*assignment) ? assignment->key : std::string_view{};
}

bool has_token_like_word(std::string_view text) {
    constexpr std::string_view kTokenAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-";
    std::size_t at = text.find_first_of(kTokenAlphabet);
    while (at != std::string_view::npos) {
        const std::size_t end = text.find_first_not_of(kTokenAlphabet, at);
        if (is_token_like(text.substr(at, end == std::string_view::npos ? end : end - at)))
            return true;
        at = end == std::string_view::npos ? end : text.find_first_of(kTokenAlphabet, end);
    }
    return false;
}

std::string note_name_source(std::string_view content, std::string_view type) {
    if (type_has_secret_values(type)) {
        while (!content.empty()) {
            const std::size_t end = content.find('\n');
            if (const std::string_view key = secret_line_key(content.substr(0, end)); !key.empty())
                return std::string(key);
            content.remove_prefix(end == std::string_view::npos ? content.size() : end + 1);
        }
        return {};
    }
    std::string line = first_non_empty_line(content);
    const std::optional<Assignment> assignment = split_assignment(line);
    if (assignment && is_value_assignment(*assignment))
        return std::string(assignment->key);
    return line;
}

std::string human_size(std::uintmax_t bytes) {
    constexpr double kStep = 1024.0;
    constexpr std::array kUnits = {"KiB", "MiB", "GiB", "TiB"};
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    auto value = static_cast<double>(bytes) / kStep;
    std::size_t unit = 0;
    while (value >= kStep && unit + 1 < kUnits.size()) {
        value /= kStep;
        ++unit;
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

namespace {

std::tm local_tm(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local;
}

std::string format_tm(const std::tm& local, const char* format) {
    std::array<char, 32> text{};
    return std::string(text.data(), std::strftime(text.data(), text.size(), format, &local));
}

}  // namespace

std::string local_time_text(std::filesystem::file_time_type time) {
    return format_tm(local_tm(std::chrono::clock_cast<std::chrono::system_clock>(time)), "%Y-%m-%d %H:%M");
}

std::string short_time_text(std::filesystem::file_time_type time, std::chrono::system_clock::time_point now) {
    const std::tm local = local_tm(std::chrono::clock_cast<std::chrono::system_clock>(time));
    const std::tm today = local_tm(now);
    const bool same_day = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday;
    return format_tm(local, same_day ? "%H:%M" : "%Y-%m-%d");
}

std::chrono::system_clock::time_point next_local_midnight(std::chrono::system_clock::time_point now) {
    std::tm local = local_tm(now);
    ++local.tm_mday;  // mktime ay ve yıl taşmasını düzeltir
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;  // yaz saati geçişinde yerel kural karar versin
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

char32_t to_lower(char32_t cp) {
    if (cp < 0x80)
        return (cp >= U'A' && cp <= U'Z') ? cp + 0x20 : cp;
    if (cp == 0x130)
        return U'i';  // İ
    // Latin-1: À-Þ (× hariç)
    if (cp >= 0xC0 && cp <= 0xDE)
        return cp == 0xD7 ? cp : cp + 0x20;
    // Latin Extended-A: çift/tek çiftler; aralık sınırları Unicode tablosundan.
    if ((cp >= 0x100 && cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177))
        return (cp % 2 == 0) ? cp + 1 : cp;
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
        return (cp % 2 == 1) ? cp + 1 : cp;
    if (cp == 0x178)
        return 0xFF;  // Ÿ
    return cp;
}

}  // namespace noteapp::core
