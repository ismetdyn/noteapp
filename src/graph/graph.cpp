#include "graph/graph.hpp"

#include "graph/cypher/query_runner.hpp"
#include "graph/merge.hpp"
#include "graph/op.hpp"
#include "graph/oplog.hpp"
#include "graph/store.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace noteapp::graph {

using detail::Op;
using detail::Statement;
using detail::StatementId;
using detail::Store;

namespace {

Error invalid_argument(std::string reason) {
    return Error{Error::Code::InvalidArgument, "graph.invalid_argument", {{"reason", std::move(reason)}}};
}

Error not_found(std::string what) {
    return Error{Error::Code::NotFound, "graph.not_found", {{"what", std::move(what)}}};
}

Error corrupt(std::string reason) {
    return Error{Error::Code::Corrupt, "graph.corrupt", {{"reason", std::move(reason)}}};
}

Result<void> require_open(const Store* store) {
    if (store == nullptr)
        return std::unexpected(invalid_argument("txn_finished"));
    return {};
}

template <class Id> Result<Id> id_from(std::span<const std::byte> blob) {
    Id id;
    if (blob.size() != id.bytes.size())
        return std::unexpected(corrupt("bad_id"));
    std::ranges::copy(blob, id.bytes.begin());
    return id;
}

std::span<const std::byte> owner_bytes(const OwnerId& owner) {
    if (const NodeId* node = std::get_if<NodeId>(&owner))
        return node->bytes;
    return std::get<EdgeId>(owner).bytes;
}

Result<bool> row_exists(Store& store, StatementId statement_id, std::span<const std::byte> id) {
    Result<Statement> statement = store.statement(statement_id);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, id);
    return statement->step();
}

// Yoksa NotFound; depolama hatası olduğu gibi çıkar.
Result<void> require_row(Store& store, StatementId statement_id, std::span<const std::byte> id,
                         std::string_view what) {
    const Result<bool> exists = row_exists(store, statement_id, id);
    if (!exists)
        return std::unexpected(exists.error());
    if (!*exists)
        return std::unexpected(not_found(std::string(what)));
    return {};
}

Result<void> require_node(Store& store, const NodeId& id) {
    return require_row(store, StatementId::NodeGet, id.bytes, "node");
}

Result<void> require_owner(Store& store, const OwnerId& owner) {
    if (const NodeId* node = std::get_if<NodeId>(&owner))
        return require_node(store, *node);
    return require_row(store, StatementId::EdgeGet, std::get<EdgeId>(owner).bytes, "edge");
}

Result<void> collect_incident_edges(Store& store, StatementId statement_id, const NodeId& id,
                                    std::vector<EdgeId>& out) {
    Result<Statement> statement = store.statement(statement_id);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, id.bytes);
    while (true) {
        const Result<bool> row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return {};
        const Result<EdgeId> edge = id_from<EdgeId>(statement->column_blob(0));
        if (!edge)
            return std::unexpected(edge.error());
        out.push_back(*edge);
    }
}

// Aynı kimlikli kenarın ucu ve tipi değişemez: kimlik kenarın kendisidir.
Result<void> require_same_edge_if_exists(Store& store, const EdgeId& id, const NodeId& src,
                                         std::string_view type, const NodeId& dst) {
    Result<Statement> statement = store.statement(StatementId::EdgeGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, id.bytes);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return {};
    const bool same = std::ranges::equal(statement->column_blob(0), std::span<const std::byte>(src.bytes)) &&
                      statement->column_text(1) == type &&
                      std::ranges::equal(statement->column_blob(2), std::span<const std::byte>(dst.bytes));
    if (!same)
        return std::unexpected(invalid_argument("edge_identity_changed"));
    return {};
}

// PropVersions satırlarından en büyük (hlc, cihaz) head; kaybedenler çözülmez (büyük değerli kaybeden
// yüklenmez). `stored` boşsa head yok.
struct Winner {
    detail::DeviceId device{};
    std::uint64_t seq = 0;
    std::uint64_t hlc = 0;
    std::string stored;  // kısa değerde yığın ayırmaz: bu yol sorgu başına on binlerce kez koşar
};

Result<Winner> winning_head(Statement& statement) {
    Winner best;
    while (true) {
        const Result<bool> row = statement.step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return best;
        const auto hlc = static_cast<std::uint64_t>(statement.column_int(2));
        const std::span<const std::byte> device = statement.column_blob(0);
        if (device.size() != best.device.size())
            return std::unexpected(corrupt("bad_device"));
        if (best.stored.empty() || hlc > best.hlc ||
            (hlc == best.hlc && std::ranges::lexicographical_compare(best.device, device))) {
            best.hlc = hlc;
            best.seq = static_cast<std::uint64_t>(statement.column_int(1));
            std::ranges::copy(device, best.device.begin());
            const std::span<const std::byte> stored = statement.column_blob(3);
            best.stored.assign(reinterpret_cast<const char*>(stored.data()), stored.size());
        }
    }
}

}  // namespace

