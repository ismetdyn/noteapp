#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::cli {

// `ranges`, `text` içindeki [başlangıç, bitiş) bayt aralıklarıdır (search::Hit::ranges). Terminalde vurgu
// için aralıkları kalın + sarı ANSI koduna sarar. Bozuk aralık (taşan, boş, öncekiyle çakışan) çıktıyı
// bozmasın diye kırpılır/atlanır.
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
        out.append(text.substr(cursor, begin - cursor));
        out.append(kOn).append(text.substr(begin, end - begin)).append(kOff);
        cursor = end;
    }
    out.append(text.substr(cursor));
    return out;
}

}  // namespace noteapp::cli
