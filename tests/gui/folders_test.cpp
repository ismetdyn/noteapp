#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <string>
#include <tuple>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::core::tr;
using noteapp::gui::ConflictChoice;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;
namespace fs = std::filesystem;

namespace {

std::string current(const GuiState& state) {
    const auto path = state.current_path();
    return path ? path->string() : std::string{};
}

bool has_folder(const GuiState& state, const std::string& folder) {
    return std::ranges::find(state.folders(), folder) != state.folders().end();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct FolderFixture : WindowFixture {
    std::optional<std::string> name_answer;
    bool delete_answer = false;
    int confirms = 0;
    std::string last_confirm;
    std::string last_prompt_initial;
    int prompts = 0;
    std::optional<SaveDetails> move_answer;
    int move_dialogs = 0;
    SaveDialogRequest move_request;

    void wire(GuiState& state) {
        state.deciders.folder_name = [this](const std::string&, const std::string& initial) {
            ++prompts;
            last_prompt_initial = initial;
            return name_answer;
        };
        state.deciders.confirm_delete = [this](const std::string& message) {
            ++confirms;
            last_confirm = message;
            return delete_answer;
        };
        state.deciders.save_dialog = [this](const SaveDialogRequest& request) {
            ++move_dialogs;
            move_request = request;
            return move_answer;
        };
        state.deciders.conflict = [](const std::string&) { return ConflictChoice::Cancel; };
    }
    bool dir_exists(const std::string& relative) const {
        return noteapp::test::folder_exists(store_dir(), relative);
    }
};

// Tuşlar diyalog karelerinde verilir; gerçek ImGui modalı böyle sınanır.
struct DialogScript {
    int frame = 0;
    const char* text = nullptr;  // varsa alana yazılır
    ImGuiKey answer = ImGuiKey_Enter;

    void operator()() {
        constexpr int kTypeFrame = 5;  // odak ve kısayol rotası yerleşsin
        constexpr int kGiveUpFrame = 200;
        ImGuiIO& io = ImGui::GetIO();
        ++frame;
        if (frame == 1) {  // diyaloğu açan kısayolun tuşları bırakılır
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiKey_M, false);
        }
        if (frame == kTypeFrame && text != nullptr)
            io.AddInputCharactersUTF8(text);
        if (frame == kTypeFrame + 2)
            gui_test::tap_key(answer);
        if (frame == kGiveUpFrame)  // yanıt tutmazsa test asılı kalmasın, sonucu tutmadığı için düşsün
            gui_test::tap_key(ImGuiKey_Escape);
    }
};

}  // namespace

// ---- SPEC-gui kabul 4 ----

TEST_CASE_FIXTURE(FolderFixture, "deleting a non-empty folder never happens without confirmation") {
    add_note("work/a.md", "a", 10);
    add_note("work/b.md", "b", 20);
    auto state = make_state();
    wire(*state);

    delete_answer = false;
    state->delete_folder("work");
    CHECK(confirms == 1);
    CHECK(note_exists("work/a.md"));
    CHECK(note_exists("work/b.md"));
    CHECK(dir_exists("work"));
    // Onay metni klasör adını ve not sayısını taşır.
    CHECK(last_confirm == tr("gui.confirm_delete_folder", {{"folder", "work"}, {"count", "2"}}));

    delete_answer = true;
    state->delete_folder("work");
    CHECK(confirms == 2);
    CHECK_FALSE(dir_exists("work"));
    CHECK(state->folders().empty());
}

TEST_CASE_FIXTURE(FolderFixture, "deleting an empty folder asks a simple confirmation") {
    auto state = make_state();
    wire(*state);
    name_answer = "boş";
    state->new_folder("");
    REQUIRE(dir_exists("boş"));

    delete_answer = false;
    state->delete_folder("boş");
    CHECK(dir_exists("boş"));
    CHECK(last_confirm == tr("gui.confirm_delete_empty_folder", {{"folder", "boş"}}));

    delete_answer = true;
    state->delete_folder("boş");
    CHECK_FALSE(dir_exists("boş"));
}

TEST_CASE_FIXTURE(FolderFixture, "deleting an unknown folder reports the error and touches nothing") {
    add_note("work/a.md", "a", 10);
    auto state = make_state();
    wire(*state);
    // contains_link yolu ayrıcalık ister (Windows'ta bağ oluşturma); hata gösterimi zaten-yok klasörle
    // denenir.
    delete_answer = true;
    state->delete_folder("ghost");
    CHECK(contains(state->status(), "ghost"));
    CHECK(note_exists("work/a.md"));
}

// ---- Oluşturma / yeniden adlandırma ----

