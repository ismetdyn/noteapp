// MAP-R: TUI haritasında ilişki kurma ve kaldırma. Kural GUI ile ortaktır (app::map_relations); burada
// TUI'nin klavye (`C`, Tab, Enter, `r` `l` `d`, Esc) ve fare (sağ sürükleme, kenara sağ tık) yolu gerçek
// depoyla.
#include "core/i18n.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Intent;
using tui::Key;
using tui::MouseButton;
using tui::MouseMotion;

namespace {

constexpr int kWidth = 120;
constexpr int kHeight = 30;
constexpr int kSettleSteps = 5000;
constexpr int kFirstCanvasRow = 3;  // üst çubuk, panel kenarı, kapsam satırı
constexpr int kFrameCells = 2;      // panel kenarı ve iç boşluk

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

struct Scene {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    int changes = 0;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { refresh(); }, [this] { ++changes; }}};
    tui::EditorSession editor{
        tui::EditorHost{*store, mutex, internal, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};

    explicit Scene(const char* a_body = "a\n") {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "b.md", "b\n"));
        REQUIRE(test::seed_note(*store, "c.md", "c #proje\n"));
        REQUIRE(test::seed_note(*store, "a.md", a_body));
        state.set_window(kWidth, kHeight);
        refresh();
        REQUIRE(state.select_note(np("a.md")));
        for (const tui::Command& command : tui::TuiState::commands())
            if (command.id == "view.map" && (command.contexts & tui::kInList) != 0)
                command.run(state);
        REQUIRE(state.map_view());
        key(Key::Char, "w");  // bütün çizge: üç not da görünür
        key(Key::Char, "t");  // etiketler
        settle();
    }
    void refresh() {
        preview.invalidate();
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    void update() {
        state.update_map([this] { return store->edges(); }, np("a.md"));
    }
    void settle() {
        update();
        for (int i = 0; i < kSettleSteps && state.step_map(); ++i) {
        }
    }
    // Tuş ve doğurduğu iş, main'in yolundan (run_intent → ListActions::run).
    void key(Key key, std::string_view text = {}) {
        const Intent intent = state.handle(key, text);
        if (intent == Intent::MapEdit)
            actions.run(intent, confirm, rename);
        update();
    }
    void mouse(int x, int y, MouseButton button, MouseMotion motion) {
        actions.on_mouse({x, y, button, motion}, editor);
        update();
    }
    std::string body(const std::string& name) const { return store->read(np(name)).value().text; }
    std::string target_key() const {
        const auto& link = state.map_link();
        return link && link->target ? link->target->key : std::string{};
    }
    // `C` kipinde Tab ile hedefe gelinir (sıra haritanın).
    void tab_to(const std::string& key_wanted) {
        for (std::size_t i = 0; i < state.map_model().map().nodes.size() && target_key() != key_wanted; ++i)
            key(Key::Tab);
        REQUIRE(target_key() == key_wanted);
    }
    // Düğümün tuval noktası (map_pane ile aynı hesap).
    std::pair<float, float> dot_of(const std::string& node_key) const {
        const auto& nodes = state.map_model().map().nodes;
        const auto found = std::ranges::find(nodes, node_key, &app::MapNode::key);
        REQUIRE(found != nodes.end());
        const app::LayoutPoint p =
            state.map_model().layout().positions()[static_cast<std::size_t>(found - nodes.begin())];
        const tui::MapCamera camera = state.map_camera();
        return {std::round(camera.x + p.x * camera.zoom), std::round(camera.y + p.y * camera.zoom)};
    }
    static std::pair<int, int> screen_cell(float dot_x, float dot_y) {
        return {static_cast<int>(std::floor(dot_x / tui::kMapDotsPerCellX)) + kFrameCells,
                static_cast<int>(std::floor(dot_y / tui::kMapDotsPerCellY)) + kFirstCanvasRow};
    }
    std::pair<int, int> screen_of(const std::string& node_key) const {
        const auto [x, y] = dot_of(node_key);
        return screen_cell(x, y);
    }
    void right_drag(const std::string& from, const std::string& to) {
        const auto [x, y] = screen_of(from);
        const auto [tx, ty] = screen_of(to);
        mouse(x, y, MouseButton::Right, MouseMotion::Press);
        mouse((x + tx) / 2, (y + ty) / 2, MouseButton::Right, MouseMotion::Move);
        mouse(tx, ty, MouseButton::Right, MouseMotion::Move);
        mouse(tx, ty, MouseButton::Right, MouseMotion::Release);
    }
};

}  // namespace

