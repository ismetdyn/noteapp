#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "gui/view.hpp"
#include "layout_scenes.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;

namespace {

constexpr const char* kNewNote = "toolbar_new_note";
constexpr const char* kNewFolder = "toolbar_new_folder";
constexpr const char* kDelete = "toolbar_delete";
constexpr const char* kTrash = "toolbar_trash";
constexpr const char* kMove = "toolbar_move";
constexpr const char* kExport = "toolbar_export";
const std::string kBinary("bin\0ary", 7);

bool is_disabled(const View& view, const std::string& name) {
    return view.item_rects().at(name).disabled;
}

struct ToolbarScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    void open() {
        state = fixture.make_state();
        view = make_view(*state);
        settle(*view);
    }
};

}  // namespace

TEST_CASE("toolbar New note button starts a draft") {
    ToolbarScene scene;
    scene.open();
    CHECK_FALSE(scene.state->is_new_note());
    click_button(*scene.view, kNewNote);
    CHECK(scene.state->is_new_note());
    settle(*scene.view);  // odak isteği birkaç karede oturur
    CHECK(scene.view->editor_active());
}

TEST_CASE("toolbar New folder button asks for a name and creates the folder under the selected one") {
    ToolbarScene scene;
    scene.fixture.add_note("work/a.md", "a\n");
    scene.open();
    scene.state->set_folder("work");
    settle(*scene.view);
    scene.state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("sub");
    };
    click_button(*scene.view, kNewFolder);
    const auto& folders = scene.state->folders();
    CHECK(std::ranges::find(folders, "work/sub") != folders.end());
}

TEST_CASE("toolbar Delete button moves the note to the trash without asking") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.open();
    int asked = 0;
    scene.state->deciders.confirm_delete = [&](const std::string&) {
        ++asked;
        return true;
    };
    click_button(*scene.view, kDelete);
    CHECK(asked == 0);
    CHECK_FALSE(scene.fixture.note_exists("a.md"));
    CHECK(scene.fixture.note_exists(".trash/a.md"));
}

TEST_CASE("Shift+Del deletes permanently only after confirmation") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.open();
    std::string asked;
    bool answer = false;
    scene.state->deciders.confirm_delete = [&](const std::string& message) {
        asked = message;
        return answer;
    };
    scene.state->request_focus(noteapp::gui::FocusRequest::List);
    settle(*scene.view);
    gui_test::press(*scene.view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK(asked.find("a.md") != std::string::npos);
    CHECK(scene.fixture.note_exists("a.md"));
    answer = true;
    gui_test::press(*scene.view, ImGuiMod_Shift | ImGuiKey_Delete);
    CHECK_FALSE(scene.fixture.note_exists("a.md"));
    CHECK_FALSE(scene.fixture.note_exists(".trash/a.md"));
}

TEST_CASE("Shift+Del uses the real dialog whose default answer is Cancel") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.open();
    scene.view->install_deciders();
    scene.state->request_focus(noteapp::gui::FocusRequest::List);
    settle(*scene.view);
    int frame = 0;
    scene.view->on_modal_frame = [&] {
        ++frame;
        constexpr int kAnswerFrame = 5;  // varsayılan düğmeye gezinti imleci bu karede oturmuştur
        constexpr int kGiveUpFrame = 200;
        if (frame == kAnswerFrame)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        if (frame == kAnswerFrame + 1)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        if (frame % kGiveUpFrame == 0)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    };
    gui_test::press(*scene.view, ImGuiMod_Shift | ImGuiKey_Delete);
    scene.view->on_modal_frame = nullptr;
    CHECK(frame > 0);  // diyalog gerçekten açıldı
    CHECK(frame < 200);
    CHECK(scene.fixture.note_exists("a.md"));
}

TEST_CASE("toolbar Move button opens the folder-only dialog") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.fixture.add_note("work/keep.md", "k\n", 5);
    scene.open();
    scene.state->select_row(scene.state->model().index_of(*noteapp::core::NotePath::parse("a.md")));
    settle(*scene.view);
    std::optional<SaveDialogRequest> asked;
    scene.state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        asked = request;
        return std::optional<SaveDetails>(SaveDetails{"a", "work", "md"});
    };
    click_button(*scene.view, kMove);
    REQUIRE(asked.has_value());
    CHECK(asked->folder_only);
    CHECK(scene.fixture.note_exists("work/a.md"));
}

TEST_CASE("toolbar Export button asks for the PDF path") {
    ToolbarScene scene;
    scene.fixture.add_note("rapor.md", "# r\n");
    scene.open();
    std::string suggested;
    scene.state->deciders.export_path = [&](const std::string& name) {
        suggested = name;
        return std::nullopt;
    };
    click_button(*scene.view, kExport);
    CHECK(suggested == "rapor.pdf");
}

