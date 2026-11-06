// Yazma cümleleri, salt-okunur savunması ve cihazlar arası yakınsayan MERGE.
#include "cypher_support.hpp"
#include "graph/store.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::test;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kWriteClauses[] = {
    "CREATE (n)",
    "CREATE (a:L {k: 1})-[:T]->(b)",
    "MERGE (n:L {k: 1})",
    "MATCH (n) SET n.k = 1",
    "MATCH (n) SET n:L",
    "MATCH (n) REMOVE n.k",
    "MATCH (n) REMOVE n:L",
    "MATCH (n) DELETE n",
    "MATCH (n) DETACH DELETE n",
    "MATCH ()-[r]->() DELETE r",
    "MATCH (n) WITH n SET n.k = 1 RETURN n",
    "MATCH (n) WITH n CREATE (m) RETURN n, m",
    "OPTIONAL MATCH (n) SET n.k = 1",
    "create (n)",
    "CrEaTe (n)",
};

std::int64_t ops_count(const fs::path& db) {
    RawDb raw(db);
    return raw.scalar("SELECT count(*) FROM ops");
}

// Mantıksal içerik: düğüm, etiket, kenar ve özellik satırları.
Strings logical_dump(const fs::path& db) {
    RawDb raw(db);
    Strings dump;
    for (const char* query :
         {"SELECT hex(id) FROM nodes ORDER BY id", "SELECT hex(node), label FROM labels ORDER BY 1, 2",
          "SELECT hex(id), hex(src), type, hex(dst) FROM edges ORDER BY id",
          "SELECT hex(owner), key, hex(value) FROM props ORDER BY 1, 2, 3"})
        for (std::string& row : raw.rows(query))
            dump.push_back(std::move(row));
    return dump;
}

std::string file_bytes(const fs::path& path) {
    return fs::exists(path) ? noteapp::test::slurp(path) : std::string("<absent>");
}

}  // namespace

TEST_CASE("read_only_rejects_every_write_clause_and_file_bytes_unchanged") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:L {k: 1})-[:T {w: 1}]->(b:L {k: 2})");
    const Strings before = logical_dump(f.path());
    const std::int64_t ops_before = ops_count(f.path());
    REQUIRE(ops_before > 0);
    const fs::path wal = f.path().string() + "-wal";
    const std::string db_bytes = file_bytes(f.path());
    const std::string wal_bytes = file_bytes(wal);

    for (const std::string_view text : kWriteClauses) {
        INFO(std::string(text));
        const Result<QueryResult> result = f.graph.query(text);
        REQUIRE_FALSE(result.has_value());
        INFO(describe_error(result.error()));
        CHECK(result.error().code == Error::Code::ReadOnlyViolation);
        CHECK_FALSE(arg_of(result.error(), "line").empty());
        CHECK_FALSE(arg_of(result.error(), "col").empty());
    }
    // Plan aşamasında reddedildi: işlem açılmadı, dosyaya dokunulmadı. Bağlantı açıkken bayt bayt aynı.
    CHECK(file_bytes(f.path()) == db_bytes);
    CHECK(file_bytes(wal) == wal_bytes);
    // Reddedilen sorgudan sonra bağlantı yazmaya kapalı kalmaz, işlem de açık kalmaz.
    CHECK(f.graph.read().has_value());
    CHECK(logical_dump(f.path()) == before);
    CHECK(ops_count(f.path()) == ops_before);

    // Başarılı salt-okunur sorgu mantıksal durumu ve ops'u değiştirmez. Dosya baytları burada sınanmaz:
    // bağlantı kapanışı WAL'ı checkpoint eder, query_only bunu durdurmaz (SPEC-graph "Sorgu dili").
    CHECK(must_query(f.graph, "MATCH (a)-[r]->(b) RETURN a.k, r.w, b.k").rows.size() == 1);
    CHECK(logical_dump(f.path()) == before);
    CHECK(ops_count(f.path()) == ops_before);
}

