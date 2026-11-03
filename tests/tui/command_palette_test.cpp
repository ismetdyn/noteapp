// T79: komut paleti ve tek dağıtım tablosu. Model saf; çizim ekran metniyle; ayar yazımı gerçek config'le.
#include "core/config.hpp"
#include "core/file_icons.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Command;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

core::NoteInfo info(const std::string& path) {
    return core::NoteInfo{np(path), std::filesystem::file_time_type{}, 1};
}

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
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

std::string screen_text(const tui::TuiState& state, int width = kWidth, int height = kHeight) {
    const ftxui::Screen screen = draw(state, width, height);
    std::string out;
    for (int y = 0; y < height; ++y)
        out += row_of(screen, y) + "\n";
    return out;
}

void type(tui::TuiState& state, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i)
        state.handle(Key::Char, text.substr(i, 1));
}

// Klasör, iki not; bağlantısız. Seçim ilk notta (a.md).
tui::TuiState sample_state() {
    core::init_i18n("en");
    tui::TuiState state;
    state.set_notes({info("a.md"), info("b.md"), info("work/c.md")});
    state.set_folders({"work"});
    state.set_window(kWidth, kHeight);
    REQUIRE(state.select_note(np("a.md")));
    return state;
}

// Palette imleci bu komuta getirir; görünen satırda olmalı.
void aim(tui::TuiState& state, std::string_view id) {
    for (std::size_t step = 0; step < 200; ++step) {
        const Command* selected = state.palette_selected();
        REQUIRE(selected != nullptr);
        if (selected->id == id)
            return;
        state.handle(Key::Down);
    }
    FAIL("palette has no row: ", id);
}

const Command* palette_row(const tui::TuiState& state, std::string_view id) {
    for (const tui::PaletteRow& row : state.palette_rows())
        if (row.command->id == id)
            return row.command;
    return nullptr;
}

const char* reason_of(const tui::TuiState& state, std::string_view id) {
    for (const tui::PaletteRow& row : state.palette_rows())
        if (row.command->id == id)
            return row.reason;
    return nullptr;
}

// Tablodaki bağı tuş olarak basar: `gg` önek, `o n` menü, `F2` özel tuş.
Intent press(tui::TuiState& state, const Command& command) {
    if (command.key != Key::Char)
        return state.handle(command.key);
    const std::string_view keys = command.keys;
    if (keys == "Space")
        return state.handle(Key::Char, " ");
    if (keys.size() == 3 && keys[1] == ' ') {  // `o n`
        state.handle(Key::Char, keys.substr(0, 1));
        return state.handle(Key::Char, keys.substr(2, 1));
    }
    if (keys.size() == 2 && keys[0] == 'g') {  // `gg`
        state.handle(Key::Char, keys.substr(0, 1));
        return state.handle(Key::Char, keys.substr(1, 1));
    }
    return state.handle(Key::Char, keys);
}

// Gözlenebilir model durumu: tuş ve palet aynı işi yaptıysa aynıdır.
std::string observed(const tui::TuiState& s) {
    std::ostringstream out;
    out << "focus=" << static_cast<int>(s.focus()) << " selected=" << s.selected()
        << " rows=" << s.rows().size() << " folder=" << s.current_folder() << " query=" << s.query()
        << " mode=" << static_cast<int>(s.mode()) << " column=" << static_cast<int>(s.list_column())
        << " sort=" << static_cast<int>(s.list_sort().field) << s.list_sort().descending
        << " marks=" << s.marked_count() << " yank=" << s.yank().has_value() << " trash=" << s.trash_view()
        << " rendered=" << s.review_rendered() << " zoom=" << s.layout().zoomed()
        << " list_width=" << s.layout().list_width(kWidth) << " scroll=" << s.preview_scroll()
        << " open=" << s.list_prefs().open_folders.size() << " expanded=" << s.is_expanded(np("a.md"))
        << " prefix=" << s.awaiting_prefix();
    return out.str();
}

