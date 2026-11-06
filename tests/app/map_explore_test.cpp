// F2: haritada keşif (plan-graph-map). Komşu aç/kapa, kenar ve etiket süzgeci, en kısa yol, sabitleme, haritada
// bul ve haritadan oluşturulan notun yeri. Hepsi saf modeldedir (app::MapModel, build_note_map); GUI ve TUI
// aynı modeli çağırır.
#include "app/graph_layout.hpp"
#include "app/note_map.hpp"

#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace app = noteapp::app;
namespace core = noteapp::core;
using app::MapEdgeKind;
using app::MapExpansion;
using app::MapNodeKind;
using app::MapOptions;

namespace {

core::NoteMeta meta(const std::string& path, std::vector<std::string> tags = {}) {
    return core::NoteMeta{.path = core::NotePath::parse(path).value(), .tags = std::move(tags)};
}

core::NoteEdge edge(const std::string& from, const std::string& to, core::EdgeKind kind = core::EdgeKind::LinksTo,
                    std::string name = {}, bool directed = false) {
    return {core::NotePath::parse(from).value(), core::NotePath::parse(to).value(), kind, std::move(name), directed};
}

core::NoteEdge relation(const std::string& from, const std::string& to, std::string name, bool directed = false) {
    return edge(from, to, core::EdgeKind::RelatedTo, std::move(name), directed);
}

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

// a → b → c → d → e (bağlantı), a ~ f ("uses", yönsüz), f → g ("owns", yönlü), h yalnız.
// Etiketler: b #proje, c #proje/alt, d #projeler.
struct Graph {
    std::vector<core::NoteMeta> notes{meta("a.md"), meta("b.md", {"proje"}), meta("c.md", {"proje/alt"}),
                                      meta("d.md", {"projeler"}), meta("e.md"), meta("f.md"), meta("g.md"),
                                      meta("h.md")};
    std::vector<core::NoteEdge> edges{edge("a.md", "b.md"),        edge("b.md", "c.md"),
                                      edge("c.md", "d.md"),        edge("d.md", "e.md"),
                                      relation("a.md", "f.md", "uses"), relation("f.md", "g.md", "owns", true)};
    app::MapModel model;

    Graph() { model.set_options(MapOptions{.depth = 1}); }
    void update(const std::string& center = "a.md") {
        model.update([this]() -> std::expected<std::vector<core::NoteEdge>, core::Error> { return edges; }, notes,
                     np(center));
    }
    std::set<std::string> keys() const {
        std::set<std::string> out;
        for (const app::MapNode& node : model.map().nodes)
            out.insert((node.kind == MapNodeKind::Tag ? "#" : "") + node.key);
        return out;
    }
    std::vector<std::string> path() const {
        std::vector<std::string> out;
        for (const std::size_t node : model.map().path)
            out.push_back(model.map().nodes[node].key);
        return out;
    }
    std::size_t index_of(const std::string& key) const {
        const auto& nodes = model.map().nodes;
        return static_cast<std::size_t>(std::ranges::find(nodes, key, &app::MapNode::key) - nodes.begin());
    }
};

using Keys = std::set<std::string>;

// Çizilen yolun uzunluğu; bulunmadıysa ya da sınır gizlediyse nullopt.
std::optional<std::size_t> found(const app::MapPathResult& result) {
    return result.status == app::MapPathResult::Status::Found ? std::optional(result.hops) : std::nullopt;
}

}  // namespace

TEST_CASE("map explore: expanding a node shows its neighbours past the depth; again adds a hop; collapse hides") {
    Graph g;
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "f.md"});
    g.model.expand("b.md", {});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md"});
    CHECK(g.model.scope().expanded.at("b.md").depth == 1);
    g.model.expand("b.md", {});  // aynı açılış: bir adım daha
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "d.md", "f.md"});
    CHECK(g.model.scope().expanded.at("b.md").depth == 2);

    g.model.collapse("b.md");  // b'nin içinden geçilmez: c ve d gider, b kalır
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "f.md"});
    CHECK_FALSE(g.model.scope().expanded.contains("b.md"));
    g.model.collapse("a.md");  // merkez kapanınca yalnız kendisi
    g.update();
    CHECK(g.keys() == Keys{"a.md"});
    g.model.expand("a.md", {});  // açmak kapamayı kaldırır; merkezde derinliğin bir adım ötesi
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
}

