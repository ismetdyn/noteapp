// T74: yerinde açılan klasör ağacı, `t` ile taşıma, satır görünümü, Nerd Font ikonları.
#include "core/file_icons.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
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

namespace {

constexpr int kWidth = 110;
constexpr int kHeight = 24;
constexpr int kFirstRowY = 2;

core::NoteInfo note(const std::string& path, int age_hours = 0, std::uintmax_t size = 1) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(1000 - age_hours), size};
}

tui::ListPrefs by_name() {
    tui::ListPrefs prefs;
    prefs.sort = {search::SortField::Name, false};
    return prefs;
}

tui::TuiState tree_state() {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_window(kWidth, kHeight);
    s.set_list_prefs(by_name());
    s.set_notes({note("work/a.md"), note("work/sub/deep/d.md"), note("work/sub/b.md"), note("c.md"),
                 note("archive/old.md")});
    return s;
}

std::string row_of(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

ftxui::Screen draw(const tui::TuiState& state, int width = kWidth, int height = kHeight) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, {}, {"", ""}, {width, height}));
    return screen;
}

std::vector<std::string> labels(const tui::TuiState& s) {
    std::vector<std::string> out;
    for (const tui::Row& row : s.rows())
        out.push_back(row.shown_text());
    return out;
}

using Names = std::vector<std::string>;

void select_label(tui::TuiState& s, const std::string& label) {
    const Names names = labels(s);
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == label) {
            s.select_index(i);
            return;
        }
    FAIL("no row " << label);
}

void open_folder(tui::TuiState& s, const std::string& label) {
    select_label(s, label);
    REQUIRE(s.handle(Key::Char, " ") == Intent::SavePrefs);
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
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { load(); }, [this] { ++changes; }}};

    Fixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
        state.set_list_prefs(by_name());
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
    void begin_move(std::string_view folder) {
        REQUIRE(state.handle(Key::Char, "m") == Intent::MoveTo);
        actions.begin_move_to(rename);
        REQUIRE(rename);
        while (!rename->text().empty())
            actions.handle_rename_key(rename, Key::Backspace, {});
        for (const char c : folder)
            actions.handle_rename_key(rename, Key::Char, std::string_view(&c, 1));
    }
    void submit() { actions.handle_rename_key(rename, Key::Enter, {}); }
};

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("tree: Space opens a folder in place at any depth and closes it again") {
    tui::TuiState s = tree_state();
    CHECK(labels(s) == Names{"archive/", "work/", "c.md"});
    open_folder(s, "work/");
    CHECK(labels(s) == Names{"archive/", "work/", "sub/", "a.md", "c.md"});
    open_folder(s, "sub/");
    CHECK(labels(s) == Names{"archive/", "work/", "sub/", "deep/", "b.md", "a.md", "c.md"});
    open_folder(s, "deep/");
    CHECK(s.rows()[3].depth == 2);
    CHECK(s.rows()[4].depth == 3);
    CHECK(s.rows()[4].shown_text() == "d.md");
    CHECK(s.current_folder().empty());
    open_folder(s, "work/");
    CHECK(labels(s) == Names{"archive/", "work/", "c.md"});
    CHECK(s.selected_folder_row()->path.string() == "work");
    open_folder(s, "work/");  // alt açıklık kalır
    CHECK(labels(s)[4] == "d.md");
    CHECK(labels(s).size() == 8);
}

TEST_CASE("tree: numbers restart per level and children are indented") {
    tui::TuiState s = tree_state();
    open_folder(s, "work/");
    const auto screen = draw(s);
    CHECK(has(row_of(screen, kFirstRowY), " 1 ▸ archive/"));
    CHECK(has(row_of(screen, kFirstRowY + 1), " 2 ▾ work/"));
    CHECK(has(row_of(screen, kFirstRowY + 2), "   1 ▸ sub/"));
    CHECK(has(row_of(screen, kFirstRowY + 3), "   2   a.md"));
    CHECK(has(row_of(screen, kFirstRowY + 4), " 3   c.md"));
    const int top = tui::row_marker_column(s, 1);
    CHECK(tui::row_marker_column(s, 2) == top + 2);
    CHECK(screen.PixelAt(top, kFirstRowY + 1).character == "▾");
    CHECK(screen.PixelAt(top + 2, kFirstRowY + 2).character == "▸");
}

