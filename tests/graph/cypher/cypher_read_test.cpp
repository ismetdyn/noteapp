// SPEC-graph "Var" listesinin okuma tarafı: her madde beklenen sonuçla.
#include "cypher_support.hpp"
#include "graph/cypher/planner.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::test;

namespace {

// a -X-> b -X-> c -Y-> a; b -Y-> c; d yalıtık, e değersiz.
class Sample : public GraphFixture {
public:
    Sample() {
        must_write(graph, R"(CREATE (a:P:Q {name: 'a', v: 1, f: 1.5, ok: true}),
                                    (b:P {name: 'b', v: 2, ok: false}),
                                    (c:P {name: 'c', v: 3}),
                                    (d:R {name: 'd', v: 4}),
                                    (e:S),
                                    (a)-[:X {w: 10}]->(b), (b)-[:X {w: 20}]->(c),
                                    (c)-[:Y {w: 30}]->(a), (b)-[:Y]->(c))");
    }
    Strings rows(std::string_view text, const Params& params = {}) {
        return lines(must_query(graph, text, params));
    }
    Strings set(std::string_view text, const Params& params = {}) { return sorted(rows(text, params)); }
};

}  // namespace

TEST_CASE("match_scans_labels_and_directions") {
    Sample s;
    CHECK(s.set("MATCH (n) RETURN n.name") == Strings{"a", "b", "c", "d", "null"});
    CHECK(s.set("MATCH (n:P) RETURN n.name") == Strings{"a", "b", "c"});
    CHECK(s.set("MATCH (n:P:Q) RETURN n.name") == Strings{"a"});
    CHECK(s.set("MATCH (n:Absent) RETURN n.name").empty());
    CHECK(s.set("MATCH (n {name: 'b'}) RETURN n.v") == Strings{"2"});
    CHECK(s.set("MATCH (a {name: 'a'})-->(b) RETURN b.name") == Strings{"b"});
    CHECK(s.set("MATCH (a {name: 'a'})<--(b) RETURN b.name") == Strings{"c"});
    CHECK(s.set("MATCH (a {name: 'a'})--(b) RETURN b.name") == Strings{"b", "c"});
    CHECK(s.set("MATCH (a {name: 'b'})-[:X]->(b) RETURN b.name") == Strings{"c"});
    CHECK(s.set("MATCH (a {name: 'b'})-[:Y]->(b) RETURN b.name") == Strings{"c"});
    CHECK(s.set("MATCH (a {name: 'b'})-[:X|Y]->(b) RETURN b.name") == Strings{"c", "c"});
    CHECK(s.set("MATCH (a {name: 'b'})-[:X|:Y]->(b) RETURN b.name") == Strings{"c", "c"});
    CHECK(s.set("MATCH (a)-[r:X {w: 20}]->(b) RETURN a.name, b.name") == Strings{"b|c"});
    CHECK(s.set("MATCH (a)<-[r:X {w: 20}]-(b) RETURN a.name, b.name") == Strings{"c|b"});
}

TEST_CASE("multiple_patterns_share_variables_and_cartesian_products_work") {
    Sample s;
    CHECK(s.set("MATCH (a:Q), (b:R) RETURN a.name, b.name") == Strings{"a|d"});
    CHECK(s.set("MATCH (a:Q)-[:X]->(b), (b)-[:X]->(c) RETURN c.name") == Strings{"c"});
    CHECK(s.set("MATCH (a:Q) MATCH (a)-[:X]->(b) RETURN b.name") == Strings{"b"});
    CHECK(s.set("MATCH (a)-[:X]->(b)-[:X]->(c)-[:Y]->(a) RETURN a.name, b.name, c.name") == Strings{"a|b|c"});
}

