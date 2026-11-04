// T84: haritanın saf birimleri. Yerleşim (GraphLayout) ve kapsam (build_note_map) ImGui ve depo bilmez.
#include "app/graph_layout.hpp"
#include "app/note_map.hpp"
#include "core/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <expected>
#include <limits>
#include <set>
#include <string>
#include <vector>

using noteapp::app::GraphLayout;
using noteapp::app::LayoutPoint;
using noteapp::app::MapEdgeKind;
using noteapp::app::MapNodeKind;
using noteapp::app::MapOptions;

namespace core = noteapp::core;

namespace {

constexpr float kK = GraphLayout::kIdealEdge;
constexpr int kEnoughIterations = 2000;  // yakınsamanın üst sınırı; aşılırsa test düşer

// Halka + kirişler: bağlı, düğüm başına birkaç kenar; yerleşim gerçekten iş yapsın.
struct Sample {
    std::vector<std::string> keys;
    std::vector<GraphLayout::Edge> edges;
};

Sample ring(std::size_t count) {
    Sample sample;
    for (std::size_t i = 0; i < count; ++i)
        sample.keys.push_back("note-" + std::to_string(i) + ".md");
    for (std::size_t i = 0; i < count; ++i) {
        sample.edges.push_back({i, (i + 1) % count});
        if (i % 3 == 0)
            sample.edges.push_back({i, (i + count / 2) % count});
    }
    return sample;
}

void settle(GraphLayout& layout) {
    for (int i = 0; i < kEnoughIterations && layout.step(1); ++i) {
    }
    REQUIRE(layout.settled());
}

float distance(LayoutPoint a, LayoutPoint b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

std::size_t index_of(const std::vector<std::string>& keys, const std::string& key) {
    return static_cast<std::size_t>(std::ranges::find(keys, key) - keys.begin());
}

}  // namespace

TEST_CASE("graph layout: the same graph always gets the same positions") {
    const Sample sample = ring(40);
    GraphLayout first;
    GraphLayout second;
    first.set_graph(sample.keys, sample.edges);
    second.set_graph(sample.keys, sample.edges);
    CHECK(first.positions().size() == 40);
    settle(first);
    settle(second);
    for (std::size_t i = 0; i < sample.keys.size(); ++i) {
        CHECK(first.positions()[i].x == second.positions()[i].x);
        CHECK(first.positions()[i].y == second.positions()[i].y);
    }
}

TEST_CASE("graph layout: it converges with lower energy, finite positions and separated nodes") {
    const Sample sample = ring(60);
    GraphLayout layout;
    layout.set_graph(sample.keys, sample.edges);
    const double before = layout.energy();
    CHECK_FALSE(layout.settled());
    settle(layout);
    CHECK(layout.iterations_done() > 0);
    CHECK(layout.iterations_done() < static_cast<std::size_t>(kEnoughIterations));
    CHECK(layout.energy() < before);
    float closest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < layout.size(); ++i) {
        const LayoutPoint p = layout.positions()[i];
        REQUIRE(std::isfinite(p.x));
        REQUIRE(std::isfinite(p.y));
        for (std::size_t j = i + 1; j < layout.size(); ++j)
            closest = std::min(closest, distance(p, layout.positions()[j]));
    }
    CHECK(closest > kK * 0.3F);
}

TEST_CASE("graph layout: coincident nodes are pushed apart without NaN") {
    GraphLayout layout;
    layout.set_graph({"a", "b", "c"}, {{0, 1}});
    // a, b ve c'nin üstüne bırakılır: sıfır uzaklıkta itmenin yönü tanımsızdır.
    layout.pin(0, layout.positions()[1]);
    layout.pin(2, layout.positions()[1]);
    settle(layout);
    for (const LayoutPoint p : layout.positions()) {
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
    }
    CHECK(distance(layout.positions()[1], layout.positions()[0]) > kK * 0.3F);
}

TEST_CASE("graph layout: a settled layout stops stepping") {
    const Sample sample = ring(30);
    GraphLayout layout;
    layout.set_graph(sample.keys, sample.edges);
    settle(layout);
    const std::size_t done = layout.iterations_done();
    CHECK_FALSE(layout.step(10));
    CHECK(layout.iterations_done() == done);
}

