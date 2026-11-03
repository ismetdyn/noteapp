#pragma once

#include "app/listing.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::gui {

enum class BrowserRowKind { Folder, Note };

// Gezgin panelinin düz satırı: çizim katmanı ağacı yeniden dolaşmaz, kırpıcı (clipper) doğrudan bu diziyi
// okur.
struct BrowserRow {
    BrowserRowKind kind{};
    std::string path;  // klasör ya da not yolu; "" kök
    std::string name;  // son parça; kökte boş
    int depth = 0;
    bool has_children = false;
    bool expanded = false;
    std::uintmax_t size = 0;
    std::optional<std::filesystem::file_time_type> updated_at = std::nullopt;
    std::optional<std::filesystem::file_time_type> created_at = std::nullopt;
    std::size_t number = 0;  // TUI'deki sıra no: klasörünün içinde 1'den; kökte 0 (numarasız)
    // Not: senkron durumu (işçi doldurur) ve kaynak cihaz; klasörde yok.
    std::optional<core::SyncState> sync = std::nullopt;
    std::optional<std::string> origin = std::nullopt;
};

// Satırları ve sıralarını app::Listing kurar; burada yalnız çizim alanları (boyut, tarihler) eklenir.
// Ağaç: GUI'nin kök satırı (hep açık, numarasız), altında Listing::tree_rows bir düzey derinde.
std::vector<BrowserRow> tree_rows(const app::Listing& listing);
// Ayrıntılar: yalnız `folder`ın doğrudan çocukları (Listing::folder_rows).
std::vector<BrowserRow> folder_rows(const app::Listing& listing, const std::string& folder);

}  // namespace noteapp::gui