TEST_CASE("toolbar disables selection buttons when no note is selected") {
    ToolbarScene scene;
    scene.open();
    CHECK_FALSE(is_disabled(*scene.view, kNewNote));
    CHECK_FALSE(is_disabled(*scene.view, kNewFolder));
    CHECK(is_disabled(*scene.view, kDelete));
    CHECK(is_disabled(*scene.view, kMove));
    CHECK(is_disabled(*scene.view, kExport));
}

TEST_CASE("toolbar enables selection buttons for a normal note") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.open();
    CHECK_FALSE(is_disabled(*scene.view, kDelete));
    CHECK_FALSE(is_disabled(*scene.view, kMove));
    CHECK_FALSE(is_disabled(*scene.view, kExport));
}

TEST_CASE("toolbar Export is disabled on a binary note, which it cannot export") {
    ToolbarScene scene;
    scene.fixture.add_note("b.md", kBinary);
    scene.open();
    CHECK(is_disabled(*scene.view, kExport));
    CHECK_FALSE(is_disabled(*scene.view, kDelete));  // silme ve taşıma ikili dosyada da çalışır
    CHECK_FALSE(is_disabled(*scene.view, kMove));
}

TEST_CASE("toolbar on a draft: Delete and Move off, Export only once there is text") {
    ToolbarScene scene;
    scene.fixture.add_note("a.md", "a\n");
    scene.open();
    click_button(*scene.view, kNewNote);
    settle(*scene.view);
    CHECK(is_disabled(*scene.view, kDelete));
    CHECK(is_disabled(*scene.view, kMove));
    // Boş taslakta Ctrl+E taslağı sessizce atar: düğme bunu tıklamayla tetiklememeli.
    CHECK(is_disabled(*scene.view, kExport));
    click_button(*scene.view, kExport);
    CHECK(scene.state->is_new_note());
    scene.state->edit_text("metin");
    settle(*scene.view);
    CHECK_FALSE(is_disabled(*scene.view, kExport));
}

TEST_CASE("a disabled toolbar button ignores the click") {
    ToolbarScene scene;
    scene.fixture.add_note("b.md", kBinary);
    scene.open();
    bool asked = false;
    scene.state->deciders.export_path = [&](const std::string&) {
        asked = true;
        return std::nullopt;
    };
    click_button(*scene.view, kExport);
    CHECK_FALSE(asked);
}

TEST_CASE("every toolbar button has a tooltip naming its shortcut, even when disabled") {
    ToolbarScene scene;
    scene.open();
    const std::pair<const char*, const char*> expected[] = {
        {kNewNote, "Ctrl+N"}, {kNewFolder, "New folder"}, {kDelete, "Shift+Del"},
        {kTrash, "Trash"},    {kMove, "Ctrl+M"},          {kExport, "Ctrl+E"}};
    for (const auto& [name, text] : expected) {
        CAPTURE(name);
        const auto [x, y] = scene.view->button_center(name);
        ImGui::GetIO().AddMousePosEvent(x, y);
        const gui_test::RenderedText rendered;
        // İpucu ImGui'de bekleme süresinden sonra açılır (HoverFlagsForTooltipMouse).
        for (int frame = 0; frame < 60 && !rendered.contains(text); ++frame) {
            SDL_Delay(10);
            scene.view->draw_frame();
        }
        CHECK(rendered.contains(text));
    }
}

TEST_CASE("the empty list tells how to create a note") {
    ToolbarScene scene;
    scene.open();
    {
        const gui_test::RenderedText rendered;
        scene.view->draw_frame();
        CHECK(rendered.contains("Ctrl+N"));
    }
    scene.fixture.add_note("a.md", "a\n");
    scene.state->rescan();
    settle(*scene.view);
    const gui_test::RenderedText rendered;
    scene.view->draw_frame();
    CHECK_FALSE(rendered.contains("Ctrl+N"));
}

TEST_CASE("toolbar wraps inside the narrowest sidebar at every scale") {
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        gui_test::LayoutScene scene(1000, 650, scale);
        scene.state->layout().left_pane_width = 1.0F;
        // Ölçek değişimi bölmeyi kayıtlı boyuta yeniden açar; kısıt onu en dar haline indirir.
        scene.view->set_ui_scale(scale + 0.5F);
        scene.view->set_ui_scale(scale);
        settle(*scene.view);
        const auto& rects = scene.view->item_rects();
        const auto& left = rects.at("left_pane");
        float lowest_top = 0.0F;
        for (const char* name : {kNewNote, kNewFolder, kDelete, kTrash, kMove, kExport}) {
            CAPTURE(name);
            const auto& rect = rects.at(name);
            CHECK(rect.min_x >= left.min_x);
            CHECK(rect.max_x <= left.max_x);
            lowest_top = std::max(lowest_top, rect.min_y);
        }
        CHECK(lowest_top > rects.at(kNewNote).min_y);  // gerçekten alt satıra indi
        CHECK(rects.at(kExport).max_y <= rects.at("note_list").min_y);
    }
}
