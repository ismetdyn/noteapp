// Palet `import`: altta dosya sistemi yolu girdisi, app::import_paths (CLI ve GUI ile aynı kural), özet durum
// satırında, ilk not REVIEW'da.
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

struct Fixture {
    test::TempDir dir;
    test::TempDir state_home;
    test::TempDir files;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache cache;
    tui::StatusLine status;
    int changed = 0;
    tui::ListActions actions{tui::ListHost{store, mutex, clipboard, system, state, cache, status,
                                           [this] { load(); }, [this] { ++changed; }}};
    std::optional<tui::RenameState> prompt;

    Fixture() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "a.md", "a\n"));
        load();
        state.set_window(100, 30);
    }
    void load() {
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    std::string write(const std::string& name, const std::string& bytes) const {
        std::ofstream(files.path() / name, std::ios::binary) << bytes;
        return test::utf8(files.path() / name);
    }
    void type(std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i)
            actions.handle_rename_key(prompt, Key::Char, text.substr(i, 1));
    }
};

}  // namespace

TEST_CASE("the import palette command opens the path prompt from the list") {
    const auto commands = tui::TuiState::commands();
    const auto found = std::ranges::find(commands, std::string_view("import"), &tui::Command::id);
    REQUIRE(found != commands.end());
    tui::TuiState state;
    CHECK(found->run(state) == Intent::ImportFiles);
}

TEST_CASE("import asks for a path, imports into the target folder and shows the first note") {
    Fixture f;
    const std::string file = f.write("plan.txt", "kuzey\r\n");
    f.actions.begin_import(f.prompt, "inbox");
    REQUIRE(f.prompt);
    CHECK(f.prompt->is_import());
    CHECK(f.prompt->text().empty());
    f.type(file);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    CHECK_FALSE(f.prompt);
    CHECK(f.store->read(np("inbox/plan.txt"))->text == "kuzey\r\n");
    CHECK(f.status.message.find("Imported: 1") != std::string::npos);
    CHECK(f.changed == 1);
    REQUIRE(f.state.selected_row() != nullptr);
    CHECK(f.state.selected_row()->path == np("inbox/plan.txt"));
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("a path that imports nothing keeps the prompt open with the reason; Esc closes it") {
    Fixture f;
    f.actions.begin_import(f.prompt, "");
    const std::string missing = test::utf8(f.files.path() / "yok.md");
    f.type(missing);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    REQUIRE(f.prompt);
    CHECK(f.prompt->error().find("yok.md") != std::string::npos);
    CHECK(f.changed == 0);
    f.actions.handle_rename_key(f.prompt, Key::Escape, {});
    CHECK_FALSE(f.prompt);
}
