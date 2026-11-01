#include "core/config.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;
using noteapp::gui::Tab;
namespace fs = std::filesystem;

namespace {

std::string current(const GuiState& state) {
    const auto path = state.current_path();
    return path ? path->string() : std::string{};
}

// Üretim diyaloğunu gerçek kareler içinde tuşla yanıtlar. Yanıt tutmazsa sonsuz döngü yerine Escape'e düşer:
// test asılı kalmaz, beklenen sonuç gelmediği için düşer.
constexpr int kAnswerFrame = 5;  // ImGui kısayol rotası etkin alan için bir kare sonra yerleşir
constexpr int kGiveUpFrame = 200;

struct ModalKeys {
    int frame = 0;
    ImGuiKey answer;
    explicit ModalKeys(ImGuiKey key) : answer(key) {}
    void operator()() {
        ImGuiIO& io = ImGui::GetIO();
        ++frame;
        // Diyaloğu açan kısayol hâlâ basılı sayılır (press bırakmayı diyalog kapanınca yapar); gerçek
        // kullanıcı bu sırada parmağını çeker. Ctrl basılıyken Enter başka anlama gelirdi.
        if (frame == 1) {
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiMod_Shift, false);
            io.AddKeyEvent(ImGuiKey_S, false);
        }
        if (frame == kAnswerFrame)
            gui_test::tap_key(answer);
        if (frame % kGiveUpFrame == 0)
            gui_test::tap_key(ImGuiKey_Escape);
    }
};

}  // namespace

TEST_CASE_FIXTURE(WindowFixture, "window lists notes, selects the newest and shows its content") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    add_note("gamma.md", "gamma body", 10);
    auto state = make_state();

    CHECK(state->model().row_count() == 3);
    CHECK(current(*state) == "gamma.md");
    CHECK(state->buffer() == "gamma body");
    CHECK(state->editable());
}

TEST_CASE_FIXTURE(WindowFixture, "typing in the search box filters the list and keeps the selection valid") {
    add_note("alpha.md", "a", 30);
    add_note("beta.md", "b", 20);
    add_note("gamma.md", "g", 10);
    auto state = make_state();
    auto view = make_view(*state);

    state->focus_search();
    settle(*view);
    type_text(*view, "be");
    CHECK(state->query() == "be");
    CHECK(state->model().row_count() == 1);
    CHECK(current(*state) == "beta.md");
    CHECK(state->buffer() == "b");

    state->set_query("");
    CHECK(state->model().row_count() == 3);
    CHECK(current(*state) == "beta.md");  // seçim yola göre korunur (TUI ile aynı)
}

TEST_CASE_FIXTURE(WindowFixture, "Up/Down in the search box move the list selection and update the view") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    add_note("gamma.md", "gamma body", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->focus_search();
    settle(*view);

    press(*view, ImGuiKey_DownArrow);
    CHECK(current(*state) == "beta.md");
    CHECK(state->buffer() == "beta body");
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_DownArrow);  // sınırda kenetlenir
    CHECK(current(*state) == "alpha.md");
    press(*view, ImGuiKey_UpArrow);
    CHECK(current(*state) == "beta.md");
}

TEST_CASE_FIXTURE(WindowFixture, "Enter in the search box focuses the editor") {
    add_note("alpha.md", "alpha body");
    auto state = make_state();
    auto view = make_view(*state);

    state->focus_search();
    settle(*view);
    CHECK(view->search_active());
    press(*view, ImGuiKey_Enter);
    CHECK(view->editor_active());
    // Odak metni seçmemeli: yazılan harf notu ezerse veri kaybı olurdu.
    type_text(*view, "x");
    CHECK(state->buffer() == "xalpha body");
}

TEST_CASE_FIXTURE(WindowFixture, "focus_search selects the existing query; settings tab switches") {
    add_note("alpha.md", "alpha body");
    auto state = make_state();
    auto view = make_view(*state);

    state->set_query("alp");
    state->focus_search();
    settle(*view);
    type_text(*view, "z");  // seçili sorgu yazılanla değişir
    CHECK(state->query() == "z");

    CHECK(state->tab() == Tab::Notes);
    state->show_settings_tab();
    view->draw_frame();
    view->draw_frame();
    view->draw_frame();
    CHECK(state->tab() == Tab::Settings);
    state->focus_search();  // Ctrl+K her sekmeden Notlar'a döner
    view->draw_frame();
    view->draw_frame();
    view->draw_frame();
    CHECK(state->tab() == Tab::Notes);
}

TEST_CASE_FIXTURE(WindowFixture, "binary note shows the binary placeholder instead of its bytes") {
    add_note("blob.bin", std::string("ab\0cd", 5));
    auto state = make_state();
    CHECK(state->buffer() == noteapp::core::tr("gui.binary_preview"));
}

