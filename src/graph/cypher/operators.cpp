#include "graph/cypher/operators.hpp"

#include "graph/cypher/expr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noteapp::graph::detail::cypher {

namespace {

// Bir önceki satırın kaynağı biterken çağrılır: `source` bir düğüm değilse hata, null ise gezilecek kenar
// yoktur.
Result<std::optional<NodeId>> source_node(const QueryValue& source, Position pos) {
    if (source.is_null())
        return std::optional<NodeId>{};
    if (const auto* node = std::get_if<NodeId>(&source.v))
        return std::optional<NodeId>(*node);
    return std::unexpected(located_error(Error::Code::InvalidArgument, "graph.cypher_bad_target", pos));
}

bool edge_in_path(const QueryValue& path, const EdgeId& edge) {
    const auto* list = std::get_if<QueryList>(&path.v);
    if (list == nullptr)
        return false;
    return std::ranges::any_of(list->items, [&](const QueryValue& item) {
        const auto* other = std::get_if<EdgeId>(&item.v);
        return other != nullptr && *other == edge;
    });
}

// Aynı MATCH'te önceden bağlanmış kenar yeniden kullanılamaz (openCypher).
bool edge_already_used(const ExpandSpec& spec, const Row& row, const EdgeId& edge) {
    for (const int slot : spec.earlier_edges) {
        const auto* other = std::get_if<EdgeId>(&row[static_cast<std::size_t>(slot)].v);
        if (other != nullptr && *other == edge)
            return true;
    }
    return std::ranges::any_of(spec.earlier_paths, [&](int slot) {
        return edge_in_path(row[static_cast<std::size_t>(slot)], edge);
    });
}

bool destination_matches(const ExpandSpec& spec, const Row& row, const NodeId& node) {
    if (!spec.dst_bound)
        return true;
    const auto* bound = std::get_if<NodeId>(&row[static_cast<std::size_t>(spec.dst)].v);
    return bound != nullptr && *bound == node;
}

Result<AdjacencyRef> adjacency_of(ExecContext& context, const ExpandSpec& spec, const NodeId& node) {
    return context.memo.adjacency(context.read, node, spec.direction, *spec.types,
                                  context.budget.max_expansions());
}

class ScanOp final : public PipelineOp {
public:
    ScanOp(OperatorPtr child, std::string name, int slot, std::optional<std::string> label)
        : PipelineOp(std::move(name), std::move(child)), slot_(slot), label_(std::move(label)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            if (!cursor_) {
                const Result<bool> more = open(row, context);
                if (!more || !*more)
                    return more;
            }
            CYPHER_TRY(context.budget.tick());
            const Result<std::optional<NodeId>> id = cursor_->next();
            if (!id)
                return std::unexpected(id.error());
            if (*id) {
                row[static_cast<std::size_t>(slot_)] = **id;
                return true;
            }
            cursor_.reset();
        }
    }

private:
    Result<bool> open(Row& row, ExecContext& context) {
        const Result<bool> more = child_->next(row, context);
        if (!more || !*more)
            return more;
        Result<NodeCursor> cursor = label_ ? context.read.nodes_with_label(*label_) : context.read.nodes();
        if (!cursor)
            return std::unexpected(std::move(cursor).error());
        cursor_.emplace(std::move(*cursor));
        return true;
    }

    int slot_;
    std::optional<std::string> label_;
    std::optional<NodeCursor> cursor_;
};

class IdSeekOp final : public PipelineOp {
public:
    IdSeekOp(OperatorPtr child, std::string name, int slot, const Expr* id)
        : PipelineOp(std::move(name), std::move(child)), slot_(slot), id_(id) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more || !*more)
                return more;
            CYPHER_TRY(context.budget.tick());
            const Result<std::optional<NodeId>> found = seek(row, context);
            if (!found)
                return std::unexpected(found.error());
            if (*found) {
                row[static_cast<std::size_t>(slot_)] = **found;
                return true;
            }
        }
    }

