#pragma once

// Graph::query'nin gövdesi: ayrıştır, planla, salt-okunur denetimi, işlemi aç, işleçleri çalıştır.
#include "graph/error.hpp"
#include "graph/graph.hpp"
#include "graph/query.hpp"
#include "graph/store.hpp"

#include <string_view>

namespace noteapp::graph::detail::cypher {

Result<QueryResult> run_query(Graph& graph, Store& store, std::string_view cypher, const Params& params,
                              QueryMode mode, const QueryLimits& limits);
Result<void> check_query(std::string_view cypher, const Params& params, QueryMode mode);

}  // namespace noteapp::graph::detail::cypher
