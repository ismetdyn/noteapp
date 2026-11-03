// G4 (matris 84): açılışta geçiş dışı dosya ipucu, TUI gibi yalnız sayı; tarama UI iş parçacığı dışında
// (app::count_stray_files), sonuç tick'le durum çubuğuna gelir.
#include "core/i18n.hpp"
#include "core/migration.hpp"
#include "core/paths.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <thread>

using gui_test::WindowFixture;

TEST_CASE("startup: files dropped into notes after the migration are counted off the UI thread and noted") {
    WindowFixture fixture;
    REQUIRE(
        noteapp::core::migrate_legacy_notes(fixture.config.notes_dir, noteapp::core::data_dir()).has_value());
    std::ofstream(fixture.config.notes_dir / "yeni.md", std::ios::binary) << "x\n";
    std::ofstream(fixture.config.notes_dir / "ikinci.txt", std::ios::binary) << "y\n";
    const auto state = fixture.make_state();
    const std::string notice = noteapp::core::tr("gui.stray_notice", {{"count", "2"}});
    constexpr int kMaxWaitSteps = 3000;
    for (int step = 0; step < kMaxWaitSteps && state->status() != notice; ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state->tick(std::chrono::steady_clock::now());
    }
    CHECK(state->status() == notice);
}

TEST_CASE("startup: a clean notes folder brings no notice") {
    WindowFixture fixture;
    REQUIRE(
        noteapp::core::migrate_legacy_notes(fixture.config.notes_dir, noteapp::core::data_dir()).has_value());
    const auto state = fixture.make_state();
    for (int step = 0; step < 20; ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state->tick(std::chrono::steady_clock::now());
    }
    CHECK(state->status().empty());
}
