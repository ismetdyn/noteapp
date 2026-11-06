// T70: klasör gezinmesi. Model saf: liste bellekteki anlık görüntüden gelir; eylemler gerçek depoyla.
#include "core/file_icons.hpp"
#include "core/i18n.hpp"
#include "edit/display_text.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 110;
constexpr int kHeight = 24;

core::NoteInfo note(const std::string& path, int age_hours = 0, std::uintmax_t size = 1) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(1000 - age_hours), size};
}

std::vector<core::NoteInfo> sample() {
    return {note("work/a.md", 0, 10), note("work/sub/b.md", 1, 20), note("c.md", 2, 30),
            note("archive/old.md", 3, 40)};
}

// Satırın görünen kimliği: klasör `/` ile.
std::vector<std::string> labels(const tui::TuiState& s) {
    std::vector<std::string> out;
    for (const tui::Row& row : s.rows())
        out.push_back(row.shown_text());
    return out;
}

using Names = std::vector<std::string>;

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

bool shows(const tui::TuiState& state, std::string_view text, int width = kWidth) {
    const auto screen = draw(state, width);
    for (int y = 0; y < screen.dimy(); ++y)  // ToString vurgu kaçışı taşır: satır satır, düz metin
        if (row_of(screen, y).find(text) != std::string::npos)
            return true;
    return false;
}

tui::TuiState nav_state() {
    core::init_i18n("en");
    tui::TuiState s;
    s.set_window(kWidth, kHeight);
    s.set_notes(sample());
    return s;
}

void type(tui::TuiState& s, std::string_view text) {
    for (const char c : text)
        s.handle(Key::Char, std::string_view(&c, 1));
}

void select_label(tui::TuiState& s, const std::string& label) {
    const Names names = labels(s);
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == label) {
            s.select_index(i);
            return;
        }
    FAIL("no row " << label);
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
    int opened = 0;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::NewRowState> row;
    std::optional<tui::RenameState> rename;
    std::vector<std::pair<std::string, std::string>> moves;  // açık sekmelere giden taşımalar (T3)
    tui::ListActions actions{tui::ListHost{
        store, mutex, internal, system, state, preview, status, [this] { load(); }, [this] { ++changes; },
        nullptr, [this](const std::string& from, const std::string& to) { moves.emplace_back(from, to); }}};

    StoreFixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
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
    bool has_folder(const std::string& name) const {
        const auto folders = store->folders();
        return std::ranges::find(folders, name) != folders.end();
    }
    void press(std::string_view letter) { actions.run(state.handle(Key::Char, letter), confirm); }
    void add_row(std::string_view text) {
        row.emplace(state.name_input_sources());
        for (const char c : text)
            row->handle(Key::Char, std::string_view(&c, 1));
        actions.submit_new_row(row, {"", "md", core::kDefaultSlugMaxLength},
                               [this](const core::NotePath&) { ++opened; });
    }
    void begin_rename(std::string_view text) {
        REQUIRE(state.handle(Key::F2) == Intent::RenameSelected);
        actions.begin_rename(rename);
        REQUIRE(rename);
        actions.handle_rename_key(rename, Key::End, {});  // imleç uzantıdan önce başlar
        while (!rename->text().empty())
            actions.handle_rename_key(rename, Key::Backspace, {});
        for (const char c : text)
            actions.handle_rename_key(rename, Key::Char, std::string_view(&c, 1));
    }
    void submit_rename() { actions.handle_rename_key(rename, Key::Enter, {}); }
    void confirm_yes() {
        REQUIRE(confirm);
        CHECK_FALSE(confirm->yes_selected());
        confirm->handle(Key::Right);
        REQUIRE(confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
        actions.finish_delete(confirm);
    }
};

struct MouseFixture {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    tui::TuiState state;
    tui::EditorSession editor{
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};

    MouseFixture() {
        core::init_i18n("en");
        state.set_window(kWidth, kHeight);
        state.set_notes(sample());
        state.set_clock([this] { return now; });
    }
    void click(int x, int y) {
        tui::route_mouse({x, y, tui::MouseButton::Left, tui::MouseMotion::Press}, state, editor);
    }
};

constexpr int kFirstRowY = 2;
constexpr int kNameX = 12;

}  // namespace

