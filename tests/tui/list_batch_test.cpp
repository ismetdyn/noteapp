// T71: çoklu seçim, kopya ve yapıştır, `g` öneki. Model saf; eylemler gerçek depoyla.
#include "core/i18n.hpp"
#include "core/note_store_impl.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 110;
constexpr int kHeight = 24;

std::string row_of(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

ftxui::Screen draw(const tui::TuiState& state, int width = kWidth) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, {}, {"", ""}, {width, kHeight}));
    return screen;
}

bool shows(const tui::TuiState& state, std::string_view text) {
    const auto screen = draw(state);
    for (int y = 0; y < screen.dimy(); ++y)
        if (row_of(screen, y).find(text) != std::string::npos)
            return true;
    return false;
}

struct Fixture {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    int changes = 0;
    std::optional<tui::ConfirmState> confirm;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { load(); }, [this] { ++changes; }}};

    Fixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
        tui::ListPrefs prefs;
        prefs.sort = {search::SortField::Name, false};
        state.set_list_prefs(prefs);
    }
    void put(const std::string& name, std::string_view bytes = "x") const {
        test::upsert_note(dir.path(), name, bytes);
    }
    void load() {
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    bool exists(const std::string& name) const { return test::note_exists(dir.path(), name); }
    bool in_trash(const std::string& name) const { return test::note_in_trash(dir.path(), name); }
    std::string body(const std::string& name) const { return test::note_body(dir.path(), name); }
    void press(std::string_view keys) {
        for (const char c : keys)
            actions.run(state.handle(Key::Char, std::string_view(&c, 1)), confirm);
    }
    void yes() {
        REQUIRE(confirm);
        CHECK_FALSE(confirm->yes_selected());
        confirm->handle(Key::Right);
        REQUIRE(confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
        actions.finish_delete(confirm);
    }
    std::string summary() const {
        std::string out;
        for (const std::string& line : state.summary())
            out += line + "\n";
        return out;
    }
    void select(const std::string& shown) {
        for (std::size_t i = 0; i < state.rows().size(); ++i)
            if (state.rows()[i].shown_text() == shown) {
                state.select_index(i);
                return;
            }
        FAIL("no row " << shown);
    }
    void enter(const std::string& shown) {
        select(shown);
        state.handle(Key::Enter);
    }
};

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("x marks the row, moves down, shows a marker and a count; X clears") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("c.md");
    f.load();
    f.press("x");
    CHECK(f.state.selected() == 1);
    CHECK(f.state.marked_count() == 1);
    CHECK(f.state.is_marked(f.state.rows()[0]));
    const auto screen = draw(f.state);
    CHECK(row_of(screen, 2).find("│ *") == 0);
    CHECK(screen.PixelAt(tui::kPaneContentX, 2).foreground_color == ftxui::Color(ftxui::Color::Yellow));
    CHECK(has(row_of(screen, kHeight - 1), "selected: 1"));
    f.press("x");
    f.press("k");
    f.press("x");  // geri basınca kalkar
    CHECK(f.state.marked_count() == 1);
    CHECK_FALSE(f.state.is_marked(f.state.rows()[1]));
    f.press("X");
    CHECK(f.state.marked_count() == 0);
    CHECK_FALSE(has(row_of(draw(f.state), kHeight - 1), "selected:"));
}

TEST_CASE("Ctrl+A marks every visible row, folders too") {
    Fixture f;
    f.put("work/a.md");
    f.put("b.md");
    f.load();
    f.state.handle(Key::CtrlA);
    CHECK(f.state.marked_count() == 2);
    CHECK(f.state.is_marked(f.state.rows()[0]));
    CHECK(f.state.rows()[0].is_folder);
}

TEST_CASE("the selection survives moving between folders and the count stays") {
    Fixture f;
    f.put("work/a.md");
    f.put("b.md");
    f.load();
    f.select("b.md");
    f.press("x");
    f.enter("work/");
    CHECK(f.state.marked_count() == 1);
    CHECK(shows(f.state, "selected: 1"));
    f.state.handle(Key::Left);
    f.select("b.md");
    CHECK(f.state.is_marked(f.state.rows()[f.state.selected()]));
}

