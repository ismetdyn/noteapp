// MAP-R: haritada ilişki kurma ve kaldırma (app::map_relations). GUI ve TUI aynı kuralı çağırır; burada
// gerçek depoyla: ilişki add/remove_relation, bağlantı ve etiket notun metnine yazılır.
#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    // Satırlar kimliksiz (karşılaştırma yol, ad, yönle).
    std::vector<core::RelatedNote> related(const std::string& name) const {
        std::vector<core::RelatedNote> rows = links(name).related;
        for (core::RelatedNote& row : rows)
            row.id = {};
        return rows;
    }
    bool has_edge(const std::string& from, const std::string& to, core::EdgeKind kind) const {
        const auto edges = store.edges().value();
        return std::ranges::any_of(edges, [&](const core::NoteEdge& edge) {
            return edge.from == np(from) && edge.to == np(to) && edge.kind == kind;
        });
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
    CHECK(s.related("a.md") == std::vector{core::RelatedNote{np("b.md")}});
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

TEST_CASE("map relations: removing a relation drops only that RELATED_TO edge, from either end") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b\n");
    const auto unnamed = s.store.add_relation(np("a.md"), np("b.md"));
    REQUIRE(unnamed.has_value());
    REQUIRE(s.store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
    CHECK(app::map_edge_removable(MapEdgeKind::Related));
    const auto done =
        app::disconnect_map_nodes(s.store, note("b.md"), note("a.md"), MapEdgeKind::Related, *unnamed);
    REQUIRE(done.has_value());
    CHECK(*done == "Relation removed: b.md and a.md");
    CHECK(s.related("a.md") == std::vector{core::RelatedNote{np("b.md"), "cites"}});
    const auto again =
        app::disconnect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Related, *unnamed);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::NotFound);
    // Kimliksiz ilişki kaldırma hiçbir şey silmez.
    CHECK(app::disconnect_map_nodes(s.store, note("a.md"), note("b.md"), MapEdgeKind::Related).error().code ==
          core::Error::Code::NotFound);
    CHECK(s.related("a.md").size() == 1);
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
    const auto kind = [&](const MapNode& first, const MapNode& second) {
        const app::MapEdge* edge = app::map_edge_between(map, first, second);
        return edge != nullptr ? std::optional(edge->kind) : std::nullopt;
    };
    CHECK(kind(note("b.md"), note("a.md")) == MapEdgeKind::Related);
    CHECK(app::map_edge_between(map, note("b.md"), note("a.md")) == &map.edges[1]);  // kimliği bu kenarın
    CHECK(kind(tag("proje"), note("a.md")) == MapEdgeKind::Tag);
    CHECK_FALSE(kind(note("a.md"), note("c.md")));
    map.edges.erase(map.edges.begin() + 1);
    CHECK(kind(note("a.md"), note("b.md")) == MapEdgeKind::Link);
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
    refused(
        app::disconnect_map_nodes(s.store, note("c.md"), tag("proje"), MapEdgeKind::Tag, {}, change("c.md")));
    CHECK(s.body("c.md") == "başka cihaz 3\n");
}

// F1: adlı ilişki. GUI "Bağla…" penceresi ve TUI ad satırı aynı çağrıyı ve aynı seçici süzgecini kullanır.
TEST_CASE("map relations: a named directed relation is added, named in the message and removed by name") {
    Scene s;
    s.put("a.md", "a\n");
    s.put("b.md", "b\n");
    const auto done = app::relate_map_notes(s.store, note("a.md"), note("b.md"), {" uses ", true});
    REQUIRE(done.has_value());
    CHECK(*done == "Related as 'uses': a.md and b.md");
    REQUIRE(app::relate_map_notes(s.store, note("b.md"), note("a.md"), {}).has_value());
    CHECK(s.related("a.md") ==
          std::vector{core::RelatedNote{np("b.md")},
                      core::RelatedNote{np("b.md"), "uses", core::RelationDirection::Outgoing}});
    const auto again = app::relate_map_notes(s.store, note("b.md"), note("a.md"), {"USES", false});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == core::Error::Code::AlreadyExists);
    CHECK(app::relate_map_notes(s.store, note("a.md"), tag("proje"), {"x", false}).error().code ==
          core::Error::Code::InvalidPath);

    const auto rows = s.links("b.md").related;
    const auto uses = std::ranges::find(rows, std::string("uses"), &core::RelatedNote::name);
    REQUIRE(uses != rows.end());
    const auto removed = app::unrelate_map_notes(s.store, note("b.md"), note("a.md"), uses->id);
    REQUIRE(removed.has_value());
    CHECK(*removed == "Relation 'uses' removed: b.md and a.md");
    CHECK(s.related("a.md") == std::vector{core::RelatedNote{np("b.md")}});
    // Kimlik uçlara aittir: başka çiftin düğümleriyle verilen kimlik silinmez.
    s.put("c.md", "c\n");
    const auto other =
        app::unrelate_map_notes(s.store, note("a.md"), note("c.md"), s.links("a.md").related[0].id);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == core::Error::Code::NotFound);
    CHECK(s.related("a.md").size() == 1);
}

