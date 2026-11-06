// Kaydedilmiş Cypher görünümleri (SPEC-graph "Görünümler").
#include "core/note_views.hpp"

#include "core/note_store_impl.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace noteapp::core {

namespace {

namespace chr = std::chrono;
using graph::NodeId;

// Görünen boşluklar (ASCII ve Unicode Zs): adın başında ya da sonunda yasak.
bool is_edge_space(char32_t cp) {
    constexpr char32_t kNoBreak = 0xA0;
    constexpr char32_t kOgham = 0x1680;
    constexpr char32_t kEnQuad = 0x2000;
    constexpr char32_t kHairSpace = 0x200A;
    constexpr char32_t kNarrowNoBreak = 0x202F;
    constexpr char32_t kMediumMath = 0x205F;
    constexpr char32_t kIdeographic = 0x3000;
    return cp == U' ' || cp == U'\t' || cp == kNoBreak || cp == kOgham || (cp >= kEnQuad && cp <= kHairSpace) ||
           cp == kNarrowNoBreak || cp == kMediumMath || cp == kIdeographic;
}

struct ViewNode {
    NodeId id;
    SavedView view;
};

// Ad eşleşmesi etiketlerinkiyle aynı katlamadır.
std::string view_key(std::string_view name) {
    return tag_key(name);
}

NodeId derived_view_id(const std::string& key) {
    return graph::derive_node_id(kViewLabel, as_bytes(key));
}

std::optional<chr::system_clock::time_point> time_of(const graph::Value& value) {
    const auto* stamp = std::get_if<graph::Timestamp>(&value);
    if (stamp == nullptr)
        return std::nullopt;
    return chr::system_clock::time_point(chr::milliseconds(stamp->unix_ms));
}

graph::Value stamp(chr::system_clock::time_point when) {
    return graph::Value{
        graph::Timestamp{chr::duration_cast<chr::milliseconds>(when.time_since_epoch()).count()}};
}

std::string text_of(const graph::Value& value) {
    const auto* text = std::get_if<std::string>(&value);
    return text == nullptr ? std::string() : *text;
}

graph::Result<std::optional<ViewNode>> read_view(graph::ReadTxn& txn, const NodeId& id) {
    auto name = txn.property(id, kName);
    auto query = txn.property(id, kViewQuery);
    auto created = txn.property(id, kCreatedAt);
    auto updated = txn.property(id, kUpdatedAt);
    if (!name || !query || !created || !updated)
        return std::unexpected(!name      ? name.error()
                               : !query   ? query.error()
                               : !created ? created.error()
                                          : updated.error());
    const auto* text = std::get_if<std::string>(&*name);
    if (text == nullptr || text->empty())
        return std::nullopt;  // adsız düğüm görünüm değildir: senkronla gelen yarım veri
    // Senkronla ya da ham Cypher'la gelen ad kayıt kuralından geçmemiş olabilir (satır sonu, denetim, `/`, `:`):
    // okuma da aynı kuralı uygular, böyle görünüm yoktur (listede, Tab tamamlamasında, `view:`de).
    if (!validate_view_name(*text))
        return std::nullopt;
    return ViewNode{id, SavedView{*text, text_of(*query), time_of(*created), time_of(*updated)}};
}

// Anahtar -> görünüm. Aynı anahtarda birden çok düğüm (Cypher'la elle kurulmuş) varsa anahtardan türeyen
// kimlik, yoksa ilk düğüm seçilir: her cihaz aynı düğümü görür.
graph::Result<std::map<std::string, ViewNode>> load_views(graph::ReadTxn& txn) {
    auto cursor = txn.nodes_with_label(kViewLabel);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::vector<NodeId> ids;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        ids.push_back(**next);
    }
    std::map<std::string, ViewNode> views;
    for (const NodeId& id : ids) {
        auto node = read_view(txn, id);
        if (!node)
            return std::unexpected(node.error());
        if (!*node)
            continue;
        std::string key = view_key((*node)->view.name);
        const bool derived = id == derived_view_id(key);
        auto [at, added] = views.try_emplace(std::move(key), std::move(**node));
        if (!added && derived)
            at->second = std::move(**node);
    }
    return views;
}

Error view_not_found(std::string_view name) {
    return Error{Error::Code::NotFound, "error.view_not_found", {{"name", std::string(name)}}};
}

