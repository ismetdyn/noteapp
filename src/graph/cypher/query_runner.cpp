#include "graph/cypher/query_runner.hpp"

#include "graph/cypher/expr.hpp"
#include "graph/cypher/planner.hpp"

#include <new>
#include <utility>

namespace noteapp::graph::detail::cypher {

namespace {

Result<QueryResult> drain(Plan& plan, ExecContext& context) {
    QueryResult result;
    result.columns = plan.columns;
    Row row(static_cast<std::size_t>(plan.slot_count));
    while (true) {
        const Result<bool> more = plan.root->next(row, context);
        if (!more)
            return std::unexpected(more.error());
        if (!*more)
            return result;
        if (plan.column_slots.empty())
            continue;
        std::vector<QueryValue> output;
        output.reserve(plan.column_slots.size());
        for (const int slot : plan.column_slots)
            output.push_back(row[static_cast<std::size_t>(slot)]);
        if (result.rows.size() >= context.budget.max_rows())
            return std::unexpected(limit_error("rows"));
        CYPHER_TRY(context.budget.charge(row_bytes(output)));
        result.rows.push_back(std::move(output));
    }
}

// Plan denetiminin yedeği; bağlantıda açıkken veri yazılamaz. Checkpoint ve COMMIT'i durdurmaz. Çözülme dahil
// her çıkışta kapanır: bad_alloc sonrası bağlantı sessizce yazamaz kalırdı.
class QueryOnlyScope {
public:
    explicit QueryOnlyScope(Store& store) : store_(store) {}
    ~QueryOnlyScope() {
        if (!active_)
            return;
        // Yıkıcı fırlatamaz. Kapatılamazsa bağlantı salt-okunur kalır: yazmalar hata verir, veri bozulmaz.
        try {
            (void)store_.set_query_only(false);
        } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch) -- yukarıdaki güvenli yöne düşer
        }
    }
    QueryOnlyScope(const QueryOnlyScope&) = delete;
    QueryOnlyScope& operator=(const QueryOnlyScope&) = delete;

    Result<void> enter() {
        CYPHER_TRY(store_.set_query_only(true));
        active_ = true;
        return {};
    }
    Result<void> leave() {
        active_ = false;
        return store_.set_query_only(false);
    }

private:
    Store& store_;
    bool active_ = false;
};

// İmleçler işlemden önce kapanır: Graph > Txn > cursor sözleşmesi.
Result<QueryResult> drain_read(Graph& graph, Store& store, Plan& plan, const QueryLimits& limits) {
    Result<ReadTxn> txn = graph.read();
    if (!txn)
        return std::unexpected(std::move(txn).error());
    ExecContext context(*txn, nullptr, store, limits, limits.max_memo_bytes);
    Result<QueryResult> drained = drain(plan, context);
    plan.root.reset();
    return drained;
}

Result<QueryResult> run_read(Graph& graph, Store& store, Plan& plan, const QueryLimits& limits) {
    QueryOnlyScope scope(store);
    CYPHER_TRY(scope.enter());
    Result<QueryResult> result = drain_read(graph, store, plan, limits);
    const Result<void> restored = scope.leave();
    if (result && !restored)
        return std::unexpected(restored.error());
    return result;
}

Result<QueryResult> run_write(Graph& graph, Store& store, Plan& plan, const QueryLimits& limits) {
    Result<WriteTxn> txn = graph.write();
    if (!txn)
        return std::unexpected(std::move(txn).error());
    // Yazma görünür olmalı: anlık görüntü sabit değil, önbellek kapalı.
    ExecContext context(*txn, &*txn, store, limits, 0);
    Result<QueryResult> result = drain(plan, context);
    plan.root.reset();
    if (!result)
        return result;
    CYPHER_TRY(txn->commit());
    return result;
}

Result<QueryResult> run_guarded(Graph& graph, Store& store, std::string_view cypher, const Params& params,
                                QueryMode mode, const QueryLimits& limits) {
    Result<Plan> plan = build_plan(cypher, params, limits);
    if (!plan)
        return std::unexpected(std::move(plan).error());
    if (plan->first_write && mode == QueryMode::ReadOnly)
        return std::unexpected(
            located_error(Error::Code::ReadOnlyViolation, "graph.cypher_read_only", *plan->first_write));
    if (store.has_open_txn())
        return std::unexpected(
            Error{Error::Code::InvalidArgument, "graph.invalid_argument", {{"reason", "txn_already_open"}}});
    return plan->first_write ? run_write(graph, store, *plan, limits) : run_read(graph, store, *plan, limits);
}

}  // namespace

Result<QueryResult> run_query(Graph& graph, Store& store, std::string_view cypher, const Params& params,
                              QueryMode mode, const QueryLimits& limits) {
    // API istisna fırlatmaz: limitin altında kalsa da ayırma düşerse kesilmiş sonuç değil hata olur.
    try {
        return run_guarded(graph, store, cypher, params, mode, limits);
    } catch (const std::bad_alloc&) {
        return std::unexpected(limit_error("memory"));
    }
}

}  // namespace noteapp::graph::detail::cypher