TEST_CASE("pattern_edge_used_once_per_match") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a {n: 'a'}), (b {n: 'b'}), (a)-[:T]->(b)");
    // Tek kenar iki desen öğesine birden bağlanamaz: openCypher kenar benzersizliği.
    CHECK(sorted(lines(must_query(f.graph, "MATCH (x)-[:T]-(y)-[:T]-(z) RETURN x.n, y.n, z.n"))).empty());
    CHECK(sorted(lines(must_query(f.graph, "MATCH (x {n:'a'})-[r1]->(y), (x)-[r2]->(y) RETURN r1, r2")))
              .empty());
    // Ayrı MATCH'ler aynı kenarı yeniden kullanabilir.
    CHECK(lines(must_query(f.graph, "MATCH (x)-[:T]->(y) MATCH (x)-[:T]->(z) RETURN y.n, z.n")) ==
          Strings{"b|b"});
    // Düğüm tekrarlanabilir: a->b->a iki ayrı kenarla geçer.
    must_write(f.graph, "MATCH (a {n: 'a'}), (b {n: 'b'}) CREATE (b)-[:T]->(a)");
    CHECK(sorted(lines(must_query(f.graph, "MATCH (x)-[:T]->(y)-[:T]->(z) RETURN x.n, y.n, z.n"))) ==
          Strings{"a|b|a", "b|a|b"});
    // Paralel kenar iki ayrı öğeye bağlanabilir.
    must_write(f.graph, "MATCH (a {n: 'a'}), (b {n: 'b'}) CREATE (a)-[:P]->(b), (a)-[:P]->(b)");
    CHECK(must_query(f.graph, "MATCH (a)-[r1:P]->(b), (a)-[r2:P]->(b) RETURN r1").rows.size() == 2);
    // Değişken uzunluklu yol ile sabit kenar da aynı kümeyi paylaşır.
    CHECK(must_query(f.graph, "MATCH (a {n:'a'})-[:T*1..2]->(b)-[r:T]->(c {n:'a'}) RETURN r").rows.size() ==
          1);
}

TEST_CASE("null_three_valued_where") {
    Sample s;
    // e'nin v'si yok: karşılaştırma null, WHERE yalnız true geçirir, NOT null yine null.
    CHECK(s.set("MATCH (n) WHERE n.v = 1 RETURN n.name") == Strings{"a"});
    CHECK(s.set("MATCH (n) WHERE NOT n.v = 1 RETURN n.name") == Strings{"b", "c", "d"});
    CHECK(s.set("MATCH (n) WHERE n.absent = 1 RETURN n.name").empty());
    CHECK(s.set("MATCH (n) WHERE NOT n.absent = 1 RETURN n.name").empty());
    CHECK(s.set("MATCH (n) WHERE n.absent = n.absent RETURN n.name").empty());
    CHECK(s.set("MATCH (n) WHERE n.absent <> 1 RETURN n.name").empty());
    CHECK(s.set("MATCH (n:P) WHERE n.ok OR n.absent = 1 RETURN n.name") == Strings{"a"});
    CHECK(s.set("MATCH (n:P) WHERE n.ok AND n.absent = 1 RETURN n.name").empty());
    CHECK(s.set("MATCH (n:P) WHERE n.name IS NOT NULL AND n.ok IS NULL RETURN n.name") == Strings{"c"});
    CHECK(s.set("MATCH (n) WHERE n.name IS NULL RETURN id(n) IS NOT NULL") == Strings{"true"});
    CHECK(s.rows("RETURN null = null, null <> 1, NOT null, null AND false, null AND true, null OR true, "
                 "null OR false") == Strings{"null|null|null|false|null|true|null"});
    CHECK(s.rows("RETURN 1 IN [1, null], 2 IN [1, null], null IN [1], null IN [], 1 IN []") ==
          Strings{"true|null|null|false|false"});
    // Tip uyuşmazlığı: sıralama karşılaştırması null, eşitlik openCypher'da false.
    CHECK(s.rows("RETURN 1 < 'a', 1 = 'a', 1 <> 'a', true > 1, 'x' CONTAINS 1, 1 STARTS WITH 'x'") ==
          Strings{"null|false|true|null|null|null"});
    CHECK(s.rows("RETURN 1 AND true, NOT 'x'") == Strings{"null|null"});
}

TEST_CASE("comparisons_are_exact_and_cross_numeric") {
    Sample s;
    CHECK(s.rows("RETURN 1 = 1.0, 1 < 1.5, 2 > 1.5, 1 <= 1, 1 >= 2, 'a' < 'b', false < true") ==
          Strings{"true|true|true|true|false|true|true"});
    // 2^53 + 1 double'a sığmaz: double'a çevirip karşılaştırmak yanlış `true` verirdi.
    CHECK(s.rows("RETURN 9007199254740993 = 9007199254740992.0, 9007199254740993 > 9007199254740992.0") ==
          Strings{"false|true"});
    CHECK(s.rows(
              "RETURN 9223372036854775807 < 9223372036854775808.0, [1, 2] = [1, 2], [1] = [2], [1] = [null], "
              "[1, 2] = [1]") == Strings{"true|true|false|null|false"});
    // UTF-8 bayt sırası kod noktası sırasıdır: "ğ" > "z".
    CHECK(s.rows("RETURN 'ğ' > 'z', 'Z' < 'a'") == Strings{"true|true"});
}