tui::TuiState state_for(unsigned context) {
    tui::TuiState state = sample_state();
    if (context == tui::kInReview) {
        state.set_preview_line_count(100);
        state.set_links(core::NoteLinks{{np("b.md")}, {}, {}, {}}, 1, np("a.md"));
        state.focus_preview();
    } else if (context == tui::kInTrash) {
        state.enter_trash({core::TrashItem{np("t.md"), {}, 1}});
    }
    return state;
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
    core::Config config = core::load_config().config;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{
        tui::ListHost{store, mutex, internal, system, state, preview, status, [] {}, [] {}, &config}};
    StoreFixture() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "a.md", "a\n"));
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    void run_palette(std::string_view id) {
        state.handle(Key::Char, ":");
        aim(state, id);
        actions.run(state.handle(Key::Enter), confirm, rename);
    }
};

}  // namespace

// ---- Açma, kapama, kim yakalar ----

TEST_CASE("palette: colon opens it in List, REVIEW and the trash without changing focus") {
    for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
        CAPTURE(context);
        tui::TuiState state = state_for(context);
        const std::string before = observed(state);
        CHECK(state.handle(Key::Char, ":") == Intent::None);
        CHECK(state.palette_open());
        CHECK(observed(state) == before);
        CHECK(state.handle(Key::Escape) == Intent::None);
        CHECK_FALSE(state.palette_open());
        CHECK(observed(state) == before);
    }
}

TEST_CASE("palette: colon is text in the editor and in every input") {
    tui::TuiState editor = sample_state();
    editor.enter_editor();
    CHECK(editor.handle(Key::Char, ":") == Intent::EditorKey);
    CHECK_FALSE(editor.palette_open());

    tui::TuiState search = sample_state();
    search.handle(Key::Char, "/");
    search.handle(Key::Char, ":");
    CHECK_FALSE(search.palette_open());
    CHECK(search.query() == ":");

    tui::TuiState find = state_for(tui::kInReview);
    find.handle(Key::Char, "/");
    find.handle(Key::Char, ":");
    CHECK_FALSE(find.palette_open());
    CHECK(find.preview_find().query() == ":");

    tui::TuiState sorting = sample_state();
    sorting.handle(Key::Char, "o");
    sorting.handle(Key::Char, ":");
    CHECK_FALSE(sorting.palette_open());
}

TEST_CASE("palette: while open, letters filter instead of acting; F1 F9 close it, Ctrl-C still quits") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    const std::string before = observed(state);
    CHECK(state.handle(Key::Char, "q") == Intent::None);
    CHECK(state.palette_line().text() == "q");
    CHECK(observed(state) == before);
    state.handle(Key::Backspace);
    CHECK(state.palette_line().text().empty());
    CHECK(state.handle(Key::F1) == Intent::ShowHelp);
    CHECK_FALSE(state.palette_open());
    state.handle(Key::Char, ":");
    CHECK(state.handle(Key::F9) == Intent::ShowSettings);
    CHECK_FALSE(state.palette_open());
    state.handle(Key::Char, ":");
    CHECK(state.handle(Key::CtrlC) == Intent::Quit);
}

TEST_CASE("palette: the mouse is ignored while it is open") {
    StoreFixture f;
    f.state.set_window(kWidth, kHeight);
    f.state.handle(Key::Char, ":");
    const std::size_t rows = f.state.palette_rows().size();
    CHECK(rows > 0);
    const Focus focus = f.state.focus();
    tui::EditorSession editor(tui::EditorHost{
        *f.store, f.mutex, f.internal, tui::terminal_cell_width, 1024, core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {}, [] {},
        [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}});
    tui::route_mouse({70, 5, tui::MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.focus() == focus);
    CHECK(f.state.palette_open());
}

// ---- Tablo ----

TEST_CASE("table: ids are English, one label each, keys unique per view") {
    CHECK(tui::TuiState::commands().size() > 40);
    std::set<std::pair<unsigned, std::string>> bound;
    for (const Command& command : tui::TuiState::commands()) {
        CAPTURE(command.id);
        CHECK_FALSE(command.id.empty());
        CHECK(std::ranges::all_of(command.id, [](unsigned char c) { return c < 0x80 && c != ' '; }));
        CHECK(core::tr(command.label_key) != command.label_key);
        CHECK(command.contexts != 0);
        for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
            if ((command.contexts & context) == 0)
                continue;
            if (!command.keys.empty())
                CHECK(bound.emplace(context, std::string(command.keys)).second);
            if (!command.alias.empty())
                CHECK(bound.emplace(context, std::string(command.alias)).second);
        }
    }
}