private:
    Result<std::optional<NodeId>> seek(const Row& row, ExecContext& context) {
        const Result<QueryValue> value = eval(*id_, row, context);
        if (!value)
            return std::unexpected(value.error());
        const auto* text = std::get_if<std::string>(&value->v);
        if (text == nullptr)
            return std::optional<NodeId>{};
        const auto bytes = id_from_hex(*text);
        if (!bytes)
            return std::optional<NodeId>{};
        const NodeId id{*bytes};
        const Result<bool> exists = context.read.has_node(id);
        if (!exists)
            return std::unexpected(exists.error());
        return *exists ? std::optional<NodeId>(id) : std::nullopt;
    }

    int slot_;
    const Expr* id_;
};

class CheckNodeOp final : public PipelineOp {
public:
    CheckNodeOp(OperatorPtr child, int slot, Position pos)
        : PipelineOp("CheckNode", std::move(child)), slot_(slot), pos_(pos) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more || !*more)
                return more;
            const Result<std::optional<NodeId>> node =
                source_node(row[static_cast<std::size_t>(slot_)], pos_);
            if (!node)
                return std::unexpected(node.error());
            if (*node)
                return true;
        }
    }

private:
    int slot_;
    Position pos_;
};

class ExpandOp final : public PipelineOp {
public:
    ExpandOp(OperatorPtr child, ExpandSpec spec)
        : PipelineOp(spec.name, std::move(child)), spec_(std::move(spec)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            if (!list_ || index_ >= list_->size()) {
                const Result<bool> more = load(row, context);
                if (!more || !*more)
                    return more;
                continue;
            }
            const Adjacent& adjacent = (*list_)[index_++];
            CYPHER_TRY(context.budget.expansion());
            if (edge_already_used(spec_, row, adjacent.edge) ||
                !destination_matches(spec_, row, adjacent.node))
                continue;
            row[static_cast<std::size_t>(spec_.edge)] = adjacent.edge;
            if (!spec_.dst_bound)
                row[static_cast<std::size_t>(spec_.dst)] = adjacent.node;
            return true;
        }
    }

private:
    Result<bool> load(Row& row, ExecContext& context) {
        list_.reset();
        index_ = 0;
        const Result<bool> more = child_->next(row, context);
        if (!more || !*more)
            return more;
        const Result<std::optional<NodeId>> source =
            source_node(row[static_cast<std::size_t>(spec_.src)], spec_.pos);
        if (!source)
            return std::unexpected(source.error());
        if (!*source)
            return true;  // null kaynak: bu satır eşleşmez
        Result<AdjacencyRef> list = adjacency_of(context, spec_, **source);
        if (!list)
            return std::unexpected(std::move(list).error());
        list_ = std::move(*list);
        return true;
    }

    ExpandSpec spec_;
    AdjacencyRef list_;
    std::size_t index_ = 0;
};

// DFS, kesin *min..max. "Ziyaret edilen" yalnız o anki yolun kenar kümesidir (openCypher trail): düğüm tekrar
// edebilir, kenar edemez. Bellek O(max); patlama expansion bütçesinde durur.
class VarLengthExpandOp final : public PipelineOp {
public:
    VarLengthExpandOp(OperatorPtr child, ExpandSpec spec)
        : PipelineOp(spec.name, std::move(child)), spec_(std::move(spec)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            if (stack_.empty()) {
                const Result<bool> more = start(row, context);
                if (!more || !*more)
                    return more;
                continue;
            }
            Frame& top = stack_.back();
            if (!top.list || top.index >= top.list->size()) {
                pop();
                continue;
            }
            const Adjacent adjacent = (*top.list)[top.index++];
            CYPHER_TRY(context.budget.expansion());
            if (std::ranges::find(path_, adjacent.edge) != path_.end() ||
                edge_already_used(spec_, row, adjacent.edge))
                continue;
            CYPHER_TRY(descend(adjacent, context));
            if (static_cast<int>(path_.size()) >= spec_.min_hops &&
                destination_matches(spec_, row, adjacent.node)) {
                emit(row, adjacent.node);
                return true;
            }
        }
    }

