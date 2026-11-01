#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "graph/graph.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/display_text.hpp"
#include "tui/editor_session.hpp"
#include "tui/list_actions.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/sync_status_worker.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <array>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;
namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 24;

core::NoteInfo info(const std::string& path, int age_hours = 0, std::uintmax_t size = 1) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(1000 - age_hours), size};
}

std::vector<core::NoteInfo> sample() {
    return {info("work/meeting.md", 0, 10), info("personal/todo.md", 1, 3000),
            info("snippets/sort.java", 2, 200)};
}

void type(tui::TuiState& s, std::string_view text) {
    for (char c : text)
        s.handle(Key::Char, std::string_view(&c, 1));
}

std::vector<std::string> row_paths(const tui::TuiState& s) {
    std::vector<std::string> paths;
    for (const tui::Row& row : s.rows())
        paths.push_back(row.path.string());
    return paths;
}

std::string screen_text(const tui::TuiState& state, const tui::Preview& preview = {}) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, preview, {"", ""}, {kWidth, kHeight}));
    return screen.ToString();
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

int plain_width(char32_t) {
    return 1;
}

// Gerçek store ve ListActions.
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
    std::optional<tui::ConfirmState> confirm;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { state.set_notes(store->list()); },
                                           [this] { ++changes; }}};

    StoreFixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
    }
    void put(const std::string& name, std::string_view bytes) const {
        fs::create_directories((dir.path() / name).parent_path());
        test::upsert_note(dir.path(), name, bytes);
    }
    void load() { state.set_notes(store->list()); }
    bool exists(const std::string& name) const { return test::note_exists(dir.path(), name); }
    bool in_trash(const std::string& name) const { return test::note_in_trash(dir.path(), name); }
    void press(std::string_view letter) { actions.run(state.handle(Key::Char, letter), confirm); }
    void select(const std::string& name) { REQUIRE(state.select_note(core::NotePath::parse(name).value())); }
};

}  // namespace

TEST_CASE("slash: query arguments filter and sort, the text still fuzzy matches") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Char, "/");
    type(s, "type:md");
    CHECK(row_paths(s) == std::vector<std::string>{"work/meeting.md", "personal/todo.md"});
    CHECK_FALSE(s.query_error());
    s.handle(Key::Escape);
    s.handle(Key::Escape);
    s.handle(Key::Char, "/");
    type(s, "sort:-size");
    CHECK(row_paths(s).front() == "personal/todo.md");
    s.handle(Key::Escape);
    s.handle(Key::Escape);
    s.handle(Key::Char, "/");
    type(s, "in:work/ mee");
    CHECK(row_paths(s) == std::vector<std::string>{"work/meeting.md"});
    s.handle(Key::Escape);
    s.handle(Key::Escape);
    s.handle(Key::Char, "/");
    type(s, "size:>1kb");
    CHECK(row_paths(s) == std::vector<std::string>{"personal/todo.md"});
}

TEST_CASE("slash: an invalid argument shows its located error and is never treated as text") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes({info("size-plan.md"), info("other.md")});
    s.handle(Key::Char, "/");
    type(s, "size:abc");
    REQUIRE(s.query_error());
    CHECK(s.query_error()->begin == 5);
    // Son geçerli liste kalır ("size" süzmesi): "size:abc" metin sayılsaydı hiçbir not eşleşmezdi.
    CHECK(row_paths(s) == std::vector<std::string>{"size-plan.md"});
    CHECK(s.handle(Key::Enter) == Intent::None);
    CHECK(s.focus() == Focus::List);
    s.handle(Key::Escape);
    CHECK_FALSE(s.query_error());
    tui::TuiState empty;
    empty.handle(Key::Char, "/");
    type(empty, "size:abc");
    CHECK(empty.handle(Key::Enter) == Intent::None);  // eşleşme yok ama taslak da yok
    const std::string shown = screen_text(s);
    tui::TuiState bad;
    bad.set_notes(sample());
    bad.handle(Key::Char, "/");
    type(bad, "sync:nope");
    const std::string message = screen_text(bad);
    CHECK(has(message, "6:"));
    CHECK(has(message, "nope"));
}

