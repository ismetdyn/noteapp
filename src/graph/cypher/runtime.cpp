#include "graph/cypher/runtime.hpp"

#include "graph/cypher/expr.hpp"

#include <optional>

namespace noteapp::graph::detail::cypher {

namespace {

// Tahmini sabit maliyet: bellek bütçesi için kesin ölçü gerekmez, büyüklük mertebesi yeter.
constexpr std::size_t kMemoEntryBytes = 96;
// Büyük değer önbelleğe girmez: tek bir 10 MiB gövde bütçeyi boşa harcamasın; zaten satır başına okunmaz.
constexpr std::size_t kMaxMemoValueBytes = 4096;

Result<void> read_adjacency(ReadTxn& txn, const NodeId& id, Direction direction,
                            std::optional<std::string_view> type, std::size_t max_entries,
                            std::vector<Adjacent>& out) {
    Result<NeighborCursor> cursor = txn.neighbors(id, direction, type);
    if (!cursor)
        return std::unexpected(std::move(cursor).error());
    while (true) {
        const Result<std::optional<Neighbor>> neighbor = cursor->next();
        if (!neighbor)
            return std::unexpected(neighbor.error());
        if (!*neighbor)
            return {};
        // Tek düğümün dev komşuluğu bütçeyi önbellekten önce tüketmesin.
        if (out.size() >= max_entries)
            return std::unexpected(limit_error("expansions"));
        out.push_back(Adjacent{(*neighbor)->edge, (*neighbor)->node});
    }
}

const std::array<std::byte, 16>& owner_bytes(const OwnerId& owner) {
    if (const NodeId* node = std::get_if<NodeId>(&owner))
        return node->bytes;
    return std::get<EdgeId>(owner).bytes;
}

}  // namespace

Error located_error(Error::Code code, std::string key, Position pos,
                    std::vector<std::pair<std::string, std::string>> args) {
    args.emplace_back("line", std::to_string(pos.line));
    args.emplace_back("col", std::to_string(pos.col));
    return Error{code, std::move(key), std::move(args)};
}

Error limit_error(std::string limit) {
    return Error{Error::Code::LimitExceeded, "graph.limit_exceeded", {{"limit", std::move(limit)}}};
}

Budget::Budget(const QueryLimits& limits)
    : limits_(limits), deadline_(std::chrono::steady_clock::now() + limits.max_duration) {}

Result<void> Budget::check_clock() const {
    if (std::chrono::steady_clock::now() > deadline_)
        return std::unexpected(limit_error("duration"));
    return {};
}

Result<void> Budget::charge(std::size_t bytes) {
    memory_ += bytes;
    if (memory_ > limits_.max_memory_bytes)
        return std::unexpected(limit_error("memory"));
    return tick();
}

bool QueryMemo::reserve(std::size_t bytes) {
    if (used_ + bytes > budget_)
        return false;
    used_ += bytes;
    return true;
}

Result<AdjacencyRef> QueryMemo::adjacency(ReadTxn& txn, const NodeId& id, Direction direction,
                                          const TypeSet& types, std::size_t max_entries) {
    const Key key{id.bytes, types.id * 3U + static_cast<std::uint32_t>(direction)};
    if (const auto found = adjacency_.find(key); found != adjacency_.end())
        return found->second;

    auto list = std::make_shared<std::vector<Adjacent>>();
    if (types.types.empty()) {
        CYPHER_TRY(read_adjacency(txn, id, direction, std::nullopt, max_entries, *list));
    } else {
        for (const std::string& type : types.types)
            CYPHER_TRY(read_adjacency(txn, id, direction, type, max_entries, *list));
    }
    AdjacencyRef shared = std::move(list);
    if (reserve(kMemoEntryBytes + shared->size() * sizeof(Adjacent)))
        adjacency_.emplace(key, shared);
    return shared;
}

Result<QueryValue> QueryMemo::property(ReadTxn& txn, const OwnerId& owner, int key_id, std::string_view key) {
    const Key memo_key{owner_bytes(owner), static_cast<std::uint32_t>(key_id)};
    if (const auto found = properties_.find(memo_key); found != properties_.end())
        return found->second;

    Result<Value> stored = txn.property(owner, key);
    if (!stored)
        return std::unexpected(std::move(stored).error());
    QueryValue value = from_stored(std::move(*stored));
    const std::size_t cost = value_bytes(value);
    if (cost <= kMaxMemoValueBytes && reserve(kMemoEntryBytes + cost))
        properties_.emplace(memo_key, value);
    return value;
}

Result<LabelsRef> QueryMemo::labels(ReadTxn& txn, const NodeId& id) {
    const Key key{id.bytes, 0};
    if (const auto found = labels_.find(key); found != labels_.end())
        return found->second;

    Result<std::vector<std::string>> read = txn.labels(id);
    if (!read)
        return std::unexpected(std::move(read).error());
    std::size_t cost = kMemoEntryBytes;
    for (const std::string& label : *read)
        cost += sizeof(std::string) + label.size();
    LabelsRef shared = std::make_shared<const std::vector<std::string>>(std::move(*read));
    if (reserve(cost))
        labels_.emplace(key, shared);
    return shared;
}

}  // namespace noteapp::graph::detail::cypher