struct NeighborCursor::Impl {
    Store* store;
    NodeId self;
    Direction direction{};
    std::optional<std::string> type;
    std::optional<Statement> statement;
    bool scanning_in = false;

    Result<void> start() {
        const bool in = scanning_in || direction == Direction::In;
        StatementId id = in ? StatementId::EdgesIn : StatementId::EdgesOut;
        if (type)
            id = in ? StatementId::EdgesInOfType : StatementId::EdgesOutOfType;
        Result<Statement> opened = store->statement(id);
        if (!opened)
            return std::unexpected(opened.error());
        statement = std::move(*opened);
        statement->bind_blob(1, self.bytes);
        if (type)
            statement->bind_text(2, *type);
        return {};
    }

    // Çıkan tarama bitti: Both'ta giren taramaya geçer. false: gezilecek kenar kalmadı.
    Result<bool> advance_phase() {
        statement.reset();
        if (direction != Direction::Both || scanning_in)
            return false;
        scanning_in = true;
        const Result<void> started = start();
        if (!started)
            return std::unexpected(started.error());
        return true;
    }

    Result<std::optional<Neighbor>> next() {
        while (statement) {
            const Result<bool> row = statement->step();
            if (!row)
                return std::unexpected(row.error());
            if (!*row) {
                const Result<bool> more = advance_phase();
                if (!more)
                    return std::unexpected(more.error());
                continue;
            }
            const Result<EdgeId> edge = id_from<EdgeId>(statement->column_blob(0));
            const Result<NodeId> other = id_from<NodeId>(statement->column_blob(2));
            if (!edge || !other)
                return std::unexpected(corrupt("bad_edge_row"));
            // Öz döngü çıkan taramada verildi.
            if (scanning_in && *other == self)
                continue;
            return Neighbor{*edge, *other, statement->column_text(1)};
        }
        return std::nullopt;
    }
};

struct NodeCursor::Impl {
    explicit Impl(Statement opened) : statement(std::move(opened)) {}
    std::optional<Statement> statement;
    std::string label;
};

NeighborCursor::NeighborCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NeighborCursor::NeighborCursor(NeighborCursor&&) noexcept = default;
NeighborCursor& NeighborCursor::operator=(NeighborCursor&&) noexcept = default;
NeighborCursor::~NeighborCursor() = default;

Result<std::optional<Neighbor>> NeighborCursor::next() {
    return impl_->next();
}

NodeCursor::NodeCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NodeCursor::NodeCursor(NodeCursor&&) noexcept = default;
NodeCursor& NodeCursor::operator=(NodeCursor&&) noexcept = default;
NodeCursor::~NodeCursor() = default;

Result<std::optional<NodeId>> NodeCursor::next() {
    if (!impl_->statement)
        return std::nullopt;
    const Result<bool> row = impl_->statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row) {
        impl_->statement.reset();
        return std::nullopt;
    }
    const Result<NodeId> id = id_from<NodeId>(impl_->statement->column_blob(0));
    if (!id)
        return std::unexpected(id.error());
    return *id;
}

ReadTxn::ReadTxn(detail::Store& store) : store_(&store) {}

ReadTxn::ReadTxn(ReadTxn&& other) noexcept : store_(std::exchange(other.store_, nullptr)) {}

ReadTxn& ReadTxn::operator=(ReadTxn&& other) noexcept {
    if (this != &other) {
        if (store_ != nullptr)
            store_->end_txn();
        store_ = std::exchange(other.store_, nullptr);
    }
    return *this;
}

ReadTxn::~ReadTxn() {
    if (store_ != nullptr)
        store_->end_txn();
}

Result<bool> ReadTxn::has_node(NodeId id) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    return row_exists(*store_, StatementId::NodeGet, id.bytes);
}

Result<std::optional<EdgeEnds>> ReadTxn::edge(EdgeId id) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::EdgeGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, id.bytes);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return std::optional<EdgeEnds>{};
    const Result<NodeId> src = id_from<NodeId>(statement->column_blob(0));
    if (!src)
        return std::unexpected(src.error());
    const Result<NodeId> dst = id_from<NodeId>(statement->column_blob(2));
    if (!dst)
        return std::unexpected(dst.error());
    return std::optional<EdgeEnds>{EdgeEnds{*src, std::string(statement->column_text(1)), *dst}};
}

