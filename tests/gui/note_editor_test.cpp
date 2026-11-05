// G5a: GUI editör modelinin (ImGui'siz) TUI ile aynı kuralları ve GuiState'in pano yolu.
#include "gui/gui_state.hpp"
#include "gui/note_editor.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>

using gui_test::WindowFixture;
using noteapp::gui::NoteEditor;
using noteapp::tui::Key;

TEST_CASE("note editor: cell widths follow Unicode, not the font") {
    CHECK(noteapp::gui::cell_width(U'a') == 1);
    CHECK(noteapp::gui::cell_width(U'ş') == 1);
    CHECK(noteapp::gui::cell_width(U'́') == 0);  // birleşen akut
    CHECK(noteapp::gui::cell_width(U'中') == 2);
    CHECK(noteapp::gui::cell_width(U'😀') == 2);
}

TEST_CASE("note editor: a combining mark stays with its base letter") {
    NoteEditor editor("e\xCC\x81x", true, "txt", {});
    editor.handle(Key::Right);
    CHECK(editor.buffer().cursor() == 3);  // e + U+0301 tek adım
}

TEST_CASE("note editor: a read-only note moves and finds but never edits") {
    NoteEditor editor("abc abc", false, "txt", {});
    editor.handle(Key::Char, "x");
    editor.handle(Key::Backspace);
    editor.paste("y");
    CHECK(editor.cut() == "abc abc");
    CHECK(editor.text() == "abc abc");
    editor.handle(Key::End);
    CHECK(editor.buffer().cursor() == 7);
    editor.open_find(nullptr);
    editor.handle(Key::Char, "b");
    REQUIRE(editor.buffer().selection().has_value());
    CHECK(editor.buffer().selection()->begin == 1);  // bulma kökü imleç değil, açıldığı yer: baştan sarar
}

TEST_CASE("note editor: find keys follow the TUI: Enter and Down next, Up previous, Escape closes") {
    NoteEditor editor("x x x", true, "txt", {});
    editor.open_find(nullptr);
    editor.handle(Key::Char, "x");
    CHECK(editor.find_label() == "1/3");
    editor.handle(Key::Enter);
    CHECK(editor.find_label() == "2/3");
    editor.handle(Key::Down);
    CHECK(editor.find_label() == "3/3");
    editor.handle(Key::Up);
    CHECK(editor.find_label() == "2/3");
    editor.handle(Key::Escape);
    CHECK(editor.mode() == NoteEditor::Mode::Editing);
    CHECK(editor.find_matches().empty());
    CHECK(editor.buffer().selection()->begin == 2);
}

TEST_CASE("note editor: an edit drops stale matches, F3 searches again from the cursor") {
    NoteEditor editor("ab ab ab", true, "txt", {});
    editor.open_find(nullptr);
    editor.handle(Key::Char, "a");
    editor.handle(Key::Char, "b");
    editor.handle(Key::Escape);
    editor.handle(Key::End);
    editor.handle(Key::Char, " ab");
    CHECK(editor.find_matches().empty());
    editor.handle(Key::Home);
    editor.step_find(1);
    REQUIRE(editor.buffer().selection().has_value());
    CHECK(editor.buffer().selection()->begin == 0);
}

TEST_CASE("note editor: the wheel moves the cursor three rows like the TUI") {
    NoteEditor editor("1\n2\n3\n4\n5\n6\n7\n", true, "txt", {});
    editor.layout(20, 3);
    editor.wheel(1);
    CHECK(editor.buffer().cursor_line() == 3);
    editor.wheel(-1);
    CHECK(editor.buffer().cursor_line() == 0);
}

TEST_CASE("note editor: a drag keeps the anchor where the press was") {
    NoteEditor editor("abcdef", true, "txt", {});
    editor.layout(20, 3);
    editor.click(0, 4, false);
    editor.click(0, 1, true);
    REQUIRE(editor.buffer().selection().has_value());
    CHECK(editor.buffer().selection()->begin == 1);
    CHECK(editor.buffer().selection()->end == 4);
    CHECK(editor.buffer().cursor() == 1);  // seçim yönü korunur
}

TEST_CASE_FIXTURE(WindowFixture, "note editor: Ctrl+C, Ctrl+X and Ctrl+V go through the system clipboard") {
    add_note("clip.md", "one two");
    auto state = make_state();
    state->edit_selected();  // seçilen not incelemede açılır
    std::string copied;
    std::optional<std::string> system;
    state->deciders.copy_text = [&](const std::string& text) {
        copied = text;
        return true;
    };
    state->deciders.paste_text = [&] { return system; };

    state->editor_key(Key::ShiftRight);
    state->editor_key(Key::ShiftRight);
    state->editor_key(Key::ShiftRight);
    state->editor_key(Key::CtrlC);
    CHECK(copied == "one");
    CHECK_FALSE(state->dirty());
    state->editor_key(Key::CtrlX);
    CHECK(copied == "one");
    CHECK(state->buffer() == " two");
    CHECK(state->dirty());

    state->editor_key(Key::End);
    state->editor_key(Key::CtrlV);  // sistem panosu okunamadı: iç pano
    CHECK(state->buffer() == " twoone");
    system = "ğ";
    state->editor_key(Key::CtrlV);
    CHECK(state->buffer() == " twooneğ");
    state->save_now();
    CHECK(read_note("clip.md") == " twooneğ");
}

TEST_CASE_FIXTURE(WindowFixture, "note editor: a paste above limits.tui.paste_bytes is refused with a message") {
    config.limits.tui.paste_bytes = noteapp::core::Limit{4, "4B"};
    add_note("clip.md", "x");
    auto state = make_state();
    state->edit_selected();
    state->deciders.paste_text = [] { return std::optional<std::string>("12345"); };
    state->editor_key(Key::CtrlV);
    CHECK(state->buffer() == "x");
    CHECK_FALSE(state->status().empty());
}

// TUI: salt-okunur notta Ctrl+X hiçbir şey yapmaz (kopya Ctrl+C'dir).
TEST_CASE_FIXTURE(WindowFixture, "note editor: Ctrl+X in a read-only note neither cuts nor copies") {
    add_note("latin.txt", "caf\xe9");
    auto state = make_state();
    std::string copied;
    state->deciders.copy_text = [&](const std::string& text) {
        copied = text;
        return true;
    };
    state->editor_key(Key::CtrlX);
    CHECK(copied.empty());
    state->editor_key(Key::CtrlC);
    CHECK(copied == "caf\xEF\xBF\xBD");
}
