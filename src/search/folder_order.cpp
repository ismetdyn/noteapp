#include "search/folder_order.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <string>

namespace noteapp::search {

namespace {

std::u32string name_key(const core::FolderNode& node) {
    return fold(core::last_component(node.path)).text;
}

template <typename Key>
void sort_by(std::vector<const core::FolderNode*>& nodes, bool descending, const Key& key) {
    std::ranges::stable_sort(nodes, [&](const core::FolderNode* a, const core::FolderNode* b) {
        return descending ? key(*b) < key(*a) : key(*a) < key(*b);
    });
}

}  // namespace

std::vector<const core::FolderNode*> sorted_subfolders(const core::FolderTree& tree,
                                                       const core::FolderNode& node, const SortSpec& sort) {
    std::vector<const core::FolderNode*> nodes;
    nodes.reserve(node.subfolders.size());
    for (const std::string& path : node.subfolders)
        nodes.push_back(tree.find(path));
    // Eşitlikte ad sırası: önce adla, sonra kararlı sıralamayla alan.
    sort_by(nodes, sort.field == SortField::Name && sort.descending, name_key);
    switch (sort.field) {
    case SortField::Name:
        break;
    case SortField::Size:
        sort_by(nodes, sort.descending, [](const core::FolderNode& n) { return n.size; });
        break;
    case SortField::Updated:
        sort_by(nodes, sort.descending, [](const core::FolderNode& n) { return n.updated_at; });
        break;
    case SortField::Created:
        sort_by(nodes, sort.descending, [](const core::FolderNode& n) { return n.created_at; });
        break;
    }
    return nodes;
}

}  // namespace noteapp::search
