// Ayrıştırıcı sınırı: desteklenmeyen sözdizimi konumlu reddedilir, düşmanca girdi crash/hang/yanlış `ok`
// vermez.
#include "cypher_support.hpp"
#include "graph/cypher/parser.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::test;

namespace {

void check_position(Graph& graph, std::string_view text, Error::Code code, int line, int col) {
    INFO(std::string(text));
    const Result<QueryResult> result = graph.query(text);
    REQUIRE_FALSE(result.has_value());
    INFO(describe_error(result.error()));
    CHECK(result.error().code == code);
    CHECK(arg_of(result.error(), "line") == std::to_string(line));
    CHECK(arg_of(result.error(), "col") == std::to_string(col));
}

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i)
        out += text;
    return out;
}

}  // namespace

TEST_CASE("load_csv_rejected_with_position") {
    GraphFixture f;
    check_position(f.graph, "LOAD CSV FROM 'file:///etc/passwd' AS x RETURN x", Error::Code::Unsupported, 1,
                   1);
    check_position(f.graph, "MATCH (n)\n  LOAD CSV FROM 'http://x/y' AS x RETURN x", Error::Code::Unsupported,
                   2, 3);
    check_position(f.graph, "load csv with headers from 'file:///x' as y return y", Error::Code::Unsupported,
                   1, 1);
}

TEST_CASE("unsupported_syntax_is_reported_with_position") {
    GraphFixture f;
    check_position(f.graph, "CALL db.labels()", Error::Code::Unsupported, 1, 1);
    check_position(f.graph, "UNWIND [1,2] AS x RETURN x", Error::Code::Unsupported, 1, 1);
    check_position(f.graph, "MATCH (n) CALL { RETURN 1 } RETURN n", Error::Code::Unsupported, 1, 11);
    check_position(f.graph, "MATCH p = shortestPath((a)-[*]-(b)) RETURN p", Error::Code::Unsupported, 1, 7);
    check_position(f.graph, "MATCH (n) RETURN evil(n)", Error::Code::Unsupported, 1, 18);
    check_position(f.graph, "RETURN exists((a)-[]->(b))", Error::Code::Unsupported, 1, 8);
    check_position(f.graph, "MATCH (n) WHERE EXISTS { MATCH (n)-->() } RETURN n", Error::Code::Unsupported, 1,
                   17);
    check_position(f.graph, "RETURN COUNT { MATCH (n) RETURN n }", Error::Code::Unsupported, 1, 8);
    check_position(f.graph, "MATCH shortestPath((a)-[*]-(b)) RETURN a", Error::Code::Unsupported, 1, 7);
    check_position(f.graph, "RETURN CASE WHEN true THEN 1 END", Error::Code::Unsupported, 1, 8);
    check_position(f.graph, "MATCH (n) WITH n RETURN n UNION MATCH (n) RETURN n", Error::Code::Unsupported, 1,
                   27);
    check_position(f.graph, "CREATE INDEX FOR (n:A) ON (n.x)", Error::Code::Unsupported, 1, 8);
    check_position(f.graph, "DROP INDEX x", Error::Code::Unsupported, 1, 1);
    check_position(f.graph, "MERGE (n:A) ON CREATE SET n.x = 1", Error::Code::Unsupported, 1, 13);
    check_position(f.graph, "MATCH (n) SET n = {a: 1}", Error::Code::Unsupported, 1, 17);
    check_position(f.graph, "MATCH (n) SET n += {a: 1}", Error::Code::Unsupported, 1, 17);
    check_position(f.graph, "RETURN [1,2][0]", Error::Code::Unsupported, 1, 13);
    check_position(f.graph, "RETURN {a: 1}", Error::Code::Unsupported, 1, 8);
    check_position(f.graph, "RETURN 1 = 1 = 1", Error::Code::Unsupported, 1, 14);
    check_position(f.graph, "RETURN true XOR false", Error::Code::Unsupported, 1, 13);
}

TEST_CASE("positions_count_lines_and_code_points") {
    GraphFixture f;
    // "ğ" iki bayttır ama tek sütundur.
    check_position(f.graph, "RETURN \"ğğ\", )", Error::Code::Syntax, 1, 14);
    check_position(f.graph, "MATCH (n)\nRETURN n,\n  )", Error::Code::Syntax, 3, 3);
}

