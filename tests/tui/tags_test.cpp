// T80: sağ panelde notun etiketleri, paletten etikete göre süzme, `tag:` tamamlaması.
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/input_line.hpp"
#include "tui/link_panel.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Key;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

std::string screen_text(const tui::TuiState& state) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, {}, {"", ""}, {kWidth, kHeight}));
    std::string out;
    for (int y = 0; y < kHeight; ++y) {
        std::string row;
        for (int x = 0; x < kWidth; ++x)
            row += screen.PixelAt(x, y).character;
        out += row + "\n";
    }
    return out;
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

tui::TuiState selected_state() {
    core::init_i18n("en");
    tui::TuiState state;
    state.set_notes({core::NoteInfo{np("a.md"), std::filesystem::file_time_type{}, 1},
                     core::NoteInfo{np("b.md"), std::filesystem::file_time_type{}, 1}});
    state.set_window(kWidth, kHeight);
    REQUIRE(state.select_note(np("a.md")));
    return state;
}

void aim(tui::TuiState& state, std::string_view id) {
    for (std::size_t step = 0; step < 200; ++step) {
        const tui::Command* selected = state.palette_selected();
        REQUIRE(selected != nullptr);
        if (selected->id == id)
            return;
        state.handle(Key::Down);
    }
    FAIL("palette has no row: ", id);
}

}  // namespace

TEST_CASE("tags: a note's tags show on one line under the preview, without links too") {
    tui::TuiState state = selected_state();
    core::NoteLinks links;
    links.tags = {"b", "Proje"};
    state.set_links(links, 1, np("a.md"));
    CHECK(state.links_reserved_rows(25) == 1);
    const std::string shown = screen_text(state);
    CHECK(has(shown, "#b #Proje"));
    CHECK_FALSE(has(shown, "Incoming"));

    state.set_links({}, 2, np("a.md"));
    CHECK(state.links_reserved_rows(25) == 0);
    CHECK_FALSE(has(screen_text(state), "#b"));
}

TEST_CASE("tags: with links the tag line is the section separator and items keep their rows") {
    tui::TuiState state = selected_state();
    core::NoteLinks links;
    links.outgoing = {np("b.md")};
    links.tags = {"iş"};
    state.set_links(links, 1, np("a.md"));
    tui::LinkPanel plain;
    plain.set(core::NoteLinks{{np("b.md")}, {}, {}, {}}, false);
    CHECK(state.links_reserved_rows(25) == plain.reserved_rows(25));
    const std::string shown = screen_text(state);
    CHECK(has(shown, "#iş"));
    CHECK(has(shown, "Outgoing (1)"));
}

TEST_CASE("tags: the store's tags reach the panel with the deferred preview read") {
    test::TempDir dir;
    core::NoteStore store(dir.path());
    REQUIRE(test::seed_note(store, "a.md", "#Proje metin\n"));
    tui::TuiState state = selected_state();
    state.set_notes(store.list());
    REQUIRE(state.select_note(np("a.md")));
    tui::PreviewCache preview;
    preview.throttled(state, store, std::chrono::steady_clock::now() + std::chrono::hours(1));
    CHECK(state.link_panel().links().tags == std::vector<std::string>{"Proje"});
}

TEST_CASE("tags: the palette's tag filter opens the name search with tag: ready") {
    tui::TuiState state = selected_state();
    state.handle(Key::Char, ":");
    aim(state, "search.tag");
    state.handle(Key::Enter);
    CHECK(state.focus() == Focus::Search);
    CHECK(state.query() == "tag:");
}

TEST_CASE("tags: tag: completes from the notes' tags and every offered value parses") {
    const std::vector<core::NoteMeta> notes = {
        core::NoteMeta{.path = np("a.md"), .tags = {"proje", "proje/alt"}},
        core::NoteMeta{.path = np("b.md"), .tags = {"çalışma", "proje"}},
    };
    tui::InputLine line(tui::InputKind::Search);
    line.set_sources(tui::build_input_sources(notes));
    line.set_text("tag:");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    std::vector<std::string> items = line.popup()->items;
    std::ranges::sort(items);
    CHECK(items == std::vector<std::string>{"tag:proje", "tag:proje/alt", "tag:çalışma"});
}

TEST_CASE("tags: a palette tag filter narrows the list to tagged notes") {
    tui::TuiState state;
    state.set_notes(
        {core::NoteInfo{np("a.md"), std::filesystem::file_time_type{}, 1, {}, {}, {}, 0, 0, {"x"}},
         core::NoteInfo{np("b.md"), std::filesystem::file_time_type{}, 1}});
    state.handle(Key::Char, "/");
    for (const char c : std::string("tag:x"))
        state.handle(Key::Char, std::string(1, c));
    REQUIRE(state.rows().size() == 1);
    CHECK(state.rows()[0].path == np("a.md"));
}