TEST_CASE("string_predicates_are_byte_based") {
    Sample s;
    CHECK(s.rows("RETURN 'merhaba' CONTAINS 'hab', 'merhaba' STARTS WITH 'mer', 'merhaba' ENDS WITH 'aba', "
                 "'x' CONTAINS 'xy', 'x' STARTS WITH 'xy', 'x' ENDS WITH 'xy'") ==
          Strings{"true|true|true|false|false|false"});
    // Türkçe katlama yapılmaz: İ/i ve büyük/küçük farklıdır (arama katmanının işi).
    CHECK(s.rows("RETURN 'İSTANBUL' CONTAINS 'istanbul', 'Abc' CONTAINS 'abc', '' CONTAINS ''") ==
          Strings{"false|false|true"});
    CHECK(s.rows("RETURN null CONTAINS 'a', 'a' CONTAINS null") == Strings{"null|null"});
}

TEST_CASE("bytes_values_stay_exact") {
    GraphFixture f;
    const std::string raw("\xFF\x00\r\n\xEF\xBB\xBF", 7);
    must_query(f.graph, "CREATE (n:B {data: $d})", {{"d", Bytes{raw}}}, QueryMode::ReadWrite);
    const QueryResult result = must_query(f.graph, "MATCH (n:B) RETURN n.data");
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<Bytes>(result.rows[0][0].v).data == raw);
    CHECK(must_query(f.graph, "MATCH (n:B) WHERE n.data = $d RETURN n", {{"d", Bytes{raw}}}).rows.size() ==
          1);
    // Metin ve bayt aynı içerikte bile eşit değildir.
    CHECK(must_query(f.graph, "RETURN $a = $b", {{"a", Bytes{"x"}}, {"b", std::string("x")}}).rows[0][0] ==
          QueryValue(false));
    CHECK(lines(must_query(f.graph, "RETURN $a CONTAINS $b", {{"a", Bytes{raw}}, {"b", Bytes{"\r\n"}}})) ==
          Strings{"true"});
}

TEST_CASE("optional_match") {
    Sample s;
    CHECK(s.set("MATCH (n:R) OPTIONAL MATCH (n)-->(m) RETURN n.name, m.name") == Strings{"d|null"});
    CHECK(s.set("MATCH (n:Q) OPTIONAL MATCH (n)-[:X]->(m) RETURN n.name, m.name") == Strings{"a|b"});
    CHECK(s.set("MATCH (n:P) OPTIONAL MATCH (n)-[:X]->(m) WHERE m.v > 2 RETURN n.name, m.name") ==
          Strings{"a|null", "b|c", "c|null"});
    // Girdisiz başlangıç: tek null satır.
    CHECK(s.rows("OPTIONAL MATCH (n:Absent) RETURN n") == Strings{"null"});
    // Null bağlı değişkenle MATCH satırı düşürür, hata vermez.
    CHECK(s.rows("OPTIONAL MATCH (n:Absent) MATCH (n)-->(m) RETURN m").empty());
    CHECK(s.rows("OPTIONAL MATCH (n:Absent) MATCH (n) RETURN n").empty());
    CHECK(s.rows("OPTIONAL MATCH (n:Absent) RETURN n.x, labels(n), id(n)") == Strings{"null|null|null"});
    CHECK(s.rows("OPTIONAL MATCH (n:Absent)-[r]->() RETURN type(r)") == Strings{"null"});
    // Etiketsiz bağlı düğüm: iki OPTIONAL arka arkaya.
    CHECK(s.set("MATCH (n:R) OPTIONAL MATCH (n)-->(a) OPTIONAL MATCH (n)<--(b) RETURN a, b") ==
          Strings{"null|null"});
}