private:
    struct Frame {
        AdjacencyRef list;
        std::size_t index = 0;
    };

    Result<bool> start(Row& row, ExecContext& context) {
        const Result<bool> more = child_->next(row, context);
        if (!more || !*more)
            return more;
        const Result<std::optional<NodeId>> source =
            source_node(row[static_cast<std::size_t>(spec_.src)], spec_.pos);
        if (!source)
            return std::unexpected(source.error());
        if (!*source)
            return true;
        Result<AdjacencyRef> list = adjacency_of(context, spec_, **source);
        if (!list)
            return std::unexpected(std::move(list).error());
        stack_.push_back(Frame{std::move(*list), 0});
        return true;
    }

    // Azami derinlikte komşuluk okunmaz: son atlamanın hedefleri depoya inmeden biter.
    Result<void> descend(const Adjacent& adjacent, ExecContext& context) {
        path_.push_back(adjacent.edge);
        AdjacencyRef next_list;
        if (static_cast<int>(path_.size()) < spec_.max_hops) {
            Result<AdjacencyRef> list = adjacency_of(context, spec_, adjacent.node);
            if (!list)
                return std::unexpected(std::move(list).error());
            next_list = std::move(*list);
        }
        stack_.push_back(Frame{std::move(next_list), 0});
        return {};
    }

    void pop() {
        stack_.pop_back();
        if (!path_.empty())
            path_.pop_back();
    }

    void emit(Row& row, const NodeId& node) const {
        if (!spec_.dst_bound)
            row[static_cast<std::size_t>(spec_.dst)] = node;
        if (spec_.edge >= 0) {
            QueryList list;
            list.items.reserve(path_.size());
            for (const EdgeId& edge : path_)
                list.items.emplace_back(edge);
            row[static_cast<std::size_t>(spec_.edge)] = std::move(list);
        }
    }

    ExpandSpec spec_;
    std::vector<Frame> stack_;
    std::vector<EdgeId> path_;
};

class FilterOp final : public PipelineOp {
public:
    FilterOp(OperatorPtr child, const Expr* predicate)
        : PipelineOp("Filter", std::move(child)), predicate_(predicate) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more || !*more)
                return more;
            const Result<bool> passed = eval_predicate(*predicate_, row, context);
            if (!passed)
                return std::unexpected(passed.error());
            if (*passed)
                return true;
        }
    }

private:
    const Expr* predicate_;
};

class ProjectOp final : public PipelineOp {
public:
    ProjectOp(OperatorPtr child, std::vector<ProjectItem> items)
        : PipelineOp("Project", std::move(child)), items_(std::move(items)), costs_(items_.size(), 0) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        const Result<bool> more = child_->next(row, context);
        if (!more || !*more)
            return more;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            Result<QueryValue> value = eval(*items_[i].expr, row, context);
            if (!value)
                return std::unexpected(std::move(value).error());
            // Satırın yuvası da tutulan bellektir: WITH zincirinde her değer sınırın altında, toplamı değil.
            context.budget.release(std::exchange(costs_[i], 0));
            const std::size_t cost = value_bytes(*value);
            CYPHER_TRY(context.budget.charge(cost));
            costs_[i] = cost;
            row[static_cast<std::size_t>(items_[i].slot)] = std::move(*value);
        }
        return true;
    }

private:
    std::vector<ProjectItem> items_;
    std::vector<std::size_t> costs_;
};

class DistinctOp final : public PipelineOp {
public:
    DistinctOp(OperatorPtr child, std::vector<int> slots)
        : PipelineOp("Distinct", std::move(child)), slots_(std::move(slots)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more || !*more)
                return more;
            std::string key;
            for (const int slot : slots_)
                append_canonical(key, row[static_cast<std::size_t>(slot)]);
            const std::size_t cost = key.size() + sizeof(std::string);
            if (seen_.insert(std::move(key)).second) {
                CYPHER_TRY(context.budget.charge(cost));
                return true;
            }
        }
    }

private:
    std::vector<int> slots_;
    std::unordered_set<std::string> seen_;
};

class SortOp final : public PipelineOp {
public:
    SortOp(OperatorPtr child, std::vector<SortKey> keys, const Expr* skip, const Expr* limit,
           bool child_writes)
        : PipelineOp("Sort", std::move(child)), keys_(std::move(keys)), skip_(skip), limit_(limit),
          child_writes_(child_writes) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        if (!sorted_) {
            CYPHER_TRY(consume(row, context));
            sorted_ = true;
        }
        if (cursor_ >= entries_.size())
            return false;
        row = std::move(entries_[cursor_++].row);
        return true;
    }

