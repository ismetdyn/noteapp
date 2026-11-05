#include "app/listing.hpp"

#include "core/note_views.hpp"
#include "search/folder_order.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace noteapp::app {

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.revision == y.revision && x.mtime == y.mtime && x.size == y.size;
    });
}

// Okunamayan görünüm listesi notları gizlemez: liste boş kalır, `view:` hatayı kendisi söyler.
std::vector<std::string> view_names(const core::NoteStore& store) {
    std::vector<std::string> names;
    if (const auto views = core::list_views(store))
        for (const core::SavedView& view : *views)
            names.push_back(view.name);
    return names;
}

StoreScan scan_store(const core::NoteStore& store) {
    const auto trash = store.list_trash();
    return StoreScan{store.list(), trash ? trash->size() : 0, store.folders(), store.data_version(),
                     view_names(store)};
}

bool same_scan(const StoreScan& a, const StoreScan& b) {
    return same_listing(a.notes, b.notes) && a.trash == b.trash && a.folders == b.folders &&
           a.graph_version == b.graph_version;
}

ContentSearcher content_searcher(std::function<const search::ContentIndex&()> index, core::Limit results) {
    return [index = std::move(index), cap = results.cap()](std::string_view query) {
        return index().search(query, cap);
    };
}

bool Listing::set_notes(std::vector<core::NoteInfo> notes) {
    if (same_listing(notes_, notes))
        return false;
    notes_ = std::move(notes);
    rebuild_metas();
    return true;
}

bool Listing::set_folders(std::vector<std::string> folders) {
    if (folders == folders_)
        return false;
    folders_ = std::move(folders);
    tree_.rebuild(metas_, folders_);
    return true;
}

bool Listing::set_sync_states(std::map<core::NotePath, core::SyncState> states) {
    if (states == sync_states_)
        return false;
    sync_states_ = std::move(states);
    rebuild_metas();
    return true;
}

void Listing::rebuild_metas() {
    metas_.clear();
    meta_index_.clear();
    metas_.reserve(notes_.size());
    for (const core::NoteInfo& note : notes_) {
        core::NoteMeta meta = core::meta_of(note);
        if (const auto found = sync_states_.find(note.path); found != sync_states_.end())
            meta.sync_status = found->second;
        meta_index_.emplace(note.path, metas_.size());
        metas_.push_back(std::move(meta));
    }
    tree_.rebuild(metas_, folders_);
}

const core::NoteMeta* Listing::meta_of(const core::NotePath& path) const {
    const auto found = meta_index_.find(path);
    return found == meta_index_.end() ? nullptr : &metas_[found->second];
}

const core::NoteInfo* Listing::note_of(const core::NotePath& path) const {
    const auto found = meta_index_.find(path);  // metas_ notes_ ile aynı sırada
    return found == meta_index_.end() ? nullptr : &notes_[found->second];
}

void Listing::set_open(const std::string& folder, bool open) {
    if (open)
        open_.insert(folder);
    else
        open_.erase(folder);
}

void Listing::prune_open_folders() {
    std::erase_if(open_, [this](const std::string& folder) { return tree_.find(folder) == nullptr; });
}

// Numara her klasörde yeniden 1'den başlar: klasörler ve notlar birlikte sayılır (TUI kuralı, GUI aynı).
void Listing::append_children(const core::FolderNode& node, std::size_t depth, bool recurse,
                              std::vector<ListRow>& rows) const {
    std::size_t number = 0;
    for (const core::FolderNode* folder : search::sorted_subfolders(tree_, node, sort_)) {
        const bool open = recurse && is_open(folder->path);
        // Ağaçtaki klasör adı NotePath'ten geçmiştir (FolderTree::rebuild geçemeyeni atlar).
        rows.push_back({.path = core::NotePath::parse(folder->path).value(),
                        .is_folder = true,
                        .name = core::last_component(folder->path),
                        .depth = depth,
                        .number = ++number,
                        .has_children = folder->item_count() > 0,
                        .open = open});
        if (open)
            append_children(*folder, depth + 1, true, rows);
    }
    std::vector<std::size_t> notes = node.notes;
    search::sort_notes(notes, sort_, metas_);
    for (const std::size_t index : notes)
        rows.push_back({.path = metas_[index].path,
                        .name = core::last_component(metas_[index].path.string()),
                        .depth = depth,
                        .number = ++number});
}

std::vector<ListRow> Listing::tree_rows(std::string_view folder) const {
    std::vector<ListRow> rows;
    if (const core::FolderNode* node = tree_.find(folder))
        append_children(*node, 0, true, rows);
    return rows;
}

std::vector<ListRow> Listing::folder_rows(std::string_view folder) const {
    std::vector<ListRow> rows;
    if (const core::FolderNode* node = tree_.find(folder))
        append_children(*node, 0, false, rows);
    return rows;
}

