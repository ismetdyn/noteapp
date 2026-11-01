#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/mouse.hpp"
#include "tui/note_find.hpp"
#include "tui/pane_layout.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Focus;
using tui::Key;
using tui::MouseButton;
using tui::MouseMotion;
using tui::PaneLayout;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

struct Fixture {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::TuiState state;
    tui::EditorSession editor{
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};

    Fixture() {
        core::init_i18n("en");
        for (const char* name : {"a.md", "b.md", "c.md"})
            test::upsert_note(dir.path(), name, "metin\n");
        state.set_notes(store.list());
        state.set_window(kWidth, kHeight);
        state.set_preview_line_count(100);
    }

    void mouse(int x, int y, MouseButton button, MouseMotion motion = MouseMotion::Press) {
        tui::route_mouse({x, y, button, motion}, state, editor);
    }
    int divider() const { return state.layout().list_width(kWidth); }
    void open_editor() {
        editor.open(state.selected_row()->path);
        REQUIRE(state.focus() == Focus::Editor);
    }
};

constexpr int kRowY = 2;  // ilk liste satırı

}  // namespace

TEST_CASE("pane layout: the default is 2/5 and keys stop at the bounds, also at 60 columns") {
    PaneLayout layout;
    CHECK(layout.list_width(100) == 40);
    CHECK(layout.list_width(60) == 24);
    for (int i = 0; i < 20; ++i)
        layout.resize(-PaneLayout::kKeyStep, 60);
    CHECK(layout.list_width(60) == PaneLayout::kMinListWidth);
    for (int i = 0; i < 20; ++i)
        layout.resize(PaneLayout::kKeyStep, 60);
    CHECK(layout.list_width(60) == 60 - PaneLayout::kMinRightWidth);
}

TEST_CASE("pane layout: a shrinking window pulls a wide list back so both panes stay usable") {
    PaneLayout layout;
    layout.set_list_width(150, 200);
    CHECK(layout.list_width(200) == 150);
    CHECK(layout.list_width(60) == 60 - PaneLayout::kMinRightWidth);
}

TEST_CASE("pane layout: zoom shows only the focused pane; the divider is gone") {
    PaneLayout layout;
    layout.toggle_zoom();
    const auto left = layout.geometry(100, false);
    CHECK(left.list_visible);
    CHECK_FALSE(left.right_visible);
    CHECK(left.list_width == 100);
    const auto right = layout.geometry(100, true);
    CHECK_FALSE(right.list_visible);
    CHECK(right.right_visible);
    CHECK(right.right_width == 100);
    CHECK_FALSE(layout.on_divider(40, 100));
}

TEST_CASE("find_folded: Turkish folding, byte offsets in the original text, across lines") {
    const std::string text = "Işık\nİŞIK ışık\nyok";
    const auto matches = tui::find_folded(text, "ışık");
    REQUIRE(matches.size() == 3);
    CHECK(text.substr(matches[0].begin, matches[0].end - matches[0].begin) == "Işık");
    CHECK(text.substr(matches[1].begin, matches[1].end - matches[1].begin) == "İŞIK");
    CHECK(text.substr(matches[2].begin, matches[2].end - matches[2].begin) == "ışık");
    CHECK(tui::find_folded(text, "").empty());
    CHECK(tui::find_folded(text, "zzz").empty());
    CHECK(tui::find_folded("", "x").empty());
}

TEST_CASE("find_folded: overlapping text advances past each match and CRLF stays out of the match") {
    CHECK(tui::find_folded("aaaa", "aa").size() == 2);
    const std::string crlf = "son\r\nson";
    const auto matches = tui::find_folded(crlf, "son");
    REQUIRE(matches.size() == 2);
    CHECK(matches[1].begin == 5);
}

TEST_CASE("mouse: dragging the divider resizes within bounds and release ends the drag") {
    Fixture f;
    const int start = f.divider();
    f.mouse(start, 5, MouseButton::Left);
    REQUIRE(f.state.layout().dragging());
    f.mouse(50, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == 51);
    f.mouse(0, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == PaneLayout::kMinListWidth);
    f.mouse(99, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == kWidth - PaneLayout::kMinRightWidth);
    f.mouse(99, 5, MouseButton::Left, MouseMotion::Release);
    CHECK_FALSE(f.state.layout().dragging());
    const int kept = f.divider();
    f.mouse(30, 5, MouseButton::Other, MouseMotion::Move);
    CHECK(f.divider() == kept);
    CHECK(f.state.focus() == Focus::List);
}

// Release, modal açıkken main'de düşer ya da terminalde kaybolur: sürükleme asılı kalıp bölücüyü taşımasın.
TEST_CASE("mouse: a move with no button after a lost release ends the drag and does not resize") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    f.mouse(50, 5, MouseButton::Left, MouseMotion::Move);
    REQUIRE(f.divider() == 51);
    f.mouse(30, 5, MouseButton::Other, MouseMotion::Move);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == 51);
}

TEST_CASE("mouse: a press after a lost release ends the drag and acts as a normal click") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    const core::NotePath first = f.state.selected_row()->path;
    f.mouse(5, kRowY + 1, MouseButton::Left);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == width);
    CHECK(f.state.selected_row()->path != first);
}

TEST_CASE("mouse: a press on the divider after a lost release starts a fresh drag without resizing") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    f.mouse(width, 5, MouseButton::Left);
    CHECK(f.state.layout().dragging());
    CHECK(f.divider() == width);
}