TEST_CASE("a rescan drops marked items that vanished") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("gone/c.md");
    f.load();
    f.state.handle(Key::CtrlA);
    REQUIRE(f.state.marked_count() == 3);
    REQUIRE(f.store->remove(core::NotePath::parse("a.md").value()));
    REQUIRE(f.store->remove_folder("gone", true));
    f.load();
    CHECK(f.state.marked_count() == 1);
    f.press("d");
    CHECK(f.in_trash("b.md"));
}

TEST_CASE("d with a selection trashes all of it and one u brings the whole batch back") {
    Fixture f;
    f.put("a.md", "A");
    f.put("b.md", "B");
    f.put("c.md", "C");
    f.load();
    f.press("xx");
    f.press("d");
    CHECK(f.in_trash("a.md"));
    CHECK(f.in_trash("b.md"));
    CHECK(f.exists("c.md"));
    CHECK(f.state.marked_count() == 0);
    CHECK(f.changes == 1);
    f.press("u");
    CHECK(f.body("a.md") == "A");
    CHECK(f.body("b.md") == "B");
    CHECK(f.changes == 2);
    f.press("u");
    CHECK(has(f.status.message, "Nothing"));
}

TEST_CASE("d with a marked folder holding notes asks first; a note inside a marked folder counts once") {
    Fixture f;
    f.put("work/a.md");
    f.put("work/sub/b.md");
    f.put("c.md");
    f.load();
    f.press("x");  // work/
    f.enter("work/");
    f.select("a.md");
    f.press("x");
    f.state.handle(Key::Left);
    f.select("c.md");
    f.press("x");
    REQUIRE(f.state.marked_count() == 3);
    f.press("d");
    REQUIRE(f.confirm);
    CHECK(has(f.confirm->message(), "3 notes"));
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    f.confirm.reset();
    CHECK(f.exists("c.md"));
    f.press("d");
    f.yes();
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.in_trash("work/sub/b.md"));
    CHECK(f.in_trash("c.md"));
    CHECK_FALSE(f.exists("work/a.md"));
    CHECK(f.summary().empty());  // iç içe seçim ikinci kez atılıp hata vermez
    f.press("u");
    CHECK(f.exists("work/a.md"));
    CHECK(f.exists("work/sub/b.md"));
    CHECK(f.exists("c.md"));
}

TEST_CASE("d partial failure continues, reports the item, and u restores what was trashed") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("c.md");
    f.load();
    f.press("xxx");
    // Enjekte hata: not liste yenilenmeden depodan gider, tek öğe başarısız olur.
    REQUIRE(f.store->remove(core::NotePath::parse("b.md").value()));
    f.press("d");
    CHECK(f.in_trash("a.md"));
    CHECK(f.in_trash("c.md"));
    CHECK(has(f.summary(), "b.md"));
    CHECK(has(f.summary(), "2 done"));
    CHECK(has(f.summary(), "1 failed"));
    f.state.handle(Key::Escape);  // özeti kapatır
    CHECK(f.state.summary().empty());
    f.press("u");
    CHECK(f.exists("a.md"));
    CHECK(f.exists("c.md"));
}

TEST_CASE("u partial failure restores the rest and reports the missing item") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.load();
    f.press("xx");
    f.press("d");
    REQUIRE(f.store->purge(core::NotePath::parse("a.md").value()));
    f.press("u");
    CHECK(f.exists("b.md"));
    CHECK_FALSE(f.exists("a.md"));
    CHECK(has(f.summary(), "a.md"));
    f.state.handle(Key::Escape);
    f.press("u");
    CHECK(has(f.status.message, "Nothing"));  // geri gelemeyecek öğe adımı kilitlemez
}

TEST_CASE("D shows the total count, defaults to Cancel and removes everything selected") {
    Fixture f;
    f.put("a.md");
    f.put("work/b.md");
    f.put("work/c.md");
    f.load();
    f.select("a.md");
    f.press("x");
    f.select("work/");
    f.press("x");
    f.press("D");
    REQUIRE(f.confirm);
    CHECK(has(f.confirm->message(), "2 items"));
    CHECK(has(f.confirm->message(), "3 notes"));
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    f.confirm.reset();
    CHECK(f.exists("a.md"));
    f.press("D");
    f.yes();
    CHECK_FALSE(f.exists("a.md"));
    CHECK_FALSE(f.exists("work/b.md"));
    CHECK_FALSE(f.in_trash("a.md"));
    CHECK(f.state.marked_count() == 0);
}