// Bozuk değer metin sayılmaz (TUI kuralı); satırlar son geçerli sorguyla bu listeden yeniden kurulur (GUI
// kuralı): kapsam ya da notlar değiştiyse eski satırlar gitmiş notu göstermesin.
SearchResult Listing::search(std::string_view query, SearchMode mode, std::string_view scope) {
    SearchResult result;
    if (auto parsed = search::parse_query(query, views_)) {
        auto rows = rows_for(*parsed, query, mode, scope);
        if (rows) {
            last_valid_query_ = query;
            result.rows = std::move(*rows);
            return result;
        }
        result.error = std::move(rows.error());
    } else {
        result.error = std::move(parsed.error());
    }
    if (auto parsed = search::parse_query(last_valid_query_, views_))
        if (auto rows = rows_for(*parsed, last_valid_query_, mode, scope))
            result.rows = std::move(*rows);
    return result;
}

std::expected<std::vector<ListRow>, search::QueryError> Listing::rows_for(const search::ParsedQuery& parsed,
                                                                          std::string_view query,
                                                                          SearchMode mode,
                                                                          std::string_view scope) const {
    // Kapsam: süzgeç ve arama yalnız klasörün alt ağacını görür. Kök kapsamında kopya yok.
    // ponytail: alt klasör kapsamı her aramada üst veriyi kopyalar; GUI'nin eski kapsam önbelleği gibi bir
    // önbellek, R3'te ölçüm gerekirse eklenir.
    std::vector<core::NoteMeta> scoped_metas;
    std::vector<std::size_t> scoped_index;  // scoped_metas -> metas_
    std::span<const core::NoteMeta> metas = metas_;
    if (!scope.empty()) {
        for (std::size_t i = 0; i < metas_.size(); ++i)
            if (core::in_folder(metas_[i].path.string(), scope)) {
                scoped_metas.push_back(metas_[i]);
                scoped_index.push_back(i);
            }
        metas = scoped_metas;
    }
    const auto scoped_note = [&](std::size_t i) -> const core::NoteInfo& {
        return notes_[scope.empty() ? i : scoped_index[i]];
    };

    std::vector<ListRow> rows;
    if (parsed.text.empty()) {
        for (const std::size_t i : search::apply(parsed.filters, parsed.sort.value_or(sort_), metas))
            rows.push_back({.path = metas[i].path});
    } else if (mode == SearchMode::Global) {
        if (global_searcher_) {
            auto hits = global_searcher_(query, metas);  // argümanları ve `sort:`u search_all uygular
            if (!hits)
                return std::unexpected(std::move(hits.error()));
            for (search::GlobalHit& hit : *hits) {
                ListRow row{.path = std::move(hit.path),
                            .positions = std::move(hit.name_positions),
                            .hit_count = hit.content_hits};
                if (hit.best_line) {
                    row.hits.push_back(std::move(*hit.best_line));
                    row.show_best_line = true;
                }
                rows.push_back(std::move(row));
            }
        }
    } else if (mode == SearchMode::Content) {
        if (content_searcher_) {
            // İndeks listeden eski olabilir: listede olmayan sonuç düşer.
            std::set<core::NotePath> allowed;
            for (const std::size_t i : search::apply(parsed.filters, std::nullopt, metas))
                allowed.insert(metas[i].path);
            for (search::ContentResult& result : content_searcher_(parsed.text))
                if (allowed.contains(result.path))
                    rows.push_back({.path = std::move(result.path),
                                    .hit_count = result.hit_count,
                                    .hits = std::move(result.first_hits)});
        }
    } else {
        // Argümansız kök aramasında tuş başına not kopyası yok.
        std::vector<core::NoteInfo> subset;
        std::span<const core::NoteInfo> candidates = notes_;
        if (!parsed.filters.empty() || !scope.empty()) {
            for (const std::size_t i : search::apply(parsed.filters, std::nullopt, metas))
                subset.push_back(scoped_note(i));
            candidates = subset;
        }
        for (search::Ranked& ranked : search::rank_notes(parsed.text, candidates, fuzzy_candidate_chars_))
            rows.push_back(
                {.path = candidates[ranked.index].path, .positions = std::move(ranked.match.positions)});
    }
    if (parsed.sort && !parsed.text.empty() && mode != SearchMode::Global)
        sort_rows(rows, *parsed.sort);
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i].number = i + 1;
    return rows;
}

// `sort:` açıkça verildiyse o sıra; eşitlikte arama sırası kalır (sort_notes kararlıdır).
void Listing::sort_rows(std::vector<ListRow>& rows, const search::SortSpec& sort) const {
    std::vector<std::size_t> order;  // metas_ dizini
    std::map<std::size_t, std::size_t> row_of;
    order.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::size_t index = meta_index_.at(rows[i].path);
        order.push_back(index);
        row_of.emplace(index, i);
    }
    search::sort_notes(order, sort, metas_);
    std::vector<ListRow> sorted;
    sorted.reserve(rows.size());
    for (const std::size_t index : order)
        sorted.push_back(std::move(rows[row_of.at(index)]));
    rows = std::move(sorted);
}

}  // namespace noteapp::app
