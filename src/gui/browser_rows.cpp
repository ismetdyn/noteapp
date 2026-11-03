#include "gui/browser_rows.hpp"

#include "search/fold.hpp"
#include "search/folder_order.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr search::SortSpec kByName{search::SortField::Name, false};

BrowserRow folder_row(const core::FolderNode& node, int depth, bool expanded, std::size_t number) {
    return {BrowserRowKind::Folder,
            node.path,
            core::last_component(node.path),
            depth,
            node.item_count() > 0,
            expanded,
            node.size,
            node.updated_at,
            node.created_at,
            number};
}

BrowserRow note_row(const core::NoteMeta& meta, int depth, std::size_t number) {
    std::string path = meta.path.string();
    std::string name = core::last_component(path);
    return {BrowserRowKind::Note, std::move(path), std::move(name), depth, false, false, meta.size,
            meta.updated_at,      meta.created_at, number};
}

// Notlar klasörlerle aynı ölçütle: katlanmış ad (büyük/küçük harf ve Türkçe harf farkı sırayı bozmasın).
std::vector<std::size_t> notes_by_name(const core::FolderNode& node, std::span<const core::NoteMeta> metas) {
    // Anahtar not başına bir kez katlanır: karşılaştırmada katlamak 5.000 notta sıralamayı katlıyordu.
    std::vector<std::pair<std::u32string, std::size_t>> keyed;
    keyed.reserve(node.notes.size());
    for (const std::size_t index : node.notes)
        keyed.emplace_back(search::fold(core::last_component(metas[index].path.string())).text, index);
    std::ranges::stable_sort(keyed, {}, &std::pair<std::u32string, std::size_t>::first);
    std::vector<std::size_t> notes;
    notes.reserve(keyed.size());
    for (const auto& [key, index] : keyed)
        notes.push_back(index);
    return notes;
}

void append_children(const core::FolderTree& tree, const core::FolderNode& node,
                     std::span<const core::NoteMeta> metas, const std::set<std::string>* expanded, int depth,
                     std::vector<BrowserRow>& rows) {
    std::size_t number = 0;  // TUI kuralı: klasörler ve notlar birlikte, her klasörde yeniden 1'den
    for (const core::FolderNode* folder : search::sorted_subfolders(tree, node, kByName)) {
        const bool open = expanded != nullptr && expanded->contains(folder->path);
        rows.push_back(folder_row(*folder, depth, open, ++number));
        if (open)
            append_children(tree, *folder, metas, expanded, depth + 1, rows);
    }
    for (const std::size_t index : notes_by_name(node, metas))
        rows.push_back(note_row(metas[index], depth, ++number));
}

}  // namespace

std::vector<BrowserRow> tree_rows(const core::FolderTree& tree, std::span<const core::NoteMeta> metas,
                                  const std::set<std::string>& expanded) {
    const core::FolderNode* root = tree.find("");
    std::vector<BrowserRow> rows{folder_row(*root, 0, true, 0)};  // kök hep açık: tek satırlık ağaç olmasın
    append_children(tree, *root, metas, &expanded, 1, rows);
    return rows;
}

std::vector<BrowserRow> folder_rows(const core::FolderTree& tree, std::span<const core::NoteMeta> metas,
                                    const std::string& folder) {
    std::vector<BrowserRow> rows;
    if (const core::FolderNode* node = tree.find(folder))
        append_children(tree, *node, metas, nullptr, 0, rows);
    return rows;
}

}  // namespace noteapp::gui
