#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "edit/display_text.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;
namespace fs = std::filesystem;

namespace {

core::NoteInfo info(const std::string& path, std::uintmax_t size = 1,
                    std::filesystem::file_time_type mtime = std::filesystem::file_time_type{} +
                                                            std::chrono::hours(1000)) {
    return core::NoteInfo{core::NotePath::parse(path).value(), mtime, size};
}

std::vector<core::NoteInfo> many(int count) {
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < count; ++i) {
        const std::string number = std::to_string(1000 + i);
        notes.push_back(
            info("n" + number + ".md", 1, std::filesystem::file_time_type{} + std::chrono::hours(5000 - i)));
    }
    return notes;
}

std::string row_of(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

ftxui::Screen draw(const tui::TuiState& state, int width = 100, int height = 24,
                   const tui::RenameState* rename = nullptr) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, {}, {"", ""}, {width, height}, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr, rename));
    return screen;
}

bool shows(const ftxui::Screen& screen, std::string_view text) {
    for (int y = 0; y < screen.dimy(); ++y)
        if (row_of(screen, y).find(text) != std::string::npos)
            return true;
    return false;
}

struct StoreFixture {
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
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { state.set_notes(store->list()); },
                                           [this] { ++changes; }}};

    StoreFixture() { core::init_i18n("en"); }
    void put(const std::string& name, std::string_view bytes) const {
        test::upsert_note(dir.path(), name, bytes);
    }
    void load() { state.set_notes(store->list()); }
    bool exists(const std::string& name) const { return test::note_exists(dir.path(), name); }
    void key(Key key, std::string_view text = {}) { actions.handle_rename_key(rename, key, text); }
    void type(std::string_view letters) {
        for (const char c : letters)
            key(Key::Char, std::string_view(&c, 1));
    }
};

struct FakeClock {
    std::chrono::steady_clock::time_point now{};
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
};

tui::TuiState review_state(FakeClock& clock, std::size_t lines = 10000) {
    tui::TuiState s;
    s.set_notes({info("a.md")});
    s.set_window(100, 35);  // 30 satırlık panel
    s.set_clock([&clock] { return clock.now; });
    s.set_preview_line_count(lines);
    s.handle(Key::Right);
    REQUIRE(s.focus() == Focus::Preview);
    return s;
}

}  // namespace

// ---- A: F2 yeniden adlandırma, ayarlar `,` ve F9 ----

TEST_CASE("F2 renames from List and REVIEW; it needs a selection and does nothing elsewhere") {
    tui::TuiState empty;
    CHECK(empty.handle(Key::F2) == Intent::None);
    tui::TuiState s;
    s.set_notes({info("work/plan.md")});
    CHECK(s.handle(Key::F2) == Intent::RenameSelected);
    s.handle(Key::Right);
    CHECK(s.handle(Key::F2) == Intent::RenameSelected);
    s.handle(Key::Left);
    s.handle(Key::Char, "/");
    CHECK(s.handle(Key::F2) == Intent::None);
    s.handle(Key::Escape);
    s.enter_editor();
    CHECK(s.handle(Key::F2) == Intent::EditorKey);
}

TEST_CASE("settings: comma in List and REVIEW, F9 everywhere; the editor gets F9 for its prompt") {
    tui::TuiState s;
    s.set_notes({info("a.md")});
    CHECK(s.handle(Key::Char, ",") == Intent::ShowSettings);
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    s.handle(Key::Right);
    CHECK(s.handle(Key::Char, ",") == Intent::ShowSettings);
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    s.handle(Key::Left);
    s.handle(Key::Char, "/");
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    s.handle(Key::Char, ",");
    CHECK(s.query() == ",");  // aramada virgül metindir
    s.handle(Key::Escape);
    s.enter_trash({});
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    s.leave_trash();
    s.enter_editor();
    CHECK(s.handle(Key::F9) == Intent::EditorKey);
}

