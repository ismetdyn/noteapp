// F2: TUI haritasında keşif (SPEC-tui "Harita"). Kural GUI ile ortaktır (app::MapModel); burada TUI'nin tuşları
// gerçek depoyla: `x` `X` komşular, `e` `#` `N` süzgeçler, `/` bul, `R` en kısa yol, `P` `U` sabitleme, `n` yeni
// not, `d` çöpe ve `u` geri.
#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 120;
constexpr int kHeight = 30;
constexpr int kSettleSteps = 5000;

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

// a → b → c (bağlantı), a ~ d ("uses"), c #proje; derinlik 1'de a'nın çevresi a, b, d.
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
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { refresh(); }, [] {}}};

    Scene() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "c.md", "c #proje\n"));
        REQUIRE(test::seed_note(*store, "b.md", "[[c]]\n"));
        REQUIRE(test::seed_note(*store, "d.md", "d\n"));
        REQUIRE(test::seed_note(*store, "a.md", "[[b]]\n"));
        REQUIRE(app::relate_map_notes(*store, {app::MapNodeKind::Note, "a.md"}, {app::MapNodeKind::Note, "d.md"},
                                      {"uses", false}));
        state.set_window(kWidth, kHeight);
        refresh();
        REQUIRE(state.select_note(np("a.md")));
        for (const tui::Command& command : tui::TuiState::commands())
            if (command.id == "view.map" && (command.contexts & tui::kInList) != 0)
                command.run(state);
        REQUIRE(state.map_view());
        key(Key::Char, "1");
        settle();
    }
    void refresh() {
        preview.invalidate();
        state.set_notes(store->list());
        state.set_folders(store->folders());
        state.map_edges_changed();
    }
    void update() { state.update_map([this] { return store->edges(); }, np("a.md")); }
    void settle() {
        update();
        for (int i = 0; i < kSettleSteps && state.step_map(); ++i) {
        }
    }
    // Tuş ve doğurduğu iş main'in yolundan: harita işleri ListActions'a, `n` satırı adla gönderilir.
    void key(Key key, std::string_view text = {}, std::string_view new_note = {}) {
        const Intent intent = state.handle(key, text);
        if (intent == Intent::NewRow) {
            std::optional<tui::NewRowState> row{std::in_place};
            row->handle(Key::Paste, new_note);
            actions.submit_new_row(row, {"", "md", core::kDefaultSlugMaxLength}, [](const core::NotePath&) {});
        } else if (intent != Intent::None) {
            actions.run(intent, confirm, rename);
        }
        update();
    }
    void type(std::string_view text) {
        state.handle(Key::Paste, text);
        update();
    }
    std::set<std::string> keys() const {
        std::set<std::string> out;
        for (const app::MapNode& node : state.map_model().map().nodes)
            out.insert((node.kind == app::MapNodeKind::Tag ? "#" : "") + node.key);
        return out;
    }
    // Tab haritanın sırasıyla notları dolaşır.
    void select(const std::string& wanted) {
        for (std::size_t i = 0; i <= state.map_model().map().nodes.size() && state.map_selected_key() != wanted; ++i)
            key(Key::Tab);
        REQUIRE(state.map_selected_key() == wanted);
    }
    void tab_target(const std::string& wanted) {
        const auto target = [this] {
            const auto& link = state.map_link();
            return link && link->target ? link->target->key : std::string{};
        };
        for (std::size_t i = 0; i <= state.map_model().map().nodes.size() && target() != wanted; ++i)
            key(Key::Tab);
        REQUIRE(target() == wanted);
    }
    std::size_t node(const std::string& wanted) const {
        const auto& nodes = state.map_model().map().nodes;
        const auto found = std::ranges::find(nodes, wanted, &app::MapNode::key);
        REQUIRE(found != nodes.end());
        return static_cast<std::size_t>(found - nodes.begin());
    }
    ftxui::Screen shot() const {
        auto shot = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(shot, tui::render(state, {}, status, {kWidth, kHeight}));
        return shot;
    }
    // Harita panelinde `text`in ilk harfinin rengi (adın hücresi); sağ panelin bağlantı satırları sayılmaz.
    std::optional<ftxui::Color> color_of(std::string_view text) const {
        const ftxui::Screen screen = shot();
        constexpr int kPaneChrome = 2;  // panel kenarı ve iç boşluk
        const int pane_right = state.map_cells().first + kPaneChrome;
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x + static_cast<int>(text.size()) <= pane_right; ++x) {
                bool match = true;
                for (std::size_t i = 0; i < text.size() && match; ++i)
                    match = screen.PixelAt(x + static_cast<int>(i), y).character == std::string(1, text[i]);
                if (match)
                    return screen.PixelAt(x, y).foreground_color;
            }
        return std::nullopt;
    }
    std::string screen() const {
        const ftxui::Screen shot = this->shot();
        std::string out;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x)
                out += shot.PixelAt(x, y).character;
            out += "\n";
        }
        return out;
    }
};

