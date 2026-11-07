#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <optional>
#include <string>
#include <thread>
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
using noteapp::gui::View;
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

    // Ağaç varsayılan sırada, en yeni başta (kök, gamma, beta, alpha); açılışta en yeni not seçilidir.
    REQUIRE(current(*state) == "gamma.md");
    press(*view, ImGuiKey_DownArrow);
    CHECK(current(*state) == "beta.md");
    CHECK(state->buffer() == "beta body");
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_DownArrow);  // sınırda kenetlenir
    CHECK(current(*state) == "alpha.md");
    press(*view, ImGuiKey_UpArrow);
    press(*view, ImGuiKey_UpArrow);
    CHECK(current(*state) == "gamma.md");
}

// NY-G inceleme: arama kutusunda Enter bulunan notu TUI gibi incelemede bırakır, odak listeye; düzenleme `E`.
TEST_CASE_FIXTURE(WindowFixture, "Enter in the search box keeps the found note in review and focuses the list") {
    add_note("alpha.md", "alpha body");
    auto state = make_state();
    auto view = make_view(*state);

    state->focus_search();
    settle(*view);
    CHECK(view->search_active());
    press(*view, ImGuiKey_Enter);
    CHECK(state->reviewing());
    CHECK_FALSE(view->editor_active());
    CHECK_FALSE(view->search_active());
    type_text(*view, "x");
    CHECK(state->buffer() == "alpha body");
    press(*view, ImGuiKey_E);  // listede düzenle
    REQUIRE(view->editor_active());
    // Odak metni seçmemeli: yazılan harf notu ezerse veri kaybı olurdu.
    type_text(*view, "x");
    CHECK(state->buffer() == "xalpha body");
}

TEST_CASE_FIXTURE(WindowFixture, "E typed in the search box is text, it does not start editing") {
    add_note("egg.md", "egg body");
    auto state = make_state();
    auto view = make_view(*state);
    state->focus_search();
    settle(*view);
    type_text(*view, "e");
    settle(*view);
    CHECK(state->query() == "e");
    CHECK(state->reviewing());
    CHECK_FALSE(view->editor_active());
    CHECK(state->buffer() == "egg body");
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
    CHECK(state->buffer() == noteapp::core::tr("app.binary_preview"));
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

    // Kökten aşağı ok ilk klasörü (home) hemen seçer (TUI gibi); yukarı ok köke döner.
    press(*view, ImGuiKey_DownArrow);
    CHECK(state->current_folder() == "home");
    CHECK(state->model().row_count() == 1);
    press(*view, ImGuiKey_UpArrow);
    CHECK(state->current_folder().empty());
    CHECK(state->model().row_count() == 3);
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

TEST_CASE_FIXTURE(WindowFixture, "every GUI font draws bidi and zero-width characters as a visible glyph") {
    add_note("a.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
        ImFontBaked* baked = font->GetFontBaked(ImGui::GetFontSize());
        REQUIRE(baked != nullptr);
        for (const noteapp::core::HiddenControl& hidden : noteapp::core::hidden_controls()) {
            CAPTURE(static_cast<unsigned>(hidden.code_point));
            const ImFontGlyph* glyph = baked->FindGlyph(static_cast<ImWchar>(hidden.code_point));
            REQUIRE(glyph != nullptr);
            CHECK(glyph->Visible);
            CHECK(glyph->AdvanceX > 0.0F);
        }
    }
}

TEST_CASE_FIXTURE(WindowFixture,
                  "a note with hidden bidi or zero-width characters gets a header warning with the count") {
    const std::string content = "if (a\xE2\x80\xAE) {}\n\xE2\x80\x8Bx\xE2\x81\xA6\n";
    add_note("trojan.md", content);
    auto state = make_state();
    auto view = make_view(*state);
    REQUIRE(state->buffer() == content);
    CHECK(state->hidden_controls() == 3);
    {
        const gui_test::RenderedText text;
        settle(*view);
        REQUIRE(view->item_rects().contains("hidden_controls"));
        CHECK(text.contains(noteapp::core::tr("gui.hidden_controls", {{"count", "3"}})));
    }
    // Açmak ve kaydetmek baytlara dokunmaz.
    state->save_now();
    CHECK(read_note("trojan.md") == content);
    state->edit_text(content + "\xE2\x80\x8F");
    CHECK(state->hidden_controls() == 4);
    state->save_now();
    CHECK(read_note("trojan.md") == content + "\xE2\x80\x8F");

    state->edit_text("x\xE2\x80\x8Fy");
    CHECK(state->hidden_controls() == 1);
    CHECK(state->hidden_controls_label() == noteapp::core::tr("gui.hidden_controls_one"));
    state->edit_text("temiz");
    settle(*view);
    CHECK(state->hidden_controls() == 0);
    CHECK(state->hidden_controls_label().empty());
    CHECK_FALSE(view->item_rects().contains("hidden_controls"));
}

TEST_CASE_FIXTURE(WindowFixture, "typing Turkish text in the editor edits the buffer and autosaves") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());

    type_text(*view, "çğıİöşü");
    CHECK(state->buffer() == "çğıİöşüa");
    CHECK(state->dirty());
    REQUIRE(state->next_deadline().has_value());
    state->tick(*state->next_deadline());
    CHECK(read_note("alpha.md") == "çğıİöşüa");
}

