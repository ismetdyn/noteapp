#include "graph/graph.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "layout_scenes.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <imgui.h>
#include <memory>
#include <string>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;

namespace {

constexpr const char* kDelete = "toolbar_delete";
constexpr const char* kTrash = "toolbar_trash";
constexpr const char* kUndo = "status_undo";
constexpr const char* kRestore = "trash_restore";
constexpr const char* kPurge = "trash_purge";
constexpr const char* kEmpty = "trash_empty";
constexpr const char* kBack = "trash_back";

struct TrashScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;
    int asked = 0;
    bool answer = false;

    TrashScene() {
        fixture.add_note("a.md", "a\n", 20);
        fixture.add_note("b.md", "b\n", 10);
        state = fixture.make_state();
        view = make_view(*state);
        state->deciders.confirm_delete = [this](const std::string&) {
            ++asked;
            return answer;
        };
        settle(*view);
    }
    void select(const std::string& name) const {
        state->select_row(state->model().index_of(*noteapp::core::NotePath::parse(name)));
        settle(*view);
    }
    bool has_item(const std::string& name) const { return view->item_rects().contains(name); }
};

bool shows(View& view, const std::string& text) {
    const gui_test::RenderedText rendered;
    view.draw_frame();
    return rendered.contains(text);
}

}  // namespace

TEST_CASE("the status bar offers undo after Delete and undo brings the note back") {
    TrashScene scene;
    scene.select("b.md");
    CHECK_FALSE(scene.has_item(kUndo));
    click_button(*scene.view, kDelete);
    CHECK(scene.fixture.note_exists(".trash/b.md"));
    CHECK(scene.state->status().find("b.md") != std::string::npos);
    settle(*scene.view);
    REQUIRE(scene.has_item(kUndo));
    click_button(*scene.view, kUndo);
    CHECK(scene.fixture.note_exists("b.md"));
    CHECK_FALSE(scene.fixture.note_exists(".trash/b.md"));
    REQUIRE(scene.state->current_path().has_value());
    CHECK(scene.state->current_path()->string() == "b.md");
    settle(*scene.view);
    CHECK_FALSE(scene.has_item(kUndo));
}

TEST_CASE("a later message takes the undo offer away with it") {
    TrashScene scene;
    click_button(*scene.view, kDelete);
    settle(*scene.view);
    REQUIRE(scene.has_item(kUndo));
    scene.state->show_config_warnings(
        {noteapp::core::Error{noteapp::core::Error::Code::ConfigInvalid, "config.defaults_unusable", {}}});
    settle(*scene.view);
    CHECK_FALSE(scene.has_item(kUndo));
    CHECK_FALSE(scene.state->can_undo_trash());
}

TEST_CASE("trashing the open note saves its edits first; they are in the trashed file") {
    TrashScene scene;
    scene.select("b.md");
    scene.state->edit_text("yeni metin\n");
    REQUIRE(scene.state->dirty());
    click_button(*scene.view, kDelete);
    CHECK(scene.fixture.read_note(".trash/b.md") == "yeni metin\n");
    CHECK_FALSE(scene.state->dirty());
}

TEST_CASE("the selection stays on a neighbour after Delete, and the shown note follows") {
    TrashScene scene;
    scene.select("b.md");
    click_button(*scene.view, kDelete);
    REQUIRE(scene.state->current_path());
    CHECK(scene.state->current_path()->string() == "a.md");
    CHECK(scene.state->buffer() == "a\n");
}

TEST_CASE("the trash button shows the count and opens the trash view") {
    TrashScene scene;
    CHECK(shows(*scene.view, "Trash (0)"));
    click_button(*scene.view, kDelete);
    CHECK(scene.state->trash_count() == 1);
    CHECK(shows(*scene.view, "Trash (1)"));
    click_button(*scene.view, kTrash);
    CHECK(scene.state->trash_view());
    settle(*scene.view);
    CHECK(shows(*scene.view, "b.md"));
    CHECK(scene.has_item(kRestore));
    CHECK(scene.has_item(kPurge));
    CHECK(scene.has_item(kEmpty));
    click_button(*scene.view, kTrash);  // ikinci basış çöpten çıkar
    CHECK_FALSE(scene.state->trash_view());
}

TEST_CASE("restore from the trash view brings the selected note back and stays in the view") {
    TrashScene scene;
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kDelete);
    REQUIRE(scene.state->trash_count() == 2);
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    const std::string first = scene.state->trash_items().front().path.string();
    click_button(*scene.view, kRestore);
    CHECK(scene.fixture.note_exists(first));
    CHECK(scene.state->trash_view());
    CHECK(scene.state->trash_count() == 1);
    CHECK(scene.state->trash_row() == 0);
    click_button(*scene.view, kBack);
    CHECK_FALSE(scene.state->trash_view());
    CHECK(scene.state->model().index_of(*noteapp::core::NotePath::parse(first)) >= 0);
}

TEST_CASE("Delete permanently asks first; only Yes removes the file") {
    TrashScene scene;
    scene.select("b.md");
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    click_button(*scene.view, kPurge);
    CHECK(scene.asked == 1);
    CHECK(scene.fixture.note_exists(".trash/b.md"));
    scene.answer = true;
    click_button(*scene.view, kPurge);
    CHECK(scene.asked == 2);
    CHECK_FALSE(scene.fixture.note_exists(".trash/b.md"));
    CHECK(scene.state->trash_count() == 0);
}

