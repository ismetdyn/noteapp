#include "app/listing.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_views.hpp"
#include "core/text.hpp"
#include "search/folder_order.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <iterator>
#include <numeric>
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

std::shared_ptr<const std::vector<core::NoteEdge>> search_edges(const core::NoteStore& store) {
    auto edges = store.shared_edges();
    if (edges)
        return std::move(*edges);
    core::log_warn("search: cannot read edges: " + edges.error().message_key);
    return nullptr;
}

std::shared_ptr<const core::PropertyNotes> search_properties(const core::NoteStore& store,
                                                             std::string_view key) {
    auto notes = core::notes_with_property(store, key);
    if (notes)
        return std::move(*notes);
    core::log_warn("search: cannot read properties: " + notes.error().message_key);
    return nullptr;
}

std::optional<std::size_t> relation_depth(const core::Config& config) {
    if (!config.tuning.search_relations)
        return std::nullopt;
    return config.limits.search.relation_depth.cap();
}

std::string sync_state_text(core::SyncState state) {
    switch (state) {
    case core::SyncState::Synced:
        return core::tr("app.sync_state_synced");
    case core::SyncState::Modified:
        return core::tr("app.sync_state_modified");
    case core::SyncState::New:
        return core::tr("app.sync_state_new");
    case core::SyncState::ConflictCopy:
        return core::tr("app.sync_state_conflict");
    case core::SyncState::SyncOff:
        break;
    }
    return core::tr("app.sync_off");
}

std::string reason_text(const search::RelationReason& reason) {
    using Kind = search::RelationReason::Kind;
    const std::string& other = reason.other;
    switch (reason.kind) {
    case Kind::Tag:
        return core::tr("search.reason_tag", {{"tag", reason.label}});
    case Kind::Relation: {
        const char* arrow = reason.direction == core::RelationDirection::Outgoing   ? "→"
                            : reason.direction == core::RelationDirection::Incoming ? "←"
                                                                                    : "—";
        return core::tr("search.reason_relation",
                        {{"name", reason.label}, {"arrow", arrow}, {"path", other}});
    }
    case Kind::Linked:
        break;
    }
    if (reason.hops > 1)
        return core::tr("search.reason_linked_far", {{"path", other}, {"hops", std::to_string(reason.hops)}});
    if (!reason.label.empty())
        return core::tr("search.reason_linked_named", {{"path", other}, {"name", reason.label}});
    return core::tr("search.reason_linked", {{"path", other}});
}

SearchEnter search_enter_action(SearchMode mode, bool has_selection, bool query_error, std::string_view query) {
    if (mode == SearchMode::Global)
        return has_selection && !query_error ? SearchEnter::Remember : SearchEnter::Nothing;
    if (has_selection)
        return query_error ? SearchEnter::Select : SearchEnter::Remember;
    if (query.empty() || query_error)
        return SearchEnter::Nothing;
    const auto parsed = search::parse_query(query);
    return parsed && parsed->filters.empty() && !parsed->sort ? SearchEnter::CreateNote : SearchEnter::Nothing;
}

std::string note_field_text(const core::NoteMeta& meta, NoteField field,
                            std::optional<std::chrono::system_clock::time_point> short_now) {
    const auto time_text = [&short_now](std::filesystem::file_time_type when) {
        return short_now ? core::short_time_text(when, *short_now) : core::local_time_text(when);
    };
    const auto or_none = [](std::string text) { return text.empty() ? core::tr("app.note_none") : text; };
    switch (field) {
    case NoteField::Size:
        return core::human_size(meta.size);
    case NoteField::Updated:
        return time_text(meta.updated_at);
    case NoteField::Created:
        return meta.created_at ? time_text(*meta.created_at) : core::tr("app.note_unknown");
    case NoteField::Sync:
        return sync_state_text(meta.sync_status);
    case NoteField::Origin:
        return meta.origin.value_or(core::tr("app.note_unknown"));
    case NoteField::Type:
        return or_none(core::split_note_path(meta.path.string()).type);
    case NoteField::Tags: {
        std::string tags;  // son kayıttaki etiketler: "#a #b"
        for (const std::string& tag : meta.tags)
            tags += (tags.empty() ? "#" : " #") + tag;
        return or_none(std::move(tags));
    }
    }
    return {};
}