TEST_CASE("the rename input starts prefilled with the cursor at the end of the stem") {
    tui::RenameState r(core::NotePath::parse("klasör/ad.md").value());
    CHECK(r.text() == "klasör/ad.md");
    CHECK(r.cursor() == std::string_view("klasör/ad").size());
    CHECK(r.handle(Key::Char, "ı") == tui::RenameState::Outcome::Editing);
    CHECK(r.text() == "klasör/adı.md");
    r.handle(Key::Left);
    r.handle(Key::Backspace);
    CHECK(r.text() == "klasör/aı.md");  // imleç ı'nın önünde: Backspace d'yi siler
    r.handle(Key::Delete);
    CHECK(r.text() == "klasör/a.md");  // Delete çok baytlı ı'yı bütün siler
    r.handle(Key::Home);
    CHECK(r.cursor() == 0);
    r.handle(Key::End);
    CHECK(r.cursor() == r.text().size());
    r.handle(Key::Right);
    CHECK(r.cursor() == r.text().size());
    r.set_error("x");
    r.handle(Key::Char, "y");
    CHECK(r.error().empty());
    CHECK(r.handle(Key::Enter) == tui::RenameState::Outcome::Submit);
    CHECK(r.handle(Key::Escape) == tui::RenameState::Outcome::Cancelled);
}

TEST_CASE("a note without an extension puts the cursor at the end") {
    tui::RenameState r(core::NotePath::parse("a/b.c/ad").value());
    CHECK(r.cursor() == r.text().size());
}

TEST_CASE("the rename input is drawn on the bottom line with the cursor inside the text") {
    tui::TuiState s;
    s.set_notes({info("work/plan.md")});
    REQUIRE(s.select_note(core::NotePath::parse("work/plan.md").value()));
    const tui::RenameState r(s.selected_row()->path);
    const auto screen = draw(s, 100, 24, &r);
    CHECK(row_of(screen, 23).find("work/plan▏.md") != std::string::npos);
}

TEST_CASE("Enter renames through NoteStore::move and keeps the selection on the note") {
    StoreFixture f;
    f.put("work/plan.md", "metin");
    f.put("other.md", "o");
    f.load();
    REQUIRE(f.state.select_note(core::NotePath::parse("work/plan.md").value()));
    f.actions.begin_rename(f.rename);
    REQUIRE(f.rename);
    f.type("-v2");
    f.key(Key::Enter);
    CHECK_FALSE(f.rename);
    CHECK(f.exists("work/plan-v2.md"));
    CHECK_FALSE(f.exists("work/plan.md"));
    CHECK(f.state.selected_row()->path.string() == "work/plan-v2.md");
    CHECK(f.changes == 1);
    CHECK(f.status.message.find("plan-v2") != std::string::npos);
}

TEST_CASE("a rename onto an existing note shows the error, overwrites nothing and keeps the input open") {
    StoreFixture f;
    f.put("a.md", "A");
    f.put("b.md", "B");
    f.load();
    REQUIRE(f.state.select_note(core::NotePath::parse("a.md").value()));
    f.actions.begin_rename(f.rename);
    f.key(Key::Backspace);
    f.type("b");
    REQUIRE(f.rename.has_value());
    REQUIRE(f.rename->text() == "b.md");
    f.key(Key::Enter);
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK(test::note_body(f.dir.path(), "a.md") == "A");
    CHECK(test::note_body(f.dir.path(), "b.md") == "B");
    CHECK(f.changes == 0);
    const auto screen = draw(f.state, 100, 24, &*f.rename);
    CHECK(shows(screen, f.rename->error().substr(0, 10)));
}

TEST_CASE("Esc cancels the rename, an unchanged name writes nothing, a bad path shows an error") {
    StoreFixture f;
    f.put("a.md", "A");
    f.load();
    f.actions.begin_rename(f.rename);
    f.type("zzz");
    f.key(Key::Escape);
    CHECK_FALSE(f.rename);
    CHECK(f.exists("a.md"));
    f.actions.begin_rename(f.rename);
    f.key(Key::Enter);
    CHECK_FALSE(f.rename);
    CHECK(f.changes == 0);
    f.actions.begin_rename(f.rename);
    f.key(Key::Home);
    f.type("../");
    f.key(Key::Enter);
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK(f.exists("a.md"));
}