TEST_CASE("folder tree: counts cover the subtree, children only the direct level") {
    const auto notes = sample();
    std::vector<core::NoteMeta> metas;
    for (const auto& n : notes)
        metas.push_back({.path = n.path, .size = n.size, .updated_at = n.mtime});
    const std::vector<std::string> folders = {"empty", "work/sub/deep"};
    noteapp::core::FolderTree tree;
    tree.rebuild(metas, folders);

    const tui::FolderNode* root = tree.find("");
    REQUIRE(root);
    CHECK(root->note_count == 4);
    CHECK(root->folder_count == 5);  // archive, empty, work, work/sub, work/sub/deep
    CHECK(root->item_count() == 4);  // archive, empty, work, c.md
    const tui::FolderNode* work = tree.find("work");
    REQUIRE(work);
    CHECK(work->item_count() == 2);
    CHECK(work->note_count == 2);
    CHECK(work->size == 30);
    CHECK(work->updated_at == notes[0].mtime);
    REQUIRE(tree.find("empty"));
    CHECK(tree.find("empty")->note_count == 0);
    CHECK_FALSE(tree.find("empty")->updated_at.has_value());
    CHECK(tree.nearest_existing("work/sub/gone/x") == "work/sub");
    CHECK(tree.nearest_existing("gone") == "");
    CHECK(tree.folders_under("work") == Names{"work", "work/sub", "work/sub/deep"});
    CHECK(tree.notes_under("work").size() == 2);
}

TEST_CASE("folder tree: recently updated notes are the newest five of the subtree") {
    std::vector<core::NoteMeta> metas;
    for (int i = 0; i < 8; ++i)
        metas.push_back({.path = core::NotePath::parse("d/n" + std::to_string(i) + ".md").value(),
                         .updated_at = std::filesystem::file_time_type{} + std::chrono::hours(i)});
    noteapp::core::FolderTree tree;
    tree.rebuild(metas, {});
    const tui::FolderNode* node = tree.find("d");
    REQUIRE(node);
    REQUIRE(node->recent.size() == noteapp::core::FolderTree::kRecentNotes);
    CHECK(node->recent.front() == 7);
    CHECK(node->recent.back() == 3);
}

TEST_CASE("folder view: the root lists direct children, folders before notes") {
    const tui::TuiState s = nav_state();
    CHECK(labels(s) == Names{"work/", "archive/", "c.md"});
    CHECK(s.current_folder().empty());
    CHECK(s.selected_folder_row());
    CHECK_FALSE(s.selected_row());  // not isteyen eylemler klasörde etkisiz
}

TEST_CASE("folder view: enter with right, l and Enter; a note still goes to REVIEW and the editor") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    s.handle(Key::Right);
    CHECK(s.current_folder() == "work");
    CHECK(labels(s) == Names{"sub/", "a.md"});
    type(s, "l");
    CHECK(s.current_folder() == "work/sub");
    CHECK(labels(s) == Names{"b.md"});
    CHECK(s.focus() == Focus::List);
    CHECK(s.handle(Key::Enter) == Intent::EnterEditor);
    s.handle(Key::Right);
    CHECK(s.focus() == Focus::Preview);
    s.handle(Key::Left);
    s.handle(Key::Left);
    CHECK(s.handle(Key::Enter) == Intent::None);  // klasörde Enter girer, editör açmaz
    CHECK(s.current_folder() == "work/sub");
}

TEST_CASE("folder view: left and h go to the parent with the child selected, nothing at the root") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    s.handle(Key::Enter);
    s.handle(Key::Enter);
    REQUIRE(s.current_folder() == "work/sub");
    s.handle(Key::Left);
    CHECK(s.current_folder() == "work");
    CHECK(s.selected_folder_row()->path.string() == "work/sub");
    type(s, "h");
    CHECK(s.current_folder().empty());
    CHECK(s.selected_folder_row()->path.string() == "work");
    const std::size_t before = s.selected();
    s.handle(Key::Left);
    type(s, "h");
    CHECK(s.current_folder().empty());
    CHECK(s.selected() == before);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("folder view: sort applies inside each group and folders stay first") {
    tui::TuiState s = nav_state();
    s.set_notes({note("big/x.md", 5, 900), note("small/x.md", 1, 5), note("n-big.md", 5, 800),
                 note("n-small.md", 1, 2)});
    CHECK(labels(s) == Names{"small/", "big/", "n-small.md", "n-big.md"});  // varsayılan: en yeni
    type(s, "o");
    type(s, "s");
    CHECK(labels(s) == Names{"big/", "small/", "n-big.md", "n-small.md"});
    type(s, "o");
    type(s, "r");
    CHECK(labels(s) == Names{"small/", "big/", "n-small.md", "n-big.md"});
    type(s, "o");
    type(s, "n");
    CHECK(labels(s) == Names{"big/", "small/", "n-big.md", "n-small.md"});
}

