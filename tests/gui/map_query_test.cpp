// F3: haritanın sorgu çubuğu (plan-graph-map). Akış ortak (app::MapQuery); burada GUI'nin çubuğu, geçmişi, onay
// penceresi, tablo görünümü ve haritadaki vurgu gerçek View ve ImGui olaylarıyla sınanır.
#include "core/i18n.hpp"
#include "core/note_query.hpp"
#include "core/note_views.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/note_seed.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <set>
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

// a.md -> b.md, c.md -> a.md, d.md ilgisiz.
struct QueryScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    QueryScene() {
        fixture.add_note("b.md", "b metni\n", 40);
        fixture.add_note("a.md", "[[b]] #proje\n", 30);
        fixture.add_note("c.md", "[[a]]\n", 20);
        fixture.add_note("d.md", "ilgisiz\n", 10);
    }
    void open() {
        state = fixture.make_state();
        view = make_view(*state);
        state->open_note(np("a.md"));
        state->set_browser_view(BrowserView::Map);
        MapOptions options;
        options.whole_graph = true;
        state->set_map_options(options);
        settle(*view);
    }
    bool has(const std::string& item) const { return view->item_rects().contains(item); }
    // Tuvalin düğümsüz köşesine tıklama odak verir.
    void focus_canvas() {
        const View::ItemRect canvas = view->item_rects().at("map_canvas");
        ImGui::GetIO().AddMousePosEvent(canvas.min_x + 2.0F, canvas.min_y + 2.0F);
        view->draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        view->draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        settle(*view);
    }
    void open_bar() {
        focus_canvas();
        press(*view, ImGuiKey_Q);
        REQUIRE(has("map_query"));
    }
    // Odaktaki çubuğa yazılır ve Enter: başarıda çubuk boşalır ve odakta kalır, hatada metin kalır.
    void run(const std::string& text) {
        gui_test::type_text(*view, text.c_str());
        press(*view, ImGuiKey_Enter);
        settle(*view);
    }
    bool lit(const std::string& key) {
        const auto& nodes = state->note_map().nodes;
        const auto found = std::ranges::find(nodes, key, &noteapp::app::MapNode::key);
        REQUIRE(found != nodes.end());
        const auto& mask = state->map_highlighted_nodes();
        const auto index = static_cast<std::size_t>(found - nodes.begin());
        REQUIRE(index < mask.size());
        return mask[index] != 0;
    }
    std::size_t count(std::string_view cypher) {
        core::NoteStore store(fixture.store_dir());
        const auto done = core::run_user_query(store, cypher, core::QueryAction::Read, {});
        REQUIRE(done.has_value());
        return done->result.rows.size();
    }
};

}  // namespace

TEST_CASE("map query: Q opens the bar; typing and Enter runs a read that lights its notes; closing clears it") {
    QueryScene scene;
    scene.open();
    CHECK_FALSE(scene.has("map_query"));
    CHECK(scene.has("map_query_toggle"));  // araç çubuğundaki çip
    scene.open_bar();
    scene.run("MATCH (n:Note) WHERE n.name = 'b' RETURN n");
    REQUIRE(scene.state->map_query().outcome().has_value());
    CHECK(scene.state->map_query().outcome()->highlight.notes == std::set<std::string>{"b.md"});
    CHECK(scene.lit("b.md"));
    CHECK_FALSE(scene.lit("a.md"));
    CHECK(scene.state->status() == scene.state->map_query().outcome()->summary);
    CHECK(scene.state->map_query_buffer().empty());

    // Ctrl+Enter de çalıştırır; Çalıştır düğmesi de.
    gui_test::type_text(*scene.view, "MATCH (n:Note) WHERE n.name = 'd' RETURN n");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_Enter);
    settle(*scene.view);
    CHECK(scene.state->map_query().outcome()->highlight.notes == std::set<std::string>{"d.md"});
    gui_test::type_text(*scene.view, "MATCH (n:Note) WHERE n.name = 'c' RETURN n");
    click_button(*scene.view, "map_query_run");
    CHECK(scene.state->map_query().outcome()->highlight.notes == std::set<std::string>{"c.md"});

    click_button(*scene.view, "map_query_toggle");
    CHECK_FALSE(scene.has("map_query"));
    CHECK(scene.state->map_highlighted_nodes().empty());
    CHECK_FALSE(scene.state->map_query().outcome().has_value());
}

