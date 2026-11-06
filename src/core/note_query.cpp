// Kullanıcının Cypher sorgusu NoteStore üzerinden (SPEC-graph "Sorgu çubuğu").
#include "core/note_query.hpp"

#include "core/note_store_impl.hpp"
#include "core/note_views.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::core {

namespace {

using graph::EdgeId;
using graph::NodeId;
using graph::QueryChange;

// Uygulama şemasının etiketleri ve türettiği özellikleri (SPEC-graph "Veri modeli"). Bu anahtarlar yalnız
// NoteStore'un yazma yollarıyla değişir: gövde bağlantı ve etiket kenarlarını, yol klasör kenarlarını
// türetir; ham yazma onları birbirinden koparırdı.
constexpr std::array kNoteKeys = {kName,      kType,       kBody,         kCreatedAt,   kUpdatedAt,   kNoteOrigin,
                                  kDeletedAt, kTrashedFrom, kMigratedSha, kBodyParents, kLinkTargets};
constexpr std::array kFolderKeys = {kName};
constexpr std::array kTagKeys = {kName, kTagDisplay};
constexpr std::array kViewKeys = {kName, kViewQuery, kCreatedAt, kUpdatedAt};

struct ProtectedLabel {
    std::string_view label;
    std::span<const std::string_view> keys;
};
constexpr std::array kProtectedLabels = {ProtectedLabel{kNoteLabel, kNoteKeys},
                                         ProtectedLabel{kFolderLabel, kFolderKeys},
                                         ProtectedLabel{kTagLabel, kTagKeys}, ProtectedLabel{kViewLabel, kViewKeys}};
// Türetilmiş kenarlar: bağlantı gövdenin [[...]]'ı, etiket #...'ı, klasör kenarları yoldur.
constexpr std::array kProtectedEdges = {kLinksTo, kTagged, kInFolder, kChildOf};

const ProtectedLabel* protected_label(std::string_view label) {
    const auto found = std::ranges::find(kProtectedLabels, label, &ProtectedLabel::label);
    return found == kProtectedLabels.end() ? nullptr : &*found;
}

bool protected_edge(std::string_view type) {
    return std::ranges::find(kProtectedEdges, type) != kProtectedEdges.end();
}

graph::Error refused(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    return graph::Error{graph::Error::Code::InvalidArgument, std::move(key), std::move(args)};
}

graph::Error protected_edge_error(std::string_view type) {
    return refused("error.query_protected_edge", {{"type", std::string(type)}});
}

// Motorun denetim kancası: değişiklik öncesini gören işlemle uygulama şemasını korur ve özeti sayar.
class SchemaGuard {
public:
    SchemaGuard(QueryChanges& counts, std::size_t relation_name_bytes)
        : counts_(&counts), relation_name_bytes_(relation_name_bytes) {}

    graph::Result<void> operator()(graph::ReadTxn& txn, const QueryChange& change) const {
        using Kind = QueryChange::Kind;
        switch (change.kind) {
        case Kind::CreateNode:
            ++counts_->nodes_created;
            return {};
        case Kind::DeleteNode:
            return check_delete_node(txn, std::get<NodeId>(change.target));
        case Kind::AddLabel:
        case Kind::RemoveLabel:
            if (protected_label(change.name) != nullptr)
                return std::unexpected(refused("error.query_protected_label", {{"label", std::string(change.name)}}));
            ++(change.kind == Kind::AddLabel ? counts_->labels_added : counts_->labels_removed);
            return {};
        case Kind::CreateEdge:
        case Kind::DeleteEdge:
            if (protected_edge(change.name))
                return std::unexpected(protected_edge_error(change.name));
            // İlişkinin değişmezleri (türetilmiş kimlik, yönsüzde sıralı uçlar, çift başına ad başına tek ilişki)
            // NoteStore::add_relation'dadır: ham CREATE/MERGE onları atlardı (lead kararı). Silme ve ad/yön
            // yazımı serbesttir (doğrulanır).
            if (change.kind == Kind::CreateEdge && change.name == kRelatedTo)
                return std::unexpected(refused("error.query_relation_create", {}));
            ++(change.kind == Kind::CreateEdge ? counts_->relationships_created : counts_->relationships_deleted);
            return {};
        case Kind::SetProperty:
            return check_property(txn, change);
        }
        return {};
    }

private:
    graph::Result<void> check_delete_node(graph::ReadTxn& txn, const NodeId& id) const {
        const auto labels = txn.labels(id);
        if (!labels)
            return std::unexpected(labels.error());
        for (const std::string& label : *labels)
            if (protected_label(label) != nullptr)
                return std::unexpected(refused("error.query_protected_node", {{"label", label}}));
        ++counts_->nodes_deleted;
        return {};
    }