TEST_CASE("leaving the trash after a restore selects the first row of the list") {
    StoreFixture f;
    f.put("b.md", "b");
    f.put("c.md", "c");
    f.put("a.md", "a");
    f.load();
    f.state.set_list_prefs({{search::SortField::Name, false}, tui::ListColumn::None});
    const std::string first = f.state.selected_row()->path.string();
    std::optional<tui::ConfirmState> confirm;
    const auto press = [&](std::string_view letter) {
        f.actions.run(f.state.handle(Key::Char, letter), confirm);
    };
    press("g");
    press("g");
    press("d");
    press("T");
    press("u");
    press("q");
    CHECK(f.state.selected() == 0);
    CHECK(f.state.selected_row()->path.string() == first);
}

// ---- B: Space, i, I ----

TEST_CASE("Space and i toggle the selected row; I toggles all rows") {
    tui::TuiState s;
    s.set_notes({info("a.md"), info("b.md"), info("c.md")});
    for (const std::string_view letter : {" ", "i"}) {
        CAPTURE(letter);
        CHECK(s.handle(Key::Char, letter) == Intent::None);
        CHECK(s.row_height(0) == 6);
        CHECK(s.row_height(1) == 1);
        s.handle(Key::Char, letter);
        CHECK(s.row_height(0) == 1);
    }
    s.handle(Key::Char, " ");  // biri açık: I hepsini açar
    s.handle(Key::Char, "I");
    for (std::size_t row = 0; row < 3; ++row)
        CHECK(s.row_height(row) == 6);
    s.handle(Key::Char, "I");  // hepsi açık: I hepsini kapatır
    for (std::size_t row = 0; row < 3; ++row)
        CHECK(s.row_height(row) == 1);
}

TEST_CASE("Space works in the trash view too and stays text in the search input") {
    tui::TuiState trash;
    trash.enter_trash({core::TrashItem{core::NotePath::parse("a.md").value(), {}, 1}});
    trash.handle(Key::Char, " ");
    CHECK(trash.row_height(0) == 6);
    tui::TuiState s;
    s.set_notes({info("a b.md")});
    s.handle(Key::Char, "/");
    s.handle(Key::Char, " ");
    CHECK(s.query() == " ");
    CHECK(s.row_height(0) == 1);
}

TEST_CASE("the list bar teaches Space for details in both languages") {
    core::init_i18n("tr");
    tui::TuiState s;
    s.set_notes({info("a.md")});
    CHECK(row_of(draw(s, 60, 15), 14).find("Space aç") != std::string::npos);
    core::init_i18n("en");
    CHECK(row_of(draw(s, 60, 15), 14).find("Space open") != std::string::npos);
}

// ---- C: sağ sütun ----

TEST_CASE("the default column shows size and date together; v cycles through all of them") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes({info("a.md", 3000)});
    using Column = tui::ListColumn;
    CHECK(s.list_column() == Column::SizeAndDate);
    const auto screen = draw(s);
    const std::string row = row_of(screen, 2);
    CHECK(row.find("2.9 KiB") != std::string::npos);
    CHECK(row.find("  " + tui::short_time_text(info("a.md").mtime, std::chrono::system_clock::now())) !=
          std::string::npos);
    const Column order[] = {Column::Size,   Column::Updated, Column::Created,    Column::Sync,
                            Column::Origin, Column::None,    Column::SizeAndDate};
    for (const Column expected : order) {
        CHECK(s.handle(Key::Char, "v") == Intent::SavePrefs);
        CHECK(s.list_column() == expected);
    }
}

TEST_CASE("short_time_text shows the time today and only the date on another day") {
    const auto now = std::chrono::system_clock::now();
    const auto file_now = std::chrono::file_clock::now();
    const std::string today = tui::short_time_text(file_now, now);
    CHECK(today.size() == 5);
    CHECK(today[2] == ':');
    const std::string old = tui::short_time_text(file_now - std::chrono::hours(24 * 40), now);
    CHECK(old.size() == 10);
    CHECK(old[4] == '-');
}