TEST_CASE("map connect: C, Tab to a note, Enter and r relate the shown note to it") {
    Scene s;
    s.key(Key::Char, "C");
    REQUIRE(s.state.map_link());
    CHECK(s.state.map_link()->source.key == "a.md");
    s.tab_to("b.md");
    s.key(Key::Enter);
    REQUIRE(s.state.map_link());
    CHECK(s.state.map_link()->choosing);
    s.key(Key::Char, "r");
    CHECK_FALSE(s.state.map_link());
    CHECK(s.status.message == "Related: a.md and b.md");
    CHECK(s.store->links(np("a.md")).value().related == std::vector{np("b.md")});
    CHECK(std::ranges::any_of(s.state.map_model().map().edges, [](const app::MapEdge& edge) {
        return edge.kind == app::MapEdgeKind::Related;
    }));
    CHECK(s.body("a.md") == "a\n");
}

TEST_CASE("map connect: l appends a wiki link to the source note") {
    Scene s;
    s.key(Key::Char, "C");
    s.tab_to("b.md");
    s.key(Key::Enter);
    s.key(Key::Char, "l");
    CHECK(s.body("a.md") == "a\n[[b.md]]\n");
    CHECK(s.status.message == "Link added to a.md: [[b.md]]");
}

TEST_CASE("map connect: Enter on a tag adds it to the note; Esc leaves the mode without a change") {
    Scene s;
    s.key(Key::Char, "C");
    s.tab_to("proje");
    s.key(Key::Enter);
    CHECK_FALSE(s.state.map_link());
    CHECK(s.body("a.md") == "a\n#proje\n");
    CHECK(s.status.message == "Tag #proje added to a.md");

    s.key(Key::Char, "C");
    s.tab_to("b.md");
    s.key(Key::Enter);
    s.key(Key::Escape);
    CHECK_FALSE(s.state.map_link());
    CHECK(s.store->links(np("a.md")).value().related.empty());
    CHECK(s.body("a.md") == "a\n#proje\n");
}

TEST_CASE("map connect: d removes the relation or the tag; a link edge is explained, not edited") {
    Scene s("a #proje [[c]]\n");
    REQUIRE(s.store->add_relation(np("a.md"), np("b.md")).has_value());
    s.state.map_edges_changed();
    s.settle();
    s.key(Key::Char, "C");
    s.tab_to("b.md");
    s.key(Key::Char, "d");
    CHECK(s.status.message == "Relation removed: a.md and b.md");
    CHECK(s.store->links(np("a.md")).value().related.empty());

    s.key(Key::Char, "C");
    s.tab_to("proje");
    s.key(Key::Char, "d");
    CHECK(s.body("a.md") == "a proje [[c]]\n");

    s.key(Key::Char, "C");
    s.tab_to("c.md");
    s.key(Key::Char, "d");
    CHECK(s.status.message ==
          "The link is part of the text of a.md; remove its [[link]] to c.md in the editor");
    CHECK(s.body("a.md") == "a proje [[c]]\n");

    s.key(Key::Char, "C");
    s.tab_to("b.md");
    s.key(Key::Char, "d");
    CHECK(s.status.message == "No edge between a.md and b.md");
}

TEST_CASE("map connect: a right-drag from note to note asks r or l; onto a tag it adds the tag") {
    Scene s;
    s.right_drag("b.md", "c.md");
    REQUIRE(s.state.map_link());
    CHECK(s.state.map_link()->source.key == "b.md");
    CHECK(s.state.map_link()->choosing);
    s.key(Key::Char, "r");
    CHECK(s.store->links(np("b.md")).value().related == std::vector{np("c.md")});

    s.right_drag("b.md", "proje");
    CHECK_FALSE(s.state.map_link());
    CHECK(s.body("b.md") == "b\n#proje\n");
    CHECK(s.status.message == "Tag #proje added to b.md");
}

TEST_CASE("map connect: a right click on an edge selects it and d removes it") {
    Scene s;
    REQUIRE(s.store->add_relation(np("a.md"), np("b.md")).has_value());
    s.state.map_edges_changed();
    s.settle();
    const auto [ax, ay] = s.dot_of("a.md");
    const auto [bx, by] = s.dot_of("b.md");
    const auto [x, y] = Scene::screen_cell((ax + bx) / 2.0F, (ay + by) / 2.0F);  // kenarın ortası
    s.mouse(x, y, MouseButton::Right, MouseMotion::Press);
    s.mouse(x, y, MouseButton::Right, MouseMotion::Release);
    REQUIRE(s.state.map_link());
    CHECK(s.state.map_link()->edge == app::MapEdgeKind::Related);
    s.key(Key::Char, "d");
    CHECK(s.store->links(np("a.md")).value().related.empty());
}