private:
    struct Entry {
        std::vector<QueryValue> keys;
        std::uint64_t arrival = 0;
        Row row;
        std::size_t cost = 0;
    };

    // Eşit anahtarda varış sırası: top-K ile tam sıralama aynı satırları aynı sırada seçer.
    bool before(const std::vector<QueryValue>& left_keys, std::uint64_t left_arrival,
                const Entry& right) const {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            const int order = compare_for_order(left_keys[i], right.keys[i]);
            if (order != 0)
                return keys_[i].descending ? order > 0 : order < 0;
        }
        return left_arrival < right.arrival;
    }

    bool entry_before(const Entry& left, const Entry& right) const {
        return before(left.keys, left.arrival, right);
    }

    Result<std::size_t> capacity(ExecContext& context) const {
        constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();
        if (limit_ == nullptr)
            return kUnbounded;
        const Result<std::size_t> limit = eval_count(*limit_, context);
        if (!limit)
            return limit;
        const Result<std::size_t> skip = skip_ != nullptr ? eval_count(*skip_, context) : std::size_t{0};
        if (!skip)
            return skip;
        return *skip > kUnbounded - *limit ? kUnbounded : *skip + *limit;
    }

    Result<void> eval_keys(const Row& row, ExecContext& context) {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            Result<QueryValue> key = eval(*keys_[i].expr, row, context);
            if (!key)
                return std::unexpected(std::move(key).error());
            scratch_[i] = std::move(*key);
        }
        return {};
    }

    Result<void> retain(const Row& row, std::uint64_t arrival, std::size_t capacity, ExecContext& context) {
        const auto heap_order = [this](const Entry& left, const Entry& right) {
            return entry_before(left, right);
        };
        const bool full = entries_.size() >= capacity;
        if (full) {
            // Yığın tepesi en kötü satır: satırı kopyalamadan elenir, bellek O(K).
            if (!before(scratch_, arrival, entries_.front()))
                return {};
            std::ranges::pop_heap(entries_, heap_order);
            context.budget.release(entries_.back().cost);
            entries_.pop_back();
        }
        Entry entry{scratch_, arrival, row, 0};
        entry.cost = row_bytes(row) + row_bytes(entry.keys);
        CYPHER_TRY(context.budget.charge(entry.cost));
        entries_.push_back(std::move(entry));
        std::ranges::push_heap(entries_, heap_order);
        return {};
    }

    Result<void> consume(Row& row, ExecContext& context) {
        const Result<std::size_t> capacity_value = capacity(context);
        if (!capacity_value)
            return std::unexpected(capacity_value.error());
        scratch_.resize(keys_.size());
        if (*capacity_value == 0 && child_writes_)
            CYPHER_TRY(child_->next(row, context));
        std::uint64_t arrival = 0;
        while (*capacity_value > 0) {
            const Result<bool> more = child_->next(row, context);
            if (!more)
                return std::unexpected(more.error());
            if (!*more)
                break;
            CYPHER_TRY(eval_keys(row, context));
            CYPHER_TRY(retain(row, arrival++, *capacity_value, context));
        }
        std::ranges::sort(
            entries_, [this](const Entry& left, const Entry& right) { return entry_before(left, right); });
        return {};
    }

    std::vector<SortKey> keys_;
    const Expr* skip_;
    const Expr* limit_;
    bool child_writes_;
    std::vector<QueryValue> scratch_;
    std::vector<Entry> entries_;
    std::size_t cursor_ = 0;
    bool sorted_ = false;
};

class SkipOp final : public PipelineOp {
public:
    SkipOp(OperatorPtr child, const Expr* count) : PipelineOp("Skip", std::move(child)), count_(count) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        if (!remaining_) {
            const Result<std::size_t> count = eval_count(*count_, context);
            if (!count)
                return std::unexpected(count.error());
            remaining_ = *count;
        }
        while (*remaining_ > 0) {
            const Result<bool> more = child_->next(row, context);
            if (!more || !*more)
                return more;
            --*remaining_;
        }
        return child_->next(row, context);
    }

private:
    const Expr* count_;
    std::optional<std::size_t> remaining_;
};