// ImGui'nin varsayılanı Escape'te odaklanıldığından beri yapılan tüm düzenlemeyi geri almaktır: sessiz veri
// kaybı.
TEST_CASE_FIXTURE(WindowFixture, "Escape in the editor keeps typed text") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
    state->request_focus(FocusRequest::Editor);
    settle(*view);

    type_text(*view, "typed");
    press(*view, ImGuiKey_Escape);
    CHECK(state->buffer() == "typeda");
    CHECK(read_note("alpha.md") == "typeda");  // NY-G: Esc kaydedip incelemeye döner, metin atılmaz
    CHECK(state->reviewing());
}

TEST_CASE_FIXTURE(WindowFixture, "a read-only note ignores typing") {
    add_note("latin.txt", "caf\xe9");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
    state->request_focus(FocusRequest::Editor);
    settle(*view);

    type_text(*view, "x");
    // Salt-okunur görünüm TUI ile aynı arındırılmış metindir (app::sanitized_utf8); dosya değişmez.
    CHECK(state->buffer() == "caf\xEF\xBF\xBD");
    CHECK_FALSE(state->dirty());
    CHECK(read_note("latin.txt") == "caf\xe9");
}

TEST_CASE_FIXTURE(WindowFixture, "a clean note reloaded from disk stays editable with the new text") {
    add_note("alpha.md", "old", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
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
    REQUIRE(state->reviewing());  // NY-G: bulunan not incelemede
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
    state->deciders.confirm_delete = [&](const std::string&, const std::string&) {
        ++deletes;
        return false;
    };

    press(*view, ImGuiMod_Ctrl | ImGuiKey_M);
    CHECK(moves == 1);

    state->edit_selected();  // seçilen not incelemede açılır

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
    // Ağaç sırası varsayılan sıradır, en yeni başta: kök, beta, alpha; açılışta en yeni not (beta) seçilidir.
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

namespace {

// Gerçek zamanlı boşta bekleme; timeout 1: kırpma imleci gibi zamanlayıcı kareleri de sayılsın (0 onları
// kapatır).
void idle_for(View& view, std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until)
        view.idle_step(1);
}

// Fare olayı kuyruğa: imleç (x, y)'ye gelir.
void move_mouse(float x, float y) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    REQUIRE(count > 0);
    const SDL_WindowID window = SDL_GetWindowID(windows[0]);
    SDL_free(windows);
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = window;
    motion.motion.x = x;
    motion.motion.y = y;
    REQUIRE(SDL_PushEvent(&motion));
}

// Fare düğmenin ortasına gelir ve orada durur (sonraki olay yok).
void rest_mouse_on(View& view, const char* button) {
    const auto [x, y] = view.button_center(button);
    move_mouse(x, y);
}

}  // namespace

