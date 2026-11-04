// `import`: dosya/klasör diyaloğu ve sürükle-bırak aynı yoldan (GuiState::import_files) app::import_paths'e
// gider; hedef app::new_note_folder, özet durum çubuğunda, ilk not açılır. Komut ve menü diyaloğu ister.
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "support/test_support.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using gui_test::make_view;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;
using ImportRequest = GuiState::ImportRequest;

namespace {

struct ImportScene {
    WindowFixture fixture;
    noteapp::test::TempDir files;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    ImportScene() {
        noteapp::core::init_i18n("en");
        fixture.add_note("work/x.md", "x\n", 30);
        state = fixture.make_state();
        view = make_view(*state);
        settle(*view);
    }
    std::filesystem::path write(const std::string& name, const std::string& bytes) const {
        const std::filesystem::path file = files.path() / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
        return file;
    }
};

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

}  // namespace

TEST_CASE("imported files land in the target folder, the summary shows and the first note opens") {
    ImportScene scene;
    scene.state->open_note(*noteapp::core::NotePath::parse("work/x.md"));  // hedef: gösterilen notun klasörü
    settle(*scene.view);
    scene.write("dir/b.md", "beta\n");
    const std::vector<std::filesystem::path> sources{scene.write("a.txt", "alfa\r\n"),
                                                     scene.files.path() / "dir"};
    scene.state->import_files(sources);
    settle(*scene.view);
    CHECK(scene.fixture.read_note("work/a.txt") == "alfa\r\n");
    CHECK(scene.fixture.read_note("work/dir/b.md") == "beta\n");
    CHECK(scene.state->status().find("Imported: 2") != std::string::npos);
    CHECK(scene.state->current_path() == noteapp::core::NotePath::parse("work/a.txt"));
}

TEST_CASE("a failed import reports the reason in the status bar and opens nothing") {
    ImportScene scene;
    const std::vector<std::filesystem::path> sources{scene.files.path() / "yok.md"};
    scene.state->import_files(sources);
    CHECK(scene.state->status().find("yok.md") != std::string::npos);
    CHECK_FALSE(scene.fixture.note_exists("yok.md"));
}

TEST_CASE("the import command asks the view for the file dialog") {
    ImportScene scene;
    CHECK(scene.state->take_import_request() == ImportRequest::None);
    run_command(*scene.state, "import");
    CHECK(scene.state->take_import_request() == ImportRequest::Files);
    CHECK(scene.state->take_import_request() == ImportRequest::None);
}