TEST_CASE("where_operators") {
    Sample s;
    CHECK(s.set("MATCH (n:P) WHERE n.v > 1 AND n.v < 3 RETURN n.name") == Strings{"b"});
    CHECK(s.set("MATCH (n:P) WHERE n.v >= 3 OR n.v <= 1 RETURN n.name") == Strings{"a", "c"});
    CHECK(s.set("MATCH (n:P) WHERE n.v <> 2 RETURN n.name") == Strings{"a", "c"});
    CHECK(s.set("MATCH (n:P) WHERE n.name IN ['a', 'c', 'z'] RETURN n.v") == Strings{"1", "3"});
    CHECK(s.set("MATCH (n:P) WHERE n.name IN $names RETURN n.v", {{"names", QueryList{{QueryValue("b")}}}}) ==
          Strings{"2"});
    CHECK(s.set("MATCH (n) WHERE n.name STARTS WITH 'a' OR n.name ENDS WITH 'd' RETURN n.name") ==
          Strings{"a", "d"});
    CHECK(s.set("MATCH (n) WHERE n.name CONTAINS 'b' RETURN n.name") == Strings{"b"});
    CHECK(s.set("MATCH (a)-[r]->(b) WHERE r.w > 15 AND a.v < b.v RETURN a.name, b.name") == Strings{"b|c"});
    CHECK(s.set("MATCH (a:P)-[:X]->(b) WHERE b.v = a.v + 1 RETURN b.name") == Strings{"b", "c"});
    CHECK(s.set("MATCH (n:P) WHERE n.v = $v RETURN n.name", {{"v", std::int64_t{3}}}) == Strings{"c"});
    CHECK(s.set("MATCH (n:P) WHERE n.name = $v RETURN n.v", {{"v", "c"}}) == Strings{"3"});
}

TEST_CASE("variable_length_is_a_trail_with_exact_bounds") {
    Sample s;
    CHECK(s.set("MATCH (a:Q)-[:X*1..1]->(b) RETURN b.name") == Strings{"b"});
    CHECK(s.set("MATCH (a:Q)-[:X*1..2]->(b) RETURN b.name") == Strings{"b", "c"});
    CHECK(s.set("MATCH (a:Q)-[:X*2]->(b) RETURN b.name") == Strings{"c"});
    CHECK(s.set("MATCH (a:Q)-[:X*..2]->(b) RETURN b.name") == Strings{"b", "c"});
    // b->c arasında paralel iki kenar: a->b->c->a iki ayrı iz.
    CHECK(s.set("MATCH (a:Q)-[*3]->(b) RETURN b.name") == Strings{"a", "a"});
    CHECK(s.set("MATCH (b {name: 'c'})<-[:X*1..2]-(a) RETURN a.name") == Strings{"a", "b"});
    // Dördüncü adım a->b kenarını tekrar kullanırdı: iz burada biter, sonsuz döngü yok.
    CHECK(s.set("MATCH (a:Q)-[*4]->(b) RETURN b.name").empty());
    CHECK(s.set("MATCH (a:Q)-[*5..10]->(b) RETURN b.name").empty());
    // Yol değişkeni kenar listesidir.
    const QueryResult paths = must_query(s.graph, "MATCH (a:Q)-[r:X*2]->(b) RETURN r");
    REQUIRE(paths.rows.size() == 1);
    CHECK(std::get<QueryList>(paths.rows[0][0].v).items.size() == 2);
    // İki ucu bağlı değişken uzunluklu yol.
    CHECK(s.set("MATCH (a:Q), (c {name: 'c'}) MATCH (a)-[:X*1..2]->(c) RETURN c.name") == Strings{"c"});
    // Sol tarafta yön çevrilir.
    CHECK(s.set("MATCH (x)-[:X*1..2]->(c {name: 'c'}) RETURN x.name") == Strings{"a", "b"});
}

TEST_CASE("with_clause_scopes_projects_and_filters") {
    Sample s;
    CHECK(s.set("MATCH (n:P) WITH n.v AS v WHERE v > 1 RETURN v") == Strings{"2", "3"});
    CHECK(s.set("MATCH (n:P) WITH n ORDER BY n.v DESC LIMIT 1 MATCH (n)-->(m) RETURN m.name") ==
          Strings{"a"});
    CHECK(s.rows("WITH 1 AS a, 2 AS b RETURN a + b") == Strings{"3"});
    CHECK(s.set("MATCH (n:P) WITH count(*) AS c RETURN c") == Strings{"3"});
    CHECK(s.set("MATCH (a)-[:X]->(b) WITH a, count(b) AS c WHERE c = 1 RETURN a.name") == Strings{"a", "b"});
    CHECK(s.set("MATCH (n:P) WITH DISTINCT n.ok IS NULL AS u RETURN u") == Strings{"false", "true"});
    CHECK(s.set("MATCH (n:Q) WITH * RETURN n.name") == Strings{"a"});
    CHECK(s.set("MATCH (n:Q) WITH n AS m MATCH (m)-[:X]->(x) RETURN x.name") == Strings{"b"});
    CHECK(s.rows("WITH 1 AS a WITH a + 1 AS a WITH a * 3 AS a RETURN a") == Strings{"6"});
}

