#include "graph/graph.hpp"
#include "graph/op.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

using namespace noteapp::graph;
using noteapp::test::slurp;
using noteapp::test::TempDir;
using test::as_bytes;
using test::begin_read;
using test::begin_write;
using test::edge_named;
using test::node_named;
using test::open_graph;
using test::RawDb;

namespace {

constexpr std::int64_t kExpectedApplicationId = 0x4E474442;

struct Hop {
    EdgeId edge;
    NodeId node;
    std::string type;
    bool operator==(const Hop&) const = default;
};

std::vector<Hop> collect(ReadTxn& txn, NodeId id, Direction direction,
                         std::optional<std::string_view> type = {}) {
    Result<NeighborCursor> cursor = txn.neighbors(id, direction, type);
    REQUIRE(cursor.has_value());
    std::vector<Hop> hops;
    while (true) {
        Result<std::optional<Neighbor>> row = cursor->next();
        REQUIRE(row.has_value());
        if (!row->has_value())
            return hops;
        hops.push_back(Hop{(*row)->edge, (*row)->node, std::string((*row)->type)});
    }
}

std::vector<NodeId> collect(Result<NodeCursor> cursor) {
    REQUIRE(cursor.has_value());
    std::vector<NodeId> ids;
    while (true) {
        Result<std::optional<NodeId>> row = cursor->next();
        REQUIRE(row.has_value());
        if (!row->has_value())
            return ids;
        ids.push_back(**row);
    }
}

}  // namespace

TEST_CASE("open_creates_identified_file_and_keeps_device_id_across_reopen") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    {
        (void)open_graph(path);
    }

    RawDb raw(path);
    CHECK(raw.scalar("PRAGMA application_id") == kExpectedApplicationId);
    CHECK(raw.scalar("PRAGMA user_version") == 1);
    CHECK(raw.rows("PRAGMA journal_mode") == std::vector<std::string>{"wal"});
    const std::vector<std::string> device = raw.rows("SELECT v FROM meta WHERE k='device_id'");
    REQUIRE(device.size() == 1);
    CHECK(device[0].size() == 16);

    {
        (void)open_graph(path);
    }
    CHECK(raw.rows("SELECT v FROM meta WHERE k='device_id'") == device);
}

TEST_CASE("open_works_on_a_turkish_path") {
    TempDir dir(u8"-İşğü");
    const auto path = dir.path() / u8"çizge.db";
    {
        (void)open_graph(path);
    }
    CHECK(std::filesystem::is_regular_file(path));
}

TEST_CASE("refuses_newer_user_version") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    {
        (void)open_graph(path);
    }
    {
        RawDb raw(path);
        raw.exec("PRAGMA user_version=2");
        raw.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }
    const std::string before = slurp(path);
    const Result<Graph> graph = Graph::open(path);
    REQUIRE_FALSE(graph.has_value());
    CHECK(graph.error().code == Error::Code::TooNew);
    CHECK(slurp(path) == before);
}

TEST_CASE("refuses_foreign_application_id_without_touching_the_file") {
    TempDir dir;
    const auto path = dir.path() / "other.db";
    {
        RawDb raw(path);
        raw.exec("PRAGMA application_id=12345");
        raw.exec("CREATE TABLE t(x)");
    }
    const std::string before = slurp(path);
    const Result<Graph> graph = Graph::open(path);
    REQUIRE_FALSE(graph.has_value());
    CHECK(graph.error().code == Error::Code::Corrupt);
    // WAL'a geçilmiş olsaydı başlığın 18-19. baytları değişirdi.
    CHECK(slurp(path) == before);
}

TEST_CASE("refuses_sqlite_file_with_foreign_tables_and_zero_application_id") {
    TempDir dir;
    const auto path = dir.path() / "other.db";
    {
        RawDb raw(path);
        raw.exec("CREATE TABLE t(x)");
    }
    const std::string before = slurp(path);
    const Result<Graph> graph = Graph::open(path);
    REQUIRE_FALSE(graph.has_value());
    CHECK(graph.error().code == Error::Code::Corrupt);
    CHECK(slurp(path) == before);
}

TEST_CASE("refuses_non_database_file") {
    TempDir dir;
    const auto path = dir.path() / "text.db";
    std::ofstream(path, std::ios::binary) << std::string(8192, 'x');
    const std::string before = slurp(path);
    const Result<Graph> graph = Graph::open(path);
    REQUIRE_FALSE(graph.has_value());
    CHECK(graph.error().code == Error::Code::Corrupt);
    CHECK(slurp(path) == before);
}