std::string column_text(const core::NoteMeta& meta, ListColumn column,
                        std::optional<std::chrono::system_clock::time_point> short_now) {
    switch (column) {
    case ListColumn::Size:
        return note_field_text(meta, NoteField::Size, short_now);
    case ListColumn::Updated:
        return note_field_text(meta, NoteField::Updated, short_now);
    case ListColumn::Created:
        return note_field_text(meta, NoteField::Created, short_now);
    case ListColumn::Sync:
        return note_field_text(meta, NoteField::Sync, short_now);
    case ListColumn::Origin:
        return note_field_text(meta, NoteField::Origin, short_now);
    case ListColumn::SizeAndDate:
    case ListColumn::None:
        break;
    }
    return {};
}

void connect_store(Listing& listing, const core::NoteStore& store, const core::Config& config,
                   ContentIndexSource index, std::mutex* store_mutex) {
    // Kilitsiz arayüzde (GUI) boş kilit: depo yalnız UI iş parçacığındadır.
    const auto locked = [store_mutex](auto&& read) {
        if (store_mutex == nullptr)
            return read();
        const std::scoped_lock lock(*store_mutex);
        return read();
    };
    search::ViewLookup views = [&store, locked](std::string_view name) {
        return locked([&] { return core::view_notes(store, name); });
    };
    listing.set_view_lookup(views);
    listing.set_edge_source([&store, locked] { return locked([&] { return search_edges(store); }); });
    listing.set_property_source([&store, locked](std::string_view key) {
        return locked([&] { return search_properties(store, key); });
    });
    listing.set_relation_depth(relation_depth(config));
    listing.set_fuzzy_candidate_chars(config.limits.search.fuzzy_candidate_chars.cap());
    if (!index)
        return;
    listing.set_content_results(config.limits.search.content_results.cap());
    listing.set_content_index(index);
    listing.set_global_searcher([&config, index, views = std::move(views)](
                                    std::string_view query, std::span<const core::NoteMeta> notes,
                                    const search::GraphInput& graph) {
        return search::search_all(query, notes, index(), config.limits.search, views, graph);
    });
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
    meta_nodes_.clear();  // sıralar değişti: index_of yeniden bulur
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
    if (const auto keep = open_folder_filter(tree_))  // boş ağaç: önceki liste korunur
        std::erase_if(open_, [&keep](const std::string& folder) { return !keep(folder); });
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

namespace {

// İzinli ama doğrudan satırda olmayan notlar (metas dizinleri; shown metas boyunda): ilişki yalnız bunlara
// bakar. Dizinle işaretlenir, yol karşılaştırması yok (50.000 notta tuş başına).
std::vector<std::size_t> not_shown(std::span<const std::size_t> allowed, const std::vector<char>& shown) {
    std::vector<std::size_t> rest;
    rest.reserve(allowed.size());
    for (const std::size_t i : allowed)
        if (shown[i] == 0)
            rest.push_back(i);
    return rest;
}

std::vector<ListRow> global_rows(std::vector<search::GlobalHit> hits) {
    std::vector<ListRow> rows;
    rows.reserve(hits.size());
    for (search::GlobalHit& hit : hits) {
        ListRow row{.path = std::move(hit.path),
                    .positions = std::move(hit.name_positions),
                    .hit_count = hit.content_hits,
                    .reason = std::move(hit.reason)};
        if (hit.best_line) {
            row.hits.push_back(std::move(*hit.best_line));
            row.show_best_line = true;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

std::expected<std::vector<ListRow>, search::QueryError> Listing::rows_for(search::ParsedQuery parsed,
                                                                          std::string_view query,
                                                                          SearchMode mode,
                                                                          std::string_view scope) const {
    const auto edges = edges_for(parsed);
    const search::RelationGraph* index = index_of(edges);
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
    const std::vector<std::size_t> scoped_nodes = scoped_meta_nodes(index, scope, scoped_index);
    const search::GraphInput graph{edges ? std::span<const core::NoteEdge>(*edges)
                                         : std::span<const core::NoteEdge>{},
                                   relation_depth_, index,
                                   index == nullptr ? std::span<const std::size_t>{}
                                   : scope.empty()  ? std::span<const std::size_t>(meta_nodes_)
                                                    : std::span<const std::size_t>(scoped_nodes),
                                   property_source_};
    search::bind_graph_filters(parsed.filters, graph);

    std::vector<ListRow> rows;
    if (parsed.text.empty()) {
        for (const std::size_t i : search::apply(parsed.filters, parsed.sort.value_or(sort_), metas))
            rows.push_back({.path = metas[i].path});
    } else if (mode == SearchMode::Global) {
        if (global_searcher_) {
            auto hits = global_searcher_(query, metas, graph);  // argümanları ve `sort:`u search_all uygular
            if (!hits)
                return std::unexpected(std::move(hits.error()));
            rows = global_rows(std::move(*hits));
        }
    } else if (mode == SearchMode::Content) {
        rows = content_rows(parsed, metas, graph);
    } else {
        rows = name_rows(parsed, metas, scope.empty() ? nullptr : &scoped_index, graph);
    }
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i].number = i + 1;
    return rows;
}

std::shared_ptr<const std::vector<core::NoteEdge>>
Listing::edges_for(const search::ParsedQuery& parsed) const {
    // Kenarlar yalnız gerektiğinde ve arama başına bir kez (depo görüntü başına önbellekler).
    const bool wants = search::has_graph_filter(parsed.filters) || (relation_depth_ && !parsed.text.empty());
    return edge_source_ && wants ? edge_source_() : nullptr;
}

// Aynı liste (görüntü başına paylaşılan) tuş tuş geldikçe dizin yeniden kurulmaz; liste değişince yenisi.
// Notların düğümleri (meta_nodes_) dizin ya da not listesi değişince yeniden bulunur.
const search::RelationGraph*
Listing::index_of(const std::shared_ptr<const std::vector<core::NoteEdge>>& edges) const {
    if (!edges)
        return nullptr;
    if (edges != indexed_edges_) {
        edge_index_ = std::make_shared<const search::RelationGraph>(*edges);
        indexed_edges_ = edges;
        meta_nodes_.clear();
    }
    if (meta_nodes_.size() != metas_.size())
        meta_nodes_ = search::note_nodes(*edge_index_, metas_);
    return edge_index_.get();
}

// Alt klasör kapsamında notların düğümleri (kökte meta_nodes_ doğrudan kullanılır).
std::vector<std::size_t> Listing::scoped_meta_nodes(const search::RelationGraph* index,
                                                    std::string_view scope,
                                                    const std::vector<std::size_t>& scoped_index) const {
    std::vector<std::size_t> nodes;
    if (index == nullptr || scope.empty())
        return nodes;
    nodes.reserve(scoped_index.size());
    for (const std::size_t i : scoped_index)
        nodes.push_back(meta_nodes_[i]);
    return nodes;
}

std::vector<ListRow> Listing::content_rows(const search::ParsedQuery& parsed,
                                           std::span<const core::NoteMeta> metas,
                                           const search::GraphInput& graph) const {
    std::vector<ListRow> rows;
    if (!content_index_ && !content_searcher_)
        return rows;
    // İndeks listeden eski olabilir: listede olmayan sonuç düşer.
    const std::vector<std::size_t> indices = search::apply(parsed.filters, std::nullopt, metas);
    std::map<core::NotePath, std::size_t> allowed;  // yol -> metas dizini
    for (const std::size_t i : indices)
        allowed.emplace(metas[i].path, i);
    // Süzgeç sınırdan önce indekse gider: büyük depoda süzülen notlar için kesit üretilmez (SD7 review).
    std::vector<search::ContentResult> results =
        content_index_ ? content_index_().search(parsed.text, content_results_,
                                                 [&allowed](const core::NotePath& path) { return allowed.contains(path); })
                       : content_searcher_(parsed.text);
    std::vector<char> shown(metas.size(), 0);
    for (search::ContentResult& result : results) {
        if (rows.size() >= content_results_)
            break;
        if (const auto found = allowed.find(result.path); found != allowed.end()) {
            shown[found->second] = 1;
            rows.push_back({.path = std::move(result.path),
                            .hit_count = result.hit_count,
                            .hits = std::move(result.first_hits)});
        }
    }
    return with_relation_rows(std::move(rows), parsed, metas, not_shown(indices, shown), graph);
}

// scoped_index nullptr ise kapsam köktür (metas = metas_); değilse metas[i] = metas_[(*scoped_index)[i]].
std::vector<ListRow> Listing::name_rows(const search::ParsedQuery& parsed,
                                        std::span<const core::NoteMeta> metas,
                                        const std::vector<std::size_t>* scoped_index,
                                        const search::GraphInput& graph) const {
    // Argümansız kök aramasında tuş başına not kopyası yok.
    std::vector<core::NoteInfo> subset;
    std::span<const core::NoteInfo> candidates = notes_;
    std::vector<std::size_t> indices;  // candidates -> metas; argümansız kökte birebir
    if (!parsed.filters.empty() || scoped_index != nullptr) {
        indices = search::apply(parsed.filters, std::nullopt, metas);
        for (const std::size_t i : indices)
            subset.push_back(notes_[scoped_index == nullptr ? i : (*scoped_index)[i]]);
        candidates = subset;
    } else {
        indices.resize(metas.size());
        std::iota(indices.begin(), indices.end(), std::size_t{0});
    }
    std::vector<ListRow> rows;
    std::vector<char> shown(metas.size(), 0);
    for (search::Ranked& ranked : search::rank_notes(parsed.text, candidates, fuzzy_candidate_chars_)) {
        shown[indices[ranked.index]] = 1;
        rows.push_back(
            {.path = candidates[ranked.index].path, .positions = std::move(ranked.match.positions)});
    }
    return with_relation_rows(std::move(rows), parsed, metas, not_shown(indices, shown), graph);
}

// Doğrudan satırların altına `rest`in (doğrudan satırda olmayan izinli notlar) ilişki satırları
// (relation_hits sırası); `sort:` iki grubu ayrı sıralar.
std::vector<ListRow> Listing::with_relation_rows(std::vector<ListRow> direct,
                                                 const search::ParsedQuery& parsed,
                                                 std::span<const core::NoteMeta> metas,
                                                 std::span<const std::size_t> rest,
                                                 const search::GraphInput& graph) const {
    std::vector<ListRow> related;
    for (search::RelationHit& hit : search::relation_hits(parsed.text, metas, rest, graph))
        related.push_back({.path = metas[hit.index].path, .reason = std::move(hit.reason)});
    if (parsed.sort) {
        sort_rows(direct, *parsed.sort);
        sort_rows(related, *parsed.sort);
    }
    std::ranges::move(related, std::back_inserter(direct));
    return direct;
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

std::optional<FolderSummary> folder_summary(const Listing& listing, std::string_view folder,
                                            std::chrono::system_clock::time_point now) {
    const core::FolderNode* node = listing.tree().find(folder);
    if (node == nullptr)
        return std::nullopt;
    FolderSummary summary{
        .path = node->path,
        .counts = core::tr("tui.folder_counts",
                           {{"items", core::count_text("tui.count_items", node->item_count())},
                            {"notes", core::count_text("tui.count_notes", node->note_count)},
                            {"folders", core::count_text("tui.count_folders", node->folder_count)}}),
        .size = core::human_size(node->size),
        .updated = node->updated_at ? core::local_time_text(*node->updated_at) : core::tr("app.note_unknown"),
        .recent = {}};
    const std::size_t skip = node->path.empty() ? 0 : node->path.size() + 1;  // "klasör/"
    for (const std::size_t index : node->recent) {
        const core::NoteMeta& meta = listing.metas()[index];
        const std::string full = meta.path.string();
        summary.recent.push_back({meta.path, full.substr(skip), core::short_time_text(meta.updated_at, now)});
    }
    return summary;
}

}  // namespace noteapp::app