// Saklanan görünümün kayıttaki denetimi (T81): depo olmadan aynı kural, aynı hata.
TEST_CASE("check_query_rejects_every_write_clause_without_a_database") {
    for (const std::string_view text : kWriteClauses) {
        INFO(std::string(text));
        const Result<void> checked = check_query(text, QueryMode::ReadOnly);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().code == Error::Code::ReadOnlyViolation);
        CHECK_FALSE(arg_of(checked.error(), "line").empty());
        CHECK(check_query(text, QueryMode::ReadWrite).has_value());
    }
    CHECK(check_query("MATCH (n:Note) RETURN n ORDER BY n.name LIMIT 5", QueryMode::ReadOnly).has_value());
    CHECK(check_query("MATCH (n RETURN n", QueryMode::ReadOnly).error().code == Error::Code::Syntax);
    const Result<void> param = check_query("MATCH (n) WHERE n.k = $k RETURN n", QueryMode::ReadOnly);
    REQUIRE_FALSE(param.has_value());
    CHECK(param.error().message_key == "graph.cypher_missing_param");
    CHECK(check_query("MATCH (n) WHERE n.k = $k RETURN n", QueryMode::ReadOnly, {{"k", std::int64_t{1}}})
              .has_value());
}

TEST_CASE("query_only_is_a_real_second_defense_on_the_connection") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:L {k: 1})");
    const std::int64_t ops_before = ops_count(f.path());

    Result<std::unique_ptr<detail::Store>> store = detail::Store::open(f.path());
    REQUIRE(store.has_value());
    GRAPH_OK((*store)->set_query_only(true));
    // Plan denetimi atlansa bile yazma düşer.
    const Result<void> begun = (*store)->begin_write();
    if (begun.has_value()) {
        const Result<void> recorded = (*store)->record(detail::NodePut{node_named("x")});
        CHECK_FALSE(recorded.has_value());
        (*store)->end_txn();
    }
    GRAPH_OK((*store)->set_query_only(false));
    CHECK(ops_count(f.path()) == ops_before);

    // Graph::query okumadan sonra bağlantıyı yazmaya geri açar.
    CHECK(must_query(f.graph, "MATCH (n:L) RETURN n.k").rows.size() == 1);
    must_write(f.graph, "CREATE (:L {k: 2})");
    CHECK(must_query(f.graph, "MATCH (n:L) RETURN n.k").rows.size() == 2);
}

TEST_CASE("read_write_mode_runs_read_queries_without_touching_ops") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:L)");
    const std::int64_t ops_before = ops_count(f.path());
    CHECK(must_write(f.graph, "MATCH (n:L) RETURN count(*)").rows.at(0).at(0) == QueryValue(std::int64_t{1}));
    CHECK(ops_count(f.path()) == ops_before);
}

TEST_CASE("create_builds_nodes_edges_labels_and_properties") {
    GraphFixture f;
    const QueryResult created = must_write(
        f.graph, "CREATE (a:A:B {x: 1, y: 'ç'})-[r:T {w: 2.5}]->(b:C) RETURN a.x, r.w, labels(b), type(r)");
    CHECK(lines(created) == Strings{"1|d2.500000|[C]|T"});
    CHECK(lines(must_query(f.graph, "MATCH (a:A:B)-[r:T]->(b:C) RETURN a.y, labels(a), r.w")) ==
          Strings{"ç|[A,B]|d2.500000"});
    // Ters yön ve bağlı değişken.
    must_write(f.graph, "MATCH (a:A), (b:C) CREATE (a)<-[:U]-(b), (a)-[:V]->(:D)");
    CHECK(lines(must_query(f.graph, "MATCH (:C)-[r:U]->(:A) RETURN type(r)")) == Strings{"U"});
    CHECK(lines(must_query(f.graph, "MATCH (:A)-[r:V]->(:D) RETURN type(r)")) == Strings{"V"});
    // Null özellik yazılmaz; önceki satırın değişkenleri sonraki desende görünür.
    must_write(f.graph, "CREATE (a:E {x: null, y: 1}), (b:E {y: a.y + 1})");
    CHECK(sorted(lines(must_query(f.graph, "MATCH (e:E) RETURN e.x, e.y"))) == Strings{"null|1", "null|2"});
    // Her girdi satırı için bir kez.
    must_write(f.graph, "MATCH (e:E) CREATE (:F {y: e.y})");
    CHECK(must_query(f.graph, "MATCH (n:F) RETURN n").rows.size() == 2);
    // Bir yazma sorgusu tek çerçevedir: hepsi aynı işlem.
    CHECK(must_query(f.graph, "MATCH (n) RETURN count(*)").rows.at(0).at(0) == QueryValue(std::int64_t{7}));
}

