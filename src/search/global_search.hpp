#pragma once

#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/query.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace noteapp::search {

// Not başına tek sonuç. Sorgu metni yoksa (yalnız argüman) ikisi de false'tur.
struct GlobalHit {
    std::size_t index{};  // notes içindeki sıra
    core::NotePath path;
    bool name_matched = false;
    bool content_matched = false;
    std::vector<std::size_t> name_positions = {};  // yolun UTF-8'inde bayt ofsetleri, artan
    std::size_t content_hits = 0;                  // eşleşen satır sayısı
    // En çok farklı terim, sonra en çok geçiş, sonra en erken satır.
    std::optional<Hit> best_line = std::nullopt;
};

// Yeterince geniş varsayılan: sonuç listesi tek ekrandan büyük, kesitler yalnız dönen sonuçlar için üretilir.
inline constexpr std::size_t kGlobalSearchLimit = 200;

// Ad ve içerik birlikte: sorgu argümanları notes'u süzer, kalan metin her terimiyle yolda fuzzy ya da notta
// tam metin (ContentIndex kuralı) aranır. Sıra: güçlü ad eşleşmeleri -> içerik eşleşmeleri -> zayıf ad
// eşleşmeleri; `sort:` verilirse o sıra, eşitlikte bu sıra. limit güçlü ad eşleşmeleriyle dolacaksa limit/4'ü
// içerik ve zayıf eşleşmelere ayrılır: içerik sonuçları kaybolmaz. SPEC-search "Genel arama".
std::expected<std::vector<GlobalHit>, QueryError>
search_all(std::string_view query, std::span<const core::NoteMeta> notes, const ContentIndex& content_index,
           std::size_t limit = kGlobalSearchLimit,
           std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
           const ViewLookup& views = {});

}  // namespace noteapp::search
