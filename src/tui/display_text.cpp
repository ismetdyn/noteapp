#include "tui/display_text.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <format>

namespace noteapp::tui {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

std::size_t sequence_length(unsigned char lead) {
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return lead >= 0xC0 ? 2 : 1;
}

bool is_terminal_control(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

}  // namespace

std::string terminal_safe(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size();) {
        const std::size_t length = sequence_length(static_cast<unsigned char>(raw[at]));
        const std::string_view sequence = raw.substr(at, length);
        const auto decoded = core::decode_utf8(sequence);
        if (!decoded || decoded->size() != 1) {
            out += kReplacement;
            ++at;  // bozuk bayt tek tek: ardından gelen geçerli metin kaybolmasın
            continue;
        }
        // Sağdan sola geçersiz kılma ve benzerleri dosya adının görünen sırasını değiştirip uzantıyı
        // gizleyebilir; sıfır genişlikliler iki adı aynı gösterir.
        const bool unsafe =
            is_terminal_control(decoded->front()) || core::find_hidden_control(decoded->front()) != nullptr;
        if (unsafe)
            out += kReplacement;
        else
            out += sequence;
        at += sequence.size();
    }
    return out;
}

std::string count_text(std::string_view key, std::size_t count) {
    const std::string full = std::string(key) + (count == 1 ? "_one" : "");
    return core::tr(full, {{"count", std::to_string(count)}});
}

}  // namespace noteapp::tui
