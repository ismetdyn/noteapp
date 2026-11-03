#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "gui/browser_rows.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::core::tr;
using noteapp::gui::BrowserRow;
using noteapp::gui::BrowserRowKind;
using noteapp::gui::BrowserView;
using noteapp::gui::ConflictChoice;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;

namespace {

using Paths = std::vector<std::string>;

std::string current(const GuiState& state) {
    const auto path = state.current_path();
    return path ? path->string() : std::string{};
}

Paths row_paths(GuiState& state) {
    Paths paths;
    for (const BrowserRow& row : state.browser_rows())
        paths.push_back((row.kind == BrowserRowKind::Folder ? "d:" : "n:") + row.path);
    return paths;
}

bool has_item(const View& view, const std::string& name) {
    return view.item_rects().contains(name);
}

std::string selected_folder(const GuiState& state) {
    return state.selected_folder().value_or("<none>");
}

struct BrowserFixture : WindowFixture {
    std::optional<std::string> folder_answer;
    int conflicts = 0;

    void wire(GuiState& state) {
        state.deciders.folder_name = [this](const std::string&, const std::string&) { return folder_answer; };
        state.deciders.confirm_delete = [](const std::string&) { return true; };
    }
    bool dir_exists(const std::string& relative) const {
        return noteapp::test::folder_exists(store_dir(), relative);
    }
};

}  // namespace