TEST_CASE_FIXTURE(FolderFixture,
                  "new folder is created under root or the clicked folder, Turkish names included") {
    add_note("work/a.md", "a");
    auto state = make_state();
    wire(*state);

    name_answer = "Öğrenci Notları";
    state->new_folder("");
    CHECK(dir_exists("Öğrenci Notları"));

    name_answer = "ışık";
    state->new_folder("work");
    CHECK(dir_exists("work/ışık"));
    CHECK(has_folder(*state, "work/ışık"));
    CHECK(has_folder(*state, "Öğrenci Notları"));
}

TEST_CASE_FIXTURE(FolderFixture, "new folder: cancel and collision change nothing and report") {
    add_note("work/a.md", "a");
    auto state = make_state();
    wire(*state);

    name_answer = std::nullopt;
    state->new_folder("");
    CHECK(prompts == 1);

    name_answer = "work";
    state->new_folder("");
    CHECK(contains(state->status(), tr("error.already_exists", {{"path", "work"}})));

    name_answer = "a/../b";  // geçersiz yol çekirdekte reddedilir
    state->new_folder("");
    CHECK_FALSE(dir_exists("b"));
    CHECK_FALSE(state->status().empty());
}

TEST_CASE_FIXTURE(FolderFixture,
                  "rename folder moves notes, offers the old leaf name and rejects collisions") {
    add_note("work/a.md", "a", 10);
    add_note("other/b.md", "b", 20);
    auto state = make_state();
    wire(*state);

    name_answer = "çalışma";
    state->rename_folder("work");
    CHECK(last_prompt_initial == "work");
    CHECK(note_exists("çalışma/a.md"));
    CHECK_FALSE(note_exists("work/a.md"));

    name_answer = "other";  // çakışma: hiçbir şey taşınmaz
    state->rename_folder("çalışma");
    CHECK(note_exists("çalışma/a.md"));
    CHECK(note_exists("other/b.md"));
    CHECK(contains(state->status(), "other"));
}

TEST_CASE_FIXTURE(FolderFixture,
                  "renaming the folder of the shown dirty note flushes first and keeps state consistent") {
    add_note("work/a.md", "hello", 10);
    auto state = make_state();
    wire(*state);
    REQUIRE(current(*state) == "work/a.md");

    state->edit_text("hello world");  // kirli tampon
    const auto writes = state->write_count();
    name_answer = "proj";
    state->rename_folder("work");

    CHECK(state->write_count() == writes + 1);  // önce kaydedildi
    CHECK(read_note("proj/a.md") == "hello world");
    CHECK(current(*state) == "proj/a.md");
    CHECK(contains(state->header(), "proj/a.md"));
    CHECK(contains(state->header(), tr("gui.state_saved")));
    CHECK(state->buffer() == "hello world");

    // shown_ tazelendi: sonraki kayıt çakışma diyaloğu istemeden yeni yola yazar.
    state->edit_text("second");
    state->save_now();
    CHECK(read_note("proj/a.md") == "second");
}

TEST_CASE_FIXTURE(FolderFixture, "renaming the selected folder keeps it selected under its new name") {
    add_note("work/a.md", "hello", 10);
    add_note("keep.md", "keep", 20);
    auto state = make_state();
    wire(*state);
    state->set_folder("work");
    REQUIRE(state->model().row_count() == 1);

    name_answer = "proj";
    state->rename_folder("work");
    CHECK(state->current_folder() == "proj");
    CHECK(state->model().row_count() == 1);
}

TEST_CASE_FIXTURE(FolderFixture,
                  "deleting the folder of the shown dirty note flushes first, then clears the editor") {
    add_note("work/a.md", "hello", 10);
    add_note("keep.md", "keep", 20);
    auto state = make_state();
    wire(*state);
    REQUIRE(current(*state) == "work/a.md");
    state->edit_text("edited");

    delete_answer = false;  // vazgeçildi: kaydedilmez, tampon yerinde
    state->delete_folder("work");
    CHECK(state->write_count() == 0);
    CHECK(state->buffer() == "edited");

    delete_answer = true;
    state->delete_folder("work");
    CHECK(state->write_count() == 1);  // silmeden önce flush edildi
    CHECK_FALSE(dir_exists("work"));
    CHECK(current(*state) == "keep.md");
    CHECK(state->buffer() == "keep");
    CHECK(contains(state->header(), "keep.md"));
}