TEST_CASE("open_reports_io_error_for_missing_directory") {
    TempDir dir;
    const Result<Graph> graph = Graph::open(dir.path() / "yok" / "graph.db");
    REQUIRE_FALSE(graph.has_value());
    CHECK(graph.error().code == Error::Code::Io);
}

TEST_CASE("bytes_roundtrip_preserves_nul_crlf_bom_invalid_utf8_10mib") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    const NodeId id = node_named("n");

    std::string big = std::string("\xEF\xBB\xBF", 3) + std::string("a\0b\r\nc\xFF\xFE\xC0\xAF", 10);
    big.resize(10U * 1024U * 1024U, '\0');
    big[big.size() / 2] = '\xFF';
    big[big.size() - 1] = '\n';
    const std::string small("\0\r\n\xEF\xBB\xBF\xFF", 7);

    {
        Graph graph = open_graph(path);
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(id));
        GRAPH_OK(txn.set_property(id, "big", Bytes{big}));
        GRAPH_OK(txn.set_property(id, "small", Bytes{small}));
        GRAPH_OK(txn.commit());
    }
    // Yeni bağlantı: dönen bayt önbellek değil dosyadır.
    Graph graph = open_graph(path);
    ReadTxn txn = begin_read(graph);
    const Result<Value> read_big = txn.property(id, "big");
    REQUIRE(read_big.has_value());
    REQUIRE(std::holds_alternative<Bytes>(*read_big));
    CHECK(std::get<Bytes>(*read_big).data == big);
    const Result<Value> read_small = txn.property(id, "small");
    REQUIRE(read_small.has_value());
    CHECK(std::get<Bytes>(*read_small).data == small);
}

TEST_CASE("every_value_type_roundtrips_and_null_removes") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId id = node_named("n");
    const std::vector<std::pair<std::string, Value>> values = {
        {"bool", true},
        {"int", std::int64_t{std::numeric_limits<std::int64_t>::min()}},
        {"double", 1.5},
        {"text", std::string("İşğü")},
        {"text_with_nul", std::string("a\0b", 3)},
        {"long_text", std::string(5000, 'x')},
        {"bytes", Bytes{"\xFF"}},
        {"time", Timestamp{1700000000000}},
    };
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(id));
        for (const auto& [key, value] : values)
            GRAPH_OK(txn.set_property(id, key, value));
        GRAPH_OK(txn.commit());
    }
    {
        ReadTxn txn = begin_read(graph);
        for (const auto& [key, value] : values) {
            const Result<Value> read = txn.property(id, key);
            REQUIRE(read.has_value());
            CHECK(*read == value);
        }
        const Result<Value> missing = txn.property(id, "yok");
        REQUIRE(missing.has_value());
        CHECK(std::holds_alternative<std::monostate>(*missing));
    }
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.set_property(id, "int", std::monostate{}));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    const Result<Value> removed = txn.property(id, "int");
    REQUIRE(removed.has_value());
    CHECK(std::holds_alternative<std::monostate>(*removed));
}

TEST_CASE("set_property_rejects_nan_invalid_utf8_and_bad_keys_without_recording") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    const NodeId id = node_named("n");
    WriteTxn txn = begin_write(graph);
    GRAPH_OK(txn.put_node(id));

    const auto expect_invalid = [](const Result<void>& result) {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Error::Code::InvalidArgument);
    };
    expect_invalid(txn.set_property(id, "k", std::numeric_limits<double>::quiet_NaN()));
    expect_invalid(txn.set_property(id, "k", std::string("\xFF")));
    expect_invalid(txn.set_property(id, "", std::int64_t{1}));
    expect_invalid(txn.add_label(id, std::string(detail::kMaxNameBytes + 1, 'x')));

    // Doğrulama hatası işlemi zehirlemez: önceki yazma hâlâ commit edilebilir.
    GRAPH_OK(txn.commit());
    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM ops") == 1);
    CHECK(raw.scalar("SELECT count(*) FROM props") == 0);
}