TEST_CASE("y then p copies the bytes into the current folder and leaves the source") {
    Fixture f;
    f.put("a.md", std::string_view("A\r\n\0B", 5));
    f.put("dest/x.md");
    f.load();
    f.select("a.md");
    f.press("y");
    REQUIRE(f.state.yank());
    CHECK(shows(f.state, "copy: 1"));
    f.enter("dest/");
    f.press("p");
    CHECK(f.exists("a.md"));
    CHECK(f.body("dest/a.md") == std::string("A\r\n\0B", 5));
    CHECK_FALSE(f.state.yank());
    CHECK(f.changes == 1);
    CHECK(f.summary().empty());
}

TEST_CASE("a folder is copied with its empty subfolders") {
    Fixture f;
    f.put("work/a.md", "A");
    REQUIRE(f.store->create_folder("work/empty"));
    f.put("dest/x.md");
    f.load();
    f.select("work/");
    f.press("y");
    f.enter("dest/");
    f.press("p");
    CHECK(f.body("dest/work/a.md") == "A");
    const auto folders = f.store->folders();
    CHECK(std::ranges::find(folders, "dest/work/empty") != folders.end());
    CHECK(f.exists("work/a.md"));
}

TEST_CASE("a collision gets -2 on copy, never overwrites and reports each") {
    Fixture f;
    f.put("a.md", "new");
    f.put("v1.2.tar.gz", "gz");
    f.put("dest/a.md", "old");
    f.put("dest/v1.2.tar.gz", "old gz");
    f.put("dest/a-2.md", "older");
    f.load();
    f.select("a.md");
    f.press("y");
    f.enter("dest/");
    f.press("p");
    CHECK(f.body("dest/a.md") == "old");
    CHECK(f.body("dest/a-2.md") == "older");
    CHECK(f.body("dest/a-3.md") == "new");
    CHECK(has(f.summary(), "dest/a-3.md"));
    f.state.handle(Key::Escape);
    f.state.handle(Key::Left);
    f.select("v1.2.tar.gz");
    f.press("y");
    f.enter("dest/");
    f.press("p");
    CHECK(f.body("dest/v1.2.tar.gz") == "old gz");
    CHECK(f.body("dest/v1.2.tar-2.gz") == "gz");
    CHECK(f.exists("v1.2.tar.gz"));
    CHECK(has(f.summary(), "v1.2.tar-2.gz"));
}

// L2: eski deneme tavanı 1000 addı; depo boş adı sınırsız arar.
TEST_CASE("a note copy finds a free name past a thousand taken numbered names") {
    Fixture f;
    f.put("a.md", "new");
    std::vector<core::ImportedNote> taken;
    for (int i = 1; i <= 1000; ++i) {
        const std::string path = i == 1 ? std::string("dest/a.md") : "dest/a-" + std::to_string(i) + ".md";
        taken.push_back({core::NotePath::parse(path).value(), "old", std::chrono::system_clock::now()});
    }
    test::bulk_seed(f.dir.path(), taken);
    f.load();
    f.select("a.md");
    f.press("y");
    f.enter("dest/");
    f.press("p");
    CHECK(f.body("dest/a-1001.md") == "new");
    CHECK(f.body("dest/a.md") == "old");
}

TEST_CASE("a folder collision gets -2 on copy, twice") {
    Fixture f;
    f.put("work/a.md", "A");
    f.put("dest/work/z.md", "Z");
    f.load();
    for (const char* target : {"dest/work-2/a.md", "dest/work-3/a.md"}) {
        f.select("work/");
        f.press("y");
        f.enter("dest/");
        f.press("p");
        CHECK(f.body(target) == "A");
        CHECK(f.body("dest/work/z.md") == "Z");
        f.state.handle(Key::Escape);
        f.state.handle(Key::Left);
    }
}