TEST_CASE("map query: Up and Down walk the bar's history") {
    QueryScene scene;
    scene.open();
    scene.open_bar();
    scene.run("MATCH (n:Note) RETURN n");
    scene.run("MATCH (n:Note) WHERE n.name = 'a' RETURN n");
    REQUIRE(scene.state->map_query_buffer().empty());
    press(*scene.view, ImGuiKey_UpArrow);
    CHECK(scene.state->map_query_buffer() == "MATCH (n:Note) WHERE n.name = 'a' RETURN n");
    press(*scene.view, ImGuiKey_UpArrow);
    CHECK(scene.state->map_query_buffer() == "MATCH (n:Note) RETURN n");
    press(*scene.view, ImGuiKey_DownArrow);
    CHECK(scene.state->map_query_buffer() == "MATCH (n:Note) WHERE n.name = 'a' RETURN n");
}

TEST_CASE("map query: a write asks with its change count; Cancel writes nothing, Apply writes and redraws") {
    QueryScene scene;
    REQUIRE(core::NoteStore(scene.fixture.store_dir()).add_relation(np("a.md"), np("b.md"), {"p", false}));
    scene.open();
    std::vector<std::string> asked;
    bool answer = false;
    scene.state->deciders.confirm_query = [&](const std::string& message) {
        asked.push_back(message);
        return answer;
    };
    scene.open_bar();
    // İlişki ham Cypher'la kurulmaz (Bağla… ile kurulur); var olanın adı yazılır.
    const std::string relate = "MATCH ()-[r:RELATED_TO]->() SET r.name = 'q'";
    scene.run(relate);
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().find("1") != std::string::npos);  // "properties set: 1"
    CHECK(scene.count("MATCH ()-[r:RELATED_TO]->() WHERE r.name = 'q' RETURN r") == 0);

    // Vazgeçilen yazmanın metni çubukta kalır: Enter yeniden sorar.
    CHECK(scene.state->map_query_buffer() == relate);
    answer = true;
    press(*scene.view, ImGuiKey_Enter);
    settle(*scene.view);
    REQUIRE(asked.size() == 2);
    CHECK(scene.count("MATCH ()-[r:RELATED_TO]->() WHERE r.name = 'q' RETURN r") == 1);
    REQUIRE(scene.state->map_query().outcome().has_value());
    CHECK(scene.state->map_query().outcome()->wrote);
    const auto& map = scene.state->note_map();
    CHECK(std::ranges::any_of(map.edges, [&](const auto& edge) {
        return edge.kind == noteapp::app::MapEdgeKind::Related && edge.name == "q";
    }));

    // Korunan veri onay sormadan reddedilir, satırın altında konumlu hata.
    scene.run("MATCH (n:Note) SET n.body = 'x'");
    CHECK(asked.size() == 2);
    CHECK(scene.has("map_query_error"));
    CHECK(scene.state->map_query().error().find("body") != std::string::npos);
    CHECK(scene.fixture.read_note("b.md") == "b metni\n");
}

TEST_CASE("map query: a syntax error is shown under the bar and the last result stays") {
    QueryScene scene;
    scene.open();
    scene.open_bar();
    scene.run("MATCH (n:Note) RETURN n");
    REQUIRE(scene.state->map_query().outcome().has_value());
    scene.run("MATCH (n:Note RETURN n");
    CHECK(scene.has("map_query_error"));
    CHECK_FALSE(scene.state->map_query().error().empty());
    CHECK(scene.state->map_query().outcome().has_value());
    CHECK(scene.lit("a.md"));
}