TEST_CASE("folder view: numbers restart per folder and the totals line counts the folder") {
    tui::TuiState s = nav_state();
    const auto root = draw(s);
    CHECK(row_of(root, kFirstRowY).find(" 1 ▸ work/") != std::string::npos);
    CHECK(row_of(root, kFirstRowY + 2).find(" 3   c.md") != std::string::npos);
    CHECK(root.ToString().find("Folder: 3 items · Total 4 notes · page 1/1") != std::string::npos);
    select_label(s, "work/");
    s.handle(Key::Enter);
    const auto inner = draw(s);
    CHECK(row_of(inner, kFirstRowY).find(" 1 ▸ sub/") != std::string::npos);
    CHECK(row_of(inner, kFirstRowY + 1).find(" 2   a.md") != std::string::npos);
    CHECK(inner.ToString().find("Folder: 2 items · Total 4 notes") != std::string::npos);
}

TEST_CASE("folder view: the top line shows the breadcrumb, also shortened for a deep path") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    s.handle(Key::Enter);
    s.handle(Key::Enter);
    const ftxui::Screen crumbs = draw(s);
    CHECK(row_of(crumbs, 0).find("notes/work/sub") == 0);
    // Klasör adları kalın, ayırıcı soluk: yol bir bakışta okunur.
    CHECK(crumbs.PixelAt(0, 0).bold);
    CHECK(crumbs.PixelAt(6, 0).bold);
    CHECK_FALSE(crumbs.PixelAt(6, 0).dim);
    CHECK(crumbs.PixelAt(5, 0).character == "/");
    CHECK(crumbs.PixelAt(5, 0).dim);
    CHECK_FALSE(crumbs.PixelAt(5, 0).bold);

    tui::TuiState deep;
    deep.set_window(60, kHeight);
    std::string path = "seg0";
    for (int i = 1; i < 40; ++i)
        path += "/seg" + std::to_string(i);
    deep.set_notes({note(path + "/leaf.md")});
    deep.select_note(core::NotePath::parse(path + "/leaf.md").value());
    CHECK(deep.current_folder() == path);
    const std::string top = row_of(draw(deep, 60), 0);
    CHECK(top.find("…/") == 0);
    CHECK(top.find("seg39") != std::string::npos);
    for (int i = 0; i < 40; ++i)
        deep.handle(Key::Left);
    CHECK(deep.current_folder().empty());
    CHECK(deep.selected_folder_row()->path.string() == "seg0");
}

TEST_CASE("folder view: the right column shows the item count and Alt+→ opens folder details") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    CHECK(row_of(draw(s), kFirstRowY + 1).find("2 items") != std::string::npos);
    s.handle(Key::AltRight);
    CHECK(s.row_height(s.selected()) == 1 + tui::TuiState::kFolderMetaLines);
    const auto screen = draw(s);
    CHECK(screen.ToString().find("path: work") != std::string::npos);
    CHECK(screen.ToString().find("2 items · 2 notes · 1 folder") != std::string::npos);
    CHECK(screen.ToString().find("size: 30 B") != std::string::npos);
    CHECK(screen.ToString().find("updated: ") != std::string::npos);
    s.handle(Key::AltLeft);
    CHECK(s.row_height(s.selected()) == 1);
}

TEST_CASE("folder view: REVIEW on a folder summarizes it from the tree") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    const std::string text = draw(s).ToString();
    CHECK(text.find("total size: 30 B") != std::string::npos);
    CHECK(text.find("Recently updated") != std::string::npos);
    CHECK(text.find("sub/b.md") != std::string::npos);
}

TEST_CASE("folder view: icons and the marker column stay aligned") {
    tui::TuiState s = nav_state();
    s.set_icon_style(core::IconStyle::Nerd);
    const auto screen = draw(s);
    const int marker = tui::row_marker_column(s);
    CHECK(screen.PixelAt(marker, kFirstRowY).character == "▸");
    const std::string icon = core::icon_text(core::folder_icon(false), core::IconStyle::Nerd);
    CHECK(screen.PixelAt(marker + 2, kFirstRowY).character == icon);
    CHECK(screen.PixelAt(marker + 5, kFirstRowY).character == "w");

    // 12 satırla numara iki haneye çıkar, sütun onunla kayar.
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < 12; ++i)
        notes.push_back(note("d" + std::to_string(i) + "/n.md"));
    s.set_notes(notes);
    CHECK(tui::row_marker_column(s) == marker + 1);
    CHECK(draw(s).PixelAt(marker + 1, kFirstRowY).character == "▸");
}