TEST_CASE("map relations: the picker filters folded names, prefix matches first, and knows an exact name") {
    const std::vector<core::RelationName> names{{"kullanır", "Kullanır", 5, 4},
                                                {"ilgi", "İlgi", 3, 0},
                                                {"alt ilgi", "alt ilgi", 2, 2},
                                                {"cites", "cites", 1, 0}};
    const auto keys = [](const app::RelationMatches& matches) {
        std::vector<std::string> out;
        for (const core::RelationName& name : matches.names)
            out.push_back(name.key);
        return out;
    };
    CHECK(keys(app::match_relation_names(names, "")) ==
          std::vector<std::string>{"kullanır", "ilgi", "alt ilgi", "cites"});
    const auto ilgi = app::match_relation_names(names, " İLGİ ");
    CHECK(keys(ilgi) == std::vector<std::string>{"ilgi", "alt ilgi"});
    CHECK(ilgi.exact);
    const auto partial = app::match_relation_names(names, "IL");
    CHECK(keys(partial) == std::vector<std::string>{"ilgi", "alt ilgi"});
    CHECK_FALSE(partial.exact);
    CHECK(app::match_relation_names(names, "yok").names.empty());
    CHECK(app::match_relation_names(names, "a\nb").names.empty());

    // "Yönlü" varsayılanı: adın ilişkilerinin çoğu yönlüyse; yeni ad yönsüz.
    CHECK(app::default_directed(names, " KULLANıR "));  // I -> i, ı kalır (tag_key)
    CHECK_FALSE(app::default_directed(names, "ilgi"));
    CHECK_FALSE(app::default_directed(names, "yeni ad"));
}

TEST_CASE("map relations: rename and direction target one relation by id and say what changed") {
    // Not kimlikleri UUIDv7: önde oluşturma milisaniyesi. Yönsüz kenar uçlarını kimlik sırasıyla saklar
    // (relation_ends), yön değişiminde kenarın korunması ya da yeniden kurulması bu sıraya bağlıdır: iki sıra
    // da denenir, ayrı milisaniyede oluşan notlarla sıra belirlenir.
    for (const bool b_first : {false, true}) {
        CAPTURE(b_first);
        Scene s;
        const std::string first = b_first ? "b.md" : "a.md";
        const std::string second = b_first ? "a.md" : "b.md";
        s.put(first, first.substr(0, 1) + "\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        s.put(second, second.substr(0, 1) + "\n");
        REQUIRE(app::relate_map_notes(s.store, note("a.md"), note("b.md"), {"uses", false}).has_value());
        const core::RelationId id = s.links("a.md").related.front().id;

        const auto renamed = app::rename_map_relation(s.store, np("a.md"), np("b.md"), id, "  cites ");
        REQUIRE(renamed.has_value());
        CHECK(*renamed == "Relation renamed to 'cites': a.md and b.md");
        CHECK(s.related("a.md") == std::vector{core::RelatedNote{np("b.md"), "cites"}});
        const auto unnamed = app::rename_map_relation(s.store, np("a.md"), np("b.md"), id, " ");
        REQUIRE(unnamed.has_value());
        CHECK(*unnamed == "Relation name removed: a.md and b.md");
        CHECK(app::rename_map_relation(s.store, np("a.md"), np("b.md"), id, "a\nb").error().code ==
              core::Error::Code::InvalidPath);

        // Kaynak değişince kenar yeniden kurulabilir, kimliği değişebilir: satır yeniden okunur.
        const auto directed = app::direct_map_relation(s.store, np("a.md"), np("b.md"), id, np("b.md"));
        REQUIRE(directed.has_value());
        CHECK(*directed == "Relation directed: b.md → a.md");
        CHECK(s.related("a.md") ==
              std::vector{core::RelatedNote{np("b.md"), "", core::RelationDirection::Incoming}});
        const core::RelationId now = s.links("a.md").related.front().id;
        const auto undirected = app::direct_map_relation(s.store, np("a.md"), np("b.md"), now, std::nullopt);
        REQUIRE(undirected.has_value());
        CHECK(*undirected == "Relation made undirected: a.md and b.md");
        CHECK(s.related("a.md") == std::vector{core::RelatedNote{np("b.md")}});

        // Yönsüz↔yönlü geçişte kimlik sıraya göre kalabilir; ters yönlü iki kenarın uçları ise her sırada
        // farklıdır: çevrilen yönlü ilişkinin eski kimliği her zaman bayatlar.
        const core::RelationId plain = s.links("a.md").related.front().id;
        REQUIRE(app::direct_map_relation(s.store, np("a.md"), np("b.md"), plain, np("a.md")).has_value());
        CHECK(s.related("a.md") ==
              std::vector{core::RelatedNote{np("b.md"), "", core::RelationDirection::Outgoing}});
        const core::RelationId outgoing = s.links("a.md").related.front().id;
        REQUIRE(app::direct_map_relation(s.store, np("a.md"), np("b.md"), outgoing, np("b.md")).has_value());
        const auto stale = app::direct_map_relation(s.store, np("a.md"), np("b.md"), outgoing, np("a.md"));
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().code == core::Error::Code::NotFound);
    }
}

TEST_CASE("map relations: a flip points the relation the other way from the shown note") {
    using core::RelationDirection;
    CHECK(app::flipped_source(np("a.md"), np("b.md"), RelationDirection::Outgoing) == np("b.md"));
    CHECK(app::flipped_source(np("a.md"), np("b.md"), RelationDirection::Incoming) == np("a.md"));
    CHECK(app::flipped_source(np("a.md"), np("b.md"), RelationDirection::Undirected) == np("a.md"));
}

TEST_CASE("map relations: a related row reads arrow, path and name") {
    using core::RelationDirection;
    CHECK(app::related_row_text({np("b.md")}) == "b.md");
    CHECK(app::related_row_text({np("b.md"), "uses"}) == "b.md — uses");
    CHECK(app::related_row_text({np("b.md"), "uses", RelationDirection::Outgoing}) == "↗ b.md — uses");
    CHECK(app::related_row_text({np("b.md"), "", RelationDirection::Incoming}) == "↙ b.md");
}