TEST_CASE("table: the brief's commands are all there") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    for (const std::string_view id : {"new",
                                      "quick.add",
                                      "move",
                                      "rename",
                                      "trash",
                                      "restore",
                                      "delete",
                                      "copy",
                                      "sort.name",
                                      "sort.updated",
                                      "sort.created",
                                      "sort.size",
                                      "sort.reverse",
                                      "column.size_date",
                                      "column.size",
                                      "column.updated",
                                      "column.created",
                                      "column.sync",
                                      "column.origin",
                                      "column.none",
                                      "tree.toggle_all",
                                      "icons.toggle",
                                      "sync.now",
                                      "settings",
                                      "help",
                                      "go.root",
                                      "go.trash",
                                      "view.toggle",
                                      "links.focus"}) {
        CAPTURE(id);
        CHECK(palette_row(state, id) != nullptr);
    }
}

TEST_CASE("palette: each id has one row; it shows the row bound in the current view") {
    for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
        CAPTURE(context);
        tui::TuiState state = state_for(context);
        state.handle(Key::Char, ":");
        std::set<std::string_view> seen;
        for (const tui::PaletteRow& row : state.palette_rows()) {
            CAPTURE(row.command->id);
            CHECK(seen.insert(row.command->id).second);
        }
        std::set<std::string_view> ids;
        for (const Command& command : tui::TuiState::commands())
            ids.insert(command.id);
        CHECK(seen == ids);
    }
    tui::TuiState review = state_for(tui::kInReview);
    review.handle(Key::Char, ":");
    CHECK(palette_row(review, "top")->keys == "g");
    tui::TuiState list = state_for(tui::kInList);
    list.handle(Key::Char, ":");
    CHECK(palette_row(list, "top")->keys == "gg");
}

// Asıl sözleşme: palet ve tuş aynı satırdan geçer; model aynı duruma, aynı Intent'e varır.
TEST_CASE("palette and key do exactly the same in every view for every runnable command") {
    std::size_t compared = 0;
    for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
        for (const bool on_folder : {false, true}) {
            if (on_folder && context != tui::kInList)
                continue;
            for (const Command& command : tui::TuiState::commands()) {
                if ((command.contexts & context) == 0 || command.keys.empty())
                    continue;
                tui::TuiState by_key = state_for(context);
                tui::TuiState by_palette = state_for(context);
                if (on_folder) {
                    REQUIRE(by_key.select_folder("work"));
                    REQUIRE(by_palette.select_folder("work"));
                }
                by_palette.handle(Key::Char, ":");
                const char* block = reason_of(by_palette, command.id);
                if (block)
                    continue;
                aim(by_palette, command.id);
                CAPTURE(command.id);
                CAPTURE(context);
                CAPTURE(on_folder);
                const Intent from_palette = by_palette.handle(Key::Enter);
                const Intent from_key = press(by_key, command);
                CHECK(from_palette == from_key);
                CHECK_FALSE(by_palette.palette_open());
                CHECK(observed(by_palette) == observed(by_key));
                ++compared;
            }
        }
    }
    CHECK(compared > 60);
}

TEST_CASE("keys keep working through the table: letters with no meaning in a view do nothing") {
    tui::TuiState list = state_for(tui::kInList);
    const std::string before = observed(list);
    for (const std::string_view unbound : {"b", "R", "N", "J", "K", "ı", "Z"}) {
        CAPTURE(unbound);
        CHECK(list.handle(Key::Char, unbound) == Intent::None);
        CHECK(observed(list) == before);
    }
    tui::TuiState review = state_for(tui::kInReview);
    const std::string review_before = observed(review);
    for (const std::string_view unbound : {"a", "d", "r", "s", "?", "x", "T"}) {
        CAPTURE(unbound);
        CHECK(review.handle(Key::Char, unbound) == Intent::None);
        CHECK(observed(review) == review_before);
    }
    CHECK(review.handle(Key::CtrlO) == Intent::None);
}

// ---- Süzme, sıra ----

TEST_CASE("palette: fuzzy over the id and the translated label") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    type(state, "mov");
    CHECK(state.palette_selected()->id == "move");
    state.handle(Key::Escape);

    core::init_i18n("tr");
    state.handle(Key::Char, ":");
    type(state, "taşı");
    CHECK(state.palette_selected()->id == "move");
    state.handle(Key::Escape);
    core::init_i18n("en");

    state.handle(Key::Char, ":");
    type(state, "zzzqqq");
    CHECK(state.palette_rows().empty());
    CHECK(state.palette_selected() == nullptr);
    CHECK(state.handle(Key::Enter) == Intent::None);
    CHECK(state.palette_open());
}

