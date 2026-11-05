// MAP-R: haritada ilişki kurma ve kaldırma (app::map_relations). GUI ve TUI aynı kuralı çağırır; burada
// gerçek depoyla: ilişki add/remove_relation, bağlantı ve etiket notun metnine yazılır.
#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>

namespace app = noteapp::app;
namespace core = noteapp::core;
using app::MapEdgeKind;
using app::MapNode;
using app::MapNodeKind;

namespace {

core::NotePath np(const std::string& text) {
    return *core::NotePath::parse(text);
}

MapNode note(const std::string& key) {
    return {MapNodeKind::Note, key};
}

MapNode tag(const std::string& key) {
    return {MapNodeKind::Tag, key};
}

struct Scene {
    noteapp::test::TempDir dir;
    core::NoteStore store{dir.path()};

    Scene() { core::init_i18n("en"); }
    void put(const std::string& name, const std::string& body) {
        REQUIRE(noteapp::test::seed_note(store, name, body));
    }
    std::string body(const std::string& name) const {
        const auto content = store.read(np(name));
        return content ? content->text : "<missing>";
    }
    core::NoteLinks links(const std::string& name) const { return store.links(np(name)).value(); }
    bool has_edge(const std::string& from, const std::string& to, core::EdgeKind kind) const {
        const auto edges = store.edges().value();
        return std::ranges::find(edges, core::NoteEdge{np(from), np(to), kind}) != edges.end();
    }
};

}  // namespace

TEST_CASE("map relations: a drop decides by the node kinds") {
    CHECK(app::map_drop(note("a.md"), note("b.md")) == app::MapDrop::ChooseNoteEdge);
    CHECK(app::map_drop(note("a.md"), tag("proje")) == app::MapDrop::AddTag);
    CHECK(app::map_drop(tag("proje"), note("a.md")) == app::MapDrop::AddTag);
    CHECK(app::map_drop(tag("proje"), tag("iş")) == app::MapDrop::None);
    CHECK(app::map_drop(note("a.md"), note("a.md")) == app::MapDrop::None);
    CHECK(app::map_drop(tag("a.md"), note("a.md")) == app::MapDrop::AddTag);  // aynı ad, farklı tür
}

TEST_CASE("map relations: note to note as Related adds RELATED_TO and leaves both texts alone") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b\n");
    const auto done = app::connect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Related);
    REQUIRE(done.has_value());
    CHECK(*done == "Related: a.md and b.md");
    CHECK(s.links("a.md").related == std::vector{np("b.md")});
    CHECK(s.body("a.md") == "a\n");
    CHECK(s.body("b.md") == "b\n");
    CHECK_FALSE(app::map_edit_note(note("a.md"), note("b.md"), MapEdgeKind::Related));
    const auto again = app::connect_map_nodes(s.store, note("b.md"), note("a.md"), MapEdgeKind::Related);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::AlreadyExists);
}

TEST_CASE("map relations: note to note as Link appends the full path as a wiki link to the source") {
    Scene s;
    s.put("a.md", "a metni");  // satır sonu yok: bağlantı kendi satırına gelir
    s.put("dir/b.md", "b\n");
    s.put("b.md", "başka b\n");  // kısa ad `b` buna da çözülebilirdi: yol tam yazılır
    CHECK(app::map_edit_note(note("a.md"), note("dir/b.md"), MapEdgeKind::Link) == np("a.md"));
    const auto done = app::connect_map_nodes(s.store, note("a.md"), note("dir/b.md"), MapEdgeKind::Link);
    REQUIRE(done.has_value());
    CHECK(*done == "Link added to a.md: [[dir/b.md]]");
    CHECK(s.body("a.md") == "a metni\n[[dir/b.md]]\n");
    CHECK(s.body("dir/b.md") == "b\n");
    CHECK(s.has_edge("a.md", "dir/b.md", core::EdgeKind::LinksTo));
    const auto again = app::connect_map_nodes(s.store, note("a.md"), note("dir/b.md"), MapEdgeKind::Link);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::AlreadyExists);
    CHECK(s.body("a.md") == "a metni\n[[dir/b.md]]\n");
}

