#include "core/config.hpp"
#include "edit/clipboard.hpp"
#include "edit/completion.hpp"
#include "edit/editor_buffer.hpp"
#include "edit/input_line.hpp"
#include "edit/language_servers.hpp"
#include "edit/spellcheck.hpp"
#include "search/fuzzy.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/styled_view.hpp"
#include "tui/tui_state.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;
using tui::Mode;

namespace {

core::NoteInfo note(const std::string& path, int age_hours = 0) {
    // Büyük age_hours = eski; boş sorguda sıralama mtime azalan olduğundan test düzeni buna dayanır.
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(1000 - age_hours), 1};
}

std::vector<core::NoteInfo> sample() {
    return {note("work-meeting.md", 0), note("personal-todo.md", 1), note("snippets-sort.java", 2)};
}

// Açılış odağı Liste: yazı ancak `/` ile Arama'ya geçince sorguya gider.
void type(tui::TuiState& s, std::string_view text) {
    if (s.focus() != Focus::Search)
        s.handle(Key::Char, "/");
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
    CHECK(selected_path(s) == "work-meeting.md");
    CHECK(s.mode() == Mode::Name);
}

TEST_CASE("typing filters the list and keeps match positions for highlighting") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "tod");
    REQUIRE(s.rows().size() == 1);
    CHECK(selected_path(s) == "personal-todo.md");
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
    s.handle(Key::Down);  // personal-todo.md
    type(s, "o");         // üçü de 'o' içerir
    CHECK(selected_path(s) == "personal-todo.md");
    type(s, "rk");  // yalnız work-meeting.md kalır; seçili not düştü
    CHECK(selected_path(s) == "work-meeting.md");
    CHECK(s.selected() == 0);
}

TEST_CASE("set_notes keeps the selection by path and clamps when the note vanished") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Down);
    s.handle(Key::Down);  // snippets-sort.java
    auto more = sample();
    more.insert(more.begin(), note("new.md", -5));  // en yeni: sıra kayar
    s.set_notes(more);
    CHECK(selected_path(s) == "snippets-sort.java");
    s.set_notes({note("only.md")});
    CHECK(selected_path(s) == "only.md");
    s.set_notes({});
    CHECK(s.selected_row() == nullptr);
    CHECK(s.rows().empty());
}

TEST_CASE("Esc leaves Search keeping the query, then clears it, then stays; Ctrl-C always quits") {
    tui::TuiState s;
    s.set_notes(sample());
    type(s, "tod");
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.focus() == Focus::List);
    CHECK(s.query() == "tod");
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.query().empty());
    CHECK(s.rows().size() == 3);
    CHECK(s.handle(Key::Escape) == Intent::None);
    type(s, "x");
    CHECK(s.handle(Key::CtrlC) == Intent::Quit);
}

TEST_CASE("Enter edits the selection, creates from the query when nothing matches, else does nothing") {
    tui::TuiState s;
    CHECK(s.handle(Key::Enter) == Intent::None);  // boş liste, boş sorgu
    s.set_notes(sample());
    CHECK(s.handle(Key::Enter) == Intent::EnterEditor);
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
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    s.set_notes(sample());
    CHECK(s.handle(Key::CtrlR) == Intent::MoveSelected);
    CHECK(s.handle(Key::CtrlD) == Intent::TrashSelected);
}

TEST_CASE("PageUp/PageDown scroll the preview within bounds and reset on selection change") {
    tui::TuiState s;
    s.set_notes(sample());
    s.set_preview_line_count(15);
    s.handle(Key::Right);  // yalnız REVIEW odağında: Listede sayfa listeyi kaydırır
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == 10);
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == 14);  // son satır
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 4);
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::PageDown);
    s.handle(Key::Left);
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
        results.push_back({core::NotePath::parse("personal-todo.md").value(),
                           3,
                           {search::Hit{12, "buy milk", {{4, 8}}}, search::Hit{20, "milk again", {{0, 4}}}}});
        results.push_back({core::NotePath::parse("snippets-sort.java").value(),
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
    CHECK(s.rows()[0].path.string() == "personal-todo.md");
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
    s.handle(Key::CtrlT);
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

// app::Listing kuralı (plan-parity A2): genel arama yalnız bağlı aramacıyla; uygulama onu hep bağlar.
TEST_CASE("global mode without a searcher yields no matches instead of falling back to names") {
    tui::TuiState s;
    s.set_notes(sample());
    s.handle(Key::Char, "f");
    REQUIRE(s.mode() == Mode::Global);
    type(s, "tod");
    CHECK(s.rows().empty());
}

// app::Listing kuralı (plan-parity A2): bozuk argümanda eski satırlar kalmaz; son geçerli sorgu güncel
// listeden yeniden kurulur, gitmiş not görünmez.
TEST_CASE("an invalid argument rebuilds the last valid query on the current listing") {
    tui::TuiState s;
    s.set_notes({note("size-plan.md", 0), note("size-old.md", 1), note("other.md", 2)});
    type(s, "size:abc");
    REQUIRE(s.query_error());
    REQUIRE(s.rows().size() == 2);
    s.set_notes({note("size-plan.md", 0), note("other.md", 2)});
    CHECK(s.query_error());
    REQUIRE(s.rows().size() == 1);
    CHECK(selected_path(s) == "size-plan.md");
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
    const auto target = core::NotePath::parse("snippets-sort.java").value();
    CHECK(s.reveal(target));
    CHECK(s.query().empty());
    CHECK(s.mode() == Mode::Name);
    CHECK(selected_path(s) == "snippets-sort.java");
    CHECK_FALSE(s.reveal(core::NotePath::parse("yok.md").value()));
}

// ---- T51/T52: odak durumları (Liste / Arama / Editör) ----

namespace {

// Odak başına kurulum: Liste açılış, `/` Arama, `→` Önizleme; Editör odağını oturum notu açınca verir.
tui::TuiState in_focus(Focus focus, std::vector<core::NoteInfo> notes = sample()) {
    tui::TuiState s;
    s.set_notes(std::move(notes));
    if (focus == Focus::Search)
        s.handle(Key::Char, "/");
    if (focus == Focus::Preview)
        s.handle(Key::Right);
    if (focus == Focus::Editor)
        s.enter_editor();
    REQUIRE(s.focus() == focus);
    return s;
}

}  // namespace

TEST_CASE("the app opens in List focus") {
    tui::TuiState s;
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("List: k/j and arrows move the selection and clamp") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Char, "k") == Intent::None);
    CHECK(s.selected() == 0);
    CHECK(s.handle(Key::Char, "j") == Intent::None);
    CHECK(s.selected() == 1);
    s.handle(Key::Down);
    CHECK(s.selected() == 2);
    s.handle(Key::Char, "j");
    CHECK(s.selected() == 2);
    s.handle(Key::Up);
    CHECK(s.selected() == 1);
    s.handle(Key::Char, "k");
    CHECK(s.selected() == 0);
    CHECK(s.focus() == Focus::List);
    CHECK(s.query().empty());
}

