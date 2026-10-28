#include "tui/tui_state.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Intent;
using tui::Key;
using tui::Mode;

namespace {

core::NoteInfo note(const std::string& path, int age_hours = 0) {
    // Büyük age_hours = eski; boş sorguda sıralama mtime azalan olduğundan test düzeni buna dayanır.
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(1000 - age_hours), 1, false};
}

std::vector<core::NoteInfo> sample() {
    return {note("work/meeting.md", 0), note("personal/todo.md", 1), note("snippets/sort.java", 2)};
}

void type(tui::TuiState& s, std::string_view text) {
    for (char c : text)
        s.handle(Key::Char, std::string_view(&c, 1));
}

std::string selected_path(const tui::TuiState& s) {
    return s.selected_row() ? s.selected_row()->path.string() : "";
}

}  // namespace

TEST_CASE("empty query lists all notes newest first with the first selected") {
    tui::TuiState s;
    s.set_notes(sample());
    CHECK(s.rows().size() == 3);
    CHECK(s.total() == 3);
    CHECK(selected_path(s) == "work/meeting.md");
    CHECK(s.mode() == Mode::Name);
}

TEST_CASE("typing filters the list and keeps match positions for highlighting") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "tod");
    REQUIRE(s.rows().size() == 1);
    CHECK(selected_path(s) == "personal/todo.md");
    CHECK(s.rows()[0].positions.size() == 3);
    CHECK(s.query() == "tod");
}

TEST_CASE("backspace removes a whole UTF-8 character") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "a");
    s.handle(Key::Char, "ş");
    CHECK(s.query() == "aş");
    s.handle(Key::Backspace);
    CHECK(s.query() == "a");
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);  // boşken sessiz
    CHECK(s.query().empty());
    CHECK(s.rows().size() == 3);
}

TEST_CASE("navigation clamps at both ends and Ctrl-P/N mirror the arrows") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Up);
    CHECK(s.selected() == 0);
    s.handle(Key::Down);
    s.handle(Key::CtrlN);
    CHECK(s.selected() == 2);
    s.handle(Key::Down);
    CHECK(s.selected() == 2);
    s.handle(Key::CtrlP);
    CHECK(s.selected() == 1);
}

TEST_CASE("selection stays on the same note when the list refilters, else resets to 0") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Down);  // personal/todo.md
    type(s, "o");         // üçü de 'o' içerir
    CHECK(selected_path(s) == "personal/todo.md");
    type(s, "rk");  // yalnız work/meeting.md kalır; seçili not düştü
    CHECK(selected_path(s) == "work/meeting.md");
    CHECK(s.selected() == 0);
}

TEST_CASE("set_notes keeps the selection by path and clamps when the note vanished") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Down);
    s.handle(Key::Down);  // snippets/sort.java
    auto more = sample();
    more.insert(more.begin(), note("new.md", -5));  // en yeni: sıra kayar
    s.set_notes(more);
    CHECK(selected_path(s) == "snippets/sort.java");
    s.set_notes({note("only.md")});
    CHECK(selected_path(s) == "only.md");
    s.set_notes({});
    CHECK(s.selected_row() == nullptr);
    CHECK(s.rows().empty());
}

TEST_CASE("Esc clears a non-empty query, quits on an empty one; Ctrl-C always quits") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "tod");
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.query().empty());
    CHECK(s.rows().size() == 3);
    CHECK(s.handle(Key::Escape) == Intent::Quit);
    type(s, "x");
    CHECK(s.handle(Key::CtrlC) == Intent::Quit);
}

TEST_CASE("Enter opens the selection, creates from the query when nothing matches, else does nothing") {
    tui::TuiState s;
    CHECK(s.handle(Key::Enter) == Intent::None);  // boş liste, boş sorgu
    s.set_notes(sample());
    CHECK(s.handle(Key::Enter) == Intent::OpenSelected);
    type(s, "zzzqq");
    CHECK(s.rows().empty());
    CHECK(s.handle(Key::Enter) == Intent::CreateFromQuery);
}

TEST_CASE("action keys map to intents; selection-bound ones need a selection") {
    tui::TuiState s;
    CHECK(s.handle(Key::CtrlR) == Intent::None);
    CHECK(s.handle(Key::CtrlD) == Intent::None);
    CHECK(s.handle(Key::CtrlO) == Intent::NewNote);
    CHECK(s.handle(Key::CtrlS) == Intent::SyncNow);
    CHECK(s.handle(Key::F1) == Intent::ShowHelp);
    CHECK(s.handle(Key::F2) == Intent::ShowSettings);
    s.set_notes(sample());
    CHECK(s.handle(Key::CtrlR) == Intent::MoveSelected);
    CHECK(s.handle(Key::CtrlD) == Intent::DeleteSelected);
}