// L4b: çerçeve sınırı yalnız dosyadaki kaydı sınırlar; değer ve işlem ona sığmak zorunda değildir.
TEST_CASE("a value and a transaction larger than one frame commit and read back") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    GraphLimits small;
    small.frame_bytes = 64U * 1024U;
    Graph graph = open_graph(path, small);
    const NodeId id = node_named("n");
    const std::string big(small.frame_bytes + 1, 'x');
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(id));
        GRAPH_OK(txn.set_property(id, "big", Bytes{big}));
        for (int i = 0; i < 3; ++i)
            GRAPH_OK(txn.set_property(id, "k" + std::to_string(i), Bytes{std::string(30U * 1024U, 'y')}));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    const Result<Value> value = txn.property(id, "big");
    REQUIRE(value.has_value());
    CHECK(*value == Value{Bytes{big}});
    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM ops") == 1);
}

TEST_CASE("rollback_leaves_neither_state_nor_op") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_node(b));
        GRAPH_OK(txn.add_label(a, "L"));
        GRAPH_OK(txn.set_property(a, "k", std::string(1000, 'x')));
        GRAPH_OK(txn.put_edge(edge_named("e"), a, "T", b));
        // commit yok: yıkıcı geri alır.
    }
    RawDb raw(path);
    for (const char* table : {"nodes", "labels", "edges", "props", "large_values", "ops"})
        CHECK_MESSAGE(raw.scalar(std::string("SELECT count(*) FROM ") + table) == 0, table);
    CHECK(raw.scalar("SELECT count(*) FROM meta WHERE k='seq'") == 0);

    // Geri alınan işlem seq tüketmez: sonraki commit 1 alır, zincirde delik olmaz.
    WriteTxn txn = begin_write(graph);
    GRAPH_OK(txn.put_node(a));
    GRAPH_OK(txn.commit());
    CHECK(raw.scalar("SELECT seq FROM ops") == 1);
}

TEST_CASE("commit_records_frame_with_dot_hlc_and_ops_in_the_same_transaction") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const EdgeId e = edge_named("e");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_node(b));
        GRAPH_OK(txn.add_label(a, "L"));
        GRAPH_OK(txn.put_edge(e, a, "T", b));
        GRAPH_OK(txn.set_property(e, "w", std::int64_t{3}));
        GRAPH_OK(txn.commit());
    }
    {
        // Boş işlem çerçeve üretmez.
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.commit());
    }
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.delete_edge(e));
        GRAPH_OK(txn.commit());
    }

    RawDb raw(path);
    const std::vector<std::string> device = raw.rows("SELECT v FROM meta WHERE k='device_id'");
    REQUIRE(device.size() == 1);
    CHECK(raw.rows("SELECT dev FROM ops ORDER BY seq") == std::vector<std::string>{device[0], device[0]});
    CHECK(raw.rows("SELECT seq FROM ops ORDER BY seq") == std::vector<std::string>{"1", "2"});
    CHECK(raw.scalar("SELECT v FROM meta WHERE k='seq'") == 2);

    const std::vector<std::string> frames = raw.rows("SELECT frame FROM ops ORDER BY seq");
    const Result<detail::Frame> first = detail::decode_frame(as_bytes(frames[0]));
    const Result<detail::Frame> second = detail::decode_frame(as_bytes(frames[1]));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->seq == 1);
    CHECK(second->seq == 2);
    CHECK(second->hlc > first->hlc);
    CHECK(raw.scalar("SELECT v FROM meta WHERE k='hlc'") == static_cast<std::int64_t>(second->hlc));
    const std::vector<detail::Op> expected_first = {
        detail::NodePut{a},
        detail::NodePut{b},
        detail::LabelAdd{a, "L"},
        detail::EdgePut{e, a, "T", b},
        detail::PropSet{e, "w", std::int64_t{3}},
    };
    CHECK(first->ops == expected_first);
    CHECK(second->ops == std::vector<detail::Op>{detail::EdgeDel{e}});
}