TEST_CASE("create_rejects_what_it_cannot_store") {
    GraphFixture f;
    const auto write = QueryMode::ReadWrite;
    check_rejected(f.graph, "CREATE (a)-[:T]-(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "CREATE (a)-[]->(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "CREATE (a)-[:T|U]->(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "CREATE (a)-[:T*1..2]->(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "MATCH (a) CREATE (a:L)", Error::Code::Syntax, write);
    check_rejected(f.graph, "CREATE (a), (a)", Error::Code::Syntax, write);
    check_rejected(f.graph, "CREATE (a {x: [1]})", Error::Code::InvalidArgument, write);
    check_rejected(f.graph, "CREATE (a {x: labels(a)})", Error::Code::InvalidArgument, write);
    check_rejected(f.graph, "OPTIONAL MATCH (x:Absent) CREATE (x)-[:T]->(y)", Error::Code::InvalidArgument,
                   write);
    CHECK(must_query(f.graph, "MATCH (n) RETURN count(*)").rows.at(0).at(0) == QueryValue(std::int64_t{0}));
}

TEST_CASE("a_failing_write_query_leaves_no_trace") {
    GraphFixture f;
    must_write(f.graph, "CREATE (:L {v: 1}), (:L {v: 0})");
    const Strings before = logical_dump(f.path());
    const std::int64_t ops_before = ops_count(f.path());
    // İlk satırlar yazıldıktan sonra çalışma anı hatası: 1/0.
    check_rejected(f.graph, "MATCH (n:L) CREATE (:M {x: 1 / n.v})", Error::Code::InvalidArgument,
                   QueryMode::ReadWrite);
    check_rejected(f.graph, "MATCH (n:L) SET n.v = 5 WITH n SET n.w = 1 / 0", Error::Code::InvalidArgument,
                   QueryMode::ReadWrite);
    CHECK(logical_dump(f.path()) == before);
    CHECK(ops_count(f.path()) == ops_before);
    // Bağlantı kullanılabilir kalır.
    must_write(f.graph, "CREATE (:L {v: 9})");
}

TEST_CASE("set_and_remove_properties_and_labels") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:A {x: 1, y: 2})-[:T {w: 1}]->(b:B)");
    must_write(f.graph, "MATCH (a:A) SET a.x = a.x + 10, a.z = 'yeni', a:C:D");
    CHECK(lines(must_query(f.graph, "MATCH (a:A) RETURN a.x, a.y, a.z, labels(a)")) ==
          Strings{"11|2|yeni|[A,C,D]"});
    // Null yazmak kaldırır.
    must_write(f.graph, "MATCH (a:A) SET a.y = null");
    CHECK(lines(must_query(f.graph, "MATCH (a:A) RETURN a.y")) == Strings{"null"});
    must_write(f.graph, "MATCH (a:A) REMOVE a.x, a:C");
    CHECK(lines(must_query(f.graph, "MATCH (a:A) RETURN a.x, labels(a)")) == Strings{"null|[A,D]"});
    must_write(f.graph, "MATCH ()-[r:T]->() SET r.w = 7");
    CHECK(lines(must_query(f.graph, "MATCH ()-[r:T]->() RETURN r.w")) == Strings{"7"});
    must_write(f.graph, "MATCH ()-[r:T]->() REMOVE r.w");
    CHECK(lines(must_query(f.graph, "MATCH ()-[r:T]->() RETURN r.w")) == Strings{"null"});
    // Yazma sonrası RETURN yeni değeri görür; LIMIT yazmayı kısmaz.
    must_write(f.graph, "CREATE (:C {v: 0}), (:C {v: 0})");
    CHECK(must_write(f.graph, "MATCH (c:C) SET c.v = 1 RETURN c.v LIMIT 1").rows.size() == 1);
    CHECK(lines(must_query(f.graph, "MATCH (c:C) RETURN c.v")) == Strings{"1", "1"});
    // Null hedef sessizce geçilir (OPTIONAL MATCH); hedef olmayan tip hata verir.
    must_write(f.graph, "OPTIONAL MATCH (x:Absent) SET x.k = 1");
    must_write(f.graph, "OPTIONAL MATCH (x:Absent) REMOVE x.k");
    check_rejected(f.graph, "WITH 1 AS x SET x.k = 1", Error::Code::InvalidArgument, QueryMode::ReadWrite);
    check_rejected(f.graph, "MATCH (a:A) SET a.k = [1]", Error::Code::InvalidArgument, QueryMode::ReadWrite);
}

