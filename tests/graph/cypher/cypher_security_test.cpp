// Güvenlik incelemesinin bulguları: her biri önce burada düştü, sonra düzeltildi.
#include "cypher_support.hpp"
#include "graph/op.hpp"

#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
#include <stop_token>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::test;

namespace noteapp::graph::test {
// op_test.cpp'nin operator new kancası: n'inci ayırma bir kez bad_alloc fırlatır, 0 kapatır.
void fail_allocation_after(std::size_t count);
}  // namespace noteapp::graph::test

namespace {

std::string repeated(std::string_view item, std::size_t count) {
    std::string out;
    out.reserve((item.size() + 1) * count);
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0)
            out += ',';
        out += item;
    }
    return out;
}

void check_limit(Graph& graph, std::string_view text, QueryLimits limits, std::string_view limit,
                 QueryMode mode = QueryMode::ReadOnly) {
    INFO(std::string(text.substr(0, 120)));
    const Result<QueryResult> result = graph.query(text, {}, mode, limits);
    REQUIRE_FALSE(result.has_value());
    INFO(describe_error(result.error()));
    CHECK(result.error().code == Error::Code::LimitExceeded);
    CHECK(arg_of(result.error(), "limit") == limit);
}

}  // namespace

// Bulgu 1: yuva -1 ile sınır dışı okuma; diğer ad başka bir değişkenin özelliğini okuyordu.
TEST_CASE("property_of_a_non_variable_alias_is_null_and_stays_in_bounds") {
    GraphFixture f;
    CHECK(lines(must_query(f.graph, "RETURN 5 AS x ORDER BY x.y")) == Strings{"5"});
    CHECK(lines(must_query(f.graph, "RETURN 5 AS x ORDER BY x:L")) == Strings{"5"});
    CHECK(lines(must_query(f.graph, "RETURN 5 AS x, 6 AS z ORDER BY x.y, z.y")) == Strings{"5|6"});
    // Üç düğümde y, a'nın tersine sıralı: `n.y`'ye göre sıralamak gözlemlenebilir olur.
    must_write(f.graph, "CREATE (:N {a: 1, y: 30}), (:N {a: 2, y: 20}), (:N {a: 3, y: 10})");
    const Strings arrival = lines(must_query(f.graph, "MATCH (n:N) RETURN n.a AS x"));
    CHECK(lines(must_query(f.graph, "MATCH (n:N) RETURN n.a AS x ORDER BY x.y")) == arrival);
    CHECK(lines(must_query(f.graph, "MATCH (n:N) RETURN n.a AS x ORDER BY x.y DESC")) == arrival);
    // Diğer ad düğümse özelliği gerçekten okunur.
    CHECK(lines(must_query(f.graph, "MATCH (n:N) RETURN n AS x, n.a ORDER BY x.y")) ==
          lines(must_query(f.graph, "MATCH (n:N) RETURN n AS x, n.a ORDER BY n.y")));
    CHECK(lines(must_query(f.graph, "MATCH (n:N) RETURN n.a ORDER BY n.y LIMIT 1")) == Strings{"3"});
    CHECK(lines(must_query(f.graph, "MATCH (n:N) RETURN n AS x, n.a AS a ORDER BY x:N, a DESC LIMIT 1"))
              .size() == 1);
}