using Keys = std::set<std::string>;

}  // namespace

TEST_CASE("map explore: x opens the selected note's neighbours past the depth, x again closes; X adds a hop") {
    Scene s;
    CHECK(s.keys() == Keys{"a.md", "b.md", "d.md"});
    s.select("b.md");
    s.key(Key::Char, "x");
    CHECK(s.keys() == Keys{"a.md", "b.md", "c.md", "d.md"});
    s.key(Key::Char, "x");
    CHECK(s.keys() == Keys{"a.md", "b.md", "d.md"});
    CHECK_FALSE(s.state.map_model().scope().expanded.contains("b.md"));
    CHECK_FALSE(s.state.map_model().scope().collapsed.contains("b.md"));
    s.key(Key::Char, "X");
    s.key(Key::Char, "X");
    CHECK(s.keys() == Keys{"a.md", "b.md", "c.md", "d.md"});
    CHECK(s.state.map_model().scope().expanded.at("b.md").depth == 2);
}

// İnceleme bulgusu: merkezde `x` hiçbir şey eklemiyor, ikinci `x` haritayı merkeze indiriyordu.
TEST_CASE("map explore: x on the shown note opens one hop past the depth; x again brings the same map back") {
    Scene s;
    CHECK(s.keys() == Keys{"a.md", "b.md", "d.md"});
    s.key(Key::Char, "x");  // Tab seçimi yok: gösterilen not
    CHECK(s.keys() == Keys{"a.md", "b.md", "c.md", "d.md"});
    s.key(Key::Char, "x");
    CHECK(s.keys() == Keys{"a.md", "b.md", "d.md"});
}

// İnceleme bulgusu: seçimsiz `d` (kenar kaldırıldıktan sonraki ikinci `d` gibi) gösterilen notu sessizce çöpe atıyordu.
TEST_CASE("map explore: d removes the edge in connect mode; a second d without a selected note trashes nothing") {
    Scene s;
    s.key(Key::Char, "C");
    s.tab_target("d.md");
    s.key(Key::Char, "d");
    CHECK(s.store->links(np("a.md")).value().related.empty());
    s.key(Key::Char, "d");
    CHECK(s.store->read(np("a.md")));
    CHECK(s.store->read(np("d.md")));
    CHECK(s.status.message == core::tr("map.trash_needs_selection"));
}

TEST_CASE("map explore: n then Esc on the name line leaves no pin for a later note") {
    Scene s;
    REQUIRE(s.state.handle(Key::Char, "n") == Intent::NewRow);
    // Satırda Esc: satır main'de kapanır, TuiState'e haber gitmez; sonraki tuş (palet) bekleyen yeri düşürür.
    s.state.handle(Key::Char, ":");
    s.state.handle(Key::Escape, {});
    std::optional<tui::NewRowState> row{std::in_place};  // sonra paletten başka not
    row->handle(Key::Paste, "sonra");
    s.actions.submit_new_row(row, {"", "md", core::kDefaultSlugMaxLength}, [](const core::NotePath&) {});
    REQUIRE(s.store->read(np("sonra.md")));
    CHECK_FALSE(s.state.map_model().scope().kept.contains("sonra.md"));
}

