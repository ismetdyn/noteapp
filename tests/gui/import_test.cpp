// `import`: dosya/klasör diyaloğu ve sürükle-bırak aynı yoldan (GuiState::import_files) app::import_paths'e
// gider, işçide (app::ImportJob); hedef app::new_note_folder, ilerleme ve özet durum çubuğunda, ilk not
// açılır. Komut ve menü diyaloğu ister.
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "support/test_support.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using gui_test::click_button;
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

// İşçinin ilerlemesi ve raporu UI iş parçacığına post edilir; tick onları çalıştırır.
void wait_import(GuiState& state) {
    constexpr int kMaxWaitSteps = 3000;
    for (int step = 0; step < kMaxWaitSteps && state.importing(); ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        state.tick(std::chrono::steady_clock::now());
    }
    REQUIRE_FALSE(state.importing());
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
    CHECK(scene.state->importing());  // işçide: pencere beklemez
    wait_import(*scene.state);
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
    wait_import(*scene.state);
    CHECK(scene.state->status().find("yok.md") != std::string::npos);
    CHECK_FALSE(scene.fixture.note_exists("yok.md"));
}

// Rapor UI iş parçacığına (tick) gelene dek iş sürüyor sayılır: düğme ve ret belirlenimli sınanır.
TEST_CASE("a running import shows Cancel in the status bar; it stops the import and the summary follows") {
    ImportScene scene;
    const std::vector<std::filesystem::path> sources{scene.write("a.txt", "alfa\n")};
    scene.state->import_files(sources);
    REQUIRE(scene.state->importing());
    settle(*scene.view);
    REQUIRE(scene.view->item_rects().contains("status_cancel_import"));
    click_button(*scene.view, "status_cancel_import");
    CHECK(scene.state->status() == "Cancelling the import…");
    wait_import(*scene.state);
    CHECK(scene.state->status().starts_with("Imported: "));  // durdurmaya yetişmese de özet gelir
    settle(*scene.view);
    CHECK_FALSE(scene.view->item_rects().contains("status_cancel_import"));
}

TEST_CASE("a second import while one runs is refused in the status bar") {
    ImportScene scene;
    const std::vector<std::filesystem::path> sources{scene.write("a.txt", "alfa\n")};
    scene.state->import_files(sources);
    REQUIRE(scene.state->importing());
    scene.state->import_files(sources);
    CHECK(scene.state->status() == "An import is already running; wait for it or cancel it");
    wait_import(*scene.state);
    CHECK(scene.state->status() == "Imported: 1, skipped: 0, not imported: 0");  // yalnız ilki koştu
}

TEST_CASE("the import command asks the view for the file dialog") {
    ImportScene scene;
    CHECK(scene.state->take_import_request() == ImportRequest::None);
    run_command(*scene.state, "import");
    CHECK(scene.state->take_import_request() == ImportRequest::Files);
    CHECK(scene.state->take_import_request() == ImportRequest::None);
}

// Durum çubuğu tek satırdır ve yol dosya sisteminden gelir: denetim karakterleri (satır sonu, ESC)
// gösterilmez.
TEST_CASE("an import path with control characters shows on one clean status line") {
    ImportScene scene;
    const std::vector<std::filesystem::path> sources{scene.files.path() / "yok\nsatir\x1b[31m.md"};
    scene.state->import_files(sources);
    wait_import(*scene.state);
    const std::string& status = scene.state->status();
    CHECK(status.find("yok") != std::string::npos);
    CHECK(status.find('\n') == std::string::npos);
    CHECK(status.find('\x1b') == std::string::npos);
}

// Pencere içe alma sürerken kapanır: GuiState yıkılırken işçi durur ve beklenir; post'u posted_ hâlâ
// yaşarken çağırır (ASan/TSan bunu sınar).
TEST_CASE("closing the window while an import runs joins the worker cleanly") {
    ImportScene scene;
    for (int i = 0; i < 300; ++i)
        scene.write("many/n" + std::to_string(i) + ".md", "x");
    const std::vector<std::filesystem::path> sources{scene.files.path() / "many"};
    scene.view.reset();
    scene.state->import_files(sources);
    REQUIRE(scene.state->importing());
    scene.state.reset();
    CHECK_FALSE(scene.state);
}
