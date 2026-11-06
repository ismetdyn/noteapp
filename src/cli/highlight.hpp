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
// gelen bir not OSC 52 (panoya yazma) ya da başlık değiştiren kaçış dizisi yollayabilirdi. Görünmez bidi ve
// sıfır genişlikli karakterler (core::find_hidden_control, TUI/GUI ile aynı liste) de U+FFFD olur: metin
// saklandığından farklı sırayla görünmesin (Trojan Source). Boruya/dosyaya giden çıktı buradan geçmez, bayt
// bayt kalır. Tek geçiş (doğrusal).
// Satır sonu ve sekme boşluk olur: TAB ayraçlı tek satırlık alan (`find -l` nedeni) bozulmasın. Kaynak
// zaten temizlenmiştir (çekirdek adları okurken arındırır); bu ikinci kemerdir.
inline std::string single_line(std::string_view text) {
    std::string out(text);
    std::ranges::replace_if(out, [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    return out;
}

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
        const bool hidden =
            valid_multibyte && core::find_hidden_control(core::decode_utf8(sequence)->front());
        if (lead == '\r') {
            // CRLF'li not tek satır sonuyla görünür
        } else if (lead == '\t' || lead == '\n' || (lead >= 0x20 && lead < 0x7F)) {
            out += static_cast<char>(lead);
        } else if (valid_multibyte && !c1 && !hidden) {
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

// Kabuk tamamlamasının adayı (`note __complete`): kabuk satır başına bir aday okur. Satır sonu, sekme, \r ya da
// terminalde temizlenecek bir şey (denetim karakteri, C1, bozuk UTF-8, gizli bidi) taşıyan ad doğru
// tamamlanamaz ve menüde kaçış dizisi olarak çalışabilir: böyle aday hiç yazılmaz.
inline bool is_completion_safe(std::string_view text) {
    return !text.empty() && text.find_first_of("\t\n") == std::string_view::npos &&
           sanitize_for_terminal(text) == text;
}

// `ranges`, `text` içindeki [başlangıç, bitiş) bayt aralıklarıdır (search::Hit::ranges). Terminalde vurgu
// için aralıkları kalın + sarı ANSI koduna sarar; her parça sanitize_for_terminal'dan geçer (yalnız renk
// açıkken çağrılır: term.hpp color_enabled). Bozuk aralık (taşan, boş, öncekiyle çakışan) çıktıyı bozmasın diye kırpılır/atlanır.
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
