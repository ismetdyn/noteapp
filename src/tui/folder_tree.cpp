#include "tui/folder_tree.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <iterator>
#include <numeric>
#include <utility>

namespace noteapp::tui {

namespace {

std::u32string name_key(const FolderNode& node) {
    return search::fold(last_component(node.path)).text;
}

template <typename Key> void sort_by(std::vector<const FolderNode*>& nodes, bool descending, const Key& key) {
    std::ranges::stable_sort(nodes, [&](const FolderNode* a, const FolderNode* b) {
        return descending ? key(*b) < key(*a) : key(*a) < key(*b);
    });
}

}  // namespace

std::string folder_of(const core::NotePath& path) {
    return parent_folder(path.string());
}

std::string parent_folder(std::string_view folder) {
    const std::size_t slash = folder.rfind('/');
    return slash == std::string_view::npos ? std::string{} : std::string(folder.substr(0, slash));
}

std::string last_component(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string join_folder(std::string_view base, std::string_view name) {
    return base.empty() ? std::string(name) : std::string(base) + "/" + std::string(name);
}

bool in_folder(std::string_view path, std::string_view folder) {
    return folder.empty() ||
           (path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/');
}

FolderTree::FolderTree() {
    ensure("");
}

FolderNode& FolderTree::ensure(std::string_view folder) {
    if (const auto found = nodes_.find(folder); found != nodes_.end())
        return found->second;
    auto [inserted, added] = nodes_.emplace(std::string(folder), FolderNode{std::string(folder), {}, {}});
    if (!folder.empty()) {
        // Ata önce kurulur; std::map düğümleri ekleme sonrası yerinde kalır.
        const std::string parent = parent_folder(folder);
        ensure(parent).subfolders.emplace_back(folder);
        for (std::string walk = parent;; walk = parent_folder(walk)) {
            ++ensure(walk).folder_count;
            if (walk.empty())
                break;
        }
    }
    return inserted->second;
}

void FolderTree::add_note(std::span<const core::NoteMeta> metas, std::size_t index) {
    const core::NoteMeta& meta = metas[index];
    const std::string direct = folder_of(meta.path);
    ensure(direct).notes.push_back(index);
    for (std::string walk = direct;; walk = parent_folder(walk)) {
        FolderNode& node = ensure(walk);
        ++node.note_count;
        node.size += meta.size;
        node.updated_at = std::max(node.updated_at.value_or(meta.updated_at), meta.updated_at);
        if (meta.created_at)
            node.created_at = std::max(node.created_at.value_or(*meta.created_at), *meta.created_at);
        if (node.recent.size() < kRecentNotes)
            node.recent.push_back(index);
        if (walk.empty())
            break;
    }
}

void FolderTree::rebuild(std::span<const core::NoteMeta> metas, std::span<const std::string> folders) {
    nodes_.clear();
    ensure("");
    for (const std::string& folder : folders)
        if (!folder.empty() && core::NotePath::parse(folder))
            ensure(folder);
    // En yeni başta geçilir: düğümdeki `recent` böylece ek sıralama istemez.
    std::vector<std::size_t> newest(metas.size());
    std::iota(newest.begin(), newest.end(), std::size_t{0});
    std::ranges::stable_sort(
        newest, [&](std::size_t a, std::size_t b) { return metas[a].updated_at > metas[b].updated_at; });
    for (const std::size_t index : newest)
        add_note(metas, index);
}

const FolderNode* FolderTree::find(std::string_view folder) const {
    const auto found = nodes_.find(folder);
    return found == nodes_.end() ? nullptr : &found->second;
}

std::string FolderTree::nearest_existing(std::string folder) const {
    while (!find(folder))
        folder = parent_folder(folder);
    return folder;
}

std::vector<std::size_t> FolderTree::notes_under(std::string_view folder) const {
    std::vector<std::size_t> notes;
    for (const std::string& path : folders_under(folder))
        std::ranges::copy(nodes_.find(path)->second.notes, std::back_inserter(notes));
    return notes;
}

std::vector<std::string> FolderTree::folders_under(std::string_view folder) const {
    std::vector<std::string> folders;
    if (!find(folder))
        return folders;
    folders.emplace_back(folder);
    for (std::size_t i = 0; i < folders.size(); ++i) {
        const std::vector<std::string>& children = nodes_.find(folders[i])->second.subfolders;
        folders.insert(folders.end(), children.begin(), children.end());
    }
    return folders;
}

std::vector<const FolderNode*> FolderTree::sorted_subfolders(const FolderNode& node,
                                                             const search::SortSpec& sort) const {
    std::vector<const FolderNode*> nodes;
    nodes.reserve(node.subfolders.size());
    for (const std::string& path : node.subfolders)
        nodes.push_back(find(path));
    // Eşitlikte ad sırası: önce adla, sonra kararlı sıralamayla alan.
    sort_by(nodes, sort.field == search::SortField::Name && sort.descending, name_key);
    switch (sort.field) {
    case search::SortField::Name:
        break;
    case search::SortField::Size:
        sort_by(nodes, sort.descending, [](const FolderNode& n) { return n.size; });
        break;
    case search::SortField::Updated:
        sort_by(nodes, sort.descending, [](const FolderNode& n) { return n.updated_at; });
        break;
    case search::SortField::Created:
        sort_by(nodes, sort.descending, [](const FolderNode& n) { return n.created_at; });
        break;
    }
    return nodes;
}

}  // namespace noteapp::tui