class LimitOp final : public PipelineOp {
public:
    LimitOp(OperatorPtr child, const Expr* count, bool child_writes)
        : PipelineOp("Limit", std::move(child)), count_(count), child_writes_(child_writes) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        if (!remaining_) {
            const Result<std::size_t> count = eval_count(*count_, context);
            if (!count)
                return std::unexpected(count.error());
            remaining_ = *count;
            if (*remaining_ == 0 && child_writes_)
                CYPHER_TRY(child_->next(row, context));
        }
        if (*remaining_ == 0)
            return false;
        --*remaining_;
        return child_->next(row, context);
    }

private:
    const Expr* count_;
    bool child_writes_;
    std::optional<std::size_t> remaining_;
};

struct AggregateState {
    std::int64_t count = 0;
    std::int64_t int_sum = 0;
    double real_sum = 0;
    bool is_real = false;
    QueryValue best;
    QueryList collected;
    std::unordered_set<std::string> seen;
};

Result<void> add_to_sum(AggregateState& state, const QueryValue& value, Position pos) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.v)) {
        if (state.is_real) {
            state.real_sum += static_cast<double>(*integer);
        } else if (__builtin_add_overflow(state.int_sum, *integer, &state.int_sum)) {
            return std::unexpected(located_error(Error::Code::InvalidArgument, "graph.cypher_arithmetic", pos,
                                                 {{"reason", "overflow"}}));
        }
        return {};
    }
    const auto* real = std::get_if<double>(&value.v);
    if (real == nullptr)
        return std::unexpected(
            located_error(Error::Code::InvalidArgument, "graph.cypher_aggregate_type", pos));
    if (!state.is_real) {
        state.is_real = true;
        state.real_sum = static_cast<double>(state.int_sum);
    }
    state.real_sum += *real;
    return {};
}

// Sonsuz üretilmez (expr.cpp aritmetik ile aynı kural): saklanamaz, çerçeveyle yayılamaz.
Result<void> check_sum_finite(const AggregateState& state, Position pos) {
    if (state.is_real && !std::isfinite(state.real_sum))
        return std::unexpected(located_error(Error::Code::InvalidArgument, "graph.cypher_arithmetic", pos,
                                             {{"reason", "overflow"}}));
    return {};
}

Result<void> add_to_collect(AggregateState& state, QueryValue value, ExecContext& context, Position pos) {
    if (value_depth(value) >= kMaxValueDepth)
        return std::unexpected(limit_error("memory"));
    (void)pos;
    CYPHER_TRY(context.budget.charge(value_bytes(value)));
    state.collected.items.push_back(std::move(value));
    return {};
}

// max(n.body) grup başına tam değer tutar: bütçeye girer, değişince eski payı bırakır.
Result<void> keep_best(AggregateState& state, const QueryValue& value, bool want_smaller,
                       ExecContext& context) {
    if (state.count != 0) {
        const int order = compare_for_order(value, state.best);
        if (want_smaller ? order >= 0 : order <= 0)
            return {};
        context.budget.release(value_bytes(state.best));
    }
    CYPHER_TRY(context.budget.charge(value_bytes(value)));
    state.best = value;
    return {};
}

// null girdi yalnız count(*)'da sayılır (SPEC-graph "Semantik").
Result<void> update_aggregate(const AggregateSpec& spec, AggregateState& state, const Row& row,
                              ExecContext& context) {
    if (spec.fn == AggregateFn::CountStar) {
        ++state.count;
        return {};
    }
    Result<QueryValue> value = eval(*spec.arg, row, context);
    if (!value)
        return std::unexpected(std::move(value).error());
    if (value->is_null())
        return {};
    if (spec.distinct) {
        std::string key;
        append_canonical(key, *value);
        const std::size_t cost = key.size() + sizeof(std::string);
        if (!state.seen.insert(std::move(key)).second)
            return {};
        CYPHER_TRY(context.budget.charge(cost));
    }
    switch (spec.fn) {
    case AggregateFn::Sum:
    case AggregateFn::Avg:
        CYPHER_TRY(add_to_sum(state, *value, spec.pos));
        CYPHER_TRY(check_sum_finite(state, spec.pos));
        break;
    case AggregateFn::Min:
    case AggregateFn::Max:
        CYPHER_TRY(keep_best(state, *value, spec.fn == AggregateFn::Min, context));
        break;
    case AggregateFn::Collect:
        CYPHER_TRY(add_to_collect(state, std::move(*value), context, spec.pos));
        break;
    default:
        break;
    }
    ++state.count;
    return {};
}