TEST_CASE_FIXTURE(WindowFixture, "a periodic rescan that finds no change draws no frames") {
    add_note("alpha.md", "a");
    config.tuning.rescan_seconds = 1;  // en kısa aralık; tarama testte birkaç kez koşsun
    auto state = make_state();
    auto view = make_view(*state);
    for (int i = 0; i < 10; ++i)  // olaydan sonraki sabitlenme kareleri tükensin
        view->idle_step(0);
    const auto frames = view->frames();
    idle_for(*view, std::chrono::milliseconds(2500));  // en az iki tarama
    CHECK(view->frames() == frames);

    // Başka bağlantının yazması sonraki taramada görünür ve aynı adımda çizilir.
    const auto quiet = view->frames();
    add_note("beta.md", "b");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state->model().row_count() != 2 && std::chrono::steady_clock::now() < until)
        view->idle_step(1);
    CHECK(state->model().row_count() == 2);
    CHECK(view->frames() > quiet);
    // Değişiklik görüldükten sonraki tarama yine iş saymaz.
    CHECK_FALSE(state->tick(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
}

TEST_CASE_FIXTURE(WindowFixture, "an idle focused editor draws no frames") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    REQUIRE(ImGui::GetIO().WantTextInput);  // IME/SDL metin girdisi açık kalır
    for (int i = 0; i < 10; ++i)
        view->idle_step(0);
    const auto frames = view->frames();
    idle_for(*view, std::chrono::milliseconds(300));
    CHECK(view->frames() == frames);
    CHECK(view->editor_active());

    // Yazılan harf yine hemen çizilir.
    type_text(*view, "x");
    CHECK(view->frames() > frames);
    CHECK(state->buffer() == "xa");
}

namespace {

// Silme onayını açar; `idle` sonra başka iş parçacığından gerçek Esc (SDL olayı) gelir ve kapatır.
// on_modal_frame yok: diyalog üretimdeki gibi olay bekler. Diyalog boyunca çizilen kare sayısı döner.
std::size_t frames_in_idle_dialog(GuiState& state, View& view, std::chrono::milliseconds idle) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    REQUIRE(count > 0);
    const SDL_WindowID window = SDL_GetWindowID(windows[0]);
    SDL_free(windows);
    const std::size_t before = view.frames();
    std::atomic<bool> closed = false;
    std::jthread escape([window, idle, &closed] {
        std::this_thread::sleep_for(idle);
        constexpr int kTries = 50;  // tuş kaybolursa yeniden: test asılı kalmasın
        for (int attempt = 0; attempt < kTries && !closed; ++attempt) {
            for (const bool down : {true, false}) {
                SDL_Event key{};
                key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                key.key.windowID = window;
                key.key.key = SDLK_ESCAPE;
                key.key.scancode = SDL_SCANCODE_ESCAPE;
                key.key.down = down;
                SDL_PushEvent(&key);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
    CHECK_FALSE(state.deciders.confirm_delete("really?", {}));
    const std::size_t frames = view.frames() - before;
    closed = true;
    escape.join();
    settle(view);  // geç kalan tuş olayları sonraki diyaloğa taşınmasın
    return frames;
}

}  // namespace

TEST_CASE_FIXTURE(WindowFixture, "an open dialog draws no frames while nothing happens") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    // İlk ölçüm açılış karelerinden ve karartma uyanmasından (kModalDimWake) sonra biter: yüklü makinede açılış
    // kareleri 100 ms'yi aşıp Esc'i yutuyordu (quick=3, slow=5). Eski hata (500 ms'de bir kare) 600'e karşı
    // 2500 ms'de de ≥ 3 fazla kare verir: sınama gücü aynı.
    const std::size_t quick = frames_in_idle_dialog(*state, *view, std::chrono::milliseconds(600));
    const std::size_t slow = frames_in_idle_dialog(*state, *view, std::chrono::milliseconds(2500));
    MESSAGE("dialog frames: quick=", quick, " slow=", slow);
    CHECK(quick > 0);
    CHECK(slow <= quick + 1);  // bekleme süresi kare sayısını değiştirmez (eskiden 500 ms'de bir kare)
}

// ImGui modal arka planını kareler arası süreyle karartır (DimBgRatio += DeltaTime * 6). Açılışın birkaç
// karesi milisaniyeler içinde biter; olay gelmezse karartma yarım kalmamalı.
TEST_CASE_FIXTURE(WindowFixture, "a dialog's dimmed background finishes fading in without events") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    struct Sample {
        std::chrono::steady_clock::time_point at;
        float dim = 0.0F;
    };
    std::vector<Sample> samples;
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_EndFramePre;
    hook.UserData = &samples;
    hook.Callback = [](ImGuiContext* context, ImGuiContextHook* self) {
        static_cast<std::vector<Sample>*>(self->UserData)
            ->push_back({std::chrono::steady_clock::now(), context->DimBgRatio});
    };
    const ImGuiID hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
    const auto opened = std::chrono::steady_clock::now();
    constexpr auto kIdle = std::chrono::milliseconds(800);
    frames_in_idle_dialog(*state, *view, kIdle);
    ImGui::RemoveContextHook(ImGui::GetCurrentContext(), hook_id);
    float dim_before_key = 0.0F;  // Esc gelmeden önceki karelerin en koyusu
    for (const Sample& sample : samples)
        if (sample.at < opened + kIdle)
            dim_before_key = std::max(dim_before_key, sample.dim);
    CHECK(dim_before_key == doctest::Approx(1.0F));
}

// Çöp görünümünde editör çizilmez: bir önceki karenin "editör odakta" bilgisi arama kutusunun imlecini
// durdurmamalı.
TEST_CASE_FIXTURE(WindowFixture, "the search box caret blinks in the trash view after the editor had focus") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();  // seçilen not incelemede açılır
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    state->open_trash();
    settle(*view);
    state->request_focus(FocusRequest::Search);
    settle(*view);
    REQUIRE(view->search_active());
    REQUIRE(ImGui::GetIO().WantTextInput);
    CHECK_FALSE(view->editor_active());
    for (int i = 0; i < 10; ++i)  // olaydan sonraki sabitlenme kareleri tükensin (0: imleç karesi yok)
        view->idle_step(0);
    const auto frames = view->frames();
    constexpr int kSteps = 5;
    for (int i = 0; i < kSteps; ++i)
        view->idle_step(1);
    CHECK(view->frames() == frames + kSteps);  // her uyanmada imleç karesi
}