TEST_CASE("Del and Shift+Del in the trash view purge the selected item after asking") {
    for (const ImGuiKeyChord chord : {ImGuiKeyChord(ImGuiKey_Delete), ImGuiMod_Shift | ImGuiKey_Delete}) {
        TrashScene scene;
        scene.select("b.md");
        click_button(*scene.view, kDelete);
        click_button(*scene.view, kTrash);
        settle(*scene.view);
        scene.state->request_focus(FocusRequest::List);
        settle(*scene.view);
        press(*scene.view, chord);
        CHECK(scene.asked == 1);
        CHECK(scene.fixture.note_exists(".trash/b.md"));
        scene.answer = true;
        press(*scene.view, chord);
        CHECK_FALSE(scene.fixture.note_exists(".trash/b.md"));
    }
}

TEST_CASE("Empty asks first and removes everything; the default dialog answer is Cancel") {
    TrashScene scene;
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    click_button(*scene.view, kEmpty);
    CHECK(scene.asked == 1);
    CHECK(scene.state->trash_count() == 2);

    scene.view->install_deciders();
    int frame = 0;
    scene.view->on_modal_frame = [&] {
        ++frame;
        constexpr int kAnswerFrame = 5;
        if (frame == kAnswerFrame)
            gui_test::tap_key(ImGuiKey_Enter);
    };
    click_button(*scene.view, kEmpty);
    scene.view->on_modal_frame = nullptr;
    CHECK(frame > 0);
    CHECK(scene.state->trash_count() == 2);

    scene.state->deciders.confirm_delete = [](const std::string&) { return true; };
    click_button(*scene.view, kEmpty);
    CHECK(scene.state->trash_count() == 0);
    CHECK_FALSE(scene.fixture.note_exists(".trash/a.md"));
    CHECK_FALSE(scene.fixture.note_exists(".trash/b.md"));
}

TEST_CASE("the trash buttons turn off when the trash is empty") {
    TrashScene scene;
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    CHECK(scene.view->item_rects().at(kRestore).disabled);
    CHECK(scene.view->item_rects().at(kPurge).disabled);
    CHECK(scene.view->item_rects().at(kEmpty).disabled);
    CHECK(scene.view->item_rects().at(kDelete).disabled);
}

TEST_CASE("up and down walk the trash rows, not the note list") {
    TrashScene scene;
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    const int live_row = scene.state->current_row();
    REQUIRE(scene.state->trash_row() == 0);
    scene.state->request_focus(FocusRequest::List);
    settle(*scene.view);
    press(*scene.view, ImGuiKey_DownArrow);
    CHECK(scene.state->trash_row() == 1);
    press(*scene.view, ImGuiKey_DownArrow);
    CHECK(scene.state->trash_row() == 1);
    press(*scene.view, ImGuiKey_UpArrow);
    CHECK(scene.state->trash_row() == 0);
    CHECK(scene.state->current_row() == live_row);
}

TEST_CASE("a rescan picks up trash changes made elsewhere and keeps the view consistent") {
    TrashScene scene;
    click_button(*scene.view, kDelete);
    click_button(*scene.view, kTrash);
    settle(*scene.view);
    REQUIRE(scene.state->trash_count() == 1);
    scene.fixture.add_note(".trash/uzaktan.md", "u\n");
    scene.state->rescan();
    CHECK(scene.state->trash_count() == 2);
    CHECK(scene.state->trash_items().size() == 2);
}

TEST_CASE("typing a search or choosing a folder leaves the trash view") {
    TrashScene scene;
    click_button(*scene.view, kTrash);
    REQUIRE(scene.state->trash_view());
    scene.state->set_query("a");
    CHECK_FALSE(scene.state->trash_view());
    click_button(*scene.view, kTrash);
    REQUIRE(scene.state->trash_view());
    scene.state->set_folder({});
    CHECK_FALSE(scene.state->trash_view());
}

TEST_CASE("the trash view is labelled in Turkish") {
    gui_test::LayoutScene scene(1000, 650, 1.0F);
    scene.show_trash_view();
    CHECK(shows(*scene.view, "Çöp kutusu"));
    CHECK(shows(*scene.view, "Geri getir"));
    CHECK(shows(*scene.view, "Kalıcı sil"));
    CHECK(shows(*scene.view, "Boşalt"));
    CHECK(shows(*scene.view, "Çöp (1)"));
}

TEST_CASE("a failed undo keeps the offer; the next undo brings the note back") {
    TrashScene scene;
    scene.select("b.md");
    click_button(*scene.view, kDelete);
    settle(*scene.view);
    REQUIRE(scene.state->can_undo_trash());
    {
        // Yazma kilidi başka bağlantıda: gerçek yazma hatası.
        auto holder = noteapp::graph::Graph::open(scene.fixture.store_dir() / "graph.db");
        REQUIRE(holder.has_value());
        auto lock = holder->write();
        REQUIRE(lock.has_value());
        scene.state->undo_trash();
        CHECK(scene.fixture.note_exists(".trash/b.md"));
        CHECK(scene.state->status().find("b.md") == std::string::npos);
        CHECK(scene.state->can_undo_trash());
    }
    settle(*scene.view);
    REQUIRE(scene.has_item(kUndo));
    click_button(*scene.view, kUndo);
    CHECK(scene.fixture.note_exists("b.md"));
    CHECK_FALSE(scene.fixture.note_exists(".trash/b.md"));
    CHECK_FALSE(scene.state->can_undo_trash());
}
