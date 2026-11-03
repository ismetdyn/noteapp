// T84: not haritası (Harita görünümü). Veri GuiState'te, çizim ve etkileşim gerçek View'da; fare ve tuşlar
// gerçek ImGui/SDL olaylarıyla verilir.
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/note_seed.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::BrowserView;
using noteapp::gui::GuiState;
using noteapp::gui::MapOptions;

namespace core = noteapp::core;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

// a.md -> b.md, c.md -> a.md, a.md [[yok]] eksik; d.md ilgisiz; e.md #proje etiketli.
struct MapScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    MapScene() {
        fixture.add_note("b.md", "b metni\n", 40);
        fixture.add_note("a.md", "[[b]] [[yok]] #proje\n", 30);
        fixture.add_note("c.md", "[[a]]\n", 20);
        fixture.add_note("d.md", "ilgisiz\n", 10);
        fixture.add_note("e.md", "#proje ayrı\n", 5);
    }
    void open(bool whole_graph = true) {
        state = fixture.make_state();
        view = make_view(*state);
        state->open_note(np("a.md"));
        state->set_browser_view(BrowserView::Map);
        MapOptions options;
        options.whole_graph = whole_graph;
        state->set_map_options(options);
        settle_layout();
    }
    // Yerleşim durana dek kare üretilir (sınırlı): düğüm konumları testte sabit kalsın.
    void settle_layout() {
        constexpr int kMaxFrames = 600;
        for (int frame = 0; frame < kMaxFrames && !state->map_layout().settled(); ++frame)
            view->draw_frame();
        REQUIRE(state->map_layout().settled());
        settle(*view);
    }
    View::ItemRect rect(const std::string& name) const { return view->item_rects().at(name); }
};

// SDL olayı: '+' ve '-' klavye düzeninden bağımsız tuş koduyla okunur (Türkçe Q'da '+' Shift+4).
void press_sdl_key(View& view, SDL_Keycode key) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.down = true;
    REQUIRE(SDL_PushEvent(&event));
    view.draw_frame();
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    REQUIRE(SDL_PushEvent(&event));
    view.draw_frame();
}

void mouse_to(View& view, float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    view.draw_frame();
}

void mouse_button(View& view, bool down) {
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    view.draw_frame();
}

// Tuvalin düğümsüz köşesi: arka plan tıklaması odak verir, düğüm açmaz.
void focus_canvas(MapScene& scene) {
    const View::ItemRect canvas = scene.rect("map_canvas");
    mouse_to(*scene.view, canvas.min_x + 2.0F, canvas.min_y + 2.0F);
    mouse_button(*scene.view, true);
    mouse_button(*scene.view, false);
    settle(*scene.view);
}

}  // namespace

TEST_CASE("map: nodes are notes and edges are links and relations; a missing target is not drawn") {
    MapScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("d.md"), np("e.md")).has_value());
    }
    scene.open();
    const auto& map = scene.state->note_map();
    CHECK(map.nodes.size() == 5);
    CHECK(map.edges.size() == 3);  // a->b, c->a, d~e
    for (const char* note : {"map:a.md", "map:b.md", "map:c.md", "map:d.md", "map:e.md"})
        CHECK_MESSAGE(scene.view->item_rects().contains(note), note);
    CHECK_FALSE(scene.view->item_rects().contains("map:yok"));
    CHECK_FALSE(scene.view->item_rects().contains("map:#proje"));
    scene.state->set_map_options(MapOptions{.whole_graph = true, .show_tags = true});
    settle(*scene.view);
    CHECK(scene.view->item_rects().contains("map:#proje"));
}