TEST_CASE("palette: arrows stop at the ends and editing the filter returns to the top") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    const std::vector<tui::PaletteRow> rows = state.palette_rows();
    CHECK(state.palette_selected() == rows.front().command);
    state.handle(Key::Up);
    CHECK(state.palette_selected() == rows.front().command);
    state.handle(Key::Down);
    CHECK(state.palette_selected() == rows[1].command);
    for (std::size_t i = 0; i < rows.size() + 3; ++i)
        state.handle(Key::Down);
    CHECK(state.palette_selected() == rows.back().command);
    type(state, "m");
    CHECK(state.palette_selected() == state.palette_rows().front().command);
}

TEST_CASE("palette: recently used first, across reopen and through the prefs file") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    CHECK(state.palette_rows().front().command->id != "column.sync");
    aim(state, "column.sync");
    state.handle(Key::Enter);
    CHECK(state.take_prefs_changed());
    CHECK_FALSE(state.take_prefs_changed());
    state.handle(Key::Char, ":");
    aim(state, "column.size");
    state.handle(Key::Enter);

    state.handle(Key::Char, ":");
    std::vector<tui::PaletteRow> rows = state.palette_rows();
    CHECK(rows[0].command->id == "column.size");
    CHECK(rows[1].command->id == "column.sync");
    state.handle(Key::Escape);

    const tui::ListPrefs loaded = tui::parse_list_prefs(tui::format_list_prefs(state.list_prefs()));
    CHECK(loaded.recent_commands == std::vector<std::string>{"column.size", "column.sync"});

    tui::TuiState restarted = sample_state();
    tui::ListPrefs prefs;
    prefs.recent_commands = {"gone.command", "column.origin"};  // eski sürümün adı palette yer tutmaz
    restarted.set_list_prefs(prefs);
    restarted.handle(Key::Char, ":");
    CHECK(restarted.palette_rows().front().command->id == "column.origin");
}

TEST_CASE("palette: the recent list is capped, also when the file was edited by hand") {
    tui::TuiState state = sample_state();
    for (const Command& command : tui::TuiState::commands()) {
        if (!(command.contexts & tui::kInList))
            continue;
        state.handle(Key::Char, ":");
        if (state.command_block(command)) {
            state.handle(Key::Escape);
            continue;
        }
        aim(state, command.id);
        state.handle(Key::Enter);
        if (state.palette_open())
            state.handle(Key::Escape);
        if (state.focus() != Focus::List)
            state.handle(Key::Escape);
    }
    CHECK(state.list_prefs().recent_commands.size() <= tui::TuiState::kMaxRecentCommands);

    std::string text = "recent_commands = [";
    for (int i = 0; i < 500; ++i)
        text += "\"x" + std::to_string(i) + "\", ";
    text += "\"last\"]\n";
    CHECK(tui::parse_list_prefs(text).recent_commands.size() == tui::TuiState::kMaxRecentCommands);
}

// ---- Çalışamayan ----

TEST_CASE("palette: unavailable commands say why and do not run") {
    tui::TuiState list = state_for(tui::kInList);
    list.handle(Key::Char, ":");
    const std::pair<std::string_view, std::string_view> expected[] = {
        {"links.focus", "tui.cmd_why_review"},
        {"view.toggle", "tui.cmd_why_review"},
        {"restore", ""},
        {"paste", "tui.paste_nothing"},
        {"sync.now", "tui.sync_disabled_hint"},
        {"trash.empty", "tui.cmd_why_trash"},
        {"move", ""},
    };
    for (const auto& [id, reason] : expected) {
        CAPTURE(id);
        const char* actual = reason_of(list, id);
        CHECK(std::string_view(actual ? actual : "") == reason);
    }
    aim(list, "links.focus");
    const std::string before = observed(list);
    CHECK(list.handle(Key::Enter) == Intent::None);
    CHECK(list.palette_open());
    CHECK(observed(list) == before);
    list.handle(Key::Escape);

    list.set_sync_enabled(true);
    list.handle(Key::Char, "y");
    list.handle(Key::Char, ":");
    CHECK(reason_of(list, "paste") == nullptr);
    CHECK(reason_of(list, "sync.now") == nullptr);
    list.handle(Key::Escape);

    tui::TuiState empty;
    core::init_i18n("en");
    empty.handle(Key::Char, ":");
    CHECK(std::string_view(reason_of(empty, "move")) == "tui.cmd_why_row");
    CHECK(std::string_view(reason_of(empty, "external")) == "tui.cmd_why_note");
    CHECK(reason_of(empty, "new") == nullptr);
}