TEST_CASE("neighbors_keep_parallel_edges_by_direction_and_type") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const NodeId c = node_named("c");
    const EdgeId knows1 = edge_named("knows1");
    const EdgeId knows2 = edge_named("knows2");
    const EdgeId likes = edge_named("likes");
    const EdgeId from_c = edge_named("from_c");
    {
        WriteTxn txn = begin_write(graph);
        for (const NodeId id : {a, b, c})
            GRAPH_OK(txn.put_node(id));
        GRAPH_OK(txn.put_edge(knows1, a, "KNOWS", b));
        GRAPH_OK(txn.put_edge(knows2, a, "KNOWS", b));
        GRAPH_OK(txn.put_edge(likes, a, "LIKES", b));
        GRAPH_OK(txn.put_edge(from_c, c, "KNOWS", a));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);

    std::vector<Hop> out = collect(txn, a, Direction::Out);
    CHECK(out.size() == 3);
    CHECK(std::ranges::count_if(out, [&](const Hop& h) { return h.type == "KNOWS" && h.node == b; }) == 2);
    CHECK(std::ranges::count_if(out, [&](const Hop& h) { return h.edge == likes && h.type == "LIKES"; }) ==
          1);
    CHECK(std::ranges::is_sorted(out, {}, [](const Hop& h) { return h.type; }));

    std::vector<Hop> typed = collect(txn, a, Direction::Out, "KNOWS");
    CHECK(typed.size() == 2);
    CHECK_FALSE(typed[0].edge == typed[1].edge);

    CHECK(collect(txn, a, Direction::In) == std::vector<Hop>{Hop{from_c, c, "KNOWS"}});
    CHECK(collect(txn, a, Direction::In, "LIKES").empty());
    CHECK(collect(txn, b, Direction::In).size() == 3);
    CHECK(collect(txn, b, Direction::Out).empty());
    CHECK(collect(txn, a, Direction::Both).size() == 4);
    CHECK(collect(txn, a, Direction::Both, "KNOWS").size() == 3);
}

TEST_CASE("self_loop_appears_once_in_both_and_once_per_direction") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const EdgeId loop = edge_named("loop");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_edge(loop, a, "SELF", a));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    CHECK(collect(txn, a, Direction::Out).size() == 1);
    CHECK(collect(txn, a, Direction::In).size() == 1);
    CHECK(collect(txn, a, Direction::Both).size() == 1);
}

TEST_CASE("nested_cursors_on_the_same_statement_do_not_interfere") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const NodeId c = node_named("c");
    {
        WriteTxn txn = begin_write(graph);
        for (const NodeId id : {a, b, c})
            GRAPH_OK(txn.put_node(id));
        GRAPH_OK(txn.put_edge(edge_named("ab"), a, "T", b));
        GRAPH_OK(txn.put_edge(edge_named("ac"), a, "T", c));
        GRAPH_OK(txn.put_edge(edge_named("bc"), b, "T", c));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    Result<NeighborCursor> outer = txn.neighbors(a, Direction::Out);
    REQUIRE(outer.has_value());
    int outer_rows = 0;
    int inner_rows = 0;
    while (true) {
        Result<std::optional<Neighbor>> row = outer->next();
        REQUIRE(row.has_value());
        if (!row->has_value())
            break;
        ++outer_rows;
        inner_rows += static_cast<int>(collect(txn, (*row)->node, Direction::Out).size());
    }
    CHECK(outer_rows == 2);
    CHECK(inner_rows == 1);
}

TEST_CASE("labels_and_label_scan_follow_add_and_remove") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_node(b));
        GRAPH_OK(txn.add_label(a, "Tür"));
        GRAPH_OK(txn.add_label(a, "Other"));
        GRAPH_OK(txn.add_label(b, "Tür"));
        GRAPH_OK(txn.remove_label(b, "yok"));
        GRAPH_OK(txn.commit());
    }
    {
        ReadTxn txn = begin_read(graph);
        CHECK(*txn.labels(a) == std::vector<std::string>{"Other", "Tür"});
        CHECK(collect(txn.nodes_with_label("Tür")).size() == 2);
        CHECK(collect(txn.nodes_with_label("yok")).empty());
        CHECK(collect(txn.nodes()).size() == 2);
    }
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.remove_label(a, "Tür"));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    CHECK(*txn.labels(a) == std::vector<std::string>{"Other"});
    CHECK(collect(txn.nodes_with_label("Tür")) == std::vector<NodeId>{b});
}

TEST_CASE("new_label_leaves_sqlite_master_unchanged") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    RawDb raw(path);
    const std::vector<std::string> before =
        raw.rows("SELECT type, name, sql FROM sqlite_master ORDER BY name");
    REQUIRE_FALSE(before.empty());
    const std::int64_t schema_version_before = raw.scalar("PRAGMA schema_version");

    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    WriteTxn txn = begin_write(graph);
    GRAPH_OK(txn.put_node(a));
    GRAPH_OK(txn.put_node(b));
    for (int i = 0; i < 50; ++i) {
        GRAPH_OK(txn.add_label(a, "Label" + std::to_string(i)));
        GRAPH_OK(txn.put_edge(edge_named(std::to_string(i)), a, "TYPE_" + std::to_string(i), b));
        GRAPH_OK(txn.set_property(a, "key" + std::to_string(i), std::int64_t{i}));
    }
    GRAPH_OK(txn.commit());

    CHECK(raw.rows("SELECT type, name, sql FROM sqlite_master ORDER BY name") == before);
    CHECK(raw.scalar("PRAGMA schema_version") == schema_version_before);
    CHECK(raw.scalar("SELECT count(DISTINCT label) FROM labels") == 50);
}