TEST_CASE("tree: enter and leave keep what is open; left always goes to the parent") {
    tui::TuiState s = tree_state();
    open_folder(s, "work/");
    select_label(s, "sub/");
    s.handle(Key::Right);
    CHECK(s.current_folder() == "work/sub");
    CHECK(labels(s) == Names{"deep/", "b.md"});
    s.handle(Key::Left);
    CHECK(s.current_folder() == "work");
    s.handle(Key::Left);
    CHECK(s.current_folder().empty());
    CHECK(labels(s) == Names{"archive/", "work/", "sub/", "a.md", "c.md"});
    select_label(s, "a.md");
    s.handle(Key::Left);  // kökte etkisiz, açık satırda da kapatmaz
    CHECK(s.current_folder().empty());
    CHECK(labels(s).size() == 5);
}

TEST_CASE("tree: the open set round-trips through tui-state.toml, also names that need quoting") {
    tui::ListPrefs prefs = by_name();
    prefs.open_folders = {"work", "a \"b\"/c", "ayerinde1234567890123456789abcd"};
    const tui::ListPrefs back = tui::parse_list_prefs(tui::format_list_prefs(prefs));
    CHECK(back.open_folders == prefs.open_folders);
    CHECK(tui::parse_list_prefs("version = 2\n").open_folders.empty());

    tui::TuiState s = tree_state();
    open_folder(s, "work/");
    CHECK(s.list_prefs().open_folders.contains("work"));
    tui::TuiState restored;
    restored.set_list_prefs(s.list_prefs());
    restored.set_notes({note("work/a.md")});
    CHECK(labels(restored) == Names{"work/", "a.md"});
}

TEST_CASE("tree: Space on a note still toggles details, i opens folder details, right column stays") {
    tui::TuiState s = tree_state();
    select_label(s, "c.md");
    CHECK(s.handle(Key::Char, " ") == Intent::None);
    CHECK(s.row_height(s.selected()) == 1 + tui::TuiState::kMetaLines);
    s.handle(Key::Char, " ");
    CHECK(s.row_height(s.selected()) == 1);
    select_label(s, "work/");
    s.handle(Key::AltRight);
    CHECK(s.row_height(s.selected()) == 1 + tui::TuiState::kFolderMetaLines);
    CHECK(labels(s).size() == 3);  // ağaç kapalı kalır
    s.handle(Key::AltLeft);
    CHECK(s.row_height(s.selected()) == 1);
}

TEST_CASE("tree: paging, the totals and mouse rows work on tree rows") {
    tui::TuiState s;
    core::init_i18n("en");
    s.set_window(kWidth, kHeight);
    s.set_list_prefs(by_name());
    std::vector<core::NoteInfo> notes;
    for (int i = 100; i < 160; ++i)
        notes.push_back(note("d/n" + std::to_string(i) + ".md"));
    s.set_notes(notes);
    open_folder(s, "d/");
    CHECK(s.rows().size() == 61);
    CHECK(has(row_of(draw(s), kHeight - 2), "Folder: 61 items"));
    s.handle(Key::PageDown);
    CHECK(s.selected() == s.page_rows());
    s.handle(Key::Char, "G");
    CHECK(s.selected() == 60);
    CHECK(s.rows()[60].number == 60);
    CHECK(has(row_of(draw(s), kFirstRowY + static_cast<int>(s.page_rows()) - 1), "60"));
}

TEST_CASE("tree: x and d work on tree rows; a marked child of a marked folder counts once") {
    Fixture f;
    f.put("work/a.md");
    f.put("work/b.md");
    f.put("c.md");
    f.load();
    open_folder(f.state, "work/");
    REQUIRE(labels(f.state) == Names{"work/", "a.md", "b.md", "c.md"});
    select_label(f.state, "a.md");
    f.press("v");
    CHECK(f.state.selected() == 2);
    f.press("d");
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.exists("work/b.md"));
    CHECK(labels(f.state) == Names{"work/", "b.md", "c.md"});

    f.state.handle(Key::CtrlA);
    CHECK(f.state.marked_count() == 3);
    CHECK(f.state.action_items().size() == 2);  // klasör ve c.md
}