TEST_CASE("palette: a folder row blocks note commands, links need a section, trash blocks list commands") {
    tui::TuiState folder = sample_state();
    REQUIRE(folder.select_folder("work"));
    folder.handle(Key::Char, ":");
    CHECK(std::string_view(reason_of(folder, "external")) == "tui.cmd_why_note");
    CHECK(reason_of(folder, "move") == nullptr);
    CHECK(reason_of(folder, "tree.toggle_all") == nullptr);

    tui::TuiState plain = state_for(tui::kInReview);
    plain.set_links(core::NoteLinks{}, 2, np("a.md"));
    plain.handle(Key::Char, ":");
    CHECK(std::string_view(reason_of(plain, "links.focus")) == "tui.cmd_why_links");

    tui::TuiState trash = state_for(tui::kInTrash);
    trash.handle(Key::Char, ":");
    CHECK(std::string_view(reason_of(trash, "new")) == "tui.cmd_why_list");
    CHECK(reason_of(trash, "trash.empty") == nullptr);
    CHECK(reason_of(trash, "restore") == nullptr);

    tui::TuiState filtered = sample_state();
    filtered.handle(Key::Char, "/");
    type(filtered, "a");
    filtered.handle(Key::Enter);
    filtered.handle(Key::Char, ":");
    CHECK(std::string_view(reason_of(filtered, "tree.toggle_all")) == "tui.cmd_why_folder_view");
}

// ---- Tuşsuz komutlar ----

TEST_CASE("column variants set the column and ask to save") {
    tui::TuiState state = sample_state();
    const std::pair<std::string_view, tui::ListColumn> variants[] = {
        {"column.size", tui::ListColumn::Size},
        {"column.updated", tui::ListColumn::Updated},
        {"column.created", tui::ListColumn::Created},
        {"column.sync", tui::ListColumn::Sync},
        {"column.origin", tui::ListColumn::Origin},
        {"column.none", tui::ListColumn::None},
        {"column.size_date", tui::ListColumn::SizeAndDate},
    };
    for (const auto& [id, column] : variants) {
        CAPTURE(id);
        state.handle(Key::Char, ":");
        aim(state, id);
        CHECK(state.handle(Key::Enter) == Intent::SavePrefs);
        CHECK(state.list_column() == column);
    }
}

TEST_CASE("tree.toggle_all opens every folder, then closes them") {
    tui::TuiState state = sample_state();
    state.set_notes({info("a.md"), info("work/sub/d.md"), info("x/y.md")});
    state.set_folders({"work", "work/sub", "x"});
    state.handle(Key::Char, ":");
    aim(state, "tree.toggle_all");
    CHECK(state.handle(Key::Enter) == Intent::SavePrefs);
    CHECK(state.is_folder_open("work"));
    CHECK(state.is_folder_open("work/sub"));
    CHECK(state.is_folder_open("x"));
    CHECK(state.rows().size() == 6);  // work, sub, d.md, x, y.md, a.md

    state.handle(Key::Char, ":");
    aim(state, "tree.toggle_all");
    CHECK(state.handle(Key::Enter) == Intent::SavePrefs);
    CHECK_FALSE(state.is_folder_open("work"));
    CHECK(state.rows().size() == 3);
}

TEST_CASE("icons.toggle flips the setting, writes config.toml and takes effect at once") {
    StoreFixture f;
    f.config.icons = "nerd";
    f.state.set_icon_style(core::IconStyle::Nerd);
    f.run_palette("icons.toggle");
    CHECK(f.config.icons == "off");
    CHECK(f.state.icon_style() == core::IconStyle::Off);
    CHECK(core::load_config().config.icons == "off");
    f.run_palette("icons.toggle");
    CHECK(f.config.icons == "nerd");
    CHECK(f.state.icon_style() == core::IconStyle::Nerd);
    CHECK(core::load_config().config.icons == "nerd");
}

