// T84: not haritası (Harita görünümü). Veri GuiState'te, çizim ve etkileşim gerçek View'da; fare ve tuşlar
// gerçek ImGui/SDL olaylarıyla verilir.
#include "gui/gui_state.hpp"
#include "graph/graph.hpp"
#include "gui/view.hpp"
#include "support/note_seed.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
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

// MAP-R: Shift basılıyken düğümden düğüme sürükleme (gerçek ImGui olayları).
void shift_drag(View& view, const std::string& from, const std::string& to) {
    const auto [x, y] = view.button_center(from);
    const auto [tx, ty] = view.button_center(to);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    mouse_to(view, x, y);
    mouse_button(view, true);
    constexpr int kSteps = 4;
    for (int step = 1; step <= kSteps; ++step) {
        const float share = static_cast<float>(step) / kSteps;
        mouse_to(view, x + (tx - x) * share, y + (ty - y) * share);
    }
    mouse_button(view, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    settle(view);
}

void right_click(View& view, float x, float y) {
    mouse_to(view, x, y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    settle(view);
}

// İki düğümün ortası: kenarın üstü.
std::pair<float, float> edge_middle(const View& view, const std::string& from, const std::string& to) {
    const auto [x1, y1] = view.button_center(from);
    const auto [x2, y2] = view.button_center(to);
    return {(x1 + x2) / 2.0F, (y1 + y2) / 2.0F};
}

bool has_map_edge(GuiState& state, const std::string& from, const std::string& to,
                  noteapp::gui::MapEdgeKind kind) {
    const auto& map = state.note_map();
    return std::ranges::any_of(map.edges, [&](const auto& edge) {
        const std::string& a = map.nodes[edge.from].key;
        const std::string& b = map.nodes[edge.to].key;
        return edge.kind == kind && ((a == from && b == to) || (a == to && b == from));
    });
}

// Tuvalin düğümsüz köşesi: arka plan tıklaması odak verir, düğüm açmaz.
void focus_canvas(MapScene& scene) {
    const View::ItemRect canvas = scene.rect("map_canvas");
    mouse_to(*scene.view, canvas.min_x + 2.0F, canvas.min_y + 2.0F);
    mouse_button(*scene.view, true);
    mouse_button(*scene.view, false);
    settle(*scene.view);
}


// F1 ad penceresi: modal döngüsünün karelerinde sırayla adımlar, her biri kendi karesinde (ImGui olay kuyruğu
// bas-bırak ve fare olaylarını ardışık karelere dağıtır). Yanıt tutmazsa Esc: test asılı kalmaz.
using ModalStep = std::function<void()>;
struct ModalSteps {
    std::vector<ModalStep> steps;
    std::size_t frame = 0;
    void operator()() {
        constexpr std::size_t kSettleFrames = 4;  // odak ve kısayol rotası yerleşsin
        constexpr std::size_t kStepFrames = 3;
        constexpr std::size_t kGiveUpFrame = 300;
        ++frame;
        if (frame == 1) {  // açan sürükleme ve tıklamanın tuşları bırakılır
            ImGuiIO& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Shift, false);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        }
        if (frame > kSettleFrames && (frame - kSettleFrames) % kStepFrames == 0) {
            const std::size_t index = (frame - kSettleFrames) / kStepFrames - 1;
            if (index < steps.size())
                steps[index]();
        }
        if (frame == kGiveUpFrame)
            gui_test::tap_key(ImGuiKey_Escape);
    }
};

ModalStep press_key(ImGuiKey key) {
    return [key] { gui_test::tap_key(key); };
}

ModalStep type(std::string text) {
    return [text = std::move(text)] { ImGui::GetIO().AddInputCharactersUTF8(text.c_str()); };
}

// Diyalogdaki adlı alana tıklama: konum, basış ve bırakış kuyrukta ardışık karelere düşer.
ModalStep click(const View& view, std::string name) {
    return [&view, name = std::move(name)] {
        REQUIRE_MESSAGE(view.item_rects().contains(name), name);
        const auto [x, y] = view.button_center(name);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    };
}

std::vector<core::RelatedNote> related_of(const MapScene& scene, const std::string& path) {
    return core::NoteStore(scene.fixture.store_dir()).links(np(path)).value().related;
}

void put_relation(const MapScene& scene, const std::string& from, const std::string& to,
                  const core::RelationSpec& spec) {
    core::NoteStore store(scene.fixture.store_dir());
    REQUIRE(store.add_relation(np(from), np(to), spec).has_value());
}

// Tekerlek `x`,`y` çevresinde yakınlaştırır (o nokta yerinde kalır).
void wheel_at(View& view, float x, float y, float notches) {
    mouse_to(view, x, y);
    ImGui::GetIO().AddMouseWheelEvent(0.0F, notches);
    view.draw_frame();
    settle(view);
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

// F2: çift tıklama komşuları açar/kapar (Neo4j Browser); eskiden notu açıp Ağaç'a geçiyordu. İlk tıklama notu
// açar, görünüm Harita'da kalır.
TEST_CASE("map: double-clicking a node opens its neighbours, again closes them; the view stays the map") {
    MapScene scene;
    scene.open(false);
    scene.state->set_map_options(MapOptions{.depth = 1});
    scene.settle_layout();
    const auto shown = [&] {
        std::set<std::string> keys;
        for (const auto& node : scene.state->note_map().nodes)
            keys.insert(node.key);
        return keys;
    };
    const auto double_click = [&](const std::string& node) {
        const auto [x, y] = scene.view->button_center(node);
        mouse_to(*scene.view, x, y);
        for (int click = 0; click < 2; ++click) {
            mouse_button(*scene.view, true);
            mouse_button(*scene.view, false);
        }
        settle(*scene.view);
    };
    // Derinlik 1: c'nin çevresi c, a. İlk tıklama c'yi açıp merkez yapar, çift tıklama bir adım daha (b) açar;
    // ikinci çift tıklama yalnız açılışın eklediğini geri alır (inceleme bulgusu: harita tek düğüme iniyordu).
    double_click("map:c.md");
    scene.settle_layout();
    REQUIRE(scene.state->current_path());
    CHECK(scene.state->current_path()->string() == "c.md");
    CHECK(scene.state->browser_view() == BrowserView::Map);
    CHECK(shown() == std::set<std::string>{"a.md", "b.md", "c.md"});
    focus_canvas(scene);  // başka yere tıklama: sonraki iki tıklama yeni bir çift tıklamadır (üçüncü değil)
    double_click("map:c.md");
    scene.settle_layout();
    CHECK(shown() == std::set<std::string>{"a.md", "c.md"});
    CHECK_FALSE(scene.state->map_scope().expanded.contains("c.md"));
    CHECK_FALSE(scene.state->map_scope().collapsed.contains("c.md"));
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

TEST_CASE("map: Shift+dragging a note onto another asks Related or Link; Related adds the relation") {
    MapScene scene;
    scene.open();
    scene.view->install_deciders();
    ModalSteps accept{{press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(accept);
    shift_drag(*scene.view, "map:d.md", "map:e.md");
    REQUIRE(scene.view->item_rects().contains("map_connect_related"));
    CHECK(scene.view->item_rects().contains("map_connect_link"));
    CHECK(scene.state->current_path()->string() == "a.md");  // sürükleme açmaz
    click_button(*scene.view, "map_connect_related");
    CHECK(scene.state->status() == "Related: d.md and e.md");
    CHECK(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
    const auto links = core::NoteStore(scene.fixture.store_dir()).links(np("d.md")).value();
    REQUIRE(links.related.size() == 1);
    CHECK(links.related.front().path == np("e.md"));
    CHECK(scene.fixture.read_note("d.md") == "ilgisiz\n");
    for (std::size_t node = 0; node < scene.state->map_layout().size(); ++node)
        CHECK_FALSE(scene.state->map_layout().pinned(node));  // Shift+sürükleme iğnelemez
}

TEST_CASE("map: the Link choice saves the shown note's edit first and appends the wiki link") {
    MapScene scene;
    scene.open();
    scene.state->edit_text("a düzenlendi\n");
    REQUIRE(scene.state->dirty());
    settle(*scene.view);
    shift_drag(*scene.view, "map:a.md", "map:d.md");
    click_button(*scene.view, "map_connect_link");
    CHECK(scene.fixture.read_note("a.md") == "a düzenlendi\n[[d.md]]\n");
    CHECK_FALSE(scene.state->dirty());
    CHECK(scene.state->buffer() == "a düzenlendi\n[[d.md]]\n");  // editör yeni metni gösterir
    CHECK(has_map_edge(*scene.state, "a.md", "d.md", noteapp::gui::MapEdgeKind::Link));
}

TEST_CASE("map: Esc closes the choice and nothing changes; a drop on empty space does nothing") {
    MapScene scene;
    scene.open();
    shift_drag(*scene.view, "map:d.md", "map:e.md");
    REQUIRE(scene.view->item_rects().contains("map_connect_related"));
    press(*scene.view, ImGuiKey_Escape);
    CHECK_FALSE(scene.view->item_rects().contains("map_connect_related"));
    CHECK(core::NoteStore(scene.fixture.store_dir()).links(np("d.md")).value().related.empty());

    const View::ItemRect canvas = scene.rect("map_canvas");
    const auto [x, y] = scene.view->button_center("map:d.md");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    mouse_to(*scene.view, x, y);
    mouse_button(*scene.view, true);
    mouse_to(*scene.view, (x + canvas.min_x + 2.0F) / 2.0F, (y + canvas.min_y + 2.0F) / 2.0F);
    mouse_to(*scene.view, canvas.min_x + 2.0F, canvas.min_y + 2.0F);
    mouse_button(*scene.view, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    settle(*scene.view);
    CHECK_FALSE(scene.view->item_rects().contains("map_connect_related"));
    CHECK(scene.fixture.read_note("d.md") == "ilgisiz\n");
    CHECK(scene.state->current_path()->string() == "a.md");
}

TEST_CASE("map: Shift+dragging between a note and a tag adds the tag to the note") {
    MapScene scene;
    scene.open();
    scene.state->set_map_options(MapOptions{.whole_graph = true, .show_tags = true});
    scene.settle_layout();
    shift_drag(*scene.view, "map:#proje", "map:d.md");  // etiketten nota da aynı
    CHECK(scene.fixture.read_note("d.md") == "ilgisiz\n#proje\n");
    CHECK(scene.state->status() == "Tag #proje added to d.md");
    CHECK(has_map_edge(*scene.state, "d.md", "proje", noteapp::gui::MapEdgeKind::Tag));
}

TEST_CASE("map: right-clicking an edge removes a relation; a link edge's item is disabled") {
    MapScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("d.md"), np("e.md")).has_value());
    }
    scene.open();
    const auto [rx, ry] = edge_middle(*scene.view, "map:d.md", "map:e.md");
    right_click(*scene.view, rx, ry);
    REQUIRE(scene.view->item_rects().contains("map_edge_remove"));
    CHECK_FALSE(scene.view->item_rects().at("map_edge_remove").disabled);
    click_button(*scene.view, "map_edge_remove");
    CHECK(scene.state->status() == "Relation removed: d.md and e.md");
    CHECK(core::NoteStore(scene.fixture.store_dir()).links(np("d.md")).value().related.empty());
    CHECK_FALSE(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));

    scene.settle_layout();
    const auto [lx, ly] = edge_middle(*scene.view, "map:a.md", "map:b.md");
    right_click(*scene.view, lx, ly);
    REQUIRE(scene.view->item_rects().contains("map_edge_remove"));
    CHECK(scene.view->item_rects().at("map_edge_remove").disabled);
    click_button(*scene.view, "map_edge_remove");  // kapalı öğe: metin düzenlenmez
    CHECK(scene.fixture.read_note("a.md") == "[[b]] [[yok]] #proje\n");
    CHECK(has_map_edge(*scene.state, "a.md", "b.md", noteapp::gui::MapEdgeKind::Link));
}

TEST_CASE("map: right-clicking a tag edge removes the tag from the note's text") {
    MapScene scene;
    scene.open();
    scene.state->set_map_options(MapOptions{.whole_graph = true, .show_tags = true});
    scene.settle_layout();
    const auto [x, y] = edge_middle(*scene.view, "map:e.md", "map:#proje");
    right_click(*scene.view, x, y);
    REQUIRE(scene.view->item_rects().contains("map_edge_remove"));
    click_button(*scene.view, "map_edge_remove");
    CHECK(scene.fixture.read_note("e.md") == "proje ayrı\n");
    CHECK(scene.state->status() == "Tag #proje removed from e.md");
    CHECK_FALSE(has_map_edge(*scene.state, "e.md", "proje", noteapp::gui::MapEdgeKind::Tag));
}

// G13: düğüm bağlantı sayısıyla büyür (en çok iki kat); denetimler açma/kapama çipi ve adımlı derinlik.
TEST_CASE("map: a node with more links is drawn larger and the controls are a chip and a depth stepper") {
    MapScene scene;
    scene.open();
    const auto width = [&](const std::string& name) {
        const View::ItemRect r = scene.rect(name);
        return r.max_x - r.min_x;
    };
    // a.md: b'ye bağlantı, c'den bağlantı, #proje etiketi; d.md'nin bağlantısı yok.
    CHECK(width("map:a.md") > width("map:d.md") * 1.2F);
    CHECK(width("map:a.md") <= width("map:d.md") * 2.0F + 1.0F);

    MapOptions options = scene.state->map_options();
    options.whole_graph = false;
    options.depth = 1;
    scene.state->set_map_options(options);
    scene.settle_layout();
    REQUIRE(scene.view->item_rects().contains("map_depth_more"));
    click_button(*scene.view, "map_depth_more");
    CHECK(scene.state->map_options().depth == 2);
    click_button(*scene.view, "map_depth_less");
    CHECK(scene.state->map_options().depth == 1);
    CHECK(scene.view->item_rects().at("map_depth_less").disabled);  // en küçük derinlikte
    click_button(*scene.view, "map_whole");
    CHECK(scene.state->map_options().whole_graph);
}

// NX-B (L1): adımlayıcının üstü limits.app.map_depth (TUI derinlik tuşlarıyla aynı model sınırı).
TEST_CASE("map: the depth stepper stops at limits.app.map_depth and 0 lets it go past 6") {
    MapScene scene;
    MapOptions options;
    SUBCASE("a limit of 3") {
        scene.fixture.config.limits.app.map_depth = {3, "3"};
        scene.open(false);
        options = scene.state->map_options();
        options.depth = 5;
        scene.state->set_map_options(options);
        CHECK(scene.state->map_options().depth == 3);  // sınıra iner
        scene.settle_layout();
        CHECK(scene.view->item_rects().at("map_depth_more").disabled);
    }
    SUBCASE("unlimited") {
        scene.fixture.config.limits.app.map_depth = {0, "0"};
        scene.open(false);
        options = scene.state->map_options();
        options.depth = 6;
        scene.state->set_map_options(options);
        scene.settle_layout();
        REQUIRE_FALSE(scene.view->item_rects().at("map_depth_more").disabled);
        click_button(*scene.view, "map_depth_more");
        CHECK(scene.state->map_options().depth == 7);
    }
}

// G13: büyüyen düğüm çizildiği boyda tıklanır; kenarına yakın tık da o notu açar.
TEST_CASE("map: a click near the rim of a node grown by its links opens that note") {
    MapScene scene;
    scene.open();
    scene.state->open_note(np("d.md"));
    scene.settle_layout();
    const View::ItemRect node = scene.rect("map:a.md");
    const float radius = (node.max_x - node.min_x) / 2.0F;
    const float x =
        ((node.min_x + node.max_x) / 2.0F) + (0.9F * radius);  // tabanın ötesi, çizilen dairenin içi
    const float y = (node.min_y + node.max_y) / 2.0F;
    ImGui::GetIO().AddMousePosEvent(x, y);
    scene.view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    scene.view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    settle(*scene.view);
    REQUIRE(scene.state->current_path());
    CHECK(scene.state->current_path()->string() == "a.md");
}

// İnceleme 1: etiket küçük karedir (0,7 pay); erişimi de aynı payla küçülür. Uzakta (en küçük ekran boyunda)
// kare yarıçapının ötesine düşen fare, notun erişim payı kadar uzakta olsa da etiketi yakalamaz.
TEST_CASE("map: a tag is hovered within its drawn square, not within a note's reach") {
    MapScene scene;
    scene.open();
    scene.state->open_note(np("d.md"));  // gösterilen notun başlığında #proje yazmasın: metin ipucundan gelir
    scene.state->set_map_options(MapOptions{.whole_graph = true, .show_tags = true});
    scene.settle_layout();
    REQUIRE(scene.view->item_rects().contains("map:#proje"));
    const float font = ImGui::GetFontSize();
    const float smallest = 0.25F * font * 0.7F;  // etiketin en küçük çizilen yarıçapı
    const auto half = [&] {
        const View::ItemRect r = scene.rect("map:#proje");
        return (r.max_x - r.min_x) / 2.0F;
    };
    // Kare en küçük boyuna inene dek uzaklaşılır (tekerlek tuvalin ortasında).
    const View::ItemRect canvas = scene.rect("map_canvas");
    ImGui::GetIO().AddMousePosEvent((canvas.min_x + canvas.max_x) / 2.0F,
                                    (canvas.min_y + canvas.max_y) / 2.0F);
    scene.view->draw_frame();
    for (int notch = 0; notch < 40 && half() > smallest + 0.5F; ++notch) {
        ImGui::GetIO().AddMouseWheelEvent(0.0F, -1.0F);
        scene.view->draw_frame();
        scene.view->draw_frame();
    }
    REQUIRE(half() <= smallest + 0.5F);
    const View::ItemRect tag = scene.rect("map:#proje");
    const float cx = (tag.min_x + tag.max_x) / 2.0F;
    const float cy = (tag.min_y + tag.max_y) / 2.0F;
    // Komşu düğümlerden uzak yön: dört köşegenden en uzağı.
    float best_dx = 1.0F;
    float best_dy = 0.0F;
    float best_room = -1.0F;
    for (const auto& [dx, dy] :
         {std::pair{1.0F, 0.0F}, std::pair{-1.0F, 0.0F}, std::pair{0.0F, 1.0F}, std::pair{0.0F, -1.0F}}) {
        float room = std::numeric_limits<float>::max();
        for (const auto& [name, rect] : scene.view->item_rects()) {
            if (!name.starts_with("map:") || name == "map:#proje")
                continue;
            const float ox = ((rect.min_x + rect.max_x) / 2.0F) - (cx + (dx * font));
            const float oy = ((rect.min_y + rect.max_y) / 2.0F) - (cy + (dy * font));
            room = std::min(room, std::hypot(ox, oy));
        }
        if (room > best_room) {
            best_room = room;
            best_dx = dx;
            best_dy = dy;
        }
    }
    const auto hovered_at = [&](float distance) {
        const gui_test::RenderedText rendered;
        ImGui::GetIO().AddMousePosEvent(cx + (best_dx * distance), cy + (best_dy * distance));
        scene.view->draw_frame();
        scene.view->draw_frame();
        return rendered.contains("#proje");
    };
    CHECK(hovered_at(0.0F));                // karenin ortası
    CHECK_FALSE(hovered_at(0.45F * font));  // notun erişimi (0,5 em) içinde, etiketinkinin dışında
}

// NY-G: çevre görünümünde düğüm seçmek merkezi değiştirir. a'nın ve b'nin iki adımlık çevresi aynı üç nottur;
// önceden yalnız sıra değiştiği için harita yeniden kuruluyor, yerleşim ılınıyor ve düğümler kare başına
// 30–450 px sıçrıyordu (titreme). Seçimden sonra hiçbir düğüm kıpırdamaz.
TEST_CASE("map: selecting a node in the same neighbourhood leaves every node still") {
    MapScene scene;
    scene.open(false);
    const std::size_t generation = scene.state->map_generation();
    const auto [x, y] = scene.view->button_center("map:b.md");
    mouse_to(*scene.view, x, y);
    mouse_button(*scene.view, true);
    mouse_button(*scene.view, false);
    REQUIRE(scene.state->current_path());
    REQUIRE(scene.state->current_path()->string() == "b.md");
    const auto before = scene.view->item_rects();
    constexpr int kWatchedFrames = 20;
    for (int frame = 0; frame < kWatchedFrames; ++frame) {
        scene.view->draw_frame();
        CHECK(scene.state->map_layout().settled());
        for (const std::string name : {"map:a.md", "map:b.md", "map:c.md"}) {
            CHECK(scene.rect(name).min_x == before.at(name).min_x);
            CHECK(scene.rect(name).min_y == before.at(name).min_y);
        }
    }
    CHECK(scene.state->map_generation() == generation);
}

// NY-G: ilişki kurmak keşfedilir olsun. Düğüme sağ tık menüsü "Bağla…" kipini açar, sıradaki tıklanan düğüm
// hedeftir (Shift+sürüklemenin bırakmasıyla aynı kural); denetimlerin altında yolu söyleyen ipucu.
TEST_CASE("map: right-click Connect… then a click on another note asks Related or Link") {
    MapScene scene;
    scene.open();
    {  // ImGui günlüğü tek: iki RenderedText aynı anda yaşamaz
        const gui_test::RenderedText hint;
        settle(*scene.view);
        CHECK(hint.contains(core::tr("map.connect_hint")));
    }
    const auto [x, y] = scene.view->button_center("map:d.md");
    right_click(*scene.view, x, y);
    REQUIRE(scene.view->item_rects().contains("map_node_connect"));
    click_button(*scene.view, "map_node_connect");
    {
        const gui_test::RenderedText pending;
        settle(*scene.view);
        CHECK(pending.contains(core::tr("map.connect_pending", {{"from", "d.md"}})));
    }
    click_button(*scene.view, "map:e.md");
    REQUIRE(scene.view->item_rects().contains("map_connect_related"));
    CHECK(scene.state->current_path()->string() == "a.md");  // bağlama tıklaması notu açmaz
    scene.view->install_deciders();
    ModalSteps accept{{press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(accept);
    click_button(*scene.view, "map_connect_related");
    CHECK(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
}

TEST_CASE("map: Esc leaves connect mode; the next click opens the note as usual") {
    MapScene scene;
    scene.open();
    const auto [x, y] = scene.view->button_center("map:d.md");
    right_click(*scene.view, x, y);
    click_button(*scene.view, "map_node_connect");
    press(*scene.view, ImGuiKey_Escape);
    click_button(*scene.view, "map:e.md");
    CHECK_FALSE(scene.view->item_rects().contains("map_connect_related"));
    CHECK(scene.state->current_path()->string() == "e.md");
}

TEST_CASE("map: the node menu removes the node's relation") {
    MapScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("d.md"), np("e.md")).has_value());
    }
    scene.open();
    REQUIRE(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
    const auto [x, y] = scene.view->button_center("map:d.md");
    right_click(*scene.view, x, y);
    REQUIRE(scene.view->item_rects().contains("map_node_remove"));
    const auto [mx, my] = scene.view->button_center("map_node_remove");
    mouse_to(*scene.view, mx, my);  // alt menü üzerine gelince açılır
    settle(*scene.view);
    REQUIRE(scene.view->item_rects().contains("map_node_remove:e.md"));
    click_button(*scene.view, "map_node_remove:e.md");
    CHECK_FALSE(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
}

TEST_CASE("map: C on the focused canvas connects from the shown note") {
    MapScene scene;
    scene.open();
    focus_canvas(scene);
    press(*scene.view, ImGuiKey_C);
    click_button(*scene.view, "map:d.md");
    REQUIRE(scene.view->item_rects().contains("map_connect_related"));
    scene.view->install_deciders();
    ModalSteps accept{{press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(accept);
    click_button(*scene.view, "map_connect_related");
    CHECK(has_map_edge(*scene.state, "a.md", "d.md", noteapp::gui::MapEdgeKind::Related));
}

// İnceleme bulgusu: menü açıkken çizge yeniden kurulursa dizin başka düğümü gösterir; Bağla… o zaman kip
// açmaz.
TEST_CASE("map: Connect… from a menu opened before the map rebuilt does nothing") {
    MapScene scene;
    scene.open();
    const auto [x, y] = scene.view->button_center("map:d.md");
    right_click(*scene.view, x, y);
    REQUIRE(scene.view->item_rects().contains("map_node_connect"));
    const std::size_t generation = scene.state->map_generation();
    scene.state->set_map_options(MapOptions{.whole_graph = false, .depth = 1});  // çevre: a, b, c
    REQUIRE(scene.state->map_generation() != generation);
    click_button(*scene.view, "map_node_connect");
    const gui_test::RenderedText rendered;
    settle(*scene.view);
    CHECK_FALSE(rendered.contains(core::tr("map.connect_pending", {{"from", "d.md"}})));
    CHECK(rendered.contains(core::tr("map.connect_hint")));
}

// F1: "İlişkili" ad penceresini açar. Liste var olan adları sayılarıyla süzer, ↓ Enter seçer; seçilen adın
// "Yönlü" varsayılanı adın alışkanlığıdır.
TEST_CASE("map: Related asks a name; the list filters known names, Down and Enter take one") {
    MapScene scene;
    put_relation(scene, "b.md", "c.md", {"uses", true});
    put_relation(scene, "a.md", "c.md", {"cites", false});
    scene.open();
    scene.view->install_deciders();
    const View& view = *scene.view;
    bool listed_both = false;
    bool filtered = false;
    ModalSteps steps{{[&] {
                          listed_both = view.item_rects().contains("relation_row:uses") &&
                                        view.item_rects().contains("relation_row:cites");
                      },
                      type("u"),
                      [&] {
                          filtered = view.item_rects().contains("relation_row:uses") &&
                                     !view.item_rects().contains("relation_row:cites");
                      },
                      press_key(ImGuiKey_DownArrow), press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(steps);
    shift_drag(*scene.view, "map:d.md", "map:e.md");
    click_button(*scene.view, "map_connect_related");
    CHECK(listed_both);
    CHECK(filtered);
    const auto rows = related_of(scene, "d.md");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].path == np("e.md"));
    CHECK(rows[0].name == "uses");
    CHECK(rows[0].direction == core::RelationDirection::Outgoing);  // "uses" çoğunlukla yönlü
    CHECK(scene.state->status() == "Related as 'uses': d.md and e.md");
    REQUIRE_FALSE(scene.state->notices().items().empty());
    CHECK(scene.state->notices().items().back().text == "Related as 'uses': d.md and e.md");
}

TEST_CASE("map: the name dialog takes a new name and the Directed box; a click on a listed name fills it") {
    MapScene scene;
    put_relation(scene, "b.md", "c.md", {"cites", false});
    scene.open();
    scene.view->install_deciders();
    const View& view = *scene.view;
    bool shows_new = false;
    ModalSteps fresh{{type("parent of"), [&] { shows_new = view.item_rects().contains("relation_new"); },
                      click(view, "relation_directed"), click(view, "dialog_ok")}};
    scene.view->on_modal_frame = std::ref(fresh);
    shift_drag(*scene.view, "map:d.md", "map:e.md");
    click_button(*scene.view, "map_connect_related");
    CHECK(shows_new);
    INFO(scene.state->status());
    auto rows = related_of(scene, "d.md");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "parent of");
    CHECK(rows[0].direction == core::RelationDirection::Outgoing);

    scene.settle_layout();
    // Tıklama alanı odağa geri çağırır; odak bir karede oturur, Enter ondan sonra.
    ModalSteps listed{{click(view, "relation_row:cites"), [] {}, press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(listed);
    shift_drag(*scene.view, "map:a.md", "map:d.md");
    click_button(*scene.view, "map_connect_related");
    rows = related_of(scene, "a.md");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "cites");
    CHECK(rows[0].direction == core::RelationDirection::Undirected);
}

TEST_CASE("map: Esc in the name dialog writes nothing; a refused name is reported") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"uses", false});
    scene.open();
    scene.view->install_deciders();
    ModalSteps cancel{{type("x"), press_key(ImGuiKey_Escape)}};
    scene.view->on_modal_frame = std::ref(cancel);
    shift_drag(*scene.view, "map:a.md", "map:d.md");
    click_button(*scene.view, "map_connect_related");
    CHECK(related_of(scene, "a.md").empty());

    scene.settle_layout();
    ModalSteps duplicate{{type("USES"), press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(duplicate);
    shift_drag(*scene.view, "map:e.md", "map:d.md");
    click_button(*scene.view, "map_connect_related");
    CHECK(related_of(scene, "d.md").size() == 1);
    REQUIRE_FALSE(scene.state->notices().items().empty());
    CHECK(scene.state->notices().items().back().kind == noteapp::app::NoticeKind::Error);
}

TEST_CASE("map: the edge menu renames, directs, flips, undirects and removes a relation by its id") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"uses", false});
    scene.open();
    scene.view->install_deciders();
    const auto menu = [&] {
        scene.settle_layout();
        const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
        right_click(*scene.view, x, y);
    };
    const auto relation = [&] {
        const auto rows = related_of(scene, "d.md");
        REQUIRE(rows.size() == 1);
        return rows[0];
    };
    menu();
    REQUIRE(scene.view->item_rects().contains("map_edge_rename"));
    CHECK_FALSE(scene.view->item_rects().contains("map_edge_flip"));  // yönsüz: çevrilecek yön yok
    click_button(*scene.view, "map_edge_direct");
    CHECK(relation().direction == core::RelationDirection::Outgoing);
    CHECK(scene.state->status() == "Relation directed: d.md → e.md");

    menu();
    click_button(*scene.view, "map_edge_flip");
    CHECK(relation().direction == core::RelationDirection::Incoming);
    CHECK(scene.state->status() == "Relation directed: e.md → d.md");

    menu();
    click_button(*scene.view, "map_edge_undirect");
    CHECK(relation().direction == core::RelationDirection::Undirected);

    menu();
    ModalSteps rename{{press_key(ImGuiKey_Backspace), press_key(ImGuiKey_Backspace), press_key(ImGuiKey_Backspace),
                       press_key(ImGuiKey_Backspace), type("cites"), press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(rename);
    click_button(*scene.view, "map_edge_rename");
    CHECK(rename.frame > 0);
    CHECK(relation().name == "cites");
    CHECK(scene.state->status() == "Relation renamed to 'cites': d.md and e.md");

    menu();
    click_button(*scene.view, "map_edge_remove");
    CHECK(related_of(scene, "d.md").empty());
    CHECK(scene.state->status() == "Relation 'cites' removed: d.md and e.md");
}

TEST_CASE("map: a relation's name is drawn on its edge only when zoomed in enough; hover always tells it") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"uses", true});
    scene.open();
    const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
    wheel_at(*scene.view, x, y, 4.0F);
    CHECK(scene.view->item_rects().contains("map_edge_label:uses"));
    {
        const gui_test::RenderedText rendered;
        mouse_to(*scene.view, x, y);
        settle(*scene.view);
        CHECK(rendered.contains("uses"));
        CHECK(rendered.contains("d.md → e.md"));
    }
    wheel_at(*scene.view, x, y, -16.0F);
    CHECK_FALSE(scene.view->item_rects().contains("map_edge_label:uses"));
}

TEST_CASE("map: a hostile relation name is drawn without control or hidden characters") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"x", false});
    {
        auto raw = noteapp::graph::Graph::open(scene.fixture.store_dir() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH ()-[r:RELATED_TO]->() SET r.name = $name",
                           {{"name", noteapp::graph::QueryValue(std::string("ab\xE2\x80\xAE" "cd"))}},
                           noteapp::graph::QueryMode::ReadWrite)
                    .has_value());
    }
    scene.open();
    const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
    wheel_at(*scene.view, x, y, 4.0F);
    std::vector<std::string> labels;
    for (const auto& [name, rect] : scene.view->item_rects())
        if (name.starts_with("map_edge_label:"))
            labels.push_back(name);
    CHECK(labels == std::vector<std::string>{"map_edge_label:ab\xEF\xBF\xBD" "cd"});
}