TEST_CASE("map: the graph is read once per change, never per frame") {
    MapScene scene;
    scene.open();
    const std::size_t reads = scene.state->map_reads();
    CHECK(reads == 1);
    for (int frame = 0; frame < 20; ++frame)
        scene.view->draw_frame();
    scene.state->rescan();  // değişiklik yok
    scene.view->draw_frame();
    CHECK(scene.state->map_reads() == reads);
    scene.fixture.add_note("f.md", "[[d]]\n", 1);
    scene.state->rescan();
    scene.view->draw_frame();
    CHECK(scene.state->map_reads() == reads + 1);
    CHECK(scene.state->note_map().nodes.size() == 6);
}

TEST_CASE("map: the neighbourhood of the shown note has the chosen depth") {
    MapScene scene;
    scene.open(false);
    // a'nın çevresi derinlik 2: b ve c (d, e bağsız).
    CHECK(scene.state->note_map().nodes.size() == 3);
    scene.state->open_note(np("d.md"));
    scene.view->draw_frame();
    CHECK(scene.state->note_map().nodes.size() == 1);
    scene.state->open_note(np("c.md"));
    scene.state->set_map_options(MapOptions{.depth = 1});
    CHECK(scene.state->note_map().nodes.size() == 2);  // c ve a
}

TEST_CASE("map: positions are kept across a rescan for notes that still exist") {
    MapScene scene;
    scene.open();
    const auto& map = scene.state->note_map();
    const auto index = [&](const std::string& key) {
        return static_cast<std::size_t>(
            std::ranges::find_if(map.nodes, [&](const auto& node) { return node.key == key; }) -
            map.nodes.begin());
    };
    const auto before = scene.state->map_layout().positions()[index("b.md")];
    scene.fixture.add_note("g.md", "[[b]]\n", 1);
    scene.state->rescan();
    const auto after = scene.state->map_layout().positions()[index("b.md")];
    CHECK(after.x == before.x);
    CHECK(after.y == before.y);
}

TEST_CASE("map: clicking a node saves the shown note's edit to that note only and opens the clicked note") {
    MapScene scene;
    scene.open();
    scene.state->edit_text("a düzenlendi\n");
    REQUIRE(scene.state->dirty());
    settle(*scene.view);
    click_button(*scene.view, "map:b.md");
    CHECK(scene.fixture.read_note("a.md") == "a düzenlendi\n");
    CHECK(scene.fixture.read_note("b.md") == "b metni\n");
    REQUIRE(scene.state->current_path());
    CHECK(scene.state->current_path()->string() == "b.md");
    CHECK(scene.state->buffer() == "b metni\n");
    CHECK(scene.state->browser_view() == BrowserView::Map);
}

TEST_CASE("map: double-clicking a node opens it and switches to the tree") {
    MapScene scene;
    scene.open();
    const auto [x, y] = scene.view->button_center("map:c.md");
    mouse_to(*scene.view, x, y);
    for (int click = 0; click < 2; ++click) {
        mouse_button(*scene.view, true);
        mouse_button(*scene.view, false);
    }
    settle(*scene.view);
    REQUIRE(scene.state->current_path());
    CHECK(scene.state->current_path()->string() == "c.md");
    CHECK(scene.state->browser_view() == BrowserView::Tree);
}

TEST_CASE("map: dragging a node pins it where it was dropped") {
    MapScene scene;
    scene.open();
    const auto [x, y] = scene.view->button_center("map:d.md");
    mouse_to(*scene.view, x, y);
    mouse_button(*scene.view, true);
    constexpr float kDrag = 60.0F;
    for (int step = 1; step <= 4; ++step)
        mouse_to(*scene.view, x + kDrag * static_cast<float>(step) / 4.0F, y);
    mouse_button(*scene.view, false);
    scene.settle_layout();
    const auto [after_x, after_y] = scene.view->button_center("map:d.md");
    CHECK(after_x == doctest::Approx(x + kDrag).epsilon(0.02));
    CHECK(after_y == doctest::Approx(y).epsilon(0.02));
    CHECK(scene.state->current_path()->string() == "a.md");  // sürükleme açmaz
}

