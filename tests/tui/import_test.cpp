// Palet `import`: altta dosya sistemi yolu girdisi, app::import_paths (CLI ve GUI ile aynı kural) işçide
// (app::ImportJob), ilerleme ve özet durum satırında, ilk not REVIEW'da; Esc/Ctrl+C sürerken durdurur.
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kMaxWaitSteps = 3000;
constexpr auto kWaitStep = std::chrono::milliseconds(1);

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
    // main'in app_.Post'u yerine: işçinin işleri burada birikir, testin iş parçacığında koşar.
    std::mutex posted_mutex;
    std::deque<std::function<void()>> posted;
    tui::ListActions actions{host()};
    std::optional<tui::RenameState> prompt;

    tui::ListHost host() {
        tui::ListHost made{
            store, mutex, clipboard, system, state, cache, status, [this] { load(); }, [this] { ++changed; }};
        made.post = [this](std::function<void()> work) {
            const std::scoped_lock lock(posted_mutex);
            posted.push_back(std::move(work));
        };
        return made;
    }
    // Bekleyen ilk işi koşar; iş gelene dek bekler.
    void run_next() {
        for (int step = 0; step < kMaxWaitSteps; ++step) {
            std::function<void()> work;
            {
                const std::scoped_lock lock(posted_mutex);
                if (!posted.empty()) {
                    work = std::move(posted.front());
                    posted.pop_front();
                }
            }
            if (work) {
                work();
                return;
            }
            std::this_thread::sleep_for(kWaitStep);
        }
        FAIL("nothing was posted");
    }
    // İçe alma bitene dek işleri koşar (rapor UI iş parçacığına gelince biter).
    void finish_import() {
        for (int step = 0; step < kMaxWaitSteps && actions.importing(); ++step)
            run_next();
        REQUIRE_FALSE(actions.importing());
    }

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
    CHECK_FALSE(f.prompt);  // girdi kapanır, içe alma işçide sürer
    CHECK(f.actions.importing());
    f.finish_import();
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
    f.finish_import();
    REQUIRE(f.prompt);  // yol düzeltilebilsin: girdi yazılanla geri açılır
    CHECK(f.prompt->is_import());
    CHECK(f.prompt->text() == missing);
    CHECK(f.prompt->error().find("yok.md") != std::string::npos);
    CHECK(f.changed == 0);
    f.actions.handle_rename_key(f.prompt, Key::Escape, {});
    CHECK_FALSE(f.prompt);
}

TEST_CASE(
    "a running import shows its progress in the status line; Esc and Ctrl+C outside the editor cancel it") {
    Fixture f;
    CHECK_FALSE(
        f.actions.cancel_import_on(Key::Escape, Focus::List));  // içe alma yokken tuş kendi işini yapar
    const std::string file = f.write("plan.txt", "kuzey\n");
    f.actions.begin_import(f.prompt, "");
    f.type(file);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    REQUIRE(f.actions.importing());
    f.run_next();  // durdurulmamış işçinin ilk işi ilerlemedir (rapordan önce gelir)
    CHECK(f.status.message.starts_with("Importing 1/1: " + file));
    CHECK(f.status.message.ends_with("Esc cancels"));
    // Rapor UI iş parçacığına gelene dek iş sürüyor sayılır: tuşlar belirlenimli sınanır.
    REQUIRE(f.actions.importing());
    CHECK_FALSE(f.actions.cancel_import_on(Key::Escape, Focus::Editor));
    // Arama girdisinde Esc kendi işini yapar.
    CHECK_FALSE(f.actions.cancel_import_on(Key::Escape, Focus::Search));
    CHECK_FALSE(f.actions.cancel_import_on(Key::CtrlC, Focus::Editor));
    CHECK_FALSE(f.actions.cancel_import_on(Key::Char, Focus::List));
    CHECK(f.actions.cancel_import_on(Key::CtrlC, Focus::List));
    CHECK(f.status.message == "Cancelling the import…");
    CHECK(f.actions.cancel_import_on(Key::Escape, Focus::Preview));
    f.finish_import();
    CHECK_FALSE(f.status.message.starts_with("Importing"));  // özet ilerlemenin yerine geçer
}

TEST_CASE("a second import while one runs is refused in the prompt") {
    Fixture f;
    const std::string file = f.write("plan.txt", "kuzey\n");
    f.actions.begin_import(f.prompt, "");
    f.type(file);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    REQUIRE(f.actions.importing());
    f.actions.begin_import(f.prompt, "");
    f.type(file);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    REQUIRE(f.prompt);
    CHECK(f.prompt->error() == "An import is already running; wait for it or cancel it");
    f.finish_import();
    CHECK(f.prompt);  // açık girdi rapordan etkilenmez
}

// Klasör kuruldu ama not alınmadı (boş klasör): girdi geri açılmaz; yeniden denemek ikinci bir `-2` klasörü
// kurardı.
TEST_CASE("an import that wrote only a folder closes the prompt and reports in the status line") {
    Fixture f;
    std::filesystem::create_directory(f.files.path() / "bos");
    const std::string folder = test::utf8(f.files.path() / "bos");
    f.actions.begin_import(f.prompt, "");
    f.type(folder);
    f.actions.handle_rename_key(f.prompt, Key::Enter, {});
    f.finish_import();
    CHECK_FALSE(f.prompt);
    CHECK(f.changed == 1);
    CHECK(f.status.message == "Imported: 0, skipped: 0, not imported: 0");
}

// Oturum içe alma sürerken kapanır: yıkıcı işçiyi durdurup bekler, sonra kuyruğa iş gelmez (yıkılmış
// nesneye dokunan iş kalmaz). Kuyrukta kalanlar koşturulmaz: main'de de döngü bitmiştir.
TEST_CASE("destroying the list actions while an import runs joins the worker; nothing is posted after") {
    Fixture f;
    std::filesystem::create_directory(f.files.path() / "many");
    for (int i = 0; i < 300; ++i)
        f.write("many/n" + std::to_string(i) + ".md", "x");
    std::optional<tui::ListActions> actions{std::in_place, f.host()};
    std::optional<tui::RenameState> prompt;
    actions->begin_import(prompt, "");
    const std::string folder = test::utf8(f.files.path() / "many");
    for (std::size_t i = 0; i < folder.size(); ++i)
        actions->handle_rename_key(prompt, Key::Char, std::string_view(folder).substr(i, 1));
    actions->handle_rename_key(prompt, Key::Enter, {});
    REQUIRE(actions->importing());
    actions.reset();
    const std::size_t queued = [&] {
        const std::scoped_lock lock(f.posted_mutex);
        return f.posted.size();
    }();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::scoped_lock lock(f.posted_mutex);
    CHECK(f.posted.size() == queued);
    f.posted.clear();  // yıkılmış ListActions'a dokunurlardı
}