Result<std::vector<std::string>> ReadTxn::labels(NodeId id) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::LabelsOfNode);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, id.bytes);
    std::vector<std::string> labels;
    while (true) {
        const Result<bool> row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return labels;
        labels.emplace_back(statement->column_text(0));
    }
}

Result<NodeCursor> ReadTxn::nodes() {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::NodesAll);
    if (!statement)
        return std::unexpected(statement.error());
    return NodeCursor(std::make_unique<NodeCursor::Impl>(std::move(*statement)));
}

Result<NodeCursor> ReadTxn::nodes_with_label(std::string_view label) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    if (const Result<void> valid = detail::validate_name(label); !valid)
        return std::unexpected(valid.error());
    Result<Statement> statement = store_->statement(StatementId::LabelScan);
    if (!statement)
        return std::unexpected(statement.error());
    // Tarama cursor kapanana dek sürer; etiket metni çağıran tarafta ölebilir, kopyası step boyunca yaşamalı.
    auto impl = std::make_unique<NodeCursor::Impl>(std::move(*statement));
    impl->label = std::string(label);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Impl kurucusu statement'ı doldurur
    impl->statement->bind_text(1, impl->label);
    return NodeCursor(std::move(impl));
}

Result<NeighborCursor> ReadTxn::neighbors(NodeId id, Direction direction,
                                          std::optional<std::string_view> type) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    if (type)
        if (const Result<void> valid = detail::validate_name(*type); !valid)
            return std::unexpected(valid.error());
    auto impl = std::make_unique<NeighborCursor::Impl>();
    impl->store = store_;
    impl->self = id;
    impl->direction = direction;
    if (type)
        impl->type = std::string(*type);
    if (const Result<void> started = impl->start(); !started)
        return std::unexpected(started.error());
    return NeighborCursor(std::move(impl));
}

Result<Value> ReadTxn::property(OwnerId owner, std::string_view key) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::PropVersions);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, owner_bytes(owner)).bind_text(2, key);
    const Result<Winner> best = winning_head(*statement);
    if (!best)
        return std::unexpected(best.error());
    if (best->stored.empty())
        return Value{};
    return store_->load_value(std::as_bytes(std::span(best->stored.data(), best->stored.size())));
}

Result<std::optional<PropertyHead>> ReadTxn::property_head(OwnerId owner, std::string_view key) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::PropVersions);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, owner_bytes(owner)).bind_text(2, key);
    const Result<Winner> best = winning_head(*statement);
    if (!best)
        return std::unexpected(best.error());
    if (best->stored.empty())
        return std::nullopt;
    Result<Store::StoredValue> stored =
        store_->peek_value(std::as_bytes(std::span(best->stored.data(), best->stored.size())));
    if (!stored)
        return std::unexpected(stored.error());
    PropertyHead head;
    head.device = best->device;
    head.seq = best->seq;
    head.hlc = best->hlc;
    head.value = std::move(stored->value);
    head.key = std::string(key);
    head.size = stored->size;
    head.large = stored->large;
    return head;
}

Result<std::vector<Version>> ReadTxn::versions(OwnerId owner, std::string_view key) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::PropVersions);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, owner_bytes(owner)).bind_text(2, key);
    std::vector<Version> versions;
    while (true) {
        const Result<bool> row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return versions;
        Version version;
        const std::span<const std::byte> device = statement->column_blob(0);
        if (device.size() != version.device.size())
            return std::unexpected(corrupt("bad_device"));
        std::ranges::copy(device, version.device.begin());
        version.seq = static_cast<std::uint64_t>(statement->column_int(1));
        version.hlc = static_cast<std::uint64_t>(statement->column_int(2));
        Result<Value> value = store_->load_value(statement->column_blob(3));
        if (!value)
            return std::unexpected(value.error());
        version.value = std::move(*value);
        versions.push_back(std::move(version));
    }
}

Result<std::vector<PropertyHead>> ReadTxn::properties(OwnerId owner) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::PropsOfOwner);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, owner_bytes(owner));
    std::vector<PropertyHead> heads;
    while (true) {
        const Result<bool> row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return heads;
        PropertyHead head;
        head.key = std::string(statement->column_text(0));
        const std::span<const std::byte> device = statement->column_blob(1);
        if (device.size() != head.device.size())
            return std::unexpected(corrupt("bad_device"));
        std::ranges::copy(device, head.device.begin());
        head.seq = static_cast<std::uint64_t>(statement->column_int(2));
        head.hlc = static_cast<std::uint64_t>(statement->column_int(3));
        Result<Store::StoredValue> stored = store_->peek_value(statement->column_blob(4));
        if (!stored)
            return std::unexpected(stored.error());
        head.value = std::move(stored->value);
        head.size = stored->size;
        head.large = stored->large;
        heads.push_back(std::move(head));
    }
}