TEST_CASE("map explore: in the whole graph x says every neighbour is already shown") {
    Scene s;
    s.key(Key::Char, "w");
    s.key(Key::Char, "x");
    CHECK(s.status.message == core::tr("map.whole_graph_open"));
}

TEST_CASE("map explore: e cycles links only, relations only and all; x then follows the filtered kind") {
    Scene s;
    s.key(Key::Char, "e");
    CHECK_FALSE(s.state.map_model().options().show_related);
    CHECK(s.keys() == Keys{"a.md", "b.md"});
    CHECK(s.screen().find("links only") != std::string::npos);
    s.key(Key::Char, "e");
    CHECK_FALSE(s.state.map_model().options().show_links);
    CHECK(s.keys() == Keys{"a.md", "d.md"});
    s.key(Key::Char, "e");
    CHECK(s.state.map_model().options().show_links);
    CHECK(s.state.map_model().options().show_related);

    s.key(Key::Char, "e");  // yalnız bağlantılar: açılış bağlantı türüyle
    s.select("b.md");
    s.key(Key::Char, "x");
    CHECK(s.state.map_model().scope().expanded.at("b.md").kind == app::MapEdgeKind::Link);
}

TEST_CASE("map explore: N filters one relation name from the store's names; # filters a tag; empty clears") {
    Scene s;
    s.key(Key::Char, "N");
    REQUIRE(s.state.map_input_kind() == tui::TuiState::MapInputKind::Relation);
    CHECK(s.screen().find("Relation filter:") != std::string::npos);
    s.key(Key::Tab);  // tek ad: tamamlanır
    CHECK(s.state.map_input().text() == "uses");
    s.key(Key::Enter);
    CHECK_FALSE(s.state.map_input_kind());
    CHECK(s.state.map_model().options().relation == std::optional<std::string>("uses"));
    CHECK(s.keys() == Keys{"a.md", "b.md", "d.md"});
    CHECK(s.screen().find("relation 'uses'") != std::string::npos);
    s.key(Key::Char, "N");
    CHECK(s.state.map_input().text() == "uses");  // satır geçerli süzgeçle açılır
    for (int i = 0; i < 4; ++i)
        s.key(Key::Backspace);
    s.key(Key::Enter);
    CHECK_FALSE(s.state.map_model().options().relation);

    s.key(Key::Char, "w");
    s.key(Key::Char, "#");
    REQUIRE(s.state.map_input_kind() == tui::TuiState::MapInputKind::Tag);
    s.type("proje");
    s.key(Key::Enter);
    CHECK(s.state.map_model().options().tag == "proje");
    CHECK(s.keys() == Keys{"a.md", "c.md"});  // gösterilen not kalır
    s.key(Key::Char, "#");
    CHECK(s.state.map_input().text() == "proje");  // satır geçerli süzgeçle açılır
    s.key(Key::Escape);
    CHECK(s.state.map_model().options().tag == "proje");
    s.key(Key::Char, "#");
    s.key(Key::Backspace);
    s.key(Key::Backspace);
    s.key(Key::Backspace);
    s.key(Key::Backspace);
    s.key(Key::Backspace);
    s.key(Key::Enter);
    CHECK(s.state.map_model().options().tag.empty());
}

