// G4 (matris 16–18): arama kutusu TUI'nin girdi satırıdır (tui::InputLine, SPEC-tui "Girdi satırı"): Enter'la
// kullanılan arama geçmişe girer (gui-state.toml, sır süzgeci), Ctrl+P/Ctrl+N geçmişte gezer, Tab argüman,
// etiket ve görünüm adaylarını tamamlar, gölge öneri → ile alınır, argümanlar renklidir.
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace {

struct SearchScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    SearchScene() {
        fixture.add_note("planlama.md", "plan #proje\n", 10);
        fixture.add_note("proje.md", "proje #plan\n", 20);
        fixture.add_note("gizli.md", "password=hunter2-abc\n", 30);
        restart();
    }
    void restart() {
        view.reset();
        state.reset();
        state = fixture.make_state();
        view = make_view(*state);
    }
    void type_in_search(const char* text) const {
        state->set_query({});
        state->focus_search();
        settle(*view);
        type_text(*view, text);
        settle(*view);
    }
};

}  // namespace

TEST_CASE("search line: Enter remembers a found query across restarts; Ctrl+P/Ctrl+N walk the history") {
    SearchScene scene;
    scene.type_in_search("plan");
    press(*scene.view, ImGuiKey_Enter);
    scene.type_in_search("proje");
    press(*scene.view, ImGuiKey_Enter);
    CHECK(scene.state->search_history() == std::vector<std::string>{"proje", "plan"});

    scene.restart();  // gui-state.toml'dan
    REQUIRE(scene.state->search_history() == std::vector<std::string>{"proje", "plan"});
    scene.type_in_search("pr");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_P);  // "pr"yi içeren önceki arama
    CHECK(scene.state->query() == "proje");
    scene.type_in_search("");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_P);
    CHECK(scene.state->query() == "proje");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_P);
    CHECK(scene.state->query() == "plan");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_N);
    CHECK(scene.state->query() == "proje");
    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_N);  // en yeninin ötesi: yazılan metin
    CHECK(scene.state->query().empty());
    CHECK_FALSE(scene.state->is_new_note());  // kutuda Ctrl+N geçmiştir, yeni not değil
    CHECK(scene.view->search_active());
}

TEST_CASE("search line: secret-looking, broken and unmatched queries stay out of the history") {
    SearchScene scene;
    scene.state->set_content_mode(true);
    scene.type_in_search("password=hunter2-abc");  // içerikte bulunur ama sır gibi
    REQUIRE(scene.state->current_path().has_value());
    press(*scene.view, ImGuiKey_Enter);
    scene.state->set_content_mode(false);
    scene.type_in_search("size:kocaman");  // bozuk argüman
    press(*scene.view, ImGuiKey_Enter);
    CHECK(scene.state->search_history().empty());

    // Elle değişmiş dosya: sır benzeri kayıt yüklenmez.
    scene.view.reset();
    scene.state.reset();
    const auto file = noteapp::core::state_dir() / "gui-state.toml";
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "search_history = [\"plan\", \"API_KEY=abc123\"]\n";
    }
    scene.restart();
    CHECK(scene.state->search_history() == std::vector<std::string>{"plan"});
}

TEST_CASE(
    "search line: Tab completes tags; with several candidates typing filters the list and Enter inserts") {
    SearchScene scene;
    scene.type_in_search("tag:pr");
    press(*scene.view, ImGuiKey_Tab);  // tek aday doğrudan yazılır
    CHECK(scene.state->query() == "tag:proje");
    CHECK_FALSE(scene.state->search_popup());
    CHECK(scene.view->search_active());

    scene.type_in_search("tag:");
    press(*scene.view, ImGuiKey_Tab);
    REQUIRE(scene.state->search_popup() != nullptr);
    CHECK(scene.state->search_popup()->items.size() == 2);
    CHECK(scene.view->item_rects().contains("search_popup"));
    type_text(*scene.view, "la");  // liste süzülür, metin değişmez
    CHECK(scene.state->query() == "tag:");
    REQUIRE(scene.state->search_popup() != nullptr);
    CHECK(scene.state->search_popup()->items == std::vector<std::string>{"tag:plan"});
    press(*scene.view, ImGuiKey_Enter);
    CHECK(scene.state->query() == "tag:plan");
    CHECK_FALSE(scene.state->search_popup());
    CHECK(scene.view->search_active());

    scene.type_in_search("tag:");
    press(*scene.view, ImGuiKey_Tab);
    REQUIRE(scene.state->search_popup() != nullptr);
    press(*scene.view, ImGuiKey_Escape);  // liste kapanır, metin kalır
    CHECK_FALSE(scene.state->search_popup());
    CHECK(scene.state->query() == "tag:");
}

TEST_CASE("search line: the ghost shows the rest of a note name and Right takes it") {
    SearchScene scene;
    scene.type_in_search("planl");
    CHECK(scene.state->search_ghost() == "ama.md");
    CHECK(scene.view->item_rects().contains("search_ghost"));
    press(*scene.view, ImGuiKey_RightArrow);
    CHECK(scene.state->query() == "planlama.md");
    CHECK(scene.state->search_ghost().empty());
}

TEST_CASE("search line: argument keys, values and broken values are coloured like the TUI") {
    SearchScene scene;
    scene.type_in_search("tag:proje size:kocaman plan");
    const auto& rects = scene.view->item_rects();
    CHECK(rects.contains("search_arg:key:0"));
    CHECK(rects.contains("search_arg:value:4"));
    CHECK(rects.contains("search_arg:key:10"));
    CHECK(rects.contains("search_arg:invalid:15"));
    CHECK_FALSE(rects.contains("search_arg:key:23"));  // düz metin boyanmaz
}

TEST_CASE("search line: Ctrl+Shift+M switches the search mode now that Tab completes") {
    SearchScene scene;
    scene.type_in_search("x");
    CHECK_FALSE(scene.state->content_mode());
    press(*scene.view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M);
    CHECK(scene.state->content_mode());
    CHECK(scene.view->search_active());
}