TEST_CASE("slash: a filter that matches nothing does not start a draft from the arguments") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Char, "/");
    type(s, "type:xyz");
    CHECK(s.rows().empty());
    CHECK(s.handle(Key::Enter) == Intent::None);
}

TEST_CASE("sync: argument follows statuses delivered later, and they survive a rescan") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Char, "/");
    type(s, "sync:modified");
    CHECK(s.rows().empty());
    s.set_sync_states({{core::NotePath::parse("personal/todo.md").value(), core::SyncState::Modified}});
    CHECK(row_paths(s) == std::vector<std::string>{"personal/todo.md"});
    s.set_notes(sample());
    CHECK(row_paths(s) == std::vector<std::string>{"personal/todo.md"});
    CHECK(s.meta_of(core::NotePath::parse("personal/todo.md").value())->sync_status ==
          core::SyncState::Modified);
    CHECK(s.meta_of(core::NotePath::parse("work/meeting.md").value())->sync_status ==
          core::SyncState::SyncOff);
}

TEST_CASE("f: global search shows the hit line; Enter opens REVIEW scrolled to the hit") {
    StoreFixture f;
    std::string body;
    for (int i = 0; i < 40; ++i)
        body += "satir " + std::to_string(i) + "\n";
    body += "aranan kelime burada\n";
    f.put("uzun.md", body);
    f.put("diger.md", "baska\n");
    f.load();
    search::ContentIndex index(*f.store, 1024 * 1024);
    index.refresh();
    f.state.set_global_searcher([&](std::string_view query, std::span<const core::NoteMeta> metas) {
        return search::search_all(query, metas, index);
    });
    CHECK(f.state.handle(Key::Char, "f") == Intent::None);
    CHECK(f.state.focus() == Focus::Search);
    CHECK(f.state.mode() == tui::Mode::Global);
    type(f.state, "aranan");
    REQUIRE(row_paths(f.state) == std::vector<std::string>{"uzun.md"});
    const tui::Row& row = *f.state.selected_row();
    REQUIRE(row.hits.size() == 1);
    CHECK(row.hits.front().line == 41);
    CHECK(has(screen_text(f.state), "kelime burada"));
    CHECK(f.state.handle(Key::Enter) == Intent::None);
    CHECK(f.state.focus() == Focus::Preview);
    CHECK(f.state.preview_scroll() == 38);  // eşleşmenin üstünde iki satır bağlam
    f.state.handle(Key::Left);
    f.state.handle(Key::Escape);
    CHECK(f.state.mode() == tui::Mode::Name);
    CHECK(f.state.query().empty());
    CHECK(f.state.rows().size() == 2);
}

TEST_CASE("f: arguments work and an invalid value shows an error") {
    StoreFixture f;
    f.put("a.md", "icerik\n");
    f.put("b.txt", "icerik\n");
    f.load();
    search::ContentIndex index(*f.store, 1024 * 1024);
    index.refresh();
    f.state.set_global_searcher([&](std::string_view query, std::span<const core::NoteMeta> metas) {
        return search::search_all(query, metas, index);
    });
    f.state.handle(Key::Char, "f");
    type(f.state, "type:md icerik");
    CHECK(row_paths(f.state) == std::vector<std::string>{"a.md"});
    f.state.handle(Key::Escape);
    f.state.handle(Key::Escape);
    f.state.handle(Key::Char, "f");
    type(f.state, "updated:xx");
    CHECK(f.state.query_error());
}

TEST_CASE("d trashes with no confirmation, the selection stays near, u restores") {
    StoreFixture f;
    f.put("a.md", "a");
    f.put("b.md", "b");
    f.put("c.md", "c");
    f.load();
    f.select("b.md");
    const std::size_t index = f.state.selected();
    f.press("d");
    CHECK_FALSE(f.confirm);
    CHECK_FALSE(f.exists("b.md"));
    CHECK(f.in_trash("b.md"));
    CHECK(f.state.selected() == std::min(index, f.state.rows().size() - 1));
    CHECK(f.changes == 1);
    CHECK(has(f.status.message, "b.md"));
    f.press("u");
    CHECK(f.exists("b.md"));
    CHECK_FALSE(f.in_trash("b.md"));
    CHECK(f.state.selected_row()->path.string() == "b.md");
    CHECK(f.changes == 2);
    f.press("u");  // başka geri alınacak yok
    CHECK(has(f.status.message, "Nothing"));
    CHECK(f.changes == 2);
}