TEST_CASE("icons.toggle refuses to overwrite a config.toml it cannot read") {
    StoreFixture f;
    f.config.icons = "nerd";
    f.state.set_icon_style(core::IconStyle::Nerd);
    REQUIRE(core::save_config(f.config));
    {
        std::ofstream broken(core::config_file(), std::ios::trunc);
        broken << "= not toml [\n";
    }
    f.run_palette("icons.toggle");
    CHECK(f.config.icons == "nerd");
    CHECK(f.state.icon_style() == core::IconStyle::Nerd);
    CHECK(has(f.status.message, "config.toml"));
    std::ifstream kept(core::config_file());
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "= not toml [\n");
}

TEST_CASE("recent commands reach tui-state.toml through the host") {
    StoreFixture f;
    f.run_palette("column.sync");
    f.actions.save_prefs_if_changed();
    std::ifstream in(core::state_dir() / "tui-state.toml");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(has(text, "recent_commands = [\"column.sync\"]"));
}

// ---- Çizim ----

TEST_CASE("render: a row is id, label, key at the right; unavailable rows are dim with the reason") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    type(state, "move");
    const std::string shown = screen_text(state);
    CHECK(has(shown, "move — Move"));

    const ftxui::Screen screen = draw(state);
    bool found = false;
    for (int y = 0; y < kHeight; ++y) {
        const std::string row = row_of(screen, y);
        if (!has(row, "move — Move"))
            continue;
        found = true;
        CHECK(has(row, " m │"));  // tuş sağ kenarda
    }
    CHECK(found);

    state.handle(Key::Escape);
    state.handle(Key::Char, ":");
    type(state, "links.focus");
    const ftxui::Screen dim_screen = draw(state);
    bool found_dim = false;
    for (int y = 0; y < kHeight; ++y) {
        const std::string row = row_of(dim_screen, y);
        const std::size_t at = row.find("links.focus — ");
        if (at == std::string::npos)
            continue;
        found_dim = true;
        CHECK(has(row, core::tr("tui.cmd_why_review")));
        // Bayt ofseti değil hücre: satırın ilk harfini ara.
        for (int x = 0; x < kWidth; ++x)
            if (dim_screen.PixelAt(x, y).character == "—") {
                CHECK(dim_screen.PixelAt(x, y).dim);
                break;
            }
    }
    CHECK(found_dim);
}

TEST_CASE("render: fits 60x15 with the key still visible and the selected reason on the hint line") {
    for (const char* language : {"en", "tr"}) {
        tui::TuiState state = sample_state();
        core::init_i18n(language);
        state.set_window(60, 15);
        state.handle(Key::Char, ":");
        type(state, "links.focus");
        const std::string shown = screen_text(state, 60, 15);
        CHECK(has(shown, "links.focus"));
        CHECK(has(shown, " b │"));
        CHECK(has(shown, core::tr("tui.cmd_why_review")));
    }
    core::init_i18n("en");
}

TEST_CASE("render: the selected row stays visible while walking a list longer than the box") {
    tui::TuiState state = sample_state();
    state.set_window(60, 15);
    state.handle(Key::Char, ":");
    const std::size_t count = state.palette_rows().size();
    for (std::size_t i = 0; i + 1 < count; ++i) {
        state.handle(Key::Down);
        CAPTURE(state.palette_selected()->id);
        CHECK(has(screen_text(state, 60, 15), std::string(state.palette_selected()->id) + " — "));
    }
}

TEST_CASE("render: hostile note names and filter text never reach the terminal raw") {
    // NotePath ESC ve C0 baytını reddeder; bidi ve C1 yoldan geçer, senkronla da gelebilir.
    const std::string evil = "x\xE2\x80\xAE"
                             "gpj\xC2\x9B";
    tui::TuiState state = sample_state();
    state.set_notes({info(evil + ".md")});
    state.handle(Key::Char, ":");
    state.handle(Key::Char, "\xE2\x80\xAE");
    state.handle(Key::Backspace);
    const std::string title = screen_text(state);
    CHECK_FALSE(has(title, "\x1B"));
    CHECK_FALSE(has(title, "\xE2\x80\xAE"));
    CHECK_FALSE(has(title, "\xC2\x9B"));
    CHECK(has(title, "x\xEF\xBF\xBD"));  // hedef başlıkta arındırılmış görünür

    state.handle(Key::Char, "\xE2\x80\xAE");
    const std::string filter = screen_text(state);
    CHECK_FALSE(has(filter, "\xE2\x80\xAE"));
}