// İnceleme (F1): kenar adı önbelleği yerleşim kuşağına bağlıydı; ad değişimi kuşağı artırmaz, eski ad kalırdı.
TEST_CASE("map: a renamed relation's label and tooltip show the new name") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"uses", false});
    scene.open();
    scene.view->install_deciders();
    const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
    wheel_at(*scene.view, x, y, 4.0F);
    REQUIRE(scene.view->item_rects().contains("map_edge_label:uses"));
    right_click(*scene.view, x, y);
    ModalSteps rename{{press_key(ImGuiKey_Backspace), press_key(ImGuiKey_Backspace), press_key(ImGuiKey_Backspace),
                       press_key(ImGuiKey_Backspace), type("cites"), press_key(ImGuiKey_Enter)}};
    scene.view->on_modal_frame = std::ref(rename);
    click_button(*scene.view, "map_edge_rename");
    REQUIRE(related_of(scene, "d.md").at(0).name == "cites");
    settle(*scene.view);
    CHECK(scene.view->item_rects().contains("map_edge_label:cites"));
    CHECK_FALSE(scene.view->item_rects().contains("map_edge_label:uses"));
    const gui_test::RenderedText rendered;
    mouse_to(*scene.view, x, y);
    settle(*scene.view);
    CHECK(rendered.contains("cites"));
    CHECK_FALSE(rendered.contains("uses"));
}