// Bulgu 2: LIMIT 0 alt ağacı hiç sürmüyordu: yazma eager sözü bozuluyordu, çıkış 0 ile.
TEST_CASE("write_subtree_runs_even_at_limit_zero") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:A {x: 0}), (:A {x: 0})");
    const auto values = [&] { return sorted(lines(must_query(f.graph, "MATCH (n:A) RETURN n.x"))); };

    CHECK(must_write(f.graph, "MATCH (n:A) SET n.x = 1 RETURN n LIMIT 0").rows.empty());
    CHECK(values() == Strings{"1", "1"});
    CHECK(must_write(f.graph, "MATCH (n:A) SET n.x = 2 RETURN n ORDER BY n.x LIMIT 0").rows.empty());
    CHECK(values() == Strings{"2", "2"});
    CHECK(must_write(f.graph, "MATCH (n:A) SET n.x = 3 RETURN n SKIP 1 LIMIT 0").rows.empty());
    CHECK(values() == Strings{"3", "3"});
    CHECK(must_write(f.graph, "MATCH (n:A) SET n.x = 4 WITH n LIMIT 0 RETURN n").rows.empty());
    CHECK(values() == Strings{"4", "4"});
    CHECK(must_write(f.graph, "MATCH (n:A) SET n.x = 5 WITH n ORDER BY n.x LIMIT 0 RETURN n").rows.empty());
    CHECK(values() == Strings{"5", "5"});
    CHECK(must_write(f.graph, "CREATE (:B) RETURN 1 LIMIT 0").rows.empty());
    CHECK(must_query(f.graph, "MATCH (b:B) RETURN b").rows.size() == 1);
    CHECK(must_write(f.graph, "MERGE (:C {k: 1}) RETURN 1 LIMIT 0").rows.empty());
    CHECK(must_query(f.graph, "MATCH (c:C) RETURN c").rows.size() == 1);
    // Yazma hatası da gizlenmez.
    check_rejected(f.graph, "MATCH (n:A) SET n.x = 1 / 0 RETURN n LIMIT 0", Error::Code::InvalidArgument,
                   QueryMode::ReadWrite);

    // Yazmayan sorguda LIMIT 0 hâlâ hiç gezmez.
    QueryLimits none;
    none.max_expansions = 0;
    CHECK(
        must_query(f.graph, "MATCH (a)-->(b) RETURN b LIMIT 0", {}, QueryMode::ReadOnly, none).rows.empty());
    CHECK(must_query(f.graph, "MATCH (a)-->(b) RETURN b ORDER BY b.x LIMIT 0", {}, QueryMode::ReadOnly, none)
              .rows.empty());
}

// Bulgu 3: 64 KiB'lik sorgu diğer ad açılımıyla ~2,5e8 ifade düğümü kopyalatıyordu.
TEST_CASE("alias_inlining_is_budgeted_in_the_plan_phase") {
    GraphFixture f;
    const std::string text =
        "RETURN [" + repeated("1", 15000) + "] AS a ORDER BY [" + repeated("a", 17000) + "]";
    REQUIRE(text.size() <= 64U * 1024U);
    const auto start = std::chrono::steady_clock::now();
    check_limit(f.graph, text, QueryLimits{}, "plan_nodes");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
    // Makul açılım geçer.
    CHECK(lines(must_query(f.graph, "RETURN [1, 2] AS a ORDER BY [a, a]")) == Strings{"[1,2]"});
}

// Bulgu 4: satıra yazılan değer bütçeye girmiyordu: her cümlede sınırın altında, toplamda sınırsız.
TEST_CASE("nested_with_chain_hits_the_memory_limit") {
    GraphFixture f;
    QueryLimits limits;
    limits.max_memory_bytes = 1024U * 1024U;
    std::string chain = "WITH [" + repeated("1", 3000) + "] AS a ";  // ~120 KiB değer
    for (int i = 0; i < 20; ++i)
        chain += "WITH [a] AS a ";
    check_limit(f.graph, chain + "RETURN 1", limits, "memory");
    // Aynı yuva satır satır yeniden yazılınca bütçe birikmez: 2000 satır, her biri kısa ömürlü.
    must_write(f.graph, "CREATE (:R)");
    for (int i = 0; i < 11; ++i)
        must_write(f.graph, "MATCH (r:R) CREATE (:R)");
    QueryLimits row_limits;
    row_limits.max_memory_bytes = 64U * 1024U;
    const std::string per_row = "MATCH (r:R) WITH [" + repeated("r", 100) + "] AS l RETURN count(*)";
    CHECK(must_query(f.graph, per_row, {}, QueryMode::ReadOnly, row_limits).rows.size() == 1);
    // Varsayılan sınırla üstel zincir de hata verir, süreç belleği şişmez (ulimit -v altında da koşar).
    std::string doubling = "WITH [" + repeated("1", 2000) + "] AS a ";
    for (int i = 0; i < 40; ++i)
        doubling += "WITH [" + repeated("a", 8) + "] AS a ";
    check_limit(f.graph, doubling + "RETURN 1", QueryLimits{}, "memory");
}

