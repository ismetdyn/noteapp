// F4: GUI'nin özellik denetçisi (SPEC-gui "Özellik denetçisi"). Model ortak (app::Inspector); burada panel,
// ekleme satırı, satır içi düzenleme, onaylı silme, etiket, ilişki konusu ve güvenilmez metnin çizimi gerçek
// View ve ImGui olaylarıyla.
#include "core/i18n.hpp"
#include "core/note_props.hpp"
#include "graph/graph.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;
namespace graph = noteapp::graph;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

struct Scene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;
    std::vector<std::string> asked;
    bool answer = false;

    Scene() {
        fixture.add_note("a.md", "a metni\n", 20);
        fixture.add_note("b.md", "b\n", 10);
    }
    void open(const std::string& note = "a.md") {
        state = fixture.make_state();
        state->deciders.confirm_delete = [this](const std::string& question, const std::string&) {
            asked.push_back(question);
            return answer;
        };
        view = make_view(*state);
        state->open_note(np(note));
        settle(*view);
    }
    std::expected<void, core::Error> set_prop(const core::NotePath& path, std::string_view key,
                                              const graph::Value& value) const {
        core::NoteStore store(fixture.store_dir());
        return core::set_note_property(store, path, key, value);
    }
    // Önceki işlemin bildirimi panelin üstünde durur: tıklama düğmeye değil ona giderdi (links_test ile
    // aynı).
    void clear_notices() {
        std::vector<std::uint64_t> shown;
        for (const auto& notice : state->notices().items())
            shown.push_back(notice.id);
        for (const std::uint64_t id : shown)
            state->dismiss_notice(id);
        settle(*view);
    }
    bool has(const std::string& item) const { return view->item_rects().contains(item); }
    void open_panel() {
        REQUIRE(has("inspector_header"));
        if (!state->inspector_open())
            click_button(*view, "inspector_header");
        settle(*view);
        REQUIRE(state->inspector_open());
    }
    void type_into(const std::string& item, const std::string& text) {
        click_button(*view, item);
        gui_test::type_text(*view, text.c_str());
    }
    graph::Value stored(std::string_view key) const {
        const auto props = core::note_properties(core::NoteStore(fixture.store_dir()), np("a.md"));
        REQUIRE(props.has_value());
        for (const core::Property& property : *props)
            if (property.key == key)
                return property.value;
        return graph::Value{};
    }
};

}  // namespace

TEST_CASE("inspector panel: the header toggles it; the open note's read-only keys have no edit or delete") {
    Scene scene;
    scene.open();
    CHECK_FALSE(scene.state->inspector_open());
    scene.open_panel();
    CHECK(scene.has("inspector_key:body"));  // ilk salt okunur satır (alttakiler kırpılır: ListClipper)
    CHECK_FALSE(scene.has("inspector_edit:body"));  // görünen salt okunur satırın düğmesi yok
    CHECK_FALSE(scene.has("inspector_delete:body"));
    CHECK_FALSE(scene.has("inspector_edit:type"));
    CHECK_FALSE(scene.has("inspector_delete:type"));
    CHECK_FALSE(scene.has("inspector_delete:name"));
    click_button(*scene.view, "inspector_header");
    CHECK_FALSE(scene.state->inspector_open());
}

TEST_CASE("inspector panel: the add row writes a typed property; Enter in the value field adds too") {
    Scene scene;
    scene.open();
    scene.open_panel();
    scene.type_into("inspector_new_key", "status");
    scene.type_into("inspector_new_value", "draft");
    click_button(*scene.view, "inspector_add");
    settle(*scene.view);
    CHECK(scene.stored("status") == graph::Value{std::string("draft")});
    CHECK(scene.has("inspector_edit:status"));
    CHECK(scene.state->notices().items().back().kind == noteapp::app::NoticeKind::Success);
    scene.clear_notices();

    scene.type_into("inspector_new_key", "priority");
    scene.type_into("inspector_new_value", "3");
    press(*scene.view, ImGuiKey_Enter);
    settle(*scene.view);
    CHECK(scene.stored("priority") == graph::Value{std::int64_t{3}});
    scene.clear_notices();

    // Uygulamanın anahtarı reddedilir: hata bildirimde, satır temizlenmez.
    scene.type_into("inspector_new_key", "type");
    scene.type_into("inspector_new_value", "txt");
    click_button(*scene.view, "inspector_add");
    settle(*scene.view);
    CHECK(scene.state->notices().items().back().kind == noteapp::app::NoticeKind::Error);
    CHECK(scene.stored("type") == graph::Value{std::string("md")});
}

TEST_CASE("inspector panel: Edit opens the value in place with its round-trip text; Enter saves it") {
    Scene scene;
    REQUIRE(scene.set_prop(np("a.md"), "code", graph::Value{std::string("42")}).has_value());
    scene.open();
    scene.open_panel();
    click_button(*scene.view, "inspector_edit:code");
    settle(*scene.view);
    REQUIRE(scene.has("inspector_edit_value"));
    // Metin "42" sayı okunurdu: düzenleme `text:` önekiyle açılır, tür korunur.
    press(*scene.view, ImGuiKey_End);
    gui_test::type_text(*scene.view, "7");
    press(*scene.view, ImGuiKey_Enter);
    settle(*scene.view);
    CHECK(scene.stored("code") == graph::Value{std::string("427")});
    CHECK_FALSE(scene.has("inspector_edit_value"));
}