TEST_CASE_FIXTURE(FolderFixture, "a failing flush aborts folder rename and delete") {
    add_note("work/a.md", "original", 10);
    auto state = make_state();
    wire(*state);
    state->edit_text("mine");
    add_note("work/a.md", "external", 0);  // çakışma; karar Vazgeç

    delete_answer = true;
    state->delete_folder("work");
    CHECK(note_exists("work/a.md"));
    name_answer = "proj";
    state->rename_folder("work");
    CHECK(note_exists("work/a.md"));
    CHECK_FALSE(dir_exists("proj"));
    CHECK(state->buffer() == "mine");
}

// ---- Not silme ----

TEST_CASE_FIXTURE(FolderFixture, "delete confirmation box defaults to Cancel") {
    const auto box = noteapp::gui::make_delete_box("x");
    CHECK(box.buttons.at(box.default_button).first == tr("gui.cancel"));
    CHECK(box.escape_button == box.default_button);
    CHECK(box.message == "x");
}

TEST_CASE_FIXTURE(FolderFixture,
                  "delete note asks confirmation, deletes only when confirmed, selects the neighbour") {
    add_note("n1.md", "one", 0);
    add_note("n2.md", "two", 10);
    add_note("n3.md", "three", 20);
    auto state = make_state();
    wire(*state);
    state->select_row(1);
    REQUIRE(current(*state) == "n2.md");

    delete_answer = false;
    state->delete_note();
    CHECK(last_confirm == tr("gui.confirm_delete_note", {{"path", "n2.md"}}));
    CHECK(note_exists("n2.md"));
    CHECK(current(*state) == "n2.md");

    delete_answer = true;
    state->delete_note();
    CHECK_FALSE(note_exists("n2.md"));
    CHECK(current(*state) == "n3.md");  // komşu satır
    CHECK(state->buffer() == "three");

    state->delete_note();  // son satır silinince bir üstteki
    CHECK(current(*state) == "n1.md");
    state->delete_note();
    CHECK(current(*state).empty());
    CHECK(state->buffer().empty());
    CHECK(state->header().empty());
}

TEST_CASE_FIXTURE(FolderFixture,
                  "permanent delete by shortcut and by context menu goes through the confirmation decider") {
    add_note("n1.md", "one");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    delete_answer = false;

    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK(confirms == 1);
    CHECK(note_exists("n1.md"));

    // Bağlam yolu: klavyeyle (aramadan Shift+Tab listeye, Enter içine, Shift+F10 menü, üçüncü öğe "kalıcı
    // sil").
    state->focus_search();
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_Tab);
    settle(*view);
    press(*view, ImGuiKey_Enter);
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_F10);
    settle(*view);
    press(*view, ImGuiKey_DownArrow);  // menü imleci ilk öğede açılır: taşı, çöpe at, kalıcı sil
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_Enter);
    settle(*view);
    CHECK(confirms == 2);
    CHECK(note_exists("n1.md"));
}

TEST_CASE_FIXTURE(FolderFixture,
                  "the folder context menu opens from the keyboard and renames through the prompt") {
    add_note("work/a.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    name_answer = "proj";

    // Arama → liste → ağaç (Shift+Tab iki kez), Enter içine; imleç "Tümü"de başlar, ok "work"e iner.
    state->focus_search();
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_Tab);
    settle(*view);
    press(*view, ImGuiMod_Shift | ImGuiKey_Tab);
    settle(*view);
    press(*view, ImGuiKey_Enter);
    settle(*view);
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiMod_Shift | ImGuiKey_F10);
    settle(*view);
    press(*view, ImGuiKey_DownArrow);  // menü imleci "Yeni klasör"de açılır: alttaki "Yeniden adlandır"
    press(*view, ImGuiKey_Enter);
    settle(*view);
    CHECK(prompts == 1);
    CHECK(last_prompt_initial == "work");
    CHECK(note_exists("proj/a.md"));
}

TEST_CASE_FIXTURE(FolderFixture,
                  "the folder name dialog: Enter accepts the typed name, Escape cancels, blank cancels") {
    add_note("work/a.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();

    DialogScript accept;
    accept.text = "yeni";
    view->on_modal_frame = accept;
    state->new_folder("work");
    CHECK(dir_exists("work/yeni"));
    // Enter basılı kalırsa KeyRepeatDelay dolduğunda sonraki diyaloğu da yanıtlar.
    view->draw_frame();
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Enter));

    DialogScript cancel;
    cancel.text = "iptal";
    cancel.answer = ImGuiKey_Escape;
    view->on_modal_frame = cancel;
    state->new_folder("");
    CHECK_FALSE(dir_exists("iptal"));

    DialogScript blank;
    blank.text = "   ";
    view->on_modal_frame = blank;
    const auto before = state->folders();
    state->new_folder("");
    CHECK(state->folders() == before);
}