TEST_CASE("write_validation_refuses_dangling_references_and_changed_edges") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const NodeId ghost = node_named("ghost");
    const EdgeId e = edge_named("e");
    WriteTxn txn = begin_write(graph);
    GRAPH_OK(txn.put_node(a));
    GRAPH_OK(txn.put_node(b));
    GRAPH_OK(txn.put_edge(e, a, "T", b));

    const auto expect = [](const Result<void>& result, Error::Code code) {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == code);
    };
    expect(txn.put_edge(edge_named("x"), a, "T", ghost), Error::Code::NotFound);
    expect(txn.put_edge(edge_named("x"), ghost, "T", a), Error::Code::NotFound);
    expect(txn.add_label(ghost, "L"), Error::Code::NotFound);
    expect(txn.remove_label(ghost, "L"), Error::Code::NotFound);
    expect(txn.set_property(ghost, "k", std::int64_t{1}), Error::Code::NotFound);
    expect(txn.set_property(edge_named("x"), "k", std::int64_t{1}), Error::Code::NotFound);
    expect(txn.delete_node(ghost, true), Error::Code::NotFound);
    expect(txn.delete_edge(edge_named("x")), Error::Code::NotFound);
    expect(txn.put_edge(e, b, "T", a), Error::Code::InvalidArgument);
    expect(txn.put_edge(e, a, "OTHER", b), Error::Code::InvalidArgument);
    GRAPH_OK(txn.put_edge(e, a, "T", b));
    GRAPH_OK(txn.commit());
}

TEST_CASE("edge_returns_the_ends_and_type_of_a_live_edge_only") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const EdgeId ab = edge_named("ab");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_node(b));
        GRAPH_OK(txn.put_edge(ab, a, "T", b));
        GRAPH_OK(txn.commit());
    }
    {
        ReadTxn txn = begin_read(graph);
        const auto ends = txn.edge(ab);
        REQUIRE(ends.has_value());
        CHECK(*ends == EdgeEnds{a, "T", b});
        CHECK(txn.edge(edge_named("yok")).value() == std::nullopt);
    }
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.delete_edge(ab));
        CHECK(txn.edge(ab).value() == std::nullopt);
        GRAPH_OK(txn.commit());
    }
}

TEST_CASE("delete_node_refuses_edges_unless_detach_and_leaves_no_trace") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    const NodeId a = node_named("a");
    const NodeId b = node_named("b");
    const EdgeId ab = edge_named("ab");
    const EdgeId ba = edge_named("ba");
    const EdgeId loop = edge_named("loop");
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.put_node(b));
        GRAPH_OK(txn.add_label(a, "L"));
        GRAPH_OK(txn.set_property(a, "big", std::string(1000, 'x')));
        GRAPH_OK(txn.put_edge(ab, a, "T", b));
        GRAPH_OK(txn.put_edge(ba, b, "T", a));
        GRAPH_OK(txn.put_edge(loop, a, "T", a));
        GRAPH_OK(txn.set_property(ab, "big", std::string(1000, 'y')));
        GRAPH_OK(txn.commit());
    }
    {
        WriteTxn txn = begin_write(graph);
        const Result<void> refused = txn.delete_node(a, false);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Error::Code::InvalidArgument);
        GRAPH_OK(txn.delete_node(a, true));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    CHECK_FALSE(*txn.has_node(a));
    CHECK(*txn.has_node(b));
    CHECK(collect(txn, b, Direction::Both).empty());
    CHECK(collect(txn.nodes_with_label("L")).empty());

    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM edges") == 0);
    CHECK(raw.scalar("SELECT count(*) FROM props") == 0);
    CHECK(raw.scalar("SELECT count(*) FROM labels") == 0);
    CHECK(raw.scalar("SELECT count(*) FROM large_values") == 0);

    // Silme çerçevesi kenarları açıkça taşır: uzak cihaz ilişkiyi kendi çıkarmak zorunda kalmaz.
    const std::vector<std::string> frames = raw.rows("SELECT frame FROM ops ORDER BY seq DESC LIMIT 1");
    const Result<detail::Frame> frame = detail::decode_frame(as_bytes(frames[0]));
    REQUIRE(frame.has_value());
    CHECK(frame->ops.size() == 4);
    CHECK(std::holds_alternative<detail::NodeDel>(frame->ops.back()));
}