TEST_CASE("return_distinct_as_order_skip_limit") {
    Sample s;
    CHECK(s.rows("MATCH (n:P) RETURN n.name ORDER BY n.v") == Strings{"a", "b", "c"});
    CHECK(s.rows("MATCH (n:P) RETURN n.name ORDER BY n.v DESC") == Strings{"c", "b", "a"});
    CHECK(s.rows("MATCH (n:P) RETURN n.name AS x ORDER BY x DESC") == Strings{"c", "b", "a"});
    CHECK(s.rows("MATCH (n:P) RETURN n.v AS v ORDER BY v ASCENDING SKIP 1") == Strings{"2", "3"});
    CHECK(s.rows("MATCH (n:P) RETURN n.v AS v ORDER BY v DESCENDING LIMIT 2") == Strings{"3", "2"});
    CHECK(s.rows("MATCH (n:P) RETURN n.v ORDER BY n.v SKIP 1 LIMIT 1") == Strings{"2"});
    CHECK(s.rows("MATCH (n:P) RETURN n.v ORDER BY n.v SKIP $s LIMIT $l",
                 {{"s", std::int64_t{2}}, {"l", std::int64_t{5}}}) == Strings{"3"});
    CHECK(s.rows("MATCH (n:P) RETURN n.v ORDER BY n.v LIMIT 0").empty());
    CHECK(s.rows("MATCH (n:P) RETURN n.v ORDER BY n.v SKIP 99").empty());
    CHECK(s.rows("MATCH (n:P) RETURN n.v LIMIT 0").empty());
    CHECK(s.set("MATCH (a)--(b:Q) RETURN DISTINCT b.name") == Strings{"a"});
    CHECK(s.rows("MATCH (a)--(b:Q) RETURN DISTINCT b.name AS x ORDER BY x") == Strings{"a"});
    CHECK(s.rows("MATCH (n:P) RETURN DISTINCT n.v ORDER BY n.v DESC") == Strings{"3", "2", "1"});
    CHECK(s.set("MATCH (n:Q) RETURN *") == s.rows("MATCH (n:Q) RETURN id(n)"));
    // 1 ile 1.0 aynı gruptur.
    CHECK(s.rows("WITH 1 AS a RETURN DISTINCT a") == Strings{"1"});
    // Sıralama: null ASC'de sonda, DESC'te başta; karışık tipler openCypher sırasıyla.
    CHECK(s.rows("MATCH (n) RETURN n.v ORDER BY n.v") == Strings{"1", "2", "3", "4", "null"});
    CHECK(s.rows("MATCH (n) RETURN n.v ORDER BY n.v DESC") == Strings{"null", "4", "3", "2", "1"});
    CHECK(s.rows("MATCH (n) RETURN n.v ORDER BY n.v DESC LIMIT 2") == Strings{"null", "4"});
    CHECK(s.rows("MATCH (n) RETURN n.name ORDER BY n.v DESC, n.name LIMIT 2") == Strings{"null", "d"});
    // Eşitlikte sıra tohum sırasıdır: top-K ile tam sıralama aynı satırları seçer.
    const Strings full = s.rows("MATCH (n) RETURN id(n) ORDER BY 1");
    const Strings top = s.rows("MATCH (n) RETURN id(n) ORDER BY 1 LIMIT 2");
    CHECK(top == Strings(full.begin(), full.begin() + 2));
    check_rejected(s.graph, "RETURN 1 LIMIT -1", Error::Code::InvalidArgument);
    check_rejected(s.graph, "RETURN 1 SKIP 1.5", Error::Code::InvalidArgument);
    check_rejected(s.graph, "RETURN 1 LIMIT 'x'", Error::Code::InvalidArgument);
    check_rejected(s.graph, "MATCH (n) RETURN n LIMIT n.v", Error::Code::Syntax);
}