QueryValue finish_aggregate(const AggregateSpec& spec, AggregateState& state) {
    switch (spec.fn) {
    case AggregateFn::CountStar:
    case AggregateFn::Count:
        return state.count;
    case AggregateFn::Sum:
        return state.is_real ? QueryValue(state.real_sum) : QueryValue(state.int_sum);
    case AggregateFn::Avg: {
        if (state.count == 0)
            return {};
        const double total = state.is_real ? state.real_sum : static_cast<double>(state.int_sum);
        return total / static_cast<double>(state.count);
    }
    case AggregateFn::Collect:
        return std::move(state.collected);
    default:
        return std::move(state.best);
    }
}

class AggregateOp final : public PipelineOp {
public:
    AggregateOp(OperatorPtr child, std::vector<ProjectItem> keys, std::vector<AggregateSpec> aggregates,
                std::vector<ProjectItem> finals)
        : PipelineOp("Aggregate", std::move(child)), keys_(std::move(keys)),
          aggregates_(std::move(aggregates)), finals_(std::move(finals)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        if (!grouped_) {
            CYPHER_TRY(consume(row, context));
            grouped_ = true;
        }
        if (cursor_ >= order_.size())
            return false;
        Group& group = *order_[cursor_++];
        for (std::size_t i = 0; i < keys_.size(); ++i)
            row[static_cast<std::size_t>(keys_[i].slot)] = std::move(group.keys[i]);
        for (std::size_t i = 0; i < aggregates_.size(); ++i)
            row[static_cast<std::size_t>(aggregates_[i].slot)] =
                finish_aggregate(aggregates_[i], group.states[i]);
        for (const ProjectItem& item : finals_) {
            Result<QueryValue> value = eval(*item.expr, row, context);
            if (!value)
                return std::unexpected(std::move(value).error());
            row[static_cast<std::size_t>(item.slot)] = std::move(*value);
        }
        return true;
    }

private:
    struct Group {
        std::vector<QueryValue> keys;
        std::vector<AggregateState> states;
    };

    Result<Group*> group_of(const Row& row, ExecContext& context) {
        std::vector<QueryValue> keys;
        std::string canonical;
        for (const ProjectItem& item : keys_) {
            Result<QueryValue> value = eval(*item.expr, row, context);
            if (!value)
                return std::unexpected(std::move(value).error());
            append_canonical(canonical, *value);
            keys.push_back(std::move(*value));
        }
        const auto [entry, inserted] = groups_.try_emplace(std::move(canonical));
        if (inserted)
            CYPHER_TRY(open_group(entry->second, std::move(keys), entry->first.size(), context));
        return &entry->second;
    }

    Result<void> open_group(Group& group, std::vector<QueryValue> keys, std::size_t key_bytes,
                            ExecContext& context) {
        group.keys = std::move(keys);
        group.states.resize(aggregates_.size());
        order_.push_back(&group);
        return context.budget.charge(key_bytes + row_bytes(group.keys) +
                                     aggregates_.size() * sizeof(AggregateState));
    }

    Result<void> consume(Row& row, ExecContext& context) {
        // Gruplama anahtarsızsa girdi boş olsa da tek satır çıkar: count(*) = 0.
        if (keys_.empty())
            CYPHER_TRY(group_of(row, context));
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more)
                return std::unexpected(more.error());
            if (!*more)
                return {};
            const Result<Group*> group = group_of(row, context);
            if (!group)
                return std::unexpected(group.error());
            for (std::size_t i = 0; i < aggregates_.size(); ++i)
                CYPHER_TRY(update_aggregate(aggregates_[i], (*group)->states[i], row, context));
        }
    }

    std::vector<ProjectItem> keys_;
    std::vector<AggregateSpec> aggregates_;
    std::vector<ProjectItem> finals_;
    std::unordered_map<std::string, Group> groups_;  // node tabanlı: `order_` işaretçileri kararlı
    std::vector<Group*> order_;
    std::size_t cursor_ = 0;
    bool grouped_ = false;
};

class OptionalApplyOp final : public PipelineOp {
public:
    OptionalApplyOp(OperatorPtr child, OperatorPtr inner, OnceOp* leaf, std::vector<int> null_slots)
        : PipelineOp("OptionalApply", std::move(child)), inner_(std::move(inner)), leaf_(leaf),
          null_slots_(std::move(null_slots)) {}

