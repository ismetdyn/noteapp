// G5a: GUI editörü EditorBuffer üstünde kendi alanı (TUI ile aynı tampon, geri alma, imleç, seçim). Testler
// gerçek kareler, gerçek tuş ve fare olaylarıyla çalışır; veri bütünlüğü diske yazılan baytlardan okunur.
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/theme.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <imgui.h>
#include <string>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::View;

namespace {

// Seçilen not incelemede açılır (TUI REVIEW): düzenleme açık eylemle başlar; taslak zaten düzenlemededir.
void focus_editor(GuiState& state, View& view) {
    state.edit_selected();
    state.request_focus(FocusRequest::Editor);
    settle(view);
    REQUIRE(view.editor_active());
}

void mouse_to(View& view, float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    view.draw_frame();
}

void mouse_button(View& view, bool down) {
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    view.draw_frame();
}

float center_y(const View::ItemRect& rect) {
    return (rect.min_y + rect.max_y) / 2.0F;
}

std::string status_fields(const std::string& tail) {
    return "UTF-8  " + tail;
}

}  // namespace

TEST_CASE_FIXTURE(WindowFixture, "editor: line numbers are drawn for the visible lines only") {
    std::string text;
    for (int line = 1; line <= 3000; ++line)
        text += "line " + std::to_string(line) + "\n";
    add_note("long.txt", text);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    const auto& rects = view->item_rects();
    REQUIRE(rects.contains("editor"));
    CHECK(rects.contains("editor_line:1"));
    CHECK(rects.contains("editor_line:2"));
    CHECK_FALSE(rects.contains("editor_line:2999"));  // yalnız görünen satırlar çizilir
    // Numaralar metnin solunda, editörün içinde.
    CHECK(rects.at("editor_line:1").min_x >= rects.at("editor").min_x);
    CHECK(rects.at("editor_line:2").min_y > rects.at("editor_line:1").min_y);
}

TEST_CASE_FIXTURE(WindowFixture, "editor: the status bar shows type, encoding, line ending, BOM and line:column") {
    add_note("crlf.md", "\xEF\xBB\xBF" "ab\r\ncd\r\n");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    const std::string lines = noteapp::core::tr("tui.status_lines", {{"count", "3"}});
    {
        const gui_test::RenderedText rendered;
        settle(*view);
        CHECK(rendered.contains("md  " + status_fields("CRLF  BOM  1:1  " + lines)));
    }
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_End);
    const gui_test::RenderedText rendered;
    settle(*view);
    CHECK(rendered.contains(status_fields("CRLF  BOM  2:3  " + lines)));
}

TEST_CASE_FIXTURE(WindowFixture, "editor: Ctrl+F finds folded matches, F3 and Shift+F3 step, Escape keeps the match") {
    add_note("find.md", "alfa Beta\nbeta gama\nBETA");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);

    press(*view, ImGuiMod_Ctrl | ImGuiKey_F);
    type_text(*view, "beta");
    REQUIRE(state->editor_selection().has_value());
    CHECK(state->editor_selection()->start == 5);
    {
        const gui_test::RenderedText rendered;
        settle(*view);
        CHECK(rendered.contains("1/3"));
    }
    press(*view, ImGuiKey_F3);
    CHECK(state->editor_selection()->start == 10);
    press(*view, ImGuiKey_F3);
    CHECK(state->editor_selection()->start == 20);
    press(*view, ImGuiKey_F3);  // başa sarar
    CHECK(state->editor_selection()->start == 5);
    press(*view, ImGuiMod_Shift | ImGuiKey_F3);
    CHECK(state->editor_selection()->start == 20);
    press(*view, ImGuiKey_Escape);  // bul kapanır, eşleşme seçili kalır (TUI)
    CHECK(view->editor_active());
    type_text(*view, "X");
    CHECK(state->buffer() == "alfa Beta\nbeta gama\nX");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a find without a match says so and moves nothing") {
    add_note("find.md", "alfa");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_F);
    type_text(*view, "zz");
    const gui_test::RenderedText rendered;
    settle(*view);
    CHECK(rendered.contains(noteapp::core::tr("tui.editor_not_found")));
    CHECK_FALSE(state->editor_selection().has_value());
    CHECK(state->buffer() == "alfa");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: undo and redo come from the buffer, a typing run is one step") {
    add_note("undo.md", "base");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    type_text(*view, "abc");
    REQUIRE(state->buffer() == "abcbase");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Z);
    CHECK(state->buffer() == "base");
    CHECK_FALSE(state->dirty());  // kayıtlı metne dönmek temizdir (TUI)
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Y);
    CHECK(state->buffer() == "abcbase");
    CHECK(state->dirty());
}