TEST_CASE("map relations: a link keeps CRLF line ends") {
    Scene s;
    s.put("a.md", "bir\r\niki\r\n");
    s.put("b.md", "b\n");
    REQUIRE(app::connect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Link).has_value());
    CHECK(s.body("a.md") == "bir\r\niki\r\n[[b.md]]\r\n");
}

TEST_CASE("map relations: note to tag and tag to note add the tag to the note's text") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b #iş\n");
    CHECK(app::map_edit_note(note("a.md"), tag("proje"), MapEdgeKind::Tag) == np("a.md"));
    CHECK(app::map_edit_note(tag("proje"), note("b.md"), MapEdgeKind::Tag) == np("b.md"));

    const auto first = app::connect_map_nodes(s.store, note("a.md"), tag("proje"), MapEdgeKind::Tag);
    REQUIRE(first.has_value());
    CHECK(*first == "Tag #proje added to a.md");
    CHECK(s.body("a.md") == "a\n#proje\n");
    CHECK(s.links("a.md").tags == std::vector<std::string>{"proje"});

    const auto second = app::connect_map_nodes(s.store, tag("proje"), note("b.md"), MapEdgeKind::Tag);
    REQUIRE(second.has_value());
    CHECK(s.body("b.md") == "b #iş\n#proje\n");
    const auto again = app::connect_map_nodes(s.store, tag("proje"), note("b.md"), MapEdgeKind::Tag);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::AlreadyExists);
    CHECK(s.body("b.md") == "b #iş\n#proje\n");
}

TEST_CASE("map relations: tag to tag and a node onto itself change nothing") {
    Scene s;
    s.put("a.md", "a #proje\n");
    for (const auto& [from, to] :
         {std::pair(tag("proje"), tag("iş")), std::pair(note("a.md"), note("a.md"))}) {
        for (const MapEdgeKind kind : {MapEdgeKind::Related, MapEdgeKind::Link, MapEdgeKind::Tag}) {
            const auto done = app::connect_map_nodes(s.store, from, to, kind);
            REQUIRE_FALSE(done.has_value());
            CHECK(done.error().message_key == "map.invalid_pair");
        }
    }
    CHECK(s.body("a.md") == "a #proje\n");
    CHECK(s.links("a.md").related.empty());
}

TEST_CASE("map relations: removing a relation drops RELATED_TO in both directions") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b\n");
    REQUIRE(s.store.add_relation(np("a.md"), np("b.md")).has_value());
    CHECK(app::map_edge_removable(MapEdgeKind::Related));
    const auto done = app::disconnect_map_nodes(s.store, note("b.md"), note("a.md"), MapEdgeKind::Related);
    REQUIRE(done.has_value());
    CHECK(*done == "Relation removed: b.md and a.md");
    CHECK(s.links("a.md").related.empty());
    const auto again = app::disconnect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Related);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::NotFound);
}

TEST_CASE("map relations: removing a tag drops every # in prose, a tag-only line and keeps code") {
    Scene s;
    s.put("a.md", "başlık #Proje sonra\n#proje\n`#proje` kodda\n");
    CHECK(app::map_edge_removable(MapEdgeKind::Tag));
    const auto done = app::disconnect_map_nodes(s.store, tag("proje"), note("a.md"), MapEdgeKind::Tag);
    REQUIRE(done.has_value());
    CHECK(*done == "Tag #proje removed from a.md");
    CHECK(s.body("a.md") == "başlık Proje sonra\n`#proje` kodda\n");
    CHECK(s.links("a.md").tags.empty());
    const auto again = app::disconnect_map_nodes(s.store, note("a.md"), tag("proje"), MapEdgeKind::Tag);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::NotFound);
}

