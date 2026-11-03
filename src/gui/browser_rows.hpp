#pragma once

#include "core/folder_tree.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
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
};

// Ağaç: kök, ardından açık klasörlerin çocukları; her düzeyde önce klasörler, sonra notlar, adla sıralı.
std::vector<BrowserRow> tree_rows(const core::FolderTree& tree, std::span<const core::NoteMeta> metas,
                                  const std::set<std::string>& expanded);
// Ayrıntılar: yalnız `folder`ın doğrudan çocukları, aynı sırayla.
std::vector<BrowserRow> folder_rows(const core::FolderTree& tree, std::span<const core::NoteMeta> metas,
                                    const std::string& folder);

}  // namespace noteapp::gui