TEST_CASE("delete_and_detach_delete") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:A)-[:T]->(b:B), (a)-[:T]->(b), (c:C)");
    // Kenarı olan düğüm DETACH'siz silinemez, hata tüm sorguyu geri alır.
    check_rejected(f.graph, "MATCH (a:A) DELETE a", Error::Code::InvalidArgument, QueryMode::ReadWrite);
    CHECK(must_query(f.graph, "MATCH (a:A) RETURN a").rows.size() == 1);
    // Düğüm ve kenar aynı cümlede: yazım sırası önemli değil, tekrar eden hedef sorun çıkarmaz.
    must_write(f.graph, "MATCH (a:A)-[r]->(b:B) DELETE a, r, b");
    CHECK(must_query(f.graph, "MATCH (n) WHERE n:A OR n:B RETURN n").rows.empty());
    CHECK(must_query(f.graph, "MATCH ()-[r]->() RETURN r").rows.empty());

    must_write(f.graph, "CREATE (x:X)-[:T]->(y:Y), (y)-[:T]->(x)");
    must_write(f.graph, "MATCH (x:X) DETACH DELETE x");
    CHECK(must_query(f.graph, "MATCH (x:X) RETURN x").rows.empty());
    CHECK(must_query(f.graph, "MATCH (y:Y) RETURN y").rows.size() == 1);
    CHECK(must_query(f.graph, "MATCH ()-[r]->() RETURN r").rows.empty());

    // Yalnız kenar; değişken uzunluklu yolun kenar listesi; null geçilir.
    must_write(f.graph, "CREATE (p:P)-[:E]->(q:Q)-[:E]->(s:S)");
    must_write(f.graph, "MATCH (:P)-[r:E*2]->(:S) DELETE r");
    CHECK(must_query(f.graph, "MATCH ()-[r:E]->() RETURN r").rows.empty());
    must_write(f.graph, "OPTIONAL MATCH (x:Absent) DELETE x");
    check_rejected(f.graph, "WITH 1 AS x DELETE x", Error::Code::InvalidArgument, QueryMode::ReadWrite);
    // Silinen düğümün özelliği null okunur.
    CHECK(lines(must_write(f.graph, "MATCH (s:S) DELETE s RETURN s.k")) == Strings{"null"});
}

