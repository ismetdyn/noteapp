// SPEC-graph kabul 1 ve 8: yalnız kamu API'si üzerinden ölçülür. 50 ms kapısı gerçek Graph::query yolundan
// geçer: Expand ve Sort'un sorgu içi önbelleği burada sınanır; önbelleksiz yollar yalnız raporlanır.
#include "graph/graph.hpp"
#include "support/test_support.hpp"
#include "wine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <doctest/doctest.h>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace noteapp::graph;
using noteapp::test::kWineFactor;
using noteapp::test::running_under_wine;
using noteapp::test::TempDir;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSmallNodes = 5000;
constexpr int kLargeNodes = 50000;
constexpr int kFanOut = 4;
constexpr int kBatchNodes = 1000;
constexpr std::size_t kTopK = 20;
constexpr int kRuns = 5;
constexpr int kRatioStarts = 200;
constexpr int kRatioRuns = 21;
constexpr double kTwoHopBudgetMs = 50.0;
constexpr double kMaxRatio = 1.5;
constexpr std::string_view kLabel = "Item";
constexpr std::string_view kEdgeType = "LINKS";
constexpr std::string_view kRankKey = "rank";
constexpr std::size_t kBodyBytes = 2048;  // SPEC kabul 8: ortalama 2 KB
constexpr const char* kProductionQuery = "MATCH (n:Note)-[:LINKS_TO*1..2]->(m) WHERE n.origin = \"windows\" "
                                         "RETURN m.name ORDER BY m.updated_at DESC LIMIT 20";
constexpr const char* kFixedTwoHopQuery =
    "MATCH (s:Note)-[:LINKS_TO]->(x)-[:LINKS_TO]->(m) RETURN m.name ORDER BY m.updated_at DESC LIMIT 20";

template <class Work> double time_ms(Work&& work) {
    const auto start = Clock::now();
    work();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Work> double median_ms(int runs, Work&& work) {
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i)
        samples.push_back(time_ms(work));
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

template <class T> T must(Result<T> result) {
    REQUIRE_MESSAGE(result.has_value(), (result.has_value() ? "" : result.error().message_key));
    return std::move(*result);
}

void must(const Result<void>& result) {
    REQUIRE_MESSAGE(result.has_value(), (result.has_value() ? "" : result.error().message_key));
}

struct Dataset {
    TempDir dir;
    std::vector<NodeId> nodes;
    double load_ms = 0;
    std::filesystem::path path() const { return dir.path() / "graph.db"; }
};

void load(Dataset& data, int node_count) {
    Graph graph = must(Graph::open(data.path()));
    std::mt19937 random(20261101);
    data.nodes.reserve(static_cast<std::size_t>(node_count));
    for (int i = 0; i < node_count; ++i)
        data.nodes.push_back(new_node_id());
    std::uniform_int_distribution<std::size_t> pick(0, data.nodes.size() - 1);

    data.load_ms = time_ms([&] {
        for (int first = 0; first < node_count; first += kBatchNodes) {
            WriteTxn txn = must(graph.write());
            const int end = std::min(first + kBatchNodes, node_count);
            for (int i = first; i < end; ++i) {
                const NodeId id = data.nodes[static_cast<std::size_t>(i)];
                must(txn.put_node(id));
                must(txn.add_label(id, kLabel));
                must(txn.set_property(id, kRankKey, std::int64_t{i}));
            }
            must(txn.commit());
        }
        for (int first = 0; first < node_count; first += kBatchNodes) {
            WriteTxn txn = must(graph.write());
            const int end = std::min(first + kBatchNodes, node_count);
            for (int i = first; i < end; ++i)
                for (int edge = 0; edge < kFanOut; ++edge)
                    must(txn.put_edge(new_edge_id(), data.nodes[static_cast<std::size_t>(i)], kEdgeType,
                                      data.nodes[pick(random)]));
            must(txn.commit());
        }
    });
}

std::int64_t rank_of(ReadTxn& txn, const NodeId& id) {
    const Value value = must(txn.property(id, kRankKey));
    return std::get<std::int64_t>(value);
}

struct Ranked {
    std::int64_t rank;
    NodeId node;
    bool operator<(const Ranked& other) const {
        return std::tie(rank, node) < std::tie(other.rank, other.node);
    }
};

// LIMIT'li ORDER BY: yığında en iyi K kalır, bellek O(K).
class TopK {
public:
    void offer(std::int64_t rank, const NodeId& node) {
        const Ranked candidate{rank, node};
        if (heap_.size() < kTopK) {
            heap_.push(candidate);
        } else if (candidate < heap_.top()) {
            heap_.pop();
            heap_.push(candidate);
        }
    }
    std::size_t size() const { return heap_.size(); }

private:
    std::priority_queue<Ranked> heap_;
};

// MATCH (s:Item)-[e1:LINKS]->(m)-[e2:LINKS]->(t) ORDER BY <anahtar> LIMIT 20. e1 != e2: openCypher kenar
// benzersizliği.
template <class RankOf> std::size_t two_hop_all_starts(ReadTxn& txn, RankOf&& rank) {
    TopK top;
    std::size_t rows = 0;
    NodeCursor starts = must(txn.nodes_with_label(kLabel));
    while (const std::optional<NodeId> start = must(starts.next())) {
        NeighborCursor first = must(txn.neighbors(*start, Direction::Out, kEdgeType));
        while (const std::optional<Neighbor> hop = must(first.next())) {
            const EdgeId first_edge = hop->edge;
            NeighborCursor second = must(txn.neighbors(hop->node, Direction::Out, kEdgeType));
            while (const std::optional<Neighbor> target = must(second.next())) {
                if (target->edge == first_edge)
                    continue;
                ++rows;
                top.offer(rank(target->node), target->node);
            }
        }
    }
    REQUIRE(top.size() == kTopK);
    return rows;
}

double two_hop_fixed_starts_median_ms(Dataset& data) {
    Graph graph = must(Graph::open(data.path()));
    std::mt19937 random(7);
    std::uniform_int_distribution<std::size_t> pick(0, data.nodes.size() - 1);
    std::vector<NodeId> starts;
    for (int i = 0; i < kRatioStarts; ++i)
        starts.push_back(data.nodes[pick(random)]);

    return median_ms(kRatioRuns, [&] {
        ReadTxn txn = must(graph.read());
        std::size_t rows = 0;
        for (const NodeId& start : starts) {
            NeighborCursor first = must(txn.neighbors(start, Direction::Out, kEdgeType));
            while (const std::optional<Neighbor> hop = must(first.next())) {
                NeighborCursor second = must(txn.neighbors(hop->node, Direction::Out, kEdgeType));
                while (must(second.next()))
                    ++rows;
            }
        }
        REQUIRE(rows > 0);
    });
}

}  // namespace

