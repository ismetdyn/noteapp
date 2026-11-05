#pragma once

#include "core/text.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::cli {

namespace suggest_detail {

// Bozuk UTF-8 bayt bayt sayılır: öneri yalnız ipucudur, girdi reddedilmez.
inline std::u32string code_points(std::string_view text) {
    if (auto decoded = core::decode_utf8(text))
        return std::move(*decoded);
    return {text.begin(), text.end()};
}

// Levenshtein (ekle/sil/değiştir), kod noktası üzerinde; tek satırlık DP.
inline std::size_t distance(const std::u32string& a, const std::u32string& b) {
    std::vector<std::size_t> row(b.size() + 1);
    std::iota(row.begin(), row.end(), std::size_t{0});
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

}  // namespace suggest_detail

// Yazım hatasına en yakın tek aday (SPEC-cli "Hata biçimi"): uzaklık en çok 2 ve sözcük uzunluğunun yarısından
// küçük olmalı (kısa sözcükte her şey "yakın" olmasın); en yakın iki aday eşitse öneri yok (yanlış yönlendirme
// sessiz kalmaktan kötü). Saf fonksiyon.
inline std::optional<std::string_view> suggest(std::string_view word, std::span<const std::string_view> candidates) {
    constexpr std::size_t kMaxDistance = 2;
    const std::u32string typed = suggest_detail::code_points(word);
    std::optional<std::string_view> best;
    std::size_t best_distance = kMaxDistance + 1;
    bool tied = false;
    for (const std::string_view candidate : candidates) {
        const std::size_t d = suggest_detail::distance(typed, suggest_detail::code_points(candidate));
        if (d > kMaxDistance || 2 * d >= typed.size())
            continue;
        if (d < best_distance) {
            best = candidate;
            best_distance = d;
            tied = false;
        } else if (d == best_distance && candidate != *best) {
            tied = true;
        }
    }
    return tied ? std::nullopt : best;
}

}  // namespace noteapp::cli