TEST_CASE("render: the box names the target, or the count when rows are marked") {
    tui::TuiState state = sample_state();
    state.handle(Key::Char, ":");
    CHECK(has(screen_text(state), "a.md"));
    state.handle(Key::Escape);
    state.handle(Key::Char, "x");
    state.handle(Key::Char, ":");
    CHECK(has(screen_text(state), core::tr("tui.marked_count", {{"count", "1"}})));
}

namespace {

// Tablo anahtarının tek tek belirteçleri: `o n` iki tuştur.
std::vector<std::string> key_tokens(const Command& command) {
    std::vector<std::string> tokens;
    std::istringstream words{std::string(command.keys)};
    for (std::string word; words >> word;)
        tokens.push_back(word);
    if (!command.alias.empty())
        tokens.emplace_back(command.alias);
    return tokens;
}

}  // namespace

// Kullanıcının istediği denetim: tablodaki her tuş hem palet ekranında hem F1 ekranında görünür.
TEST_CASE("every key in the dispatch table appears in the palette and in F1, in both languages") {
    for (const char* language : {"en", "tr"}) {
        CAPTURE(language);
        core::init_i18n(language);

        // F1: gerçek çizimden, kaydırarak.
        std::set<std::string> help_words;
        tui::TuiState base;
        const std::vector<std::string> lines = tui::help_lines();
        tui::HelpState help(lines.size());
        for (std::size_t page = 0; page <= lines.size(); ++page) {
            auto screen =
                ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
            ftxui::Render(screen,
                          tui::render(base, {}, {"", ""}, {kWidth, kHeight}, nullptr, nullptr, &help));
            for (int y = 0; y < kHeight; ++y) {
                std::string row = row_of(screen, y);
                for (std::size_t at = row.find("│"); at != std::string::npos; at = row.find("│"))
                    row.replace(at, std::string_view("│").size(), " ");
                std::ranges::replace(row, '(', ' ');
                std::ranges::replace(row, ')', ' ');
                std::istringstream words(row);
                for (std::string word; words >> word;)
                    help_words.insert(word);
            }
            help.handle(Key::Down);
        }
        CHECK(help_words.contains(std::string(tui::TuiState::kPaletteKey)));

        for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
            tui::TuiState state = state_for(context);
            core::init_i18n(language);
            state.handle(Key::Char, ":");
            const std::vector<tui::PaletteRow> rows = state.palette_rows();
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Command& command = *state.palette_selected();
                CAPTURE(command.id);
                const std::string shown = screen_text(state);
                CHECK(has(shown, std::string(command.id) + " — "));
                // Alias'lı satır `gt T` gibi: tuşlar bir arada, satır sonunda.
                std::string keys{command.keys};
                if (!command.alias.empty())
                    keys += " " + std::string(command.alias);
                if (!keys.empty()) {
                    CHECK(has(shown, " " + keys + " │"));
                }
                for (const std::string& token : key_tokens(command)) {
                    CAPTURE(token);
                    CHECK(help_words.contains(token));
                }
                state.handle(Key::Down);
            }
        }
    }
    core::init_i18n("en");
}

TEST_CASE("the hint bar puts the palette first at every width from 60, in List, REVIEW and the trash") {
    const std::pair<const char*, const char*> languages[] = {{"en", ": cmd"}, {"tr", ": komut"}};
    for (const auto& [language, hint] : languages) {
        for (const unsigned context : {tui::kInList, tui::kInReview, tui::kInTrash}) {
            for (const int width : {60, 80, 120}) {
                CAPTURE(language);
                CAPTURE(context);
                CAPTURE(width);
                tui::TuiState state = state_for(context);
                core::init_i18n(language);
                const std::string bar = row_of(draw(state, width, 15), 14);
                CHECK(bar.starts_with(hint));
            }
        }
        tui::TuiState folder = sample_state();
        core::init_i18n(language);
        REQUIRE(folder.select_folder("work"));
        CHECK(row_of(draw(folder, 60, 15), 14).starts_with(hint));
    }
    core::init_i18n("en");
}