TEST_CASE_FIXTURE(WindowFixture, "folder tree limits the list to the subtree") {
    add_note("root.md", "r", 30);
    add_note("work/one.md", "1", 20);
    add_note("work/deep/two.md", "2", 10);
    add_note("home/three.md", "3", 5);
    auto state = make_state();
    REQUIRE(state->model().row_count() == 4);
    CHECK(state->folders() == std::vector<std::string>{"home", "work", "work/deep"});
    auto view = make_view(*state);  // ağaç çizilir (kök + iç içe klasörler)

    state->set_folder("work");
    view->draw_frame();
    CHECK(state->model().row_count() == 2);
    state->set_folder("");
    view->draw_frame();
    CHECK(state->model().row_count() == 4);
}

TEST_CASE_FIXTURE(WindowFixture, "the folder tree selects by click and Enter under the all notes root") {
    add_note("root.md", "r", 30);
    add_note("work/one.md", "1", 20);
    add_note("home/three.md", "3", 5);
    auto state = make_state();
    auto view = make_view(*state);
    {
        const gui_test::RenderedText rendered;
        view->draw_frame();
        CHECK(rendered.contains(noteapp::core::tr("gui.all_notes")));
    }

    gui_test::click_button(*view, "folder:work");
    CHECK(state->current_folder() == "work");
    CHECK(state->model().row_count() == 1);
    gui_test::click_button(*view, "folder:");
    CHECK(state->current_folder().empty());
    CHECK(state->model().row_count() == 3);

    // Kökten aşağı ok ilk klasöre (home) gider; Enter onu seçer, gezinme tek başına seçmez.
    press(*view, ImGuiKey_DownArrow);
    CHECK(state->current_folder().empty());
    press(*view, ImGuiKey_Enter);
    CHECK(state->current_folder() == "home");
    CHECK(state->model().row_count() == 1);
    press(*view, ImGuiKey_UpArrow);
    press(*view, ImGuiKey_Enter);
    CHECK(state->current_folder().empty());
}

TEST_CASE_FIXTURE(WindowFixture,
                  "rescan picks up new notes, keeps selection and editor, resets only on change") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    auto state = make_state();
    state->move_selection(1);
    REQUIRE(current(*state) == "alpha.md");

    const auto resets = state->model().resets();
    state->rescan();
    CHECK(state->model().resets() == resets);  // değişiklik yok: sıfırlama yok

    add_note("newest.md", "fresh", 0);
    state->rescan();
    CHECK(state->model().resets() == resets + 1);
    CHECK(state->model().row_count() == 3);
    CHECK(current(*state) == "alpha.md");    // seçim yola göre korundu
    CHECK(state->buffer() == "alpha body");  // editör sabit
}

// Plan davranış değişikliği 7: gui-state.ini yerine gui-state.toml.
TEST_CASE_FIXTURE(WindowFixture, "selected note is restored from gui-state.toml after close") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    {
        auto state = make_state();
        state->move_selection(1);
        REQUIRE(current(*state) == "alpha.md");
        state->layout().width = 777;
        CHECK(state->request_close());
    }
    CHECK(fs::exists(home.path() / "state" / "gui-state.toml"));

    auto reopened = make_state();
    CHECK(current(*reopened) == "alpha.md");
    CHECK(reopened->buffer() == "alpha body");
    CHECK(reopened->layout().width == 777);
}

TEST_CASE_FIXTURE(WindowFixture, "a corrupt or out-of-range gui-state.toml falls back to defaults") {
    add_note("alpha.md", "alpha body");
    const auto defaults = make_state()->layout();
    fs::create_directories(home.path() / "state");
    {
        std::ofstream(home.path() / "state" / "gui-state.toml", std::ios::binary) << "not [valid toml";
        auto state = make_state();
        CHECK(state->layout().width == defaults.width);
        CHECK(current(*state) == "alpha.md");
    }
    std::ofstream(home.path() / "state" / "gui-state.toml", std::ios::binary)
        << "width = -5\nheight = 99999999\nlast_note = \"../outside.md\"\n";
    auto state = make_state();
    CHECK(state->layout().width == defaults.width);
    CHECK(state->layout().height == defaults.height);
    CHECK(current(*state) == "alpha.md");
}