TEST_CASE("syntax_errors_carry_position_and_never_succeed") {
    GraphFixture f;
    for (const std::string_view text : {"",
                                        "   ",
                                        ";",
                                        "MATCH",
                                        "MATCH (n)",
                                        "MATCH (n) RETURN",
                                        "RETURN",
                                        "RETURN 1 2",
                                        "RETURN 1;;",
                                        "RETURN 'abc",
                                        "RETURN `abc",
                                        "RETURN /* never closed",
                                        "RETURN \"\\q\"",
                                        "RETURN \"\\uD800\"",
                                        "RETURN 1 RETURN 2",
                                        "RETURN 1 MATCH (n) RETURN n",
                                        "WITH 1",
                                        "WITH 1 AS a",
                                        "MATCH (n RETURN n",
                                        "MATCH (n)-[ RETURN n",
                                        "MATCH (n)-[r]- RETURN n",
                                        "MATCH (n {a: 1, a: 2}) RETURN n",
                                        "RETURN 1 AS a, 2 AS a",
                                        "SET",
                                        "DELETE",
                                        "RETURN $",
                                        "RETURN @",
                                        "RETURN 1 ORDER n",
                                        "MATCH (n)-[r:A|]->(m) RETURN n",
                                        "RETURN 1e",
                                        "RETURN .5.",
                                        "RETURN 1 != 2"}) {
        INFO(std::string(text));
        const Result<QueryResult> result = f.graph.query(text);
        REQUIRE_FALSE(result.has_value());
        INFO(describe_error(result.error()));
        CHECK(result.error().code == Error::Code::Syntax);
        CHECK_FALSE(arg_of(result.error(), "line").empty());
        CHECK_FALSE(arg_of(result.error(), "col").empty());
    }
}

TEST_CASE("undefined_variables_are_errors_not_nulls") {
    GraphFixture f;
    check_rejected(f.graph, "RETURN x", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) WHERE m.x = 1 RETURN n", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) WITH n AS a RETURN n", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) RETURN DISTINCT n.a ORDER BY n.b", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) RETURN count(*) ORDER BY n.b", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) RETURN n.a + count(*)", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (n) WHERE count(*) > 1 RETURN n", Error::Code::Syntax);
    check_rejected(f.graph, "RETURN sum(count(*))", Error::Code::Syntax);
}

TEST_CASE("missing_parameter_is_an_error_with_position") {
    GraphFixture f;
    check_position(f.graph, "MATCH (n) WHERE n.x = $absent RETURN n", Error::Code::InvalidArgument, 1, 23);
}

TEST_CASE("variable_length_bounds_are_handled_clearly") {
    GraphFixture f;
    // GraphQLite'ta `*1..$h` sessizce yanlış sonuç veriyordu.
    check_position(f.graph, "MATCH (a)-[*1..$h]->(b) RETURN b", Error::Code::Unsupported, 1, 16);
    check_position(f.graph, "MATCH (a)-[*$h]->(b) RETURN b", Error::Code::Unsupported, 1, 13);
    check_rejected(f.graph, "MATCH (a)-[*]->(b) RETURN b", Error::Code::Unsupported);
    check_rejected(f.graph, "MATCH (a)-[*1..]->(b) RETURN b", Error::Code::Unsupported);
    check_rejected(f.graph, "MATCH (a)-[*0..2]->(b) RETURN b", Error::Code::Unsupported);
    check_rejected(f.graph, "MATCH (a)-[*1..11]->(b) RETURN b", Error::Code::LimitExceeded);
    check_rejected(f.graph, "MATCH (a)-[*1..99999999999999999999]->(b) RETURN b", Error::Code::LimitExceeded);
    check_rejected(f.graph, "MATCH (a)-[*3..1]->(b) RETURN b", Error::Code::Syntax);
    check_rejected(f.graph, "MATCH (a)-[r:X*1..2 {k: 1}]->(b) RETURN b", Error::Code::Unsupported);
}