TEST_CASE("PageUp/PageDown scroll the preview within bounds and reset on selection change") {
    tui::TuiState s;
    s.set_notes(sample());
    s.set_preview_line_count(15);
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == 10);
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == 14);  // son satır
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 4);
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::PageDown);
    s.handle(Key::Down);
    CHECK(s.preview_scroll() == 0);
}

namespace {

// Sahte içerik araması: sorgu ne olursa olsun todo.md (3 eşleşme, ilk satır 12) sonra sort.java (1) döner.
tui::ContentSearcher fake_searcher(std::vector<std::string>* seen_queries = nullptr) {
    return [seen_queries](std::string_view query) {
        if (seen_queries)
            seen_queries->emplace_back(query);
        std::vector<search::ContentResult> results;
        results.push_back({core::NotePath::parse("personal/todo.md").value(),
                           3,
                           {search::Hit{12, "buy milk", {{4, 8}}}, search::Hit{20, "milk again", {{0, 4}}}}});
        results.push_back({core::NotePath::parse("snippets/sort.java").value(),
                           1,
                           {search::Hit{1, "milk sort", {{0, 4}}}}});
        return results;
    };
}

}  // namespace

TEST_CASE("Tab toggles name and content mode") {
    tui::TuiState s;
    s.set_notes(sample());
    s.set_content_searcher(fake_searcher());
    CHECK(s.mode() == Mode::Name);
    CHECK(s.handle(Key::Tab) == Intent::None);
    CHECK(s.mode() == Mode::Content);
    s.handle(Key::Tab);
    CHECK(s.mode() == Mode::Name);
}

TEST_CASE("content mode rows come from the searcher with hit counts, only for a non-empty query") {
    tui::TuiState s;
    s.set_notes(sample());
    std::vector<std::string> seen;
    s.set_content_searcher(fake_searcher(&seen));
    s.handle(Key::Tab);
    CHECK(s.rows().size() == 3);  // boş sorgu: tüm notlar, arama çağrılmaz
    CHECK(seen.empty());
    type(s, "milk");
    REQUIRE(s.rows().size() == 2);
    CHECK(s.rows()[0].path.string() == "personal/todo.md");
    CHECK(s.rows()[0].hit_count == 3);
    CHECK(s.rows()[1].hit_count == 1);
    CHECK_FALSE(seen.empty());
    CHECK(seen.back() == "milk");
    CHECK(s.total() == 3);
}

TEST_CASE("content mode scrolls the preview to the first hit and follows the selection") {
    tui::TuiState s;
    s.set_notes(sample());
    s.set_content_searcher(fake_searcher());
    s.handle(Key::Tab);
    type(s, "milk");
    // 12. satır, 2 satır bağlamla: 0 tabanlı 11 - 2
    CHECK(s.preview_scroll() == 9);
    s.handle(Key::Down);  // hit satırı 1: bağlam sıfırın altına inmez
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::Up);
    CHECK(s.preview_scroll() == 9);
}

TEST_CASE("switching back to name mode restores the fuzzy list and resets the scroll") {
    tui::TuiState s;
    s.set_notes(sample());
    s.set_content_searcher(fake_searcher());
    s.handle(Key::Tab);
    type(s, "tod");
    s.handle(Key::Tab);
    CHECK(s.mode() == Mode::Name);
    REQUIRE(s.rows().size() == 1);
    CHECK(s.rows()[0].hit_count == 0);
    CHECK(s.rows()[0].positions.size() == 3);
    CHECK(s.preview_scroll() == 0);
}

TEST_CASE("set_notes re-runs the content search in content mode") {
    tui::TuiState s;
    s.set_notes(sample());
    std::vector<std::string> seen;
    s.set_content_searcher(fake_searcher(&seen));
    s.handle(Key::Tab);
    type(s, "milk");
    const auto before = seen.size();
    s.set_notes(sample());
    CHECK(seen.size() == before + 1);
}

TEST_CASE("content mode without a searcher yields no matches instead of crashing") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Tab);
    type(s, "milk");
    CHECK(s.rows().empty());
}

TEST_CASE("same_listing detects added, changed and reordered notes") {
    const auto a = sample();
    auto b = sample();
    CHECK(tui::same_listing(a, b));
    b[1].size = 99;
    CHECK_FALSE(tui::same_listing(a, b));
    b = sample();
    b.pop_back();
    CHECK_FALSE(tui::same_listing(a, b));
}

TEST_CASE("reveal clears the query and selects the given note") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "todo");
    const auto target = core::NotePath::parse("snippets/sort.java").value();
    CHECK(s.reveal(target));
    CHECK(s.query().empty());
    CHECK(s.mode() == Mode::Name);
    CHECK(selected_path(s) == "snippets/sort.java");
    CHECK_FALSE(s.reveal(core::NotePath::parse("yok.md").value()));
}