TEST_CASE("graph layout: a pinned node stays where it was dropped") {
    const Sample sample = ring(30);
    GraphLayout layout;
    layout.set_graph(sample.keys, sample.edges);
    settle(layout);
    layout.pin(3, {1234.0F, -567.0F});
    CHECK(layout.pinned(3));
    CHECK_FALSE(layout.settled());  // komşular yeniden yerleşir
    settle(layout);
    CHECK(layout.positions()[3].x == 1234.0F);
    CHECK(layout.positions()[3].y == -567.0F);
}

TEST_CASE("graph layout: nodes that still exist keep their position and pin across a rebuild") {
    const Sample sample = ring(20);
    GraphLayout layout;
    layout.set_graph(sample.keys, sample.edges);
    settle(layout);
    layout.pin(5, {900.0F, 900.0F});
    settle(layout);
    const LayoutPoint kept = layout.positions()[2];
    const LayoutPoint anchor = layout.positions()[7];

    // 0 gitti, yeni not 7'ye bağlı; sıra da değişti.
    std::vector<std::string> keys(sample.keys.begin() + 1, sample.keys.end());
    keys.emplace_back("new.md");
    std::vector<GraphLayout::Edge> edges{{index_of(keys, "note-7.md"), index_of(keys, "new.md")},
                                         {index_of(keys, "note-2.md"), index_of(keys, "note-3.md")}};
    layout.set_graph(keys, edges);
    REQUIRE(layout.size() == 20);
    const std::size_t two = index_of(keys, "note-2.md");
    CHECK(layout.positions()[two].x == kept.x);
    CHECK(layout.positions()[two].y == kept.y);
    CHECK(layout.pinned(index_of(keys, "note-5.md")));
    CHECK(layout.positions()[index_of(keys, "note-5.md")].x == 900.0F);
    CHECK(distance(layout.positions()[index_of(keys, "new.md")], anchor) < 2.0F * kK);
}

TEST_CASE("graph layout: rebuilding the identical graph does not wake the layout") {
    const Sample sample = ring(20);
    GraphLayout layout;
    layout.set_graph(sample.keys, sample.edges);
    settle(layout);
    layout.set_graph(sample.keys, sample.edges);
    CHECK(layout.settled());
}

namespace {

core::NoteMeta meta(const std::string& path, std::vector<std::string> tags = {}) {
    return core::NoteMeta{.path = core::NotePath::parse(path).value(), .tags = std::move(tags)};
}

core::NoteEdge link(const std::string& from, const std::string& to,
                    core::EdgeKind kind = core::EdgeKind::LinksTo) {
    return {core::NotePath::parse(from).value(), core::NotePath::parse(to).value(), kind};
}

std::set<std::string> keys_of(const noteapp::app::NoteMap& map) {
    std::set<std::string> keys;
    for (const auto& node : map.nodes)
        keys.insert((node.kind == MapNodeKind::Tag ? "#" : "") + node.key);
    return keys;
}

// a -> b -> c <- d, d ~ e (ilişki), f yalnız.
struct Chain {
    std::vector<core::NoteMeta> notes{meta("a.md"), meta("b.md"), meta("c.md"),
                                      meta("d.md"), meta("e.md"), meta("f.md")};
    std::vector<core::NoteEdge> edges{link("a.md", "b.md"), link("b.md", "c.md"), link("d.md", "c.md"),
                                      link("d.md", "e.md", core::EdgeKind::RelatedTo)};
};

}  // namespace

TEST_CASE("note map: the neighbourhood follows links both ways up to the depth") {
    const Chain chain;
    MapOptions options;
    options.depth = 1;
    const auto center = core::NotePath::parse("c.md").value();
    CHECK(keys_of(build_note_map(chain.notes, chain.edges, center, options)) ==
          std::set<std::string>{"b.md", "c.md", "d.md"});
    options.depth = 2;
    const auto two = build_note_map(chain.notes, chain.edges, center, options);
    CHECK(keys_of(two) == std::set<std::string>{"a.md", "b.md", "c.md", "d.md", "e.md"});
    CHECK(two.nodes.front().key == "c.md");  // merkez önce
    CHECK_FALSE(two.capped());
    CHECK(two.edges.size() == 4);
    const auto related = std::ranges::count_if(
        two.edges, [](const noteapp::app::MapEdge& edge) { return edge.kind == MapEdgeKind::Related; });
    CHECK(related == 1);
}

