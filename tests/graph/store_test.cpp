// İç katman: depolama planı ve register kuralı. Uzak çerçeveler G2'de gelir; kuralın kendisi bugün sınanır.
#include "graph/op.hpp"
#include "graph/store.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using noteapp::test::TempDir;
using test::begin_read;
using test::begin_write;
using test::edge_named;
using test::node_named;
using test::open_graph;
using test::RawDb;

namespace {

std::vector<std::string> plan_of(RawDb& raw, std::string_view sql) {
    std::vector<std::string> steps;
    for (std::string& row : raw.rows("EXPLAIN QUERY PLAN " + std::string(sql))) {
        // Sütunlar: id|parent|notused|detail. Kararlı olan yalnız detail.
        steps.push_back(row.substr(row.rfind('|') + 1));
    }
    return steps;
}

bool is_data_statement(std::string_view sql) {
    return sql.starts_with("SELECT") || sql.starts_with("UPDATE") || sql.starts_with("DELETE");
}

void fill_skewed_graph(Graph& graph) {
    WriteTxn txn = begin_write(graph);
    const NodeId hub = node_named("hub");
    GRAPH_OK(txn.put_node(hub));
    GRAPH_OK(txn.add_label(hub, "Hub"));
    for (int i = 0; i < 300; ++i) {
        const NodeId leaf = node_named("leaf" + std::to_string(i));
        GRAPH_OK(txn.put_node(leaf));
        GRAPH_OK(txn.add_label(leaf, "Leaf"));
        GRAPH_OK(txn.put_edge(edge_named("e" + std::to_string(i)), hub, i % 2 == 0 ? "A" : "B", leaf));
        GRAPH_OK(txn.set_property(leaf, "k", std::int64_t{i}));
    }
    GRAPH_OK(txn.commit());
}

Dot other_dot(unsigned char device, std::uint64_t seq) {
    return Dot{DeviceId{std::byte{device}}, seq};
}

}  // namespace

TEST_CASE("query_plan_same_with_and_without_stat1") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    {
        Graph graph = open_graph(path);
        fill_skewed_graph(graph);
    }
    const std::vector<std::string_view> statements = statement_texts();
    REQUIRE(statements.size() == static_cast<std::size_t>(StatementId::Count));

    constexpr std::string_view kUnpinnedControl = "SELECT id FROM edges WHERE src=?1 AND dst=?2";
    std::vector<std::vector<std::string>> without_stats;
    std::vector<std::string> control_before;
    {
        RawDb raw(path);
        CHECK(raw.scalar("SELECT count(*) FROM sqlite_master WHERE name='sqlite_stat1'") == 0);
        for (const std::string_view sql : statements)
            without_stats.push_back(plan_of(raw, sql));
        control_before = plan_of(raw, kUnpinnedControl);
    }

    SUBCASE("real_analyze") {
        RawDb raw(path);
        raw.exec("ANALYZE");
        REQUIRE(raw.scalar("SELECT count(*) FROM sqlite_stat1") > 0);
    }
    SUBCASE("adversarial_stats_prefer_the_wrong_index") {
        // Planlayıcıya gerçeğin tersini söyler: her ikincil dizin harika, her birincil anahtar berbat
        // görünür.
        RawDb raw(path);
        raw.exec("ANALYZE");
        raw.exec("DELETE FROM sqlite_stat1");
        raw.exec("INSERT INTO sqlite_stat1(tbl, idx, stat) VALUES "
                 "('labels','labels_scan','1000000 1 1'),"
                 "('labels','sqlite_autoindex_labels_1','1000000 1000000 1000000'),"
                 "('edges','edges_out','1000000 1 1 1'),"
                 "('edges','edges_in','1000000 1000000 1000000 1000000'),"
                 "('edges','sqlite_autoindex_edges_1','1000000 1000000'),"
                 "('props','sqlite_autoindex_props_1','1000000 1000000 1000000 1000000 1000000')");
        // Kontrol grubu: sabitlenmemiş sorguda aynı istatistik planı gerçekten değiştiriyor, yoksa bu alt
        // test boş geçerdi.
        RawDb fresh(path);
        CHECK_FALSE(plan_of(fresh, kUnpinnedControl) == control_before);
    }

    // Yeni bağlantı: istatistik şema yüklenirken okunur.
    RawDb raw(path);
    for (std::size_t i = 0; i < statements.size(); ++i) {
        INFO(statements[i]);
        CHECK(plan_of(raw, statements[i]) == without_stats[i]);
    }
}

TEST_CASE("every_read_is_a_single_table_seek_and_adjacency_is_covering") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    {
        (void)open_graph(path);
    }
    RawDb raw(path);

    const std::vector<std::string_view> statements = statement_texts();
    for (const std::string_view sql : statements) {
        if (!is_data_statement(sql))
            continue;
        INFO(sql);
        const std::vector<std::string> plan = plan_of(raw, sql);
        REQUIRE(plan.size() == 1);
        // Tek tam tarama, başlangıç kısıtı olmayan desen içindir (SPEC-graph ilke 2).
        if (sql != statements[static_cast<std::size_t>(StatementId::NodesAll)])
            CHECK(plan[0].starts_with("SEARCH"));
    }
    for (const StatementId id : {StatementId::EdgesOut, StatementId::EdgesOutOfType, StatementId::EdgesIn,
                                 StatementId::EdgesInOfType}) {
        const std::string_view sql = statements[static_cast<std::size_t>(id)];
        INFO(sql);
        const std::vector<std::string> plan = plan_of(raw, sql);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].find("COVERING INDEX edges_") != std::string::npos);
    }
}

