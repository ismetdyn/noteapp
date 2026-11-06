// F4: özellik denetçisinin ortak modeli (app::Inspector). GUI paneli ve TUI bölümü bunu çizer: satır sırası,
// salt okunur anahtarlar, türlü değerler, etiket (label = etiket), güvenilmez metnin gösterimi, iletiler;
// ayrıca `prop:` aramasının app::Listing yolu.
#include "app/inspector.hpp"
#include "app/listing.hpp"
#include "core/i18n.hpp"
#include "core/note_props.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace app = noteapp::app;
namespace core = noteapp::core;
namespace graph = noteapp::graph;
using app::InspectorRow;
using app::InspectTarget;

namespace {

core::NotePath np(const std::string& text) {
    return *core::NotePath::parse(text);
}

struct Scene {
    noteapp::test::TempDir dir;
    core::NoteStore store{dir.path()};
    app::Inspector inspector;

    Scene() {
        core::init_i18n("en");
        REQUIRE(noteapp::test::seed_note(store, "a.md", "a #proje\n"));
        REQUIRE(noteapp::test::seed_note(store, "b.md", "b\n"));
    }
    void open_note(const std::string& path) {
        inspector.inspect(InspectTarget::of_note(np(path)));
        inspector.reload(store);
        REQUIRE(inspector.error().empty());
    }
    const InspectorRow* row(std::string_view key) const {
        const auto& rows = inspector.rows();
        const auto found = std::ranges::find(rows, key, &InspectorRow::key);
        return found == rows.end() ? nullptr : &*found;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        for (const InspectorRow& item : inspector.rows())
            out.push_back(item.key);
        return out;
    }
};

}  // namespace

TEST_CASE("inspector: editable properties first, then labels, then the app's read-only keys") {
    Scene scene;
    REQUIRE(core::set_note_property(scene.store, np("a.md"), "zeta", graph::Value{true}).has_value());
    REQUIRE(
        core::set_note_property(scene.store, np("a.md"), "alpha", graph::Value{std::int64_t{1}}).has_value());
    scene.open_note("a.md");
    const std::vector<std::string> keys = scene.keys();
    REQUIRE(keys.size() >= 4);
    CHECK(keys[0] == "alpha");
    CHECK(keys[1] == "zeta");
    CHECK(keys[2] == "proje");
    CHECK(scene.row("proje")->kind == InspectorRow::Kind::Label);
    CHECK(scene.row("proje")->key_text == "#proje");
    CHECK(scene.row("alpha")->type == "number");
    CHECK(scene.row("alpha")->input == "1");
    CHECK(scene.row("zeta")->value_text == "true");
    for (const char* key : {"name", "type", "created_at", "updated_at", "body"}) {
        INFO(key);
        REQUIRE(scene.row(key) != nullptr);
        CHECK(scene.row(key)->read_only);
        CHECK(scene.row(key)->input.empty());
    }
    CHECK(scene.row("created_at")->type == "time");
    CHECK_FALSE(scene.row("alpha")->read_only);
}

TEST_CASE("inspector: set and remove write one property, reload and report") {
    Scene scene;
    scene.open_note("b.md");
    const auto added = scene.inspector.set(scene.store, "status", "42");
    REQUIRE(added.has_value());
    CHECK(*added == core::tr("inspector.set", {{"key", "status"}, {"value", "42"}}));
    REQUIRE(scene.row("status") != nullptr);
    CHECK(scene.row("status")->type == "number");

    // Önek metni korur, tür seçimi önekten önce gelir.
    REQUIRE(scene.inspector.set(scene.store, "status", "text:42").has_value());
    CHECK(scene.row("status")->type == "text");
    CHECK(scene.row("status")->input == "text:42");
    REQUIRE(scene.inspector.set(scene.store, "done", "true", core::PropertyType::Text).has_value());
    CHECK(scene.row("done")->type == "text");
    const auto bad_number = scene.inspector.set(scene.store, "n", "x", core::PropertyType::Number);
    REQUIRE_FALSE(bad_number.has_value());
    CHECK(bad_number.error().message_key == "error.property_value");
    CHECK(scene.row("n") == nullptr);

    const auto reserved = scene.inspector.set(scene.store, "type", "txt");
    REQUIRE_FALSE(reserved.has_value());
    CHECK(reserved.error().message_key == "error.property_reserved");
    CHECK(core::format_error(reserved.error()).find("type") != std::string::npos);

    const auto removed = scene.inspector.remove(scene.store, "status");
    REQUIRE(removed.has_value());
    CHECK(*removed == core::tr("inspector.removed", {{"key", "status"}}));
    CHECK(scene.row("status") == nullptr);
    CHECK(scene.inspector.remove(scene.store, "status").error().code == core::Error::Code::NotFound);
}

