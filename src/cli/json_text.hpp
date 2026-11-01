#pragma once

// `note query --json` ve `note graph export` aynı kodlamayı kullanır.
#include "core/text.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace noteapp::cli {

inline constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";  // U+FFFD

inline std::string base64_of(std::string_view data) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t left = data.size() - i;
        const auto byte = [&](std::size_t at) {
            return at < left ? static_cast<unsigned char>(data[i + at]) : 0U;
        };
        const unsigned group = (byte(0) << 16U) | (byte(1) << 8U) | byte(2);
        out += kAlphabet[(group >> 18U) & 0x3FU];
        out += kAlphabet[(group >> 12U) & 0x3FU];
        out += left > 1 ? kAlphabet[(group >> 6U) & 0x3FU] : '=';
        out += left > 2 ? kAlphabet[group & 0x3FU] : '=';
    }
    return out;
}

inline void append_json_control(std::string& out, unsigned value) {
    std::array<char, 7> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "\\u%04x", value);
    out += buffer.data();
}

inline std::size_t sequence_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead < 0xE0)
        return 2;
    return lead < 0xF0 ? 3 : 4;
}

// Metin normalde geçerli UTF-8'dir; parametreyle gelen geçersiz bayt JSON'u bozmasın.
inline void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead == '"' || lead == '\\') {
            out += '\\';
            out += text[i++];
        } else if (lead == '\n') {
            out += "\\n";
            ++i;
        } else if (lead == '\t') {
            out += "\\t";
            ++i;
        } else if (lead == '\r') {
            out += "\\r";
            ++i;
        } else if (lead < 0x20 || lead == 0x7F) {
            append_json_control(out, lead);
            ++i;
        } else {
            const std::size_t length = sequence_length(lead);
            const std::string_view sequence = text.substr(i, length);
            if (!core::is_valid_utf8(sequence)) {
                out += kReplacementCharacter;
                ++i;
            } else if (lead == 0xC2 && static_cast<unsigned char>(sequence[1]) < 0xA0) {
                // C1 denetim karakterleri: bazı terminaller kaçış dizisi sayar.
                append_json_control(out, static_cast<unsigned char>(sequence[1]));
                i += length;
            } else {
                out += sequence;
                i += length;
            }
        }
    }
    out += '"';
}

}  // namespace noteapp::cli