TEST_CASE("merge_matches_or_creates") {
    GraphFixture f;
    CHECK(lines(must_write(f.graph, "MERGE (n:A {k: 1}) RETURN n.k")) == Strings{"1"});
    CHECK(lines(must_write(f.graph, "MERGE (n:A {k: 1}) RETURN n.k")) == Strings{"1"});
    CHECK(must_query(f.graph, "MATCH (n:A) RETURN n").rows.size() == 1);
    must_write(f.graph, "MERGE (n:A {k: 2})");
    CHECK(must_query(f.graph, "MATCH (n:A) RETURN n").rows.size() == 2);
    // Bağlı uçlar arasında kenar: yoksa kurulur, varsa tekrar kurulmaz.
    must_write(f.graph, "MATCH (a:A {k: 1}), (b:A {k: 2}) MERGE (a)-[r:T {w: 1}]->(b)");
    must_write(f.graph, "MATCH (a:A {k: 1}), (b:A {k: 2}) MERGE (a)-[r:T {w: 1}]->(b)");
    CHECK(must_query(f.graph, "MATCH ()-[r:T]->() RETURN r").rows.size() == 1);
    // Girdi satırı başına; önceki satırın kurduğunu görür.
    must_write(f.graph, "CREATE (:S {v: 7}), (:S {v: 7}), (:S {v: 8})");
    must_write(f.graph, "MATCH (s:S) MERGE (m:M {v: s.v})");
    CHECK(sorted(lines(must_query(f.graph, "MATCH (m:M) RETURN m.v"))) == Strings{"7", "8"});
    // Çok eşleşme: her biri için satır.
    must_write(f.graph, "CREATE (:D {g: 1}), (:D {g: 1})");
    CHECK(must_write(f.graph, "MERGE (d:D {g: 1}) RETURN d").rows.size() == 2);
    // Tüm yol: bağsız uçlar türetilmiş kimlikle yeniden kullanılır.
    must_write(f.graph, "MERGE (a:PA {k: 1})-[:T]->(b:PB {k: 2})");
    must_write(f.graph, "MERGE (a:PA {k: 1})-[:T]->(b:PB {k: 2})");
    must_write(f.graph, "MERGE (a:PA {k: 1})-[:U]->(c:PC {k: 3})");
    CHECK(must_query(f.graph, "MATCH (a:PA) RETURN a").rows.size() == 1);
    CHECK(must_query(f.graph, "MATCH (:PA)-[r]->() RETURN r").rows.size() == 2);
    // Soldaki ok da bağlanır.
    must_write(f.graph, "MERGE (a:LA {k: 1})<-[:T]-(b:LB {k: 2})");
    CHECK(must_query(f.graph, "MATCH (:LB)-[:T]->(:LA) RETURN 1").rows.size() == 1);
}