TEST_CASE("integer_literals_and_arithmetic_never_overflow_silently") {
    GraphFixture f;
    // GraphQLite'ın bulduğu sınıf: INT64_MIN / -1 donanımda SIGFPE'dir.
    check_rejected(f.graph, "RETURN (-9223372036854775807 - 1) / -1", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN (-9223372036854775807 - 1) % -1", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN 9223372036854775807 + 1", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN 9223372036854775807 * 2", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN -9223372036854775807 - 2", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN -(-9223372036854775807 - 1)", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN 1 / 0", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN 1 % 0", Error::Code::InvalidArgument);
    check_rejected(f.graph, "RETURN 9223372036854775808", Error::Code::Syntax);
    check_rejected(f.graph, "RETURN 1e999", Error::Code::Syntax);
    CHECK(lines(must_query(f.graph, "RETURN -9223372036854775808")) == Strings{"-9223372036854775808"});
    CHECK(lines(must_query(f.graph, "RETURN 7 / 2, 7 % 3, 7.0 / 2, -7 / 2")) == Strings{"3|1|d3.500000|-3"});
    CHECK(lines(must_query(f.graph, "RETURN 1 + 2 * 3, (1 + 2) * 3, 2 - 1 - 1")) == Strings{"7|9|0"});
}

TEST_CASE("oversized_tokens_and_queries_are_limit_errors") {
    GraphFixture f;
    check_rejected(f.graph, "RETURN '" + std::string(70000, 'x') + "'", Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN " + std::string(70000, 'x'), Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN " + std::string(70000, '9'), Error::Code::LimitExceeded);
    check_rejected(f.graph, std::string(70000, ' ') + "RETURN 1", Error::Code::LimitExceeded);
    check_rejected(f.graph, "MATCH (n:" + std::string(60000, 'L') + ") RETURN n", Error::Code::LimitExceeded);
    check_rejected(f.graph, "MATCH (n) RETURN n." + std::string(60000, 'k'), Error::Code::LimitExceeded);
    // 64 KiB'in altında uzun metin kabul edilir.
    const QueryResult ok = must_query(f.graph, "RETURN '" + std::string(60000, 'x') + "' AS s");
    CHECK(std::get<std::string>(ok.rows.at(0).at(0).v).size() == 60000);
    // Yakın sınır: tam 64 KiB geçer, bir bayt fazlası geçmez.
    const std::string shell = "RETURN ''";
    must_query(f.graph, "RETURN '" + std::string(65536 - shell.size(), 'y') + "'");
    check_rejected(f.graph, "RETURN '" + std::string(65537 - shell.size(), 'y') + "'",
                   Error::Code::LimitExceeded);
}

TEST_CASE("deep_nesting_and_long_chains_are_limit_errors_not_stack_overflows") {
    GraphFixture f;
    check_rejected(f.graph, "RETURN " + repeat("(", 30000) + "1" + repeat(")", 30000),
                   Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN " + repeat("[", 30000) + repeat("]", 30000), Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN " + repeat("NOT ", 16000) + "true", Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN " + repeat("- ", 30000) + "1", Error::Code::LimitExceeded);
    // Sola derin zincir parantezsizdir ama ağaç yüksekliği yine sınırdadır: değerlendirme yığını taşırmasın.
    check_rejected(f.graph, "RETURN 1" + repeat("+1", 30000), Error::Code::LimitExceeded);
    check_rejected(f.graph, "RETURN true" + repeat(" OR true", 8000), Error::Code::LimitExceeded);
    check_rejected(f.graph, "MATCH (a)" + repeat("-->()", 300) + " RETURN a", Error::Code::LimitExceeded);
    check_rejected(f.graph, repeat("MATCH (a) ", 300) + "RETURN a", Error::Code::LimitExceeded);
    check_rejected(f.graph, repeat("WITH 1 AS a ", 300) + "RETURN a", Error::Code::LimitExceeded);
    // Sınırın altı geçer.
    CHECK(lines(must_query(f.graph, "RETURN " + repeat("(", 60) + "1" + repeat(")", 60))) == Strings{"1"});
}

// L4: ayrıştırma ve plan sınırları da QueryLimits'tedir (config limits.query.* / limits.view.*); SIZE_MAX
// sınırsızdır. Varsayılanlar yukarıdaki testlerin bugünkü değerleridir.
TEST_CASE("parser_and_planner_limits_come_from_query_limits_and_can_be_lifted") {
    GraphFixture f;
    const std::string long_text = "RETURN '" + std::string(70000, 'x') + "' AS s";
    const std::string many_clauses = repeat("WITH 1 AS a ", 100) + "RETURN a";
    const std::string deep = "RETURN " + repeat("(", 100) + "1" + repeat(")", 100);
    const std::string long_path = "MATCH (a)-[*1..20]->(b) RETURN count(*)";
    const std::string many_elements = "MATCH (a)" + repeat("-->()", 300) + " RETURN count(*)";
    for (const std::string& text : {long_text, many_clauses, deep, long_path, many_elements})
        check_rejected(f.graph, text, Error::Code::LimitExceeded);

    QueryLimits open;
    open.max_text_bytes = SIZE_MAX;
    open.max_clauses = SIZE_MAX;
    open.max_nesting = SIZE_MAX;
    open.max_var_length_hops = SIZE_MAX;
    open.max_pattern_elements = SIZE_MAX;
    CHECK(std::get<std::string>(
              must_query(f.graph, long_text, {}, QueryMode::ReadOnly, open).rows.at(0).at(0).v)
              .size() == 70000);
    CHECK(lines(must_query(f.graph, many_clauses, {}, QueryMode::ReadOnly, open)) == Strings{"1"});
    CHECK(lines(must_query(f.graph, deep, {}, QueryMode::ReadOnly, open)) == Strings{"1"});
    CHECK(lines(must_query(f.graph, long_path, {}, QueryMode::ReadOnly, open)) == Strings{"0"});
    CHECK(lines(must_query(f.graph, many_elements, {}, QueryMode::ReadOnly, open)) == Strings{"0"});

    QueryLimits tight;
    tight.max_clauses = 2;
    const Result<QueryResult> refused =
        f.graph.query("WITH 1 AS a WITH a AS b RETURN b", {}, QueryMode::ReadOnly, tight);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::LimitExceeded);
}

// Satır sınırı SIZE_MAX'ta kalkar: sınırsız kullanıcı sorgusu bugünkü tavanı aşan sonucu verir.
TEST_CASE("unlimited_rows_return_more_than_the_old_ceiling") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:N), (:N), (:N), (:N), (:N)");
    QueryLimits capped;
    capped.max_rows = 3;
    const Result<QueryResult> refused =
        f.graph.query("MATCH (n:N) RETURN n", {}, QueryMode::ReadOnly, capped);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::LimitExceeded);
    QueryLimits open;
    open.max_rows = SIZE_MAX;
    open.max_duration = std::chrono::milliseconds::max();
    CHECK(must_query(f.graph, "MATCH (n:N) RETURN n", {}, QueryMode::ReadOnly, open).rows.size() == 5);
}