    Result<bool> next(Row& row, ExecContext& context) override {
        while (true) {
            if (!active_) {
                const Result<bool> more = child_->next(row, context);
                if (!more || !*more)
                    return more;
                saved_ = row;
                leaf_->rearm();
                active_ = true;
                matched_ = false;
            }
            const Result<bool> inner = inner_->next(row, context);
            if (!inner)
                return inner;
            if (*inner) {
                matched_ = true;
                return true;
            }
            active_ = false;
            if (!matched_) {
                row = saved_;
                for (const int slot : null_slots_)
                    row[static_cast<std::size_t>(slot)] = QueryValue{};
                return true;
            }
        }
    }

    void describe(std::vector<std::string>& out) const override {
        child_->describe(out);
        std::vector<std::string> inner;
        inner_->describe(inner);
        std::string name = name_ + "[";
        for (std::size_t i = 0; i < inner.size(); ++i)
            name += (i == 0 ? "" : ",") + inner[i];
        out.push_back(name + "]");
    }

private:
    OperatorPtr inner_;
    OnceOp* leaf_;
    std::vector<int> null_slots_;
    Row saved_;
    bool active_ = false;
    bool matched_ = false;
};

}  // namespace

Result<bool> OnceOp::next(Row&, ExecContext&) {
    return std::exchange(pending_, false);
}

OperatorPtr make_node_scan(OperatorPtr child, std::string name, int slot) {
    return std::make_unique<ScanOp>(std::move(child), std::move(name), slot, std::nullopt);
}

OperatorPtr make_label_scan(OperatorPtr child, std::string name, int slot, std::string label) {
    return std::make_unique<ScanOp>(std::move(child), std::move(name), slot, std::move(label));
}

OperatorPtr make_id_seek(OperatorPtr child, std::string name, int slot, const Expr* id) {
    return std::make_unique<IdSeekOp>(std::move(child), std::move(name), slot, id);
}

OperatorPtr make_check_node(OperatorPtr child, int slot, Position pos) {
    return std::make_unique<CheckNodeOp>(std::move(child), slot, pos);
}

OperatorPtr make_expand(OperatorPtr child, ExpandSpec spec) {
    return std::make_unique<ExpandOp>(std::move(child), std::move(spec));
}

OperatorPtr make_var_length_expand(OperatorPtr child, ExpandSpec spec) {
    return std::make_unique<VarLengthExpandOp>(std::move(child), std::move(spec));
}

OperatorPtr make_filter(OperatorPtr child, const Expr* predicate) {
    return std::make_unique<FilterOp>(std::move(child), predicate);
}

OperatorPtr make_project(OperatorPtr child, std::vector<ProjectItem> items) {
    return std::make_unique<ProjectOp>(std::move(child), std::move(items));
}

OperatorPtr make_distinct(OperatorPtr child, std::vector<int> slots) {
    return std::make_unique<DistinctOp>(std::move(child), std::move(slots));
}

OperatorPtr make_sort(OperatorPtr child, std::vector<SortKey> keys, const Expr* skip, const Expr* limit,
                      bool child_writes) {
    return std::make_unique<SortOp>(std::move(child), std::move(keys), skip, limit, child_writes);
}

OperatorPtr make_skip(OperatorPtr child, const Expr* count) {
    return std::make_unique<SkipOp>(std::move(child), count);
}

OperatorPtr make_limit(OperatorPtr child, const Expr* count, bool child_writes) {
    return std::make_unique<LimitOp>(std::move(child), count, child_writes);
}

OperatorPtr make_aggregate(OperatorPtr child, std::vector<ProjectItem> keys,
                           std::vector<AggregateSpec> aggregates, std::vector<ProjectItem> finals) {
    return std::make_unique<AggregateOp>(std::move(child), std::move(keys), std::move(aggregates),
                                         std::move(finals));
}

OperatorPtr make_optional_apply(OperatorPtr child, OperatorPtr inner, OnceOp* inner_leaf,
                                std::vector<int> null_slots) {
    return std::make_unique<OptionalApplyOp>(std::move(child), std::move(inner), inner_leaf,
                                             std::move(null_slots));
}

}  // namespace noteapp::graph::detail::cypher