TEST_CASE("a folder pasted into its own parent becomes a -2 copy beside it") {
    Fixture f;
    f.put("work/a.md", "A");
    f.load();
    f.select("work/");
    f.press("y");
    REQUIRE(f.state.current_folder().empty());
    f.press("p");
    CHECK(has(f.summary(), "1 done, 0 failed"));
    CHECK(has(f.summary(), "work -> work-2"));
    CHECK(f.body("work-2/a.md") == "A");
    CHECK(f.body("work/a.md") == "A");
}

TEST_CASE("copying a folder into itself is refused and reported; the rest continues") {
    Fixture f;
    f.put("work/sub/a.md", "A");
    f.put("b.md", "B");
    f.load();
    f.select("b.md");
    f.press("x");
    f.select("work/");
    f.press("x");
    f.press("y");
    f.enter("work/");
    f.enter("sub/");
    REQUIRE(f.state.current_folder() == "work/sub");
    f.press("p");
    CHECK(f.body("work/sub/a.md") == "A");
    CHECK(f.body("work/sub/b.md") == "B");
    CHECK(f.exists("b.md"));
    CHECK(has(f.summary(), "1 failed"));
    CHECK(has(f.summary(), "work"));
    f.state.handle(Key::Escape);
    f.state.go_to_folder("");
    f.select("work/");
    f.press("y");
    f.enter("work/");
    f.press("p");
    CHECK(has(f.summary(), "1 failed"));
    CHECK_FALSE(f.exists("work/work/sub/a.md"));
}

TEST_CASE("paste partial failure continues and loses nothing") {
    Fixture f;
    f.put("a.md", "A");
    f.put("b.md", "B");
    f.put("c.md", "C");
    f.put("dest/x.md");
    f.load();
    f.select("a.md");
    f.press("xxx");
    f.press("y");
    f.enter("dest/");
    REQUIRE(f.store->remove(core::NotePath::parse("b.md").value()));  // enjekte hata: kaynak gitti
    f.press("p");
    CHECK(f.body("dest/a.md") == "A");
    CHECK(f.body("dest/c.md") == "C");
    CHECK(f.body("a.md") == "A");
    CHECK(has(f.summary(), "b.md"));
    CHECK(has(f.summary(), "2 done"));
    CHECK(has(f.summary(), "1 failed"));
    CHECK(shows(f.state, "b.md:"));  // özet penceresi çizilir
    CHECK(f.changes == 1);
}

TEST_CASE("Esc cancels the pending yank, then p does nothing; c stays the system clipboard") {
    Fixture f;
    f.put("a.md", "A");
    f.load();
    f.press("y");
    REQUIRE(f.state.yank());
    f.state.handle(Key::Escape);
    CHECK_FALSE(f.state.yank());
    CHECK_FALSE(shows(f.state, "copy:"));
    f.press("p");
    CHECK(has(f.status.message, "Nothing"));
    CHECK(f.changes == 0);
    f.press("c");
    CHECK(f.internal == "A");
    CHECK_FALSE(f.state.yank());
}

TEST_CASE("p outside a folder view asks for a folder and keeps the yank") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.load();
    f.press("y");
    f.press("/");
    f.state.handle(Key::Char, "b");
    f.state.handle(Key::Escape);
    REQUIRE(f.state.focus() == Focus::List);
    f.press("p");
    CHECK(f.state.yank());
    CHECK(has(f.status.message, "folder"));
    CHECK(f.changes == 0);
}

TEST_CASE("search results: x marks with full paths and d acts on them") {
    Fixture f;
    f.put("work/a.md");
    f.put("other/a2.md");
    f.put("z.txt");
    f.load();
    f.press("/");
    f.state.handle(Key::Char, "a");
    f.state.handle(Key::Escape);
    REQUIRE(f.state.focus() == Focus::List);
    REQUIRE(f.state.rows().size() == 2);
    f.press("xx");
    CHECK(f.state.marked_count() == 2);
    f.press("d");
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.in_trash("other/a2.md"));
    CHECK(f.exists("z.txt"));
}