TEST_CASE("merge_rejects_null_and_unmergeable_patterns") {
    GraphFixture f;
    const auto write = QueryMode::ReadWrite;
    check_rejected(f.graph, "MERGE (n:A {k: null})", Error::Code::InvalidArgument, write);
    check_rejected(f.graph, "OPTIONAL MATCH (x:Absent) MERGE (n:A {k: x.v})", Error::Code::InvalidArgument,
                   write);
    check_rejected(f.graph, "MERGE (a)-[:T*1..2]->(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "MERGE (a)-[]->(b)", Error::Code::Syntax, write);
    check_rejected(f.graph, "MERGE (a {k: [1]})", Error::Code::InvalidArgument, write);
    check_rejected(f.graph, "MATCH (x) MERGE (x:L)", Error::Code::Syntax, write);
    check_rejected(f.graph, "MERGE (a:A), (b:B)", Error::Code::Syntax, write);
    CHECK(must_query(f.graph, "MATCH (n) RETURN count(*)").rows.at(0).at(0) == QueryValue(std::int64_t{0}));
}

TEST_CASE("merge_on_two_devices_yields_one_node") {
    GraphFixture left;
    GraphFixture right;
    for (Graph* graph : {&left.graph, &right.graph})
        must_write(*graph, "MERGE (t:Tag {name: 'iş', rank: 3})-[:IN]->(g:Group {name: 'kök'})");
    // Etiket ve özellik sırası kimliği değiştirmez.
    must_write(right.graph, "MERGE (t:Tag {rank: 3, name: 'iş'})");
    const std::string_view read = "MATCH (t:Tag)-[r:IN]->(g:Group) RETURN id(t), id(g), id(r)";
    const Strings on_left = lines(must_query(left.graph, read));
    REQUIRE(on_left.size() == 1);
    CHECK(on_left == lines(must_query(right.graph, read)));
    CHECK(must_query(right.graph, "MATCH (t:Tag) RETURN t").rows.size() == 1);

    // Çok etiket sırası da aynı kimliği verir. 1 ve 1.0 da: MATCH bunları zaten eşit görür.
    GraphFixture one;
    GraphFixture two;
    must_write(one.graph, "MERGE (n:A:B {x: 1})");
    must_write(two.graph, "MERGE (n:B:A {x: 1.0})");
    CHECK(lines(must_query(one.graph, "MATCH (n:A) RETURN id(n)")) ==
          lines(must_query(two.graph, "MATCH (n:A) RETURN id(n)")));

    // Farklı içerik farklı kimlik: tip ve değer ayrımı kaybolmaz.
    must_write(one.graph, "MERGE (n:A:B {x: '1'})");
    CHECK(must_query(one.graph, "MATCH (n:A) RETURN n").rows.size() == 2);

    // Silinip yeniden kurulan aynı kimliğe döner.
    const Strings id_before = lines(must_query(left.graph, "MATCH (g:Group) RETURN id(g)"));
    must_write(left.graph, "MATCH (g:Group) DETACH DELETE g");
    must_write(left.graph, "MERGE (g:Group {name: 'kök'})");
    CHECK(lines(must_query(left.graph, "MATCH (g:Group) RETURN id(g)")) == id_before);
}

TEST_CASE("merge_refuses_to_overwrite_a_changed_node_with_the_same_derived_id") {
    GraphFixture f;
    must_write(f.graph, "MERGE (n:A {k: 1})");
    must_write(f.graph, "MATCH (n:A) SET n.k = 2");
    // Desen eşleşmez ama türetilmiş kimlik doludur: sessizce k=1'e geri yazmak kullanıcı verisini yok ederdi.
    check_rejected(f.graph, "MERGE (n:A {k: 1})", Error::Code::InvalidArgument, QueryMode::ReadWrite);
    CHECK(lines(must_query(f.graph, "MATCH (n:A) RETURN n.k")) == Strings{"2"});
}

// F3: uygulamanın yazma denetimi (QueryHooks). Her değişiklik yapılmadan önce, değişiklik öncesini gören işlemle
// sorulur; ret işlemi geri alır ve sorgudaki konumu taşır; commit=false önizlemedir, graf değişmez.
namespace {

std::string change_text(const QueryChange& change) {
    using Kind = QueryChange::Kind;
    switch (change.kind) {
    case Kind::CreateNode:
        return "node+";
    case Kind::DeleteNode:
        return "node-";
    case Kind::AddLabel:
        return "label+:" + std::string(change.name);
    case Kind::RemoveLabel:
        return "label-:" + std::string(change.name);
    case Kind::CreateEdge:
        return "edge+:" + std::string(change.name);
    case Kind::DeleteEdge:
        return "edge-:" + std::string(change.name);
    case Kind::SetProperty: {
        const std::string value =
            change.value == nullptr
                ? std::string("?")
                : show(std::visit([](const auto& v) { return QueryValue(v); }, *change.value));
        return "set:" + std::string(change.name) + "=" + value;
    }
    }
    return "?";
}

}  // namespace

TEST_CASE("query_hooks_see_every_change_before_it_happens") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:L {k: 1})-[:T]->(b:L {k: 2})");
    Strings seen;
    Strings labels_seen;
    QueryHooks hooks;
    hooks.before_change = [&](ReadTxn& txn, const QueryChange& change) -> Result<void> {
        seen.push_back(change_text(change));
        if (change.kind == QueryChange::Kind::DeleteNode) {
            // İşlem değişiklikten önceki durumu görür: silinecek düğümün etiketi okunur.
            const auto labels = txn.labels(std::get<NodeId>(change.target));
            REQUIRE(labels.has_value());
            labels_seen = *labels;
        }
        return {};
    };
    const auto write = [&](std::string_view text) {
        seen.clear();
        const Result<QueryResult> done = f.graph.query(text, {}, QueryMode::ReadWrite, {}, hooks);
        REQUIRE_MESSAGE(done.has_value(), (done ? "" : describe_error(done.error())));
    };
    write("MATCH (a:L {k: 1}) SET a.x = 5, a:M REMOVE a:L CREATE (a)-[:U {w: 'v'}]->(c:N)");
    CHECK(seen == Strings{"set:x=5", "label+:M", "label-:L", "node+", "label+:N", "edge+:U", "set:w=v"});
    write("MATCH ()-[r:U]->() SET r.w = null");
    CHECK(seen == Strings{"set:w=null"});
    // DETACH: düğümden sonra kenarları da tek tek sorulur (tipleriyle).
    write("MATCH (b:L {k: 2}) DETACH DELETE b");
    CHECK(seen == Strings{"node-", "edge-:T"});
    CHECK(labels_seen == Strings{"L"});
    write("MATCH ()-[r:U]->() DELETE r");
    CHECK(seen == Strings{"edge-:U"});
    // Okuma sorgusu kancayı hiç çağırmaz.
    seen.clear();
    REQUIRE(f.graph.query("MATCH (n) RETURN n", {}, QueryMode::ReadWrite, {}, hooks).has_value());
    CHECK(seen.empty());
}