TEST_CASE("D deletes permanently only after confirming; the default is Cancel") {
    StoreFixture f;
    f.put("a.md", "a");
    f.load();
    f.press("D");
    REQUIRE(f.confirm);
    CHECK_FALSE(f.confirm->yes_selected());
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    f.confirm.reset();
    CHECK(f.exists("a.md"));
    f.press("D");
    REQUIRE(f.confirm);
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.confirm);
    CHECK_FALSE(f.exists("a.md"));
    CHECK_FALSE(f.in_trash("a.md"));
}

TEST_CASE("T shows the trash: u restores the selected, D purges after confirming, q and Esc go back") {
    StoreFixture f;
    f.put("work/a.md", "a");
    f.put("b.md", "b");
    f.load();
    f.select("b.md");
    f.press("d");
    f.select("work/a.md");
    f.press("d");
    f.press("T");
    REQUIRE(f.state.trash_view());
    CHECK(f.state.rows().size() == 2);
    CHECK(has(screen_text(f.state), "[trash]"));
    for (const std::string_view ignored : {"e", "n", "a", "r", "s", "/", "f", "c", "o", "v", "d"})
        CHECK(f.state.handle(Key::Char, ignored) == Intent::None);
    CHECK(f.state.handle(Key::Enter) == Intent::None);
    CHECK(f.state.handle(Key::Right) == Intent::None);
    REQUIRE(f.state.select_note(core::NotePath::parse("work/a.md").value()));
    f.press("u");
    CHECK(f.exists("work/a.md"));
    CHECK(f.state.trash_view());
    CHECK(f.state.rows().size() == 1);
    f.press("D");
    REQUIRE(f.confirm);
    CHECK_FALSE(f.confirm->yes_selected());
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.in_trash("b.md"));
    CHECK(f.state.rows().empty());
    f.press("q");
    CHECK_FALSE(f.state.trash_view());
    f.press("T");
    CHECK(f.state.trash_view());
    f.actions.run(f.state.handle(Key::Escape), f.confirm);
    CHECK_FALSE(f.state.trash_view());
    CHECK(f.state.rows().size() == 1);
}

TEST_CASE("the trash view has a clear title and its own hints at 60 columns in both languages") {
    const std::pair<const char*, std::array<const char*, 5>> expected[] = {
        {"en", {"TRASH", "u restore", "D delete forever", "E empty", "Esc/q back"}},
        {"tr", {"ÇÖP KUTUSU", "u geri getir", "D kalıcı sil", "E boşalt", "Esc/q geri"}},
    };
    for (const auto& [language, parts] : expected) {
        core::init_i18n(language);
        tui::TuiState s;
        s.enter_trash({core::TrashItem{core::NotePath::parse("a.md").value(), {}, 1}});
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(15));
        ftxui::Render(screen, tui::render(s, {}, {"", ""}, {60, 15}));
        const std::string shown = screen.ToString();
        for (const char* part : parts) {
            CAPTURE(language);
            CAPTURE(part);
            CHECK(has(shown, part));
        }
    }
    core::init_i18n("en");
}

TEST_CASE("REVIEW shows the trashed note, not a live note with the same name") {
    StoreFixture f;
    f.put("a.md", "silinen metin\n");
    f.load();
    f.press("d");
    f.put("a.md", "canli metin\n");
    f.load();
    const tui::Preview& live = f.preview.current(f.state, *f.store);
    REQUIRE(live.lines.size() >= 1);
    CHECK(live.lines.front() == "canli metin");
    f.press("T");
    REQUIRE(f.state.trash_view());
    const tui::Preview& trashed = f.preview.current(f.state, *f.store);
    REQUIRE(trashed.kind == tui::Preview::Kind::Text);
    CHECK(trashed.lines.front() == "silinen metin");
    CHECK(has(screen_text(f.state, trashed), "silinen metin"));
    f.press("q");
    CHECK(f.preview.current(f.state, *f.store).lines.front() == "canli metin");
}