TEST_CASE("trash view: x with u restores and D purges the selected, marks stay separate") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("c.md");
    f.load();
    f.select("c.md");
    f.press("x");  // canlı seçim, çöpe karışmaz
    REQUIRE(f.state.marked_count() == 1);
    REQUIRE(f.store->trash(core::NotePath::parse("a.md").value()));
    REQUIRE(f.store->trash(core::NotePath::parse("b.md").value()));
    f.load();
    f.press("T");
    REQUIRE(f.state.trash_view());
    CHECK(f.state.marked_count() == 0);
    f.state.handle(Key::CtrlA);
    CHECK(f.state.marked_count() == 2);
    f.press("u");
    CHECK(f.exists("a.md"));
    CHECK(f.exists("b.md"));
    CHECK(f.state.rows().empty());
    REQUIRE(f.store->trash(core::NotePath::parse("a.md").value()));
    REQUIRE(f.store->trash(core::NotePath::parse("b.md").value()));
    f.actions.refresh_trash_view();
    f.press("x");
    f.press("x");
    f.press("D");
    REQUIRE(f.confirm);
    CHECK(has(f.confirm->message(), "2 items"));
    f.yes();
    CHECK_FALSE(f.in_trash("a.md"));
    CHECK_FALSE(f.in_trash("b.md"));
    f.press("T");
    CHECK(f.state.marked_count() == 1);  // canlı seçim yerinde
}

TEST_CASE("g prefix: the hint shows, gg top, gh root, gt trash, any other key cancels and does nothing") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("work/sub/c.md");
    f.load();
    f.press("G");
    const std::size_t last = f.state.selected();
    REQUIRE(last > 0);
    f.press("g");
    CHECK(f.state.selected() == last);
    CHECK(f.state.awaiting_prefix());
    CHECK(shows(f.state, "gg top"));
    f.press("g");
    CHECK(f.state.selected() == 0);
    CHECK_FALSE(f.state.awaiting_prefix());

    f.press("G");
    f.press("g");
    f.press("d");  // başka tuş: iptal, eylem değil
    CHECK(f.changes == 0);
    CHECK(f.state.selected() == last);
    CHECK_FALSE(f.state.awaiting_prefix());
    f.press("g");
    f.state.handle(Key::Up);
    CHECK(f.state.selected() == last);

    f.state.go_to_folder("work/sub");
    f.press("gh");
    CHECK(f.state.current_folder().empty());
    f.press("gt");
    CHECK(f.state.trash_view());
    f.press("gt");
    CHECK_FALSE(f.state.trash_view());
}

TEST_CASE("g prefix does not touch REVIEW: g and G still scroll there") {
    Fixture f;
    f.put("a.md");
    f.load();
    f.state.handle(Key::Right);
    REQUIRE(f.state.focus() == Focus::Preview);
    f.state.set_preview_line_count(100);
    f.press("G");
    CHECK(f.state.preview_scroll() == 99);
    f.press("g");
    CHECK(f.state.preview_scroll() == 0);
    CHECK_FALSE(f.state.awaiting_prefix());
}

TEST_CASE("the hints and F1 name the new keys") {
    Fixture f;
    f.put("a.md");
    f.load();
    const std::string hints = core::tr("tui.key_hints_list");
    for (const std::string_view key : {"x select", "y copy", "p paste", "m move"})
        CHECK(has(hints, key));
    const std::string help = core::tr("tui.help_body");
    for (const std::string_view key : {"gg", "gh", "gt", "Ctrl+A"})
        CHECK(has(help, key));
}

TEST_CASE("stale yank: a folder gone before p is reported, no empty target is created") {
    for (const bool rescanned : {true, false}) {
        CAPTURE(rescanned);
        Fixture f;
        f.put("work/a.md", "A");
        f.put("dest/x.md");
        f.load();
        f.select("work/");
        f.press("y");
        REQUIRE(f.store->remove_folder("work", true));
        if (rescanned)
            f.load();
        f.enter("dest/");
        f.press("p");
        const auto folders = f.store->folders();
        CHECK(std::ranges::find(folders, "dest/work") == folders.end());
        CHECK(has(f.summary(), "1 failed"));
        CHECK(has(f.summary(), "work"));
        CHECK(f.changes == 0);
    }
}