// Boşta kare yok: fare bir düğmede durunca ipucu yine gecikmesi dolunca açılmalı (tek uyanma).
TEST_CASE_FIXTURE(WindowFixture,
                  "a tooltip appears when the mouse rests on a button with no further events") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    rest_mouse_on(*view, "sort");
    const gui_test::RenderedText rendered;
    const std::string tip = noteapp::core::tr("gui.sort_tip");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // RenderedText yetmez: ipucunun ilk (ölçüm) karesi gizlidir ama metni günlüğe düşer.
    while (!(rendered.contains(tip) && view->tooltip_visible()) && std::chrono::steady_clock::now() < until)
        view->idle_step(1);
    CHECK(rendered.contains(tip));
    REQUIRE(view->tooltip_visible());
    for (int i = 0; i < 10; ++i)  // olay ya da uyanmanın kalan kareleri tükensin
        view->idle_step(0);
    // İpucu açıldıktan sonra yine boşta kare yok.
    const auto frames = view->frames();
    idle_for(*view, std::chrono::milliseconds(800));
    CHECK(view->frames() == frames);
}

// Yük altında olay kareleri yavaşsa ImGui gecikmeyi (gerçek kareler arası süre) uyanmadan önce doldurur ve
// ipucu o karelerde açılır; bekleyen uyanma sonra boşta kare çizmemeli. Yavaş kare uyku ile kurulur.
// Gecikmeler sabittir; DelayNormal büyütülür: uyanmayı en uzun gecikmeye kuran eski hata, kare süresinden
// bağımsız olarak sabitlenme karelerinden sonra, ölçüm aralığında uyanırdı.
// Fare önce pencerenin dışındadır: konumu bilinmeyen fare düğmede "belirince" ImGui onu hareket saymaz, durma
// süresi açılıştan beri birikir; ipucunun açıldığı kare o zaman kurulum karelerinin hızına bağlı olurdu.
TEST_CASE_FIXTURE(WindowFixture, "a tooltip already shown by slow frames leaves no wake behind") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    ImGuiStyle& style = ImGui::GetStyle();
    constexpr float kTooltipDelaySeconds = 0.15F;  // SetItemTooltip: Stationary + DelayShort
    constexpr float kLongHoverDelaySeconds = 1.0F;
    style.HoverStationaryDelay = kTooltipDelaySeconds;
    style.HoverDelayShort = kTooltipDelaySeconds;
    style.HoverDelayNormal = kLongHoverDelaySeconds;
    move_mouse(-1.0F, -1.0F);
    settle(*view);
    rest_mouse_on(*view, "sort");
    const gui_test::RenderedText rendered;
    const std::string tip = noteapp::core::tr("gui.sort_tip");
    view->idle_step(1);  // olay karesi: fare düğmeye hareket etti, durma süresi sıfırlandı
    std::this_thread::sleep_for(std::chrono::duration<float>(2.0F * kTooltipDelaySeconds));
    view->idle_step(1);  // yavaş kare: gecikme ve durma süresi doldu
    view->idle_step(1);  // durma kilidi bu karenin başında açılır: ipucu ölçülür
    view->idle_step(1);  // ipucu görünür
    CHECK(rendered.contains(tip));
    REQUIRE(view->tooltip_visible());
    for (int i = 0; i < 10; ++i)  // olaydan sonraki sabitlenme kareleri tükensin
        view->idle_step(0);
    const auto frames = view->frames();
    idle_for(*view, std::chrono::milliseconds(1500));  // uyanma (olay karesi + 1 sn + 50 ms) bunun içinde
    CHECK(view->frames() == frames);
}