// İnceleme bulgusu: açılış düğümden yürüyüşü derinlikten kısa tutuyordu; merkezde ve çevrenin içinde hiçbir şey
// eklemiyor, ikinci çift tıklama haritayı merkeze indiriyordu. Açılış o düğümden görünen erimin ötesine gider,
// kapatma yalnız açılışın eklediğini geri alır.
TEST_CASE("map explore: opening the centre adds one hop past the depth; toggling again restores the same map") {
    Graph g;
    g.update();
    const Keys before = g.keys();
    CHECK(g.model.toggle_neighbours("a.md", {}));
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
    g.model.expand("a.md", {});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "d.md", "f.md", "g.md"});
    CHECK_FALSE(g.model.toggle_neighbours("a.md", {}));
    g.update();
    CHECK(g.keys() == before);
    CHECK_FALSE(g.model.scope().collapsed.contains("a.md"));
    CHECK_FALSE(g.model.scope().expanded.contains("a.md"));
}

TEST_CASE("map explore: opening a note inside the neighbourhood reaches one hop past what it already shows") {
    Graph g;
    g.model.set_options(MapOptions{.depth = 2});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
    g.model.expand("b.md", {});  // b bir adımda: c'yi zaten gösteriyor, açılış d'ye uzanır
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "d.md", "f.md", "g.md"});
    g.model.collapse("b.md");  // açılmış: yalnız açılışın eklediği gider
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
}

TEST_CASE("map explore: an expansion by kind or relation name follows only those edges") {
    Graph g;
    g.update("b.md");
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md"});
    g.model.expand("a.md", {.kind = MapEdgeKind::Related, .name = "USES"});  // ad tag_key ile eşlenir
    g.update("b.md");
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md"});
    g.model.expand("a.md", {.kind = MapEdgeKind::Related, .name = "uses"});  // ikinci adım: owns "uses" değil
    g.update("b.md");
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md"});
    g.model.expand("a.md", {.kind = MapEdgeKind::Related});  // başka açılış: yeniden bir adım
    g.update("b.md");
    CHECK(g.model.scope().expanded.at("a.md").depth == 1);
    g.model.expand("a.md", {.kind = MapEdgeKind::Related});
    g.update("b.md");
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
    g.model.expand("c.md", {.kind = MapEdgeKind::Related});  // c'nin ilişkisi yok
    g.update("b.md");
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "f.md", "g.md"});
}

TEST_CASE("map explore: toggle opens then closes; an expansion's depth stops at limits.app.map_depth") {
    Graph g;
    g.model.set_depth_limit(core::Limit{2, "2"});
    g.update();
    CHECK(g.model.toggle_neighbours("b.md", {}));
    g.update();
    CHECK(g.keys().contains("c.md"));
    CHECK_FALSE(g.model.toggle_neighbours("b.md", {}));
    g.update();
    CHECK_FALSE(g.keys().contains("c.md"));
    CHECK_FALSE(g.model.scope().expanded.contains("b.md"));
    CHECK_FALSE(g.model.scope().collapsed.contains("b.md"));  // kapatma açılışı geri alır, gizlemez
    for (int i = 0; i < 5; ++i)
        g.model.expand("b.md", {});
    CHECK(g.model.scope().expanded.at("b.md").depth == 2);
}

TEST_CASE("map explore: the whole graph ignores expansions and collapses") {
    Graph g;
    g.model.set_options(MapOptions{.whole_graph = true});
    g.model.collapse("a.md");
    g.update();
    CHECK(g.keys().size() == 8);
}

TEST_CASE("map explore: link and relation filters hide edges and the walk follows only the shown ones") {
    Graph g;
    g.model.set_options(MapOptions{.depth = 3, .show_related = false});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md", "d.md"});
    CHECK(std::ranges::none_of(g.model.map().edges,
                               [](const app::MapEdge& e) { return e.kind == MapEdgeKind::Related; }));
    g.model.set_options(MapOptions{.depth = 3, .show_links = false});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "f.md", "g.md"});
    g.model.set_options(MapOptions{.depth = 3, .show_links = false, .relation = "Uses"});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "f.md"});
    // Boş ad adsız ilişkidir: burada hiç yok.
    g.model.set_options(MapOptions{.depth = 3, .show_links = false, .relation = ""});
    g.update();
    CHECK(g.keys() == Keys{"a.md"});
}

TEST_CASE("map explore: the tag filter keeps notes with the tag by the search tag: rule; the center stays") {
    Graph g;
    g.model.set_options(MapOptions{.whole_graph = true, .tag = "#Proje"});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md"});  // proje/alt iç içe girer, projeler girmez
    g.model.set_options(MapOptions{.depth = 4, .tag = "proje"});
    g.update();
    CHECK(g.keys() == Keys{"a.md", "b.md", "c.md"});
    g.model.set_options(MapOptions{.whole_graph = true, .tag = "/"});  // geçersiz etiket hiçbir notu geçirmez
    g.update();
    CHECK(g.keys() == Keys{"a.md"});
}