// İnceleme (güvenlik): ImGui etiketteki `##`i kimlik ayırıcısı sayar; senkronla gelen ad menüde gerçek yolu
// gizleyip yanlış ilişkiyi sildirebilirdi. Ad ve yoldaki `#` görünür işarete (U+FF03) döner.
TEST_CASE("map: a # in a relation name cannot cut the menu label that names the relation") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"a##b", false});
    scene.open();
    {
        const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
        const gui_test::RenderedText rendered;
        right_click(*scene.view, x, y);
        REQUIRE(scene.view->item_rects().contains("map_edge_remove"));
        CHECK(rendered.contains("a\xEF\xBC\x83\xEF\xBC\x83" "b"));
        press(*scene.view, ImGuiKey_Escape);
    }
    const auto [nx, ny] = scene.view->button_center("map:d.md");
    right_click(*scene.view, nx, ny);
    // Alt menünün metni ImGui günlüğüne düşmez: öğenin varlığı ve kaldırdığı ilişki denetlenir.
    const auto [mx, my] = scene.view->button_center("map_node_remove");
    mouse_to(*scene.view, mx, my);
    settle(*scene.view);
    REQUIRE(scene.view->item_rects().contains("map_node_remove:e.md:a##b"));
    click_button(*scene.view, "map_node_remove:e.md:a##b");
    CHECK(related_of(scene, "d.md").empty());
}