    graph::Result<void> check_property(graph::ReadTxn& txn, const QueryChange& change) const {
        if (const auto* node = std::get_if<NodeId>(&change.target)) {
            const auto labels = txn.labels(*node);
            if (!labels)
                return std::unexpected(labels.error());
            for (const std::string& label : *labels)
                if (const ProtectedLabel* schema = protected_label(label);
                    schema != nullptr && std::ranges::find(schema->keys, change.name) != schema->keys.end())
                    return std::unexpected(refused("error.query_protected_property",
                                                   {{"label", label}, {"key", std::string(change.name)}}));
        } else {
            const auto ends = txn.edge(std::get<EdgeId>(change.target));
            if (!ends)
                return std::unexpected(ends.error());
            if (*ends && protected_edge((*ends)->type))
                return std::unexpected(protected_edge_error((*ends)->type));
            if (*ends && (*ends)->type == kRelatedTo && change.value != nullptr)
                if (auto valid = check_relation_property(change.name, *change.value); !valid)
                    return valid;
        }
        ++counts_->properties_set;
        return {};
    }

    // İlişkinin adı ve yönü NoteStore'un kuralıyla (SPEC-graph "Adlı ilişkiler"): ad relation_name'den geçer,
    // directed yalnız true yazılır. null ikisini de kaldırır.
    graph::Result<void> check_relation_property(std::string_view key, const graph::Value& value) const {
        if (std::holds_alternative<std::monostate>(value))
            return {};
        if (key == kName) {
            const auto* text = std::get_if<std::string>(&value);
            if (text == nullptr)
                return std::unexpected(refused("error.query_relation_name", {{"reason", "not_text"}}));
            if (const auto name = relation_name(*text, relation_name_bytes_); !name) {
                const auto reason = std::ranges::find(name.error().args, std::string("reason"),
                                                      &std::pair<std::string, std::string>::first);
                return std::unexpected(refused(
                    "error.query_relation_name",
                    {{"reason", reason == name.error().args.end() ? std::string() : reason->second}}));
            }
        } else if (key == kDirected) {
            const auto* flag = std::get_if<bool>(&value);
            if (flag == nullptr || !*flag)
                return std::unexpected(refused("error.query_relation_directed", {}));
        }
        return {};
    }

    QueryChanges* counts_;
    std::size_t relation_name_bytes_;
};

// Sorgunun kendi sorunu (sözdizimi, sınır, korunan veri) InvalidQuery: ileti motorun ya da korumanın konumlu
// iletisidir. Depo hatası Io.
Error query_error(const graph::Error& error, std::string_view database) {
    using Code = graph::Error::Code;
    switch (error.code) {
    case Code::ReadOnlyViolation:
        return Error{Error::Code::InvalidQuery, "error.query_writes", error.args};
    case Code::Syntax:
    case Code::Unsupported:
    case Code::InvalidArgument:
    case Code::NotFound:
    case Code::LimitExceeded:
        return Error{Error::Code::InvalidQuery, error.message_key, error.args};
    default:
        return from_graph(error, database);
    }
}

std::string edge_hex(const EdgeId& id) {
    return id_hex(NodeId{id.bytes});
}

void collect_ids(const graph::QueryValue& value, std::vector<NodeId>& nodes, std::vector<EdgeId>& edges) {
    if (const auto* node = std::get_if<NodeId>(&value.v))
        nodes.push_back(*node);
    else if (const auto* edge = std::get_if<EdgeId>(&value.v))
        edges.push_back(*edge);
    else if (const auto* list = std::get_if<graph::QueryList>(&value.v))
        for (const graph::QueryValue& item : list->items)
            collect_ids(item, nodes, edges);
}

// Sonuçtaki düğüm ve kenarları uygulamanın adlarıyla adlar: not yolu görüntüden, etiket ve kenar tek nokta
// okumasıyla.
// Adlandırma sorgunun süre bütçesine sayılır (limits.query.max_duration, sorgunun başından): 100.000 satırda
// kenar başına nokta okuması saniyeler alabilir, UI iş parçacığı bütçeden uzun donmasın.
class Namer {
public:
    Namer(graph::ReadTxn& txn, const Snapshot* snapshot, std::chrono::steady_clock::time_point deadline)
        : txn_(txn), snapshot_(snapshot), deadline_(deadline) {}