TEST_CASE("List: gg and G jump to the first and last note") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Char, "G") == Intent::None);
    CHECK(s.selected() == 2);
    CHECK(s.handle(Key::Char, "g") == Intent::None);
    CHECK(s.selected() == 2);
    CHECK(s.handle(Key::Char, "g") == Intent::None);
    CHECK(s.selected() == 0);
}

TEST_CASE("List: PgUp/PgDn page the list, leave the preview alone and keep the focus") {
    auto s = in_focus(Focus::List);
    s.set_preview_line_count(30);
    s.handle(Key::PageDown);
    CHECK(s.selected() == s.rows().size() - 1);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::PageUp);
    CHECK(s.selected() == 0);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("List: right arrow and l go to Preview and never open the editor") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Right) == Intent::None);
    CHECK(s.focus() == Focus::Preview);
    auto t = in_focus(Focus::List);
    CHECK(t.handle(Key::Char, "l") == Intent::None);
    CHECK(t.focus() == Focus::Preview);
}

TEST_CASE("List: Enter and e edit in the right pane, E is the external editor; empty list is a no-op") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Enter) == Intent::EnterEditor);
    CHECK(s.handle(Key::Char, "e") == Intent::EnterEditor);
    CHECK(s.handle(Key::Char, "E") == Intent::OpenSelected);
    CHECK(s.editor_start_line() == 0);
    auto empty = in_focus(Focus::List, {});
    CHECK(empty.handle(Key::Enter) == Intent::None);
    CHECK(empty.handle(Key::Char, "e") == Intent::None);
    CHECK(empty.handle(Key::Char, "E") == Intent::None);
}

TEST_CASE("List: < and > resize within bounds, z toggles zoom; the keys are inert in text inputs") {
    auto s = in_focus(Focus::List);
    s.set_window(100, 30);
    const int before = s.layout().list_width(100);
    s.handle(Key::Char, ">");
    CHECK(s.layout().list_width(100) == before + tui::PaneLayout::kKeyStep);
    s.handle(Key::Char, "<");
    s.handle(Key::Char, "<");
    CHECK(s.layout().list_width(100) == before - tui::PaneLayout::kKeyStep);
    s.handle(Key::Char, "z");
    CHECK(s.layout().zoomed());
    s.handle(Key::Char, "z");
    CHECK_FALSE(s.layout().zoomed());

    auto search = in_focus(Focus::Search, {});
    search.handle(Key::Char, "z");
    search.handle(Key::Char, "<");
    CHECK(search.query() == "z<");
    CHECK_FALSE(search.layout().zoomed());
}

TEST_CASE("F11 toggles zoom in the List, the preview and inside text inputs without typing") {
    auto list = in_focus(Focus::List);
    CHECK(list.handle(Key::F11) == Intent::None);
    CHECK(list.layout().zoomed());
    list.handle(Key::F11);
    CHECK_FALSE(list.layout().zoomed());

    auto preview = in_focus(Focus::Preview);
    preview.handle(Key::F11);
    CHECK(preview.layout().zoomed());
    CHECK(preview.focus() == Focus::Preview);

    auto search = in_focus(Focus::Search, {});
    search.handle(Key::Char, "a");
    search.handle(Key::F11);
    CHECK(search.layout().zoomed());
    CHECK(search.query() == "a");
    CHECK(search.focus() == Focus::Search);
}