TEST_CASE("map explore: / finds nodes, Enter steps to the next match and centres it; Esc keeps it selected") {
    Scene s;
    s.key(Key::Char, "w");
    s.settle();
    s.key(Key::Char, "/");
    REQUIRE(s.state.map_input_kind() == tui::TuiState::MapInputKind::Find);
    s.type("c.");
    REQUIRE(s.state.map_found().size() == 1);
    CHECK(s.state.map_selected_key() == "c.md");
    const auto [columns, rows] = s.state.map_cells();
    const app::LayoutPoint at = s.state.map_model().layout().positions()[s.node("c.md")];
    const tui::MapCamera camera = s.state.map_camera();
    CHECK(camera.x + at.x * camera.zoom == doctest::Approx(columns * tui::kMapDotsPerCellX / 2.0F).epsilon(0.01));
    CHECK(camera.y + at.y * camera.zoom == doctest::Approx(rows * tui::kMapDotsPerCellY / 2.0F).epsilon(0.01));
    CHECK(s.screen().find("1/1") != std::string::npos);

    s.state.handle(Key::Backspace, {});
    s.state.handle(Key::Backspace, {});
    s.type(".md");
    REQUIRE(s.state.map_found().size() == 4);
    const std::string first = s.state.map_selected_key();
    s.key(Key::Enter);
    CHECK(s.state.map_selected_key() != first);
    s.key(Key::ShiftTab);
    CHECK(s.state.map_selected_key() == first);
    s.key(Key::Escape);
    CHECK_FALSE(s.state.map_input_kind());
    CHECK(s.state.map_found().empty());
    CHECK(s.state.map_selected_key() == first);
}

TEST_CASE("map explore: R, a target and Enter show the shortest path with its hops; d makes it directed") {
    Scene s;
    s.key(Key::Char, "w");
    s.key(Key::Char, "R");
    REQUIRE(s.state.map_link());
    CHECK(s.state.map_link()->path);
    s.tab_target("c.md");
    s.key(Key::Enter);
    CHECK_FALSE(s.state.map_link());
    CHECK(s.status.message == "Shortest path a.md → c.md: length 2");
    const app::NoteMap& map = s.state.map_model().map();
    REQUIRE(map.path.size() == 3);
    CHECK(map.nodes[map.path[1]].key == "b.md");
    CHECK(s.screen().find("path length 2") != std::string::npos);
    // Yolun ucu sıkışık haritada da adlanır ve yeşildir (ara düğümün adı yalnız yakınken yazılır).
    CHECK(s.color_of("c.md") == ftxui::Color(ftxui::Color::Green));
    CHECK(s.color_of("d.md") != ftxui::Color(ftxui::Color::Green));  // yolda değil

    s.key(Key::Escape);  // yol temizlenir, arama değil
    CHECK(s.state.map_model().map().path.empty());

    s.select("c.md");
    s.key(Key::Char, "R");
    s.tab_target("a.md");
    s.key(Key::Char, "d");
    CHECK(s.state.map_link()->directed);
    CHECK(s.screen().find("(directed)") != std::string::npos);
    s.key(Key::Enter);
    CHECK(s.status.message == "No directed path from c.md to a.md");
    CHECK(s.state.map_model().map().path.empty());
}

TEST_CASE("map explore: P pins the selected note, P again unpins it, U unpins all") {
    Scene s;
    s.select("b.md");
    s.key(Key::Char, "P");
    CHECK(s.state.map_model().layout().pinned(s.node("b.md")));
    CHECK(s.status.message == "Pinned b.md");
    s.key(Key::Char, "P");
    CHECK_FALSE(s.state.map_model().layout().pinned(s.node("b.md")));
    s.key(Key::Char, "P");
    s.key(Key::Char, "U");
    CHECK_FALSE(s.state.map_model().layout().has_pins());
}

TEST_CASE("map explore: n makes a note that stays on the map pinned at its middle; d trashes and u restores") {
    Scene s;
    s.key(Key::Char, "n", "yeni");
    REQUIRE(s.store->read(np("yeni.md")));
    s.settle();
    REQUIRE(s.keys().contains("yeni.md"));  // a'nın çevresinde değil, haritadan oluşturuldu
    CHECK(s.state.map_model().layout().pinned(s.node("yeni.md")));

    s.select("b.md");
    s.key(Key::Char, "d");
    CHECK_FALSE(s.store->read(np("b.md")));
    CHECK_FALSE(s.keys().contains("b.md"));
    s.key(Key::Char, "u");
    CHECK(s.store->read(np("b.md")));
    CHECK(s.keys().contains("b.md"));
}