TEST_CASE("stale yank: a renamed folder is not found, the renamed one stays") {
    {
        const char* key = "y";
        Fixture f;
        f.put("work/a.md", "A");
        f.put("dest/x.md");
        f.load();
        f.select("work/");
        f.press(key);
        REQUIRE(f.store->rename_folder("work", "renamed"));
        f.load();
        f.enter("dest/");
        f.press("p");
        CHECK(has(f.summary(), "1 failed"));
        CHECK(f.body("renamed/a.md") == "A");
        CHECK_FALSE(f.exists("dest/work/a.md"));
        const auto folders = f.store->folders();
        CHECK(std::ranges::find(folders, "dest/work") == folders.end());
    }
}

TEST_CASE("stale yank: a note gone before p is reported, nothing is written") {
    {
        const char* key = "y";
        Fixture f;
        f.put("a.md", "A");
        f.put("dest/x.md");
        f.load();
        f.select("a.md");
        f.press(key);
        REQUIRE(
            f.store->move(core::NotePath::parse("a.md").value(), core::NotePath::parse("moved.md").value()));
        f.load();
        f.enter("dest/");
        f.press("p");
        CHECK(has(f.summary(), "a.md"));
        CHECK_FALSE(f.exists("dest/a.md"));
        CHECK(f.body("moved.md") == "A");
        CHECK(f.changes == 0);
    }
}

TEST_CASE("a folder copy follows the store when the list is stale: the gone note is not copied") {
    Fixture f;
    f.put("work/a.md", "A");
    f.put("work/b.md", "B");
    f.put("dest/x.md");
    f.load();
    f.select("work/");
    f.press("y");
    f.enter("dest/");
    REQUIRE(f.store->remove(core::NotePath::parse("work/b.md").value()));  // liste yenilenmedi: enjekte hata
    f.press("p");
    CHECK(f.body("dest/work/a.md") == "A");
    CHECK_FALSE(f.exists("dest/work/b.md"));
    CHECK(f.summary().empty());
    CHECK(f.changes == 1);
}

TEST_CASE("a partial folder trash notifies: notes left even though the folder item failed") {
    Fixture f;
    f.put("work/a.md");
    f.put("work/b.md");
    f.put("z.md");
    f.load();
    f.select("work/");
    f.press("x");  // satır seçimi: toplu yol
    REQUIRE(f.store->remove(core::NotePath::parse("work/b.md").value()));
    f.press("d");
    REQUIRE(f.confirm);
    f.yes();
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.changes == 1);
}

TEST_CASE("a partial folder copy notifies, tells the count and is not retried under a new name") {
    Fixture f;
    f.put("work/a.md", std::string(std::size_t{33} * 1024U * 1024U, 'x'));  // tek başına bir çerçeve
    f.put("work/b.md", "B");
    f.put("dest/x.md");
    f.load();
    f.select("work/");
    f.press("y");
    f.enter("dest/");
    f.store->impl().after_copy_batch = [&f] { f.put("dest/work/b.md", "someone else"); };
    f.press("p");
    CHECK(f.exists("dest/work/a.md"));
    CHECK(f.body("dest/work/b.md") == "someone else");
    CHECK_FALSE(f.exists("dest/work-2/a.md"));
    CHECK(has(f.summary(), "dest/work"));
    CHECK(has(f.summary(), "1 note(s)"));
    CHECK(f.changes == 1);
}

TEST_CASE("purging a note keeps an undo step that still holds folders") {
    Fixture f;
    f.put("a.md");
    REQUIRE(f.store->create_folder("empty"));
    f.load();
    f.select("empty/");
    f.press("x");
    f.select("a.md");
    f.press("x");
    f.press("d");  // notsuz klasör: onay yok
    REQUIRE_FALSE(f.confirm);
    REQUIRE(f.in_trash("a.md"));
    f.press("T");
    REQUIRE(f.state.selected_row());
    f.press("D");
    f.yes();  // tek öğe, forget_trashed
    f.press("T");
    REQUIRE_FALSE(f.state.trash_view());
    f.press("u");
    const auto folders = f.store->folders();
    CHECK(std::ranges::find(folders, "empty") != folders.end());
}