TEST_CASE_FIXTURE(WindowFixture, "editor: each tab keeps its own text and undo history") {
    add_note("a.md", "A", 20);
    add_note("b.md", "B", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("a.md")));
    focus_editor(*state, *view);
    const auto tab_a = state->active_tab();
    type_text(*view, "1");
    REQUIRE(state->buffer() == "1A");

    state->open_in_new_tab(*noteapp::core::NotePath::parse("b.md"));
    CHECK(read_note("a.md") == "1A");  // sekmeden ayrılırken kendi notuna yazıldı
    focus_editor(*state, *view);
    type_text(*view, "2");
    CHECK(state->buffer() == "2B");

    REQUIRE(state->activate_tab(tab_a));
    CHECK(read_note("b.md") == "2B");
    focus_editor(*state, *view);
    CHECK(state->buffer() == "1A");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Z);  // geri alma geçmişi sekmeyle döndü
    CHECK(state->buffer() == "A");
    state->save_now();
    CHECK(read_note("a.md") == "A");
    CHECK(read_note("b.md") == "2B");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a click places the cursor and a drag selects across lines") {
    add_note("mouse.md", "first line\nsecond line\nthird");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    const View::ItemRect editor = view->item_rects().at("editor");
    const View::ItemRect line1 = view->item_rects().at("editor_line:1");
    const View::ItemRect line2 = view->item_rects().at("editor_line:2");
    const View::ItemRect line3 = view->item_rects().at("editor_line:3");

    mouse_to(*view, editor.max_x - 16.0F, center_y(line2));  // satırın ötesi: satır sonu
    mouse_button(*view, true);
    mouse_button(*view, false);
    type_text(*view, "!");
    CHECK(state->buffer() == "first line\nsecond line!\nthird");

    mouse_to(*view, line1.max_x + 1.0F, center_y(line1));  // ilk hücrenin solu: satır başı
    mouse_button(*view, true);
    mouse_to(*view, editor.max_x - 16.0F, center_y(line3));
    mouse_button(*view, false);
    type_text(*view, "Z");
    CHECK(state->buffer() == "Z");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: autosave writes the typed text to its own note, never to the next one") {
    add_note("a.md", "A", 20);
    add_note("b.md", "B", 10);
    auto state = make_state();
    auto view = make_view(*state);
    state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("a.md")));
    focus_editor(*state, *view);
    type_text(*view, "x");
    REQUIRE(state->next_deadline().has_value());
    const auto deadline = *state->next_deadline();

    state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("b.md")));
    settle(*view);
    REQUIRE(state->buffer() == "B");
    state->tick(deadline);
    CHECK(read_note("a.md") == "xA");
    CHECK(read_note("b.md") == "B");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a CRLF note with a BOM keeps its bytes when edited and saved") {
    add_note("win.md", "\xEF\xBB\xBF" "one\r\ntwo\r\n");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_End);
    type_text(*view, "x");
    press(*view, ImGuiKey_Enter);
    type_text(*view, "y");
    press(*view, ImGuiMod_Ctrl | ImGuiKey_S);
    CHECK(read_note("win.md") == "\xEF\xBB\xBF" "one\r\ntwo\r\nx\r\ny");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a clean note changed on disk reloads and keeps the cursor line") {
    add_note("alpha.md", "one\ntwo\nthree\n", 10);
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_DownArrow);

    add_note("alpha.md", "ONE\nTWO\nTHREE\n", 0);
    state->rescan();
    settle(*view);
    REQUIRE(state->buffer() == "ONE\nTWO\nTHREE\n");
    CHECK(view->editor_active());
    type_text(*view, "x");
    CHECK(state->buffer() == "ONE\nTWO\nxTHREE\n");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a dirty buffer is never replaced by a change on disk") {
    add_note("alpha.md", "old", 10);
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    type_text(*view, "mine ");
    add_note("alpha.md", "theirs", 0);
    state->rescan();
    settle(*view);
    CHECK(state->buffer() == "mine old");
    CHECK(read_note("alpha.md") == "theirs");
}

TEST_CASE_FIXTURE(WindowFixture, "editor: the hidden character under the cursor is named in the status bar") {
    add_note("bidi.md", "a\xE2\x80\xAE" "b");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    press(*view, ImGuiKey_RightArrow);
    const gui_test::RenderedText rendered;
    settle(*view);
    CHECK(rendered.contains("U+202E"));
}

TEST_CASE_FIXTURE(WindowFixture, "editor: a focused editor asks the platform for text input (IME, SDL text events)") {
    add_note("alpha.md", "a");
    auto state = make_state();
    auto view = make_view(*state);
    focus_editor(*state, *view);
    CHECK(ImGui::GetIO().WantTextInput);
    type_text(*view, "ğüşİı");
    CHECK(state->buffer() == "ğüşİıa");
}

// G9: geçerli satır doygun mavi bant değil, metin renginin %4'ü; oluk ile metin arasında 12 px; metin tuvalin
// kenarına yapışmaz (üstte 8, solda 4 px).
TEST_CASE_FIXTURE(WindowFixture,
                  "editor: the current line is a faint tint and the gutter keeps 12 px to the text") {
    add_note("a.txt", "first\nsecond\n");
    auto state = make_state();
    auto view = make_view(*state);
    noteapp::gui::theme::apply_colors(ImGui::GetStyle(), noteapp::gui::theme::kDark);
    ImGui::GetIO().AddMousePosEvent(-1.0F, -1.0F);
    settle(*view);
    const auto& rects = view->item_rects();
    const View::ItemRect editor = rects.at("editor");
    const View::ItemRect cursor = rects.at("editor_cursor");
    const View::ItemRect line = rects.at("editor_line:1");
    const float cell = cursor.max_x - cursor.min_x;
    CHECK(line.max_x - line.min_x - cell == doctest::Approx(12.0F));  // tek rakam ve 12 px boşluk
    CHECK(cursor.min_y - editor.min_y == doctest::Approx(8.0F));
    CHECK(line.min_x - editor.min_x >= 4.0F);
    // Satır sonundan sonra yalnız zemin: tuval (#1C1F24) üstünde metin (#DDE1E7) %4 ≈ #23262B.
    const std::uint32_t tint =
        gui_test::pixel_at((cursor.max_x + editor.max_x) / 2.0F, (cursor.min_y + cursor.max_y) / 2.0F);
    constexpr std::uint32_t kTint = 0x23262B;  // 8 bitlik karışım (yazılımsal çizici) aşağı yuvarlar
    constexpr int kChannelTolerance = 1;
    for (const int shift : {16, 8, 0}) {
        CAPTURE(shift);
        const int got = static_cast<int>((tint >> shift) & 0xFFU);
        const int want = static_cast<int>((kTint >> shift) & 0xFFU);
        CHECK(std::abs(got - want) <= kChannelTolerance);
    }
}