TEST_CASE("folder view: the hint bar names folder creation at 60 columns and rename from 70") {
    tui::TuiState s = nav_state();
    tui::TuiState narrow = nav_state();
    narrow.set_window(60, kHeight);
    const std::string bar = row_of(draw(narrow, 60), kHeight - 1);
    CHECK(bar.find("n add, name/ = folder") != std::string::npos);
    CHECK(bar.find("← up") != std::string::npos);
    CHECK(bar.ends_with("F1 help"));  // yardım sağa sabit: `F2` 60 sütunda ona yer bırakır
    tui::TuiState wider = nav_state();
    wider.set_window(70, kHeight);
    CHECK(row_of(draw(wider, 70), kHeight - 1).find("F2 rename") != std::string::npos);
    select_label(s, "c.md");
    CHECK(row_of(draw(s), kHeight - 1).find("n add") != std::string::npos);
}

TEST_CASE("search: slash and f stay flat with full paths across folders, Esc returns to the same spot") {
    tui::TuiState s = nav_state();
    select_label(s, "work/");
    s.handle(Key::Enter);
    select_label(s, "a.md");
    type(s, "/");
    CHECK(labels(s) == Names{"sub/", "a.md"});  // yazılana dek klasör görünümü
    type(s, "md");
    CHECK(s.rows().size() == 4);  // kökten, tüm klasörler
    CHECK(s.rows().front().label.empty());
    CHECK(shows(s, "work/sub/b.md"));  // tam yol
    s.handle(Key::Escape);
    s.handle(Key::Escape);
    CHECK(s.current_folder() == "work");
    CHECK(s.selected_row()->path.string() == "work/a.md");

    bool searched = false;
    s.set_global_searcher(
        [&](std::string_view, std::span<const core::NoteMeta> metas, const search::GraphInput&) {
            searched = true;
            std::vector<search::GlobalHit> hits;
            for (const core::NoteMeta& meta : metas)
                hits.push_back(search::GlobalHit{0, meta.path});
            return hits;
        });
    type(s, "f");
    type(s, "z");
    CHECK(searched);
    CHECK(s.rows().size() == 4);
    s.handle(Key::Escape);
    s.handle(Key::Escape);
    CHECK(s.current_folder() == "work");
    CHECK(s.selected_row()->path.string() == "work/a.md");
}

TEST_CASE("search: the in argument works from any folder and left goes to the result's folder") {
    tui::TuiState s = nav_state();
    type(s, "/");
    type(s, "in:archive/");
    REQUIRE(s.rows().size() == 1);
    s.handle(Key::Enter);
    REQUIRE(s.focus() == Focus::List);
    s.handle(Key::Left);
    CHECK(s.query().empty());
    CHECK(s.current_folder() == "archive");
    CHECK(s.selected_row()->path.string() == "archive/old.md");
}

TEST_CASE("trash view stays flat with full paths") {
    tui::TuiState s = nav_state();
    s.enter_trash({core::TrashItem{core::NotePath::parse("deep/er/gone.md").value(), {}, 1},
                   core::TrashItem{core::NotePath::parse("x.md").value(), {}, 1}});
    CHECK(s.rows().size() == 2);
    CHECK(s.selected_row()->path.string() == "deep/er/gone.md");
    CHECK(shows(s, "deep/er/gone.md"));
    s.leave_trash();
    CHECK(labels(s) == Names{"work/", "archive/", "c.md"});
}

TEST_CASE("hostile names are sanitized in the row, breadcrumb, details and summary") {
    core::init_i18n("en");
    const std::string hostile = "gpj\xE2\x80\xAE";
    tui::TuiState s;
    s.set_window(kWidth, kHeight);
    s.set_notes({note(hostile + "/" + hostile + "/n.md")});
    REQUIRE(s.selected_folder_row());
    type(s, "i");
    CHECK(draw(s).ToString().find("\xE2\x80\xAE") == std::string::npos);
    s.handle(Key::Enter);
    type(s, "i");
    const std::string inner = draw(s).ToString();
    CHECK(inner.find("\xE2\x80\xAE") == std::string::npos);
    CHECK(row_of(draw(s), 0).find("notes/") != std::string::npos);
}

