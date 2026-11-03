// T81: paletten "Run view" kayıtlı görünümleri listeler ve `view:AD` süzgecini kurar; arama satırında
// `view:` tamamlaması; bilinmeyen görünüm diğer bozuk argümanlar gibi kırmızı hatadır.
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "edit/input_line.hpp"
#include "search/query.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Key;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

// Depo yerine: `work` b.md'yi döndürür, `Ev Notları` a.md'yi, kalanı yok.
search::ViewLookup fake_views() {
    return [](std::string_view name) -> std::expected<std::vector<core::NotePath>, core::Error> {
        if (name == "work")
            return std::vector{np("b.md")};
        if (name == "Ev Notları")
            return std::vector{np("a.md")};
        return std::unexpected(
            core::Error{core::Error::Code::NotFound, "error.view_not_found", {{"name", std::string(name)}}});
    };
}

tui::TuiState view_state() {
    core::init_i18n("en");
    tui::TuiState state;
    state.set_view_lookup(fake_views());
    state.set_views({"Ev Notları", "work"});
    state.set_notes({core::NoteInfo{np("a.md"), std::filesystem::file_time_type{}, 1},
                     core::NoteInfo{np("b.md"), std::filesystem::file_time_type{}, 1},
                     core::NoteInfo{np("c.md"), std::filesystem::file_time_type{}, 1}});
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

void type(tui::TuiState& state, std::string_view text) {
    for (const char c : text)
        state.handle(Key::Char, std::string(1, c));
}

std::vector<core::NotePath> row_paths(const tui::TuiState& state) {
    std::vector<core::NotePath> out;
    for (const auto& row : state.rows())
        out.push_back(row.path);
    return out;
}

}  // namespace

TEST_CASE("views: the palette's Run view lists saved views and the pick filters the list") {
    tui::TuiState state = view_state();
    state.handle(Key::Char, ":");
    aim(state, "search.view");
    state.handle(Key::Enter);
    CHECK(state.focus() == Focus::Search);
    CHECK(state.query() == "view:");
    const auto* popup = state.search_line().popup();
    REQUIRE(popup != nullptr);
    std::vector<std::string> items = popup->items;
    std::ranges::sort(items);
    CHECK(items == std::vector<std::string>{"view:Ev Notları", "view:work"});

    type(state, "wrk");  // fzf gibi süzer
    REQUIRE(state.search_line().popup() != nullptr);
    CHECK(state.search_line().popup()->items == std::vector<std::string>{"view:work"});
    state.handle(Key::Enter);
    CHECK(state.query() == "view:work");
    CHECK(row_paths(state) == std::vector{np("b.md")});
    CHECK_FALSE(state.query_error().has_value());
}

TEST_CASE("views: a view name with a space is quoted and still filters") {
    tui::TuiState state = view_state();
    state.handle(Key::Char, "/");
    type(state, "view:Ev");
    state.handle(Key::Tab);
    CHECK(state.query() == "view:\"Ev Notları\"");
    CHECK(row_paths(state) == std::vector{np("a.md")});
}

TEST_CASE("views: an unknown view is an argument error and the list keeps its last valid rows") {
    tui::TuiState state = view_state();
    state.handle(Key::Char, "/");
    type(state, "view:work");
    REQUIRE(row_paths(state) == std::vector{np("b.md")});
    type(state, "x");
    REQUIRE(state.query_error().has_value());
    CHECK(core::format_error(state.query_error()->error).find("workx") != std::string::npos);
    CHECK(row_paths(state) == std::vector{np("b.md")});
}

TEST_CASE("views: view: completes from the saved views and every offered value parses") {
    tui::InputLine line(tui::InputKind::Search);
    auto sources = std::make_shared<tui::InputSources>();
    sources->views = {"Ev Notları", "work"};
    line.set_sources(sources);
    line.set_text("view:");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    for (const std::string& item : line.popup()->items) {
        CAPTURE(item);
        CHECK(search::parse_query(item).has_value());
    }
    line.set_text("vi");
    line.handle(Key::Tab);
    CHECK(line.text() == "view:");
}