TEST_CASE("List: slash goes to Search without entering the query") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Char, "/") == Intent::None);
    CHECK(s.focus() == Focus::Search);
    CHECK(s.query().empty());
}

TEST_CASE("List: action letters map to the same intents as their Ctrl keys") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Enter) == Intent::EnterEditor);
    CHECK(s.handle(Key::Char, "a") == Intent::NewNote);
    CHECK(s.handle(Key::Char, "n") == Intent::NewRow);
    CHECK(s.handle(Key::Char, "d") == Intent::TrashSelected);
    CHECK(s.handle(Key::Char, "r") == Intent::MoveSelected);
    CHECK(s.handle(Key::CtrlS) == Intent::SyncNow);
    CHECK(s.handle(Key::Char, "?") == Intent::ShowHelp);
    CHECK(s.handle(Key::Char, "q") == Intent::Quit);
    CHECK(s.query().empty());
    CHECK(s.focus() == Focus::List);
}

// NY-T: Liste harfleri CLI kısa bayraklarıdır (-n -a -s -S -x -T -i); senkron Ctrl+S, seçim v, sütun V, çöp t.
TEST_CASE("List: letters follow the CLI short flags, sync is Ctrl+S in the list and in REVIEW") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Char, "x") == Intent::ExportPdf);
    CHECK(s.handle(Key::Char, "i") == Intent::ImportFiles);
    CHECK(s.handle(Key::Char, "t") == Intent::ToggleTrash);
    for (const std::string_view search : {"s", "f"}) {
        CAPTURE(search);
        auto searching = in_focus(Focus::List);
        searching.handle(Key::Char, search);
        CHECK(searching.focus() == Focus::Search);
        CHECK(searching.mode() == Mode::Global);
    }
    auto tags = in_focus(Focus::List);
    tags.handle(Key::Char, "T");
    CHECK(tags.focus() == Focus::Search);
    CHECK(tags.query().starts_with("tag:"));
    auto marks = in_focus(Focus::List);
    marks.handle(Key::Char, "v");
    CHECK(marks.marked_count() == 1);
    CHECK(marks.handle(Key::Char, "V") == Intent::SavePrefs);  // sağ sütun döngüsü
    auto shown = in_focus(Focus::List);
    shown.handle(Key::Char, "S");
    CHECK(shown.focus() == Focus::Preview);
    CHECK(shown.handle(Key::CtrlS) == Intent::SyncNow);
}

TEST_CASE("List: Tab toggles the mode") {
    auto s = in_focus(Focus::List);
    s.set_content_searcher(fake_searcher());
    CHECK(s.handle(Key::Tab) == Intent::None);
    CHECK(s.mode() == Mode::Content);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("List: Esc clears a filter and does nothing when it is empty") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Escape) == Intent::None);
    type(s, "tod");
    s.handle(Key::Escape);
    REQUIRE(s.focus() == Focus::List);
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.query().empty());
    CHECK(s.rows().size() == 3);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("List: unknown letters do nothing, including Turkish dotless i and uppercase actions") {
    for (const std::string_view letter : {"w", "h", "A", "N", "Q", "ı", "İ", "ş", "ğ", " "}) {
        CAPTURE(letter);
        auto s = in_focus(Focus::List);
        s.handle(Key::Down);
        CHECK(s.handle(Key::Char, letter) == Intent::None);
        CHECK(s.query().empty());
        CHECK(s.selected() == 1);
        CHECK(s.focus() == Focus::List);
        CHECK(s.rows().size() == 3);
    }
}

TEST_CASE("List: a control character delivered as text does nothing") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::Char, "\x04") == Intent::None);
    CHECK(s.handle(Key::Char, "") == Intent::None);
    CHECK(s.query().empty());
}