// Başka bir ipucu penceresi (editörün tamamlama listesi) açıkken de fare duran düğmenin ipucu, olay
// gelmeden, gecikmesi dolunca açılır.
TEST_CASE_FIXTURE(WindowFixture, "a tooltip appears on a resting mouse while the completion list is open") {
    add_note("söz.md", "alpine alpaca\n");
    auto state = make_state();
    auto view = make_view(*state);
    state->edit_selected();
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    press(*view, ImGuiMod_Ctrl | ImGuiKey_End);
    type_text(*view, "al");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Space);
    settle(*view);
    REQUIRE(view->item_rects().contains("editor_completion"));
    rest_mouse_on(*view, "sort");
    const gui_test::RenderedText rendered;
    const std::string tip = noteapp::core::tr("gui.sort_tip");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(rendered.contains(tip) && view->tooltip_visible()) && std::chrono::steady_clock::now() < until)
        view->idle_step(1);
    CHECK(rendered.contains(tip));
    CHECK(view->tooltip_visible());
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

// G5 (K2): üst Notlar/Ayarlar sekme satırı yok; Ayarlar araç çubuğundaki ⚙ ile ve Ctrl+, ile açılır, `←
// Notlar` ve ikinci Ctrl+, ile kapanır.
TEST_CASE_FIXTURE(WindowFixture, "Ctrl+, opens Settings and a second Ctrl+, returns to Notes") {
    add_note("alpha.md", "alpha body");
    auto state = make_state();
    auto view = make_view(*state);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Comma);
    CHECK(state->tab() == Tab::Settings);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Comma);
    CHECK(state->tab() == Tab::Notes);
}

TEST_CASE_FIXTURE(WindowFixture, "the gear button opens Settings and the back button returns to Notes") {
    add_note("alpha.md", "alpha body");
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    REQUIRE(view->item_rects().contains("settings_open"));
    CHECK_FALSE(view->item_rects().contains("settings_back"));
    gui_test::click_button(*view, "settings_open");
    CHECK(state->tab() == Tab::Settings);
    REQUIRE(view->item_rects().contains("settings_back"));
    CHECK_FALSE(view->item_rects().contains("toolbar_new_note"));  // Ayarlar tüm içerik alanıdır
    gui_test::click_button(*view, "settings_back");
    CHECK(state->tab() == Tab::Notes);
    CHECK(view->item_rects().contains("toolbar_new_note"));
}