TEST_CASE("note map: the whole graph, or no current note, shows every note") {
    const Chain chain;
    MapOptions options;
    options.whole_graph = true;
    const auto center = core::NotePath::parse("c.md").value();
    CHECK(build_note_map(chain.notes, chain.edges, center, options).nodes.size() == 6);
    CHECK(build_note_map(chain.notes, chain.edges, std::nullopt, MapOptions{}).nodes.size() == 6);
}

TEST_CASE("note map: the cap keeps the nearest or newest notes and reports the total") {
    const Chain chain;
    MapOptions whole;
    whole.whole_graph = true;
    const auto capped = build_note_map(chain.notes, chain.edges, std::nullopt, whole, 3);
    CHECK(keys_of(capped) == std::set<std::string>{"a.md", "b.md", "c.md"});  // liste sırası: yeni önce
    CHECK(capped.capped());
    CHECK(capped.total == 6);
    for (const auto& edge : capped.edges) {  // kenar yalnız çizilen düğümler arasında
        CHECK(edge.from < 3);
        CHECK(edge.to < 3);
    }
    MapOptions near;
    near.depth = 2;
    const auto around =
        build_note_map(chain.notes, chain.edges, core::NotePath::parse("c.md").value(), near, 3);
    CHECK(keys_of(around) == std::set<std::string>{"b.md", "c.md", "d.md"});
    CHECK(around.total == 5);
}

TEST_CASE("note map: tags are optional nodes and do not widen the neighbourhood") {
    const std::vector<core::NoteMeta> notes{meta("a.md", {"proje"}), meta("b.md", {"proje", "iş"}),
                                            meta("c.md", {"proje"})};
    const std::vector<core::NoteEdge> edges{link("a.md", "b.md")};
    MapOptions options;
    options.depth = 1;
    const auto center = core::NotePath::parse("a.md").value();
    CHECK(keys_of(build_note_map(notes, edges, center, options)) == std::set<std::string>{"a.md", "b.md"});
    options.show_tags = true;
    const auto tagged = build_note_map(notes, edges, center, options);
    CHECK(keys_of(tagged) == std::set<std::string>{"a.md", "b.md", "#proje", "#iş"});
    CHECK(std::ranges::count_if(tagged.edges, [](const noteapp::app::MapEdge& edge) {
              return edge.kind == MapEdgeKind::Tag;
          }) == 3);
    // Sınır önce notlara: etiket yer kalırsa girer.
    const auto tight = build_note_map(notes, edges, center, options, 3);
    CHECK(tight.nodes.size() == 3);
    CHECK(tight.capped());
    CHECK(tight.total == 4);
}

// Sabit tavan yok: düğüm sınırı limits.app.map_nodes (0 = sınırsız, varsayılan 2.000); iki arayüz aynı model.
TEST_CASE("note map: the node cap is limits.app.map_nodes and 0 is unlimited") {
    CHECK(core::parse_config("").config.limits.app.map_nodes == core::Limit{2000, "2000"});
    CHECK(core::parse_config("[limits.app]\nmap_nodes = 0\n").config.limits.app.map_nodes.unlimited());
    const Chain chain;
    const auto edges = [&]() -> std::expected<std::vector<core::NoteEdge>, core::Error> {
        return chain.edges;
    };
    noteapp::app::MapModel model;
    model.set_options(MapOptions{.whole_graph = true});
    model.set_node_limit(core::Limit{3, "3"});
    model.update(edges, chain.notes, std::nullopt);
    CHECK(model.map().nodes.size() == 3);
    CHECK(model.map().capped());
    model.set_node_limit(core::Limit{});
    model.update(edges, chain.notes, std::nullopt);
    CHECK(model.map().nodes.size() == 6);
    CHECK_FALSE(model.map().capped());
}