// Bulgu 5: en büyük değer tutulurken bütçeye girmiyordu.
TEST_CASE("min_and_max_keep_their_best_value_on_the_budget") {
    GraphFixture f;
    const std::string body(100'000, 'x');
    for (int i = 0; i < 40; ++i)
        must_write(f.graph, "CREATE (:B {body: $b})", {{"b", body}});
    QueryLimits limits;
    limits.max_memory_bytes = 1024U * 1024U;
    check_limit(f.graph, "MATCH (n:B) WITH n, max(n.body) AS m RETURN count(*)", limits, "memory");
    check_limit(f.graph, "MATCH (n:B) WITH n, min(n.body) AS m RETURN count(*)", limits, "memory");
    // Tek grupta aynı değer yeniden yazılır, bütçe birikmez.
    CHECK(must_query(f.graph, "MATCH (n:B) WITH max(n.body) AS m RETURN count(*)", {}, QueryMode::ReadOnly,
                     limits)
              .rows.size() == 1);
}

// Bulgu 6: sonsuz saklanabiliyor ve çerçeveyle yayılabiliyordu.
TEST_CASE("non_finite_doubles_are_rejected_everywhere") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:S {v: 1e308}), (:S {v: 1e308})");
    check_rejected(f.graph, "MATCH (n:S) RETURN sum(n.v)", Error::Code::InvalidArgument);
    check_rejected(f.graph, "MATCH (n:S) RETURN avg(n.v)", Error::Code::InvalidArgument);
    check_rejected(f.graph, "MATCH (n:S) WITH sum(n.v) AS s CREATE (:T {v: s})", Error::Code::InvalidArgument,
                   QueryMode::ReadWrite);
    CHECK(must_query(f.graph, "MATCH (n:T) RETURN n").rows.empty());

    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const double value : {infinity, -infinity, nan}) {
        // Kitaplık yazması.
        WriteTxn txn = begin_write(f.graph);
        const NodeId id = node_named("finite");
        GRAPH_OK(txn.put_node(id));
        const Result<void> stored = txn.set_property(id, "v", value);
        CHECK_FALSE(stored.has_value());
    }
    CHECK_FALSE(f.graph.query("RETURN $v", {{"v", infinity}}).has_value());
    CHECK_FALSE(f.graph.query("RETURN $v", {{"v", nan}}).has_value());
    CHECK_FALSE(f.graph.query("RETURN $v", {{"v", QueryList{{QueryValue(infinity)}}}}).has_value());

    // Çerçeve: güvenilmez kaynaktan gelen sonsuz da reddedilir.
    detail::Frame frame;
    frame.seq = 1;
    frame.ops.emplace_back(detail::PropSet{node_named("x"), "k", infinity});
    const std::string encoded = detail::encode_frame(frame);
    CHECK_FALSE(detail::decode_frame(as_bytes(encoded)).has_value());
}

// Bulgu 8: bad_alloc çözülürken bağlantı query_only'de kalıyordu: sonraki yazmalar sessizce kırıktı.
TEST_CASE("read_query_restores_the_connection_after_allocation_failure") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:Q {v: 1}), (:Q {v: 2})");
    std::size_t limit_errors = 0;
    for (std::size_t nth = 1; nth < 200; ++nth) {
        fail_allocation_after(nth);
        const Result<QueryResult> result = f.graph.query("MATCH (n:Q) RETURN n.v ORDER BY n.v");
        fail_allocation_after(0);
        if (!result.has_value()) {
            ++limit_errors;
            CHECK(result.error().code == Error::Code::LimitExceeded);
        }
        // Her durumda bağlantı yeniden yazabilmeli ve okuyabilmeli.
        INFO(nth);
        GRAPH_OK(f.graph.query("MATCH (n:Q) SET n.v = n.v", {}, QueryMode::ReadWrite));
        CHECK(must_query(f.graph, "MATCH (n:Q) RETURN count(*)").rows.size() == 1);
    }
    CHECK(limit_errors > 0);
}

