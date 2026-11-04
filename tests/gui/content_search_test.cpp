#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <doctest/doctest.h>
#include <imgui.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;
using noteapp::gui::TextSelection;

namespace {

// Emoji (4 UTF-8 baytı) ve Türkçe harfler: bayt ve kod noktası konumları ayrışır.
constexpr const char kAlpha[] = "😀 İstanbul güzel. istanbul'a git\nbir satır daha\nISTANBUL";

std::vector<std::string> rows(const GuiState& state) {
    std::vector<std::string> out;
    for (int row = 0; row < state.model().row_count(); ++row)
        out.push_back(state.model().display(row));
    return out;
}

// Plan davranış 3: tek seçim gelir; seçilen metin eşleşmenin kendisidir.
std::string selected_text(const GuiState& state) {
    const auto selection = state.editor_selection();
    return selection ? state.buffer().substr(selection->start, selection->end - selection->start)
                     : std::string{};
}

void click(gui_test::View& view, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    view.draw_frame();
    view.draw_frame();
}

struct ContentFixture : WindowFixture {
    void seed() const {
        add_note("alpha.md", kAlpha, 10);
        add_note("beta.md", "istanbul tek", 20);
        add_note("gamma.md", "hiçbir şey", 30);
    }
};

}  // namespace

TEST_CASE_FIXTURE(ContentFixture, "content mode lists matches with their line counts, best first") {
    seed();
    auto state = make_state();
    state->set_query("istanbul");  // ad modu: yola göre fuzzy, içerik değil
    CHECK(rows(*state).empty());

    state->toggle_search_mode();
    CHECK(rows(*state) == std::vector<std::string>{"alpha.md  (2)", "beta.md  (1)"});  // alpha: 2 satır
    CHECK(state->current_path().has_value());                                          // ilk sonuç seçili
}

// TUI ile aynı anahtar: limits.search.content_results (0 = tüm sonuçlar).
TEST_CASE_FIXTURE(ContentFixture,
                  "content results are capped by limits.search.content_results, 0 lists all") {
    seed();
    REQUIRE(noteapp::core::set_config_value(config, "limits.search.content_results", "1"));
    auto capped = make_state();
    capped->set_query("istanbul");
    capped->toggle_search_mode();
    CHECK(rows(*capped) == std::vector<std::string>{"alpha.md  (2)"});

    REQUIRE(noteapp::core::set_config_value(config, "limits.search.content_results", "0"));
    auto unlimited = make_state();
    unlimited->set_query("istanbul");
    unlimited->toggle_search_mode();
    CHECK(rows(*unlimited) == std::vector<std::string>{"alpha.md  (2)", "beta.md  (1)"});
}

// Kayıt listeyi tazeler: düzenleme notu aramadan düşürürse editör başka nota geçer ya da boşalır. Kaydı
// tetikleyen işlem yine başladığı nota uygulanmalı (alpha.md -> beta.txt olayı).
struct DroppedNoteFixture : ContentFixture {
    std::unique_ptr<GuiState> state;
    std::optional<noteapp::gui::SaveDialogRequest> asked;
    std::optional<noteapp::gui::SaveDetails> answer;

    // `other`: arama sonucunda ikinci eşleşme kalır mı.
    void open_beta_then_drop_it(bool other = true) {
        if (other)
            add_note("alpha.md", "istanbul", 10);
        add_note("work/beta.md", "istanbul tek", 20);
        state = make_state();
        state->deciders.save_dialog = [this](const noteapp::gui::SaveDialogRequest& request) {
            asked = request;
            return answer;
        };
        state->deciders.confirm_delete = [](const std::string&) { return true; };
        state->toggle_search_mode();
        state->set_query("istanbul");
        state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("work/beta.md")));
        REQUIRE(state->current_path().has_value());
        REQUIRE(state->current_path()->string() == "work/beta.md");
        state->edit_text("no match");
    }
    void check_alpha_untouched() const {
        CHECK(read_note("alpha.md") == "istanbul");
        CHECK_FALSE(note_exists("alpha.txt"));
    }
};

