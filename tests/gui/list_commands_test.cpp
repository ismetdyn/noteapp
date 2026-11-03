// G2: hızlı ekleme satırı, Space/tümünü aç-kapa, Ayrıntılar sütunları, köke git ve çöp önizlemesi; TUI'deki
// komut kimlikleriyle (quick.add, toggle, tree.toggle_all, details.all, go.root, column*).
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::BrowserView;
using noteapp::gui::DetailsColumn;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;

namespace {

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

struct ListScene : WindowFixture {
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    ListScene() {
        add_note("a.md", "a\n", 20);
        add_note("work/x.md", "x\n", 30);
        add_note("work/deep/y.md", "y\n", 40);
        add_note("home/z.md", "z\n", 50);
        add_note(".trash/old.md", "eski metin\n", 60);
        state = make_state();
        view = make_view(*state);
        settle(*view);
    }
    void focus_list() const {
        state->request_focus(FocusRequest::List);
        settle(*view);
    }
    std::string state_text() const {
        std::ifstream in(home.path() / "state" / "gui-state.toml", std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

}  // namespace

TEST_CASE_FIXTURE(ListScene,
                  "A opens the quick add row; a name creates the note there, a trailing / a folder") {
    state->select_folder("work");
    focus_list();
    press(*view, ImGuiKey_A);
    REQUIRE(state->quick_add_open());
    settle(*view);
    REQUIRE(view->item_rects().contains("quick_add"));
    type_text(*view, "fikir");
    press(*view, ImGuiKey_Enter);
    CHECK_FALSE(state->quick_add_open());
    CHECK(note_exists("work/fikir.md"));
    REQUIRE(state->current_path().has_value());
    CHECK(state->current_path()->string() == "work/fikir.md");

    // Dolu ad -2 almaz: hata satırda kalır, satır açık.
    run_command(*state, "quick.add");
    state->quick_add_buffer() = "fikir";
    state->submit_quick_add();
    CHECK(state->quick_add_open());
    CHECK_FALSE(state->quick_add_error().empty());
    state->cancel_quick_add();

    state->select_folder("work");
    run_command(*state, "quick.add");
    state->quick_add_buffer() = "proje/";
    state->submit_quick_add();
    CHECK_FALSE(state->quick_add_open());
    REQUIRE(state->selected_folder().has_value());
    CHECK(*state->selected_folder() == "work/proje");
}

TEST_CASE_FIXTURE(ListScene,
                  "Space toggles a folder in place and the info panel on a note; Shift+Space all") {
    state->select_folder("work");
    focus_list();
    CHECK_FALSE(state->is_expanded("work"));
    press(*view, ImGuiKey_Space);
    CHECK(state->is_expanded("work"));
    press(*view, ImGuiKey_Space);
    CHECK_FALSE(state->is_expanded("work"));

    state->open_note(*noteapp::core::NotePath::parse("a.md"));
    focus_list();
    const bool info = state->info_open();
    press(*view, ImGuiKey_Space);
    CHECK(state->info_open() != info);

    press(*view, ImGuiMod_Shift | ImGuiKey_Space);
    CHECK(state->is_expanded("work"));
    CHECK(state->is_expanded("work/deep"));
    CHECK(state->is_expanded("home"));
    run_command(*state, "tree.toggle_all");
    CHECK_FALSE(state->is_expanded("work"));
    CHECK_FALSE(state->is_expanded("home"));
}

TEST_CASE_FIXTURE(ListScene,
                  "details.all switches between Tree and Details; go.root leaves trash, search and folder") {
    run_command(*state, "details.all");
    CHECK(state->browser_view() == BrowserView::Details);
    state->enter_folder("work/deep");
    state->set_query("x");
    state->open_trash();
    press(*view, ImGuiMod_Alt | ImGuiKey_Home);
    CHECK_FALSE(state->trash_view());
    CHECK(state->query().empty());
    CHECK(state->current_folder().empty());
    REQUIRE(state->selected_folder().has_value());
    CHECK(state->selected_folder()->empty());
    run_command(*state, "details.all");
    CHECK(state->browser_view() == BrowserView::Tree);
}

TEST_CASE_FIXTURE(ListScene,
                  "C cycles the Details columns like the TUI; the choice is saved with its TUI name") {
    state->set_browser_view(BrowserView::Details);
    state->enter_folder("");
    focus_list();
    CHECK(state->details_column() == DetailsColumn::SizeAndDate);
    CHECK(view->item_rects().contains("details_header:3"));  // ad; boyut, güncelleme, oluşturma
    press(*view, ImGuiKey_C);
    CHECK(state->details_column() == DetailsColumn::Size);
    settle(*view);
    CHECK(view->item_rects().contains("details_header:1"));
    CHECK_FALSE(view->item_rects().contains("details_header:2"));
    run_command(*state, "column.sync");
    settle(*view);
    CHECK(view->item_rects().contains("details_cell:1:a.md"));
    run_command(*state, "column.origin");
    CHECK(state_text().find("origin") != std::string::npos);
    run_command(*state, "column.none");
    settle(*view);
    CHECK(view->item_rects().contains("details_header:0"));  // yalnız ad
    CHECK_FALSE(view->item_rects().contains("details_header:1"));
    for (int step = 0; step < 7; ++step)
        run_command(*state, "column");  // döngü TUI sırasıyla başa döner
    CHECK(state->details_column() == DetailsColumn::None);

    auto reopened = make_state();
    CHECK(reopened->details_column() == DetailsColumn::None);
}

TEST_CASE_FIXTURE(ListScene, "the trash shows the selected item's text read-only in the right pane") {
    state->open_trash();
    state->select_trash_row(0);
    settle(*view);
    CHECK(state->trash_preview() == "eski metin\n");
    CHECK(view->item_rects().contains("trash_preview"));
}