TEST_CASE("map: a tag: query highlights the matching notes") {
    MapScene scene;
    scene.open();
    scene.state->set_query("tag:proje");
    const auto& map = scene.state->note_map();
    const auto& matches = scene.state->map_matches();
    REQUIRE(matches.size() == map.nodes.size());
    std::vector<std::string> matched;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        if (matches[i] != 0)
            matched.push_back(map.nodes[i].key);
    }
    std::ranges::sort(matched);
    CHECK(matched == std::vector<std::string>{"a.md", "e.md"});
    scene.state->set_query({});
    CHECK(std::ranges::all_of(scene.state->map_matches(), [](char match) { return match != 0; }));
}

TEST_CASE("map: arrows pan, + and - zoom, 0 fits everything into view") {
    MapScene scene;
    scene.open();
    focus_canvas(scene);
    const auto [ax, ay] = scene.view->button_center("map:a.md");
    press(*scene.view, ImGuiKey_RightArrow);
    CHECK(scene.view->button_center("map:a.md").first < ax);
    CHECK(scene.view->button_center("map:a.md").second == doctest::Approx(ay));

    const auto spread = [&] {
        return std::abs(scene.view->button_center("map:a.md").first -
                        scene.view->button_center("map:b.md").first) +
               std::abs(scene.view->button_center("map:a.md").second -
                        scene.view->button_center("map:b.md").second);
    };
    const float before = spread();
    press_sdl_key(*scene.view, SDLK_PLUS);
    CHECK(spread() > before);
    press_sdl_key(*scene.view, SDLK_MINUS);
    press_sdl_key(*scene.view, SDLK_MINUS);
    CHECK(spread() < before);

    for (int step = 0; step < 6; ++step)
        press_sdl_key(*scene.view, SDLK_PLUS);
    for (int step = 0; step < 8; ++step)
        press(*scene.view, ImGuiKey_LeftArrow);
    press(*scene.view, ImGuiKey_0);
    const View::ItemRect canvas = scene.rect("map_canvas");
    for (const char* note : {"map:a.md", "map:b.md", "map:c.md", "map:d.md", "map:e.md"}) {
        REQUIRE_MESSAGE(scene.view->item_rects().contains(note), note);
        const auto [x, y] = scene.view->button_center(note);
        CHECK((x > canvas.min_x && x < canvas.max_x && y > canvas.min_y && y < canvas.max_y));
    }
}

TEST_CASE("map: the view mode survives a restart") {
    MapScene scene;
    scene.open();
    REQUIRE(scene.state->request_close());
    scene.view.reset();
    scene.state = scene.fixture.make_state();
    CHECK(scene.state->browser_view() == BrowserView::Map);
}

TEST_CASE("map: no frames are drawn once the layout has settled") {
    MapScene scene;
    constexpr int kNoRescanSeconds = 3600;  // zamanlı tarama ölçülen boşluğa kare eklemesin
    scene.fixture.config.tuning.rescan_seconds = kNoRescanSeconds;
    scene.state = scene.fixture.make_state();
    scene.view = make_view(*scene.state);
    scene.state->set_browser_view(BrowserView::Map);
    scene.view->draw_frame();
    REQUIRE_FALSE(scene.state->map_layout().settled());
    std::size_t frames = scene.view->frames();
    scene.view->idle_step(0);
    scene.view->idle_step(0);
    scene.view->idle_step(0);
    scene.view->idle_step(0);
    CHECK(scene.view->frames() >= frames + 4);  // yerleşim sürerken her adım çizer
    constexpr int kMaxSteps = 2000;
    for (int step = 0; step < kMaxSteps && !scene.state->map_layout().settled(); ++step)
        scene.view->idle_step(0);
    REQUIRE(scene.state->map_layout().settled());
    for (int step = 0; step < 10; ++step)  // olaydan sonraki birkaç yerleşme karesi
        scene.view->idle_step(0);
    frames = scene.view->frames();
    for (int step = 0; step < 20; ++step)
        scene.view->idle_step(0);
    CHECK(scene.view->frames() == frames);
}
