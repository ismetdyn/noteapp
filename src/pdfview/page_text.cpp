#include "pdfview/page_text.hpp"

namespace noteapp::pdfview::detail {

std::optional<char32_t> decode_one(std::string_view text, std::size_t& length) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(0);
    length = 1;
    if (lead < 0x80)
        return lead;
    std::size_t count = 0;
    char32_t cp = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        count = 1;
        cp = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        count = 2;
        cp = lead & 0x0FU;
        low = lead == 0xE0 ? 0xA0 : 0x80;   // fazla uzun
        high = lead == 0xED ? 0x9F : 0xBF;  // vekil (U+D800–DFFF)
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        count = 3;
        cp = lead & 0x07U;
        low = lead == 0xF0 ? 0x90 : 0x80;   // fazla uzun
        high = lead == 0xF4 ? 0x8F : 0xBF;  // > U+10FFFF
    } else {
        return std::nullopt;  // devam baytı, C0/C1 (fazla uzun 2 bayt), F5+
    }
    if (text.size() <= count)
        return std::nullopt;
    for (std::size_t i = 1; i <= count; ++i) {
        const unsigned char next = byte(i);
        if (next < (i == 1 ? low : 0x80) || next > (i == 1 ? high : 0xBF))
            return std::nullopt;
        cp = (cp << 6U) | (next & 0x3FU);
    }
    length = count + 1;
    return cp;
}

std::string clean_text(std::string_view raw) {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size();) {
        std::size_t length = 1;
        const std::optional<char32_t> cp = decode_one(raw.substr(at), length);
        const bool control = cp && ((*cp < 0x20 && *cp != '\n' && *cp != '\t') || (*cp >= 0x7F && *cp < 0xA0));
        if (!cp || control)
            out += kReplacement;
        else
            out += raw.substr(at, length);
        at += length;
    }
    return out;
}

}  // namespace noteapp::pdfview::detail
