// T4: TUI not haritası (SPEC-tui "Harita"). Model ve yerleşim GUI ile ortaktır (app::MapModel); TUI onu
// FTXUI braille tuvaline çizer, klavyeyle kaydırır, yakınlaştırır ve düğüm seçer.
#include "app/note_map.hpp"
#include "core/i18n.hpp"
#include "tui/map_view.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Key;

namespace {

constexpr int kWidth = 120;
constexpr int kHeight = 30;
constexpr int kSettleSteps = 5000;

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

core::NoteInfo listed(const std::string& path) {
    return core::NoteInfo{np(path), std::filesystem::file_time_type{}, 1};
}

core::NoteEdge link(const std::string& from, const std::string& to) {
    return {np(from), np(to), core::EdgeKind::LinksTo};
}

void run(tui::TuiState& state, std::string_view id) {
    for (const tui::Command& command : tui::TuiState::commands())
        if (command.id == id && (command.contexts & tui::kInList) != 0) {
            command.run(state);
            return;
        }
    FAIL("no list command ", id);
}

// a -> b -> c -> d: derinlik 2'de a'nın çevresi a, b, c.
struct MapFixture {
    tui::TuiState state;
    std::vector<core::NoteEdge> edges{link("a.md", "b.md"), link("b.md", "c.md"), link("c.md", "d.md")};
    int reads = 0;
    std::optional<core::NotePath> center = np("a.md");

    MapFixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
        state.set_notes({listed("a.md"), listed("b.md"), listed("c.md"), listed("d.md")});
        run(state, "view.map");
        update();
    }
    void update() {
        state.update_map(
            [this]() -> std::expected<std::vector<core::NoteEdge>, core::Error> {
                ++reads;
                return edges;
            },
            center);
    }
    void settle() {
        for (int i = 0; i < kSettleSteps && state.step_map(); ++i) {
        }
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        for (const app::MapNode& node : state.map_model().map().nodes)
            out.push_back((node.kind == app::MapNodeKind::Tag ? "#" : "") + node.key);
        std::ranges::sort(out);
        return out;
    }
    ftxui::Screen draw() const {
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(state, tui::Preview{}, {"sync off", ""}, {kWidth, kHeight}));
        return screen;
    }
};

bool is_braille(const std::string& cell) {
    // U+2800–U+28FF: E2 A0 80 – E2 A3 BF
    return cell.size() == 3 && static_cast<unsigned char>(cell[0]) == 0xE2 &&
           static_cast<unsigned char>(cell[1]) >= 0xA0 && static_cast<unsigned char>(cell[1]) <= 0xA3;
}

std::optional<std::pair<int, int>> find(const ftxui::Screen& screen, std::string_view text, int max_x) {
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x + static_cast<int>(text.size()) <= max_x; ++x) {
            bool match = true;
            for (std::size_t i = 0; i < text.size() && match; ++i)
                match = screen.CellAt(x + static_cast<int>(i), y).character == std::string(1, text[i]);
            if (match)
                return std::pair(x, y);
        }
    return std::nullopt;
}

}  // namespace

TEST_CASE("map: view.map shows the map, view.tree and the trash leave it") {
    MapFixture f;
    CHECK(f.state.map_view());
    run(f.state, "view.tree");
    CHECK_FALSE(f.state.map_view());
    run(f.state, "view.map");
    f.state.enter_trash({});
    CHECK_FALSE(f.state.map_view());
    f.state.leave_trash();
    CHECK(f.state.map_view());
}

TEST_CASE("map: edges are read once per change and never while the map is hidden") {
    MapFixture f;
    f.update();
    CHECK(f.reads == 1);
    f.state.set_notes({listed("a.md"), listed("b.md"), listed("c.md"), listed("d.md"), listed("e.md")});
    f.update();
    CHECK(f.reads == 2);
    run(f.state, "view.tree");
    f.state.set_notes({listed("a.md")});
    f.update();
    CHECK(f.reads == 2);
}