// Bulgu 9: düğüm olmayan bağlı değişken düğümmüş gibi geçiyordu.
TEST_CASE("binding_a_non_node_as_a_node_is_a_type_error") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:A)-[:T]->(b:B)");
    check_rejected(f.graph, "WITH 1 AS x MATCH (x) RETURN x", Error::Code::InvalidArgument);
    check_rejected(f.graph, "WITH 'a' AS x MATCH (x:A) RETURN x", Error::Code::InvalidArgument);
    check_rejected(f.graph, "WITH 1 AS x MATCH (x)-->(y) RETURN y", Error::Code::InvalidArgument);
    check_rejected(f.graph, "MATCH (y:A) WITH 1 AS x, y MATCH (y)-->(x) RETURN y",
                   Error::Code::InvalidArgument);
    check_rejected(f.graph, "WITH [1] AS x OPTIONAL MATCH (x)-->(y) RETURN y", Error::Code::InvalidArgument);
    // Null eşleşmez, hata değildir; düğüm değeri geçer.
    CHECK(must_query(f.graph, "WITH null AS x MATCH (x) RETURN x").rows.empty());
    CHECK(must_query(f.graph, "WITH null AS x MATCH (x)-->(y) RETURN y").rows.empty());
    CHECK(must_query(f.graph, "MATCH (a:A) WITH a AS x MATCH (x) RETURN x").rows.size() == 1);
    CHECK(must_query(f.graph, "MATCH (a:A) WITH min(a) AS x MATCH (x)-->(y) RETURN y").rows.size() == 1);
    CHECK(must_query(f.graph, "MATCH (b:B) WITH min(b) AS x MATCH (y)-->(x) RETURN y").rows.size() == 1);
}

// Bulgu 10: toplu silme süre bütçesini görmüyordu.
TEST_CASE("bulk_delete_honours_the_time_budget") {
    GraphFixture f;
    {
        WriteTxn txn = begin_write(f.graph);
        NodeId previous = node_named("first");
        GRAPH_OK(txn.put_node(previous));
        for (int i = 0; i < 4000; ++i) {
            const NodeId node = new_node_id();
            GRAPH_OK(txn.put_node(node));
            GRAPH_OK(txn.add_label(node, "D"));
            GRAPH_OK(txn.put_edge(new_edge_id(), previous, "T", node));
            previous = node;
        }
        GRAPH_OK(txn.commit());
    }
    // Süre yalnız silmeye yetmeyecek kadar: taramanın süresinin katı; silme satır başına çok daha pahalıdır.
    const auto start = std::chrono::steady_clock::now();
    CHECK(must_query(f.graph, "MATCH (n:D) RETURN count(*)").rows.size() == 1);
    const auto scan = std::chrono::steady_clock::now() - start;
    QueryLimits limits;
    limits.max_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(scan) * 2 + std::chrono::milliseconds(1);
    const Result<QueryResult> deleted =
        f.graph.query("MATCH (n:D) DETACH DELETE n", {}, QueryMode::ReadWrite, limits);
    REQUIRE_FALSE(deleted.has_value());
    INFO(describe_error(deleted.error()));
    CHECK(arg_of(deleted.error(), "limit") == "duration");
    // Geri alındı.
    CHECK(lines(must_query(f.graph, "MATCH (n:D) RETURN count(*)")) == Strings{"4000"});
    const auto delete_start = std::chrono::steady_clock::now();
    must_write(f.graph, "MATCH (n:D) DETACH DELETE n");
    const auto delete_time = std::chrono::steady_clock::now() - delete_start;
    MESSAGE("scan_ms=" << std::chrono::duration<double, std::milli>(scan).count() << " detach_delete_4000_ms="
                       << std::chrono::duration<double, std::milli>(delete_time).count());
}

// İnceleme (eşzamanlılık, düşük): limits.view.duration = 0 (sınırsız) iken takılan görünüm sahibinin
// kapanışını sonsuz bekletmesin: iptal süre denetiminde sorguyu durdurur.
TEST_CASE("a cancelled query stops even with an unlimited duration") {
    GraphFixture f;
    {
        WriteTxn txn = begin_write(f.graph);
        for (int i = 0; i < 4000; ++i) {
            const NodeId node = new_node_id();
            GRAPH_OK(txn.put_node(node));
            GRAPH_OK(txn.add_label(node, "D"));
        }
        GRAPH_OK(txn.commit());
    }
    std::stop_source stop;
    QueryLimits limits;
    limits.max_duration = std::chrono::milliseconds::max();
    limits.stop = stop.get_token();
    CHECK(must_query(f.graph, "MATCH (n:D) RETURN count(*)").rows.size() == 1);
    CHECK(f.graph.query("MATCH (n:D) RETURN count(*)", {}, QueryMode::ReadOnly, limits).has_value());
    stop.request_stop();
    check_limit(f.graph, "MATCH (n:D) RETURN count(*)", limits, "cancelled");
}