TEST_CASE("map explore: shortest_path is a BFS over the edges, optionally along their direction") {
    // 0 → 1 → 2, 0 ~ 3 (yönsüz ilişki), 3 → 2 (yönlü ilişki), 4 yalnız.
    const std::vector<app::MapEdge> edges{{0, 1, MapEdgeKind::Link},
                                          {1, 2, MapEdgeKind::Link},
                                          {0, 3, MapEdgeKind::Related},
                                          {3, 2, MapEdgeKind::Related, "x", true}};
    CHECK(app::shortest_path(5, edges, 0, 2, false) == std::vector<std::size_t>{0, 1, 2});
    CHECK(app::shortest_path(5, edges, 2, 0, false) == std::vector<std::size_t>{2, 1, 0});
    CHECK(app::shortest_path(5, edges, 2, 0, true) == std::nullopt);
    CHECK(app::shortest_path(5, edges, 3, 0, true) == std::vector<std::size_t>{3, 0});  // yönsüz ilişki iki yönlü
    CHECK(app::shortest_path(5, edges, 0, 4, false) == std::nullopt);
    CHECK(app::shortest_path(5, edges, 1, 1, true) == std::vector<std::size_t>{1});
}

TEST_CASE("map explore: a found path is shown and highlighted beyond the depth; no path clears it") {
    Graph g;
    g.update();
    CHECK(found(g.model.find_path(g.notes, "a.md", "e.md", false)) == std::optional<std::size_t>{4});
    g.update();
    CHECK(g.path() == std::vector<std::string>{"a.md", "b.md", "c.md", "d.md", "e.md"});
    const auto& on_path = g.model.map().path_edges;
    REQUIRE(on_path.size() == g.model.map().edges.size());
    CHECK(std::ranges::count(on_path, 1) == 4);

    CHECK(found(g.model.find_path(g.notes, "g.md", "a.md", true)) == std::nullopt);
    g.update();
    CHECK(g.model.map().path.empty());
    CHECK(found(g.model.find_path(g.notes, "g.md", "a.md", false)) == std::optional<std::size_t>{2});
    g.update();
    CHECK(g.path() == std::vector<std::string>{"g.md", "f.md", "a.md"});
    CHECK(found(g.model.find_path(g.notes, "a.md", "h.md", false)) == std::nullopt);
    g.update();
    CHECK(g.model.map().path.empty());
    CHECK(g.keys() == Keys{"a.md", "b.md", "f.md"});
}

TEST_CASE("map explore: a path between notes already shown is highlighted without rebuilding the layout") {
    Graph g;
    g.model.set_options(MapOptions{.whole_graph = true});
    g.update();
    const std::size_t generation = g.model.generation();
    const std::size_t version = g.model.version();
    REQUIRE(found(g.model.find_path(g.notes, "a.md", "g.md", true)));
    g.update();
    CHECK(g.path() == std::vector<std::string>{"a.md", "f.md", "g.md"});
    CHECK(g.model.generation() == generation);
    CHECK(g.model.version() > version);
    g.model.clear_path();
    g.update();
    CHECK(g.model.map().path.empty());
    CHECK(std::ranges::count(g.model.map().path_edges, 1) == 0);
}

// İnceleme bulguları: düğüm sınırı yolun bir düğümünü kesince "N adım" denip hiçbir şey çizilmiyordu; yönlü yolda
// ters yöndeki paralel kenar da yanıyordu.
TEST_CASE("map explore: a path the node limit cannot draw is reported hidden, not found") {
    Graph g;
    g.model.set_node_limit(core::Limit{3, "3"});
    g.update();
    const app::MapPathResult result = g.model.find_path(g.notes, "a.md", "e.md", false);
    CHECK(result.status == app::MapPathResult::Status::Hidden);
    CHECK(result.hops == 4);
    g.update();
    CHECK(g.model.map().path.empty());
    CHECK_FALSE(g.model.scope().path);
    CHECK(found(g.model.find_path(g.notes, "a.md", "c.md", false)) == std::optional<std::size_t>{2});
}