TEST_CASE("graph_perf_load_upsert_scan_and_two_hop_within_budget") {
    Dataset data;
    load(data, kSmallNodes);
    std::printf("graph_perf load_%d_nodes_%d_edges_ms=%.1f\n", kSmallNodes, kSmallNodes * kFanOut,
                data.load_ms);

    Graph graph = must(Graph::open(data.path()));
    const double upsert_ms = median_ms(21, [&] {
        WriteTxn txn = must(graph.write());
        must(txn.set_property(data.nodes[0], kRankKey, std::int64_t{1}));
        must(txn.commit());
    });
    std::printf("graph_perf single_upsert_commit_ms=%.2f\n", upsert_ms);

    const double scan_ms = median_ms(kRuns, [&] {
        ReadTxn txn = must(graph.read());
        NodeCursor cursor = must(txn.nodes_with_label(kLabel));
        std::size_t count = 0;
        while (must(cursor.next()))
            ++count;
        REQUIRE(count == kSmallNodes);
    });
    std::printf("graph_perf label_scan_%d_ms=%.2f\n", kSmallNodes, scan_ms);

    const auto report = [&](const char* name, auto&& query) {
        std::size_t rows = 0;
        const double ms = median_ms(kRuns, [&] {
            ReadTxn txn = must(graph.read());
            rows = query(txn);
        });
        std::printf("graph_perf two_hop_%s_ms=%.2f rows=%zu\n", name, ms, rows);
        return ms;
    };
    report("order_by_id_no_property", [](ReadTxn& txn) {
        return two_hop_all_starts(
            txn, [](const NodeId& id) { return std::to_integer<std::int64_t>(id.bytes[15]); });
    });
    report("order_by_property_no_memo", [](ReadTxn& txn) {
        return two_hop_all_starts(txn, [&](const NodeId& id) { return rank_of(txn, id); });
    });
}

TEST_CASE("two_hop_ratio_5k_50k") {
    Dataset small;
    load(small, kSmallNodes);
    Dataset large;
    load(large, kLargeNodes);
    std::printf("graph_perf load_%d_nodes_%d_edges_ms=%.1f\n", kLargeNodes, kLargeNodes * kFanOut,
                large.load_ms);

    const double small_ms = two_hop_fixed_starts_median_ms(small);
    const double large_ms = two_hop_fixed_starts_median_ms(large);
    const double ratio = large_ms / small_ms;
    std::printf("graph_perf two_hop_fixed_%d_starts small_ms=%.3f large_ms=%.3f ratio=%.3f\n", kRatioStarts,
                small_ms, large_ms, ratio);
    CHECK(ratio <= kMaxRatio);
}