TEST_CASE("REVIEW in the trash is read-only: letters never reach an editor") {
    StoreFixture f;
    f.put("a.md", "metin\n");
    f.load();
    f.press("d");
    f.press("T");
    CHECK(f.state.handle(Key::Enter) == Intent::None);
    CHECK(f.state.handle(Key::Char, "e") == Intent::None);
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE("E empties the trash only after confirming; the default is Cancel") {
    StoreFixture f;
    f.put("a.md", "a");
    f.put("b.md", "b");
    f.load();
    f.press("d");
    f.press("d");
    f.press("T");
    REQUIRE(f.state.rows().size() == 2);
    const int changes = f.changes;
    f.press("E");
    REQUIRE(f.confirm);
    CHECK(has(f.confirm->message(), "2"));
    CHECK_FALSE(f.confirm->yes_selected());
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    f.confirm.reset();
    CHECK(f.in_trash("a.md"));
    CHECK(f.in_trash("b.md"));
    CHECK(f.changes == changes);
    f.press("E");
    REQUIRE(f.confirm);
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.confirm);
    CHECK_FALSE(f.in_trash("a.md"));
    CHECK_FALSE(f.in_trash("b.md"));
    CHECK(f.state.rows().empty());
    CHECK(f.state.trash_view());
    CHECK(f.changes == changes + 1);
    CHECK(f.state.handle(Key::Char, "E") == Intent::None);  // boş çöpte onay açılmaz
}

TEST_CASE("cancelling Empty does not turn the next single purge into an empty") {
    StoreFixture f;
    f.put("a.md", "a");
    f.put("b.md", "b");
    f.load();
    f.press("d");
    f.press("d");
    f.press("T");
    f.press("E");
    REQUIRE(f.confirm);
    f.confirm.reset();  // Vazgeç: main yalnız sıfırlar
    f.press("D");
    REQUIRE(f.confirm);
    f.actions.finish_delete(f.confirm);
    CHECK(f.state.rows().size() == 1);
}

TEST_CASE("clicking the trash indicator opens the trash view; the rest of the top line does not") {
    StoreFixture f;
    f.put("a.md", "a");
    f.load();
    tui::EditorSession editor(
        tui::EditorHost{*f.store, f.mutex, f.internal, plain_width, 1024, core::kDefaultSlugMaxLength,
                        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {},
                        [] {}, [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}});
    f.press("d");
    const auto press_at = [](int x, int y) {
        return tui::MouseInput{x, y, tui::MouseButton::Left, tui::MouseMotion::Press};
    };
    f.state.set_trash_count(1);
    f.actions.on_mouse(press_at(9, 0), editor);  // "trash: 1" 8 hücre
    CHECK_FALSE(f.state.trash_view());
    f.actions.on_mouse(press_at(0, 1), editor);  // çerçeve satırı
    CHECK_FALSE(f.state.trash_view());
    f.state.set_trash_count(0);
    f.actions.on_mouse(press_at(0, 0), editor);  // boş çöp: gösterge yok
    CHECK_FALSE(f.state.trash_view());
    f.state.set_trash_count(1);
    f.actions.on_mouse(press_at(7, 0), editor);
    CHECK(f.state.trash_view());
    CHECK(f.state.rows().size() == 1);
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE("clicking the row marker toggles its details and selects it; clicking the path only selects") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes(sample());
    s.set_window(kWidth, kHeight);
    constexpr int kFirstRowY = 2;
    constexpr int kMarkerX = 4;  // kenar, seçim, numara, boşluk
    constexpr int kPathX = 6;
    const test::TempDir dir;
    core::NoteStore store(dir.path());
    std::mutex mutex;
    std::string clip;
    tui::EditorSession editor(
        tui::EditorHost{store, mutex, clip, plain_width, 1024, core::kDefaultSlugMaxLength,
                        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {},
                        [] {}, [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}});
    const auto click = [&](int x, int y) {
        tui::route_mouse({x, y, tui::MouseButton::Left, tui::MouseMotion::Press}, s, editor);
    };
    click(kPathX, kFirstRowY + 1);
    CHECK(s.selected() == 1);
    CHECK(s.row_height(1) == 1);
    click(kMarkerX, kFirstRowY + 1);
    CHECK(s.selected() == 1);
    CHECK(s.row_height(1) == 6);
    // Açık satırın ayrıntı satırlarında işaret yok: aynı sütuna tıklamak yalnız seçer.
    click(kMarkerX, kFirstRowY + 2);
    CHECK(s.selected() == 1);
    CHECK(s.row_height(1) == 6);
    click(kMarkerX, kFirstRowY + 1);
    CHECK(s.row_height(1) == 1);
}

