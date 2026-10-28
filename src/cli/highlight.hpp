#pragma once

#include "core/text.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::cli {

// Terminale (TTY) yazılacak not baytları: C0 denetim karakterleri (\t ve \n dışında), DEL, C1 (U+0080–U+009F,
// UTF-8 kodlu) ve geçersiz UTF-8 baytları görünür U+FFFD olur, \r atılır. Neden: VT işleme açıkken senkronla
// gelen bir not OSC 52 (panoya yazma) ya da başlık değiştiren kaçış dizisi yollayabilirdi. Boruya/dosyaya
// giden çıktı buradan geçmez, bayt bayt kalır. Tek geçiş (doğrusal).
inline std::string sanitize_for_terminal(std::string_view text) {
    constexpr std::string_view kPlaceholder = "\xEF\xBF\xBD";  // U+FFFD
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0xC0 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::string_view sequence = text.substr(i, length);
        const bool valid_multibyte = length > 1 && core::is_valid_utf8(sequence);
        const bool c1 = valid_multibyte && lead == 0xC2 && static_cast<unsigned char>(sequence[1]) < 0xA0;
        if (lead == '\r') {
            // CRLF'li not tek satır sonuyla görünür
        } else if (lead == '\t' || lead == '\n' || (lead >= 0x20 && lead < 0x7F)) {
            out += static_cast<char>(lead);
        } else if (valid_multibyte && !c1) {
            out.append(sequence);
        } else {
            out.append(kPlaceholder);
            if (!valid_multibyte)
                length = 1;  // geçersiz dizide yalnız ilk bayt tüketilir, kalanı ayrıca denetlenir
        }
        i += length;
    }
    return out;
}

// `ranges`, `text` içindeki [başlangıç, bitiş) bayt aralıklarıdır (search::Hit::ranges). Terminalde vurgu
// için aralıkları kalın + sarı ANSI koduna sarar; her parça sanitize_for_terminal'dan geçer (yalnız TTY'de
// çağrılır). Bozuk aralık (taşan, boş, öncekiyle çakışan) çıktıyı bozmasın diye kırpılır/atlanır.
inline std::string highlight_ranges(std::string_view text,
                                    const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
    constexpr std::string_view kOn = "\x1b[1;33m";
    constexpr std::string_view kOff = "\x1b[0m";
    std::string out;
    std::size_t cursor = 0;
    for (const auto& [begin, end_raw] : ranges) {
        const std::size_t end = std::min(end_raw, text.size());
        if (begin < cursor || begin >= end)
            continue;
        out.append(sanitize_for_terminal(text.substr(cursor, begin - cursor)));
        out.append(kOn).append(sanitize_for_terminal(text.substr(begin, end - begin))).append(kOff);
        cursor = end;
    }
    out.append(sanitize_for_terminal(text.substr(cursor)));
    return out;
}

}  // namespace noteapp::cli
