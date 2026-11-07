// G2: çoklu seçim (ağaç, Ayrıntılar ve çöp), işaretlilerde toplu çöp/geri getir/kalıcı sil, yank/yapıştır ve
// klasör taşıma/kopyalama. Kurallar app::TrashOps ve app::item_ops'tadır; burada GUI bağı ve tuşlar sınanır.
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;

namespace {

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

struct MarksScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;
    int asked = 0;
    bool answer = true;

    MarksScene() {
        fixture.add_note("a.md", "a\n", 20);
        fixture.add_note("b.md", "b\n", 10);
        fixture.add_note("work/x.md", "x\n", 30);
        fixture.add_note(".trash/t1.md", "t1\n", 40);
        fixture.add_note(".trash/t2.md", "t2\n", 50);
        state = fixture.make_state();
        view = make_view(*state);
        state->deciders.confirm_delete = [this](const std::string&, const std::string&) {
            ++asked;
            return answer;
        };
        settle(*view);
    }
    void open(const std::string& name) const {
        state->open_note(*noteapp::core::NotePath::parse(name));
        focus_list();
    }
    void focus_list() const {
        state->request_focus(FocusRequest::List);
        settle(*view);
    }
};

}  // namespace

TEST_CASE("M marks the row and moves down, Shift+M clears, Ctrl+A marks every row but the root") {
    MarksScene s;
    s.open("b.md");
    press(*s.view, ImGuiKey_M);
    CHECK(s.state->is_marked("b.md", false));
    REQUIRE(s.state->current_path().has_value());
    CHECK(s.state->current_path()->string() == "a.md");  // TUI `x` gibi bir aşağı
    CHECK(s.state->marked_count() == 1);
    press(*s.view, ImGuiKey_M);
    CHECK(s.state->is_marked("a.md", false));
    press(*s.view, ImGuiMod_Shift | ImGuiKey_M);
    CHECK(s.state->marked_count() == 0);
    press(*s.view, ImGuiMod_Ctrl | ImGuiKey_A);
    CHECK(s.state->is_marked("work", true));
    CHECK(s.state->is_marked("a.md", false));
    CHECK(s.state->is_marked("b.md", false));
    CHECK_FALSE(s.state->is_marked("", true));
    CHECK(s.state->marked_count() == 3);
}

TEST_CASE("Delete on marks trashes them as one undo step; Undo brings them all back") {
    MarksScene s;
    s.open("b.md");
    run_command(*s.state, "mark");
    run_command(*s.state, "mark");  // b.md, a.md
    press(*s.view, ImGuiKey_Delete);
    CHECK(s.fixture.note_exists(".trash/a.md"));
    CHECK(s.fixture.note_exists(".trash/b.md"));
    CHECK(s.asked == 0);  // notlar geri alınır: sorulmaz
    CHECK(s.state->marked_count() == 0);
    s.state->undo_trash();
    CHECK(s.fixture.note_exists("a.md"));
    CHECK(s.fixture.note_exists("b.md"));
}