TEST_CASE("map query: the table view lists rows virtualised, draws hostile text safely and opens a note") {
    QueryScene scene;
    for (int i = 0; i < 300; ++i)
        scene.fixture.add_note("many/n" + std::to_string(i) + ".md", "x\n", 100 + i);
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(core::run_user_query(store, "MATCH (n:Note {name: 'd'}) SET n.title = 'ab\\u202Ecd'",
                                     core::QueryAction::Write, {}));
    }
    scene.open();
    scene.open_bar();
    scene.run("MATCH (n:Note) RETURN n, n.title AS title ORDER BY n.name");
    click_button(*scene.view, "map_query_table");
    REQUIRE(scene.has("map_query_rows"));
    std::size_t drawn = 0;
    for (const auto& [name, rect] : scene.view->item_rects())
        if (name.starts_with("map_query_cell:"))
            ++drawn;
    CHECK(drawn > 0);
    CHECK(drawn < 2 * 304);  // görünen satırlar çizilir, hepsi değil

    // d.md satırı en üstte değil: tablo onu bulup gösterene dek kaydırılır.
    const gui_test::RenderedText rendered;
    scene.view->draw_frame();
    CHECK(rendered.contains("a.md"));
    CHECK_FALSE(rendered.contains("\xE2\x80\xAE"));

    click_button(*scene.view, "map_query_cell:0:0");  // a.md
    REQUIRE(scene.state->current_path().has_value());
    CHECK(scene.state->current_path()->string() == "a.md");
    click_button(*scene.view, "map_query_table");
    CHECK_FALSE(scene.has("map_query_rows"));
    CHECK(scene.has("map_canvas"));
}

// İnceleme (Medium): ImGui tablosu IMGUI_TABLE_MAX_COLUMNS'tan (512) az sütun alır (BeginTable assert'i
// `columns < 512`); sonuç daha geniş olabilir. Fazlası çizilmez, kaç sütunun gizlendiği söylenir. Sınır tam 512'de
// sınanır: 512 sütun da assert'e düşerdi.
TEST_CASE("map query: a result wider than an ImGui table draws what fits and says how many columns are hidden") {
    QueryScene scene;
    scene.open();
    scene.open_bar();
    std::string text = "MATCH (n:Note {name: 'a'}) RETURN ";
    for (int column = 0; column < 512; ++column)
        text += (column == 0 ? "" : ", ") + std::to_string(column) + " AS c" + std::to_string(column);
    scene.run(text);
    REQUIRE(scene.state->map_query().outcome().has_value());
    REQUIRE(scene.state->map_query().outcome()->table.columns.size() == 512);
    click_button(*scene.view, "map_query_table");
    CHECK(scene.has("map_query_rows"));
    CHECK(scene.has("map_query_hidden_columns"));
    const gui_test::RenderedText rendered;
    scene.view->draw_frame();
    CHECK(rendered.contains(core::tr("gui.map_query_columns_hidden", {{"count", "1"}, {"shown", "511"}})));
}

TEST_CASE("map query: a hostile value is drawn without hidden characters in the table") {
    QueryScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(core::run_user_query(store, "MATCH (n:Note {name: 'a'}) SET n.title = 'ab\\u202Ecd\\nef'",
                                     core::QueryAction::Write, {}));
    }
    scene.open();
    scene.open_bar();
    scene.run("MATCH (n:Note {name: 'a'}) RETURN n.title AS title");
    click_button(*scene.view, "map_query_table");
    const gui_test::RenderedText rendered;
    scene.view->draw_frame();
    CHECK(rendered.contains("ab\xEF\xBF\xBD" "cd\xEF\xBF\xBD" "ef"));
    CHECK_FALSE(rendered.contains("\xE2\x80\xAE"));
}

TEST_CASE("map query: the views list runs a saved view; typing a view's name runs it too") {
    QueryScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(core::save_view(store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    }
    scene.open();
    scene.open_bar();
    click_button(*scene.view, "map_query_views");
    REQUIRE(scene.has("map_query_view:linked"));
    click_button(*scene.view, "map_query_view:linked");
    REQUIRE(scene.state->map_query().outcome().has_value());
    CHECK(scene.state->map_query().outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md", "c.md"});
    scene.run("MATCH (n:Note) WHERE n.name = 'd' RETURN n");
    scene.run("Linked");
    CHECK(scene.state->map_query().outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md", "c.md"});
    // Yazılan metin görünüm olarak çözüldü: durum satırı bunu kayıtlı adıyla söyler.
    CHECK(scene.state->status() == core::tr("map.query_view", {{"name", "linked"}, {"rows", "2"}, {"notes", "3"}}));
}
