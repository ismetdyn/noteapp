// tests/graph/fuzz'un bulduğu en küçük girdiler. Sonuçtan çok crash ve hang olmaması sınanır, ama kod da
// doğru olmalı.
#include "cypher_support.hpp"

#include <doctest/doctest.h>

using namespace noteapp::graph;
using namespace noteapp::graph::test;

TEST_CASE("fuzz_relationship_variable_reusing_a_node_name_in_create_is_rejected") {
    GraphFixture f;
    const auto write = QueryMode::ReadWrite;
    check_rejected(f.graph, "CREATE (p:P)-[:E]->(q:Q)-[s:T]->(s:S)", Error::Code::Unsupported, write);
    check_rejected(f.graph, "CREATE (s)-[s:T]->(b)", Error::Code::Unsupported, write);
    check_rejected(f.graph, "MERGE (s)-[s:T]->(b)", Error::Code::Unsupported, write);
    CHECK(must_query(f.graph, "MATCH (n) RETURN count(*)").rows.at(0).at(0) == QueryValue(std::int64_t{0}));
}