TEST_CASE("mouse: click selects, double click enters, a breadcrumb segment jumps") {
    MouseFixture f;
    f.click(kNameX, kFirstRowY + 1);
    CHECK(f.state.selected_folder_row()->path.string() == "archive");
    f.now += std::chrono::seconds(1);
    f.click(kNameX, kFirstRowY);
    CHECK(f.state.selected_folder_row()->path.string() == "work");
    CHECK(f.state.current_folder().empty());
    f.now += std::chrono::seconds(1);
    f.click(kNameX, kFirstRowY);
    CHECK(f.state.current_folder().empty());  // aralık uzun: yine seçim
    f.now += std::chrono::milliseconds(100);
    f.click(kNameX, kFirstRowY);
    CHECK(f.state.current_folder() == "work");
    f.now += std::chrono::seconds(1);
    f.click(kNameX, kFirstRowY);
    f.now += std::chrono::milliseconds(100);
    f.click(kNameX, kFirstRowY);
    REQUIRE(f.state.current_folder() == "work/sub");

    const std::string top = "notes/work/sub";
    REQUIRE(row_of(draw(f.state), 0).find(top) == 0);
    f.click(static_cast<int>(top.find('/')), 0);  // ayırıcı atlamaz
    CHECK(f.state.current_folder() == "work/sub");
    f.click(static_cast<int>(top.find("work")), 0);
    CHECK(f.state.current_folder() == "work");
    CHECK(f.state.selected_folder_row()->path.string() == "work/sub");
    f.click(0, 0);
    CHECK(f.state.current_folder().empty());
    CHECK(f.state.selected_folder_row()->path.string() == "work");
}

TEST_CASE("a: a name ending in a slash creates a folder here, stays in the list and selects it") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    f.state.handle(Key::Enter);
    REQUIRE(f.state.current_folder() == "work");
    f.add_row("new/");
    CHECK_FALSE(f.row);
    CHECK(f.opened == 0);
    CHECK(f.has_folder("work/new"));
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.current_folder() == "work");
    CHECK(f.state.selected_folder_row()->path.string() == "work/new");
    CHECK(f.changes == 1);
}

TEST_CASE("a: a nested name is relative to the current folder and creates what it needs") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    f.state.handle(Key::Enter);
    f.add_row("sub/x.cpp");
    CHECK(f.opened == 1);
    CHECK(f.exists("work/sub/x.cpp"));  // uzantı kuralı değişmez
    CHECK(f.state.current_folder() == "work/sub");
    CHECK(f.state.selected_row()->path.string() == "work/sub/x.cpp");

    f.add_row("plain");
    CHECK(f.exists("work/sub/plain.md"));  // uzantı yok: default_type
}

TEST_CASE("a: at the root the name goes to the root; a bad folder name keeps the row open") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    f.add_row("top.md");
    CHECK(f.exists("top.md"));
    f.add_row(".hidden/");
    REQUIRE(f.row);
    CHECK_FALSE(f.row->error().empty());
}

TEST_CASE("F2 on a folder renames it and keeps it selected; r does the same") {
    StoreFixture f;
    f.put("work/sub/a.md");
    f.put("work/b.md");
    f.load();
    f.state.handle(Key::Enter);
    REQUIRE(f.state.selected_folder_row()->path.string() == "work/sub");
    f.begin_rename("renamed");
    f.submit_rename();
    CHECK_FALSE(f.rename);
    CHECK(f.exists("work/renamed/a.md"));
    CHECK_FALSE(f.has_folder("work/sub"));
    CHECK(f.state.selected_folder_row()->path.string() == "work/renamed");
    CHECK(f.state.handle(Key::CtrlR) == Intent::RenameSelected);
    CHECK(f.state.handle(Key::Char, "r") == Intent::RenameSelected);
}

TEST_CASE("F2 on a folder prefills its name relative to the current folder, dots are not extensions") {
    StoreFixture f;
    f.put("work/v1.2/a.md");
    f.load();
    f.state.handle(Key::Enter);
    f.actions.begin_rename(f.rename);
    REQUIRE(f.rename);
    CHECK(f.rename->text() == "v1.2");
    CHECK(f.rename->cursor() == 4);
}

TEST_CASE("F2 on a note is relative to the current folder and a path with a slash moves it") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    f.state.handle(Key::Enter);
    f.actions.begin_rename(f.rename);
    REQUIRE(f.rename);
    CHECK(f.rename->text() == "a.md");
    f.rename.reset();
    f.begin_rename("sub/z.md");
    f.submit_rename();
    CHECK(f.exists("work/sub/z.md"));
    CHECK(f.state.current_folder() == "work/sub");
}