TEST_CASE("map relations: a link is not removed from the map, the text is not edited") {
    Scene s;
    s.put("a.md", "[[b]]\n");
    s.put("b.md", "b\n");
    CHECK_FALSE(app::map_edge_removable(MapEdgeKind::Link));
    const auto done = app::disconnect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Link);
    REQUIRE_FALSE(done.has_value());
    CHECK(done.error().message_key == "map.link_in_text");
    CHECK(s.body("a.md") == "[[b]]\n");
}

TEST_CASE(
    "map relations: a missing note, a code note and an open code block are errors, nothing is written") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("kod.py", "print(1)\n");
    s.put("açık.md", "```\nkod\n");
    const auto missing = app::connect_map_nodes(s.store, note("yok.md"), tag("proje"), MapEdgeKind::Tag);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == core::Error::Code::NotFound);
    const auto missing_target =
        app::connect_map_nodes(s.store, note("a.md"), note("yok.md"), MapEdgeKind::Related);
    REQUIRE_FALSE(missing_target.has_value());
    CHECK(missing_target.error().code == core::Error::Code::NotFound);
    const auto missing_link =
        app::connect_map_nodes(s.store, note("yok.md"), note("a.md"), MapEdgeKind::Link);
    REQUIRE_FALSE(missing_link.has_value());
    CHECK(missing_link.error().code == core::Error::Code::NotFound);

    for (const MapEdgeKind kind : {MapEdgeKind::Link, MapEdgeKind::Tag}) {
        const MapNode other = kind == MapEdgeKind::Link ? note("a.md") : tag("proje");
        const auto code = app::connect_map_nodes(s.store, note("kod.py"), other, kind);
        REQUIRE_FALSE(code.has_value());
        CHECK(code.error().message_key == "map.not_prose");
    }
    CHECK(s.body("kod.py") == "print(1)\n");
    const auto fenced = app::connect_map_nodes(s.store, note("açık.md"), tag("proje"), MapEdgeKind::Tag);
    REQUIRE_FALSE(fenced.has_value());
    CHECK(fenced.error().message_key == "map.tag_not_added");
    CHECK(s.body("açık.md") == "```\nkod\n");
    CHECK(s.body("a.md") == "a\n");
}

// Review bulgusu: yazılacak [[...]] geri okunduğunda tam olarak hedefe giden tek yeni bağlantı olmalı.
TEST_CASE("map relations: a link is refused unless it reads back as exactly one new link to the target") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("plan.md", "plan\n");
    s.put("x]] [[plan.md", "senkronla gelen ad\n");  // iki sahte bağlantı kurardı: [[x]] ve [[plan.md]]
    s.put("b]", "köşeli ad\n");                      // [[b]]] başka nota (b.md) çözülürdü
    s.put("b.md", "b\n");
    s.put("g#h.md", "başlıklı ad\n");        // [[g#h.md]] `g`ye çözülür
    s.put("açık.md", "bak [[kapanmamış\n");  // önceki [[ bizim bağlantımızı yutardı
    s.put("kod.md", "kod `[[x` içinde\n");
    for (const char* target : {"x]] [[plan.md", "b]", "g#h.md"}) {
        CAPTURE(target);
        const auto done = app::connect_map_nodes(s.store, note("a.md"), note(target), MapEdgeKind::Link);
        REQUIRE_FALSE(done.has_value());
        CHECK(done.error().message_key == "map.link_not_added");
    }
    CHECK(s.body("a.md") == "a\n");
    CHECK(s.links("a.md").outgoing.empty());
    for (const char* source : {"açık.md", "kod.md"}) {
        CAPTURE(source);
        const std::string before = s.body(source);
        for (int attempt = 0; attempt < 2; ++attempt) {  // yeniden denemek satır biriktirmez
            const auto done =
                app::connect_map_nodes(s.store, note(source), note("plan.md"), MapEdgeKind::Link);
            REQUIRE_FALSE(done.has_value());
            CHECK(done.error().message_key == "map.link_not_added");
        }
        CHECK(s.body(source) == before);
    }
}