namespace {

// Uygulama şemasının kopyası: ad, zaman, köken ve gövde (büyük değer tablosuna gider). Köken hepsinde aynı:
// süzme başlangıçları azaltmaz, en kötü durum ölçülür.
std::vector<NodeId> load_notes(Graph& graph) {
    std::mt19937 random(20261101);
    std::vector<NodeId> ids;
    for (int i = 0; i < kSmallNodes; ++i)
        ids.push_back(new_node_id());
    std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
    const std::string body(kBodyBytes, 'x');
    for (int first = 0; first < kSmallNodes; first += kBatchNodes) {
        WriteTxn txn = must(graph.write());
        for (int i = first; i < std::min(first + kBatchNodes, kSmallNodes); ++i) {
            const NodeId id = ids[static_cast<std::size_t>(i)];
            must(txn.put_node(id));
            must(txn.add_label(id, "Note"));
            must(txn.set_property(id, "name", "note-" + std::to_string(i)));
            must(txn.set_property(id, "updated_at", Timestamp{i}));
            must(txn.set_property(id, "origin", std::string("windows")));
            must(txn.set_property(id, "body", body));
        }
        must(txn.commit());
    }
    for (int first = 0; first < kSmallNodes; first += kBatchNodes) {
        WriteTxn txn = must(graph.write());
        for (int i = first; i < std::min(first + kBatchNodes, kSmallNodes); ++i)
            for (int edge = 0; edge < kFanOut; ++edge)
                must(txn.put_edge(new_edge_id(), ids[static_cast<std::size_t>(i)], "LINKS_TO",
                                  ids[pick(random)]));
        must(txn.commit());
    }
    return ids;
}

double query_median_ms(Graph& graph, const char* text, QueryLimits limits, std::size_t& rows) {
    return median_ms(kRuns, [&] {
        const QueryResult result = must(graph.query(text, {}, QueryMode::ReadOnly, limits));
        rows = result.rows.size();
    });
}

}  // namespace

// Kabul 8'in kapısı: üretim sorgusu ve sabit iki atlama, gerçek Expand ve Sort ile. Süre kRuns ardışık sorgunun
// medyanıdır. NX-F: Wine yerli platform değil, Windows ikilisinin test ortamıdır (wine.hpp); yükte bu kapıdan
// düşüyordu. Release medyanı: üretim Linux 17,9 ms / Wine 22,3 ms, sabit Linux 14,8 ms / Wine 15,4 ms (Linux 3
// koşu, Wine geçen koşu). Wine'da sınır kWineFactor ile genişler, Linux ve Windows sınırı değişmez.
TEST_CASE("cypher_two_hop_5000_starts_within_budget_through_real_operators") {
    const double budget_ms = running_under_wine() ? kTwoHopBudgetMs * kWineFactor : kTwoHopBudgetMs;
    TempDir dir;
    Graph graph = must(Graph::open(dir.path() / "graph.db"));
    load_notes(graph);

    std::size_t rows = 0;
    const double production_ms = query_median_ms(graph, kProductionQuery, {}, rows);
    std::printf("graph_perf cypher_production_var_length_ms=%.2f rows=%zu\n", production_ms, rows);
    CHECK(rows == kTopK);
    CHECK(production_ms <= budget_ms);

    const double fixed_ms = query_median_ms(graph, kFixedTwoHopQuery, {}, rows);
    std::printf("graph_perf cypher_two_hop_fixed_ms=%.2f rows=%zu\n", fixed_ms, rows);
    CHECK(rows == kTopK);
    CHECK(fixed_ms <= budget_ms);

    // Yalnız rapor: önbelleğin neyi kazandırdığı sayıyla görünsün.
    QueryLimits no_memo;
    no_memo.max_memo_bytes = 0;
    const double no_memo_ms = query_median_ms(graph, kProductionQuery, no_memo, rows);
    std::printf("graph_perf cypher_production_no_memo_ms=%.2f rows=%zu\n", no_memo_ms, rows);

    // Üst sınır tutuyorsa bellek gerçekten sınırlıdır: küçük bütçede yine aynı sonuç, sorgu hata vermez.
    QueryLimits tiny_memo;
    tiny_memo.max_memo_bytes = 256U * 1024U;
    const QueryResult bounded = must(graph.query(kProductionQuery, {}, QueryMode::ReadOnly, tiny_memo));
    const QueryResult full = must(graph.query(kProductionQuery));
    CHECK(bounded.rows == full.rows);

    // Sıra gerçekten azalan: bütçe yanlış sonuçla tutturulmuş olmasın.
    const QueryResult ordered = must(graph.query(
        "MATCH (n:Note)-[:LINKS_TO*1..2]->(m) RETURN m.updated_at ORDER BY m.updated_at DESC LIMIT 20"));
    REQUIRE(ordered.rows.size() == kTopK);
    for (std::size_t i = 1; i < ordered.rows.size(); ++i)
        CHECK(std::get<Timestamp>(ordered.rows[i - 1][0].v).unix_ms >=
              std::get<Timestamp>(ordered.rows[i][0].v).unix_ms);
}