TEST_CASE("query_hook_rejection_rolls_back_and_carries_the_position") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:L {k: 1})");
    const Strings before = logical_dump(f.path());
    const std::int64_t ops_before = ops_count(f.path());
    QueryHooks hooks;
    hooks.before_change = [](ReadTxn&, const QueryChange& change) -> Result<void> {
        if (change.kind == QueryChange::Kind::SetProperty && change.name == "locked")
            return std::unexpected(Error{Error::Code::InvalidArgument, "test.locked", {{"key", "locked"}}});
        return {};
    };
    const Result<QueryResult> rejected =
        f.graph.query("MATCH (a:L) SET a.free = 1\nSET a.locked = 2", {}, QueryMode::ReadWrite, {}, hooks);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message_key == "test.locked");
    CHECK(arg_of(rejected.error(), "key") == "locked");
    CHECK(arg_of(rejected.error(), "line") == "2");
    CHECK_FALSE(arg_of(rejected.error(), "col").empty());
    // İlk SET de geri alınır: işlem commit edilmedi.
    CHECK(logical_dump(f.path()) == before);
    CHECK(ops_count(f.path()) == ops_before);
}

TEST_CASE("query_preview_runs_the_write_and_leaves_the_graph_unchanged") {
    GraphFixture f;
    must_write(f.graph, "CREATE (a:L {k: 1})");
    const Strings before = logical_dump(f.path());
    const std::int64_t ops_before = ops_count(f.path());
    std::size_t changes = 0;
    QueryHooks hooks;
    hooks.commit = false;
    hooks.before_change = [&](ReadTxn&, const QueryChange&) -> Result<void> {
        ++changes;
        return {};
    };
    const Result<QueryResult> preview = f.graph.query("MATCH (a:L) SET a.k = 2 CREATE (n:L {k: 9}) RETURN n.k",
                                                      {}, QueryMode::ReadWrite, {}, hooks);
    REQUIRE(preview.has_value());
    CHECK(lines(*preview) == Strings{"9"});
    CHECK(changes == 4);  // k'nin değeri, düğüm, etiket, özellik
    CHECK(logical_dump(f.path()) == before);
    CHECK(ops_count(f.path()) == ops_before);
    CHECK(lines(must_query(f.graph, "MATCH (n:L) RETURN n.k")) == Strings{"1"});
    // Önizleme açık işlem bırakmaz: hemen ardından yazma çalışır.
    must_write(f.graph, "MATCH (a:L) SET a.k = 3");
    CHECK(lines(must_query(f.graph, "MATCH (n:L) RETURN n.k")) == Strings{"3"});
}