TEST_CASE("Alt arrows and page keys work in the trash view") {
    core::init_i18n("en");
    tui::TuiState s;
    s.enter_trash({core::TrashItem{core::NotePath::parse("a.md").value(), {}, 1}});
    s.set_preview_line_count(100);
    s.handle(Key::AltRight);
    CHECK(s.row_height(0) == 6);
    s.handle(Key::AltLeft);
    CHECK(s.row_height(0) == 1);
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == tui::TuiState::kPreviewPageLines);
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 0);
}

TEST_CASE("an editor open on a trashed note follows the not-found path") {
    StoreFixture f;
    f.put("a.md", "a");
    f.load();
    int errors = 0;
    tui::EditorSession editor(tui::EditorHost{
        *f.store, f.mutex, f.internal, plain_width, 1024, core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {}, [] {},
        [](std::optional<std::size_t>) {}, [&](const core::Error&) { ++errors; }, [] {}});
    editor.open(f.state.selected_row()->path);
    REQUIRE(f.store->trash(f.state.selected_row()->path));
    editor.on_listing(f.store->list());
    CHECK_FALSE(editor.is_open());
    CHECK(errors == 1);
}

TEST_CASE("Alt arrows expand and collapse rows; navigation and clicks walk over the extra lines") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes(sample());
    CHECK(s.row_height(0) == 1);
    s.handle(Key::AltRight);
    CHECK(s.row_height(0) == 6);
    s.handle(Key::Down);
    s.handle(Key::AltRight);
    CHECK(s.row_height(1) == 6);
    CHECK(s.row_height(0) == 6);  // birden çok satır açık kalır
    CHECK(s.selected() == 1);
    s.handle(Key::Down);
    CHECK(s.selected() == 2);
    s.handle(Key::Up);
    CHECK(s.selected() == 1);
    s.handle(Key::AltLeft);
    CHECK(s.row_height(1) == 1);
    CHECK(s.row_height(0) == 6);

    // Görsel satır -> not satırı.
    CHECK(s.row_at(10, 0) == 0);
    CHECK(s.row_at(10, 5) == 0);
    CHECK(s.row_at(10, 6) == 1);
    CHECK(s.row_at(10, 7) == 2);

    // Seçili satır pencerede kalır; açık satır yukarıdan sayılır.
    s.handle(Key::Char, "g");
    s.handle(Key::AltRight);
    s.handle(Key::Char, "G");
    CHECK(s.list_top(7) == 1);
    CHECK(s.list_top(20) == 0);

    const std::string shown = screen_text(s);
    CHECK(has(shown, "unknown"));  // created ve origin henüz yok
    core::init_i18n("tr");
    CHECK(has(screen_text(s), "bilinmiyor"));
    core::init_i18n("en");
}

