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

// fzy yaklaşımı: sorgu aday içinde (katlanmış) alt dizi olmalı, yoksa nullopt.
// Boş sorgu: puan 0, konum yok. Çok uzun aday: düşük pozitif puan, konum yok.
std::optional<FuzzyMatch> fuzzy_match(std::string_view query, std::string_view candidate);

struct Ranked {
    std::size_t index{};  // notes içindeki sıra
    FuzzyMatch match;
};

// Aday = göreli yol + uzantı. Sıra: puan azalan -> kısa yol -> mtime azalan. Boş sorgu: tümü, mtime azalan.
std::vector<Ranked> rank_notes(std::string_view query, std::span<const core::NoteInfo> notes);

}  // namespace noteapp::search