// İnceleme (eşitlik): "Yönlü yap" gösterilen not uçlardan biriyse ondan başlar (TUI ve bağlantı paneli gibi).
TEST_CASE("map: Make directed starts at the shown note when it is an end of the relation") {
    MapScene scene;
    put_relation(scene, "d.md", "e.md", {"uses", false});
    scene.open();
    scene.state->open_note(np("e.md"));
    scene.settle_layout();
    const auto [x, y] = edge_middle(*scene.view, "map:d.md", "map:e.md");
    {
        const gui_test::RenderedText rendered;
        right_click(*scene.view, x, y);
        CHECK(rendered.contains("e.md → d.md"));
    }
    click_button(*scene.view, "map_edge_direct");
    CHECK(related_of(scene, "d.md").at(0).direction == core::RelationDirection::Incoming);
    CHECK(scene.state->status() == "Relation directed: e.md → d.md");
}

// İnceleme (eşitlik): geçersiz ad pencereyi kapatıp metni yitirmez; neden pencerede kalır (TUI gibi).
TEST_CASE("map: an invalid relation name keeps the dialog open with the reason") {
    MapScene scene;
    scene.fixture.config.limits.graph.relation_name_bytes = core::Limit{4, "4"};
    scene.open();
    scene.view->install_deciders();
    const View& view = *scene.view;
    bool hidden_refused = false;
    bool long_refused = false;
    bool cleared_on_edit = false;
    // Ret alanı yeniden odaklar (metin seçili): ilk Backspace metni siler, hata kalkar.
    ModalSteps steps{{type("a\xE2\x80\xAE" "b"), press_key(ImGuiKey_Enter),
                      [&] { hidden_refused = view.item_rects().contains("relation_error"); },
                      press_key(ImGuiKey_Backspace),
                      [&] { cleared_on_edit = !view.item_rects().contains("relation_error"); },
                      press_key(ImGuiKey_Backspace), press_key(ImGuiKey_Backspace),
                      type("abcdef"), press_key(ImGuiKey_Enter),
                      [&] { long_refused = view.item_rects().contains("relation_error"); },
                      press_key(ImGuiKey_Escape)}};
    scene.view->on_modal_frame = std::ref(steps);
    shift_drag(*scene.view, "map:d.md", "map:e.md");
    click_button(*scene.view, "map_connect_related");
    CHECK(hidden_refused);
    CHECK(cleared_on_edit);
    CHECK(long_refused);
    CHECK(steps.frame < 200);  // Esc'e dek açık kaldı, vazgeçme zaman aşımından değil
    CHECK(related_of(scene, "d.md").empty());
}