TEST_CASE("a click on an expanded line selects its row") {
    StoreFixture f;
    f.put("a.md", "a");
    f.put("b.md", "b");
    f.load();
    f.state.handle(Key::AltRight);
    const std::string below = f.state.rows()[1].path.string();
    tui::EditorSession editor(
        tui::EditorHost{*f.store, f.mutex, f.internal, plain_width, 1024, core::kDefaultSlugMaxLength,
                        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {},
                        [] {}, [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}});
    // İlk satır y=2, açık satırın 5 ek satırı var: y=8 ikinci nottur, y=5 ilk notun ek satırı.
    tui::route_mouse({1, 8, tui::MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.selected_row()->path.string() == below);
    tui::route_mouse({1, 5, tui::MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.selected() == 0);
}

TEST_CASE("v cycles the right column; o opens the sort menu; both are saved") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes(sample());
    using Column = tui::ListColumn;
    CHECK(s.list_column() == Column::SizeAndDate);
    CHECK(has(screen_text(s), "2.9 KiB"));

    CHECK(s.handle(Key::Char, "o") == Intent::None);
    CHECK(s.focus() == Focus::SortMenu);
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.focus() == Focus::List);
    s.handle(Key::Char, "o");
    CHECK(s.handle(Key::Char, "s") == Intent::SavePrefs);
    CHECK(s.focus() == Focus::List);
    CHECK(s.list_sort().field == search::SortField::Size);
    CHECK(s.list_sort().descending);
    CHECK(row_paths(s).front() == "personal/todo.md");
    CHECK(has(screen_text(s), "size"));
    s.handle(Key::Char, "o");
    CHECK(s.handle(Key::Char, "r") == Intent::SavePrefs);
    CHECK_FALSE(s.list_sort().descending);
    CHECK(row_paths(s).front() == "work/meeting.md");
    s.handle(Key::Char, "o");
    s.handle(Key::Char, "n");
    CHECK(s.list_sort().field == search::SortField::Name);
    CHECK_FALSE(s.list_sort().descending);
    s.handle(Key::Char, "o");
    CHECK(s.handle(Key::Char, "x") == Intent::None);  // bilinmeyen harf menüyü kapatmaz
    CHECK(s.focus() == Focus::SortMenu);
}

TEST_CASE("prefs: round trip through tui-state.toml; broken values fall back to defaults") {
    using Column = tui::ListColumn;
    const tui::ListPrefs prefs{{search::SortField::Size, false}, Column::Origin};
    const auto parsed = tui::parse_list_prefs(tui::format_list_prefs(prefs));
    CHECK(parsed.sort.field == search::SortField::Size);
    CHECK_FALSE(parsed.sort.descending);
    CHECK(parsed.column == Column::Origin);
    const auto broken = tui::parse_list_prefs("sort = \"wat\"\ncolumn = 9\nx y z\n");
    CHECK(broken.sort.field == search::SortField::Updated);
    CHECK(broken.sort.descending);
    CHECK(broken.column == Column::SizeAndDate);

    StoreFixture f;
    f.put("a.md", "a");
    f.load();
    f.press("v");
    f.state.handle(Key::Char, "o");
    f.actions.run(f.state.handle(Key::Char, "s"), f.confirm);
    const fs::path file = core::state_dir() / "tui-state.toml";
    REQUIRE(fs::exists(file));
    tui::TuiState fresh;
    tui::load_list_prefs(fresh);
    CHECK(fresh.list_column() == Column::Size);
    CHECK(fresh.list_sort().field == search::SortField::Size);
}

TEST_CASE("terminal_safe replaces control, C1 and bidi characters and keeps text") {
    CHECK(tui::terminal_safe("düz metin") == "düz metin");
    CHECK(tui::terminal_safe("a\x1B[31mb") == "a\xEF\xBF\xBD[31mb");
    CHECK(tui::terminal_safe("a\xC2\x9B"
                             "31m") == "a\xEF\xBF\xBD"
                                       "31m");  // C1 CSI
    CHECK(tui::terminal_safe("gpj\xE2\x80\xAE"
                             "exe") == "gpj\xEF\xBF\xBD"
                                       "exe");  // sağdan sola geçersiz kılma
    CHECK(tui::terminal_safe("\xE2\x81\xA6x") == "\xEF\xBF\xBDx");
    CHECK(tui::terminal_safe("a\xFF") == "a\xEF\xBF\xBD");
    CHECK(tui::human_size(512) == "512 B");
    CHECK(tui::human_size(2048) == "2.0 KiB");
}