TEST_CASE("tree: y and F2 on a nested row use its real path") {
    Fixture f;
    f.put("work/sub/a.md", "A");
    f.put("dest/x.md");
    f.load();
    open_folder(f.state, "work/");
    open_folder(f.state, "sub/");
    select_label(f.state, "a.md");
    f.press("y");
    REQUIRE(f.state.yank());
    CHECK(f.state.yank()->items.front().path.string() == "work/sub/a.md");
    select_label(f.state, "dest/");
    f.state.handle(Key::Enter);
    f.press("p");
    CHECK(f.body("dest/a.md") == "A");
    CHECK(f.body("work/sub/a.md") == "A");
    f.state.handle(Key::Left);
    select_label(f.state, "a.md");
    f.actions.begin_rename(f.rename);
    REQUIRE(f.rename);
    CHECK(f.rename->text() == "work/sub/a.md");
}

// Süre değil iş sayılır: duvar saati yük altında yanlış kırmızı, gevşekse yanlış yeşil verir.
TEST_CASE("tree: a move rebuilds nothing and a frame walks only what is visible, at 20000 rows") {
    tui::TuiState s;
    core::init_i18n("en");
    s.set_window(kWidth, kHeight);
    s.set_list_prefs(by_name());
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < 20000; ++i)
        notes.push_back(note("big/n" + std::to_string(i) + ".md"));
    s.set_notes(notes);
    const std::size_t rebuilds = s.list_work().rebuilds;
    open_folder(s, "big/");
    REQUIRE(s.rows().size() == 20001);
    CHECK(s.list_work().rebuilds == rebuilds + 1);

    const std::size_t per_frame = 3 * s.page_rows();
    const auto frame_within_budget = [&] {
        const std::size_t visits = s.list_work().row_visits;
        (void)draw(s);
        const std::size_t walked = s.list_work().row_visits - visits;
        CHECK(walked > 0);  // sayaç gerçekten çiziminde artıyor
        CHECK(walked <= per_frame);
    };
    frame_within_budget();
    for (int i = 0; i < 50; ++i) {
        s.handle(Key::Down);
        frame_within_budget();
    }
    s.handle(Key::Char, "G");
    frame_within_budget();
    CHECK(s.list_work().rebuilds == rebuilds + 1);
}

// Ağaç henüz kurulmadıysa (ilk tarama, açılamayan depo) kayıt açık klasörleri silmez.
TEST_CASE("tree: saving before the tree is built keeps the open folders") {
    Fixture f;
    tui::ListPrefs prefs = by_name();
    prefs.open_folders = {"work"};
    f.state.set_list_prefs(prefs);
    f.press("V");  // tercih yazımı
    CHECK(f.state.list_prefs().open_folders.contains("work"));
    std::ifstream in(core::state_dir() / "tui-state.toml");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(tui::parse_list_prefs(text).open_folders.contains("work"));
}

// Okuma tarafı sır süzgeci (app::UiPrefsRules::keep_history): elle yazılmış sır geçmişe girmez.
TEST_CASE("prefs: a secret-like search in tui-state.toml never reaches the history") {
    Fixture f;
    std::filesystem::create_directories(core::state_dir());
    std::ofstream(core::state_dir() / "tui-state.toml") << "search_history = [\"API_KEY=abc123\", \"plan\"]\n";
    tui::load_list_prefs(f.state);
    CHECK(f.state.list_prefs().search_history == std::vector<std::string>{"plan"});
}

// Elle bozulmuş dosya ezilmeden kenara alınır (tek yedek): kullanıcının yazdığı kaybolmaz.
TEST_CASE("prefs: a broken tui-state.toml is set aside before defaults are used") {
    Fixture f;
    std::filesystem::create_directories(core::state_dir());
    const auto file = core::state_dir() / "tui-state.toml";
    std::ofstream(file) << "sort = [\n";
    const std::string notice = tui::load_list_prefs(f.state);
    CHECK(notice.find("tui-state.toml.bad") != std::string::npos);  // kullanıcıya bir kez söylenir
    std::ifstream kept(core::state_dir() / "tui-state.toml.bad");
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "sort = [\n");
    CHECK_FALSE(std::filesystem::exists(file));
}