// ---- F2: keşif ve düğüm işlemleri (plan-graph-map). Kural app::MapModel'de (tests/app/map_explore_test);
// burada GUI'nin menüleri, tuşları ve fareyle yolu. ----

namespace {

std::size_t node_index(GuiState& state, const std::string& key) {
    const auto& nodes = state.note_map().nodes;
    const auto found = std::ranges::find(nodes, key, &noteapp::app::MapNode::key);
    REQUIRE(found != nodes.end());
    return static_cast<std::size_t>(found - nodes.begin());
}

bool on_map(GuiState& state, const std::string& key) {
    return std::ranges::any_of(state.note_map().nodes, [&](const auto& node) { return node.key == key; });
}

void open_submenu(View& view, const std::string& item) {
    const auto [x, y] = view.button_center(item);
    mouse_to(view, x, y);  // alt menü üzerine gelince açılır
    settle(view);
}

// Tuvalin düğümsüz köşesine sağ tık: tuval menüsü.
void right_click_canvas(MapScene& scene) {
    const View::ItemRect canvas = scene.rect("map_canvas");
    right_click(*scene.view, canvas.min_x + 3.0F, canvas.min_y + 3.0F);
}

void right_click_node(MapScene& scene, const std::string& key) {
    const auto [x, y] = scene.view->button_center("map:" + key);
    right_click(*scene.view, x, y);
}

}  // namespace

