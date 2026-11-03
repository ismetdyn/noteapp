#include "gui/browser_rows.hpp"

#include <utility>

namespace noteapp::gui {

namespace {

BrowserRow folder_row(const core::FolderNode& node, std::string name, int depth, bool expanded,
                      std::size_t number) {
    return {BrowserRowKind::Folder, node.path, std::move(name), depth,
            node.item_count() > 0,  expanded,  node.size,       node.updated_at,
            node.created_at,        number};
}

// Listing satırları ağaçtan ve listedeki notlardan kurulur: düğüm ve üst veri vardır.
BrowserRow browser_row(const app::Listing& listing, app::ListRow row, int depth) {
    if (row.is_folder)
        return folder_row(*listing.tree().find(row.path.string()), std::move(row.name), depth, row.open,
                          row.number);
    const core::NoteMeta& meta = *listing.meta_of(row.path);
    return {BrowserRowKind::Note,
            row.path.string(),
            std::move(row.name),
            depth,
            false,
            false,
            meta.size,
            meta.updated_at,
            meta.created_at,
            row.number,
            meta.sync_status,
            meta.origin};
}

std::vector<BrowserRow> browser_rows(const app::Listing& listing, std::vector<app::ListRow> rows,
                                     int depth_offset, std::vector<BrowserRow> out) {
    out.reserve(out.size() + rows.size());
    for (app::ListRow& row : rows) {
        const int depth = static_cast<int>(row.depth) + depth_offset;
        out.push_back(browser_row(listing, std::move(row), depth));
    }
    return out;
}

}  // namespace

std::vector<BrowserRow> tree_rows(const app::Listing& listing) {
    // Kök hep açık: tek satırlık ağaç olmasın.
    std::vector<BrowserRow> root{folder_row(*listing.tree().find(""), {}, 0, true, 0)};
    return browser_rows(listing, listing.tree_rows(""), 1, std::move(root));
}

std::vector<BrowserRow> folder_rows(const app::Listing& listing, const std::string& folder) {
    return browser_rows(listing, listing.folder_rows(folder), 0, {});
}

}  // namespace noteapp::gui