TEST_CASE("map: the neighbourhood, the depth keys, whole graph and tags follow the GUI scopes") {
    MapFixture f;
    CHECK(f.keys() == std::vector<std::string>{"a.md", "b.md", "c.md"});
    f.state.handle(Key::Char, "1");
    f.update();
    CHECK(f.keys() == std::vector<std::string>{"a.md", "b.md"});
    CHECK(f.state.map_model().options().depth == 1);
    f.state.handle(Key::Char, "w");
    f.update();
    CHECK(f.keys().size() == 4);
    f.state.handle(Key::Char, "t");
    CHECK(f.state.map_model().options().show_tags);
}

// NY-T: Listede `t` çöptür; haritada etiket tuşu onu gölgeler, çöpe `gt` ile gidilir.
TEST_CASE("map: t toggles the tags instead of the trash; gt still opens the trash") {
    MapFixture f;
    CHECK(f.state.handle(Key::Char, "t") == tui::Intent::None);
    CHECK(f.state.map_model().options().show_tags);
    CHECK_FALSE(f.state.trash_view());
    f.state.handle(Key::Char, "t");
    CHECK_FALSE(f.state.map_model().options().show_tags);
    f.state.handle(Key::Char, "g");
    CHECK(f.state.handle(Key::Char, "t") == tui::Intent::ToggleTrash);
    run(f.state, "view.tree");
    CHECK(f.state.handle(Key::Char, "t") == tui::Intent::ToggleTrash);  // haritasız Listede çöp
}

// NX-B (L1): derinlik tuşlarının üstü limits.app.map_depth (varsayılan 6, GUI adımlayıcısıyla aynı model
// sınırı); 0'da tek haneli her derinlik. Sınır küçülürse geçerli derinlik ona iner.
TEST_CASE("map: limits.app.map_depth bounds the depth keys and 0 lets every digit through") {
    MapFixture f;
    const auto depth = [&f] { return f.state.map_model().options().depth; };
    f.state.set_map_depth_limit({6, "6"});
    f.state.handle(Key::Char, "7");  // sınırın üstü derinlik tuşu değildir
    CHECK(depth() == app::MapOptions::kDefaultDepth);
    f.state.set_map_depth_limit({3, "3"});
    f.state.handle(Key::Char, "4");
    CHECK(depth() == app::MapOptions::kDefaultDepth);
    f.state.handle(Key::Char, "3");
    CHECK(depth() == 3);
    CHECK(f.state.map_model().max_depth() == 3);
    f.state.set_map_depth_limit({0, "0"});
    f.state.handle(Key::Char, "9");
    f.update();
    CHECK(depth() == 9);
    CHECK(f.keys().size() == 4);  // a'dan 3 adımda d
    f.state.set_map_depth_limit({2, "2"});
    CHECK(depth() == 2);
    f.update();
    CHECK(f.keys() == std::vector<std::string>{"a.md", "b.md", "c.md"});
}

TEST_CASE("map: the layout settles and then a frame does no layout work") {
    MapFixture f;
    REQUIRE(f.state.step_map());  // yeni çizge: yerleşim çalışır
    f.settle();
    CHECK_FALSE(f.state.step_map());
    const std::size_t done = f.state.map_model().layout().iterations_done();
    CHECK(done > 0);
    CHECK_FALSE(f.state.step_map());
    f.update();
    CHECK(f.state.map_model().layout().iterations_done() == done);
}

TEST_CASE("map: the graph is drawn with braille edges and labels, the current note in yellow") {
    MapFixture f;
    f.settle();
    const ftxui::Screen screen = f.draw();
    const int list_width = f.state.layout().geometry(kWidth, false).list_width;
    int braille = 0;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < list_width; ++x)
            braille += is_braille(screen.CellAt(x, y).character) ? 1 : 0;
    CHECK(braille > 0);
    const auto current = find(screen, "a.md", list_width);
    REQUIRE(current);
    CHECK(screen.CellAt(current->first, current->second).foreground_color ==
          ftxui::Color(ftxui::Color::Yellow));
    CHECK(find(screen, "b.md", list_width));
}