TEST_CASE("map: the node menu opens neighbours by kind past the depth, closes them, and Open opens the note") {
    MapScene scene;
    scene.open(false);
    scene.state->open_note(np("b.md"));
    scene.state->set_map_options(MapOptions{.depth = 1});
    scene.settle_layout();
    REQUIRE_FALSE(on_map(*scene.state, "c.md"));  // b'den bir adım: a, b
    right_click_node(scene, "a.md");
    open_submenu(*scene.view, "map_node_expand");
    REQUIRE(scene.view->item_rects().contains("map_expand_links"));
    click_button(*scene.view, "map_expand_links");
    scene.settle_layout();
    CHECK(on_map(*scene.state, "c.md"));
    CHECK(scene.state->map_scope().expanded.at("a.md").kind == noteapp::app::MapEdgeKind::Link);

    right_click_node(scene, "a.md");
    click_button(*scene.view, "map_node_collapse");
    scene.settle_layout();
    CHECK_FALSE(on_map(*scene.state, "c.md"));
    CHECK(on_map(*scene.state, "b.md"));  // açılmış notu kapatmak yalnız açılışın eklediğini geri alır
    CHECK_FALSE(scene.state->map_scope().expanded.contains("a.md"));
    CHECK_FALSE(scene.state->map_scope().collapsed.contains("a.md"));

    right_click_node(scene, "a.md");
    click_button(*scene.view, "map_node_open");
    CHECK(scene.state->current_path()->string() == "a.md");
}