TEST_CASE_FIXTURE(BrowserFixture, "the tree lists folders before notes, nested under the root") {
    add_note("zeta.md", "z", 10);
    add_note("work/b.md", "b", 20);
    add_note("alpha.md", "a", 30);
    add_note("home/c.md", "c", 40);
    auto state = make_state();
    // Karar 6: varsayılan sıra TUI'ninki, en yeni başta (klasörde alt ağacın en yeni notu).
    CHECK(row_paths(*state) == Paths{"d:", "d:work", "d:home", "n:zeta.md", "n:alpha.md"});
    const auto& rows = state->browser_rows();
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].depth == 0);
    CHECK(rows[1].depth == 1);
    CHECK(rows[1].name == "work");
    CHECK(rows[1].has_children);
    CHECK_FALSE(rows[1].expanded);
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "a nested note appears after expanding its folder and hides after collapsing") {
    add_note("newest.md", "n", 0);
    add_note("work/deep/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK_FALSE(has_item(*view, "note:work/deep/a.md"));

    state->set_expanded("work", true);
    settle(*view);
    CHECK(has_item(*view, "folder:work/deep"));
    CHECK_FALSE(has_item(*view, "note:work/deep/a.md"));
    state->set_expanded("work/deep", true);
    settle(*view);
    CHECK(has_item(*view, "note:work/deep/a.md"));
    CHECK(row_paths(*state) == Paths{"d:", "d:work", "d:work/deep", "n:work/deep/a.md", "n:newest.md"});
    CHECK(state->browser_rows()[3].depth == 3);

    state->set_expanded("work", false);
    settle(*view);
    CHECK_FALSE(has_item(*view, "note:work/deep/a.md"));
    CHECK_FALSE(has_item(*view, "folder:work/deep"));
    CHECK(row_paths(*state) == Paths{"d:", "d:work", "n:newest.md"});
}

TEST_CASE_FIXTURE(BrowserFixture, "clicking the arrow toggles a folder without selecting it") {
    add_note("newest.md", "n", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    const auto rect = view->item_rects().at("folder:work");
    const float font = ImGui::GetStyle().FontScaleDpi * 16.0F;
    ImGui::GetIO().AddMousePosEvent(rect.min_x + font * 1.5F, (rect.min_y + rect.max_y) / 2.0F);
    view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    settle(*view);
    CHECK(state->is_expanded("work"));
    CHECK_FALSE(state->selected_folder().has_value());
    CHECK(current(*state) == "newest.md");
    CHECK(has_item(*view, "note:work/a.md"));
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "the selected nested note is revealed once, then the user may collapse it") {
    add_note("work/a.md", "a", 0);
    add_note("old.md", "o", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK(state->is_expanded("work"));
    CHECK(has_item(*view, "note:work/a.md"));
    state->set_expanded("work", false);
    settle(*view);
    CHECK_FALSE(state->is_expanded("work"));
}

TEST_CASE_FIXTURE(BrowserFixture, "selecting a folder saves the pending edit to that note and to no other") {
    add_note("work/a.md", "aaa", 0);
    add_note("work/b.md", "bbb", 10);
    add_note("c.md", "ccc", 20);
    auto state = make_state();
    REQUIRE(current(*state) == "work/a.md");
    state->edit_text("edited");
    const auto writes = state->write_count();

    state->select_folder("work");

    CHECK(state->write_count() == writes + 1);
    CHECK(read_note("work/a.md") == "edited");
    CHECK(read_note("work/b.md") == "bbb");
    CHECK(read_note("c.md") == "ccc");
    CHECK(selected_folder(*state) == "work");
    CHECK_FALSE(state->editable());
    CHECK(state->buffer().empty());
    CHECK_FALSE(state->dirty());
    CHECK_FALSE(state->current_path().has_value());

    // Özetteyken gelen tarama seçimi nota geri düşürmez; yazılmaya çalışılan metin hiçbir yere gitmez.
    state->rescan();
    CHECK(selected_folder(*state) == "work");
    CHECK_FALSE(state->current_path().has_value());
    state->edit_text("must not land anywhere");
    state->save_now();
    CHECK(state->write_count() == writes + 1);
    CHECK(read_note("work/a.md") == "edited");
    CHECK(read_note("work/b.md") == "bbb");
}

TEST_CASE_FIXTURE(BrowserFixture, "a failing save keeps the note open instead of selecting the folder") {
    add_note("work/a.md", "aaa", 0);
    auto state = make_state();
    state->deciders.conflict = [this](const std::string&) {
        ++conflicts;
        return ConflictChoice::Cancel;
    };
    state->edit_text("mine");
    add_note("work/a.md", "external", 0);

    state->select_folder("work");

    CHECK(conflicts == 1);
    CHECK_FALSE(state->selected_folder().has_value());
    CHECK(current(*state) == "work/a.md");
    CHECK(state->buffer() == "mine");
    CHECK(state->dirty());
}

TEST_CASE_FIXTURE(BrowserFixture, "a selected folder shows its summary instead of the editor") {
    add_note("work/a.md", "a", 0);
    add_note("work/sub/b.md", "b", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    REQUIRE(has_item(*view, "editor"));
    click_button(*view, "folder:work");
    settle(*view);
    CHECK_FALSE(has_item(*view, "editor"));
    CHECK(has_item(*view, "folder_summary"));
    const auto summary = state->folder_summary();
    REQUIRE(summary.has_value());
    CHECK(summary->path == "work");
    CHECK(summary->notes == 2);
    CHECK(summary->folders == 1);
    const gui_test::RenderedText rendered;
    view->draw_frame();
    CHECK(rendered.contains(tr("gui.folder_summary_counts", {{"notes", "2"}, {"folders", "1"}})));
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "toolbar Delete removes the folder on a folder row and trashes on a note row") {
    add_note("work/a.md", "a", 0);
    add_note("keep/b.md", "b", 10);
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    settle(*view);
    REQUIRE(current(*state) == "work/a.md");

    click_button(*view, "toolbar_delete");  // not satırı: onaysız çöpe
    CHECK_FALSE(note_exists("work/a.md"));
    CHECK(note_exists(".trash/work/a.md"));
    CHECK(dir_exists("work"));
    CHECK(note_exists("keep/b.md"));
    settle(*view);

    state->select_folder("keep");
    settle(*view);
    REQUIRE_FALSE(view->item_rects().at("toolbar_delete").disabled);
    click_button(*view, "toolbar_delete");  // klasör satırı: onaylı klasör silme
    CHECK_FALSE(dir_exists("keep"));
    CHECK_FALSE(note_exists("keep/b.md"));
    CHECK(dir_exists("work"));
    CHECK(selected_folder(*state).empty());  // silinen klasörden köke
    settle(*view);
    CHECK(view->item_rects().at("toolbar_delete").disabled);  // kök silinemez
    CHECK(view->item_rects().at("toolbar_rename").disabled);
    CHECK(view->item_rects().at("toolbar_move").disabled);
    CHECK(view->item_rects().at("toolbar_export").disabled);
}

TEST_CASE_FIXTURE(BrowserFixture, "toolbar Rename acts on the note row or on the folder row") {
    add_note("work/a.md", "a", 0);
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    settle(*view);
    std::optional<SaveDialogRequest> asked;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        asked = request;
        return std::optional<SaveDetails>(SaveDetails{"b", "work", "md"});
    };
    click_button(*view, "toolbar_rename");
    REQUIRE(asked.has_value());
    CHECK_FALSE(asked->folder_only);
    CHECK(note_exists("work/b.md"));
    CHECK_FALSE(note_exists("work/a.md"));
    settle(*view);

    state->select_folder("work");
    settle(*view);
    folder_answer = "proj";
    click_button(*view, "toolbar_rename");
    CHECK(note_exists("proj/b.md"));
    CHECK(selected_folder(*state) == "proj");
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "New note and New folder target the selected folder or the selected note's folder") {
    add_note("work/a.md", "a", 0);
    add_note("other/b.md", "b", 10);
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    settle(*view);
    REQUIRE(current(*state) == "work/a.md");

    folder_answer = "from-note";
    click_button(*view, "toolbar_new_folder");
    CHECK(dir_exists("work/from-note"));

    state->select_folder("other");
    settle(*view);
    folder_answer = "from-folder";
    click_button(*view, "toolbar_new_folder");
    CHECK(dir_exists("other/from-folder"));

    std::string offered = "<none>";
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        offered = request.initial.folder;
        return std::nullopt;
    };
    click_button(*view, "toolbar_new_note");
    CHECK(state->is_new_note());
    state->edit_text("taslak");
    state->save_now();
    CHECK(offered == "other");
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "Details shows only the direct children of the current folder and Up works") {
    add_note("root.md", "r", 0);
    add_note("work/a.md", "aa", 10);
    add_note("work/sub/b.md", "bbb", 20);
    auto state = make_state();
    auto view = make_view(*state);
    state->set_browser_view(BrowserView::Details);
    settle(*view);
    REQUIRE(state->current_folder().empty());
    CHECK(row_paths(*state) == Paths{"d:work", "n:root.md"});
    CHECK(has_item(*view, "folder:work"));
    CHECK(has_item(*view, "note:root.md"));
    CHECK_FALSE(has_item(*view, "note:work/a.md"));
    CHECK(view->item_rects().at("details_up").disabled);  // kökte yukarı yok

    state->enter_folder("work");
    settle(*view);
    CHECK(state->current_folder() == "work");
    CHECK(row_paths(*state) == Paths{"d:work/sub", "n:work/a.md"});
    CHECK(has_item(*view, "note:work/a.md"));
    CHECK_FALSE(has_item(*view, "note:root.md"));
    CHECK_FALSE(has_item(*view, "note:work/sub/b.md"));
    CHECK(has_item(*view, "crumb:"));
    CHECK(has_item(*view, "crumb:work"));
    const auto& rows = state->browser_rows();
    CHECK(rows[0].size == 3);  // klasör boyutu alt ağacı kapsar
    CHECK(rows[1].size == 2);
    CHECK(rows[1].updated_at.has_value());

    click_button(*view, "details_up");
    settle(*view);
    CHECK(state->current_folder().empty());
    CHECK(row_paths(*state) == Paths{"d:work", "n:root.md"});

    state->enter_folder("work/sub");
    settle(*view);
    click_button(*view, "crumb:work");
    settle(*view);
    CHECK(state->current_folder() == "work");
}

TEST_CASE_FIXTURE(BrowserFixture, "Details: a click selects a folder, a double click or Enter enters it") {
    add_note("root.md", "r", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->set_browser_view(BrowserView::Details);
    settle(*view);

    click_button(*view, "folder:work");
    settle(*view);
    CHECK(selected_folder(*state) == "work");
    CHECK(state->current_folder().empty());  // konum değişmedi

    press(*view, ImGuiKey_Enter);
    settle(*view);
    CHECK(state->current_folder() == "work");
    CHECK(row_paths(*state) == Paths{"n:work/a.md"});

    click_button(*view, "details_up");
    settle(*view);
    REQUIRE(state->current_folder().empty());
    ImGuiIO& io = ImGui::GetIO();
    constexpr float kUnboundedDoubleClickSeconds = 3600.0F;  // gerçek zaman: yük altında süre aşılmasın
    io.MouseDoubleClickTime = kUnboundedDoubleClickSeconds;
    io.MouseSingleClickDelay = kUnboundedDoubleClickSeconds + 1.0F;
    const auto [x, y] = view->button_center("folder:work");
    io.AddMousePosEvent(x, y);
    view->draw_frame();
    for (int click = 0; click < 2; ++click) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        view->draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        view->draw_frame();
    }
    settle(*view);
    CHECK(state->current_folder() == "work");
    CHECK(current(*state).empty());  // ikinci bırakış yeni listedeki satırı seçmedi
}

TEST_CASE_FIXTURE(BrowserFixture, "Details follows the shown note's folder") {
    add_note("work/a.md", "a", 0);
    add_note("old.md", "o", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->set_browser_view(BrowserView::Details);
    settle(*view);
    CHECK(state->current_folder() == "work");
    CHECK(current(*state) == "work/a.md");
    CHECK(has_item(*view, "note:work/a.md"));
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "Details never moves the selection to a note it does not list after a delete") {
    add_note("root.md", "r", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->set_browser_view(BrowserView::Details);
    settle(*view);
    REQUIRE(state->current_folder().empty());
    REQUIRE(current(*state) == "root.md");
    state->trash_note();
    settle(*view);
    CHECK(current(*state).empty());
    CHECK(selected_folder(*state).empty());
    CHECK(note_exists("work/a.md"));
}

TEST_CASE_FIXTURE(BrowserFixture, "the view mode survives saving and loading the layout") {
    add_note("a.md", "a", 0);
    {
        auto state = make_state();
        CHECK(state->browser_view() == BrowserView::Tree);
        state->set_browser_view(BrowserView::Details);
        REQUIRE(state->request_close());
    }
    {
        auto state = make_state();
        CHECK(state->browser_view() == BrowserView::Details);
        state->set_browser_view(BrowserView::Tree);
        REQUIRE(state->request_close());
    }
    CHECK(make_state()->browser_view() == BrowserView::Tree);
}

TEST_CASE_FIXTURE(BrowserFixture, "a saved layout with the retired folder pane height still loads") {
    add_note("a.md", "a", 0);
    std::filesystem::create_directories(noteapp::core::state_dir());
    std::ofstream(noteapp::core::state_dir() / "gui-state.toml")
        << "left_pane_width = 400.0\nfolder_pane_height = 222.0\nview_mode = \"bogus\"\n";
    auto state = make_state();
    CHECK(state->layout().left_pane_width == doctest::Approx(400.0F));
    CHECK(state->browser_view() == BrowserView::Tree);
}

TEST_CASE_FIXTURE(BrowserFixture, "an active search shows flat results with no tree") {
    add_note("work/deep/alpha.md", "a", 0);
    add_note("beta.md", "b", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK(has_item(*view, "folder:work"));

    state->set_query("alpha");
    settle(*view);
    CHECK_FALSE(state->browsing());
    CHECK_FALSE(has_item(*view, "folder:work"));
    CHECK_FALSE(has_item(*view, "folder:"));
    REQUIRE(state->model().row_count() == 1);
    CHECK(state->model().display(0) == "work/deep/alpha.md");
    CHECK(has_item(*view, "note_list"));

    state->set_query("");
    settle(*view);
    CHECK(has_item(*view, "folder:work"));
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "a search typed while a folder is selected selects a result, so Enter opens it") {
    add_note("work/alpha.md", "a", 0);
    auto state = make_state();
    state->select_folder("work");
    REQUIRE(state->selected_folder().has_value());
    state->set_query("alpha");
    CHECK_FALSE(state->selected_folder().has_value());
    CHECK(current(*state) == "work/alpha.md");
    state->search_enter();
    CHECK_FALSE(state->is_new_note());
}

TEST_CASE_FIXTURE(
    BrowserFixture,
    "tree keys: Up/Down walk visible rows, Right expands then enters, Left collapses then goes up") {
    add_note("top.md", "t", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::List);
    settle(*view);
    REQUIRE(current(*state) == "top.md");
    REQUIRE(row_paths(*state) == Paths{"d:", "d:work", "n:top.md"});

    press(*view, ImGuiKey_UpArrow);
    CHECK(selected_folder(*state) == "work");
    CHECK_FALSE(state->is_expanded("work"));
    press(*view, ImGuiKey_RightArrow);
    CHECK(state->is_expanded("work"));
    CHECK(selected_folder(*state) == "work");
    press(*view, ImGuiKey_RightArrow);
    CHECK(current(*state) == "work/a.md");
    press(*view, ImGuiKey_LeftArrow);  // notun klasörü
    CHECK(selected_folder(*state) == "work");
    press(*view, ImGuiKey_LeftArrow);
    CHECK_FALSE(state->is_expanded("work"));
    press(*view, ImGuiKey_LeftArrow);
    CHECK(selected_folder(*state).empty());
    press(*view, ImGuiKey_LeftArrow);  // kökte durur
    CHECK(selected_folder(*state).empty());

    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_DownArrow);
    CHECK(current(*state) == "top.md");
    press(*view, ImGuiKey_DownArrow);  // sınırda kenetlenir
    CHECK(current(*state) == "top.md");
    press(*view, ImGuiKey_Enter);
    settle(*view);
    CHECK(view->editor_active());
}

TEST_CASE_FIXTURE(BrowserFixture, "Del on a focused folder row deletes that folder after confirmation") {
    add_note("top.md", "t", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_UpArrow);
    REQUIRE(selected_folder(*state) == "work");
    press(*view, ImGuiKey_Delete);
    CHECK_FALSE(dir_exists("work"));
    CHECK_FALSE(note_exists("work/a.md"));
    CHECK(note_exists("top.md"));
}

TEST_CASE_FIXTURE(BrowserFixture, "rows are rebuilt only when the listing or the expansion changes") {
    add_note("top.md", "t", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    const auto idle = state->browser_rebuilds();
    REQUIRE(idle > 0);
    settle(*view);
    state->rescan();  // listeleme aynı
    settle(*view);
    CHECK(state->browser_rebuilds() == idle);

    state->set_expanded("work", true);
    settle(*view);
    CHECK(state->browser_rebuilds() == idle + 1);

    add_note("new.md", "n", 0);
    state->rescan();
    settle(*view);
    CHECK(state->browser_rebuilds() == idle + 2);
    CHECK(has_item(*view, "note:new.md"));
}

TEST_CASE_FIXTURE(BrowserFixture, "renaming an expanded folder keeps it expanded under the new name") {
    add_note("top.md", "t", 0);
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    wire(*state);
    state->set_expanded("work", true);
    folder_answer = "proj";
    state->rename_folder("work");
    CHECK(state->is_expanded("proj"));
    CHECK_FALSE(state->is_expanded("work"));
    CHECK(row_paths(*state) == Paths{"d:", "d:proj", "n:proj/a.md", "n:top.md"});
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "Details: the folder being viewed is not a highlighted row, so it cannot be deleted") {
    add_note("work/a.md", "a", 0);
    add_note("work/sub/b.md", "b", 10);
    add_note("root.md", "r", 20);
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    state->set_browser_view(BrowserView::Details);
    settle(*view);

    state->enter_folder("work");
    settle(*view);
    REQUIRE(state->current_folder() == "work");
    CHECK(state->browser_row() < 0);  // görüntülenen klasörün satırı yok
    CHECK(view->item_rects().at("toolbar_delete").disabled);
    CHECK(view->item_rects().at("toolbar_rename").disabled);
    CHECK(view->item_rects().at("toolbar_move").disabled);
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_Delete);
    state->delete_selected(false);
    CHECK(dir_exists("work"));
    CHECK(note_exists("work/a.md"));
    CHECK(note_exists("work/sub/b.md"));

    // Görünen son not çöpe gidince seçim görüntülenen klasöre düşer; ikinci Del hiçbir şey silmez.
    state->open_note(*noteapp::core::NotePath::parse("work/a.md"));
    settle(*view);
    REQUIRE(current(*state) == "work/a.md");
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_Delete);
    CHECK(note_exists(".trash/work/a.md"));
    CHECK(current(*state).empty());
    CHECK(view->item_rects().at("toolbar_delete").disabled);
    press(*view, ImGuiKey_Delete);
    click_button(*view, "toolbar_delete");
    CHECK(dir_exists("work"));
    CHECK(dir_exists("work/sub"));
    CHECK(note_exists("work/sub/b.md"));
    CHECK(note_exists("root.md"));
}

TEST_CASE_FIXTURE(BrowserFixture,
                  "a folder hidden under a collapsed parent cannot be deleted from the toolbar") {
    add_note("work/sub/b.md", "b", 0);
    auto state = make_state();
    wire(*state);
    state->set_expanded("work", true);
    state->select_folder("work/sub");
    REQUIRE(state->can_delete());
    state->set_expanded("work", false);
    CHECK_FALSE(state->can_delete());
    CHECK_FALSE(state->can_rename());
    state->delete_selected(false);
    CHECK(dir_exists("work/sub"));
}

// Ölçüm: 5.000 not tek klasörde; ilk kurulum (depodan okuma + ağaç + satırlar) ve yalnız satır yeniden
// kurulumu. Süre raporlanır (Debug/sanitizer yapısında eşik oynak olurdu); doğruluk satır sayısıyla.
TEST_CASE_FIXTURE(BrowserFixture, "measure: browser rows for 5000 notes in one folder") {
    constexpr int kNotes = 5000;
    std::vector<noteapp::core::ImportedNote> notes;
    notes.reserve(kNotes);
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(
            {*noteapp::core::NotePath::parse("work/Not-" + std::to_string(kNotes - i) + ".md"), "x\n"});
    REQUIRE(noteapp::core::NoteStore(store_dir()).import_notes(notes).has_value());
    auto state = make_state();
    state->set_expanded("work", true);
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const std::size_t rows = state->browser_rows().size();
    const auto built = Clock::now();
    state->set_expanded("work", false);
    state->browser_rows();
    state->set_expanded("work", true);
    state->browser_rows();
    const auto rebuilt = Clock::now();
    CHECK(rows == kNotes + 2);  // kök, work, notlar
    MESSAGE("first ensure_browser ms=", std::chrono::duration<double, std::milli>(built - started).count(),
            " two row rebuilds ms=", std::chrono::duration<double, std::milli>(rebuilt - built).count());
}
