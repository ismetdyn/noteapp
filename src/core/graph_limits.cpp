#include "core/graph_limits.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace noteapp::core {

namespace {

// Değer config'te int64 ile sınırlıdır: milisaniye sayısına kayıpsız sığar.
std::chrono::milliseconds duration_of(const Limit& limit) {
    if (limit.unlimited())
        return std::chrono::milliseconds::max();
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
        std::min<std::uint64_t>(limit.value, std::chrono::milliseconds::max().count())));
}

std::uint64_t bytes_of(const Limit& limit) {
    return limit.unlimited() ? UINT64_MAX : limit.value;
}

}  // namespace

graph::GraphLimits graph_limits_of(const GraphLimits& limits) {
    return graph::GraphLimits{
        .frame_bytes = limits.frame_bytes.cap(),
        .frame_ops = limits.frame_ops.cap(),
        .group_bytes = limits.group_bytes.cap(),
        .group_ops = limits.group_ops.cap(),
        .peers = limits.peers.cap(),
        .hlc_skew = duration_of(limits.hlc_skew),
        .ingest_bytes = bytes_of(limits.ingest_bytes),
        .merge_owners = limits.merge_owners.cap(),
        .report_entries = limits.report_entries.cap(),
        .touched_properties = limits.touched_properties.cap(),
        .set_aside_logs = limits.set_aside_logs.cap(),
    };
}

graph::QueryLimits query_limits_of(const QueryLimits& limits) {
    return graph::QueryLimits{
        .max_rows = limits.rows.cap(),
        .max_expansions = limits.expansions.cap(),
        .max_duration = duration_of(limits.duration),
        .max_memory_bytes = limits.memory.cap(),
        .max_memo_bytes = limits.memo.cap(),
        .max_memo_value_bytes = limits.memo_value.cap(),
        .max_text_bytes = limits.text.cap(),
        .max_nesting = limits.nesting.cap(),
        .max_pattern_elements = limits.pattern_elements.cap(),
        .max_clauses = limits.clauses.cap(),
        .max_var_length_hops = limits.var_length_hops.cap(),
        .max_plan_nodes = limits.plan_nodes.cap(),
    };
}

}  // namespace noteapp::core