    graph::Result<void> name_all(UserQuery& query) {
        if (auto in_time = tick(); !in_time)  // sorgu süreyi zaten bitirmiş olabilir
            return in_time;
        std::vector<NodeId> nodes;
        std::vector<EdgeId> edges;
        for (const auto& row : query.result.rows)
            for (const graph::QueryValue& cell : row)
                collect_ids(cell, nodes, edges);
        for (const NodeId& id : nodes)
            if (!query.nodes.contains(id)) {
                if (auto in_time = tick(); !in_time)
                    return in_time;
                auto element = node(id);
                if (!element)
                    return std::unexpected(element.error());
                query.nodes.emplace(id, std::move(*element));
            }
        for (const EdgeId& id : edges)
            if (!query.edges.contains(id)) {
                if (auto in_time = tick(); !in_time)
                    return in_time;
                auto element = edge(id);
                if (!element)
                    return std::unexpected(element.error());
                query.edges.emplace(id, std::move(*element));
            }
        return {};
    }

private:
    // Saat her 1024 adlandırmada bir (ve başta) okunur: kare başına pahalı değil.
    graph::Result<void> tick() {
        constexpr std::size_t kTicksPerClockCheck = 1024;
        if (ticks_++ % kTicksPerClockCheck != 0 || std::chrono::steady_clock::now() <= deadline_)
            return {};
        return std::unexpected(graph::Error{graph::Error::Code::LimitExceeded, "graph.limit_exceeded",
                                            {{"limit", "duration"}}});
    }

    graph::Result<QueryElement> node(const NodeId& id) {
        using Kind = QueryElement::Kind;
        if (snapshot_ != nullptr) {
            if (const auto live = snapshot_->live_by_id.find(id); live != snapshot_->live_by_id.end())
                return QueryElement{Kind::Note, snapshot_->live[live->second].info.path.string()};
            if (auto trashed = note_path_in(*snapshot_, id))
                return QueryElement{Kind::TrashedNote, std::move(*trashed)};
        }
        const auto labels = txn_.labels(id);
        if (!labels)
            return std::unexpected(labels.error());
        if (std::ranges::find(*labels, kTagLabel) != labels->end()) {
            const auto name = txn_.property(id, kName);
            if (!name)
                return std::unexpected(name.error());
            if (const auto* key = std::get_if<std::string>(&*name))
                return QueryElement{Kind::Tag, *key};
        }
        QueryElement other{Kind::Node, id_hex(id)};
        if (!labels->empty())
            other.name = labels->front();
        return other;
    }

    graph::Result<std::string> end_key(const NodeId& id) {
        auto element = node(id);
        if (!element)
            return std::unexpected(element.error());
        return std::move(element->key);
    }

    graph::Result<QueryElement> edge(const EdgeId& id) {
        using Kind = QueryElement::Kind;
        const auto ends = txn_.edge(id);
        if (!ends)
            return std::unexpected(ends.error());
        if (!*ends)
            return QueryElement{Kind::Edge, edge_hex(id)};
        auto from = end_key((*ends)->src);
        if (!from)
            return std::unexpected(from.error());
        auto to = end_key((*ends)->dst);
        if (!to)
            return std::unexpected(to.error());
        const std::string& type = (*ends)->type;
        QueryElement element{type == kLinksTo     ? Kind::Link
                             : type == kRelatedTo ? Kind::Relation
                             : type == kTagged    ? Kind::Tagged
                                                  : Kind::Edge,
                             type, std::move(*from), std::move(*to)};
        if (element.kind == Kind::Relation) {
            const auto name = txn_.property(id, kName);
            const auto directed = txn_.property(id, kDirected);
            if (!name || !directed)
                return std::unexpected(!name ? name.error() : directed.error());
            if (const auto* text = std::get_if<std::string>(&*name))
                element.name = *text;
            const auto* flag = std::get_if<bool>(&*directed);
            element.directed = flag != nullptr && *flag;
        }
        return element;
    }

    graph::ReadTxn& txn_;
    const Snapshot* snapshot_;
    std::chrono::steady_clock::time_point deadline_;
    std::size_t ticks_ = 0;
};

// mutex tutulurken.
// Sorgunun süresinin bittiği an (motorun kuralı: sınırsızda taşmaz).
std::chrono::steady_clock::time_point deadline_after(std::chrono::milliseconds limit) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    return limit >= room ? Clock::time_point::max() : now + limit;
}

std::expected<void, Error> name_elements(NoteStore::Impl& impl, const Snapshot* snapshot, UserQuery& query,
                                         std::chrono::steady_clock::time_point deadline) {
    if (impl.query_naming_fault)
        if (auto fault = impl.query_naming_fault())
            return std::unexpected(std::move(*fault));
    if (query.result.rows.empty())
        return {};
    auto txn = impl.opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), path_utf8(impl.database())));
    if (auto named = Namer(*txn, snapshot, deadline).name_all(query); !named)
        return std::unexpected(query_error(named.error(), path_utf8(impl.database())));
    return {};
}