TEST_CASE("a narrow list drops the date, then the size, and never clips a value") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes({info("uzun-bir-not-adi-burada.md", 3000)});
    const std::string date = tui::short_time_text(info("a.md").mtime, std::chrono::system_clock::now());
    s.layout().set_list_width(70, 120);
    std::string row = row_of(draw(s, 120, 24), 2);
    CHECK(row.find("2.9 KiB  " + date) != std::string::npos);
    s.layout().set_list_width(32, 120);  // iç genişlik 30: tarih sığmaz, boyut yol kısalınca sığar
    row = row_of(draw(s, 120, 24), 2);
    CHECK(row.find("2.9 KiB") != std::string::npos);
    CHECK(row.find(date.substr(0, 3)) == std::string::npos);
    s.layout().set_list_width(tui::PaneLayout::kMinListWidth, 120);
    row = row_of(draw(s, 120, 24), 2);
    const std::string left = row.substr(0, row.find("││") == std::string::npos ? row.size() : row.find("││"));
    CHECK((left.find("2.9 KiB") != std::string::npos || left.find("KiB") == std::string::npos));
    CHECK(left.find("2.9") == left.find("2.9 KiB"));  // yarım değer yok
}

TEST_CASE("prefs: saved with a version; an old file's column none migrates once, a new one stays") {
    using Column = tui::ListColumn;
    CHECK(tui::parse_list_prefs("").column == Column::SizeAndDate);
    CHECK(tui::parse_list_prefs("column = \"none\"\n").column == Column::SizeAndDate);
    CHECK(tui::parse_list_prefs("column = \"size\"\n").column == Column::Size);
    const tui::ListPrefs none{{search::SortField::Updated, true}, Column::None};
    const std::string saved = tui::format_list_prefs(none);
    CHECK(saved.find("version") != std::string::npos);
    CHECK(tui::parse_list_prefs(saved).column == Column::None);
    const tui::ListPrefs both{{search::SortField::Updated, true}, Column::SizeAndDate};
    CHECK(tui::parse_list_prefs(tui::format_list_prefs(both)).column == Column::SizeAndDate);
}

// ---- D: satır numarası, toplamlar, sayfalama ----

TEST_CASE("rows are numbered, right-aligned to the width of the total, after the selection mark") {
    tui::TuiState s;
    s.set_notes(many(120));
    const auto screen = draw(s);
    CHECK(row_of(screen, 2).find("   1   n1000.md") != std::string::npos);
    CHECK(row_of(screen, 3).find("   2   n1001.md") != std::string::npos);
    s.handle(Key::Char, "G");
    CHECK(shows(draw(s), " 120   n1119.md"));
}

TEST_CASE("the line above the hints shows totals, matches and the page in both languages") {
    core::init_i18n("tr");
    tui::TuiState s;
    s.set_notes(many(248));
    s.set_window(100, 24);  // 19 satırlık panel: 14 sayfa
    auto screen = draw(s);
    CHECK(row_of(screen, 22) == " LİSTE  Klasör: 248 öğe · Toplam 248 not · sayfa 1/14");
    s.handle(Key::Char, "/");
    s.handle(Key::Char, "1");
    s.handle(Key::Char, "2");
    s.handle(Key::Char, "4");
    s.handle(Key::Escape);
    const std::size_t matches = s.rows().size();
    screen = draw(s);
    CHECK(row_of(screen, 22)
              .starts_with(" LİSTE  Toplam 248 not · " + std::to_string(matches) + " eşleşme · sayfa 1/"));
    core::init_i18n("en");
    CHECK(row_of(draw(s), 22)
              .starts_with(" LIST  Total 248 notes · " + std::to_string(matches) + " matches · page 1/"));
}

TEST_CASE("the page number follows the selection") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes(many(100));
    s.set_window(100, 24);
    s.handle(Key::PageDown);
    CHECK(row_of(draw(s), 22).ends_with("page 2/6"));
    s.handle(Key::Char, "G");
    CHECK(row_of(draw(s), 22).ends_with("page 6/6"));
    s.handle(Key::Char, "g");
    s.handle(Key::Char, "g");
    CHECK(row_of(draw(s), 22).ends_with("page 1/6"));
}