TEST_CASE("map relations: a tag that would not read back as itself is refused") {
    Scene s;
    s.put("a.md", "a\n");
    for (const char* key : {"x\nnew", "a b"}) {
        CAPTURE(key);
        const auto done = app::connect_map_nodes(s.store, note("a.md"), tag(key), MapEdgeKind::Tag);
        REQUIRE_FALSE(done.has_value());
        CHECK(done.error().message_key == "map.tag_not_added");
    }
    CHECK(s.body("a.md") == "a\n");
}

TEST_CASE("map relations: the edge under the pointer is the nearest one within reach") {
    app::NoteMap map;
    map.nodes = {note("a.md"), note("b.md"), note("c.md")};
    map.edges = {{0, 1, MapEdgeKind::Link}, {1, 2, MapEdgeKind::Related}};
    const std::vector<app::LayoutPoint> positions{{0.0F, 0.0F}, {100.0F, 0.0F}, {100.0F, 100.0F}};
    CHECK(app::map_edge_at(map, positions, {50.0F, 3.0F}, 5.0F) == 0U);
    CHECK(app::map_edge_at(map, positions, {98.0F, 60.0F}, 5.0F) == 1U);
    CHECK_FALSE(app::map_edge_at(map, positions, {50.0F, 50.0F}, 5.0F));
    CHECK_FALSE(app::map_edge_at(map, positions, {-20.0F, 0.0F}, 5.0F));  // ucun ötesi parçaya sayılmaz
}

TEST_CASE("map relations: the edge between two nodes prefers the removable kind, either direction") {
    app::NoteMap map;
    map.nodes = {note("a.md"), note("b.md"), tag("proje"), note("c.md")};
    map.edges = {{0, 1, MapEdgeKind::Link}, {1, 0, MapEdgeKind::Related}, {0, 2, MapEdgeKind::Tag}};
    CHECK(app::map_edge_between(map, note("b.md"), note("a.md")) == MapEdgeKind::Related);
    CHECK(app::map_edge_between(map, tag("proje"), note("a.md")) == MapEdgeKind::Tag);
    CHECK_FALSE(app::map_edge_between(map, note("a.md"), note("c.md")));
    map.edges.erase(map.edges.begin() + 1);
    CHECK(app::map_edge_between(map, note("a.md"), note("b.md")) == MapEdgeKind::Link);
}

// Okumayla yazma arasında başka bağlantı (senkron, CLI) notu değiştirirse harita onu ezmez: okunan revizyonla
// yazılır, ModifiedExternally döner, durum satırı nedeni söyler.
TEST_CASE("map relations: a note changed between read and write is not overwritten") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b\n");
    s.put("c.md", "c #proje\n");
    int writes = 0;
    const auto change = [&](const std::string& name) {
        return [&s, &writes, name] {
            core::NoteStore other(s.dir.path());
            const auto content = other.read(np(name)).value();
            REQUIRE(
                other.write(np(name), "başka cihaz " + std::to_string(++writes) + "\n", content.revision));
        };
    };
    const auto refused = [](const std::expected<std::string, core::Error>& done) {
        REQUIRE_FALSE(done.has_value());
        CHECK(done.error().code == core::Error::Code::ModifiedExternally);
        CHECK(core::format_error(done.error()).starts_with("Modified outside the app"));
    };
    refused(app::connect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Link, change("a.md")));
    CHECK(s.body("a.md") == "başka cihaz 1\n");
    refused(app::connect_map_nodes(s.store, note("a.md"), tag("iş"), MapEdgeKind::Tag, change("a.md")));
    CHECK(s.body("a.md") == "başka cihaz 2\n");
    refused(app::disconnect_map_nodes(s.store, note("c.md"), tag("proje"), MapEdgeKind::Tag, change("c.md")));
    CHECK(s.body("c.md") == "başka cihaz 3\n");
}