TEST_CASE_FIXTURE(WindowFixture, "config warnings are shown in the status bar at startup") {
    auto state = make_state();

    state->show_config_warnings({});
    CHECK(state->status().empty());

    const noteapp::core::Error unknown{
        noteapp::core::Error::Code::ConfigInvalid, "config.unknown_key", {{"key", "bogus"}}};
    const noteapp::core::Error invalid{noteapp::core::Error::Code::ConfigInvalid,
                                       "config.invalid_value",
                                       {{"key", "sync.idle_seconds"}, {"reason", "must_be_positive"}}};
    state->show_config_warnings({unknown, invalid});
    const std::string shown = state->status();
    CHECK(shown.find(noteapp::core::format_error(unknown)) != std::string::npos);
    CHECK(shown.find(noteapp::core::format_error(invalid)) != std::string::npos);
}

// --- Gerçek çizim katmanı (SDL dummy sürücü, yazılımsal çizim, io.AddKeyEvent) ---

TEST_CASE_FIXTURE(WindowFixture, "the window draws a Turkish note with glyphs from the embedded font") {
    add_note("türkçe.md", "çğıİöşü");
    auto state = make_state();
    auto view = make_view(*state);

    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    for (const ImWchar glyph : {0x00E7, 0x011F, 0x0131, 0x0130, 0x00F6, 0x015F, 0x00FC})
        CHECK(ImGui::GetIO().Fonts->Fonts[0]->IsGlyphInFont(glyph));
}

TEST_CASE_FIXTURE(WindowFixture, "typing Turkish text in the editor edits the buffer and autosaves") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());

    type_text(*view, "çğıİöşü");
    CHECK(state->buffer() == "çğıİöşüa");
    CHECK(state->dirty());
    state->tick(*state->next_deadline());
    CHECK(read_note("alpha.md") == "çğıİöşüa");
}

// ImGui'nin varsayılanı Escape'te odaklanıldığından beri yapılan tüm düzenlemeyi geri almaktır: sessiz veri
// kaybı.
TEST_CASE_FIXTURE(WindowFixture, "Escape in the editor keeps typed text") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);

    type_text(*view, "typed");
    press(*view, ImGuiKey_Escape);
    CHECK(state->buffer() == "typeda");
    CHECK(view->editor_active());
}

TEST_CASE_FIXTURE(WindowFixture, "a read-only note ignores typing") {
    add_note("latin.txt", "caf\xe9");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);

    type_text(*view, "x");
    CHECK(state->buffer() == "caf\xe9");
    CHECK_FALSE(state->dirty());
}

TEST_CASE_FIXTURE(WindowFixture, "a clean note reloaded from disk stays editable with the new text") {
    add_note("alpha.md", "old", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());

    add_note("alpha.md", "new text", 0);
    state->rescan();
    view->draw_frame();
    view->draw_frame();
    CHECK(view->editor_active());  // yeniden yükleme odağı düşürmez
    type_text(*view, "x");
    CHECK(state->buffer() == "xnew text");  // eski metin ImGui'nin iç kopyasından geri gelmez
}

TEST_CASE_FIXTURE(WindowFixture, "the shortcut table drives the same actions as before") {
    add_note("alpha.md", "alpha body", 20);
    add_note("beta.md", "beta body", 10);
    auto state = make_state();
    auto view = make_view(*state);
    int dialogs = 0;
    state->deciders.save_dialog = [&](const SaveDialogRequest&) {
        ++dialogs;
        return std::optional<SaveDetails>{};
    };

    press(*view, ImGuiMod_Ctrl | ImGuiKey_Comma);
    CHECK(state->tab() == Tab::Settings);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_K);
    CHECK(state->tab() == Tab::Notes);
    settle(*view);
    CHECK(view->search_active());
    press(*view, ImGuiKey_Enter);
    REQUIRE(view->editor_active());
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_P);
    settle(*view);
    CHECK(view->search_active());

    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);  // temiz notta kayıt yok
    CHECK(state->write_count() == 0);

    press(*view, ImGuiMod_Ctrl | ImGuiKey_N);
    CHECK(state->is_new_note());
    CHECK(view->editor_active());
    type_text(*view, "draft");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    CHECK(dialogs == 1);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);  // yeni notta Ctrl+Shift+S de diyalog açar
    CHECK(dialogs == 2);
}

TEST_CASE_FIXTURE(WindowFixture, "Ctrl+M asks only for the folder, Del trashes, Shift+Del asks to delete") {
    add_note("alpha.md", "alpha", 20);
    add_note("beta.md", "beta", 10);
    auto state = make_state();
    auto view = make_view(*state);
    int moves = 0;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        ++moves;
        CHECK(request.folder_only);
        return std::optional<SaveDetails>{};
    };
    int deletes = 0;
    state->deciders.confirm_delete = [&](const std::string&) {
        ++deletes;
        return false;
    };

    press(*view, ImGuiMod_Ctrl | ImGuiKey_M);
    CHECK(moves == 1);

    state->request_focus(FocusRequest::Editor);  // Del editördeyken karakter siler, not silmez
    settle(*view);
    press(*view, ImGuiKey_Delete);
    CHECK(deletes == 0);

    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK(deletes == 1);
    CHECK(note_exists("beta.md"));

    press(*view, ImGuiKey_Delete);
    CHECK(deletes == 1);
    CHECK_FALSE(note_exists("beta.md"));
    CHECK(note_exists(".trash/beta.md"));
}