TEST_CASE("sqlite_is_built_with_the_designed_flags") {
    CHECK(sqlite3_libversion_number() == 3054001);
    CHECK(sqlite3_compileoption_used("THREADSAFE=2") == 1);
    CHECK(sqlite3_compileoption_used("DQS=0") == 1);
    CHECK(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 1);

    // OMIT_LOAD_EXTENSION gerçekten etkili: SQL'den çağrılacak işlev yok.
    RawDb raw(":memory:");
    CHECK_THROWS(raw.exec("SELECT load_extension('x')"));
    // DQS=0: çift tırnak tanımlayıcıdır, dizgeye düşmez.
    raw.exec("CREATE TABLE t(a)");
    CHECK_THROWS(raw.exec("SELECT \"yok\" FROM t"));
}

TEST_CASE("connection_runs_with_the_designed_settings") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Result<std::unique_ptr<Store>> store = Store::open(path);
    REQUIRE(store.has_value());
    sqlite3* db = (*store)->connection_for_test();
    const auto pragma = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    };
    CHECK(pragma("PRAGMA synchronous") == 2);
    CHECK(pragma("PRAGMA busy_timeout") == 5000);
    CHECK(pragma("PRAGMA trusted_schema") == 0);
    CHECK(pragma("PRAGMA cache_size") == -8000);
    CHECK(pragma("PRAGMA foreign_keys") == 0);
    int defensive = -1;
    CHECK(sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, -1, &defensive) == SQLITE_OK);
    CHECK(defensive == 1);
}

TEST_CASE("register_keeps_unseen_versions_as_heads_and_reads_max_hlc_device") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Result<std::unique_ptr<Store>> opened = Store::open(path);
    REQUIRE(opened.has_value());
    Store& store = **opened;
    const NodeId a = node_named("a");

    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{a}));
    GRAPH_OK(store.record(PropSet{a, "k", std::string("local")}));

    // Yerel yazmayı görmemiş uzak yazma: yerel sürümü silmez, ikisi de head olur.
    const Op remote = PropSet{a, "k", std::string("remote")};
    const ApplyContext unseen{other_dot(9, 1), std::numeric_limits<std::uint64_t>::max(), CausalContext{}};
    GRAPH_OK(store.apply(unseen, remote));

    const auto versions_of = [&](Store& s) {
        auto statement = s.statement(StatementId::PropVersions);
        REQUIRE(statement.has_value());
        const std::string owner_key = "k";
        statement->bind_blob(1, a.bytes).bind_text(2, owner_key);
        std::vector<std::pair<std::uint8_t, std::uint64_t>> versions;
        while (true) {
            Result<bool> row = statement->step();
            REQUIRE(row.has_value());
            if (!*row)
                return versions;
            versions.emplace_back(std::to_integer<std::uint8_t>(statement->column_blob(0)[0]),
                                  static_cast<std::uint64_t>(statement->column_int(1)));
        }
    };
    CHECK(versions_of(store).size() == 2);

    // Tekrar uygulama aynı dot'u çoğaltmaz (idempotent).
    GRAPH_OK(store.apply(unseen, remote));
    CHECK(versions_of(store).size() == 2);

    // Her ikisini de görmüş yazma tek head bırakır.
    const Dot local_dot{store.device(), 1};
    const ApplyContext seen_all{other_dot(7, 1), 5, CausalContext{{local_dot, other_dot(9, 1)}}};
    GRAPH_OK(store.apply(seen_all, PropSet{a, "k", std::string("final")}));
    const auto final_versions = versions_of(store);
    REQUIRE(final_versions.size() == 1);
    CHECK(final_versions[0].first == 7);
    store.end_txn();
}

TEST_CASE("versions_and_property_expose_heads_through_the_public_api") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    const NodeId a = node_named("a");
    const std::string big(1000, 'b');
    {
        Result<std::unique_ptr<Store>> opened = Store::open(path);
        REQUIRE(opened.has_value());
        Store& store = **opened;
        GRAPH_OK(store.begin_write());
        GRAPH_OK(store.record(NodePut{a}));
        GRAPH_OK(store.record(PropSet{a, "k", std::string("local")}));
        // HLC'si yerelden büyük, görülmemiş uzak sürüm: okuma onu seçer.
        const ApplyContext later{other_dot(9, 4), std::numeric_limits<std::uint64_t>::max() >> 1U,
                                 CausalContext{}};
        GRAPH_OK(store.apply(later, PropSet{a, "k", big}));
        // Aynı HLC'de daha küçük cihaz kaybeder.
        const ApplyContext tie{other_dot(8, 2), std::numeric_limits<std::uint64_t>::max() >> 1U,
                               CausalContext{}};
        GRAPH_OK(store.apply(tie, PropSet{a, "k", std::string("tie")}));
        GRAPH_OK(store.commit_write());
    }
    Graph graph = open_graph(path);
    ReadTxn txn = begin_read(graph);
    CHECK(*txn.property(a, "k") == Value{big});
    const Result<std::vector<Version>> versions = txn.versions(a, "k");
    REQUIRE(versions.has_value());
    REQUIRE(versions->size() == 3);
    CHECK(std::ranges::is_sorted(*versions, {}, [](const Version& v) { return v.device; }));
    CHECK(std::ranges::any_of(*versions,
                              [&](const Version& v) { return v.seq == 4 && v.value == Value{big}; }));
}

