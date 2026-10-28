#include "search/fold.hpp"

#include "core/text.hpp"

namespace noteapp::search {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Tek kod noktası çözer; geçersizse U+FFFD ve 1 bayt ilerler (çökmez, ayrıştırma sonraki baytla sürer).
// core::decode_utf8 tüm girdiyi ya hep ya hiç çözdüğü için burada ayrı, toleranslı bir çözücü gerekir.
struct Decoded {
    char32_t cp;
    std::size_t len;
};

Decoded decode_one(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80)
        return {b0, 1};

    std::size_t len = 0;
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        cp = b0 & 0x0F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        cp = b0 & 0x07;
    } else {
        return {kReplacement, 1};
    }
    if (i + len > s.size())
        return {kReplacement, 1};
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80)
            return {kReplacement, 1};
        cp = (cp << 6) | (b & 0x3F);
    }
    const bool overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (overlong || surrogate || cp > 0x10FFFF)
        return {kReplacement, 1};
    return {cp, len};
}

// ı ve Türkçe aksanlı harflerin ASCII karşılığı; küçük harf girdisi bekler.
char32_t strip_turkish(char32_t cp) {
    switch (cp) {
    case U'ı':
        return U'i';
    case U'ç':
        return U'c';
    case U'ğ':
        return U'g';
    case U'ö':
        return U'o';
    case U'ş':
        return U's';
    case U'ü':
        return U'u';
    default:
        return cp;
    }
}

}  // namespace

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

Folded fold(std::string_view utf8) {
    Folded out;
    out.original.reserve(utf8.size());
    out.text.reserve(utf8.size());
    out.byte_offsets.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto [cp, len] = decode_one(utf8, i);
        out.original.push_back(cp);
        out.text.push_back(strip_turkish(to_lower(cp)));
        out.byte_offsets.push_back(i);
        i += len;
    }
    return out;
}

std::string fold_to_utf8(std::string_view utf8) {
    return core::encode_utf8(fold(utf8).text);
}

}  // namespace noteapp::search