TEST_CASE("aggregates_skip_nulls_and_group_implicitly") {
    Sample s;
    CHECK(s.rows("MATCH (n) RETURN count(*), count(n.v), sum(n.v), min(n.v), max(n.v), avg(n.v)") ==
          Strings{"5|4|10|1|4|d2.500000"});
    CHECK(s.rows("MATCH (n:Absent) RETURN count(*), count(n.v), sum(n.v), min(n.v), max(n.v), avg(n.v), "
                 "collect(n)") == Strings{"0|0|0|null|null|null|[]"});
    CHECK(s.rows("MATCH (n:Absent) RETURN n.x, count(*)").empty());
    CHECK(s.rows("MATCH (n:P) WITH n ORDER BY n.v RETURN collect(n.name)") == Strings{"[a,b,c]"});
    CHECK(s.rows("MATCH (n) WITH n ORDER BY n.v RETURN collect(n.ok)") == Strings{"[true,false]"});
    CHECK(s.set("MATCH (a)-[r]->(b) RETURN type(r), count(*)") == Strings{"X|2", "Y|2"});
    CHECK(s.rows("MATCH (a)-[r]->(b) RETURN type(r) AS t, count(*) AS c ORDER BY t DESC") ==
          Strings{"Y|2", "X|2"});
    CHECK(s.rows("MATCH (a)-[r]->(b) RETURN type(r), count(*) ORDER BY type(r)") == Strings{"X|2", "Y|2"});
    CHECK(s.rows("MATCH (n:P) RETURN count(*) + 1, 2 * count(*)") == Strings{"4|6"});
    CHECK(
        s.rows(
            "MATCH (a {name: 'b'})-[r]->(b) RETURN count(DISTINCT b), count(b), collect(DISTINCT b.name)") ==
        Strings{"1|2|[c]"});
    CHECK(s.rows("MATCH (a {name: 'b'})-[r]->(b) RETURN sum(DISTINCT b.v), avg(DISTINCT b.v), min(DISTINCT "
                 "b.v)") == Strings{"3|d3.000000|3"});
    CHECK(s.rows("WITH 1.5 AS a RETURN sum(a), avg(a)") == Strings{"d1.500000|d1.500000"});
    CHECK(s.rows("MATCH (n:P) RETURN sum(n.f)") == Strings{"d1.500000"});
    CHECK(s.rows("RETURN min('b'), max('b')") == Strings{"b|b"});
    check_rejected(s.graph, "MATCH (n:P) RETURN sum(n.name)", Error::Code::InvalidArgument);
    check_rejected(s.graph, "MATCH (n:P) RETURN avg(n.name)", Error::Code::InvalidArgument);
    // sum taşması yanlış sonuç vermez.
    GraphFixture g;
    must_write(g.graph, "CREATE (:N {v: 9223372036854775807}), (:N {v: 1})");
    check_rejected(g.graph, "MATCH (n:N) RETURN sum(n.v)", Error::Code::InvalidArgument);
}

TEST_CASE("labels_type_and_id_functions") {
    Sample s;
    CHECK(s.rows("MATCH (n:Q) RETURN labels(n)") == Strings{"[P,Q]"});
    CHECK(s.rows("MATCH (n:S) RETURN n.name, labels(n)") == Strings{"null|[S]"});
    CHECK(s.set("MATCH (:Q)-[r]->() RETURN type(r)") == Strings{"X"});
    CHECK(s.rows("RETURN labels(null), type(null), id(null)") == Strings{"null|null|null"});
    check_rejected(s.graph, "RETURN labels(1, 2)", Error::Code::Syntax);
    check_rejected(s.graph, "RETURN count()", Error::Code::Syntax);

    const Strings ids = s.rows("MATCH (n:Q) RETURN id(n)");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0].size() == 32);
    CHECK(s.rows("MATCH (n) WHERE id(n) = $i RETURN n.name", {{"i", ids[0]}}) == Strings{"a"});
    CHECK(s.rows("MATCH (n) WHERE id(n) = 'not-an-id' RETURN n.name").empty());
    CHECK(s.rows("MATCH (n) WHERE id(n) = 5 RETURN n.name").empty());
    const Strings edge = s.rows("MATCH (:Q)-[r]->() RETURN id(r)");
    REQUIRE(edge.size() == 1);
    CHECK(edge[0].size() == 32);
    // Düğüm değerinin kendisi de kimliktir.
    const QueryResult node = must_query(s.graph, "MATCH (n:Q) RETURN n");
    CHECK(std::holds_alternative<NodeId>(node.rows.at(0).at(0).v));
    CHECK(hex_of(std::get<NodeId>(node.rows[0][0].v).bytes) == ids[0]);
}