Result<std::vector<PropertyHead>> ReadTxn::properties_at(const Version& write, OwnerId owner) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    Result<Statement> statement = store_->statement(StatementId::OpGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, write.device).bind_int(2, static_cast<std::int64_t>(write.seq));
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    std::vector<PropertyHead> found;
    if (!*row)
        return found;
    Result<detail::Frame> frame = detail::decode_frame(statement->column_blob(0), detail::kStoredFrameLimits);
    if (!frame)
        return std::unexpected(corrupt("bad_stored_frame"));
    for (detail::Op& op : frame->ops) {
        detail::PropSet* set = std::get_if<detail::PropSet>(&op);
        if (set == nullptr || set->owner != owner)
            continue;
        PropertyHead head;
        head.device = write.device;
        head.seq = write.seq;
        head.hlc = frame->hlc;
        head.key = std::move(set->key);
        if (const std::string* text = std::get_if<std::string>(&set->value))
            head.size = text->size();
        else if (const Bytes* bytes = std::get_if<Bytes>(&set->value))
            head.size = bytes->data.size();
        head.value = std::move(set->value);
        found.push_back(std::move(head));
    }
    return found;
}

Result<bool> ReadTxn::saw(const Version& later, const Version& earlier) {
    if (const Result<void> open = require_open(store_); !open)
        return std::unexpected(open.error());
    if (later.device == earlier.device)  // kendi cihazından seq-1 örtüktür
        return later.seq >= earlier.seq;
    Result<Statement> statement = store_->statement(StatementId::OpGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, later.device).bind_int(2, static_cast<std::int64_t>(later.seq));
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return false;
    const Result<detail::Frame> frame =
        detail::decode_frame(statement->column_blob(0), detail::kStoredFrameLimits);
    if (!frame)
        return std::unexpected(corrupt("bad_stored_frame"));
    return std::ranges::any_of(frame->deps, [&](const detail::Dot& dep) {
        return dep.dev == earlier.device && dep.seq >= earlier.seq;
    });
}

Result<void> WriteTxn::put_node(NodeId id) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    return store_->record(detail::NodePut{id});
}

Result<void> WriteTxn::delete_node(NodeId id, bool detach) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> exists = require_node(*store_, id); !exists)
        return exists;
    std::vector<EdgeId> edges;
    if (const Result<void> out = collect_incident_edges(*store_, StatementId::EdgesOut, id, edges); !out)
        return out;
    if (const Result<void> in = collect_incident_edges(*store_, StatementId::EdgesIn, id, edges); !in)
        return in;
    if (!edges.empty() && !detach)
        return std::unexpected(invalid_argument("node_has_edges"));
    std::ranges::sort(edges);
    const auto duplicates = std::ranges::unique(edges);  // öz döngü iki yönde de görünür
    edges.erase(duplicates.begin(), duplicates.end());
    for (const EdgeId& edge : edges)
        if (const Result<void> removed = store_->record(detail::EdgeDel{edge}); !removed)
            return removed;
    return store_->record(detail::NodeDel{id});
}

Result<void> WriteTxn::add_label(NodeId id, std::string_view label) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> valid = detail::validate_name(label); !valid)
        return valid;
    if (const Result<void> exists = require_node(*store_, id); !exists)
        return exists;
    return store_->record(detail::LabelAdd{id, std::string(label)});
}

Result<void> WriteTxn::remove_label(NodeId id, std::string_view label) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> valid = detail::validate_name(label); !valid)
        return valid;
    if (const Result<void> exists = require_node(*store_, id); !exists)
        return exists;
    {
        Result<Statement> statement = store_->statement(StatementId::LabelGet);
        if (!statement)
            return std::unexpected(statement.error());
        statement->bind_blob(1, id.bytes).bind_text(2, label);
        const Result<bool> has_label = statement->step();
        if (!has_label)
            return std::unexpected(has_label.error());
        // Olmayan etiketi kaldırmak Cypher REMOVE gibi boştur; günlüğe boş işlem yazılmaz.
        if (!*has_label)
            return {};
    }
    return store_->record(detail::LabelDel{id, std::string(label)});
}

