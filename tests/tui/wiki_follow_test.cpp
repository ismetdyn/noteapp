// T2: REVIEW'da [[wiki]] bağlantısı izlenir (Tab/Enter, tıklama); eksik hedef soluk. Model saf, izleme gerçek
// depoyla, çizim ekran hücreleriyle.
#include "core/i18n.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

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

constexpr int kWidth = 100;
constexpr int kHeight = 30;
constexpr int kFirstPaneRow = 2;  // üst çubuk + panel kenarı

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

std::string row_of(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    return out;
}

struct Fixture {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache cache;
    tui::StatusLine status;
    std::optional<tui::ConfirmState> confirm;
    std::vector<std::string> lines = {"see [[b]] and [[gone]]", "plain", "then [[c|the c note]]"};
    tui::EditorSession editor{
        tui::EditorHost{*store, mutex, clipboard, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};
    tui::ListActions actions{
        tui::ListHost{store, mutex, clipboard, system, state, cache, status, [this] { load(); }, [] {}}};

    Fixture() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "a.md", "see [[b]] and [[gone]]\nplain\nthen [[c|the c note]]\n"));
        REQUIRE(test::seed_note(*store, "b.md", "bee\n"));
        REQUIRE(test::seed_note(*store, "c.md", "sea\n"));
        load();
        state.set_window(kWidth, kHeight);
        REQUIRE(state.select_note(np("a.md")));
        state.set_preview_lines(&lines, 1);
        state.set_links(core::NoteLinks{{np("b.md"), np("c.md")}, {}, {}, {"gone"}}, 1, np("a.md"));
        state.focus_preview();
    }
    void load() {
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    // REVIEW'daki `line`. satırın `byte`. baytının hücresi.
    int cell_x(std::size_t byte) const {
        return state.layout().geometry(kWidth, true).right_x + 2 +
               static_cast<int>(byte);  // kenar, iç boşluk
    }
    tui::MouseResult click(int x, int y) {
        tui::route_mouse({x, y, tui::MouseButton::Left, tui::MouseMotion::Press}, state, editor);
        return tui::route_mouse({x, y, tui::MouseButton::Left, tui::MouseMotion::Release}, state, editor);
    }
};

tui::Preview preview_of(const std::vector<std::string>& lines, bool rendered) {
    tui::Preview preview;
    preview.kind = tui::Preview::Kind::Text;
    preview.lines = lines;
    preview.rendered = rendered;
    if (rendered) {
        preview.styled.resize(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i)
            preview.source_lines.push_back(i);
    }
    return preview;
}

}  // namespace

TEST_CASE("wiki: Tab and Shift+Tab walk the links in REVIEW, Esc drops the choice, then Esc leaves") {
    Fixture f;
    CHECK_FALSE(f.state.selected_wiki_target().has_value());
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "b");
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "gone");
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "c");  // "|takma ad" hedefe girmez
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "c");  // sonda durur
    f.state.handle(Key::ShiftTab);
    CHECK(f.state.selected_wiki_target() == "gone");
    f.state.handle(Key::Escape);
    CHECK_FALSE(f.state.selected_wiki_target().has_value());
    CHECK(f.state.focus() == Focus::Preview);
    f.state.handle(Key::Escape);
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE("wiki: Enter follows the chosen link to its note and stays in REVIEW; without a link it edits") {
    Fixture f;
    f.state.handle(Key::Tab);
    const Intent intent = f.state.handle(Key::Enter);
    CHECK(intent == Intent::FollowLink);
    f.actions.run(intent, f.confirm);
    REQUIRE(f.state.selected_row() != nullptr);
    CHECK(f.state.selected_row()->path == np("b.md"));
    CHECK(f.state.focus() == Focus::Preview);
    CHECK_FALSE(f.state.selected_wiki_target().has_value());

    Fixture plain;
    CHECK(plain.state.handle(Key::Enter) == Intent::EnterEditor);
}

TEST_CASE("wiki: a missing target says so and stays on the note; nothing is written") {
    Fixture f;
    f.state.handle(Key::Tab);
    f.state.handle(Key::Tab);
    REQUIRE(f.state.selected_wiki_target() == "gone");
    const std::size_t notes = f.store->list().size();
    f.actions.run(f.state.handle(Key::Enter), f.confirm);
    CHECK(f.status.message == core::tr("tui.link_missing"));
    CHECK(f.state.selected_row()->path == np("a.md"));
    CHECK(f.store->list().size() == notes);
}