// Sorgu hatası: görünümün kendi sorunu InvalidQuery'dir (ileti motorun konumlu iletisi), depo hatası Io.
Error query_error(const graph::Error& error, std::string_view name, std::string_view database) {
    using Code = graph::Error::Code;
    std::vector<std::pair<std::string, std::string>> args = error.args;
    args.emplace_back("view", std::string(name));
    if (error.code == Code::ReadOnlyViolation)
        return Error{Error::Code::InvalidQuery, "error.view_writes", std::move(args)};
    if (error.message_key == "graph.cypher_missing_param")
        return Error{Error::Code::InvalidQuery, "error.view_param", std::move(args)};
    switch (error.code) {
    case Code::Syntax:
    case Code::Unsupported:
    case Code::InvalidArgument:
    case Code::NotFound:
    case Code::LimitExceeded:
        return Error{Error::Code::InvalidQuery, error.message_key, std::move(args)};
    default:
        return from_graph(error, database);
    }
}

// Depo yoksa boş görüntü; okunamıyorsa nedeni. mutex tutulurken çağrılır.
std::expected<std::shared_ptr<const Snapshot>, Error> open_locked(NoteStore::Impl& impl) {
    auto snapshot = impl.current();
    if (!snapshot && impl.last_error)
        return std::unexpected(*impl.last_error);
    return snapshot;
}

std::expected<std::map<std::string, ViewNode>, Error> views_locked(NoteStore::Impl& impl) {
    auto snapshot = open_locked(impl);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    if (!*snapshot)
        return std::map<std::string, ViewNode>{};
    auto txn = impl.opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), path_utf8(impl.database())));
    auto views = load_views(*txn);
    if (!views)
        return std::unexpected(from_graph(views.error(), path_utf8(impl.database())));
    return std::move(*views);
}

std::expected<SavedView, Error> find_locked(NoteStore::Impl& impl, std::string_view name) {
    if (impl.view_lookup_fault)
        if (auto fault = impl.view_lookup_fault())
            return std::unexpected(std::move(*fault));
    auto views = views_locked(impl);
    if (!views)
        return std::unexpected(views.error());
    const auto found = views->find(view_key(name));
    if (found == views->end())
        return std::unexpected(view_not_found(name));
    return std::move(found->second.view);
}

// Güvenlik sınırı: görünüm başka cihazdan gelmiş olabilir. Mod her zaman ReadOnly: yazan sorgu plan
// aşamasında reddedilir, işlem açılmaz; motorun ikinci savunması PRAGMA query_only'dir.
std::expected<graph::QueryResult, Error> run_locked(NoteStore::Impl& impl, std::string_view name) {
    auto view = find_locked(impl, name);
    if (!view)
        return std::unexpected(view.error());
    if (impl.before_view_run)
        impl.before_view_run();
    auto result = impl.opened_graph().query(view->query, {}, graph::QueryMode::ReadOnly, impl.view_limits);
    if (!result)
        return std::unexpected(query_error(result.error(), name, path_utf8(impl.database())));
    return std::move(*result);
}

// Hücrenin notu: canlı Note düğümü ya da canlı not yolu olan metin.
std::optional<NotePath> note_in(const graph::QueryValue& cell, const Snapshot& snapshot) {
    if (const auto* id = std::get_if<NodeId>(&cell.v)) {
        const auto row = snapshot.live_by_id.find(*id);
        return row == snapshot.live_by_id.end() ? std::nullopt
                                                : std::optional(snapshot.live[row->second].info.path);
    }
    if (const auto* text = std::get_if<std::string>(&cell.v)) {
        const auto row = snapshot.live_by_path.find(*text);
        return row == snapshot.live_by_path.end() ? std::nullopt
                                                  : std::optional(snapshot.live[row->second].info.path);
    }
    return std::nullopt;
}

bool can_name_a_note(const graph::QueryValue& cell) {
    return std::holds_alternative<NodeId>(cell.v) || std::holds_alternative<std::string>(cell.v);
}

std::expected<std::vector<NotePath>, Error> notes_locked(NoteStore::Impl& impl, std::string_view name,
                                                         const Snapshot& snapshot) {
    auto result = run_locked(impl, name);
    if (!result)
        return std::unexpected(result.error());
    std::set<NotePath> notes;
    bool can_name = false;
    for (const auto& row : result->rows)
        for (const graph::QueryValue& cell : row) {
            can_name = can_name || can_name_a_note(cell);
            if (auto path = note_in(cell, snapshot))
                notes.insert(std::move(*path));
        }
    if (!result->rows.empty() && !can_name)
        return std::unexpected(
            Error{Error::Code::InvalidQuery, "error.view_no_notes", {{"view", std::string(name)}}});
    return std::vector<NotePath>(notes.begin(), notes.end());
}

}  // namespace