TEST_CASE("concurrent_label_add_wins_over_a_delete_that_did_not_see_it") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    const NodeId a = node_named("a");
    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{a}));
    GRAPH_OK(store.record(LabelAdd{a, "L"}));

    // Uzak ekleme, uzak silmenin görmediği dot.
    const ApplyContext add{other_dot(9, 1), 1, CausalContext{}};
    GRAPH_OK(store.apply(add, LabelAdd{a, "L"}));
    const Dot local_dot{store.device(), 1};
    const ApplyContext del{other_dot(8, 1), 2, CausalContext{{local_dot}}};
    GRAPH_OK(store.apply(del, LabelDel{a, "L"}));
    {
        Result<Statement> labels = store.statement(StatementId::LabelsOfNode);
        REQUIRE(labels.has_value());
        labels->bind_blob(1, a.bytes);
        CHECK(*labels->step());  // görülmeyen ekleme kazandı
    }

    const ApplyContext del_all{other_dot(8, 2), 3, CausalContext{{local_dot, other_dot(9, 1)}}};
    GRAPH_OK(store.apply(del_all, LabelDel{a, "L"}));
    {
        Result<Statement> labels = store.statement(StatementId::LabelsOfNode);
        REQUIRE(labels.has_value());
        labels->bind_blob(1, a.bytes);
        CHECK_FALSE(*labels->step());
    }
    store.end_txn();
}

TEST_CASE("node_delete_without_seeing_every_write_has_no_effect") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{a}));
    GRAPH_OK(store.record(NodePut{b}));
    GRAPH_OK(store.record(EdgePut{edge_named("e"), a, "T", b}));

    const auto node_exists = [&](NodeId id) {
        Result<Statement> statement = store.statement(StatementId::NodeGet);
        REQUIRE(statement.has_value());
        statement->bind_blob(1, id.bytes);
        return *statement->step();
    };
    const auto edge_exists = [&](EdgeId id) {
        Result<Statement> statement = store.statement(StatementId::EdgeGet);
        REQUIRE(statement.has_value());
        statement->bind_blob(1, id.bytes);
        return *statement->step();
    };

    // Yerel put'u görmemiş silme boştur: düzenleme kazanır.
    const ApplyContext blind{other_dot(9, 1), 1, CausalContext{}};
    GRAPH_OK(store.apply(blind, NodeDel{a}));
    CHECK(node_exists(a));

    // Kenar eklemek silinmiş düğümü canlandırmaz ve sarkan kenar üretmez.
    const Dot local_dot{store.device(), 1};
    const ApplyContext seen{other_dot(9, 2), 2, CausalContext{{local_dot}}};
    GRAPH_OK(store.apply(seen, NodeDel{a}));
    CHECK_FALSE(node_exists(a));
    CHECK_FALSE(edge_exists(edge_named("e")));

    const ApplyContext late_edge{other_dot(8, 1), 3, CausalContext{}};
    GRAPH_OK(store.apply(late_edge, EdgePut{edge_named("late"), a, "T", b}));
    CHECK_FALSE(node_exists(a));
    CHECK_FALSE(edge_exists(edge_named("late")));
    store.end_txn();
}

TEST_CASE("frame_lists_seen_peers_as_deps_and_excludes_itself") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    const NodeId a = node_named("a");
    {
        Graph graph = open_graph(path);
        RawDb raw(path);
        const std::string device = raw.rows("SELECT v FROM meta WHERE k='device_id'").at(0);
        sqlite3_stmt* insert = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw.get(), "INSERT INTO peers(dev, other, seq) VALUES (?1, ?2, 7)", -1,
                                   &insert, nullptr) == SQLITE_OK);
        sqlite3_bind_blob(insert, 1, device.data(), static_cast<int>(device.size()), SQLITE_TRANSIENT);
        const DeviceId peer{std::byte{42}};
        sqlite3_bind_blob(insert, 2, peer.data(), static_cast<int>(peer.size()), SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(insert) == SQLITE_DONE);
        sqlite3_finalize(insert);

        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.commit());

        const std::string frame_blob = raw.rows("SELECT frame FROM ops").at(0);
        const Result<Frame> frame = decode_frame(test::as_bytes(frame_blob));
        REQUIRE(frame.has_value());
        REQUIRE(frame->deps.size() == 1);
        CHECK(frame->deps[0].dev == peer);
        CHECK(frame->deps[0].seq == 7);
    }
}