// T3: açık sekmeler taşımayı izler; çağrı listeden önce, yalnız başarılı taşımada.
TEST_CASE("F2 tells the open tabs about a renamed folder or note, not about a refused one") {
    StoreFixture f;
    f.put("work/sub/a.md");
    f.put("work/b.md");
    f.load();
    f.state.handle(Key::Enter);
    REQUIRE(f.state.selected_folder_row()->path.string() == "work/sub");
    f.begin_rename("renamed");
    f.submit_rename();
    REQUIRE(f.moves.size() == 1);
    CHECK(f.moves[0] == std::pair<std::string, std::string>{"work/sub", "work/renamed"});
    REQUIRE(f.state.select_note(core::NotePath::parse("work/b.md").value()));
    f.begin_rename("c.md");
    f.submit_rename();
    REQUIRE(f.moves.size() == 2);
    CHECK(f.moves[1] == std::pair<std::string, std::string>{"work/b.md", "work/c.md"});
    REQUIRE(f.state.select_folder("work/renamed"));
    f.put("work/taken/x.md");
    f.load();
    REQUIRE(f.state.select_folder("work/renamed"));
    f.begin_rename("taken");
    f.submit_rename();
    CHECK(f.moves.size() == 2);
}

TEST_CASE("F2 refused by the store keeps the input open with the error") {
    StoreFixture f;
    f.put("a/x.md");
    f.put("b/y.md");
    f.load();
    REQUIRE(f.state.select_folder("a"));
    f.begin_rename("b");
    f.submit_rename();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK(f.has_folder("a"));
}

// Yeniden tarama seçimi kaydırabilir: F2 başladığı notu taşır, o sıradaki seçili satırı değil.
TEST_CASE("F2 renames the note it started on even if a rescan shifts the selection") {
    StoreFixture f;
    f.put("b.md");
    f.put("c.md");
    f.load();
    select_label(f.state, "c.md");
    f.begin_rename("renamed.md");
    f.put("a.md");
    f.load();
    f.submit_rename();
    CHECK(f.exists("renamed.md"));
    CHECK_FALSE(f.exists("c.md"));
    CHECK(f.exists("a.md"));
    CHECK(f.exists("b.md"));
}

TEST_CASE("F2 on a note deleted meanwhile keeps the input open with the error") {
    StoreFixture f;
    f.put("b.md");
    f.put("c.md");
    f.load();
    select_label(f.state, "c.md");
    f.begin_rename("renamed.md");
    test::erase_note(f.dir.path(), "c.md");
    f.load();
    f.submit_rename();
    REQUIRE(f.rename);
    CHECK_FALSE(f.rename->error().empty());
    CHECK_FALSE(f.exists("renamed.md"));
    CHECK(f.exists("b.md"));
}

// Ctrl-R diyaloğu hedefi açılışta sabitler (main.cpp start_move); silinmiş hedef başka notu taşımaz.
TEST_CASE("the move dialog target deleted meanwhile returns an error and moves nothing") {
    StoreFixture f;
    f.put("b.md");
    f.put("c.md");
    const core::NotePath from = *core::NotePath::parse("c.md");
    test::erase_note(f.dir.path(), "c.md");
    CHECK_FALSE(tui::move_note(*f.store, from, {"", "renamed", "md"}).has_value());
    CHECK_FALSE(f.exists("renamed.md"));
    CHECK(f.exists("b.md"));
}

TEST_CASE("d on an empty folder removes it without asking and u brings it back") {
    StoreFixture f;
    REQUIRE(f.store->create_folder("empty"));
    f.load();
    REQUIRE(f.state.selected_folder_row());
    f.press("d");
    CHECK_FALSE(f.confirm);
    CHECK_FALSE(f.has_folder("empty"));
    f.press("u");
    CHECK(f.has_folder("empty"));
    CHECK(f.state.selected_folder_row()->path.string() == "empty");
}

TEST_CASE("d on a non-empty folder asks with counts, Cancel is the default and writes nothing") {
    StoreFixture f;
    f.put("work/a.md");
    f.put("work/sub/b.md");
    f.load();
    f.press("d");
    REQUIRE(f.confirm);
    CHECK_FALSE(f.confirm->yes_selected());
    CHECK(f.confirm->message().find("2 notes") != std::string::npos);
    CHECK(f.confirm->message().find("work") != std::string::npos);
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    CHECK(f.exists("work/a.md"));
    CHECK(f.changes == 0);
}