TEST_CASE("snippets and trash paths are sanitized before rendering, without moving the highlight") {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_notes({info("a.md")});
    const std::string evil = "x\xE2\x80\xAE\x1B[31mkelime";
    s.set_global_searcher([&](std::string_view, std::span<const core::NoteMeta>) {
        search::GlobalHit hit{0, core::NotePath::parse("a.md").value()};
        hit.content_matched = true;
        hit.content_hits = 1;
        hit.best_line = search::Hit{1, evil, {{evil.size() - 6, evil.size()}}};
        return std::expected<std::vector<search::GlobalHit>, search::QueryError>(std::vector{hit});
    });
    s.handle(Key::Char, "f");
    type(s, "k");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(s, {}, {"", ""}, {kWidth, kHeight}));
    const std::string shown = screen.ToString();
    CHECK(has(shown, "kelime"));
    CHECK_FALSE(has(shown, "\xE2\x80\xAE"));
    CHECK(has(shown, "\xEF\xBF\xBD"));

    tui::TuiState trash;
    trash.enter_trash({core::TrashItem{core::NotePath::parse("gpj\xE2\x80\xAE"
                                                             "exe.md")
                                           .value(),
                                       {},
                                       5}});
    const std::string listed = screen_text(trash);
    CHECK_FALSE(has(listed, "\xE2\x80\xAE"));
    CHECK(has(listed, "gpj\xEF\xBF\xBD"
                      "exe.md"));
}

TEST_CASE("sync status worker delivers off the calling thread and coalesces requests") {
    core::Config config;
    config.sync.enabled = false;
    std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::size_t delivered = 0;
    const std::vector<core::NoteMeta> metas = {core::NoteMeta{core::NotePath::parse("a.md").value()},
                                               core::NoteMeta{core::NotePath::parse("b.md").value()}};
    {
        tui::SyncStatusWorker worker([&](tui::SyncStates states) {
            const std::scoped_lock lock(mutex);
            threads.push_back(std::this_thread::get_id());
            delivered = states.size();
        });
        worker.request(config, metas);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const std::scoped_lock lock(mutex);
            if (!threads.empty())
                break;
        }
    }
    REQUIRE_FALSE(threads.empty());
    CHECK(threads.front() != std::this_thread::get_id());
    CHECK(delivered == 2);
}

// ---- graph depo: üst veri ön yüze gerçekten ulaşır ----

TEST_CASE("graph store: created and origin really show in row details, the v column and queries") {
    StoreFixture f;
    const auto old = std::chrono::system_clock::now() - std::chrono::hours(48);
    test::upsert_note(f.dir.path(), "eski.md", "e\n", old);
    test::upsert_note(f.dir.path(), "yeni.md", "y\n");
    f.load();
    REQUIRE(f.state.rows().size() == 2);
    const core::NoteMeta* meta = f.state.meta_of(core::NotePath::parse("eski.md").value());
    REQUIRE(meta != nullptr);
    REQUIRE(meta->created_at.has_value());
    REQUIRE(meta->origin.has_value());
    const std::string origin = *meta->origin;
    const std::string created = tui::local_time_text(*meta->created_at);

    f.select("eski.md");
    f.press(" ");
    const std::string details = screen_text(f.state);
    CHECK(has(details, "created: " + created));
    CHECK(has(details, "origin: " + origin));
    CHECK_FALSE(has(details, "unknown"));
    f.press(" ");

    tui::ListPrefs prefs;
    prefs.column = tui::ListColumn::Origin;
    f.state.set_list_prefs(prefs);
    CHECK(has(screen_text(f.state), origin));
    prefs.column = tui::ListColumn::Created;
    f.state.set_list_prefs(prefs);
    const std::string created_column = screen_text(f.state);
    CHECK(has(created_column, created));
    CHECK_FALSE(has(created_column, "unknown"));

    const auto replace_query = [&](const std::string& query) {
        while (!f.state.query().empty())
            f.state.handle(Key::Backspace);
        type(f.state, query);
    };
    f.state.handle(Key::Char, "/");
    replace_query("sort:created");
    CHECK(row_paths(f.state) == std::vector<std::string>{"eski.md", "yeni.md"});
    replace_query("sort:-created");
    CHECK(row_paths(f.state) == std::vector<std::string>{"yeni.md", "eski.md"});
    replace_query("origin:" + origin);
    CHECK(f.state.rows().size() == 2);
    replace_query("origin:plan9");
    CHECK(f.state.rows().empty());
    CHECK_FALSE(f.state.query_error());
}

