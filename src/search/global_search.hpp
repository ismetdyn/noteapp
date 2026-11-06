#pragma once

#include "core/limits.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/query.hpp"
#include "search/relation_search.hpp"

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
    // Yalnız ilişkiden gelen sonuçta (ad ve içerik eşleşmesi yok): en güçlü neden.
    std::optional<RelationReason> reason = std::nullopt;
};

// Ad ve içerik birlikte: sorgu argümanları notes'u süzer, kalan metin her terimiyle yolda fuzzy ya da notta
// tam metin (ContentIndex kuralı) aranır. Sıra: güçlü ad eşleşmeleri -> içerik eşleşmeleri -> zayıf ad
// eşleşmeleri; `sort:` verilirse o sıra, eşitlikte bu sıra. Sonuç sayısı limits.search.global_results (0:
// hepsi); sınır güçlü ad eşleşmeleriyle dolacaksa dörtte biri içerik ve zayıf eşleşmelere ayrılır: içerik
// sonuçları kaybolmaz. Ad eşleşmesinin aday tavanı limits.search.fuzzy_candidate_chars. Ad ve içerikte
// bulunmayan notlar en sonda ilişkiyle gelir (relation_hits, graph): `sort:` verilse de doğrudan
// eşleşmelerin altında kalırlar, sınırı ilk onlar bırakır. `rel:`/`linked:` graph.edges'e bağlanır.
// SPEC-search "Genel arama", "İlişki araması". Tarih süzgeçleri şimdiki zamana göredir.
std::expected<std::vector<GlobalHit>, QueryError>
search_all(std::string_view query, std::span<const core::NoteMeta> notes, const ContentIndex& content_index,
           const core::SearchLimits& limits, const ViewLookup& views = {}, const GraphInput& graph = {});

}  // namespace noteapp::search
