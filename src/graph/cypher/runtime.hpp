#pragma once

// Bir sorgunun yürütme durumu: bütçe (limitler) ve sorgu içi önbellek.
#include "graph/cypher/ast.hpp"
#include "graph/error.hpp"
#include "graph/graph.hpp"
#include "graph/id.hpp"
#include "graph/query.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Result zincirinde erken dönüş: dilin `?` işleci yok; her çağrıda aynı üç satır okunurluğu öldürür.
#define CYPHER_TRY(expression)                                                                               \
    do {                                                                                                     \
        if (auto cypher_try_result = (expression); !cypher_try_result)                                       \
            return std::unexpected(std::move(cypher_try_result).error());                                    \
    } while (false)

namespace noteapp::graph::detail {
class Store;
}

namespace noteapp::graph::detail::cypher {

using Row = std::vector<QueryValue>;

// Bir kenar deseninin tip kümesi. Boşsa her tip. `id`, önbellek anahtarıdır.
struct TypeSet {
    std::uint32_t id = 0;
    std::vector<std::string> types;
};

struct Adjacent {
    EdgeId edge;
    NodeId node;
};

using AdjacencyRef = std::shared_ptr<const std::vector<Adjacent>>;
using LabelsRef = std::shared_ptr<const std::vector<std::string>>;

Error located_error(Error::Code code, std::string key, Position pos,
                    std::vector<std::pair<std::string, std::string>> args = {});
Error limit_error(std::string limit);

class Budget {
public:
    explicit Budget(const QueryLimits& limits);

    Result<void> tick() {
        if ((++ticks_ & kTicksPerClockCheck) == 0)
            return check_clock();
        return {};
    }
    Result<void> expansion() {
        if (++expansions_ > limits_.max_expansions)
            return std::unexpected(limit_error("expansions"));
        return tick();
    }
    Result<void> charge(std::size_t bytes);
    void release(std::size_t bytes) { memory_ -= bytes; }
    // Henüz tutulmayan geçici değer: kopyalanmadan önce sorulur.
    bool would_exceed(std::size_t bytes) const { return memory_ + bytes > limits_.max_memory_bytes; }
    std::size_t max_expansions() const { return limits_.max_expansions; }
    std::size_t max_rows() const { return limits_.max_rows; }

private:
    static constexpr std::uint64_t kTicksPerClockCheck = 1023;  // saat okuması satır başına pahalı olurdu
    Result<void> check_clock() const;

    QueryLimits limits_;
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t ticks_ = 0;
    std::size_t expansions_ = 0;
    std::size_t memory_ = 0;
};

// Sorgu içi önbellek: iki atlamalı sorguda aynı düğümün komşuluğu ve özelliği binlerce kez istenir; depoya
// her seferinde inmek 50 ms kapısını geçemez (tasks/graph-design.md Risk 1). Tek anlık görüntüde geçerlidir,
// yazan sorguda kapalıdır (bütçe 0). Bütçe dolunca yalnız yeni giriş eklenmez: sonuç aynı kalır.
class QueryMemo {
public:
    explicit QueryMemo(std::size_t budget_bytes) : budget_(budget_bytes) {}

    Result<AdjacencyRef> adjacency(ReadTxn& txn, const NodeId& id, Direction direction, const TypeSet& types,
                                   std::size_t max_entries);
    Result<QueryValue> property(ReadTxn& txn, const OwnerId& owner, int key_id, std::string_view key);
    Result<LabelsRef> labels(ReadTxn& txn, const NodeId& id);
    std::size_t used_bytes() const { return used_; }

private:
    struct Key {
        std::array<std::byte, 16> id;
        std::uint32_t sub;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            // Kimliğin son 8 baytı rastgele (UUIDv7) veya özettir (UUIDv8).
            std::uint64_t value = 0;
            std::memcpy(&value, key.id.data() + 8, sizeof value);
            return static_cast<std::size_t>(value ^ (std::uint64_t{key.sub} * 0x9E3779B97F4A7C15ULL));
        }
    };

    // Doluysa false: çağıran değeri tutmaz, yalnız bu seferlik kullanır.
    bool reserve(std::size_t bytes);

    std::size_t budget_;
    std::size_t used_ = 0;
    std::unordered_map<Key, AdjacencyRef, KeyHash> adjacency_;
    std::unordered_map<Key, QueryValue, KeyHash> properties_;
    std::unordered_map<Key, LabelsRef, KeyHash> labels_;
};

// Satırdan bağımsız yürütme durumu. `write` yalnız yazan planda doludur.
struct ExecContext {
    ExecContext(ReadTxn& read_txn, WriteTxn* write_txn, Store& store_ref, const QueryLimits& limits,
                std::size_t memo_budget)
        : read(read_txn), write(write_txn), store(store_ref), budget(limits), memo(memo_budget) {}

    ReadTxn& read;
    WriteTxn* write;
    Store& store;
    Budget budget;
    QueryMemo memo;
};

}  // namespace noteapp::graph::detail::cypher