TEST_CASE("graph store: a trashed note keeps its created and origin in the trash view") {
    StoreFixture f;
    f.put("a.md", "a\n");
    f.load();
    f.actions.run(tui::Intent::TrashSelected, f.confirm);
    f.actions.run(tui::Intent::ToggleTrash, f.confirm);
    REQUIRE(f.state.trash_view());
    REQUIRE(f.state.rows().size() == 1);
    const core::NoteMeta* meta = f.state.meta_of(core::NotePath::parse("a.md").value());
    REQUIRE(meta != nullptr);
    CHECK(meta->created_at.has_value());
    CHECK(meta->origin.has_value());
    f.press(" ");
    CHECK_FALSE(has(screen_text(f.state), "unknown"));
}

TEST_CASE("graph store: the list keeps local write numbers, or every note would read as synced") {
    StoreFixture f;
    f.put("a.md", "a\n");
    f.load();
    const std::vector<core::NoteMeta> from_store = f.store->list_meta();
    REQUIRE(from_store.size() == 1);
    REQUIRE(from_store.front().local_seq > 0);
    REQUIRE(f.state.metas().size() == 1);
    CHECK(f.state.metas().front().local_seq == from_store.front().local_seq);
    CHECK(f.state.metas().front().created_local_seq == from_store.front().created_local_seq);
}

TEST_CASE("trash preview: the body comes through the store API and never mixes with a live note") {
    StoreFixture f;
    f.put("a.md", "copte kalan govde\n");
    f.put("b.md", "canli\n");
    f.load();
    f.select("a.md");
    f.actions.run(tui::Intent::TrashSelected, f.confirm);
    f.actions.run(tui::Intent::ToggleTrash, f.confirm);
    REQUIRE(f.state.selected_row() != nullptr);
    const tui::Preview& preview = f.preview.current(f.state, *f.store);
    REQUIRE(preview.kind == tui::Preview::Kind::Text);
    CHECK(preview.text == "copte kalan govde\n");
    f.put("a.md", "yeni canli a\n");
    f.preview.invalidate();
    CHECK(f.preview.current(f.state, *f.store).text == "copte kalan govde\n");
    f.actions.run(tui::Intent::ToggleTrash, f.confirm);
    f.load();
    f.select("a.md");
    CHECK(f.preview.current(f.state, *f.store).text == "yeni canli a\n");
}

TEST_CASE("u: a failed restore keeps the item, the next u brings it back") {
    StoreFixture f;
    f.put("a.md", "a\n");
    f.load();
    f.actions.run(tui::Intent::TrashSelected, f.confirm);
    REQUIRE(f.in_trash("a.md"));
    {
        // Yazma kilidi başka bağlantıda: gerçek yazma hatası.
        auto holder = graph::Graph::open(f.dir.path() / "graph.db");
        REQUIRE(holder.has_value());
        auto lock = holder->write();
        REQUIRE(lock.has_value());
        f.actions.run(tui::Intent::RestoreLast, f.confirm);
        CHECK(f.in_trash("a.md"));
        CHECK(f.status.message != core::tr("tui.nothing_to_restore"));
        CHECK_FALSE(f.status.message.empty());
    }
    f.actions.run(tui::Intent::RestoreLast, f.confirm);
    CHECK(f.exists("a.md"));
    CHECK_FALSE(f.in_trash("a.md"));
}