TEST_CASE("map: arrows pan, + and - zoom, 0 fits again") {
    MapFixture f;
    f.settle();
    const tui::MapCamera fitted = f.state.map_camera();
    f.state.handle(Key::Right);
    CHECK(f.state.map_camera().x != fitted.x);
    f.state.handle(Key::Down);
    CHECK(f.state.map_camera().y != fitted.y);
    f.state.handle(Key::Char, "+");
    CHECK(f.state.map_camera().zoom > fitted.zoom);
    f.state.handle(Key::Char, "-");
    f.state.handle(Key::Char, "-");
    CHECK(f.state.map_camera().zoom < fitted.zoom);
    f.state.handle(Key::Char, "0");
    CHECK(f.state.map_camera().zoom == doctest::Approx(fitted.zoom));
    CHECK(f.state.map_camera().x == doctest::Approx(fitted.x));
}

TEST_CASE("map: Tab cycles the notes, Enter opens the selected one and its label is bracketed") {
    MapFixture f;
    f.settle();
    CHECK_FALSE(f.state.map_selected());
    CHECK(f.state.handle(Key::Enter) == tui::Intent::None);
    f.state.handle(Key::Tab);
    const auto first = f.state.map_selected();
    REQUIRE(first);
    f.state.handle(Key::Tab);
    REQUIRE(f.state.map_selected());
    CHECK(*f.state.map_selected() != *first);
    f.state.handle(Key::ShiftTab);
    CHECK(f.state.map_selected() == first);
    CHECK(f.state.handle(Key::Enter) == tui::Intent::OpenMapNode);
    const int list_width = f.state.layout().geometry(kWidth, false).list_width;
    CHECK(find(f.draw(), "[" + core::last_component(first->string()) + "]", list_width));
}

// NY-T: GUI'de düğüm seçilince diğerleri titriyordu; TUI'de seçim (Tab, düğüme tıklayıp bırakma) yalnız vurgudur:
// merkez, yerleşim ve kamera değişmez, yeni yerleşim adımı (animasyon karesi) istenmez.
TEST_CASE("map: selecting a node moves no node, keeps the camera and starts no layout frames") {
    MapFixture f;
    f.settle();
    REQUIRE_FALSE(f.state.step_map());
    const std::vector<app::LayoutPoint> positions = f.state.map_model().layout().positions();
    const tui::MapCamera camera = f.state.map_camera();
    const auto unchanged = [&] {
        const std::vector<app::LayoutPoint>& now = f.state.map_model().layout().positions();
        REQUIRE(now.size() == positions.size());
        for (std::size_t i = 0; i < now.size(); ++i) {
            CHECK(now[i].x == positions[i].x);
            CHECK(now[i].y == positions[i].y);
        }
        CHECK(f.state.map_camera().x == camera.x);
        CHECK(f.state.map_camera().y == camera.y);
        CHECK(f.state.map_camera().zoom == camera.zoom);
        CHECK_FALSE(f.state.step_map());
    };
    for (int i = 0; i < 4; ++i) {
        f.state.handle(Key::Tab);
        f.update();  // her karede olduğu gibi: merkez aynı not
        unchanged();
    }
    // Düğüme basıp aynı yerde bırakmak vazgeçmektir: yine hiçbir şey kımıldamaz.
    const auto& nodes = f.state.map_model().map().nodes;
    const auto b = std::ranges::find_if(nodes, [](const app::MapNode& node) { return node.key == "b.md"; });
    REQUIRE(b != nodes.end());
    const app::LayoutPoint at = positions[static_cast<std::size_t>(b - nodes.begin())];
    const int column = static_cast<int>(std::lround(camera.x + at.x * camera.zoom)) / tui::kMapDotsPerCellX;
    const int row = static_cast<int>(std::lround(camera.y + at.y * camera.zoom)) / tui::kMapDotsPerCellY;
    REQUIRE(f.state.begin_map_drag(column, row));
    f.state.end_map_drag(column, row);
    f.update();
    unchanged();
}

// F2: haritada `/` haritada bul'dur (map_explore_test); ad araması paletin `search` komutuyla açılır.
TEST_CASE("map: a search lights the matching notes and dims the rest") {
    MapFixture f;
    run(f.state, "search");
    f.state.handle(Key::Char, "b");
    f.update();
    const app::NoteMap& map = f.state.map_model().map();
    const std::vector<char>& matches = f.state.map_model().matches();
    REQUIRE(map.nodes.size() == 3);
    REQUIRE(matches.size() == map.nodes.size());
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        CAPTURE(map.nodes[i].key);
        CHECK((matches[i] != 0) == (map.nodes[i].key == "b.md"));
    }
}
