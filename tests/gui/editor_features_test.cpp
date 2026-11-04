// G5b: GUI editöründe sözdizimi renkleri, yazım denetimi alt çizgisi, gölge öneri ve Ctrl+Space listesi; TUI ile
// aynı modüller (edit/syntax, edit/spellcheck, edit/completion).
#include "edit/syntax.hpp"
#include "gui/gui_state.hpp"
#include "gui/note_editor.hpp"
#include "gui/view.hpp"
#include "support/test_support.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <string>
#include <thread>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::NoteEditor;
using noteapp::gui::View;
using noteapp::tui::Key;
namespace fs = std::filesystem;

namespace {

void focus_editor(GuiState& state, View& view) {
    state.request_focus(FocusRequest::Editor);
    settle(view);
    REQUIRE(view.editor_active());
}

bool has_rect_with_prefix(const View& view, const std::string& prefix) {
    const auto& rects = view.item_rects();
    const auto found = rects.lower_bound(prefix);
    return found != rects.end() && found->first.starts_with(prefix);
}

}  // namespace

TEST_CASE("note editor: syntax spans come from the shared highlighter for the note's type") {
    const std::string line = "def run():  # yorum";
    const NoteEditor editor(line + "\n", true, "py", {});
    std::vector<noteapp::tui::SyntaxSpan> expected;
    noteapp::tui::highlight_line(noteapp::tui::SyntaxLanguage::Python, line, noteapp::tui::kInitialSyntaxState,
                                 &expected);
    const auto spans = editor.syntax_spans(0);
    REQUIRE_FALSE(spans.empty());
    REQUIRE(spans.size() == expected.size());
    for (std::size_t index = 0; index < spans.size(); ++index) {
        CHECK(spans[index].begin == expected[index].begin);
        CHECK(spans[index].kind == expected[index].kind);
    }
}

TEST_CASE("note editor: a note above limits.tui.highlight_bytes is drawn plain") {
    noteapp::gui::EditorLimits limits;
    limits.syntax.note_bytes = 4;
    const NoteEditor editor("def run(): pass\n", true, "py", limits);
    CHECK(editor.syntax_spans(0).empty());
}

TEST_CASE("note editor: typing a known prefix shows a ghost, Tab accepts it") {
    NoteEditor editor("merhaba\n", true, "md", {});
    editor.handle(Key::CtrlEnd);
    editor.handle(Key::Char, "m");
    editor.handle(Key::Char, "e");
    CHECK(editor.ghost() == "rhaba");
    editor.handle(Key::Tab);
    CHECK(editor.text() == "merhaba\nmerhaba");
    CHECK(editor.ghost().empty());
}

TEST_CASE("note editor: Ctrl+Space in a read-only note opens nothing") {
    NoteEditor editor("alpha alp", false, "md", {});
    editor.handle(Key::CtrlEnd);
    editor.handle(Key::CtrlSpace);
    CHECK(editor.mode() == NoteEditor::Mode::Editing);
}

TEST_CASE_FIXTURE(WindowFixture, "editor: syntax colours are drawn for the visible lines") {
    add_note("kod.py", "def run():\n    return 1  # bitti\n");
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK(view->item_rects().contains("editor_syntax:1"));
    CHECK(view->item_rects().contains("editor_syntax:2"));
}

TEST_CASE_FIXTURE(WindowFixture, "editor: plain text notes get no syntax colours") {
    add_note("düz.txt", "def run():\n");
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK_FALSE(has_rect_with_prefix(*view, "editor_syntax:"));
}

// Taslağın tipi ilk kayda kadar yoktu: renk, yazım kuralı ve durum çubuğu kayıttan sonra geliyordu. TUI gibi
// kaydedilmemiş taslakta tip canlı detect'tir.
TEST_CASE_FIXTURE(WindowFixture, "editor: an unsaved draft gets a live detected type before the first save") {
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    state->new_note();
    REQUIRE(state->is_new_note());

    state->edit_text("{\n  \"ad\": \"deneme\",\n  \"sayi\": 1\n}\n");
    CHECK(state->current_type() == "json");
    CHECK(state->editor().type() == "json");
    state->edit_text("#!/usr/bin/env python3\ndef run():\n    return 1  # bitti\n");
    CHECK(state->current_type() == "py");
    CHECK(state->editor_fields().starts_with("py"));
    settle(*view);
    CHECK(view->item_rects().contains("editor_syntax:2"));
    CHECK(state->is_new_note());  // tip yalnız gösterim içindir: taslak kaydedilmedi
}

