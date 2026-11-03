#pragma once

#include "core/folder_tree.hpp"
#include "search/query.hpp"

#include <vector>

namespace noteapp::search {

// Klasörler kendi grubunda `sort`a göre; eşitlikte ad sırası.
std::vector<const core::FolderNode*> sorted_subfolders(const core::FolderTree& tree,
                                                       const core::FolderNode& node, const SortSpec& sort);

}  // namespace noteapp::search