TEST_CASE("list_doubling_through_with_chain_is_bounded") {
    GraphFixture f;
    // Sorgu 64 KiB'ten küçük ama değer üstel büyürdü.
    std::string text = "WITH [1,2,3,4,5,6,7,8,9,0] AS a ";
    for (int i = 0; i < 40; ++i)
        text += "WITH [a,a,a,a,a,a,a,a,a,a] AS a ";
    const auto start = std::chrono::steady_clock::now();
    check_rejected(f.graph, text + "RETURN a", Error::Code::LimitExceeded);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
}

TEST_CASE("parser_entry_takes_a_raw_buffer_for_fuzzing") {
    // NUL ve geçersiz UTF-8: hedef (const char*, size_t), metin sonlandırıcısına güvenmez.
    const char with_nul[] = {'R', 'E', 'T', 'U', 'R', 'N', ' ', '1', '\0', ' ', 'x'};
    const auto parsed_nul = detail::cypher::parse_query(with_nul, sizeof with_nul);
    REQUIRE_FALSE(parsed_nul.has_value());
    CHECK(parsed_nul.error().code == Error::Code::Syntax);

    const std::string bad_utf8 = "RETURN '\xC3('";
    const auto parsed_utf8 = detail::cypher::parse_query(bad_utf8.data(), bad_utf8.size());
    REQUIRE_FALSE(parsed_utf8.has_value());
    CHECK(parsed_utf8.error().code == Error::Code::Syntax);

    const std::string bad_name = "RETURN \xFF";
    CHECK_FALSE(detail::cypher::parse_query(bad_name.data(), bad_name.size()).has_value());

    CHECK_FALSE(detail::cypher::parse_query(nullptr, 0).has_value());
    const std::string fine = "MATCH (n:A)-[:T*1..2]->(m) WHERE n.x = 1 RETURN m ORDER BY m.y DESC LIMIT 3";
    CHECK(detail::cypher::parse_query(fine.data(), fine.size()).has_value());
}

