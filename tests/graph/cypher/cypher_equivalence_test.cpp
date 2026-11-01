// Özellik testi: rastgele küçük graflarda motor, testteki saf bellek içi değerlendiriciyle aynı sonucu verir.
// Değerlendirici planlayıcıyı, ön süzmeyi ve önbelleği bilmez: tüm atamaları dener, kenar benzersizliğini
// sınar.
#include "cypher_support.hpp"

#include <doctest/doctest.h>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::test;

namespace {

struct ModelNode {
    int value;
    bool has_label;
    std::optional<int> extra;
};

struct ModelEdge {
    int src;
    int dst;
    char type;
};

struct Model {
    std::vector<ModelNode> nodes;
    std::vector<ModelEdge> edges;
};

Model random_model(std::mt19937& random) {
    Model model;
    const int node_count = std::uniform_int_distribution(1, 6)(random);
    const int edge_count = std::uniform_int_distribution(0, 10)(random);
    for (int i = 0; i < node_count; ++i) {
        ModelNode node{i, random() % 2 == 0, std::nullopt};
        if (random() % 2 == 0)
            node.extra = static_cast<int>(random() % 3);
        model.nodes.push_back(node);
    }
    for (int i = 0; i < edge_count; ++i)
        model.edges.push_back(ModelEdge{static_cast<int>(random() % static_cast<unsigned>(node_count)),
                                        static_cast<int>(random() % static_cast<unsigned>(node_count)),
                                        random() % 2 == 0 ? 'X' : 'Y'});
    return model;
}

void load_model(Graph& graph, const Model& model) {
    WriteTxn txn = begin_write(graph);
    std::vector<NodeId> ids;
    for (const ModelNode& node : model.nodes) {
        const NodeId id = node_named(std::to_string(node.value));
        ids.push_back(id);
        GRAPH_OK(txn.put_node(id));
        if (node.has_label)
            GRAPH_OK(txn.add_label(id, "L"));
        GRAPH_OK(txn.set_property(id, "v", std::int64_t{node.value}));
        if (node.extra)
            GRAPH_OK(txn.set_property(id, "e", std::int64_t{*node.extra}));
    }
    int index = 0;
    for (const ModelEdge& edge : model.edges) {
        const std::string name = std::to_string(index++);
        GRAPH_OK(txn.put_edge(edge_named(name), ids[static_cast<std::size_t>(edge.src)],
                              std::string(1, edge.type), ids[static_cast<std::size_t>(edge.dst)]));
    }
    GRAPH_OK(txn.commit());
}

enum class Way { Out, In, Any };

// Bir kenarın `from`'dan yürünüp yürünemeyeceği; varsa varış düğümü. Öz döngü Any'de bir kez geçer.
std::optional<int> walk(const ModelEdge& edge, int from, Way way, char type) {
    if (type != '*' && edge.type != type)
        return std::nullopt;
    const bool out = edge.src == from;
    const bool in = edge.dst == from;
    if (way == Way::Out)
        return out ? std::optional(edge.dst) : std::nullopt;
    if (way == Way::In)
        return in ? std::optional(edge.src) : std::nullopt;
    if (out)
        return edge.dst;
    return in ? std::optional(edge.src) : std::nullopt;
}

std::string join_row(std::initializer_list<std::string> parts) {
    std::string out;
    bool first = true;
    for (const std::string& part : parts) {
        out += (first ? "" : "|") + part;
        first = false;
    }
    return out;
}

// MATCH (a:L?)-[:T]-(b)-[:T]-(c), kenar benzersiz.
Strings naive_two_hop(const Model& model, bool labeled, Way way, char type, int min_c) {
    Strings rows;
    for (const ModelNode& a : model.nodes) {
        if (labeled && !a.has_label)
            continue;
        for (std::size_t e1 = 0; e1 < model.edges.size(); ++e1) {
            const auto b = walk(model.edges[e1], a.value, way, type);
            if (!b)
                continue;
            for (std::size_t e2 = 0; e2 < model.edges.size(); ++e2) {
                const auto c = e1 == e2 ? std::nullopt : walk(model.edges[e2], *b, way, type);
                if (c && *c >= min_c)
                    rows.push_back(
                        join_row({std::to_string(a.value), std::to_string(*b), std::to_string(*c)}));
            }
        }
    }
    return sorted(std::move(rows));
}

void trails(const Model& model, int at, Way way, char type, int depth, int min, int max,
            std::vector<std::size_t>& used, int start, Strings& rows) {
    for (std::size_t e = 0; e < model.edges.size(); ++e) {
        if (std::ranges::find(used, e) != used.end())
            continue;
        const auto next = walk(model.edges[e], at, way, type);
        if (!next)
            continue;
        if (depth + 1 >= min)
            rows.push_back(join_row({std::to_string(start), std::to_string(*next)}));
        if (depth + 1 < max) {
            used.push_back(e);
            trails(model, *next, way, type, depth + 1, min, max, used, start, rows);
            used.pop_back();
        }
    }
}

Strings naive_var_length(const Model& model, Way way, char type, int min, int max) {
    Strings rows;
    for (const ModelNode& node : model.nodes) {
        std::vector<std::size_t> used;
        trails(model, node.value, way, type, 0, min, max, used, node.value, rows);
    }
    return sorted(std::move(rows));
}

Strings naive_optional(const Model& model, char type) {
    Strings rows;
    for (const ModelNode& a : model.nodes) {
        bool matched = false;
        for (const ModelEdge& edge : model.edges) {
            const auto b = walk(edge, a.value, Way::Out, type);
            if (b && model.nodes[static_cast<std::size_t>(*b)].extra.value_or(-1) == 1) {
                rows.push_back(join_row({std::to_string(a.value), std::to_string(*b)}));
                matched = true;
            }
        }
        if (!matched)
            rows.push_back(join_row({std::to_string(a.value), "null"}));
    }
    return sorted(std::move(rows));
}

Strings naive_group(const Model& model) {
    std::map<int, std::pair<int, int>> groups;  // extra -> (count, sum of v)
    int null_count = 0;
    int null_sum = 0;
    for (const ModelNode& node : model.nodes) {
        if (node.extra) {
            ++groups[*node.extra].first;
            groups[*node.extra].second += node.value;
        } else {
            ++null_count;
            null_sum += node.value;
        }
    }
    Strings rows;
    for (const auto& [extra, totals] : groups)
        rows.push_back(
            join_row({std::to_string(extra), std::to_string(totals.first), std::to_string(totals.second)}));
    if (null_count > 0)
        rows.push_back(join_row({"null", std::to_string(null_count), std::to_string(null_sum)}));
    return sorted(std::move(rows));
}

Strings naive_top(const Model& model, char type, int limit) {
    // ORDER BY b.v DESC, a.v, adım sırası eşitlikte yoksayılır: yalnız kümeler karşılaştırılır.
    std::vector<std::pair<int, int>> pairs;
    for (const ModelNode& a : model.nodes)
        for (const ModelEdge& edge : model.edges)
            if (const auto b = walk(edge, a.value, Way::Out, type))
                pairs.emplace_back(*b, a.value);
    std::ranges::sort(pairs, [](const auto& l, const auto& r) {
        return l.first != r.first ? l.first > r.first : l.second < r.second;
    });
    Strings rows;
    for (std::size_t i = 0; i < pairs.size() && i < static_cast<std::size_t>(limit); ++i)
        rows.push_back(join_row({std::to_string(pairs[i].second), std::to_string(pairs[i].first)}));
    return rows;
}

void check_same(Graph& graph, const std::string& text, const Strings& expected, bool ordered = false) {
    INFO(text);
    QueryLimits no_memo;
    no_memo.max_memo_bytes = 0;
    Strings with_memo = lines(must_query(graph, text));
    Strings without_memo = lines(must_query(graph, text, {}, QueryMode::ReadOnly, no_memo));
    if (!ordered) {
        with_memo = sorted(std::move(with_memo));
        without_memo = sorted(std::move(without_memo));
    }
    CHECK(with_memo == expected);
    CHECK(without_memo == expected);
}

}  // namespace