TEST_CASE("List: Backspace and Left are ignored") {
    auto s = in_focus(Focus::List);
    type(s, "tod");
    s.handle(Key::Escape);
    CHECK(s.handle(Key::Backspace) == Intent::None);
    CHECK(s.query() == "tod");
    CHECK(s.handle(Key::Left) == Intent::None);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("Ctrl-O is the same as a and never opens the external editor") {
    auto s = in_focus(Focus::List);
    CHECK(s.handle(Key::CtrlO) == s.handle(Key::Char, "a"));
    CHECK(s.handle(Key::CtrlO) == Intent::NewNote);
}

TEST_CASE("empty list: d, r and Enter are no-ops in List while n, a, / still work") {
    tui::TuiState s;
    for (const std::string_view letter : {"d", "r", "g", "g", "G", "j", "k"})
        CHECK(s.handle(Key::Char, letter) == Intent::None);
    CHECK(s.handle(Key::Enter) == Intent::None);
    CHECK(s.handle(Key::Char, "n") == Intent::NewRow);
    CHECK(s.handle(Key::Char, "a") == Intent::NewNote);
    CHECK(s.handle(Key::Char, "/") == Intent::None);
    CHECK(s.focus() == Focus::Search);
}

TEST_CASE("a filter that matches nothing: Enter in List does not create a note") {
    auto s = in_focus(Focus::List);
    type(s, "zzzqq");
    s.handle(Key::Escape);
    REQUIRE(s.focus() == Focus::List);
    REQUIRE(s.rows().empty());
    CHECK(s.handle(Key::Enter) == Intent::None);
    CHECK(s.handle(Key::Char, "d") == Intent::None);
    CHECK(s.handle(Key::Char, "r") == Intent::None);
}

TEST_CASE("Search: every letter is query text and never an action") {
    auto s = in_focus(Focus::Search, {});
    const std::string letters = "dangrskqjlhG?/";
    for (const char c : letters)
        CHECK(s.handle(Key::Char, std::string_view(&c, 1)) == Intent::None);
    CHECK(s.query() == letters);
    CHECK(s.focus() == Focus::Search);
    s.handle(Key::Char, "ı");
    CHECK(s.query() == letters + "ı");
}

TEST_CASE("Search: typing filters on each key") {
    auto s = in_focus(Focus::Search);
    s.handle(Key::Char, "t");
    CHECK(s.rows().size() == 3);  // meeting/todo/sort hepsinde t var
    s.handle(Key::Char, "o");
    s.handle(Key::Char, "d");
    CHECK(s.rows().size() == 1);
    s.handle(Key::Backspace);
    CHECK(s.query() == "to");
    CHECK(s.focus() == Focus::Search);
}

TEST_CASE("Search: arrows move the selection while staying in Search") {
    auto s = in_focus(Focus::Search);
    s.handle(Key::Down);
    CHECK(s.selected() == 1);
    s.handle(Key::Down);
    CHECK(s.selected() == 2);
    s.handle(Key::Up);
    CHECK(s.selected() == 1);
    s.handle(Key::Up);
    CHECK(s.selected() == 0);
    CHECK(s.focus() == Focus::Search);
}

TEST_CASE("Search: Enter keeps the selection and returns to List, no match creates from the query") {
    auto s = in_focus(Focus::Search);
    s.handle(Key::Down);
    const auto selected = s.selected();
    CHECK(s.handle(Key::Enter) == Intent::None);
    CHECK(s.focus() == Focus::List);
    CHECK(s.selected() == selected);
    type(s, "zzzqq");
    CHECK(s.handle(Key::Enter) == Intent::CreateFromQuery);
    CHECK(s.query() == "zzzqq");
    auto empty = in_focus(Focus::Search, {});
    CHECK(empty.handle(Key::Enter) == Intent::None);
}

TEST_CASE("Search: Ctrl+T toggles the mode and stays in Search") {
    auto s = in_focus(Focus::Search);
    s.set_content_searcher(fake_searcher());
    CHECK(s.handle(Key::CtrlT) == Intent::None);
    CHECK(s.mode() == Mode::Content);
    CHECK(s.focus() == Focus::Search);
}

TEST_CASE("Search: left and right move the cursor and do not leave Search") {
    auto s = in_focus(Focus::Search);
    s.handle(Key::Char, "o");
    CHECK(s.handle(Key::Left) == Intent::None);
    CHECK(s.search_line().cursor() == 0);
    CHECK(s.handle(Key::Right) == Intent::None);
    CHECK(s.search_line().cursor() == 1);
    CHECK(s.focus() == Focus::Search);
    CHECK(s.query() == "o");
}

TEST_CASE("Search: Esc returns to List with the filter, then d acts on the filtered selection") {
    auto s = in_focus(Focus::Search);
    for (const char c : std::string("tod"))
        s.handle(Key::Char, std::string_view(&c, 1));
    REQUIRE(s.rows().size() == 1);
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.focus() == Focus::List);
    CHECK(s.query() == "tod");
    CHECK(s.rows().size() == 1);
    CHECK(s.handle(Key::Char, "d") == Intent::TrashSelected);
    CHECK(selected_path(s) == "personal-todo.md");
    CHECK(s.query() == "tod");
}

TEST_CASE("Preview: arrows and j/k scroll by line and keep the selection") {
    auto s = in_focus(Focus::Preview);
    s.set_preview_line_count(30);
    CHECK(s.handle(Key::Down) == Intent::None);
    CHECK(s.preview_scroll() == 1);
    s.handle(Key::Char, "j");
    CHECK(s.preview_scroll() == 2);
    s.handle(Key::Up);
    CHECK(s.preview_scroll() == 1);
    s.handle(Key::Char, "k");
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::Char, "k");
    CHECK(s.preview_scroll() == 0);
    CHECK(s.selected() == 0);
    CHECK(s.focus() == Focus::Preview);
}

TEST_CASE("Preview: PgUp/PgDn scroll by page, g and G go to top and bottom") {
    auto s = in_focus(Focus::Preview);
    s.set_preview_line_count(30);
    s.handle(Key::PageDown);
    CHECK(s.preview_scroll() == tui::TuiState::kPreviewPageLines);
    s.handle(Key::PageUp);
    CHECK(s.preview_scroll() == 0);
    s.handle(Key::Char, "G");
    CHECK(s.preview_scroll() == 29);
    s.handle(Key::Char, "g");
    CHECK(s.preview_scroll() == 0);
    CHECK(s.selected() == 0);
}

