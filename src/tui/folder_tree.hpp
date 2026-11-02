#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/query.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Klasörün üst verisi. Sayaçlar alt ağacı kapsar; `subfolders` ve `notes` yalnız doğrudan çocuklardır.
struct FolderNode {
    std::string path;                     // "" kök
    std::vector<std::string> subfolders;  // tam yol
    std::vector<std::size_t> notes;       // metas dizini
    std::size_t note_count = 0;
    std::size_t folder_count = 0;
    std::uintmax_t size = 0;
    std::optional<std::filesystem::file_time_type> updated_at = std::nullopt;
    std::optional<std::filesystem::file_time_type> created_at = std::nullopt;
    std::vector<std::size_t> recent = {};  // alt ağaçta en yeni notlar, en yeni başta

    std::size_t item_count() const { return subfolders.size() + notes.size(); }
};

// Bellekteki not listesinden klasör ağacı: gezinme depoya gitmez. Yalnız liste değişince yeniden kurulur.
class FolderTree {
public:
    static constexpr std::size_t kRecentNotes = 5;

    FolderTree();
    // `folders` boş klasörleri de taşır; NotePath'ten geçemeyen ad atlanır (satırı kurulamazdı).
    void rebuild(std::span<const core::NoteMeta> metas, std::span<const std::string> folders);

    const FolderNode* find(std::string_view folder) const;
    // Klasör senkronla ya da silmeyle gitmiş olabilir: kendisi ya da duran en yakın atası.
    std::string nearest_existing(std::string folder) const;
    // Alt ağaçtaki notlar (derinden yüzeye değil, sırasız) ve klasörün kendisi dahil tüm klasörler.
    std::vector<std::size_t> notes_under(std::string_view folder) const;
    std::vector<std::string> folders_under(std::string_view folder) const;
    // Klasörler kendi grubunda `sort`a göre; eşitlikte ad sırası.
    std::vector<const FolderNode*> sorted_subfolders(const FolderNode& node,
                                                     const search::SortSpec& sort) const;

private:
    FolderNode& ensure(std::string_view folder);
    void add_note(std::span<const core::NoteMeta> metas, std::size_t index);

    std::map<std::string, FolderNode, std::less<>> nodes_;
};

std::string folder_of(const core::NotePath& path);
std::string parent_folder(std::string_view folder);
std::string last_component(std::string_view path);
// `base` altındaki adı tam yola çevirir; kökte adın kendisidir.
std::string join_folder(std::string_view base, std::string_view name);
// `path`, `folder`ın alt ağacında mı (kök her yolu kapsar).
bool in_folder(std::string_view path, std::string_view folder);

}  // namespace noteapp::tui