TEST_CASE("map: Pin in the node menu and P on the canvas pin a node; U and the canvas menu unpin all") {
    MapScene scene;
    scene.open();
    right_click_node(scene, "d.md");
    click_button(*scene.view, "map_node_pin");
    CHECK(scene.state->map_layout().pinned(node_index(*scene.state, "d.md")));
    right_click_canvas(scene);
    REQUIRE(scene.view->item_rects().contains("map_canvas_unpin_all"));
    click_button(*scene.view, "map_canvas_unpin_all");
    CHECK_FALSE(scene.state->map_layout().has_pins());

    focus_canvas(scene);
    const auto [x, y] = scene.view->button_center("map:e.md");
    mouse_to(*scene.view, x, y);
    press(*scene.view, ImGuiKey_P);
    CHECK(scene.state->map_layout().pinned(node_index(*scene.state, "e.md")));
    press(*scene.view, ImGuiKey_U);
    CHECK_FALSE(scene.state->map_layout().has_pins());
}

TEST_CASE("map: Shortest path… and a click on the end show the path and its length; no path is said; Esc clears") {
    MapScene scene;
    scene.open();
    right_click_node(scene, "c.md");
    click_button(*scene.view, "map_node_path");
    click_button(*scene.view, "map:b.md");
    CHECK(scene.state->current_path()->string() == "a.md");  // yol tıklaması notu açmaz
    CHECK(scene.state->status() == "Shortest path c.md → b.md: length 2");
    {
        const auto& map = scene.state->note_map();
        REQUIRE(map.path.size() == 3);
        CHECK(map.nodes[map.path[1]].key == "a.md");
    }

    right_click_node(scene, "b.md");
    click_button(*scene.view, "map_node_path_directed");
    click_button(*scene.view, "map:c.md");
    CHECK(scene.state->status() == "No directed path from b.md to c.md");
    CHECK(scene.state->note_map().path.empty());

    right_click_node(scene, "d.md");
    click_button(*scene.view, "map_node_path");
    click_button(*scene.view, "map:b.md");
    CHECK(scene.state->status() == "No path from d.md to b.md");

    right_click_node(scene, "c.md");
    click_button(*scene.view, "map_node_path");
    click_button(*scene.view, "map:b.md");
    REQUIRE(scene.state->map_scope().path);
    focus_canvas(scene);
    press(*scene.view, ImGuiKey_Escape);
    CHECK_FALSE(scene.state->map_scope().path);
}