TEST_CASE_FIXTURE(WindowFixture, "editor: Ctrl+Space opens the completion list, typing filters, Enter inserts") {
    add_note("söz.md", "alpine alpaca\n");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_End);
    type_text(*view, "al");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Space);
    REQUIRE(state->editor().mode() == NoteEditor::Mode::Complete);
    settle(*view);
    CHECK(view->item_rects().contains("editor_completion"));
    type_text(*view, "pac");  // liste süzülür, metne yazılmaz (TUI)
    REQUIRE(state->editor().completion_items().size() == 1);
    CHECK(state->editor().completion_items().front() == "alpaca");
    press(*view, ImGuiKey_Enter);
    CHECK(state->buffer() == "alpine alpaca\nalpaca");
    CHECK(state->editor().mode() == NoteEditor::Mode::Editing);
    CHECK(state->dirty());
}

TEST_CASE_FIXTURE(WindowFixture, "editor: Ctrl+P and Ctrl+N walk the completion list instead of search and new") {
    add_note("söz.md", "alpine alpaca\n");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_End);
    type_text(*view, "al");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Space);
    REQUIRE(state->editor().completion_items().size() == 2);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_N);
    CHECK(state->editor().completion_selected() == 1);
    CHECK_FALSE(state->is_new_note());
    press(*view, ImGuiMod_Ctrl | ImGuiKey_P);
    CHECK(state->editor().completion_selected() == 0);
    CHECK_FALSE(view->search_active());
    press(*view, ImGuiKey_Escape);
    CHECK(state->editor().mode() == NoteEditor::Mode::Editing);
    CHECK(state->buffer() == "alpine alpaca\nal");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: the ghost suggestion is drawn after the cursor and Tab accepts it") {
    add_note("söz.md", "merhaba\n");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_End);
    type_text(*view, "me");
    settle(*view);
    REQUIRE(view->item_rects().contains("editor_ghost"));
    CHECK(view->item_rects().at("editor_ghost").min_x >= view->item_rects().at("editor_cursor").min_x);
    press(*view, ImGuiKey_Tab);
    CHECK(state->buffer() == "merhaba\nmerhaba");
}

#ifndef _WIN32
// TUI'nin sahte hunspell'i (tests/tui/completion_test.cpp): `kotu` ile başlayan sözcük yanlıştır.
TEST_CASE_FIXTURE(WindowFixture, "editor: misspelled words are underlined when hunspell is present") {
    const noteapp::test::TempDir tools;
    const noteapp::test::ScopedEnv path("PATH", noteapp::test::utf8(tools.path()));
    {
        std::ofstream out(tools.path() / "hunspell");
        out << "#!/bin/sh\n"
               "echo '@(#) International Ispell Version 3.2.06 (but really Hunspell 1.7.2)'\n"
               "while IFS= read -r line; do\n"
               "  for word in $line; do\n"
               "    word=\"${word#^}\"\n"
               "    case \"$word\" in kotu*) echo \"& $word 1 0: iyi\" ;; *) echo \"*\" ;; esac\n"
               "  done\n"
               "  echo\n"
               "done\n";
    }
    fs::permissions(tools.path() / "hunspell", fs::perms::owner_all);
    add_note("yazı.md", "iyi kotu söz\n");
    auto state = make_state();
    auto view = make_view(*state);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!view->item_rects().contains("editor_spell:1:4") && std::chrono::steady_clock::now() < deadline) {
        state->tick(std::chrono::steady_clock::now());
        view->draw_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(view->item_rects().contains("editor_spell:1:4"));
    CHECK_FALSE(view->item_rects().contains("editor_spell:1:0"));
}

TEST_CASE_FIXTURE(WindowFixture, "editor: code notes are not spell checked") {
    add_note("kod.py", "kotu = 1\n");
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    CHECK_FALSE(has_rect_with_prefix(*view, "editor_spell:"));
}
#endif