TEST_CASE("mouse: the wheel after a lost release does not resize") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == width);
}

TEST_CASE("mouse: the divider can be dragged from the editor without touching the text") {
    Fixture f;
    f.open_editor();
    f.editor.handle(Key::Char, "x");
    f.mouse(f.divider(), 5, MouseButton::Left);
    f.mouse(30, 5, MouseButton::Left, MouseMotion::Move);
    f.mouse(30, 5, MouseButton::Left, MouseMotion::Release);
    CHECK(f.divider() == 31);
    CHECK(f.state.focus() == Focus::Editor);
    CHECK(f.editor.buffer()->text() == "xmetin\n");
}

TEST_CASE("mouse: wheel over the list moves the selection, over the right pane scrolls the preview") {
    Fixture f;
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK(f.state.selected() == 1);
    CHECK(f.state.focus() == Focus::List);
    f.mouse(5, 5, MouseButton::WheelUp);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.state.preview_scroll() == 3);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelUp);
    CHECK(f.state.preview_scroll() == 0);
}

TEST_CASE("mouse: wheel in the editor moves in the text, over the list it does not change the selection") {
    Fixture f;
    f.open_editor();
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.editor.buffer()->position().line == 1);  // metin iki satır: sonda durur
}

TEST_CASE("mouse: clicking a list row selects it and focuses the list; a click below the rows is harmless") {
    Fixture f;
    f.state.handle(Key::Right);
    REQUIRE(f.state.focus() == Focus::Preview);
    f.mouse(5, kRowY + 2, MouseButton::Left);
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected() == 2);
    f.mouse(5, kRowY + 20, MouseButton::Left);
    CHECK(f.state.selected() == 2);
    f.mouse(5, 1, MouseButton::Left);  // çerçeve
    CHECK(f.state.selected() == 2);
}

TEST_CASE("mouse: a click on the right pane focuses Preview") {
    Fixture f;
    f.mouse(70, 5, MouseButton::Left);
    CHECK(f.state.focus() == Focus::Preview);
    f.state.handle(Key::Left);
    f.state.handle(Key::Char, "/");
    REQUIRE(f.state.focus() == Focus::Search);
    f.mouse(70, 5, MouseButton::Left);
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("mouse: clicking the list from a clean editor leaves it, from a dirty one it asks first") {
    Fixture f;
    f.open_editor();
    f.mouse(5, kRowY + 1, MouseButton::Left);
    CHECK_FALSE(f.editor.is_open());
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected() == 1);

    f.open_editor();
    f.editor.handle(Key::Char, "x");
    f.mouse(5, kRowY, MouseButton::Left);
    CHECK(f.editor.is_open());
    CHECK(f.editor.mode() == tui::EditorSession::Mode::LeavePrompt);
    CHECK(f.state.selected() == 1);
    CHECK(f.editor.buffer()->text() == "xmetin\n");
}

TEST_CASE("mouse: an open prompt or input ignores the pointer") {
    Fixture f;
    f.open_editor();
    f.editor.handle(Key::Char, "x");
    f.editor.handle(Key::CtrlQ);
    f.mouse(5, kRowY + 1, MouseButton::Left);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.editor.mode() == tui::EditorSession::Mode::LeavePrompt);
    CHECK(f.state.selected() == 0);
}

TEST_CASE("mouse: zoomed, the whole width belongs to the focused pane") {
    Fixture f;
    f.state.layout().toggle_zoom();
    f.mouse(90, kRowY + 1, MouseButton::Left);  // Liste odaklı ve tam genişlik
    CHECK(f.state.selected() == 1);
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE("mouse: a click in the editor puts the cursor at that cell, the gutter is not text") {
    Fixture f;
    test::upsert_note(f.dir.path(), "a.md", "birinci\nikinci\nson\n");
    // Seçili not mtime sırasına, o da saat tıkına bağlıdır: not adıyla açılır.
    f.editor.open(core::NotePath::parse("a.md").value());
    REQUIRE(f.state.focus() == Focus::Editor);
    const auto viewport = tui::editor_viewport({kWidth, kHeight}, f.state.layout());
    f.editor.layout(viewport.columns, viewport.rows);
    const int text_x = f.divider() + 1 + f.editor.gutter_width();
    f.mouse(text_x + 2, kRowY + 1, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 1);
    CHECK(f.editor.buffer()->position().column == 2);
    CHECK(f.state.focus() == Focus::Editor);
    // Numara sütununa tıklama satır başıdır, sayı metin sütunu sayılmaz.
    f.mouse(f.divider() + 1, kRowY + 2, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 2);
    CHECK(f.editor.buffer()->position().column == 0);
    // Dosya sonunun ötesi son satıra iner, metin değişmez.
    f.mouse(text_x + 40, kRowY + 20, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 3);
    CHECK_FALSE(f.editor.dirty());
}

TEST_CASE("mouse: the editor's two bottom bars are not text rows") {
    Fixture f;
    f.open_editor();
    const auto viewport = tui::editor_viewport({kWidth, kHeight}, f.state.layout());
    f.editor.layout(viewport.columns, viewport.rows);
    const auto before = f.editor.buffer()->cursor();
    f.mouse(f.divider() + 5, kHeight - 2, MouseButton::Left);  // durum çubuğu
    f.mouse(f.divider() + 5, kHeight - 3, MouseButton::Left);  // panelin alt kenarı
    CHECK(f.editor.buffer()->cursor() == before);
}