TEST_CASE_FIXTURE(WindowFixture, "Up/Down in the focused list move the selection") {
    add_note("alpha.md", "a", 20);
    add_note("beta.md", "b", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_DownArrow);
    CHECK(current(*state) == "alpha.md");
    press(*view, ImGuiKey_UpArrow);
    CHECK(current(*state) == "beta.md");
}

TEST_CASE_FIXTURE(WindowFixture,
                  "the conflict dialog: Enter takes the copy default and keeps both versions") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    view->on_modal_frame = ModalKeys(ImGuiKey_Enter);

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);

    CHECK(read_note("alpha.md") == "external");
    CHECK(read_note("alpha (copy).md") == "mine");
    CHECK(current(*state) == "alpha (copy).md");
}

TEST_CASE_FIXTURE(WindowFixture, "the conflict dialog: Escape cancels and keeps the buffer") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    view->on_modal_frame = ModalKeys(ImGuiKey_Escape);

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);

    CHECK(state->buffer() == "mine");
    CHECK(state->dirty());
    CHECK(read_note("alpha.md") == "external");
    CHECK_FALSE(note_exists("alpha (copy).md"));
}

TEST_CASE_FIXTURE(WindowFixture, "the save dialog: Enter accepts the prefilled fields, Escape cancels") {
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();

    view->on_modal_frame = ModalKeys(ImGuiKey_Escape);
    state->new_note();
    state->edit_text("first line\n");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    CHECK(state->is_new_note());
    CHECK(state->buffer() == "first line\n");

    view->on_modal_frame = ModalKeys(ImGuiKey_Enter);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    CHECK_FALSE(state->is_new_note());
    REQUIRE(state->current_path().has_value());
    CHECK(read_note(current(*state)) == "first line\n");
}

TEST_CASE_FIXTURE(WindowFixture, "the close-unsaved dialog defaults to Save") {
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    state->new_note();
    state->edit_text("kept\n");

    int save_dialogs = 0;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        ++save_dialogs;
        return std::optional<SaveDetails>{SaveDetails{"kept", "", request.initial.type}};
    };
    view->on_modal_frame = ModalKeys(ImGuiKey_Enter);
    CHECK(state->request_close());
    CHECK(save_dialogs == 1);
    CHECK(note_exists("kept." + config.default_type));
}

TEST_CASE_FIXTURE(WindowFixture, "the delete confirmation defaults to Cancel") {
    const auto box = noteapp::gui::make_delete_box("really?");
    CHECK(box.buttons.at(box.default_button).first == noteapp::core::tr("gui.cancel"));
    CHECK_FALSE(box.buttons.at(box.default_button).second);
    CHECK(box.buttons.at(box.escape_button).first == noteapp::core::tr("gui.cancel"));
}

TEST_CASE_FIXTURE(WindowFixture, "an idle window draws no frames") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    for (int i = 0; i < 10; ++i)  // olaydan sonraki sabitlenme kareleri tükensin
        view->idle_step(0);
    const auto frames = view->frames();
    for (int i = 0; i < 20; ++i)
        view->idle_step(0);
    CHECK(view->frames() == frames);

    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    REQUIRE(SDL_PushEvent(&wake));
    view->idle_step(0);
    CHECK(view->frames() > frames);
}

TEST_CASE_FIXTURE(WindowFixture, "closing during a modal is deferred until the modal is answered") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    int frame = 0;
    view->on_modal_frame = [&, keys = ModalKeys(ImGuiKey_Escape)]() mutable {
        if (++frame == 1) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            REQUIRE(SDL_PushEvent(&quit));
        }
        keys();
    };

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    CHECK(view->close_requested());
    CHECK(state->buffer() == "mine");
}

TEST_CASE_FIXTURE(WindowFixture, "old-layout notes: the window says how to migrate and refuses to save") {
    std::ofstream(full_path("eski.md"), std::ios::binary) << "eski";
    auto state = make_state();
    state->show_config_warnings({});
    CHECK(state->status().find("note graph migrate") != std::string::npos);
    CHECK(state->model().row_count() == 0);
    CHECK(noteapp::test::slurp(full_path("eski.md")) == "eski");
}