TEST_CASE("inspector: labels are the note's tags, added to and removed from its text") {
    Scene scene;
    scene.open_note("b.md");
    REQUIRE(scene.inspector.add_label(scene.store, "#Plan").has_value());
    CHECK(scene.store.read(np("b.md"))->text == "b\n#plan\n");
    REQUIRE(scene.row("plan") != nullptr);
    CHECK(scene.row("plan")->kind == InspectorRow::Kind::Label);
    CHECK(scene.inspector.add_label(scene.store, "plan").error().code == core::Error::Code::AlreadyExists);
    CHECK_FALSE(scene.inspector.add_label(scene.store, "a b").has_value());  // etiket değil: yazılmaz
    CHECK(scene.store.read(np("b.md"))->text == "b\n#plan\n");
    CHECK(scene.inspector.confirm_remove_text(*scene.row("plan")) ==
          core::tr("inspector.confirm_remove_label", {{"tag", "plan"}, {"path", "b.md"}}));
    REQUIRE(scene.inspector.remove_label(scene.store, "plan").has_value());
    CHECK(scene.row("plan") == nullptr);
    CHECK(scene.store.read(np("b.md"))->text == "b\n");
}

TEST_CASE("inspector: a relation shows its own properties; name and direction stay read-only") {
    Scene scene;
    REQUIRE(scene.store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    const core::RelationId id = scene.store.links(np("a.md"))->related.front().id;
    scene.inspector.inspect(InspectTarget::of_relation(id, np("a.md"), np("b.md"), "uses", true));
    scene.inspector.reload(scene.store);
    CHECK(scene.inspector.title() == "a.md → b.md 'uses'");
    REQUIRE(scene.row("name") != nullptr);
    CHECK(scene.row("name")->read_only);
    CHECK(scene.row("directed")->read_only);
    REQUIRE(scene.inspector.set(scene.store, "since", "2024").has_value());
    CHECK(scene.row("since")->value_text == "2024");
    CHECK(scene.inspector.set(scene.store, "name", "x").error().message_key ==
          "error.property_relation_reserved");
    CHECK(scene.inspector.add_label(scene.store, "x").error().message_key == "inspector.labels_on_notes");
    // Kenar kalkınca yeniden okuma hatayı söyler, satır kalmaz.
    REQUIRE(scene.store.remove_relation(id).has_value());
    scene.inspector.reload(scene.store);
    CHECK(scene.inspector.rows().empty());
    CHECK_FALSE(scene.inspector.error().empty());
}

TEST_CASE("inspector: untrusted keys and values are shown sanitised and cut, written back raw") {
    Scene scene;
    const std::string key = "k\x1b[31m";
    const std::string value = "v\x07" + std::string(10, 'x');
    REQUIRE(core::set_note_property(scene.store, np("b.md"), key, graph::Value{value}).has_value());
    scene.inspector.set_cell_bytes(6);
    scene.open_note("b.md");
    const InspectorRow* row = scene.row(key);
    REQUIRE(row != nullptr);
    CHECK(row->key_text.find('\x1b') == std::string::npos);
    CHECK(row->value_text.find('\x07') == std::string::npos);
    CHECK(row->value_text.ends_with("…"));
    CHECK(row->input == value);  // düzenleme ham değerle başlar
    REQUIRE(scene.inspector.remove(scene.store, key).has_value());
}

// İnceleme: yeniden okuma seçimi yalnız indeksle kısıyordu; senkron seçili satırın önüne anahtar ekleyince
// düzenleme ve silme başka anahtara giderdi. Seçim anahtarıyla korunur, anahtar gidince en yakın satıra iner.
TEST_CASE("inspector: a reload keeps the selected key even when a sync adds a key before it") {
    Scene scene;
    REQUIRE(core::set_note_property(scene.store, np("b.md"), "b", graph::Value{true}).has_value());
    REQUIRE(core::set_note_property(scene.store, np("b.md"), "c", graph::Value{true}).has_value());
    scene.open_note("b.md");
    scene.inspector.select(0);
    REQUIRE(scene.inspector.selected_row()->key == "b");
    {
        core::NoteStore other(scene.dir.path());  // başka cihazın senkronu gibi ayrı bağlantı
        REQUIRE(core::set_note_property(other, np("b.md"), "a0", graph::Value{true}).has_value());
    }
    scene.inspector.reload(scene.store);
    REQUIRE(scene.inspector.selected_row() != nullptr);
    CHECK(scene.inspector.selected_row()->key == "b");

    // Seçili anahtar kalkınca seçim aynı sıradaki satıra iner (sınırın içinde).
    {
        core::NoteStore other(scene.dir.path());
        REQUIRE(core::set_note_property(other, np("b.md"), "b", graph::Value{}).has_value());
    }
    scene.inspector.reload(scene.store);
    REQUIRE(scene.inspector.selected_row() != nullptr);
    CHECK(scene.inspector.selected_row()->key == "c");
}

TEST_CASE("inspector: selection follows the rows and stays inside them") {
    Scene scene;
    scene.open_note("b.md");
    const std::size_t count = scene.inspector.rows().size();
    REQUIRE(count > 2);
    scene.inspector.move(1);
    CHECK(scene.inspector.selected() == 1);
    scene.inspector.move(100);
    CHECK(scene.inspector.selected() == count - 1);
    scene.inspector.move(-100);
    CHECK(scene.inspector.selected() == 0);
    scene.inspector.inspect(InspectTarget::of_note(np("a.md")));
    CHECK(scene.inspector.rows().empty());
    CHECK(scene.inspector.selected() == 0);
}

TEST_CASE("listing: prop:key and prop:key=value filter notes by their properties") {
    Scene scene;
    REQUIRE(noteapp::test::seed_note(scene.store, "c.md", "c\n"));
    REQUIRE(core::set_note_property(scene.store, np("a.md"), "status", graph::Value{std::string("Done")})
                .has_value());
    REQUIRE(core::set_note_property(scene.store, np("b.md"), "status", graph::Value{std::string("open")})
                .has_value());
    REQUIRE(core::set_note_property(scene.store, np("c.md"), "priority", graph::Value{std::int64_t{2}})
                .has_value());

    app::Listing listing;
    listing.set_notes(scene.store.list());
    const auto paths = [&](std::string_view query) {
        const app::SearchResult result = listing.search(query, app::SearchMode::Name);
        REQUIRE_FALSE(result.error.has_value());
        std::vector<std::string> out;
        for (const app::ListRow& row : result.rows)
            out.push_back(row.path.string());
        std::ranges::sort(out);
        return out;
    };
    CHECK(paths("prop:status").empty());  // kaynak bağlanmadı: süzgeç not geçirmez
    listing.set_property_source(
        [&](std::string_view key) { return app::search_properties(scene.store, key); });
    CHECK(paths("prop:status") == std::vector<std::string>{"a.md", "b.md"});
    CHECK(paths("prop:status=done") == std::vector<std::string>{"a.md"});
    CHECK(paths("prop:\"status=open\"") == std::vector<std::string>{"b.md"});
    CHECK(paths("prop:priority=2") == std::vector<std::string>{"c.md"});
    CHECK(paths("prop:Status").empty());     // anahtar tam
    CHECK(paths("prop:status=do").empty());  // değer tam (katlanmış)
    CHECK(paths("prop:status a") == std::vector<std::string>{"a.md"});
    const app::SearchResult bad = listing.search("prop:=x", app::SearchMode::Name);
    REQUIRE(bad.error.has_value());
    CHECK(core::format_error(bad.error->error).find("prop") != std::string::npos);
}
