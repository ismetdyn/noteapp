// T3: TUI editör sekmeleri (SPEC-tui "Sekmeler"). Veri bütünlüğü: ayrılan sekmenin düzenlemesi yalnız kendi
// notuna yazılır, taslak kaydedilmeden kalır, kapanış mevcut kaydedilmemiş akışından geçer, senkron silmesi
// ve klasör taşıması sekmeleri doğru günceller.
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_tabs.hpp"
#include "tui/list_actions.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::EditorSession;
using tui::EditorTabs;
using tui::Key;

namespace {

int plain_width(char32_t) {
    return 1;
}

constexpr std::size_t kRoomy = 1024 * 1024;

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

struct TabsFixture {
    explicit TabsFixture(core::Limit limit = {}) : tabs(factory(), std::move(limit)) {
        core::init_i18n("en");
        put("a.md", "alpha\n");
        put("b.md", "beta\n");
        put("c.md", "gamma\n");
    }

    EditorTabs::Factory factory() {
        return [this](std::uint64_t) {
            auto session = std::make_unique<EditorSession>(
                tui::EditorHost{store, mutex, clipboard, plain_width, kRoomy, core::kDefaultSlugMaxLength,
                                [](std::string_view) { return std::string("md"); },
                                [](const core::NotePath&) {}, [] {}, [](std::optional<std::size_t>) {},
                                [this](const core::Error& error) { errors.push_back(error); }, [] {}});
            session->layout(40, 10);
            return session;
        };
    }

    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    std::vector<core::Error> errors;
    EditorTabs tabs;

    void put(const std::string& name, std::string_view bytes) const {
        test::upsert_note(dir.path(), name, bytes);
    }
    std::string bytes(const std::string& name) const { return test::note_body(dir.path(), name); }
    bool exists(const std::string& name) const { return test::note_exists(dir.path(), name); }
    void type(std::string_view text) {
        for (const char c : text)
            tabs.active().handle(Key::Char, std::string_view(&c, 1));
        tabs.mark_edited();
    }
    std::string text() const {
        const tui::EditorBuffer* buffer = tabs.active().buffer();
        return buffer ? buffer->text() : std::string{};
    }
    std::string active_path() const {
        const auto& path = tabs.active().path();
        return path ? path->string() : std::string{};
    }
    std::vector<std::string> titles() const {
        std::vector<std::string> out;
        for (const tui::TabLabel& label : tabs.labels())
            out.push_back(label.title);
        return out;
    }
};

}  // namespace

TEST_CASE("tabs: leaving a tab writes its pending edit to its own note only") {
    TabsFixture f;
    REQUIRE(f.tabs.open_note(np("a.md"), 0, false) == EditorTabs::Result::Done);
    f.type("A");
    REQUIRE(f.tabs.open_note(np("b.md"), 0, true) == EditorTabs::Result::Done);
    CHECK(f.bytes("a.md") == "Aalpha\n");
    CHECK(f.bytes("b.md") == "beta\n");
    CHECK(f.active_path() == "b.md");
    f.type("B");
    REQUIRE(f.tabs.cycle(-1));
    CHECK(f.bytes("b.md") == "Bbeta\n");
    CHECK(f.bytes("a.md") == "Aalpha\n");
    CHECK(f.active_path() == "a.md");
    // Sekmenin imleci yerinde: sonraki harf önceki harfin arkasına.
    f.type("x");
    CHECK(f.text() == "Axalpha\n");
}

TEST_CASE("tabs: a written draft stays unsaved in its tab and the note opens in a new tab") {
    TabsFixture f;
    REQUIRE(f.tabs.begin_note([](EditorSession& s) { s.begin_new_note(""); }, false) ==
            EditorTabs::Result::Done);
    f.type("draft text");
    REQUIRE(f.tabs.open_note(np("a.md"), 0, false) == EditorTabs::Result::Done);
    CHECK(f.tabs.size() == 2);
    CHECK(f.store.list().size() == 3);  // taslak yazılmadı
    REQUIRE(f.tabs.cycle(1));
    CHECK(f.tabs.active().is_draft());
    CHECK(f.text() == "draft text");
}