TEST_CASE("map explore: a directed path lights only the edges along its direction") {
    app::MapModel model;
    const std::vector<core::NoteMeta> notes{meta("x.md"), meta("y.md")};
    const std::vector<core::NoteEdge> edges{edge("x.md", "y.md"), edge("y.md", "x.md")};
    const auto read = [&]() -> std::expected<std::vector<core::NoteEdge>, core::Error> { return edges; };
    model.update(read, notes, np("x.md"));
    REQUIRE(found(model.find_path(notes, "x.md", "y.md", true)));
    model.update(read, notes, np("x.md"));
    const app::NoteMap& map = model.map();
    REQUIRE(map.edges.size() == 2);
    for (std::size_t e = 0; e < map.edges.size(); ++e)
        CHECK((map.path_edges[e] != 0) == (map.nodes[map.edges[e].from].key == "x.md"));
    REQUIRE(found(model.find_path(notes, "x.md", "y.md", false)));  // yönsüzde iki kenar da yolun adımıdır
    model.update(read, notes, np("x.md"));
    CHECK(std::ranges::count(model.map().path_edges, 1) == 2);
}

TEST_CASE("map explore: openings, closings, kept notes and a path of notes that are gone are dropped") {
    Graph g;
    g.update();  // kenarlar okunsun
    g.model.expand("b.md", {});
    g.model.collapse("c.md");
    g.model.keep("h.md", app::LayoutPoint{1.0F, 2.0F});
    REQUIRE(found(g.model.find_path(g.notes, "a.md", "e.md", false)));
    g.update();
    // b, c, e ve h yeniden adlandırıldı: eski anahtarlar kalırsa yeni adlı not açık ya da sabit doğardı.
    std::erase_if(g.notes, [](const core::NoteMeta& note) {
        const std::string path = note.path.string();
        return path == "b.md" || path == "c.md" || path == "e.md" || path == "h.md";
    });
    g.model.edges_changed();
    g.update();
    CHECK(g.model.scope().expanded.empty());
    CHECK(g.model.scope().collapsed.empty());
    CHECK(g.model.scope().kept.empty());
    CHECK_FALSE(g.model.scope().path);
}

TEST_CASE("map explore: pin keeps a node in place, unpin and unpin all release it") {
    app::GraphLayout layout;
    layout.set_graph({"n:a", "n:b", "n:c"}, {{0, 1}, {1, 2}});
    layout.pin(0, {10.0F, 20.0F});
    layout.pin(1, layout.positions()[1]);
    CHECK(layout.pinned(0));
    CHECK(layout.has_pins());
    layout.unpin(0);
    CHECK_FALSE(layout.pinned(0));
    CHECK(layout.pinned(1));
    CHECK_FALSE(layout.settled());  // çözülen düğüm yeniden yerleşir
    layout.unpin_all();
    CHECK_FALSE(layout.pinned(1));
    CHECK_FALSE(layout.has_pins());
}

TEST_CASE("map explore: a note made on the map stays shown and is pinned where it was made") {
    Graph g;
    g.update();
    g.model.keep("h.md", app::LayoutPoint{300.0F, -40.0F});
    g.update();
    REQUIRE(g.keys().contains("h.md"));
    const std::size_t node = g.index_of("h.md");
    CHECK(g.model.layout().pinned(node));
    CHECK(g.model.layout().positions()[node].x == doctest::Approx(300.0F));
    CHECK(g.model.layout().positions()[node].y == doctest::Approx(-40.0F));
    g.update("c.md");  // merkez değişse de kalır
    CHECK(g.keys().contains("h.md"));
}

TEST_CASE("map explore: find matches nodes by their folded path or #tag, in map order") {
    Graph g;
    g.notes.push_back(meta("İş/Şema.md", {"proje"}));
    g.model.set_options(MapOptions{.whole_graph = true, .show_tags = true});
    g.update();
    const app::NoteMap& map = g.model.map();
    const auto keys = [&](const std::vector<std::size_t>& found) {
        std::vector<std::string> out;
        for (const std::size_t node : found)
            out.push_back((map.nodes[node].kind == MapNodeKind::Tag ? "#" : "") + map.nodes[node].key);
        return out;
    };
    CHECK(keys(app::find_map_nodes(map, "sema")) == std::vector<std::string>{"İş/Şema.md"});
    CHECK(keys(app::find_map_nodes(map, " is/ ")) == std::vector<std::string>{"İş/Şema.md"});
    CHECK(keys(app::find_map_nodes(map, "#proje")) == std::vector<std::string>{"#proje", "#proje/alt", "#projeler"});
    CHECK(app::find_map_nodes(map, "").empty());
    CHECK(app::find_map_nodes(map, "zzz").empty());
}
