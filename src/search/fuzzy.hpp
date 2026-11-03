#pragma once

#include "core/note_store.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace noteapp::search {

struct FuzzyMatch {
    double score{};
    std::vector<std::size_t>
        positions;  // sorgu karakteri başına, adayın özgün UTF-8'indeki bayt ofseti (vurgulama için)
};

// limits.search.fuzzy_candidate_chars varsayılanı: config'i olmayan çağıran (app::Listing'in kurulumu)
// içindir; defaults.toml ile aynı olduğunu bir test denetler.
inline constexpr std::size_t kDefaultFuzzyCandidateChars = 1024;

// fzy yaklaşımı: sorgu aday içinde (katlanmış) alt dizi olmalı, yoksa nullopt.
// Boş sorgu: puan 0, konum yok. `max_candidate` kod noktasından uzun aday
// (limits.search.fuzzy_candidate_chars, sınırsız SIZE_MAX): düşük pozitif puan, konum yok. Bellek sorgu x
// aday x 16 bayttır.
std::optional<FuzzyMatch> fuzzy_match(std::string_view query, std::string_view candidate,
                                      std::size_t max_candidate);

struct Ranked {
    std::size_t index{};  // notes içindeki sıra
    FuzzyMatch match;
};

// Aday = göreli yol + uzantı. Sıra: puan azalan -> kısa yol -> mtime azalan. Boş sorgu: tümü, mtime azalan.
std::vector<Ranked> rank_notes(std::string_view query, std::span<const core::NoteInfo> notes,
                               std::size_t max_candidate);

}  // namespace noteapp::search