TEST_CASE("Shift+Delete on marks asks once and deletes them for good") {
    MarksScene s;
    s.open("b.md");
    run_command(*s.state, "mark");
    run_command(*s.state, "mark");
    press(*s.view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK(s.asked == 1);
    CHECK_FALSE(s.fixture.note_exists("a.md"));
    CHECK_FALSE(s.fixture.note_exists("b.md"));
    CHECK_FALSE(s.fixture.note_exists(".trash/a.md"));
    CHECK(s.fixture.note_exists("work/x.md"));
}

// Onay açıkken senkron (burada tarama) işaretlileri düşürür ya da aynı yola başka not koyar: Evet yalnız
// sorulan hâli siler, başka hiçbir şeyi (app::TrashOps, TUI ile aynı kural).
TEST_CASE("a permanent delete removes only what the confirmation showed") {
    MarksScene s;
    s.open("b.md");
    run_command(*s.state, "mark");
    run_command(*s.state, "mark");
    s.state->deciders.confirm_delete = [&s](const std::string&, const std::string&) {
        noteapp::core::NoteStore store(s.fixture.store_dir());
        REQUIRE(store.remove(*noteapp::core::NotePath::parse("a.md")));
        REQUIRE(store.remove(*noteapp::core::NotePath::parse("b.md")));
        s.fixture.add_note("b.md", "another note at the same path\n");
        s.state->rescan();
        return true;
    };
    press(*s.view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK(s.fixture.note_exists("b.md"));
    CHECK(s.fixture.note_exists("work/x.md"));

    s.open("work/x.md");
    s.state->deciders.confirm_delete = [&s](const std::string&, const std::string&) {
        s.fixture.add_note("work/x.md", "changed by sync\n");
        return true;
    };
    s.state->delete_note();
    CHECK(s.fixture.note_exists("work/x.md"));
}

TEST_CASE("in the trash, marks are their own set; restore and purge act on all marked items") {
    MarksScene s;
    s.open("b.md");
    run_command(*s.state, "mark");
    s.state->open_trash();
    s.focus_list();
    CHECK(s.state->marked_count() == 0);  // listenin işaretleri çöpe taşınmaz
    press(*s.view, ImGuiMod_Ctrl | ImGuiKey_A);
    CHECK(s.state->is_marked("t1.md", false));
    CHECK(s.state->is_marked("t2.md", false));
    run_command(*s.state, "restore");
    CHECK(s.fixture.note_exists("t1.md"));
    CHECK(s.fixture.note_exists("t2.md"));
    CHECK(s.state->trash_items().empty());

    s.fixture.add_note(".trash/t3.md", "t3\n", 60);
    s.fixture.add_note(".trash/t4.md", "t4\n", 70);
    s.state->rescan();
    run_command(*s.state, "mark.all");
    run_command(*s.state, "delete");  // çöpte kalıcı silme
    CHECK(s.asked == 1);
    CHECK(s.state->trash_items().empty());
    s.state->close_trash();
    CHECK(s.state->is_marked("b.md", false));  // liste kümesi yerinde
}

TEST_CASE("Y yanks, P pastes into the selected folder; a taken name gets -2 and folders copy too") {
    MarksScene s;
    s.open("b.md");
    press(*s.view, ImGuiKey_Y);
    CHECK(s.state->has_yank());
    s.state->select_folder("work");
    s.focus_list();
    press(*s.view, ImGuiKey_P);
    CHECK(s.fixture.read_note("work/b.md") == "b\n");
    CHECK(s.fixture.note_exists("b.md"));  // kopya: kaynak yerinde
    CHECK_FALSE(s.state->has_yank());      // TUI gibi yapıştırınca biter
    s.open("b.md");
    run_command(*s.state, "yank");
    s.state->select_folder("work");
    run_command(*s.state, "paste");
    CHECK(s.fixture.read_note("work/b-2.md") == "b\n");

    // Klasör kopyası (matris 74): yank'lenen klasör alt ağacıyla, dolu adda `-2`.
    s.state->select_folder("work");
    run_command(*s.state, "yank");
    CHECK(s.state->has_yank());
    s.state->select_folder("");
    run_command(*s.state, "paste");
    INFO(s.state->status());
    CHECK(s.fixture.read_note("work-2/x.md") == "x\n");
    CHECK(s.fixture.read_note("work-2/b-2.md") == "b\n");
}

TEST_CASE("Ctrl+M on a folder row moves the folder; marked notes move together") {
    MarksScene s;
    std::vector<SaveDialogRequest> asked;
    s.state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        asked.push_back(request);
        return std::optional<SaveDetails>(SaveDetails{"ignored", "dest", "md"});
    };
    s.state->select_folder("work");
    s.focus_list();
    CHECK(s.state->can_move());  // araç çubuğu Taşı klasör satırında da açık (matris 72)
    press(*s.view, ImGuiMod_Ctrl | ImGuiKey_M);
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().folder_only);
    CHECK(s.fixture.read_note("dest/work/x.md") == "x\n");
    CHECK_FALSE(s.fixture.note_exists("work/x.md"));

    s.open("b.md");
    run_command(*s.state, "mark");
    run_command(*s.state, "mark");
    run_command(*s.state, "move");
    CHECK(s.fixture.read_note("dest/a.md") == "a\n");
    CHECK(s.fixture.read_note("dest/b.md") == "b\n");
    CHECK(s.state->marked_count() == 0);
}

// G4 (G2+G3 review): işaret sayısı durum çubuğundadır (TUI `x`'in alt satırdaki sayacı).
TEST_CASE("the status bar shows how many rows are marked") {
    MarksScene s;
    s.open("b.md");
    const std::string marked = noteapp::core::tr("app.marked_count", {{"count", "2"}});
    CHECK(s.state->count_label().find(marked) == std::string::npos);
    run_command(*s.state, "mark");
    run_command(*s.state, "mark");
    CHECK(s.state->count_label().find(marked) != std::string::npos);
    gui_test::RenderedText text;
    settle(*s.view);
    CHECK(text.contains(marked));
    run_command(*s.state, "mark.clear");
    CHECK(s.state->count_label().find(marked) == std::string::npos);
}

// G4 (G2+G3 review): işaretliler arasındaki gösterilen not tek notluk Delete gibi açıkça bırakılır: sekmesi
// "artık yok" diye kapanmaz, imleç aynı sıradaki nota iner ve editör onu gösterir.
TEST_CASE("Delete on marks that include the shown note releases it like a single Delete") {
    WindowFixture fixture;
    for (int age = 1; age <= 5; ++age)  // n1 en yeni: ağaçta kökten sonra n1..n5
        fixture.add_note("n" + std::to_string(age) + ".md", "n" + std::to_string(age) + "\n", age * 10);
    auto state = fixture.make_state();
    auto view = make_view(*state);
    state->open_note(*noteapp::core::NotePath::parse("n2.md"));
    run_command(*state, "mark");  // n2, imleç n3'e
    run_command(*state, "mark");  // n3, imleç n4'e
    state->open_note(*noteapp::core::NotePath::parse("n3.md"));
    REQUIRE(state->marked_count() == 2);
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_Delete);
    REQUIRE(fixture.note_exists(".trash/n3.md"));
    CHECK(state->status().find(noteapp::core::tr("gui.batch_trashed", {{"count", "2"}})) !=
          std::string::npos);
    for (const auto& tab : state->tabs())
        CHECK(tab.path != "n3.md");
    REQUIRE(state->current_path().has_value());
    CHECK(state->current_path()->string() == "n5.md");  // n3'ün sırası: kök, n1, n4, [n5]
    CHECK(state->buffer() == "n5\n");
}