TEST_CASE("inspector panel: Delete asks first; No keeps the property, Yes removes it") {
    Scene scene;
    REQUIRE(scene.set_prop(np("a.md"), "status", graph::Value{std::string("draft")}).has_value());
    scene.open();
    scene.open_panel();
    click_button(*scene.view, "inspector_delete:status");
    REQUIRE(scene.asked.size() == 1);
    CHECK(scene.asked[0] == core::tr("inspector.confirm_remove", {{"key", "status"}, {"value", "draft"}}));
    CHECK(scene.stored("status") == graph::Value{std::string("draft")});
    scene.clear_notices();
    scene.answer = true;
    click_button(*scene.view, "inspector_delete:status");
    settle(*scene.view);
    CHECK(std::holds_alternative<std::monostate>(scene.stored("status")));
    CHECK_FALSE(scene.has("inspector_key:status"));
}

TEST_CASE("inspector panel: a #key adds a label to the note's text and Delete takes it out") {
    Scene scene;
    scene.open();
    scene.open_panel();
    scene.type_into("inspector_new_key", "#plan");
    click_button(*scene.view, "inspector_add");
    settle(*scene.view);
    CHECK(scene.fixture.read_note("a.md") == "a metni\n#plan\n");
    REQUIRE(scene.has("inspector_delete:plan"));
    scene.clear_notices();
    scene.answer = true;
    click_button(*scene.view, "inspector_delete:plan");
    settle(*scene.view);
    CHECK(scene.asked.size() == 1);
    CHECK(scene.fixture.read_note("a.md") == "a metni\n");
}

TEST_CASE("inspector panel: a relation is inspected from its menu; name stays read-only; the note returns") {
    Scene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    }
    scene.open();
    const auto& related = scene.state->links().related;
    REQUIRE(related.size() == 1);
    scene.state->inspect_relation(np("a.md"), np("b.md"), related.front().id, "uses", true);
    settle(*scene.view);
    REQUIRE(scene.has("inspector_show_note"));
    CHECK(scene.has("inspector_key:name"));
    CHECK_FALSE(scene.has("inspector_delete:name"));
    scene.type_into("inspector_new_key", "weight");
    scene.type_into("inspector_new_value", "2");
    click_button(*scene.view, "inspector_add");
    settle(*scene.view);
    scene.clear_notices();
    const auto props =
        core::relation_properties(core::NoteStore(scene.fixture.store_dir()), related.front().id);
    REQUIRE(props.has_value());
    CHECK(std::ranges::any_of(*props, [](const core::Property& property) {
        return property.key == "weight" && property.value == graph::Value{std::int64_t{2}};
    }));
    click_button(*scene.view, "inspector_show_note");
    settle(*scene.view);
    CHECK_FALSE(scene.has("inspector_show_note"));
    CHECK(scene.has("inspector_key:body"));
}

// İnceleme (Sev-3): binlerce özellikli not (senkronla gelebilir) her karede her satırı çiziyordu. Satırlar
// ImGuiListClipper'la çizilir: yalnız görünenler (bağlantı paneli kalıbı).
TEST_CASE("inspector panel: a note with thousands of properties draws only the visible rows") {
    constexpr int kProperties = 3000;
    Scene scene;
    {
        auto graph = graph::Graph::open(scene.fixture.store_dir() / "graph.db");
        REQUIRE(graph.has_value());
        auto txn = graph->write();
        REQUIRE(txn.has_value());
        std::optional<graph::NodeId> note;
        {  // imleç işlemden önce kapanır (graph.hpp ömür sırası)
            auto notes = txn->nodes_with_label("Note");
            REQUIRE(notes.has_value());
            while (true) {
                const auto next = notes->next();
                REQUIRE(next.has_value());
                if (!*next)
                    break;
                if (txn->property(**next, "name").value() == graph::Value{std::string("a")})
                    note = **next;
            }
        }
        REQUIRE(note.has_value());
        for (int i = 0; i < kProperties; ++i)
            REQUIRE(
                txn->set_property(*note, "k" + std::to_string(i), graph::Value{std::int64_t{i}}).has_value());
        REQUIRE(txn->commit().has_value());
    }
    scene.open();
    scene.open_panel();
    REQUIRE(scene.state->inspector().rows().size() > kProperties);
    std::size_t drawn = 0;
    for (const auto& [name, rect] : scene.view->item_rects())
        if (name.starts_with("inspector_key:"))
            ++drawn;
    CHECK(drawn > 0);
    CHECK(drawn < 100);
}

TEST_CASE("inspector panel: untrusted keys and values are drawn sanitised, never as widget labels") {
    Scene scene;
    REQUIRE(scene.set_prop(np("a.md"), "k##x%s", graph::Value{std::string("v\x1b[2J%n##y")}).has_value());
    scene.open();
    scene.open_panel();
    const RenderedText text;
    settle(*scene.view);
    CHECK(text.contains("k##x%s"));                 // TextUnformatted: `##` ve `%` yorumlanmaz
    CHECK(text.contains("v\xEF\xBF\xBD[2J%n##y"));  // ESC U+FFFD olur
    CHECK_FALSE(text.contains("\x1b"));
}