TEST_CASE("d confirmed trashes every note, removes the folder, u restores the whole step") {
    StoreFixture f;
    f.put("work/a.md", "A");
    f.put("work/sub/b.md", "B");
    f.put("keep.md");
    REQUIRE(f.store->create_folder("work/empty"));
    f.load();
    f.press("d");
    f.confirm_yes();
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.in_trash("work/sub/b.md"));
    CHECK_FALSE(f.exists("work/a.md"));
    CHECK_FALSE(f.has_folder("work"));
    CHECK(f.exists("keep.md"));
    CHECK(labels(f.state) == Names{"keep.md"});

    f.press("u");
    CHECK(test::note_body(f.dir.path(), "work/a.md") == "A");
    CHECK(test::note_body(f.dir.path(), "work/sub/b.md") == "B");
    CHECK(f.has_folder("work/empty"));
    CHECK(f.state.selected_folder_row()->path.string() == "work");
    f.press("u");
    CHECK(f.status.message.find("Nothing") != std::string::npos);
}

TEST_CASE("D on a folder is permanent, asks with counts and defaults to Cancel") {
    StoreFixture f;
    f.put("work/a.md");
    f.put("work/sub/b.md");
    f.load();
    f.press("D");
    REQUIRE(f.confirm);
    CHECK(f.confirm->message().find("2 notes") != std::string::npos);
    CHECK(f.confirm->message().find("1 subfolder") != std::string::npos);
    f.confirm_yes();
    CHECK_FALSE(f.exists("work/a.md"));
    CHECK_FALSE(f.in_trash("work/a.md"));
    CHECK_FALSE(f.has_folder("work"));
}

TEST_CASE("c on a folder copies its path and E does nothing there") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    CHECK(f.state.handle(Key::Char, "E") == Intent::None);
    f.press("c");
    CHECK(f.internal == "work");
}

TEST_CASE("the store's new empty folder appears, a vanished current folder falls back to its parent") {
    StoreFixture f;
    f.put("work/sub/a.md");
    f.load();
    f.state.handle(Key::Enter);
    f.state.handle(Key::Enter);
    REQUIRE(f.state.current_folder() == "work/sub");
    REQUIRE(f.store->remove_folder("work/sub", true));
    REQUIRE(f.store->create_folder("work/fresh"));
    f.load();
    CHECK(f.state.current_folder() == "work");
    CHECK(labels(f.state) == Names{"fresh/"});
}

// Depo somut sınıftır; PreviewCache tek okuma yoludur ve her okumada üretim sayacını artırır.
TEST_CASE("moving over folder rows reads no note from a real store, a note row does") {
    StoreFixture f;
    f.put("work/a.md");
    f.put("archive/b.md");
    f.put("c.md");
    f.load();
    auto now = std::chrono::steady_clock::now();
    const auto settle = [&] {
        now += std::chrono::seconds(1);
        f.preview.throttled(f.state, *f.store, now);
    };
    settle();
    const std::size_t before = f.preview.generation();
    select_label(f.state, "work/");
    settle();
    select_label(f.state, "archive/");
    settle();
    f.state.handle(Key::Enter);
    f.state.handle(Key::Left);
    REQUIRE(f.state.selected_folder_row());
    settle();
    CHECK(f.preview.generation() == before);
    select_label(f.state, "c.md");
    settle();
    CHECK(f.preview.generation() > before);  // sayaç gerçekten okumayı sayıyor
}

TEST_CASE("d on a folder that fails before any note is trashed leaves no undo step") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    REQUIRE(f.store->remove_folder("work", true));  // yeniden tarama görmedi
    f.press("d");
    f.confirm_yes();
    CHECK_FALSE(f.status.message.empty());
    f.press("u");
    CHECK(f.status.message.find("Nothing") != std::string::npos);
    CHECK_FALSE(f.has_folder("work"));
}

TEST_CASE("d partial failure on a folder trashes the rest, keeps the folder, u brings them back") {
    StoreFixture f;
    f.put("work/a.md", "A");
    f.put("work/b.md", "B");
    f.put("work/c.md", "C");
    f.load();
    REQUIRE(f.store->remove(core::NotePath::parse("work/b.md").value()));  // enjekte hata
    f.press("d");
    f.confirm_yes();
    CHECK(f.in_trash("work/a.md"));
    CHECK(f.in_trash("work/c.md"));
    CHECK(f.has_folder("work"));
    CHECK(f.status.message.find("work/b.md") != std::string::npos);
    f.press("u");
    CHECK(test::note_body(f.dir.path(), "work/a.md") == "A");
    CHECK(test::note_body(f.dir.path(), "work/c.md") == "C");
}