std::expected<void, Error> validate_view_name(std::string_view name) {
    if (auto parsed = NotePath::parse(name); !parsed)
        return std::unexpected(parsed.error());
    if (name.find('/') != std::string_view::npos)
        return std::unexpected(invalid_path("view_name_slash", name));
    // Baştaki ya da sondaki boşluk adı yazılan metinden ayırır: harita sorgusu metni kırpıp görünümü arar
    // (app::MapQuery), ` MATCH …` adlı senkron görünüm bulunmaz, Cypher olarak çalışırdı (güvenlik incelemesi,
    // Sev-2). Kayıt ve okuma aynı kural: saklı ad hiç kenar boşluğu taşımaz, kırpılmış arama onunla eşleşir.
    const auto decoded = decode_utf8(name);
    if (decoded && !decoded->empty() && (is_edge_space(decoded->front()) || is_edge_space(decoded->back())))
        return std::unexpected(invalid_path("view_name_space", name));
    return {};
}

std::expected<std::vector<SavedView>, Error> list_views(const NoteStore& store) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    auto views = views_locked(impl);
    if (!views)
        return std::unexpected(views.error());
    std::vector<SavedView> out;
    out.reserve(views->size());
    for (auto& [key, node] : *views)
        out.push_back(std::move(node.view));
    return out;
}

std::expected<SavedView, Error> find_view(const NoteStore& store, std::string_view name) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    return find_locked(impl, name);
}

std::expected<void, Error> save_view(NoteStore& store, std::string_view name, std::string_view query,
                                     bool overwrite, chr::system_clock::time_point now) {
    NOTEAPP_TRY(validate_view_name(name));
    if (auto checked = graph::check_query(query, graph::QueryMode::ReadOnly, {}, store.impl().view_limits);
        !checked)
        return std::unexpected(query_error(checked.error(), name, path_utf8(store.impl().database())));
    const std::string key = view_key(name);
    return store.impl().write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        graph::WriteTxn& txn = context.txn;
        auto views = load_views(txn);
        if (!views)
            return std::unexpected(from_graph(views.error(), name));
        const auto existing = views->find(key);
        if (existing != views->end() && !overwrite)
            return std::unexpected(Error{
                Error::Code::AlreadyExists, "error.view_exists", {{"name", existing->second.view.name}}});
        const NodeId id = existing != views->end() ? existing->second.id : derived_view_id(key);
        // Düğüm her kayıtta yeniden yazılır: başka cihazın bu yazmayı görmeden yaptığı silme etkisiz kalır.
        NOTEAPP_TRY_GRAPH(txn.put_node(id), name);
        if (existing == views->end()) {
            NOTEAPP_TRY_GRAPH(txn.add_label(id, kViewLabel), name);
            NOTEAPP_TRY_GRAPH(txn.set_property(id, kName, graph::Value{std::string(name)}), name);
            NOTEAPP_TRY_GRAPH(txn.set_property(id, kCreatedAt, stamp(now)), name);
        }
        NOTEAPP_TRY_GRAPH(txn.set_property(id, kViewQuery, graph::Value{std::string(query)}), name);
        NOTEAPP_TRY_GRAPH(txn.set_property(id, kUpdatedAt, stamp(now)), name);
        return {};
    });
}

std::expected<void, Error> remove_view(NoteStore& store, std::string_view name) {
    NOTEAPP_TRY(find_view(store, name));  // yokken yazma açılmasın: boş depoda graph.db kurulmaz
    const std::string key = view_key(name);
    return store.impl().write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        auto views = load_views(context.txn);
        if (!views)
            return std::unexpected(from_graph(views.error(), name));
        const auto existing = views->find(key);
        if (existing == views->end())
            return std::unexpected(view_not_found(name));
        NOTEAPP_TRY(context.delete_node(existing->second.id, name));  // kenarı çoksa işlemlere bölünür
        return {};
    });
}

std::expected<graph::QueryResult, Error> run_view(const NoteStore& store, std::string_view name) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    return run_locked(impl, name);
}

std::expected<std::vector<NotePath>, Error> view_notes(const NoteStore& store, std::string_view name) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    auto snapshot = open_locked(impl);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    if (!*snapshot)
        return std::unexpected(view_not_found(name));
    const std::string key = view_key(name);
    if (const auto cached = (*snapshot)->view_results.find(key); cached != (*snapshot)->view_results.end())
        return cached->second;
    auto notes = notes_locked(impl, name, **snapshot);
    // Depo hatası (Busy, G/Ç) geçicidir, saklanmaz; görünümün kendi sonucu (hata dahil) bu görüntüde
    // değişmez.
    if (notes || notes.error().code != Error::Code::Io)
        (*snapshot)->view_results.emplace(key, notes);
    return notes;
}

}  // namespace noteapp::core
