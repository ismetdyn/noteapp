// G4 (P-G4): `external` (TUI `E`) notu config'deki dış editörde açar: bekleyen düzenleme önce yazılır, editör
// işçide beklenir (pencere donmaz), not çakışma denetimiyle geri yazılıp yeniden okunur. `copy` (TUI `c`)
// notun özgün baytlarını ya da klasör yolunu panoya koyar; ikili ve geçersiz UTF-8 reddedilir.
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::test::ScopedEnv;

namespace {

namespace core = noteapp::core;

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

// İşçinin sonucu UI iş parçacığına post edilir; tick onu çalıştırır.
void wait_external(GuiState& state) {
    constexpr int kMaxWaitSteps = 3000;
    for (int step = 0; step < kMaxWaitSteps && state.external_running(); ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state.tick(std::chrono::steady_clock::now());
    }
    REQUIRE_FALSE(state.external_running());
}

struct ToolScene {
    WindowFixture fixture;
    std::vector<std::string> copied;

    ToolScene() {
        fixture.config.editor = std::string("\"") + NOTEAPP_FAKE_EDITOR + "\"";
        fixture.add_note("a.md", "alfa\n", 10);
        fixture.add_note("work/x.md", "x\n", 20);
    }
    std::unique_ptr<GuiState> make_state() {
        auto state = fixture.make_state();
        state->deciders.copy_text = [this](const std::string& text) {
            copied.push_back(text);
            return true;
        };
        return state;
    }
    static void open(GuiState& state, const std::string& path) {
        state.open_note(*core::NotePath::parse(path));
    }
};

}  // namespace

TEST_CASE("external: the pending edit is saved first, the editor's bytes come back and the note reloads") {
    ToolScene scene;
    auto state = scene.make_state();
    ToolScene::open(*state, "a.md");
    state->edit_text("bekleyen\n");
    REQUIRE(state->dirty());
    {
        ScopedEnv exit_code("FAKE_EDITOR_EXIT", "0");  // editör dosyaya dokunmaz
        run_command(*state, "external");
        wait_external(*state);
    }
    CHECK(scene.fixture.read_note("a.md") == "bekleyen\n");  // önce yazıldı, editör onu gördü
    CHECK_FALSE(state->dirty());

    ScopedEnv content("FAKE_EDITOR_CONTENT", "dış editörden\n");
    run_command(*state, "external");
    CHECK(state->external_running());  // işçide: pencere beklemez
    wait_external(*state);
    CHECK(scene.fixture.read_note("a.md") == "dış editörden\n");
    CHECK(state->buffer() == "dış editörden\n");  // gösterilen not yeniden okundu
    CHECK_FALSE(state->dirty());
}

// TUI ile aynı (NoteStore::edit_in_temp_file): editörün yazdığı bayt kaybolmaz, çıkış hatası yine söylenir.
TEST_CASE("external: a failing editor reports why; what it wrote is still kept") {
    ToolScene scene;
    auto state = scene.make_state();
    ToolScene::open(*state, "a.md");
    ScopedEnv content("FAKE_EDITOR_CONTENT", "yarım\n");
    ScopedEnv exit_code("FAKE_EDITOR_EXIT", "3");
    run_command(*state, "external");
    wait_external(*state);
    CHECK(state->status().find("3") != std::string::npos);
    CHECK(scene.fixture.read_note("a.md") == "yarım\n");
    CHECK(state->buffer() == "yarım\n");
}

TEST_CASE("external: Shift+E in the list opens the selected note; closing waits for the editor") {
    ToolScene scene;
    auto state = scene.make_state();
    auto view = make_view(*state);
    ToolScene::open(*state, "a.md");
    state->request_focus(FocusRequest::List);
    settle(*view);
    ScopedEnv content("FAKE_EDITOR_CONTENT", "kısayoldan\n");
    ScopedEnv sleep("FAKE_EDITOR_SLEEP_MS", "300");
    press(*view, ImGuiMod_Shift | ImGuiKey_E);
    REQUIRE(state->external_running());
    // Editör açıkken kapanış ertelenir: yazma geri dönüşü kaybolmasın.
    CHECK_FALSE(state->request_close());
    CHECK(state->status() == core::tr("gui.external_running"));
    wait_external(*state);
    CHECK(scene.fixture.read_note("a.md") == "kısayoldan\n");
}

TEST_CASE(
    "copy: Ctrl+C copies the note's exact bytes, a folder row its path; binary and bad UTF-8 are refused") {
    ToolScene scene;
    scene.fixture.add_note("crlf.txt", "satır\r\nikinci\r\n", 30);
    scene.fixture.add_note("ikili.bin", std::string("\x89PNG\r\n\x1a\n\0\0", 10), 40);
    scene.fixture.add_note("bozuk.txt", "a\xff\xfe b\n", 50);
    auto state = scene.make_state();
    auto view = make_view(*state);
    ToolScene::open(*state, "crlf.txt");
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_C);
    REQUIRE(scene.copied.size() == 1);
    CHECK(scene.copied.back() == "satır\r\nikinci\r\n");  // editörün LF gösterimi değil, özgün bayt

    state->select_folder("work");
    run_command(*state, "copy");
    REQUIRE(scene.copied.size() == 2);
    CHECK(scene.copied.back() == "work");

    ToolScene::open(*state, "ikili.bin");
    run_command(*state, "copy");
    CHECK(scene.copied.size() == 2);
    CHECK(state->status() == core::tr("gui.copy_refused_binary"));
    ToolScene::open(*state, "bozuk.txt");
    run_command(*state, "copy");
    CHECK(scene.copied.size() == 2);
    CHECK(state->status() == core::tr("gui.copy_refused_utf8"));
}

// Çöp görünümünde ve klasör satırında arkadaki canlı not hedef değildir (TUI'de de çöpte `E` yok).
TEST_CASE("external and copy leave the hidden live note alone in the trash view and on a folder row") {
    ToolScene scene;
    scene.fixture.add_note(".trash/eski.md", "eski\n", 40);
    auto state = scene.make_state();
    ToolScene::open(*state, "a.md");
    state->open_trash();
    ScopedEnv content("FAKE_EDITOR_CONTENT", "olmamalı\n");
    run_command(*state, "external");
    CHECK_FALSE(state->external_running());
    run_command(*state, "copy");
    CHECK(scene.copied.empty());
    state->close_trash();

    state->select_folder("work");
    run_command(*state, "external");
    CHECK_FALSE(state->external_running());
    wait_external(*state);
    CHECK(scene.fixture.read_note("a.md") == "alfa\n");
    CHECK(scene.fixture.read_note("work/x.md") == "x\n");
}