TEST_CASE_FIXTURE(FolderFixture,
                  "deleting the shown dirty note flushes first and keeps it on a failing flush") {
    add_note("n1.md", "original", 10);
    auto state = make_state();
    wire(*state);
    state->edit_text("mine");
    add_note("n1.md", "external", 0);  // çakışma, karar Vazgeç -> flush başarısız
    delete_answer = true;
    state->delete_note();
    CHECK(note_exists("n1.md"));
    CHECK(state->buffer() == "mine");
}

// ---- Ctrl+M ----

TEST_CASE_FIXTURE(FolderFixture, "move note changes only the folder (name and type are locked)") {
    add_note("work/a.md", "alpha", 10);
    add_note("dest/x.md", "x", 20);
    auto state = make_state();
    wire(*state);
    state->select_row(0);
    REQUIRE(current(*state) == "work/a.md");

    move_answer = std::nullopt;  // vazgeç
    state->move_note();
    CHECK(note_exists("work/a.md"));
    CHECK(move_request.mode == SaveDialogRequest::Mode::Move);
    CHECK(move_request.folder_only);
    CHECK(move_request.initial.folder == "work");

    state->edit_text("alpha edited");
    move_answer = SaveDetails{"ignored", "dest", "txt"};
    state->move_note();
    CHECK(read_note("dest/a.md") == "alpha edited");  // flush edildi, ad/tip korundu
    CHECK_FALSE(note_exists("work/a.md"));
    CHECK(current(*state) == "dest/a.md");
    CHECK(contains(state->header(), "dest/a.md"));

    // shown_ tutarlı: sonraki kayıt yeni yola gider.
    state->edit_text("again");
    state->save_now();
    CHECK(read_note("dest/a.md") == "again");
}

TEST_CASE_FIXTURE(FolderFixture, "Ctrl+M shortcut opens the move dialog") {
    add_note("a.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_M);
    CHECK(move_dialogs == 1);
}

TEST_CASE_FIXTURE(FolderFixture, "Ctrl+M dialog ignores clicks and typing on the locked name and type") {
    add_note("work/a.md", "alpha", 10);
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    std::optional<SaveDetails> answered;
    const auto real_dialog = state->deciders.save_dialog;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        answered = real_dialog(request);
        return answered;
    };
    // Sıra: ad, klasör, tip; adın ortası tip alanına da düşer (tip alanı yalnız ok genişliği kadar dar).
    constexpr int kRowsFromNameToType = 2;
    float name_x = 0;
    float name_y = 0;
    view->on_modal_frame = [&, frame = 0]() mutable {
        ImGuiIO& io = ImGui::GetIO();
        ++frame;
        const float row = ImGui::GetFrameHeightWithSpacing();
        switch (frame) {
        case 1:  // diyaloğu açan kısayolun tuşları bırakılır
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiKey_M, false);
            break;
        case 5:
            std::tie(name_x, name_y) = view->button_center("save_name");
            io.AddMousePosEvent(name_x, name_y);
            break;
        case 6:
        case 13:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            break;
        case 7:
        case 14:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            break;
        case 9:
            io.AddInputCharactersUTF8("n");
            break;
        case 12:
            io.AddMousePosEvent(name_x, name_y + kRowsFromNameToType * row);
            break;
        case 16:
            io.AddInputCharactersUTF8("t");
            break;
        case 19:
            io.AddKeyEvent(ImGuiKey_Enter, true);
            break;
        case 20:
            io.AddKeyEvent(ImGuiKey_Enter, false);
            break;
        case 200:  // yanıt tutmazsa test asılı kalmasın, sonucu tutmadığı için düşsün
            io.AddKeyEvent(ImGuiKey_Escape, true);
            break;
        default:
            break;
        }
    };

    press(*view, ImGuiMod_Ctrl | ImGuiKey_M);

    REQUIRE(answered.has_value());
    CHECK(answered->name == "a");
    CHECK(answered->type == "md");
}

TEST_CASE_FIXTURE(FolderFixture, "folder-only save dialog locks name and type") {
    add_note("work/a.md", "alpha", 10);
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    std::optional<SaveDetails> answered;
    const auto real_dialog = state->deciders.save_dialog;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        answered = real_dialog(request);
        return answered;
    };

    // İlk odak klasör alanındadır; yazılan yalnız oraya gider, ad ve tip başlangıç değerinde kalır.
    DialogScript script;
    script.text = "dest";
    view->on_modal_frame = script;
    state->move_note();
    REQUIRE(answered.has_value());
    CHECK(answered->name == "a");
    CHECK(answered->type == "md");
    CHECK(answered->folder == "dest");
}