TEST_CASE("parameters_cover_every_value_type") {
    GraphFixture f;
    const Params params = {{"n", QueryValue{}}, {"b", true},          {"i", std::int64_t{42}}, {"d", 1.5},
                           {"s", "ç"},          {"y", Bytes{"\x01"}}, {"t", Timestamp{1000}}};
    CHECK(lines(must_query(f.graph, "RETURN $n, $b, $i, $d, $s, $y, $t", params)) ==
          Strings{"null|true|42|d1.500000|ç|b:01|t1000"});
    must_query(f.graph, "CREATE (:V {b: $b, i: $i, d: $d, s: $s, y: $y, t: $t})", params,
               QueryMode::ReadWrite);
    CHECK(lines(must_query(f.graph, "MATCH (n:V) RETURN n.b, n.i, n.d, n.s, n.y, n.t")) ==
          Strings{"true|42|d1.500000|ç|b:01|t1000"});
    CHECK(lines(must_query(f.graph, "MATCH (n:V) WHERE n.t = $t RETURN n.i", params)) == Strings{"42"});
}

TEST_CASE("limits_are_errors_not_truncated_results") {
    GraphFixture f;
    // Tam graf: 40 düğüm, her çift için kenar. *1..5 patlar.
    must_write(f.graph, "CREATE (:K {i: 0})");
    for (int i = 1; i < 40; ++i)
        must_query(f.graph, "MATCH (o:K) WITH collect(o) AS old CREATE (n:K {i: $i})",
                   {{"i", std::int64_t{i}}}, QueryMode::ReadWrite);
    must_query(f.graph, "MATCH (a:K), (b:K) WHERE a.i <> b.i CREATE (a)-[:E]->(b)", {}, QueryMode::ReadWrite);

    const auto expect_limit = [&](std::string_view text, QueryLimits limits, std::string_view limit) {
        const Result<QueryResult> result = f.graph.query(text, {}, QueryMode::ReadOnly, limits);
        INFO(std::string(text));
        REQUIRE_FALSE(result.has_value());
        INFO(describe_error(result.error()));
        CHECK(result.error().code == Error::Code::LimitExceeded);
        CHECK(arg_of(result.error(), "limit") == limit);
    };
    // Dev fan-out: varsayılan 1M genişlemede durur, asılı kalmaz ve kesilmiş sonuç vermez.
    // Süre bütçesi geniş tutulur: yavaş makinede (sanitizer, yük) hangi sınırın önce düştüğü sınanan şey
    // değil.
    QueryLimits fan_out;
    fan_out.max_duration = std::chrono::minutes(5);
    const auto start = std::chrono::steady_clock::now();
    expect_limit("MATCH (a:K)-[:E*1..10]->(b) RETURN count(*)", fan_out, "expansions");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(60));

    QueryLimits rows;
    rows.max_rows = 3;
    expect_limit("MATCH (a:K) RETURN a", rows, "rows");
    CHECK(must_query(f.graph, "MATCH (a:K) RETURN a LIMIT 3", {}, QueryMode::ReadOnly, rows).rows.size() ==
          3);

    QueryLimits expansions;
    expansions.max_expansions = 5;
    expect_limit("MATCH (a:K)-[:E]->(b) RETURN b", expansions, "expansions");

    QueryLimits memory;
    memory.max_memory_bytes = 1000;
    expect_limit("MATCH (a:K)-[:E]->(b) RETURN a, b ORDER BY b.i", memory, "memory");
    expect_limit("MATCH (a:K)-[:E]->(b) RETURN DISTINCT a, b", memory, "memory");
    expect_limit("MATCH (a:K)-[:E]->(b) RETURN a, collect(b)", memory, "memory");

    QueryLimits time;
    time.max_duration = std::chrono::milliseconds(0);
    time.max_expansions = 1'000'000'000;
    expect_limit("MATCH (a), (b), (c), (d) RETURN count(*)", time, "duration");
}

TEST_CASE("memo_budget_changes_speed_never_results") {
    Sample s;
    QueryLimits none;
    none.max_memo_bytes = 0;
    QueryLimits tiny;
    tiny.max_memo_bytes = 200;
    for (const std::string_view text :
         {"MATCH (a)-[*1..3]->(b) RETURN a.name, b.name, labels(b)",
          "MATCH (a)-[r]->(b)-[r2]-(c) RETURN r.w, c.v",
          "MATCH (a:P)-[:X|Y*1..2]-(b:P) RETURN a.v, b.v ORDER BY b.v DESC, a.v LIMIT 5"}) {
        const Strings base = lines(must_query(s.graph, text));
        CHECK_FALSE(base.empty());
        CHECK(lines(must_query(s.graph, text, {}, QueryMode::ReadOnly, none)) == base);
        CHECK(lines(must_query(s.graph, text, {}, QueryMode::ReadOnly, tiny)) == base);
    }
}

TEST_CASE("query_with_an_open_transaction_is_refused") {
    GraphFixture f;
    ReadTxn open_read = begin_read(f.graph);
    check_rejected(f.graph, "RETURN 1", Error::Code::InvalidArgument);
}

