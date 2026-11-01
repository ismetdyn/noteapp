#pragma once

#include "graph/cypher/operators.hpp"
#include "graph/error.hpp"
#include "graph/query.hpp"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::graph::detail::cypher {

// Plan bir işleç ağacıdır; depolama dili üretilmez (SPEC-graph ilke 3). Planlama depoya dokunmaz: salt-okunur
// ihlali işlem açılmadan görülür.
struct Plan {
    Plan();
    Plan(Plan&&) noexcept;
    Plan& operator=(Plan&&) noexcept;
    ~Plan();

    Query query;
    std::vector<ExprPtr> owned_exprs;
    std::list<TypeSet> type_sets;  // işleçler adres tutar; deque taşınırken ayırır, Plan noexcept taşınır
    OperatorPtr root;
    int slot_count = 0;
    std::vector<std::string> columns;
    std::vector<int> column_slots;
    std::optional<Position> first_write;
};

Result<Plan> build_plan(std::string_view cypher, const Params& params, const QueryLimits& limits);

// İşleç adları yaprak -> kök sırasıyla. Testler planın şeklini buradan sınar.
Result<std::vector<std::string>> describe_plan(std::string_view cypher, const Params& params);

}  // namespace noteapp::graph::detail::cypher
