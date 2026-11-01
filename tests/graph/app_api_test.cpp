// Uygulama katmanının motordan istediği, nottan habersiz okuma API'si: toplu baş okuma, nedensellik sorusu,
// dış değişiklik sayacı (G4).
#include "graph_test_support.hpp"
#include "sync_test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::test;
using noteapp::test::TempDir;

namespace {

const PropertyHead* head_of(const std::vector<PropertyHead>& heads, std::string_view key) {
    const auto found = std::ranges::find(heads, key, &PropertyHead::key);
    return found == heads.end() ? nullptr : &*found;
}

}  // namespace

TEST_CASE("properties returns every head of an owner without loading large values") {
    const TempDir dir;
    Graph graph = open_graph(dir.path() / "graph.db");
    const NodeId node = node_named("n");
    const std::string big(1000, 'x');
    {
        WriteTxn txn = begin_write(graph);
        GRAPH_OK(txn.put_node(node));
        GRAPH_OK(txn.set_property(node, "small", Value{std::string("abc")}));
        GRAPH_OK(txn.set_property(node, "big", Value{Bytes{big}}));
        GRAPH_OK(txn.commit());
    }
    ReadTxn txn = begin_read(graph);
    const auto heads = txn.properties(node);
    GRAPH_OK(heads);
    REQUIRE(heads->size() == 2);
    const PropertyHead* small = head_of(*heads, "small");
    REQUIRE(small != nullptr);
    CHECK_FALSE(small->large);
    CHECK(small->size == 3);
    CHECK(std::get<std::string>(small->value) == "abc");
    const PropertyHead* large = head_of(*heads, "big");
    REQUIRE(large != nullptr);
    CHECK(large->large);
    CHECK(large->size == big.size());
    CHECK(std::holds_alternative<std::monostate>(large->value));
    CHECK(large->device == graph.device());
    CHECK(large->seq == small->seq);
}

TEST_CASE("saw tells a write that saw another apart from a concurrent one") {
    Device alpha;
    Device beta;
    const NodeId node = node_named("n");
    alpha.commit([&](WriteTxn& txn) {
        GRAPH_OK(txn.put_node(node));
        GRAPH_OK(txn.set_property(node, "k", Value{std::int64_t{1}}));
    });
    beta.ingest({alpha.exported()});
    // beta gördükten sonra yazar: nedensel. alpha bunu görmeden yazar: eşzamanlı.
    beta.commit([&](WriteTxn& txn) { GRAPH_OK(txn.set_property(node, "after", Value{std::int64_t{2}})); });
    alpha.commit(
        [&](WriteTxn& txn) { GRAPH_OK(txn.set_property(node, "concurrent", Value{std::int64_t{3}})); });
    beta.ingest({alpha.exported()});

    ReadTxn txn = begin_read(beta.graph());
    const auto heads = txn.properties(node);
    GRAPH_OK(heads);
    const PropertyHead* first = head_of(*heads, "k");
    const PropertyHead* after = head_of(*heads, "after");
    const PropertyHead* concurrent = head_of(*heads, "concurrent");
    REQUIRE((first != nullptr && after != nullptr && concurrent != nullptr));
    const auto after_saw_first = txn.saw(*after, *first);
    GRAPH_OK(after_saw_first);
    CHECK(*after_saw_first);
    const auto after_saw_concurrent = txn.saw(*after, *concurrent);
    GRAPH_OK(after_saw_concurrent);
    CHECK_FALSE(*after_saw_concurrent);
    const auto concurrent_saw_first = txn.saw(*concurrent, *first);  // aynı cihaz: önceki seq görülmüştür
    GRAPH_OK(concurrent_saw_first);
    CHECK(*concurrent_saw_first);
}

TEST_CASE("data_version changes only on another connection's commit") {
    const TempDir dir;
    const auto path = dir.path() / "graph.db";
    Graph one = open_graph(path);
    Graph two = open_graph(path);
    const auto before = one.data_version();
    GRAPH_OK(before);
    {
        WriteTxn txn = begin_write(one);
        GRAPH_OK(txn.put_node(node_named("own")));
        GRAPH_OK(txn.commit());
    }
    const auto after_own = one.data_version();
    GRAPH_OK(after_own);
    CHECK(*after_own == *before);
    {
        WriteTxn txn = begin_write(two);
        GRAPH_OK(txn.put_node(node_named("other")));
        GRAPH_OK(txn.commit());
    }
    const auto after_other = one.data_version();
    GRAPH_OK(after_other);
    CHECK(*after_other != *before);
}

TEST_CASE("properties_at reads what one write set on an owner from the log, unknown writes are empty") {
    Device device;
    const NodeId node = node_named("n");
    const std::string big(1000, 'x');
    device.commit([&](WriteTxn& txn) {
        GRAPH_OK(txn.put_node(node));
        GRAPH_OK(txn.set_property(node, "body", Value{Bytes{big}}));
        GRAPH_OK(txn.set_property(node, "k", Value{std::int64_t{1}}));
    });
    Version first;
    {
        ReadTxn txn = begin_read(device.graph());
        const auto heads = txn.properties(node);
        GRAPH_OK(heads);
        first.device = heads->front().device;
        first.seq = heads->front().seq;
    }
    device.commit([&](WriteTxn& txn) { GRAPH_OK(txn.set_property(node, "body", Value{Bytes{"yeni"}})); });

    ReadTxn txn = begin_read(device.graph());
    const auto old = txn.properties_at(first, node);
    GRAPH_OK(old);
    REQUIRE(old->size() == 2);
    const PropertyHead* body = head_of(*old, "body");
    REQUIRE(body != nullptr);
    CHECK(std::get<Bytes>(body->value).data == big);
    CHECK(body->size == big.size());
    Version missing = first;
    missing.seq = 999;
    const auto none = txn.properties_at(missing, node);
    GRAPH_OK(none);
    CHECK(none->empty());
}