Result<void> WriteTxn::put_edge(EdgeId id, NodeId src, std::string_view type, NodeId dst) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> valid = detail::validate_name(type); !valid)
        return valid;
    if (const Result<void> exists = require_node(*store_, src); !exists)
        return exists;
    if (const Result<void> exists = require_node(*store_, dst); !exists)
        return exists;
    if (const Result<void> same = require_same_edge_if_exists(*store_, id, src, type, dst); !same)
        return same;
    return store_->record(detail::EdgePut{id, src, std::string(type), dst});
}

Result<void> WriteTxn::delete_edge(EdgeId id) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> exists = require_row(*store_, StatementId::EdgeGet, id.bytes, "edge"); !exists)
        return exists;
    return store_->record(detail::EdgeDel{id});
}

Result<void> WriteTxn::set_property(OwnerId owner, std::string_view key, Value value) {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    if (const Result<void> valid = detail::validate_name(key); !valid)
        return valid;
    if (const Result<void> valid = detail::validate_value(value); !valid)
        return valid;
    if (const Result<void> exists = require_owner(*store_, owner); !exists)
        return exists;
    return store_->record(detail::PropSet{owner, std::string(key), std::move(value)});
}

Result<void> WriteTxn::set_lowest_priority() {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    return store_->set_lowest_priority();
}

std::size_t WriteTxn::pending_ops() const {
    return store_ == nullptr ? 0 : store_->pending_ops();
}

Result<void> WriteTxn::commit() {
    if (const Result<void> open = require_open(store_); !open)
        return open;
    // Sonuç ne olursa olsun işlem biter: başarısızsa Store geri almıştır.
    return std::exchange(store_, nullptr)->commit_write();
}

Graph::Graph(std::unique_ptr<detail::Store> store) : store_(std::move(store)) {}
Graph::Graph(Graph&&) noexcept = default;
Graph& Graph::operator=(Graph&&) noexcept = default;
Graph::~Graph() = default;

Result<Graph> Graph::open(const std::filesystem::path& path, const GraphLimits& limits) {
    Result<std::unique_ptr<Store>> store = Store::open(path, limits);
    if (!store)
        return std::unexpected(store.error());
    return Graph(std::move(*store));
}

Result<ReadTxn> Graph::read() {
    if (store_->has_open_txn())
        return std::unexpected(invalid_argument("txn_already_open"));
    if (const Result<void> begun = store_->begin_read(); !begun)
        return std::unexpected(begun.error());
    return ReadTxn(*store_);
}

Result<WriteTxn> Graph::write() {
    if (store_->has_open_txn())
        return std::unexpected(invalid_argument("txn_already_open"));
    if (const Result<void> begun = store_->begin_write(); !begun)
        return std::unexpected(begun.error());
    return WriteTxn(*store_);
}

// API istisnasızdır. Bellek bitince yığın sarmalanır (işlem geri alınır) ve ayırmasız hata döner.
Result<ExportReport> Graph::export_log(const std::filesystem::path& directory) {
    if (store_->has_open_txn())
        return std::unexpected(invalid_argument("txn_already_open"));
    try {
        return detail::export_log(*store_, directory);
    } catch (const std::bad_alloc&) {
        return std::unexpected(detail::out_of_memory());
    }
}

Result<MergeReport> Graph::ingest_logs(std::span<const std::filesystem::path> logs) {
    if (store_->has_open_txn())
        return std::unexpected(invalid_argument("txn_already_open"));
    try {
        return detail::ingest_logs(*store_, logs, detail::ingest_limits_of(store_->limits()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(detail::out_of_memory());
    }
}

std::array<std::byte, 16> Graph::device() const {
    return store_->device();
}

Result<std::uint64_t> Graph::data_version() {
    return store_->data_version();
}

Result<std::uint64_t> Graph::last_seq() {
    return store_->applied_seq(store_->device());
}

std::size_t Graph::max_frame_bytes() const {
    return store_->frame_limits().bytes;
}

std::size_t Graph::max_stored_bytes() const {
    return store_->max_blob_bytes();
}

Result<QueryResult>
Graph::query(std::string_view cypher, const Params& params, QueryMode mode,
             QueryLimits limits,  // cppcheck-suppress passedByValue ; küçük, kopyası ucuz
             const QueryHooks& hooks) {
    return detail::cypher::run_query(*this, *store_, cypher, params, mode, limits, hooks);
}

Result<void> check_query(std::string_view cypher, QueryMode mode, const Params& params,
                         const QueryLimits& limits) {
    return detail::cypher::check_query(cypher, params, mode, limits);
}

}  // namespace noteapp::graph
