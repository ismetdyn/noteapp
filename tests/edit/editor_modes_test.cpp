// tui::EditorModes'un doğrudan testleri: kip kararları sahipten (EditorSession, NoteEditor) bağımsız.
#include "edit/editor_modes.hpp"

#include <doctest/doctest.h>
#include <string>

using noteapp::tui::EditorBuffer;
using noteapp::tui::EditorModes;
using noteapp::tui::Key;

namespace {

int plain_width(char32_t) {
    return 1;
}

}  // namespace

TEST_CASE("modes: a read-only buffer never takes the ghost suggestion") {
    EditorModes modes({});
    EditorBuffer buffer("alphabet al", plain_width);
    buffer.move_doc_end(false);
    modes.update_ghost(buffer, true, "md", nullptr);
    REQUIRE(modes.ghost() == "phabet");
    CHECK_FALSE(modes.accept_ghost(buffer, Key::Tab, false));
    CHECK(buffer.text() == "alphabet al");
    CHECK(modes.ghost().empty());  // her tuş öneriyi düşürür
    modes.update_ghost(buffer, false, "md", nullptr);
    CHECK(modes.ghost().empty());  // salt-okunurda öneri de üretilmez
}

TEST_CASE("modes: F9 in the find input is ignored so the owner can act on it") {
    EditorModes modes({});
    EditorBuffer buffer("alpha", plain_width);
    modes.open_find(buffer, "md", nullptr);
    CHECK(modes.handle_find(buffer, Key::F9, {}) == EditorModes::FindResult::Ignored);
}

TEST_CASE("modes: Backspace in the find suggestion list edits the list, not the search") {
    EditorModes modes({});
    EditorBuffer buffer("x alpha alpine", plain_width);
    modes.open_find(buffer, "md", nullptr);
    REQUIRE(modes.handle_find(buffer, Key::Char, "a") == EditorModes::FindResult::Searched);
    REQUIRE(modes.handle_find(buffer, Key::Char, "l") == EditorModes::FindResult::Searched);
    const std::size_t cursor = buffer.cursor();
    const std::string label = modes.find_label();
    REQUIRE(modes.handle_find(buffer, Key::Tab, {}) == EditorModes::FindResult::Handled);
    REQUIRE(modes.find_line().popup() != nullptr);
    CHECK(modes.handle_find(buffer, Key::Backspace, {}) == EditorModes::FindResult::Handled);
    CHECK(modes.find_line().text() == "al");
    CHECK(buffer.cursor() == cursor);  // arama yeniden koşmadı
    CHECK(modes.find_label() == label);
}

TEST_CASE("modes: Backspace on an empty find query still searches") {
    EditorModes modes({});
    EditorBuffer buffer("alpha", plain_width);
    modes.open_find(buffer, "md", nullptr);
    CHECK(modes.handle_find(buffer, Key::Backspace, {}) == EditorModes::FindResult::Searched);
    CHECK_FALSE(modes.find_missed());
}