TEST_CASE("PgDn and PgUp page the list in List focus and leave the preview alone") {
    tui::TuiState s;
    s.set_notes(many(100));
    s.set_window(100, 24);
    s.set_preview_line_count(1000);
    s.handle(Key::PageDown);
    CHECK(s.selected() == 19);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::PageDown);
    CHECK(s.selected() == 38);
    s.handle(Key::PageUp);
    CHECK(s.selected() == 19);
    for (int i = 0; i < 10; ++i)
        s.handle(Key::PageDown);
    CHECK(s.selected() == 99);
    s.handle(Key::Char, "g");
    s.handle(Key::Char, "g");
    s.handle(Key::PageUp);
    CHECK(s.selected() == 0);
}

// ---- E: REVIEW gezinmesi ----

TEST_CASE("REVIEW: Down accelerates on key repeat up to a third of a page and resets on a pause") {
    FakeClock clock;
    auto s = review_state(clock);
    constexpr int kRepeatMs = 30;
    std::vector<std::size_t> steps;
    std::size_t previous = 0;
    for (int i = 0; i < 80; ++i) {
        clock.advance(kRepeatMs);
        s.handle(Key::Down);
        steps.push_back(s.preview_scroll() - previous);
        previous = s.preview_scroll();
    }
    CHECK(steps.front() == 1);
    CHECK(steps.back() == 10);  // 30 satırlık panelin üçte biri
    CHECK(std::is_sorted(steps.begin(), steps.end()));
    clock.advance(500);
    previous = s.preview_scroll();
    s.handle(Key::Down);
    CHECK(s.preview_scroll() - previous == 1);
}

TEST_CASE("REVIEW: changing direction resets the acceleration") {
    FakeClock clock;
    auto s = review_state(clock);
    for (int i = 0; i < 80; ++i) {
        clock.advance(30);
        s.handle(Key::Down);
    }
    const std::size_t at = s.preview_scroll();
    clock.advance(30);
    s.handle(Key::Up);
    CHECK(at - s.preview_scroll() == 1);
}

TEST_CASE("REVIEW: Ctrl+D and Ctrl+U move half a page, J and K five lines, within bounds") {
    FakeClock clock;
    auto s = review_state(clock, 100);
    s.handle(Key::CtrlD);
    CHECK(s.preview_scroll() == 15);
    s.handle(Key::CtrlU);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::CtrlU);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::Char, "J");
    CHECK(s.preview_scroll() == 5);
    s.handle(Key::Char, "K");
    CHECK(s.preview_scroll() == 0);
    for (int i = 0; i < 10; ++i)
        s.handle(Key::CtrlD);
    CHECK(s.preview_scroll() == 99);
}

TEST_CASE("fast selection moves read only the final note; a single move reads at once") {
    StoreFixture f;
    for (const char* name : {"a.md", "b.md", "c.md", "d.md"})
        f.put(name, name);
    f.load();
    FakeClock clock;
    clock.advance(1000);
    const auto wall = [&] { return clock.now; };
    tui::PreviewCache cache;
    const auto shown = [&] { return &cache.throttled(f.state, *f.store, wall()); };
    shown();
    const std::size_t first = cache.generation();
    clock.advance(1000);
    f.state.handle(Key::Down);  // sakin kullanıcı: bekleme yok
    CHECK(shown()->lines.front() == f.state.selected_row()->path.string());
    CHECK(cache.generation() == first + 1);
    CHECK_FALSE(cache.deferred());
    for (int i = 0; i < 2; ++i) {  // art arda: okuma yok
        clock.advance(20);
        f.state.handle(Key::Down);
        shown();
        CHECK(cache.deferred());
    }
    CHECK(cache.generation() == first + 1);
    clock.advance(tui::PreviewCache::kCoalesceMs);
    CHECK(shown()->lines.front() == f.state.selected_row()->path.string());
    CHECK(cache.generation() == first + 2);
    CHECK_FALSE(cache.deferred());
}

TEST_CASE("an invalidated note is reread at once even inside the coalesce window") {
    StoreFixture f;
    f.put("a.md", "bir");
    f.load();
    FakeClock clock;
    clock.advance(1000);
    tui::PreviewCache cache;
    CHECK(cache.throttled(f.state, *f.store, clock.now).lines.front() == "bir");
    f.put("a.md", "iki");
    cache.invalidate();
    clock.advance(1);
    CHECK(cache.throttled(f.state, *f.store, clock.now).lines.front() == "iki");
}