// GUI ile aynı kural (app::keeps_tab): boş sekme korunacak bir şey taşımaz, düzenlenmiş olsa da.
TEST_CASE("tabs: a draft edited back to empty is replaced by the next note") {
    TabsFixture f;
    REQUIRE(f.tabs.begin_note([](EditorSession& s) { s.begin_new_note(""); }, false) ==
            EditorTabs::Result::Done);
    f.type("x");
    f.tabs.active().handle(Key::Backspace);
    REQUIRE(f.text().empty());
    REQUIRE(f.tabs.open_note(np("a.md"), 0, false) == EditorTabs::Result::Done);
    CHECK(f.titles() == std::vector<std::string>{"a.md"});
}

TEST_CASE("tabs: an open note focuses its tab instead of duplicating it") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, true);
    REQUIRE(f.tabs.open_note(np("a.md"), 0, false) == EditorTabs::Result::Done);
    CHECK(f.tabs.size() == 2);
    CHECK(f.active_path() == "a.md");
    REQUIRE(f.tabs.open_note(np("a.md"), 0, true) == EditorTabs::Result::Done);
    CHECK(f.tabs.size() == 2);
}

TEST_CASE("tabs: an untouched tab is replaced by the next note, an edited one is kept") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, false);
    CHECK(f.titles() == std::vector<std::string>{"b.md"});
    f.type("x");
    f.tabs.active().handle(Key::CtrlS);
    f.tabs.open_note(np("c.md"), 0, false);
    CHECK(f.titles() == std::vector<std::string>{"b.md", "c.md"});
    CHECK(f.tabs.labels()[0].kept);
    CHECK_FALSE(f.tabs.labels()[1].kept);
}

TEST_CASE("tabs: closing a dirty tab goes through the unsaved prompt and the neighbour comes forward") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, true);
    f.type("B");
    f.tabs.close_active();
    CHECK(f.tabs.active().mode() == EditorSession::Mode::LeavePrompt);
    CHECK_FALSE(f.tabs.sweep().active_closed);
    f.tabs.active().handle(Key::Right);  // Kaydet -> At
    f.tabs.active().handle(Key::Enter);
    const EditorTabs::Swept swept = f.tabs.sweep();
    CHECK(swept.active_closed);
    CHECK(swept.reopen);
    CHECK(f.bytes("b.md") == "beta\n");
    CHECK(f.titles() == std::vector<std::string>{"a.md"});
    f.tabs.load_active();
    CHECK(f.active_path() == "a.md");
}

TEST_CASE("tabs: cancelling the close keeps the tab and a later plain leave does not reopen a neighbour") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, true);
    f.type("B");
    f.tabs.close_active();
    f.tabs.active().handle(Key::Escape);
    CHECK_FALSE(f.tabs.sweep().active_closed);
    CHECK(f.tabs.size() == 2);
    f.tabs.active().handle(Key::CtrlS);
    f.tabs.active().handle(Key::CtrlQ);  // Ctrl+Q: sekmeyi kapatır, editörden çıkar
    const EditorTabs::Swept swept = f.tabs.sweep();
    CHECK(swept.active_closed);
    CHECK_FALSE(swept.reopen);
}

TEST_CASE("tabs: a note deleted elsewhere closes its clean tab, a dirty tab stays") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, true);
    f.type("B");
    test::erase_note(f.dir.path(), "a.md");
    test::erase_note(f.dir.path(), "b.md");
    const auto closed = f.tabs.on_listing(f.store.list());
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].path == np("a.md"));
    f.tabs.sweep();
    CHECK(f.titles() == std::vector<std::string>{"b.md"});
    CHECK(f.text() == "Bbeta\n");
    CHECK(f.tabs.active().deleted_on_disk());
}