TEST_CASE("engine_matches_a_naive_evaluator_on_random_small_graphs") {
    std::mt19937 random(20261101);
    for (int round = 0; round < 40; ++round) {
        CAPTURE(round);
        const Model model = random_model(random);
        GraphFixture f;
        load_model(f.graph, model);

        check_same(f.graph, "MATCH (a:L)-[:X]->(b)-[:X]->(c) RETURN a.v, b.v, c.v",
                   naive_two_hop(model, true, Way::Out, 'X', 0));
        check_same(f.graph, "MATCH (a)<-[:Y]-(b)<-[:Y]-(c) WHERE c.v >= 2 RETURN a.v, b.v, c.v",
                   naive_two_hop(model, false, Way::In, 'Y', 2));
        check_same(f.graph, "MATCH (a)-[]-(b)-[]-(c) RETURN a.v, b.v, c.v",
                   naive_two_hop(model, false, Way::Any, '*', 0));
        check_same(f.graph, "MATCH (a)-[:X*1..3]->(b) RETURN a.v, b.v",
                   naive_var_length(model, Way::Out, 'X', 1, 3));
        check_same(f.graph, "MATCH (a)-[*2..3]-(b) RETURN a.v, b.v",
                   naive_var_length(model, Way::Any, '*', 2, 3));
        check_same(f.graph, "MATCH (b)<-[:Y*1..2]-(a) RETURN b.v, a.v",
                   naive_var_length(model, Way::In, 'Y', 1, 2));
        check_same(f.graph, "MATCH (a) OPTIONAL MATCH (a)-[:X]->(b) WHERE b.e = 1 RETURN a.v, b.v",
                   naive_optional(model, 'X'));
        check_same(f.graph, "MATCH (n) RETURN n.e, count(*), sum(n.v)", naive_group(model));
        check_same(f.graph, "MATCH (a)-[:X]->(b) RETURN a.v, b.v ORDER BY b.v DESC, a.v LIMIT 5",
                   naive_top(model, 'X', 5), true);
    }
}