TEST_CASE("wiki: a click on a link follows it, a click beside it does not, a drag still selects text") {
    Fixture f;
    CHECK(f.click(f.cell_x(1), kFirstPaneRow) == tui::MouseResult::None);  // "see"
    CHECK(f.state.selected_row()->path == np("a.md"));
    const tui::MouseResult result = f.click(f.cell_x(6), kFirstPaneRow);  // "[[b]]" içi
    CHECK(result == tui::MouseResult::FollowLink);
    f.actions.on_mouse({f.cell_x(6), kFirstPaneRow, tui::MouseButton::Left, tui::MouseMotion::Press}, f.editor);
    f.actions.on_mouse({f.cell_x(6), kFirstPaneRow, tui::MouseButton::Left, tui::MouseMotion::Release},
                       f.editor);
    CHECK(f.state.selected_row()->path == np("b.md"));
}

TEST_CASE("wiki: in raw and rendered REVIEW a missing target is dim, the chosen link inverted") {
    for (const bool rendered : {false, true}) {
        CAPTURE(rendered);
        Fixture f;
        const tui::Preview preview = preview_of(f.lines, rendered);
        f.state.set_preview_lines(&preview.lines, 2);
        f.state.handle(Key::Tab);  // [[b]]
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(f.state, preview, f.status, {kWidth, kHeight}));
        const int y = kFirstPaneRow;
        REQUIRE(row_of(screen, y).find("see [[b]] and [[gone]]") != std::string::npos);
        const int see = f.cell_x(0);
        const int b = f.cell_x(4);
        const int gone = f.cell_x(14);
        CHECK_FALSE(screen.PixelAt(see, y).dim);
        CHECK(screen.PixelAt(b, y).inverted);
        CHECK_FALSE(screen.PixelAt(b, y).dim);
        CHECK(screen.PixelAt(gone, y).dim);
        CHECK(screen.PixelAt(gone + 7, y).dim);  // kapanan "]]"
        CHECK_FALSE(screen.PixelAt(gone, y).inverted);
    }
}

// Biçimli görünüm GUI gibi çizer (MD_FLAG_WIKILINKS): [[bağlantı]] ayraçsız metniyle, bağlantı görünüşünde.
// Gezinme, izleme ve eksik hedefin soluk çizimi ham görünümle aynı kuralla (hedef core::link_target anahtarı);
// kod içindeki ayraçlar bağlantı değildir.
TEST_CASE("wiki: rendered REVIEW draws links by their text and walks, dims and follows them") {
    Fixture f;
    const tui::RenderedView view = tui::render_markdown(
        "see [[b]] and [[gone]]\n\nthen [[c|the c note]] and `[[b]]`\n", kWidth, tui::terminal_cell_width);
    tui::Preview preview = preview_of(view.lines, true);
    preview.styled = view.spans;
    preview.wiki_targets = view.wiki_targets;
    REQUIRE(preview.lines.size() == 3);
    REQUIRE(preview.lines[0] == "see b and gone");
    REQUIRE(preview.lines[2] == "then the c note and [[b]]");
    f.state.set_preview_lines(&preview.lines, 2, false, &preview.styled, &preview.wiki_targets);

    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "b");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(f.state, preview, f.status, {kWidth, kHeight}));
    const int y = kFirstPaneRow;
    REQUIRE(row_of(screen, y).find("see b and gone") != std::string::npos);
    CHECK_FALSE(screen.PixelAt(f.cell_x(0), y).dim);
    CHECK_FALSE(screen.PixelAt(f.cell_x(0), y).underlined);
    CHECK(screen.PixelAt(f.cell_x(4), y).inverted);  // "b"
    CHECK(screen.PixelAt(f.cell_x(10), y).dim);      // "gone": hedefi yok
    CHECK(screen.PixelAt(f.cell_x(10), y).underlined);
    CHECK(screen.PixelAt(f.cell_x(13), y).dim);
    CHECK_FALSE(screen.PixelAt(f.cell_x(10), y).inverted);
    CHECK(screen.PixelAt(f.cell_x(5), y + 2).underlined);  // "the c note", boş satırdan sonra

    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "gone");
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "c");  // etiket değil hedef
    const auto chosen = f.state.wiki_selection_on_line(2);
    REQUIRE(chosen.has_value());
    CHECK(chosen->begin == 5);
    CHECK(chosen->end == 15);
    f.state.handle(Key::Tab);
    CHECK(f.state.selected_wiki_target() == "c");  // koddaki [[b]] bağlantı değil: sonda durur

    f.state.handle(Key::ShiftTab);
    f.state.handle(Key::ShiftTab);
    REQUIRE(f.state.selected_wiki_target() == "b");
    const Intent intent = f.state.handle(Key::Enter);
    CHECK(intent == Intent::FollowLink);
    f.actions.run(intent, f.confirm);
    REQUIRE(f.state.selected_row() != nullptr);
    CHECK(f.state.selected_row()->path == np("b.md"));
}