TEST_CASE("Preview: left, h and Esc go back to List keeping query and scroll") {
    const std::array<std::pair<Key, std::string_view>, 3> backs = {
        {{Key::Left, ""}, {Key::Char, "h"}, {Key::Escape, ""}}};
    for (const auto& [key, text] : backs) {
        auto s = in_focus(Focus::List);
        type(s, "o");
        s.handle(Key::Escape);
        s.handle(Key::Right);
        s.set_preview_line_count(30);
        s.handle(Key::Char, "j");
        CHECK(s.handle(key, text) == Intent::None);
        CHECK(s.focus() == Focus::List);
        CHECK(s.query() == "o");
        CHECK(s.preview_scroll() == 1);
    }
}

TEST_CASE("Preview: e and Enter open the editor at the top visible line, E the external one") {
    auto s = in_focus(Focus::Preview);
    s.set_preview_line_count(30);
    s.handle(Key::PageDown);
    CHECK(s.handle(Key::Char, "e") == Intent::EnterEditor);
    CHECK(s.handle(Key::Enter) == Intent::EnterEditor);
    CHECK(s.editor_start_line() == tui::TuiState::kPreviewPageLines);
    CHECK(s.handle(Key::Char, "E") == Intent::OpenSelected);
    auto empty = in_focus(Focus::Preview, {});
    CHECK(empty.handle(Key::Enter) == Intent::None);
    CHECK(empty.handle(Key::Char, "e") == Intent::None);
}

TEST_CASE("Preview: list command letters and unknown letters are ignored") {
    for (const std::string_view letter : {"a", "n", "d", "r", "s", "?", "l", "x", "ı"}) {
        CAPTURE(letter);
        auto s = in_focus(Focus::Preview);
        CHECK(s.handle(Key::Char, letter) == Intent::None);
        CHECK(s.focus() == Focus::Preview);
        CHECK(s.query().empty());
        CHECK(s.selected() == 0);
    }
}

TEST_CASE("Preview: list commands are ignored in their Ctrl forms too; Ctrl+S syncs") {
    for (const Key key : {Key::CtrlD, Key::CtrlR, Key::CtrlO, Key::CtrlP, Key::CtrlN, Key::Tab}) {
        CAPTURE(static_cast<int>(key));
        auto s = in_focus(Focus::Preview);
        CHECK(s.handle(key) == Intent::None);
        CHECK(s.focus() == Focus::Preview);
        CHECK(s.selected() == 0);
        CHECK(s.mode() == Mode::Name);
    }
}

TEST_CASE("Preview: Ctrl-C, F1 and F9 still work") {
    auto s = in_focus(Focus::Preview);
    CHECK(s.handle(Key::CtrlC) == Intent::Quit);
    CHECK(s.handle(Key::F1) == Intent::ShowHelp);
    CHECK(s.handle(Key::F9) == Intent::ShowSettings);
    CHECK(s.focus() == Focus::Preview);
}

namespace {

const std::vector<std::string> kFindLines = {"Işık ılık", "boş satır", "kırmızı IŞIK ve ışık", "son"};

tui::TuiState previewing_find_lines() {
    auto s = in_focus(Focus::Preview);
    s.set_preview_lines(&kFindLines, 1);
    return s;
}

void type_find(tui::TuiState& s, std::string_view ascii) {
    for (std::size_t i = 0; i < ascii.size(); ++i)
        s.handle(Key::Char, ascii.substr(i, 1));
}

}  // namespace

TEST_CASE("Preview find: / opens the input, letters are text, Turkish folding matches I and dotless i") {
    auto s = previewing_find_lines();
    CHECK(s.handle(Key::Char, "/") == Intent::None);
    CHECK(s.focus() == Focus::PreviewFind);
    for (const std::string_view letter : {"ı", "ş", "ı", "k"})
        CHECK(s.handle(Key::Char, letter) == Intent::None);
    CHECK(s.preview_find().query() == "ışık");
    CHECK(s.preview_find().count() == 3);
    CHECK(s.preview_find().current() == 0);
    CHECK(s.preview_find().ranges_on_line(2).size() == 2);
    CHECK(tui::match_label(s.preview_find().current(), s.preview_find().count()) == "1/3");
    CHECK(s.selected() == 0);
}

TEST_CASE("Preview find: uppercase I finds i, ı, I and İ alike") {
    const std::vector<std::string> lines = {"i ı I İ"};
    auto s = in_focus(Focus::Preview);
    s.set_preview_lines(&lines, 1);
    s.handle(Key::Char, "/");
    s.handle(Key::Char, "I");
    CHECK(s.preview_find().count() == 4);
}