// GUI A5b'nin TUI karşılığı: arka plandaki sekme taşınan klasörde yeni yolu izler, kayıt oraya gider.
TEST_CASE("tabs: a background tab in a renamed folder saves to the new path") {
    TabsFixture f;
    f.put("f/n.md", "old\n");
    f.tabs.open_note(np("f/n.md"), 0, false);
    f.type("x");
    f.tabs.open_note(np("b.md"), 0, true);  // x kaydedildi, sekme arka planda
    REQUIRE(f.store.rename_folder("f", "g"));
    f.tabs.follow_moved("f", "g");
    CHECK(f.tabs.on_listing(f.store.list()).empty());
    CHECK(f.titles() == std::vector<std::string>{"n.md", "b.md"});
    REQUIRE(f.tabs.cycle(-1));
    CHECK(f.active_path() == "g/n.md");
    f.type("y");
    f.tabs.active().handle(Key::CtrlS);
    CHECK(f.bytes("g/n.md") == "xyold\n");
    CHECK_FALSE(f.exists("f/n.md"));
}

TEST_CASE("tabs: limits.app.tabs refuses one more tab and 0 means unlimited") {
    TabsFixture f(core::Limit{2, "2"});
    f.tabs.open_note(np("a.md"), 0, true);
    f.tabs.open_note(np("b.md"), 0, true);
    CHECK(f.tabs.open_note(np("c.md"), 0, true) == EditorTabs::Result::Limit);
    CHECK(f.tabs.size() == 2);
    CHECK(f.active_path() == "b.md");
    TabsFixture unlimited;
    for (const char* name : {"a.md", "b.md", "c.md"})
        unlimited.tabs.open_note(np(name), 0, true);
    CHECK(unlimited.tabs.size() == 3);
}

TEST_CASE("tabs: open tabs are saved without drafts and restored lazily") {
    TabsFixture f;
    f.tabs.open_note(np("a.md"), 0, false);
    f.tabs.open_note(np("b.md"), 0, true);
    f.tabs.begin_note([](EditorSession& s) { s.begin_new_note(""); }, true);
    f.tabs.cycle(-1);
    const app::SavedTabs saved = f.tabs.saved();
    REQUIRE(saved.tabs.size() == 2);
    CHECK(saved.tabs[1].path == "b.md");
    CHECK(saved.tabs[1].kept);
    CHECK(saved.active == 1);

    // Durum dosyasıyla gidip gelir; olmayan not atlanır.
    tui::ListPrefs prefs;
    prefs.tabs = saved;
    prefs.tabs.tabs.push_back({"gone.md"});
    const tui::ListPrefs read = tui::parse_list_prefs(tui::format_list_prefs(prefs));
    CHECK(read.tabs.tabs.size() == 3);

    TabsFixture next;
    next.tabs.restore(read.tabs, [&](const core::NotePath& path) { return next.exists(path.string()); });
    CHECK(next.titles() == std::vector<std::string>{"a.md", "b.md"});
    CHECK_FALSE(next.tabs.active().is_open());  // içerik ilk etkinleşmede okunur
    next.tabs.load_active();
    CHECK(next.active_path() == "b.md");
    CHECK(next.text() == "beta\n");
}

TEST_CASE("tabs: exit writes a written draft from a background tab") {
    TabsFixture f;
    f.tabs.begin_note([](EditorSession& s) { s.begin_new_note(""); }, false);
    f.type("kept on exit");
    f.tabs.open_note(np("a.md"), 0, false);
    const auto notes = f.tabs.flush_on_exit();
    REQUIRE(notes.size() == 1);
    CHECK(f.store.list().size() == 4);
}

namespace {

core::NoteInfo listed(const std::string& path) {
    return core::NoteInfo{np(path), std::filesystem::file_time_type{}, 1};
}

tui::TuiState listed_state() {
    tui::TuiState state;
    state.set_notes({listed("a.md"), listed("b.md")});
    return state;
}

}  // namespace