// Onay penceresi açıkken senkron klasöre not getirirse Evet yalnız onayda sayılan notları siler.
TEST_CASE("delete: a note that arrives while the folder delete is asked survives the Yes") {
    Fixture f;
    f.put("work/a.md");
    f.put("z.md");
    f.load();
    f.state.select_folder("work");
    f.press("D");
    REQUIRE(f.confirm.has_value());
    f.put("work/new.md");  // senkron, onay açıkken
    f.load();
    f.confirm->handle(Key::Right);
    REQUIRE(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.exists("work/a.md"));
    CHECK(f.exists("work/new.md"));
}

// Onay açıkken senkron işaretlileri silip işaretleri düşürse de Evet seçili satıra kaymaz; aynı yola gelen başka
// not silinmez (hedef ve revizyon onay anında alınır).
TEST_CASE("delete: Yes acts on the targets the confirmation counted, never on the live selection") {
    Fixture f;
    f.put("a.md");
    f.put("b.md");
    f.put("work/c.md");
    f.load();
    f.state.select_note(*core::NotePath::parse("a.md"));
    f.press("vv");  // a, b işaretli
    REQUIRE(f.state.marked_count() == 2);
    f.press("D");
    REQUIRE(f.confirm.has_value());
    REQUIRE(f.store->remove(*core::NotePath::parse("a.md")));  // senkron, onay açıkken
    REQUIRE(f.store->remove(*core::NotePath::parse("b.md")));
    f.put("b.md", "another note at the same path");
    f.load();
    f.state.select_folder("work");  // canlı seçim artık klasör
    f.confirm->handle(Key::Right);
    REQUIRE(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
    f.actions.finish_delete(f.confirm);
    CHECK(f.exists("b.md"));
    CHECK(f.exists("work/c.md"));
    f.state.handle(Key::Escape);  // atlanan öğelerin özet penceresi

    REQUIRE(f.state.select_note(*core::NotePath::parse("b.md")));
    f.press("D");
    REQUIRE(f.confirm.has_value());
    f.put("b.md", "changed by sync");
    f.load();
    f.confirm->handle(Key::Right);
    REQUIRE(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
    f.actions.finish_delete(f.confirm);
    CHECK(f.exists("b.md"));
}

TEST_CASE("tree: folders that no longer exist leave open_folders when saving") {
    Fixture f;
    f.put("work/a.md");
    f.put("gone/b.md");
    f.load();
    open_folder(f.state, "work/");
    open_folder(f.state, "gone/");
    REQUIRE(f.state.list_prefs().open_folders.size() == 2);
    REQUIRE(f.store->remove_folder("gone", true));
    f.load();
    CHECK(f.state.list_prefs().open_folders.contains("gone"));  // tarama silmez: sync geri getirebilir
    f.press("V");                                               // tercih yazımı
    CHECK_FALSE(f.state.list_prefs().open_folders.contains("gone"));
    CHECK(f.state.list_prefs().open_folders.contains("work"));
}

TEST_CASE("m into itself creates no folder, also for a new nested destination") {
    Fixture f;
    f.put("work/a.md", "A");
    f.load();
    select_label(f.state, "work/");
    f.begin_move("work/yeni/ic");
    f.submit();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    const auto folders = f.store->folders();
    CHECK(std::ranges::find(folders, "work/yeni") == folders.end());
    CHECK(std::ranges::find(folders, "work/yeni/ic") == folders.end());
    CHECK(f.changes == 0);
}

TEST_CASE("counts read properly: singular and plural, English and Turkish") {
    tui::TuiState s;
    core::init_i18n("en");
    s.set_window(kWidth, kHeight);
    s.set_notes({note("work/a.md")});
    s.handle(Key::AltRight);
    std::string shown = draw(s).ToString();
    CHECK(has(shown, "1 item"));
    CHECK_FALSE(has(shown, "1 items"));
    CHECK(has(shown, "1 item · 1 note · 0 folders"));
    CHECK(has(shown, "Total 1 note"));
    CHECK_FALSE(has(shown, "1 notes"));
    core::init_i18n("tr");
    shown = draw(s).ToString();
    CHECK(has(shown, "1 öğe · 1 not · 0 klasör"));
    CHECK(has(shown, "Toplam 1 not"));
    core::init_i18n("en");
}

TEST_CASE("the top line counts notes in the folder view, matches over the total only in a filter") {
    tui::TuiState s = tree_state();
    REQUIRE(s.rows().size() == 3);
    std::string top = row_of(draw(s), 0);
    CHECK(has(top, "[name] · 5 notes"));
    CHECK_FALSE(has(top, "3/5"));
    open_folder(s, "work/");
    CHECK(has(row_of(draw(s), 0), "[name] · 5 notes"));  // açık ağaç sayıyı değiştirmez
    s.handle(Key::Char, "/");
    s.handle(Key::Char, "o");
    s.handle(Key::Char, "l");
    s.handle(Key::Char, "d");
    top = row_of(draw(s), 0);
    CHECK(has(top, "[name] · 1/5"));
}

TEST_CASE("row look: no selection marker, the highlight is the row, notes have no arrow") {
    tui::TuiState s = tree_state();
    select_label(s, "c.md");
    const auto screen = draw(s);
    const std::string row = row_of(screen, kFirstRowY + 2);
    CHECK(row.find("│  3   c.md") == 0);
    CHECK(screen.PixelAt(tui::kPaneContentX, kFirstRowY + 2).inverted);
    CHECK(screen.PixelAt(tui::row_marker_column(s, 2), kFirstRowY + 2).character == " ");  // notta ok yok
    CHECK_FALSE(has(screen.ToString(), ">3"));
    CHECK_FALSE(screen.PixelAt(tui::kPaneContentX, kFirstRowY).inverted);
    CHECK(ftxui::string_width("▸") == 1);
    CHECK(ftxui::string_width("▾") == 1);
}

TEST_CASE("row look: with the right pane focused the selection keeps a dim highlight, not inverted") {
    tui::TuiState s = tree_state();
    select_label(s, "c.md");
    s.handle(Key::Right);
    REQUIRE(s.focus() == Focus::Preview);
    const auto screen = draw(s);
    CHECK_FALSE(screen.PixelAt(tui::kPaneContentX, kFirstRowY + 2).inverted);
    CHECK(screen.PixelAt(tui::kPaneContentX, kFirstRowY + 2).background_color ==
          ftxui::Color(ftxui::Color::GrayDark));
}

TEST_CASE("row look: note details are an indented block with no marker, also at depth") {
    tui::TuiState s = tree_state();
    open_folder(s, "work/");
    select_label(s, "a.md");
    s.handle(Key::Char, " ");
    const auto screen = draw(s);
    const int y = kFirstRowY + static_cast<int>(s.selected()) + 1;
    CHECK(row_of(screen, y).find("│       size:") == 0);
    CHECK(screen.PixelAt(tui::row_marker_column(s, s.selected()), y - 1).character == " ");
}

TEST_CASE("row look: columns stay aligned at 60 and 110 columns") {
    for (const int width : {60, 110}) {
        CAPTURE(width);
        tui::TuiState s = tree_state();
        s.set_window(width, kHeight);
        open_folder(s, "work/");
        const auto screen = draw(s, width);
        const int arrow = tui::row_marker_column(s, 0);
        CHECK(screen.PixelAt(arrow, kFirstRowY).character == "▸");
        CHECK(screen.PixelAt(arrow + 2, kFirstRowY).character == "a");  // ad, okun iki hücre sağında
        CHECK(screen.PixelAt(arrow + 2, kFirstRowY + 4).character == "c");
    }
}

TEST_CASE("mouse: a folder's arrow toggles and saves, a note row only selects, the nested row maps") {
    core::init_i18n("en");
    const test::TempDir dir;
    core::NoteStore store(dir.path());
    std::mutex mutex;
    std::string clipboard;
    tui::TuiState s = tree_state();
    tui::EditorSession editor(
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, 1024, core::kDefaultSlugMaxLength,
                        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {},
                        [] {}, [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}});
    const auto click = [&](int x, int y) {
        return tui::route_mouse({x, y, tui::MouseButton::Left, tui::MouseMotion::Press}, s, editor);
    };
    const int arrow = tui::row_marker_column(s, 1);
    CHECK(click(arrow, kFirstRowY + 1) == tui::MouseResult::SavePrefs);
    CHECK(labels(s) == Names{"archive/", "work/", "sub/", "a.md", "c.md"});
    CHECK(s.selected_folder_row()->path.string() == "work");
    // İç içe ok iki hücre sağda.
    CHECK(click(arrow, kFirstRowY + 2) == tui::MouseResult::None);
    CHECK(labels(s).size() == 5);
    CHECK(s.selected_folder_row()->path.string() == "work/sub");
    CHECK(click(arrow + 2, kFirstRowY + 2) == tui::MouseResult::SavePrefs);
    CHECK(labels(s).size() == 7);
    // Notta ok yok: tıklama yalnız seçer, ayrıntı açmaz.
    const std::size_t note_row = 6;
    REQUIRE(labels(s)[note_row] == "c.md");
    CHECK(click(tui::row_marker_column(s, note_row), kFirstRowY + static_cast<int>(note_row)) ==
          tui::MouseResult::None);
    CHECK(s.selected() == note_row);
    CHECK(s.row_height(note_row) == 1);
}

TEST_CASE("m moves a note into a typed folder, creating nested ones") {
    Fixture f;
    f.put("a.md", "A");
    f.load();
    f.begin_move("yeni/ic/");
    f.submit();
    CHECK_FALSE(f.rename);
    CHECK(f.body("yeni/ic/a.md") == "A");
    CHECK(f.state.current_folder() == "yeni/ic");
    CHECK(f.state.selected_row()->path.string() == "yeni/ic/a.md");
    CHECK(f.changes == 1);
}

TEST_CASE("m starts from the current folder and offers folders with Tab, also empty ones") {
    Fixture f;
    f.put("work/a.md");
    REQUIRE(f.store->create_folder("empty-one"));
    f.load();
    f.state.handle(Key::Enter);
    REQUIRE(f.state.current_folder() == "work");
    REQUIRE(f.state.handle(Key::Char, "m") == Intent::MoveTo);
    f.actions.begin_move_to(f.rename);
    REQUIRE(f.rename);
    CHECK(f.rename->text() == "work/");
    while (!f.rename->text().empty())
        f.actions.handle_rename_key(f.rename, Key::Backspace, {});
    f.actions.handle_rename_key(f.rename, Key::Char, "e");
    f.actions.handle_rename_key(f.rename, Key::Tab, {});
    CHECK(f.rename->text() == "empty-one/");
    f.submit();
    CHECK(f.exists("empty-one/a.md"));
}

TEST_CASE("m popup lists folders only and filters fuzzily") {
    Fixture f;
    f.put("work/a.md");
    f.put("wiki/plan.md");
    f.load();
    REQUIRE(f.state.handle(Key::Char, "m") == Intent::MoveTo);
    f.actions.begin_move_to(f.rename);
    REQUIRE(f.rename);
    f.actions.handle_rename_key(f.rename, Key::Char, "w");
    f.actions.handle_rename_key(f.rename, Key::Tab, {});
    REQUIRE(f.rename->line().popup());
    const std::vector<std::string>& items = f.rename->line().popup()->items;
    CHECK(std::ranges::find(items, "work/") != items.end());
    CHECK(std::ranges::find(items, "wiki/") != items.end());
    CHECK(std::ranges::find(items, "wiki/plan.md") == items.end());
}

TEST_CASE("m moves a folder through rename_folder, creating the parent, keeping the notes") {
    Fixture f;
    f.put("work/sub/a.md", "A");
    f.load();
    f.begin_move("yeni/ev/");
    f.submit();
    CHECK_FALSE(f.rename);
    CHECK(f.body("yeni/ev/work/sub/a.md") == "A");
    CHECK(f.state.selected_folder_row()->path.string() == "yeni/ev/work");
}

TEST_CASE("m on a selection moves all of it and consumes the marks") {
    Fixture f;
    f.put("a.md", "A");
    f.put("b.md", "B");
    f.put("work/c.md", "C");
    f.load();
    select_label(f.state, "a.md");
    f.press("vv");
    f.begin_move("work");
    f.submit();
    CHECK(f.body("work/a.md") == "A");
    CHECK(f.body("work/b.md") == "B");
    CHECK(f.state.marked_count() == 0);
    CHECK(f.changes == 1);
}

TEST_CASE("m collision: inline error, input stays, nothing is moved or overwritten") {
    Fixture f;
    f.put("a.md", "new");
    f.put("b.md", "B");
    f.put("dest/a.md", "old");
    f.load();
    select_label(f.state, "a.md");
    f.press("vv");
    f.begin_move("dest");
    f.submit();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK(f.body("dest/a.md") == "old");
    CHECK(f.body("a.md") == "new");
    CHECK(f.exists("b.md"));
    CHECK(f.changes == 0);
    CHECK(f.state.marked_count() == 2);
    f.actions.handle_rename_key(f.rename, Key::Escape, {});
    CHECK_FALSE(f.rename);
}

TEST_CASE("m into itself and bad paths stay inline; the same folder changes nothing") {
    Fixture f;
    f.put("work/sub/a.md", "A");
    f.load();
    f.begin_move("work/sub");
    f.submit();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK(f.body("work/sub/a.md") == "A");
    f.rename.reset();
    f.begin_move("../disari");
    f.submit();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    f.rename.reset();
    f.begin_move("");
    f.submit();
    CHECK_FALSE(f.rename);
    CHECK(f.changes == 0);
}

TEST_CASE("m shows its own label and the hints name it") {
    Fixture f;
    f.put("a.md");
    f.load();
    f.begin_move("x");
    auto shown = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(shown, tui::render(f.state, {}, {"", ""}, {kWidth, kHeight}, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr, &*f.rename));
    CHECK(has(row_of(shown, kHeight - 1), "Move to:"));
    CHECK(has(core::tr("tui.key_hints_list"), "m move"));
    CHECK(has(core::tr("tui.key_hints_folder"), "m move"));
    CHECK(has(core::tr("tui.help_body"), "m             move"));
}

TEST_CASE("icons: nerd glyphs with per-type colors, folder open and closed, off shows no column") {
    tui::TuiState s = tree_state();
    s.set_icon_style(core::IconStyle::Nerd);
    open_folder(s, "work/");
    auto screen = draw(s);
    const int arrow = tui::row_marker_column(s, 0);
    const std::string closed = core::icon_text(core::folder_icon(false), core::IconStyle::Nerd);
    const std::string opened = core::icon_text(core::folder_icon(true), core::IconStyle::Nerd);
    CHECK(closed == "\xF3\xB0\x89\x8B");  // U+F024B
    CHECK(opened == "\xF3\xB0\x9D\xB0");  // U+F0770
    CHECK(screen.PixelAt(arrow + 2, kFirstRowY).character == closed);
    CHECK(screen.PixelAt(arrow + 2, kFirstRowY).foreground_color == ftxui::Color(ftxui::Color::Blue));
    CHECK(screen.PixelAt(arrow + 2, kFirstRowY + 1).character == opened);
    CHECK(screen.PixelAt(arrow + 3, kFirstRowY + 1).character == " ");  // glif için boş hücre
    CHECK(screen.PixelAt(arrow + 4, kFirstRowY + 1).character == " ");
    CHECK(screen.PixelAt(arrow + 5, kFirstRowY + 1).character == "w");

    s.set_icon_style(core::IconStyle::Off);
    screen = draw(s);
    CHECK(screen.PixelAt(arrow + 2, kFirstRowY).character == "a");
}

TEST_CASE("icons: the REVIEW frame carries the note's glyph when icons are on") {
    tui::TuiState s = tree_state();
    s.set_icon_style(core::IconStyle::Nerd);
    select_label(s, "c.md");
    s.handle(Key::Right);
    const std::string glyph = core::icon_text(core::file_icon("c.md"), core::IconStyle::Nerd);
    CHECK(has(row_of(draw(s), 1), glyph + "  REVIEW"));
    s.set_icon_style(core::IconStyle::Off);
    CHECK(has(row_of(draw(s), 1), " REVIEW "));
    CHECK_FALSE(has(row_of(draw(s), 1), glyph));
}

TEST_CASE("t opens the trash, m opens the move input and there is no pending move") {
    Fixture f;
    f.put("a.md");
    f.load();
    CHECK(f.state.handle(Key::Char, "t") == Intent::ToggleTrash);
    CHECK(f.state.handle(Key::Char, "m") == Intent::MoveTo);
    CHECK_FALSE(f.state.yank());
    CHECK_FALSE(has(core::tr("tui.key_hints_list"), "t move"));
}