TEST_CASE("overwriting_a_large_value_frees_the_old_blob_and_reads_see_own_writes") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph graph = open_graph(path);
    const NodeId a = node_named("a");
    WriteTxn txn = begin_write(graph);
    GRAPH_OK(txn.put_node(a));
    GRAPH_OK(txn.set_property(a, "k", std::string(1000, 'x')));
    GRAPH_OK(txn.set_property(a, "k", std::string(2000, 'y')));
    CHECK(*txn.property(a, "k") == Value{std::string(2000, 'y')});
    GRAPH_OK(txn.set_property(a, "k", std::string("small")));
    GRAPH_OK(txn.commit());

    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM large_values") == 0);
    CHECK(raw.scalar("SELECT count(*) FROM props") == 1);
}

TEST_CASE("one_open_txn_per_graph_and_finished_txn_refuses_calls") {
    TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    {
        WriteTxn txn = begin_write(graph);
        CHECK_FALSE(graph.read().has_value());
        CHECK_FALSE(graph.write().has_value());
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.commit());
        const Result<void> after = txn.put_node(a);
        REQUIRE_FALSE(after.has_value());
        CHECK(after.error().code == Error::Code::InvalidArgument);
        CHECK_FALSE(txn.commit().has_value());
        CHECK_FALSE(txn.has_node(a).has_value());
        // commit sonrası yeni işlem açılabilir.
        CHECK(graph.read().has_value());
    }
    {
        ReadTxn first = begin_read(graph);
        ReadTxn moved = std::move(first);
        CHECK_FALSE(
            first.has_node(a).has_value());  // NOLINT(bugprone-use-after-move) -- taşınmış işlem hata dönmeli
        CHECK(*moved.has_node(a));
    }
    CHECK(graph.write().has_value());
}

TEST_CASE("storage_failure_poisons_the_txn_so_partial_state_cannot_commit") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    const NodeId good = node_named("good");
    const NodeId damaged = node_named("damaged");
    Graph graph = open_graph(path);
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(damaged));
        GRAPH_OK(txn.commit());
    }
    {
        // Bozuk dots, uygulamayı yarıda düşürmenin en kısa yolu.
        RawDb raw(path);
        raw.exec("UPDATE nodes SET dots=x'01'");
    }
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(good));
        const Result<void> failed = txn.add_label(damaged, "L");
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == Error::Code::Corrupt);
        // Zehirlenmiş işlemde sonraki yazma da commit de reddedilir.
        CHECK_FALSE(txn.put_node(node_named("later")).has_value());
        CHECK_FALSE(txn.commit().has_value());
    }
    ReadTxn txn = begin_read(graph);
    CHECK_FALSE(*txn.has_node(good));
    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM ops") == 1);
    CHECK(raw.scalar("SELECT v FROM meta WHERE k='seq'") == 1);
}

TEST_CASE("second_writer_gets_busy_after_the_timeout_and_nothing_is_lost") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph first = open_graph(path);
    Graph second = open_graph(path);
    const NodeId a = node_named("a");
    WriteTxn holder = begin_write(first);
    GRAPH_OK(holder.put_node(a));

    const Result<WriteTxn> blocked = second.write();
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().code == Error::Code::Busy);

    // Yazıcı okuyucuyu engellemez (WAL) ve commit'i etkilenmez.
    {
        ReadTxn reader = begin_read(second);
        CHECK_FALSE(*reader.has_node(a));
    }
    GRAPH_OK(holder.commit());
    ReadTxn reader = begin_read(second);
    CHECK(*reader.has_node(a));
}

TEST_CASE("read_snapshot_does_not_see_later_commits_from_another_connection") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph reader = open_graph(path);
    Graph writer = open_graph(path);
    const NodeId a = node_named("a");

    ReadTxn snapshot = begin_read(reader);
    CHECK_FALSE(*snapshot.has_node(a));
    {
        WriteTxn txn = begin_write(writer);
        GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.commit());
    }
    CHECK_FALSE(*snapshot.has_node(a));
    ReadTxn fresh = begin_read(writer);
    CHECK(*fresh.has_node(a));
}