TEST_CASE("tabs: in the editor tab keys are tab commands and letters still go to the text") {
    tui::TuiState state = listed_state();
    state.enter_editor();
    CHECK(state.handle(Key::CtrlT) == tui::Intent::TabNew);
    CHECK(state.handle(Key::CtrlW) == tui::Intent::TabClose);
    CHECK(state.handle(Key::AltN) == tui::Intent::TabNext);
    CHECK(state.handle(Key::CtrlPageDown) == tui::Intent::TabNext);
    CHECK(state.handle(Key::AltP) == tui::Intent::TabPrevious);
    CHECK(state.handle(Key::CtrlPageUp) == tui::Intent::TabPrevious);
    CHECK(state.handle(Key::CtrlK) == tui::Intent::ParkEditor);
    CHECK(state.handle(Key::Char, "t") == tui::Intent::EditorKey);
    CHECK(state.handle(Key::CtrlS) == tui::Intent::EditorKey);
    CHECK(state.focus() == tui::Focus::Editor);
    state.search_from_editor();
    CHECK(state.focus() == tui::Focus::Search);
}

TEST_CASE("tabs: the list creates and cycles tabs, Ctrl+T in the search line still switches the mode") {
    tui::TuiState state = listed_state();
    CHECK(state.handle(Key::CtrlT) == tui::Intent::TabNew);
    CHECK(state.handle(Key::AltN) == tui::Intent::TabNext);
    CHECK(state.handle(Key::AltP) == tui::Intent::TabPrevious);
    CHECK(state.handle(Key::CtrlW) == tui::Intent::None);  // kapatma yalnız editörde
    state.handle(Key::Char, "/");
    REQUIRE(state.focus() == tui::Focus::Search);
    CHECK(state.handle(Key::CtrlT) == tui::Intent::None);
    CHECK(state.mode() == tui::Mode::Content);
}

TEST_CASE("tabs: the row menu and the palette open the selected note in a new tab") {
    tui::TuiState state = listed_state();
    state.open_row_menu();
    REQUIRE(state.palette_open());
    REQUIRE(state.palette_selected() != nullptr);
    CHECK(state.palette_selected()->id == "tab.open");
    CHECK(state.handle(Key::Enter) == tui::Intent::TabOpenSelected);
}

TEST_CASE("tabs: the tab strip brackets the active tab and keeps it visible when narrow") {
    const std::vector<tui::TabLabel> labels{
        {"a.md", false, false, true}, {"b.md", true, true, true}, {"c.md", false, false, false}};
    CHECK(tui::tab_strip_text(labels, 80) == " a.md │ [b.md*] │ c.md ");
    const std::string narrow = tui::tab_strip_text(labels, 16);
    CHECK(narrow.find("[b.md*]") != std::string::npos);
    CHECK(narrow.find("…") != std::string::npos);
    CHECK(ftxui::string_width(narrow) <= 16);
    CHECK(tui::tab_strip_text({}, 80).empty());
    // Ad senkrondan gelir: denetim baytı terminale gitmez.
    const std::vector<tui::TabLabel> hostile{{"x\x1b[2Jy.md", true}};
    CHECK(tui::tab_strip_text(hostile, 80).find('\x1b') == std::string::npos);
}

TEST_CASE("tabs: the tab line is drawn on the right pane's top border above REVIEW") {
    core::init_i18n("en");
    tui::TuiState state = listed_state();
    constexpr int kWidth = 100;
    constexpr int kHeight = 20;
    state.set_window(kWidth, kHeight);
    const std::vector<tui::TabLabel> labels{{"a.md", false, false, true}, {"b.md", true, false, true}};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, tui::Preview{}, {"sync off", ""}, {kWidth, kHeight}, nullptr,
                                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, labels));
    const std::string shown = screen.ToString();
    const std::size_t second_line = shown.find('\n') + 1;
    const std::string border = shown.substr(second_line, shown.find('\n', second_line) - second_line);
    CHECK(border.find("a.md │ [b.md]") != std::string::npos);
}