TEST_CASE("Preview find: Enter closes keeping highlights, n and N move with wrap, Esc clears then leaves") {
    auto s = previewing_find_lines();
    s.handle(Key::Char, "/");
    type_find(s, "son");
    s.handle(Key::Enter);
    CHECK(s.focus() == Focus::Preview);
    CHECK(s.preview_find().count() == 1);
    s.handle(Key::Char, "/");
    for (const std::string_view letter : {"ı", "ş", "ı", "k"})
        s.handle(Key::Char, letter);
    s.handle(Key::Enter);
    s.handle(Key::Char, "n");
    CHECK(s.preview_find().current() == 1);
    CHECK(s.preview_find().current_line() == 2);
    s.handle(Key::Char, "n");
    s.handle(Key::Char, "n");
    CHECK(s.preview_find().current() == 0);
    s.handle(Key::Char, "N");
    CHECK(s.preview_find().current() == 2);
    s.handle(Key::Escape);
    CHECK(s.focus() == Focus::Preview);
    CHECK(s.preview_find().count() == 0);
    s.handle(Key::Escape);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("Preview find: Esc in the input clears, list letters never act, stale text recomputes safely") {
    auto s = previewing_find_lines();
    s.handle(Key::Char, "/");
    for (const std::string_view letter : {"d", "q", "n", "z", "<", "e"})
        CHECK(s.handle(Key::Char, letter) == Intent::None);
    CHECK(s.preview_find().query() == "dqnz<e");
    CHECK_FALSE(s.layout().zoomed());
    s.handle(Key::Backspace);
    CHECK(s.preview_find().query() == "dqnz<");
    s.handle(Key::Escape);
    CHECK(s.focus() == Focus::Preview);
    CHECK(s.preview_find().query().empty());

    s.handle(Key::Char, "/");
    type_find(s, "son");
    const std::vector<std::string> changed = {"yeni"};
    s.set_preview_lines(&changed, 2);
    CHECK(s.preview_find().count() == 0);
    CHECK(s.preview_find().ranges_on_line(3).empty());
}

TEST_CASE("Preview: n with no search does nothing; z and < > work here too") {
    auto s = previewing_find_lines();
    s.set_window(100, 30);
    CHECK(s.handle(Key::Char, "n") == Intent::None);
    s.handle(Key::Char, "z");
    CHECK(s.layout().zoomed());
    const auto wide = s.layout().list_width(100);
    s.handle(Key::Char, "<");
    CHECK(s.layout().list_width(100) == wide - tui::PaneLayout::kKeyStep);
    CHECK(s.focus() == Focus::Preview);
}

TEST_CASE("Editor: every key goes to the editor and list commands never fire") {
    for (const std::string_view letter :
         {"a", "n", "d", "r", "s", "q", "?", "/", "j", "k", "g", "G", "l", "ı"}) {
        CAPTURE(letter);
        auto s = in_focus(Focus::Editor);
        CHECK(s.handle(Key::Char, letter) == Intent::EditorKey);
        CHECK(s.focus() == Focus::Editor);
        CHECK(s.query().empty());
        CHECK(s.selected() == 0);
    }
    for (const Key key :
         {Key::CtrlD,    Key::CtrlR,     Key::CtrlO, Key::CtrlS, Key::CtrlP, Key::CtrlN, Key::Tab,
          Key::Enter,    Key::Escape,    Key::Left,  Key::Right, Key::Up,    Key::Down,  Key::PageUp,
          Key::PageDown, Key::Backspace, Key::CtrlC, Key::CtrlZ, Key::F2,    Key::F9}) {
        CAPTURE(static_cast<int>(key));
        auto s = in_focus(Focus::Editor);
        CHECK(s.handle(key) == Intent::EditorKey);
        CHECK(s.focus() == Focus::Editor);
        CHECK(s.selected() == 0);
        CHECK(s.mode() == Mode::Name);
    }
}

TEST_CASE("Editor: F1 shows help without leaving, Ctrl-C can never quit") {
    auto s = in_focus(Focus::Editor);
    CHECK(s.handle(Key::F1) == Intent::ShowHelp);
    CHECK(s.focus() == Focus::Editor);
    CHECK(s.handle(Key::CtrlC) != Intent::Quit);
}

TEST_CASE("Editor: leaving returns to Preview at the given line, or to List for a draft") {
    auto s = in_focus(Focus::List);
    type(s, "o");
    s.handle(Key::Escape);
    s.handle(Key::Down);
    const auto selected = s.selected();
    s.set_preview_line_count(30);
    s.enter_editor();
    s.handle(Key::Char, "x");
    s.leave_editor(7);
    CHECK(s.focus() == Focus::Preview);
    CHECK(s.preview_scroll() == 7);
    CHECK(s.query() == "o");
    CHECK(s.selected() == selected);
    s.enter_editor();
    s.leave_editor(std::nullopt);
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("select_note moves the selection without touching focus or query") {
    auto s = in_focus(Focus::Editor);
    CHECK(s.select_note(core::NotePath::parse("snippets-sort.java").value()));
    CHECK(selected_path(s) == "snippets-sort.java");
    CHECK(s.focus() == Focus::Editor);
    CHECK_FALSE(s.select_note(core::NotePath::parse("nope.md").value()));
    CHECK(selected_path(s) == "snippets-sort.java");
}

TEST_CASE("select_note clears a filter that hides the saved note") {
    auto s = in_focus(Focus::List);
    type(s, "tod");
    s.handle(Key::Escape);
    CHECK(s.select_note(core::NotePath::parse("snippets-sort.java").value()));
    CHECK(s.query().empty());
    CHECK(selected_path(s) == "snippets-sort.java");
}

TEST_CASE("Ctrl-Z suspends only outside the editor; Ctrl-C in a modal quits only outside it") {
    CHECK(tui::passes_to_terminal(Key::CtrlZ, Focus::List));
    CHECK(tui::passes_to_terminal(Key::CtrlZ, Focus::Search));
    CHECK_FALSE(tui::passes_to_terminal(Key::CtrlZ, Focus::Editor));
    CHECK_FALSE(tui::passes_to_terminal(Key::CtrlC, Focus::List));
    CHECK(tui::quits_from_modal(Key::CtrlC, Focus::List));
    CHECK(tui::quits_from_modal(Key::CtrlC, Focus::Search));
    CHECK_FALSE(tui::quits_from_modal(Key::CtrlC, Focus::Editor));
    CHECK_FALSE(tui::quits_from_modal(Key::Escape, Focus::List));
}

TEST_CASE("Ctrl keys, F1 and F9 work in List and Search") {
    for (const Focus focus : {Focus::List, Focus::Search}) {
        CAPTURE(static_cast<int>(focus));
        auto s = in_focus(focus);
        CHECK(s.handle(Key::CtrlO) == Intent::NewNote);
        CHECK(s.handle(Key::CtrlR) == Intent::MoveSelected);
        CHECK(s.handle(Key::CtrlD) == Intent::TrashSelected);
        CHECK(s.handle(Key::CtrlS) == Intent::SyncNow);
        CHECK(s.handle(Key::F1) == Intent::ShowHelp);
        CHECK(s.handle(Key::F9) == Intent::ShowSettings);
        CHECK(s.handle(Key::CtrlC) == Intent::Quit);
        if (focus == Focus::List) {  // Aramada Ctrl-P/N geçmiştir
            s.handle(Key::CtrlN);
            CHECK(s.selected() == 1);
            s.handle(Key::CtrlP);
            CHECK(s.selected() == 0);
        }
        CHECK(s.focus() == focus);
    }
}

TEST_CASE("Ctrl-R and Ctrl-D stay no-ops without a selection") {
    for (const Focus focus : {Focus::List, Focus::Search}) {
        auto s = in_focus(focus, {});
        CHECK(s.handle(Key::CtrlR) == Intent::None);
        CHECK(s.handle(Key::CtrlD) == Intent::None);
    }
}

TEST_CASE("reveal returns to List so the saved note can be acted on") {
    auto s = in_focus(Focus::Search);
    CHECK(s.reveal(core::NotePath::parse("snippets-sort.java").value()));
    CHECK(s.focus() == Focus::List);
}

TEST_CASE("deleting keeps the selection on a neighbor, on a filtered list too") {
    // main.cpp'deki delete_target akışı: set_notes(silinmiş liste) + select_index(eski satır).
    auto s = in_focus(Focus::List);
    s.handle(Key::Down);
    const std::size_t index = s.selected();
    s.set_notes({note("work-meeting.md", 0), note("snippets-sort.java", 2)});
    s.select_index(index);
    CHECK(selected_path(s) == "snippets-sort.java");  // silinenin yerine gelen

    s.set_notes({note("work-meeting.md", 0)});
    s.select_index(index);
    CHECK(selected_path(s) == "work-meeting.md");  // son satır silindi: bir üstü

    auto filtered = in_focus(Focus::List, {note("a1.md", 0), note("a2.md", 1), note("b.md", 2)});
    type(filtered, "a");
    filtered.handle(Key::Escape);
    filtered.handle(Key::Char, "G");
    REQUIRE(selected_path(filtered) == "a2.md");
    const std::size_t last = filtered.selected();
    filtered.set_notes({note("a1.md", 0), note("b.md", 2)});
    filtered.select_index(last);
    CHECK(selected_path(filtered) == "a1.md");
    CHECK(filtered.query() == "a");
}

// ---- `a` ad satırı ----

TEST_CASE("name row: typed text is kept verbatim, UTF-8 Backspace removes one character") {
    tui::NewRowState q;
    for (const std::string_view part : {"d", "q", " ", "ş"})
        CHECK(q.handle(Key::Char, part) == tui::NewRowState::Outcome::Editing);
    CHECK(q.text() == "dq ş");
    q.handle(Key::Backspace);
    CHECK(q.text() == "dq ");
    q.handle(Key::Char, "\x07");  // kontrol karakteri girmez
    CHECK(q.text() == "dq ");
}

TEST_CASE("name row: Enter submits, Esc cancels, blank input cancels instead of saving") {
    tui::NewRowState q;
    q.handle(Key::Char, "x");
    CHECK(q.handle(Key::Enter) == tui::NewRowState::Outcome::Submit);
    CHECK(q.handle(Key::Escape) == tui::NewRowState::Outcome::Cancelled);
    tui::NewRowState empty;
    CHECK(empty.handle(Key::Enter) == tui::NewRowState::Outcome::Cancelled);
    tui::NewRowState spaces;
    spaces.handle(Key::Char, " ");
    spaces.handle(Key::Char, "\t");
    CHECK(spaces.handle(Key::Enter) == tui::NewRowState::Outcome::Cancelled);
}

TEST_CASE("name row: other keys neither submit nor cancel and an edit clears the error") {
    tui::NewRowState q;
    q.handle(Key::Char, "x");
    for (const Key key : {Key::Up, Key::Down, Key::Tab, Key::CtrlD, Key::CtrlO, Key::F1, Key::F2})
        CHECK(q.handle(key) == tui::NewRowState::Outcome::Editing);
    CHECK(q.text() == "x");
    q.set_error("disk full");
    CHECK(q.error() == "disk full");
    q.handle(Key::Char, "y");
    CHECK(q.error().empty());
}

// ---- diyaloglar girdiyi sahiplenir: kısayol harfleri onlara metin ya da etkisiz tuştur ----

TEST_CASE("confirm dialog: action letters stay pending and never confirm") {
    tui::ConfirmState confirm("delete?");
    for (const std::string_view letter : {"d", "y", "a", "q", "ı"})
        CHECK(confirm.handle(Key::Char, letter) == tui::ConfirmState::Outcome::Pending);
    CHECK_FALSE(confirm.yes_selected());
    CHECK(confirm.handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
}

TEST_CASE("help: action letters keep it open") {
    tui::HelpState help(30);
    for (const std::string_view letter : {"d", "a", "n", "r", "s", "j", "k", "ı"})
        CHECK(help.handle(Key::Char, letter));
}

TEST_CASE("save dialog: action letters are name text") {
    tui::SaveDialogState dialog(tui::SaveDialogInit{"content",
                                                    "",
                                                    {},
                                                    {"md"},
                                                    detect::Suggestion{"md", 1.0, {}, true},
                                                    60,
                                                    std::chrono::system_clock::now(),
                                                    /*move=*/false,
                                                    {},
                                                    {}});
    const std::string before = dialog.name();
    for (const std::string_view letter : {"d", "a", "q"})
        CHECK(dialog.handle(Key::Char, letter) == tui::SaveDialogState::Outcome::Editing);
    CHECK(dialog.name() == before + "daq");
}

namespace {

tui::TuiState sample_state() {
    tui::TuiState state;
    state.set_notes(sample());
    return state;
}

}  // namespace

TEST_CASE("exit: q quits from List and Preview, not from any input line or the editor") {
    auto s = sample_state();
    CHECK(s.handle(Key::Char, "q") == Intent::Quit);
    s.handle(Key::Right);
    REQUIRE(s.focus() == Focus::Preview);
    CHECK(s.handle(Key::Char, "q") == Intent::Quit);

    auto find = sample_state();
    find.handle(Key::Right);
    find.handle(Key::Char, "/");
    REQUIRE(find.focus() == Focus::PreviewFind);
    CHECK(find.handle(Key::Char, "q") == Intent::None);
    CHECK(find.preview_find().query() == "q");

    auto search = sample_state();
    search.handle(Key::Char, "/");
    CHECK(search.handle(Key::Char, "q") == Intent::None);
    CHECK(search.query() == "q");

    auto editor = sample_state();
    editor.enter_editor();
    CHECK(editor.handle(Key::Char, "q") == Intent::EditorKey);
    CHECK(editor.handle(Key::CtrlQ) == Intent::EditorKey);
    CHECK(editor.handle(Key::CtrlC) == Intent::EditorKey);
}

TEST_CASE("exit: Esc in List clears the filter and never quits; Ctrl-C stays the emergency exit") {
    auto s = sample_state();
    s.handle(Key::Char, "/");
    s.handle(Key::Char, "a");
    s.handle(Key::Enter);
    REQUIRE_FALSE(s.query().empty());
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.query().empty());
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.handle(Key::CtrlC) == Intent::Quit);
    s.handle(Key::Right);
    CHECK(s.handle(Key::CtrlC) == Intent::Quit);
}

// Tek olgu: modüllerin kod varsayılanları defaults.toml'dakiyle aynı (config'siz kurulan nesneler ve
// testler).
TEST_CASE("tui and lsp limit structs default to the values in defaults.toml") {
    const core::Config config = core::parse_config("").config;
    CHECK(tui::lsp_limits_of(config.limits.lsp) == tui::LspLimits{});
    CHECK(tui::view_limits_of(config.limits.tui) == tui::ViewLimits{});
    CHECK(tui::syntax_limits_of(config.limits.tui) == tui::SyntaxLimits{});
    CHECK(tui::completion_limits_of(config.limits.tui, config.tuning.completion_min_prefix_bytes) ==
          tui::CompletionLimits{});
    CHECK(config.limits.tui.spellcheck_line_bytes.value == tui::SpellChecker::kDefaultMaxLineBytes);
    CHECK(config.limits.tui.paste_bytes.value == tui::SystemClipboard::kDefaultMaxPasteBytes);
    CHECK(config.limits.tui.undo_bytes.value == tui::EditorBuffer::kDefaultUndoLimitBytes);
    CHECK(config.limits.tui.save_suggestions.value == tui::SaveDialogInit::kDefaultSuggestions);
    CHECK(config.limits.tui.search_history.value == tui::SearchHistoryLimits{}.entries);
    CHECK(config.limits.tui.search_entry_bytes.value == tui::SearchHistoryLimits{}.entry_bytes);
    CHECK(config.limits.search.fuzzy_candidate_chars.value == search::kDefaultFuzzyCandidateChars);
    CHECK(config.tuning.tui_list_share == tui::PaneLayout::kDefaultListShare);
}