std::expected<UserQuery, Error> read_locked(NoteStore::Impl& impl, std::string_view cypher,
                                            const graph::QueryLimits& limits, const graph::Params& params) {
    const std::string database = path_utf8(impl.database());
    const auto deadline = deadline_after(limits.max_duration);  // adlandırma da bu süreye sayılır
    const auto snapshot = impl.current();
    if (!snapshot) {
        if (impl.last_error)
            return std::unexpected(*impl.last_error);
        // Depo yok: sonuç boştur ama sorgunun kendi hatası yine görünür; dosya kurulmaz.
        if (auto checked = graph::check_query(cypher, graph::QueryMode::ReadOnly, params, limits); !checked)
            return std::unexpected(query_error(checked.error(), database));
        return UserQuery{};
    }
    auto result = impl.opened_graph().query(cypher, params, graph::QueryMode::ReadOnly, limits);
    if (!result)
        return std::unexpected(query_error(result.error(), database));
    UserQuery query{std::move(*result), {}, {}, {}};
    NOTEAPP_TRY(name_elements(impl, snapshot.get(), query, deadline));
    return query;
}

// Tek işlem: ret ya da hata hiçbir şey yazmaz. Önizleme commit etmez. Yazmadan sonra görüntü yeniden kurulur:
// kendi yazmamız data_version'ı değiştirmez, eski görüntü yeni ilişkiyi göstermezdi.
std::expected<UserQuery, Error> write_locked(NoteStore::Impl& impl, std::string_view cypher, bool commit,
                                             const graph::QueryLimits& limits, const graph::Params& params) {
    const std::string database = path_utf8(impl.database());
    const auto deadline = deadline_after(limits.max_duration);  // adlandırma da bu süreye sayılır
    NOTEAPP_TRY(impl.check_gate());
    NOTEAPP_TRY(impl.open_for_write());
    QueryChanges changes;
    graph::QueryHooks hooks;
    hooks.before_change = SchemaGuard(changes, impl.relation_name_bytes);
    hooks.commit = commit;
    auto result = impl.opened_graph().query(cypher, params, graph::QueryMode::ReadWrite, limits, hooks);
    if (!result)
        return std::unexpected(query_error(result.error(), database));
    if (commit && changes.total() > 0)
        impl.snapshot.reset();
    UserQuery query{std::move(*result), changes, {}, {}};
    const auto snapshot = impl.current();
    auto named = !snapshot && impl.last_error ? std::expected<void, Error>(std::unexpected(*impl.last_error))
                                              : name_elements(impl, snapshot.get(), query, deadline);
    if (named)
        return query;
    // Commit edilmiş yazma geri alınamaz: hata "yazılmadı" sanılır ve arayüz tazelenmezdi (inceleme). Yazma
    // başarıdır, adlandırma hatası ayrıca raporlanır. Önizlemede bir şey yazılmadı: hata kalır.
    if (!commit)
        return std::unexpected(std::move(named).error());
    query.naming_error = std::move(named).error();
    return query;
}

}  // namespace

graph::Result<void> check_user_change(graph::ReadTxn& txn, const graph::QueryChange& change,
                                      std::size_t relation_name_bytes) {
    QueryChanges ignored;
    return SchemaGuard(ignored, relation_name_bytes)(txn, change);
}

bool protected_note_key(std::string_view key) {
    return std::ranges::find(kNoteKeys, key) != kNoteKeys.end();
}

std::expected<bool, Error> query_writes(std::string_view cypher, const graph::QueryLimits& limits) {
    const auto checked = graph::check_query(cypher, graph::QueryMode::ReadOnly, {}, limits);
    if (checked)
        return false;
    if (checked.error().code == graph::Error::Code::ReadOnlyViolation)
        return true;
    return std::unexpected(query_error(checked.error(), {}));
}

std::expected<UserQuery, Error> run_user_query(NoteStore& store, std::string_view cypher, QueryAction action,
                                               const graph::QueryLimits& limits, const graph::Params& params) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    if (action == QueryAction::Read)
        return read_locked(impl, cypher, limits, params);
    return write_locked(impl, cypher, action == QueryAction::Write, limits, params);
}

std::expected<UserQuery, Error> run_saved_view(const NoteStore& store, std::string_view name) {
    NoteStore::Impl& impl = store.impl();
    const auto deadline = deadline_after(impl.view_limits.max_duration);  // görünüm profili
    auto result = run_view(store, name);
    if (!result)
        return std::unexpected(result.error());
    const std::scoped_lock lock(impl.mutex);
    UserQuery query{std::move(*result), {}, {}, {}};
    const auto snapshot = impl.current();
    if (!snapshot)
        return query;  // görünüm çalıştıysa depo vardı; arada okunamazsa adsız kalır
    NOTEAPP_TRY(name_elements(impl, snapshot.get(), query, deadline));
    return query;
}

}  // namespace noteapp::core