TEST_CASE("u partial failure on a folder restores the rest and reports the missing note") {
    StoreFixture f;
    f.put("work/a.md", "A");
    f.put("work/b.md", "B");
    f.load();
    f.press("d");
    f.confirm_yes();
    REQUIRE(f.store->purge(core::NotePath::parse("work/a.md").value()));
    f.press("u");
    CHECK(test::note_body(f.dir.path(), "work/b.md") == "B");
    CHECK(f.state.selected_folder_row()->path.string() == "work");
    REQUIRE_FALSE(f.state.summary().empty());
    CHECK(f.state.summary().back().find("work/a.md") != std::string::npos);
}

TEST_CASE("mouse: rows below an open row map through its detail lines") {
    MouseFixture f;
    const int details = static_cast<int>(tui::TuiState::kFolderMetaLines);
    f.state.handle(Key::AltRight);
    REQUIRE(f.state.row_height(0) == 1 + tui::TuiState::kFolderMetaLines);
    f.click(kNameX, kFirstRowY + 1 + details);
    CHECK(f.state.selected_folder_row()->path.string() == "archive");
    f.now += std::chrono::seconds(1);
    f.click(kNameX, kFirstRowY + 2);  // ayrıntı satırı açık satıra aittir
    CHECK(f.state.selected_folder_row()->path.string() == "work");
    f.now += std::chrono::seconds(1);
    f.click(kNameX, kFirstRowY + 2 + details);
    CHECK(f.state.selected_row()->path.string() == "c.md");
}

TEST_CASE("names that leave the notes tree or are absolute are refused inline, nothing is written") {
    StoreFixture f;
    f.put("work/a.md");
    f.load();
    for (const std::string_view name :
         {"../kacis.md", "/mutlak.md", "a/../../b.md", "../kacis/", "/mutlak/"}) {
        CAPTURE(name);
        f.add_row(name);
        REQUIRE(f.row);
        CHECK_FALSE(f.row->error().empty());
        f.row.reset();
    }
    REQUIRE(f.state.select_folder("work"));
    for (const std::string_view name : {"../kacis", "/mutlak"}) {
        CAPTURE(name);
        f.begin_rename(name);
        f.submit_rename();
        REQUIRE(f.rename);
        CHECK_FALSE(f.rename->error().empty());
        f.rename.reset();
    }
    CHECK(f.has_folder("work"));
    CHECK(f.store->list().size() == 1);
    CHECK(f.changes == 0);
}

TEST_CASE("an overlong new note name is cut to the slug limit, an overlong folder is refused inline") {
    StoreFixture f;
    f.load();
    const std::string overlong(5000, 'x');
    f.add_row(overlong + ".md");
    CHECK_FALSE(f.row);
    REQUIRE(f.store->list().size() == 1);
    CHECK(f.store->list().front().path.string().size() <= core::kDefaultSlugMaxLength + 3);
    f.add_row(overlong + "/");
    REQUIRE(f.row);
    CHECK_FALSE(f.row->error().empty());
    CHECK_FALSE(f.has_folder(overlong));
}

TEST_CASE("a: an existing folder name says it exists and selects it") {
    StoreFixture f;
    f.put("work/a.md");
    f.put("z.md");
    f.load();
    select_label(f.state, "z.md");
    f.add_row("work/");
    CHECK_FALSE(f.row);
    CHECK(f.state.selected_folder_row()->path.string() == "work");
    CHECK(f.status.message.find("already exists") != std::string::npos);
    CHECK(f.changes == 0);
}

TEST_CASE("tail_cells cuts by terminal cells, wide characters do not overflow") {
    const std::string wide = "日本語日本語日本語";  // 18 hücre
    CHECK(ftxui::string_width(tui::tail_cells(wide, 7)) <= 7);
    CHECK(tui::tail_cells(wide, 7).starts_with("…"));
    CHECK(tui::tail_cells(wide, 7).ends_with("日本語"));
    CHECK(tui::tail_cells(wide, 18) == wide);
    CHECK(tui::tail_cells("abc", 3) == "abc");
    CHECK(tui::tail_cells("abcd", 3) == "…cd");
    CHECK(tui::tail_cells("abcd", 1) == "…");
    const std::string emoji = "\xF0\x9F\x93\x9D\xF0\x9F\x93\x9D\xF0\x9F\x93\x9D";  // 📝 x 3
    CHECK(ftxui::string_width(tui::tail_cells(emoji, 4)) <= 4);
}
