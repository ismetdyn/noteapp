// G4 (matris 84): açılıştaki geçiş dışı dosya ipucunun sayısı TUI ve GUI'de aynı kuraldan
// (app::count_stray_files).
#include "app/stray_notice.hpp"
#include "core/config.hpp"
#include "core/migration.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <stop_token>

namespace {

struct StrayScene {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv home_env{"NOTEAPP_HOME"};
    noteapp::core::Config config;

    StrayScene() {
        home_env.set(noteapp::test::utf8(home.path()));
        config = noteapp::core::load_config().config;
        config.notes_dir = home.path() / "notes";
        std::filesystem::create_directories(config.notes_dir);
    }
    void drop(const char* name) const { std::ofstream(config.notes_dir / name, std::ios::binary) << "x\n"; }
};

}  // namespace

TEST_CASE("stray count: only files dropped into notes_dir after the migration count") {
    StrayScene scene;
    scene.drop("eski.md");
    // Geçiş bitmemiş: o durumu `note graph status` söyler.
    CHECK(noteapp::app::count_stray_files(scene.config, {}) == 0);
    REQUIRE(
        noteapp::core::migrate_legacy_notes(scene.config.notes_dir, noteapp::core::data_dir()).has_value());
    CHECK(noteapp::app::count_stray_files(scene.config, {}) == 0);  // geçen dosya kayıtlı
    scene.drop("yeni.md");
    scene.drop("ikinci.txt");
    CHECK(noteapp::app::count_stray_files(scene.config, {}) == 2);

    std::stop_source stop;
    stop.request_stop();  // kapanış: sayı yok
    CHECK(noteapp::app::count_stray_files(scene.config, stop.get_token()) == 0);
}