TEST_CASE("planner_starts_at_the_most_constrained_node_and_pushes_filters_down") {
    using detail::cypher::describe_plan;
    const auto plan = [](std::string_view text, const Params& params = {}) {
        const auto described = describe_plan(text, params);
        INFO(std::string(text));
        REQUIRE_MESSAGE(described.has_value(),
                        (described.has_value() ? "" : describe_error(described.error())));
        return *described;
    };
    // Tercih sırası: etiket > tüm düğüm; eşitlik etiketli > etiketli; eşitlikte soldaki.
    CHECK(plan("MATCH (a)-[:T]->(b:L) RETURN a") ==
          Strings{"Once", "LabelScan(b:L)", "Expand(b<-a)", "Project"});
    CHECK(plan("MATCH (a:K)-[:T]->(b:L) RETURN a") ==
          Strings{"Once", "LabelScan(a:K)", "Expand(a->b)", "Filter", "Project"});
    CHECK(plan("MATCH (a:K)-[:T]->(b:L {x: 1}) RETURN a") ==
          Strings{"Once", "LabelScan(b:L)", "Filter", "Expand(b<-a)", "Filter", "Project"});
    CHECK(plan("MATCH (a:K)-[:T]->(b:L) WHERE b.x = 1 RETURN a") ==
          Strings{"Once", "LabelScan(b:L)", "Filter", "Expand(b<-a)", "Filter", "Project"});
    CHECK(plan("MATCH (a)-[:T]->(b) RETURN a") == Strings{"Once", "NodeScan(a)", "Expand(a->b)", "Project"});
    // id(n) = ... etiketten önce gelir, tarama değil tek okumadır.
    CHECK(plan("MATCH (a:K)-[:T]->(b) WHERE id(b) = 'x' RETURN a") ==
          Strings{"Once", "IdSeek(b)", "Filter", "Expand(b<-a)", "Filter", "Project"});
    // Bağlı değişken en iyisidir: tarama yok.
    CHECK(plan("MATCH (x:K) MATCH (a:L)-[:T]->(x) RETURN a") ==
          Strings{"Once", "LabelScan(x:K)", "Expand(x<-a)", "Filter", "Project"});
    // WHERE, AND'den bölünüp en erken işlece iner.
    CHECK(
        plan(
            "MATCH (a:K)-[:T]->(b)-[:T]->(c) WHERE a.x = 1 AND b.y = 2 AND c.z = 3 AND a.x < c.z RETURN a") ==
        Strings{"Once", "LabelScan(a:K)", "Filter", "Expand(a->b)", "Filter", "Expand(b->c)", "Filter",
                "Filter", "Project"});
    // Üretim sorgusu: sıralama yansıtmadan önce (satır başına ad okunmaz), top-K.
    CHECK(plan("MATCH (n:Note)-[:LINKS_TO*1..2]->(m) WHERE n.origin = \"windows\" RETURN m.name "
               "ORDER BY m.updated_at DESC LIMIT 20") == Strings{"Once", "LabelScan(n:Note)", "Filter",
                                                                 "VarLengthExpand(n->m)", "Sort", "Limit",
                                                                 "Project"});
    // Bağlı iki uç: ExpandInto.
    CHECK(plan("MATCH (a:K), (b:L) MATCH (a)-[:T]->(b) RETURN a") ==
          Strings{"Once", "LabelScan(a:K)", "LabelScan(b:L)", "ExpandInto(a->b)", "Project"});
    CHECK(plan("MATCH (n) RETURN DISTINCT n.x ORDER BY n.x SKIP 1 LIMIT 2") ==
          Strings{"Once", "NodeScan(n)", "Project", "Distinct", "Sort", "Skip", "Limit"});
    CHECK(plan("MATCH (n) RETURN n.x, count(*)") == Strings{"Once", "NodeScan(n)", "Aggregate"});
    CHECK(plan("OPTIONAL MATCH (n) RETURN n") ==
          Strings{"Once", "OptionalApply[Once,NodeScan(n)]", "Project"});
    CHECK(plan("CREATE (a)") == Strings{"Once", "Create"});
    CHECK(plan("MERGE (a:L {x: 1})") == Strings{"Once", "Merge[Once,LabelScan(a:L),Filter]"});
    CHECK(plan("MATCH (a) SET a.x = 1 REMOVE a.y DELETE a") ==
          Strings{"Once", "NodeScan(a)", "Set", "Remove", "Delete"});
}