TEST_CASE("comments_strings_and_escapes") {
    GraphFixture f;
    CHECK(lines(must_query(f.graph, "// baş\nRETURN /* orta */ 1 // son")) == Strings{"1"});
    CHECK(lines(must_query(f.graph, R"(RETURN 'a\'b', "c\"d", 'x\ny', '\u00e7', '\\')")) ==
          Strings{"a'b|c\"d|x\ny|ç|\\"});
    CHECK(lines(must_query(f.graph, "RETURN 'ğü' AS `sütun adı`")) == Strings{"ğü"});
    CHECK(must_query(f.graph, "RETURN 'ğü' AS `sütun adı`").columns == Strings{"sütun adı"});
    CHECK(must_query(f.graph, "RETURN 1 + 1, 2 AS b").columns == Strings{"1 + 1", "b"});
}

// Tam fuzz G3b'dedir; bu test sanitizer'lı her koşuda düşmanca dizilimleri uçtan uca yürütür. Sonuç
// umursanmaz: geçerli ve geçersiz sorgu da gelir, istenen crash ve asılı kalma olmaması. Yazma modu da
// dahildir: hatada geri alır.
TEST_CASE("random_token_sequences_never_crash_or_hang") {
    GraphFixture f;
    must_write(f.graph,
               "CREATE (a:A {x: 1})-[:T {w: 1}]->(b:B {x: 'y'}), (b)-[:T]->(a), (a)-[:U]->(a), (:C)");
    const std::vector<std::string_view> tokens = {"MATCH",
                                                  "OPTIONAL",
                                                  "WHERE",
                                                  "RETURN",
                                                  "WITH",
                                                  "CREATE",
                                                  "MERGE",
                                                  "SET",
                                                  "REMOVE",
                                                  "DELETE",
                                                  "DETACH",
                                                  "ORDER BY",
                                                  "SKIP",
                                                  "LIMIT",
                                                  "AS",
                                                  "DISTINCT",
                                                  "AND",
                                                  "OR",
                                                  "NOT",
                                                  "IN",
                                                  "IS NULL",
                                                  "IS NOT NULL",
                                                  "CONTAINS",
                                                  "STARTS WITH",
                                                  "ENDS WITH",
                                                  "DESC",
                                                  "null",
                                                  "true",
                                                  "false",
                                                  "count",
                                                  "sum",
                                                  "min",
                                                  "max",
                                                  "avg",
                                                  "collect",
                                                  "labels",
                                                  "type",
                                                  "id",
                                                  "(",
                                                  ")",
                                                  "[",
                                                  "]",
                                                  "{",
                                                  "}",
                                                  ":",
                                                  ",",
                                                  ".",
                                                  "..",
                                                  "|",
                                                  "=",
                                                  "<>",
                                                  "<",
                                                  ">",
                                                  "<=",
                                                  ">=",
                                                  "+",
                                                  "-",
                                                  "*",
                                                  "/",
                                                  "%",
                                                  ";",
                                                  "(n)",
                                                  "(m)",
                                                  "(a:A)",
                                                  "(b:B {x: 'y'})",
                                                  "-[r:T]->",
                                                  "<-[:U]-",
                                                  "--",
                                                  "-[*1..3]-",
                                                  "-[p:T*1..2]->",
                                                  "n",
                                                  "m",
                                                  "r",
                                                  "p",
                                                  "a",
                                                  "b",
                                                  "n.x",
                                                  "m.w",
                                                  "r.w",
                                                  "$v",
                                                  "$l",
                                                  "$missing",
                                                  "0",
                                                  "1",
                                                  "-1",
                                                  "2.5",
                                                  "9223372036854775807",
                                                  "'s'",
                                                  "''",
                                                  "`q`",
                                                  "RETURN n",
                                                  "RETURN *",
                                                  "RETURN 1",
                                                  "count(*)",
                                                  "UNION",
                                                  "CALL",
                                                  "LOAD CSV",
                                                  "UNWIND"};
    const std::vector<std::string_view> clauses = {"MATCH (n)",
                                                   "MATCH (n:A)",
                                                   "MATCH (n)-[r:T]->(m)",
                                                   "MATCH (n)-[p:T*1..3]-(m)",
                                                   "MATCH (n)<-[]-(m), (m)-->(o)",
                                                   "OPTIONAL MATCH (n)-[r:U]->(m)",
                                                   "OPTIONAL MATCH (z:Absent)",
                                                   "WHERE n.x = 1",
                                                   "WHERE n.x IN $l OR m.x IS NULL",
                                                   "WHERE id(n) = 'x'",
                                                   "WHERE NOT n:A",
                                                   "WITH n",
                                                   "WITH n, count(*) AS c",
                                                   "WITH collect(n) AS l",
                                                   "WITH *",
                                                   "WITH DISTINCT n.x AS n",
                                                   "RETURN n",
                                                   "RETURN *",
                                                   "RETURN count(*)",
                                                   "RETURN n.x, m.x",
                                                   "RETURN labels(n), id(n)",
                                                   "RETURN DISTINCT n",
                                                   "RETURN sum(n.x), avg(n.x)",
                                                   "RETURN n.x / 0",
                                                   "ORDER BY n.x",
                                                   "ORDER BY n.x DESC, m.x",
                                                   "SKIP 1",
                                                   "LIMIT 2",
                                                   "LIMIT $v",
                                                   "SKIP $missing",
                                                   "CREATE (x:A {x: 1})",
                                                   "CREATE (n)-[:T]->(y)",
                                                   "MERGE (:A {x: 1})",
                                                   "MERGE (n)-[:T]->(m)",
                                                   "MERGE (:A {x: $l})",
                                                   "SET n.x = 2",
                                                   "SET n:Q, n.y = n.x + 1",
                                                   "SET m.x = null",
                                                   "REMOVE n.x, n:A",
                                                   "DELETE n",
                                                   "DELETE r",
                                                   "DETACH DELETE n",
                                                   "DELETE p"};
    const Params params = {{"v", std::int64_t{1}},
                           {"l", QueryList{{QueryValue(std::int64_t{1}), QueryValue("x")}}}};
    QueryLimits limits;
    limits.max_duration = std::chrono::milliseconds(200);
    limits.max_expansions = 20'000;
    limits.max_memory_bytes = 4U * 1024U * 1024U;
    std::mt19937 random(20261101);
    std::size_t accepted = 0;
    constexpr int kRounds = 6000;
    for (int round = 0; round < kRounds; ++round) {
        std::string text;
        // Yarısı cümle parçası: yüzeyde kalmayıp planlayıcı ve işleçlere ulaşır. Yarısı saf belirteç
        // karışımı.
        const bool whole_clauses = random() % 2 == 0;
        const std::vector<std::string_view>& pieces = whole_clauses ? clauses : tokens;
        const std::size_t count = 1 + random() % (whole_clauses ? 6 : 14);
        for (std::size_t i = 0; i < count; ++i)
            text += std::string(pieces[random() % pieces.size()]) + " ";
        const QueryMode mode = random() % 2 == 0 ? QueryMode::ReadOnly : QueryMode::ReadWrite;
        INFO(text);
        const Result<QueryResult> result = f.graph.query(text, params, mode, limits);
        // Hata her zaman bir işlem bırakmadan döner: sonraki sorgu açılabilmeli.
        REQUIRE_FALSE(result.error_or(Error{}).code == Error::Code::Corrupt);
        accepted += result.has_value() ? 1U : 0U;
    }
    MESSAGE("accepted " << accepted << " of " << kRounds);
    // Hepsi reddedildiyse test yalnız ayrıştırıcıyı yoklamıştır: yanlış yeşil.
    CHECK(accepted > 50);
    CHECK(must_query(f.graph, "RETURN 1").rows.size() == 1);
}