TEST_CASE("map: the link and relation chips and the relation picker filter edges; the tag box keeps tagged notes") {
    MapScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("d.md"), np("e.md"), {"uses", false}).has_value());
    }
    scene.open();
    REQUIRE(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
    click_button(*scene.view, "map_related");
    CHECK_FALSE(scene.state->map_options().show_related);
    CHECK_FALSE(has_map_edge(*scene.state, "d.md", "e.md", noteapp::gui::MapEdgeKind::Related));
    click_button(*scene.view, "map_related");
    click_button(*scene.view, "map_links");
    CHECK_FALSE(scene.state->map_options().show_links);
    CHECK_FALSE(has_map_edge(*scene.state, "a.md", "b.md", noteapp::gui::MapEdgeKind::Link));
    click_button(*scene.view, "map_links");

    click_button(*scene.view, "map_relation");
    REQUIRE(scene.view->item_rects().contains("map_relation:uses"));
    click_button(*scene.view, "map_relation:uses");
    CHECK(scene.state->map_options().relation == std::optional<std::string>("uses"));

    click_button(*scene.view, "map_tag");
    gui_test::type_text(*scene.view, "proje");
    settle(*scene.view);
    CHECK(scene.state->map_options().tag == "proje");
    CHECK(on_map(*scene.state, "e.md"));
    CHECK_FALSE(on_map(*scene.state, "d.md"));
    CHECK(on_map(*scene.state, "a.md"));  // gösterilen not
}

TEST_CASE("map: Ctrl+F on the canvas finds a node and centres it; Esc closes the find line") {
    MapScene scene;
    scene.open();
    focus_canvas(scene);
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_F);
    REQUIRE(scene.view->item_rects().contains("map_find"));
    gui_test::type_text(*scene.view, "e.md");
    scene.settle_layout();
    const View::ItemRect canvas = scene.rect("map_canvas");
    const auto [x, y] = scene.view->button_center("map:e.md");
    CHECK(x == doctest::Approx((canvas.min_x + canvas.max_x) / 2.0F).epsilon(0.02));
    CHECK(y == doctest::Approx((canvas.min_y + canvas.max_y) / 2.0F).epsilon(0.02));
    press(*scene.view, ImGuiKey_Escape);
    CHECK_FALSE(scene.view->item_rects().contains("map_find"));
}

TEST_CASE("map: New note here… makes the note with the quick-add line, keeps it on the map, pinned") {
    MapScene scene;
    scene.open(false);
    right_click_canvas(scene);
    click_button(*scene.view, "map_canvas_new_note");
    REQUIRE(scene.view->item_rects().contains("quick_add"));
    settle(*scene.view);
    gui_test::type_text(*scene.view, "yeni");
    CHECK(scene.state->quick_add_buffer() == "yeni");  // satır odakta açılır: yazı ona gider
    press(*scene.view, ImGuiKey_Enter);
    REQUIRE(scene.fixture.note_exists("yeni.md"));
    scene.settle_layout();
    REQUIRE(on_map(*scene.state, "yeni.md"));
    CHECK(scene.state->map_layout().pinned(node_index(*scene.state, "yeni.md")));
}

TEST_CASE("map: Move to trash in the node menu trashes that note; undo brings it back") {
    MapScene scene;
    scene.open();
    right_click_node(scene, "d.md");
    click_button(*scene.view, "map_node_trash");
    CHECK_FALSE(scene.fixture.note_exists("d.md"));
    CHECK_FALSE(on_map(*scene.state, "d.md"));
    CHECK(scene.state->current_path()->string() == "a.md");  // gösterilen not yerinde
    REQUIRE(scene.state->can_undo_trash());
    scene.state->undo_trash();
    CHECK(scene.fixture.note_exists("d.md"));
}