TEST_CASE_FIXTURE(DroppedNoteFixture, "type change moves the note it started on, not the next match") {
    open_beta_then_drop_it();
    state->change_type("txt");
    CHECK(read_note("work/beta.txt") == "no match");
    CHECK_FALSE(note_exists("work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "save as moves the note it started on, not the next match") {
    answer = noteapp::gui::SaveDetails{"renamed", "", "md"};
    open_beta_then_drop_it();
    state->save_as();
    REQUIRE(asked.has_value());
    CHECK(asked->initial.name == "beta");
    CHECK(read_note("renamed.md") == "no match");
    CHECK_FALSE(note_exists("work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "folder move moves the note it started on, not the next match") {
    answer = noteapp::gui::SaveDetails{"ignored", "work/sub", "ignored"};
    open_beta_then_drop_it();
    state->move_note();
    REQUIRE(asked.has_value());
    CHECK(asked->initial.name == "beta");
    CHECK(read_note("work/sub/beta.md") == "no match");
    CHECK_FALSE(note_exists("work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "pdf export offers the note it started on, not the next match") {
    open_beta_then_drop_it();
    std::string suggested;
    state->deciders.export_path = [&suggested](const std::string& name) {
        suggested = name;
        return std::optional<std::string>{};
    };
    state->export_pdf();
    CHECK(suggested == "beta.pdf");
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "permanent delete removes the note it started on") {
    open_beta_then_drop_it();
    state->delete_note();
    CHECK_FALSE(note_exists("work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "trash takes the note it started on") {
    open_beta_then_drop_it();
    state->trash_note();
    CHECK(note_exists(".trash/work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "folder rename keeps the other match and the saved note") {
    open_beta_then_drop_it();
    state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("archive");
    };
    state->rename_folder("work");
    CHECK(read_note("archive/beta.md") == "no match");
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "folder delete keeps the other match and clears only its own note") {
    open_beta_then_drop_it();
    state->delete_folder("work");
    CHECK_FALSE(note_exists("work/beta.md"));
    check_alpha_untouched();
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "move actions still reach the note when it was the only match") {
    answer = noteapp::gui::SaveDetails{"renamed", "", "md"};
    open_beta_then_drop_it(false);
    state->save_as();
    CHECK(read_note("renamed.md") == "no match");
    CHECK_FALSE(note_exists("work/beta.md"));
}

TEST_CASE_FIXTURE(DroppedNoteFixture, "type change still reaches the note when it was the only match") {
    open_beta_then_drop_it(false);
    state->change_type("txt");
    CHECK(read_note("work/beta.txt") == "no match");
}

TEST_CASE_FIXTURE(ContentFixture, "a note deleted outside stops save as with a message, no dialog") {
    add_note("alpha.md", "body", 10);
    auto state = make_state();
    int dialogs = 0;
    state->deciders.save_dialog = [&dialogs](const noteapp::gui::SaveDialogRequest&) {
        ++dialogs;
        return std::optional(noteapp::gui::SaveDetails{"renamed", "", "md"});
    };
    remove_note("alpha.md");
    state->save_as();
    CHECK(dialogs == 0);
    CHECK_FALSE(state->status().empty());
    CHECK_FALSE(note_exists("renamed.md"));
}

TEST_CASE_FIXTURE(ContentFixture, "a dirty note deleted outside is kept as new and the action stops") {
    add_note("alpha.md", "body", 10);
    auto state = make_state();
    int dialogs = 0;
    state->deciders.save_dialog = [&dialogs](const noteapp::gui::SaveDialogRequest&) {
        ++dialogs;
        return std::optional(noteapp::gui::SaveDetails{"kept", "", "md"});
    };
    state->edit_text("edited");
    remove_note("alpha.md");
    state->change_type("txt");
    CHECK(dialogs == 1);  // yalnız yeni not kaydı; taşıma durur
    CHECK(read_note("kept.md") == "edited");
    CHECK_FALSE(note_exists("kept.txt"));
    CHECK_FALSE(note_exists("alpha.txt"));
    CHECK_FALSE(state->status().empty());
}

TEST_CASE_FIXTURE(ContentFixture,
                  "exporting a new draft under a filter offers the draft, not the filtered note") {
    add_note("beta.md", "b", 10);
    auto state = make_state();
    state->set_query("beta");
    state->new_note();
    state->edit_text("draft body");
    state->deciders.save_dialog = [](const noteapp::gui::SaveDialogRequest&) {
        return std::optional(noteapp::gui::SaveDetails{"draft", "", "md"});
    };
    std::string suggested;
    state->deciders.export_path = [&suggested](const std::string& name) {
        suggested = name;
        return std::optional<std::string>{};
    };
    state->export_pdf();
    CHECK(suggested == "draft.pdf");
    REQUIRE(state->current_path().has_value());
    CHECK(state->current_path()->string() == "draft.md");
}

TEST_CASE_FIXTURE(ContentFixture, "toggle button, API and Tab from the search box switch the mode") {
    seed();
    auto state = make_state();
    auto view = make_view(*state);
    CHECK_FALSE(state->content_mode());

    state->set_query("istanbul");
    settle(*view);
    const auto [mode_x, mode_y] = view->button_center("mode");
    click(*view, mode_x, mode_y);
    CHECK(state->content_mode());
    CHECK(rows(*state).size() == 2);

    state->focus_search();
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M);  // kısayol geri çevirir, odak kutuda kalır
    CHECK_FALSE(state->content_mode());
    CHECK(view->search_active());
    CHECK(rows(*state).empty());  // ad modunda "istanbul" hiçbir yola uymaz
    state->set_query("alp");
    CHECK(rows(*state) == std::vector<std::string>{"alpha.md"});

    state->focus_search();
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M);
    CHECK(state->content_mode());
    CHECK(rows(*state).empty());  // içerikte "alp" yok

    state->toggle_search_mode();  // API
    CHECK_FALSE(state->content_mode());
}

TEST_CASE_FIXTURE(ContentFixture, "empty query in content mode lists every note without counts") {
    seed();
    auto state = make_state();
    state->toggle_search_mode();
    CHECK(rows(*state) == std::vector<std::string>{"alpha.md", "beta.md", "gamma.md"});
}

// Plan davranış 3: editör diğer eşleşmeleri boyamaz; ilk eşleşmeye gidilir ve seçilir.
TEST_CASE_FIXTURE(ContentFixture, "selecting a result selects the first occurrence at correct byte offsets") {
    seed();
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(rows(*state).front() == "alpha.md  (2)");
    REQUIRE(state->buffer() == kAlpha);

    const std::string_view text = kAlpha;
    const auto first = text.find("İstanbul");
    REQUIRE(first == 5);  // emoji 4 bayt + boşluk
    REQUIRE(state->editor_selection().has_value());
    CHECK(state->editor_selection()->start == first);
    CHECK(selected_text(*state) == "İstanbul");
    CHECK_FALSE(state->dirty());  // seçim belgeyi kirletmez
}

TEST_CASE_FIXTURE(ContentFixture, "Enter from the search box lands on the first match in the editor") {
    seed();
    auto state = make_state();
    auto view = make_view(*state);
    state->toggle_search_mode();
    state->focus_search();
    settle(*view);
    type_text(*view, "istanbul");
    settle(*view);
    press(*view, ImGuiKey_Enter);
    settle(*view);
    REQUIRE(view->editor_active());
    REQUIRE(state->editor_selection().has_value());  // editörde seçili
    CHECK(selected_text(*state) == "İstanbul");

    type_text(*view,
              "X");  // seçili eşleşme yazılanla değişir: seçimin gerçekten ilk eşleşmede olduğunun kanıtı
    CHECK(state->buffer() == std::string("😀 X güzel. istanbul'a git\nbir satır daha\nISTANBUL"));
}

TEST_CASE_FIXTURE(ContentFixture, "folding matches Turkish letters and phrases select as one range") {
    add_note("tr.md", "Öğrenci ogrenci ÖĞRENCİ\nışık ISIK güzel. istanbul", 0);
    auto state = make_state();
    state->toggle_search_mode();

    state->set_query("ogrenci");
    CHECK(selected_text(*state) == "Öğrenci");

    state->set_query("\"güzel. istanbul\"");
    CHECK(selected_text(*state) == "güzel. istanbul");
}

TEST_CASE_FIXTURE(ContentFixture, "selection clears on edit, query change and mode change") {
    seed();
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(state->editor_selection().has_value());

    state->edit_text(state->buffer() + "x");  // kullanıcı düzenlemesi: ofsetler eskidi
    CHECK_FALSE(state->editor_selection().has_value());

    REQUIRE(state->current_path().has_value());
    state->set_query("istanbul tek");  // sorgu değişti: beta seçilir, yeni seçim
    CHECK(state->current_path()->string() == "beta.md");
    CHECK(selected_text(*state) == "istanbul");

    state->set_query("zzz");  // sonuç yok
    CHECK_FALSE(state->editor_selection().has_value());

    state->set_query("istanbul");
    REQUIRE(state->editor_selection().has_value());
    state->toggle_search_mode();  // ad moduna dönüş
    CHECK_FALSE(state->editor_selection().has_value());
}

TEST_CASE_FIXTURE(ContentFixture, "selection is not reissued across rescans") {
    seed();
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(state->editor_selection().has_value());

    // Kullanıcı imleci taşıdı (seçim uygulandı); yedek tarama onu ilk eşleşmeye geri atmamalı.
    state->take_editor_selection();
    state->rescan();
    CHECK_FALSE(state->editor_selection().has_value());
}

TEST_CASE_FIXTURE(ContentFixture, "content index follows external changes after a rescan") {
    seed();
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(rows(*state).size() == 2);

    add_note("new.md", "yeni istanbul istanbul", 0);  // dışarıdan eklendi
    state->rescan();
    CHECK(rows(*state) == std::vector<std::string>{"alpha.md  (2)", "new.md  (1)", "beta.md  (1)"});

    add_note("beta.md", "artık yok", 0);  // dışarıdan içerik değişti
    state->rescan();
    CHECK(rows(*state) == std::vector<std::string>{"alpha.md  (2)", "new.md  (1)"});
}

TEST_CASE_FIXTURE(ContentFixture, "content results respect the selected folder") {
    add_note("work/a.md", "istanbul a", 0);
    add_note("b.md", "istanbul b", 10);
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(rows(*state).size() == 2);
    state->set_folder("work");
    CHECK(rows(*state) == std::vector<std::string>{"work/a.md  (1)"});
}

TEST_CASE_FIXTURE(ContentFixture, "clicking a folder in the tree limits the content results") {
    add_note("work/a.md", "istanbul a", 0);
    add_note("b.md", "istanbul b", 10);
    auto state = make_state();
    auto view = gui_test::make_view(*state);
    state->toggle_search_mode();
    state->set_query("istanbul");
    REQUIRE(rows(*state).size() == 2);
    // Arama sürerken ağaç gizlidir (düz sonuç): klasör önce ağaçta seçilir, arama onun içinde kalır.
    state->set_query("");
    gui_test::settle(*view);
    gui_test::click_button(*view, "folder:work");
    state->set_query("istanbul");
    CHECK(rows(*state) == std::vector<std::string>{"work/a.md  (1)"});
}

// ---- Dosya izleme: zamanlı tarama + odağa dönüş (plan T40) ----

TEST_CASE_FIXTURE(ContentFixture, "the rescan timer picks up external changes only after rescan_seconds") {
    config.tuning.rescan_seconds = 30;
    add_note("alpha.md", "a", 10);
    auto state = make_state();
    REQUIRE(state->model().row_count() == 1);

    add_note("new.md", "n", 0);  // dışarıdan
    CHECK_FALSE(state->tick(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    CHECK(state->model().row_count() == 1);

    CHECK(state->tick(std::chrono::steady_clock::now() + std::chrono::seconds(31)));
    CHECK(state->model().row_count() == 2);
}

TEST_CASE_FIXTURE(ContentFixture, "window focus triggers a rescan, even when it arrives during a dialog") {
    config.tuning.rescan_seconds = 3600;  // zamanlayıcı karışmasın
    add_note("alpha.md", "a", 10);
    auto state = make_state();
    auto view = make_view(*state);

    add_note("new.md", "n", 0);
    view->draw_frame();
    CHECK(state->model().row_count() == 1);  // odak olayı yokken dışarıdaki değişiklik henüz görülmez

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    REQUIRE(SDL_PushEvent(&focus));
    view->draw_frame();
    CHECK(state->model().row_count() == 2);

    // Diyalog sırasında gelen odak düşmez: diyalog kapanınca çalışır.
    add_note("late.md", "l", 0);
    int frame = 0;
    view->on_modal_frame = [&] {
        ++frame;
        if (frame == 1)
            REQUIRE(SDL_PushEvent(&focus));
        if (frame == 5)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        if (frame == 6)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    };
    view->install_deciders();
    state->new_note();
    state->edit_text("draft\n");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    view->draw_frame();
    CHECK(state->model().total() == 3);
}
