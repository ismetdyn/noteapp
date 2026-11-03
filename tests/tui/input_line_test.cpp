#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "edit/completion.hpp"
#include "edit/editor_session.hpp"
#include "edit/input_line.hpp"
#include "search/query.hpp"
#include "support/test_support.hpp"
#include "tui/input_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::InputKind;
using tui::InputLine;
using tui::InputSpan;
using tui::Intent;
using tui::Key;
using tui::Mode;

namespace {

core::NoteMeta meta(const std::string& path) {
    return core::NoteMeta{.path = core::NotePath::parse(path).value()};
}

tui::SharedInputSources sources() {
    const std::vector<core::NoteMeta> notes = {meta("work/plan.md"), meta("work/plain.txt"),
                                               meta("personal/todo.cpp"), meta("my notes/a b.md")};
    return tui::build_input_sources(notes);
}

InputLine typed(InputKind kind, std::string_view text) {
    InputLine line(kind);
    line.set_sources(sources());
    line.set_text(std::string(text));
    return line;
}

bool has_item(const InputLine& line, std::string_view item) {
    const auto& items = line.popup()->items;
    return std::ranges::find(items, item) != items.end();
}

std::vector<std::string> sorted_items(const InputLine& line) {
    std::vector<std::string> items = line.popup()->items;
    std::ranges::sort(items);
    return items;
}

core::NoteInfo info(const std::string& path) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(100), 1};
}

tui::TuiState search_state() {
    tui::TuiState s;
    s.set_notes({info("work/plan.md"), info("work/plain.txt"), info("personal/todo.cpp")});
    s.handle(Key::Char, "/");
    return s;
}

void type_chars(tui::TuiState& s, std::string_view text) {
    for (const char& c : text)
        s.handle(Key::Char, std::string_view(&c, 1));
}

ftxui::Screen draw_state(const tui::TuiState& s, const tui::NewRowState* row = nullptr,
                         const tui::RenameState* rename = nullptr) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, tui::render(s, tui::Preview{}, {}, {100, 24}, nullptr, nullptr, nullptr, nullptr,
                                      row, nullptr, nullptr, rename));
    return screen;
}

// Ekran satırındaki ilk `needle`ın başladığı sütun; hücre başına bir kod noktası sayılır.
std::optional<int> column_of(const ftxui::Screen& screen, int y, std::string_view needle) {
    std::string row;
    std::vector<int> columns;
    for (int x = 0; x < screen.dimx(); ++x) {
        columns.insert(columns.end(), screen.PixelAt(x, y).character.size(), x);
        row += screen.PixelAt(x, y).character;
    }
    const std::size_t at = row.find(needle);
    return at == std::string::npos ? std::nullopt : std::optional(columns[at]);
}

}  // namespace

TEST_CASE("InputLine edits at a UTF-8 cursor and reports what changed") {
    InputLine line;
    CHECK(line.handle(Key::Char, "ş") == InputLine::Result::Edited);
    CHECK(line.handle(Key::Char, "a") == InputLine::Result::Edited);
    CHECK(line.cursor() == line.text().size());
    CHECK(line.handle(Key::Left) == InputLine::Result::Consumed);
    CHECK(line.handle(Key::Left) == InputLine::Result::Consumed);
    CHECK(line.cursor() == 0);
    line.handle(Key::Right);
    CHECK(line.cursor() == std::string_view("ş").size());
    line.handle(Key::Char, "ı");
    CHECK(line.text() == "şıa");
    CHECK(line.handle(Key::Backspace) == InputLine::Result::Edited);
    CHECK(line.text() == "şa");
    line.handle(Key::Home);
    CHECK(line.handle(Key::Delete) == InputLine::Result::Edited);
    CHECK(line.text() == "a");
    line.handle(Key::End);
    CHECK(line.cursor() == 1);
    CHECK(line.handle(Key::Enter) == InputLine::Result::Ignored);
    CHECK(line.handle(Key::Escape) == InputLine::Result::Ignored);
    CHECK(line.handle(Key::Up) == InputLine::Result::Ignored);
    CHECK(line.handle(Key::Char, "\x01") == InputLine::Result::Consumed);
    CHECK(line.text() == "a");
}

TEST_CASE("search ghost offers recent searches first, then matching note names") {
    const std::vector<std::string> recent = {"sort:name plan", "plan b"};
    InputLine line = typed(InputKind::Search, "pl");
    CHECK(line.ghost(recent) == "an b");
    CHECK(line.ghost() == "ain.txt");  // geçmiş yok: ad, sıralı ilk
    line.set_text("sort:name pl");
    CHECK(line.ghost() == "ain.txt");  // son sözcük tamamlanır
    line.set_text("work/pla");
    CHECK(line.ghost() == "in.txt");
    line.set_text("zz");
    CHECK(line.ghost(recent).empty());
    line.set_text("");
    CHECK(line.ghost(recent).empty());
}

TEST_CASE("the ghost appears only at the end of the text and → accepts it") {
    const std::vector<std::string> recent = {"plan b"};
    InputLine line = typed(InputKind::Search, "plan");
    line.handle(Key::Left);
    CHECK(line.ghost(recent).empty());
    CHECK(line.handle(Key::Right, {}, recent) == InputLine::Result::Consumed);  // yalnız hareket
    CHECK(line.text() == "plan");
    CHECK(line.ghost(recent) == " b");
    CHECK(line.handle(Key::Right, {}, recent) == InputLine::Result::Edited);
    CHECK(line.text() == "plan b");
    CHECK(line.cursor() == line.text().size());
    CHECK(line.handle(Key::Right, {}, recent) == InputLine::Result::Consumed);  // öneri yok: etkisiz
}

TEST_CASE("path ghost offers existing folders, then names") {
    InputLine line = typed(InputKind::Path, "wo");
    CHECK(line.ghost() == "rk/");
    line.set_text("work/pl");
    CHECK(line.ghost() == "ain.txt");
    line.set_text("tod");
    CHECK(line.ghost() == "o.cpp");  // klasörsüz ad da önerilir
    line.set_text("work/plan.md");
    CHECK(line.ghost().empty());  // tam eşleşmenin eklenecek kısmı yok
}

TEST_CASE("note names with control bytes are never offered") {
    const std::vector<core::NoteMeta> notes = {meta("work/plan.md")};
    const auto src = tui::build_input_sources(notes);
    CHECK(src->names == std::vector<std::string>{"work/plan.md"});
    CHECK(src->folders == std::vector<std::string>{"work/"});
    CHECK(src->base_names == std::vector<std::string>{"plan.md"});
    CHECK(src->types == std::vector<std::string>{".md"});
    if (const auto bad =
            core::NotePath::parse("a\x1b[31m.md")) {  // yol kuralı reddederse adaya zaten ulaşamaz
        const std::vector<core::NoteMeta> hostile = {core::NoteMeta{.path = *bad}};
        CHECK(tui::build_input_sources(hostile)->names.empty());
    }
}

TEST_CASE("Tab inserts a unique candidate directly and opens no popup") {
    InputLine line = typed(InputKind::Path, "personal");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Consumed);  // klasör ve içindeki not: iki aday
    CHECK(line.popup() != nullptr);
    line.handle(Key::Escape);
    line.set_text("todo");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Edited);
    CHECK(line.text() == "personal/todo.cpp");
    CHECK(line.popup() == nullptr);
    line.set_text("qqqq");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Consumed);  // aday yok: açılmaz
    CHECK(line.popup() == nullptr);
    CHECK(line.text() == "qqqq");
}

TEST_CASE("the Tab popup filters live, moves with arrows, Enter inserts, Esc leaves the text") {
    InputLine line = typed(InputKind::Path, "wo");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Consumed);
    REQUIRE(line.popup() != nullptr);
    CHECK(line.popup()->query == "wo");
    CHECK(has_item(line, "work/"));
    CHECK(has_item(line, "work/plan.md"));
    CHECK(line.handle(Key::Char, "r") == InputLine::Result::Consumed);
    CHECK(line.popup()->query == "wor");
    CHECK(line.text() == "wo");  // süzgeç metne yazılmaz
    line.handle(Key::Char, "k");
    line.handle(Key::Char, "/");
    line.handle(Key::Char, "n");
    REQUIRE(line.popup()->items == std::vector<std::string>{"work/plan.md", "work/plain.txt"});
    line.handle(Key::Up);
    CHECK(line.popup()->selected == 0);
    line.handle(Key::Down);
    line.handle(Key::Down);
    CHECK(line.popup()->selected == 1);
    line.handle(Key::Backspace);
    CHECK(line.popup()->query == "work/");
    line.handle(Key::Char, "n");
    line.handle(Key::Down);
    CHECK(line.handle(Key::Enter) == InputLine::Result::Edited);
    CHECK(line.popup() == nullptr);
    CHECK(line.text() == "work/plain.txt");
    CHECK(line.cursor() == line.text().size());

    line.set_text("wo");
    line.handle(Key::Tab);
    CHECK(line.handle(Key::Escape) == InputLine::Result::Consumed);
    CHECK(line.popup() == nullptr);
    CHECK(line.text() == "wo");
}

TEST_CASE("the popup swallows other keys and an empty result inserts nothing") {
    InputLine line = typed(InputKind::Path, "wo");
    line.handle(Key::Tab);
    CHECK(line.handle(Key::Left) == InputLine::Result::Consumed);
    CHECK(line.handle(Key::CtrlP) == InputLine::Result::Consumed);
    line.handle(Key::Char, "q");
    line.handle(Key::Char, "q");
    CHECK(line.popup()->items.empty());
    CHECK(line.handle(Key::Enter) == InputLine::Result::Consumed);
    CHECK(line.popup() == nullptr);
    CHECK(line.text() == "wo");
}

TEST_CASE("a type candidate replaces only the extension of a path") {
    InputLine line = typed(InputKind::Path, "yeni.c");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Edited);  // `.cpp` tek
    CHECK(line.text() == "yeni.cpp");
    line.set_text("work/yeni.");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    CHECK(has_item(line, ".md"));
    CHECK(has_item(line, ".txt"));
    CHECK_FALSE(has_item(line, "work/"));
}

TEST_CASE("a multi-dot name completes the last extension only") {
    InputLine line = typed(InputKind::Path, "v1.2.cp");
    CHECK(line.handle(Key::Tab) == InputLine::Result::Edited);
    CHECK(line.text() == "v1.2.cpp");
}

TEST_CASE("search popup lists argument keys, folders, names and types") {
    InputLine line = typed(InputKind::Search, "");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    CHECK(has_item(line, "sort:"));
    CHECK(has_item(line, "sync:"));
    CHECK(line.popup()->items.size() == InputLine::kPopupRows);
    line.handle(Key::Escape);
    line.set_text("wo");
    line.handle(Key::Tab);
    CHECK(has_item(line, "work/"));
    CHECK(has_item(line, "work/plan.md"));
    line.handle(Key::Escape);
    line.set_text(".cp");
    line.handle(Key::Tab);
    CHECK(has_item(line, ".cpp"));
}

TEST_CASE("search popup completes argument values and replaces only the word under the cursor") {
    InputLine line = typed(InputKind::Search, "plan sort:");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    CHECK(line.popup()->items ==
          std::vector<std::string>{"sort:name", "sort:size", "sort:updated", "sort:created"});
    line.handle(Key::Down);
    line.handle(Key::Enter);
    CHECK(line.text() == "plan sort:size");

    line.set_text("type:");
    line.handle(Key::Tab);
    CHECK(sorted_items(line) == std::vector<std::string>{"type:cpp", "type:md", "type:txt"});
    line.handle(Key::Escape);
    line.set_text("in:wo");
    line.handle(Key::Tab);
    CHECK(line.text() == "in:work/");
    line.set_text("origin:");
    line.handle(Key::Tab);
    CHECK(sorted_items(line) == std::vector<std::string>{"origin:android", "origin:linux", "origin:macos",
                                                         "origin:unknown", "origin:windows"});
    line.handle(Key::Escape);
    line.set_text("x so");
    line.set_text("x so y", 4);
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    CHECK(line.popup()->items.front() == "sort:");
    line.handle(Key::Enter);
    CHECK(line.text() == "x sort: y");
    CHECK(line.cursor() == 7);
}

TEST_CASE("every offered argument value is accepted by parse_query") {
    for (const std::string_view key : {"sort:", "size:", "updated:", "created:", "origin:", "sync:"}) {
        InputLine line = typed(InputKind::Search, key);
        line.handle(Key::Tab);
        REQUIRE(line.popup() != nullptr);
        for (const std::string& item : line.popup()->items) {
            CAPTURE(item);
            const auto parsed = search::parse_query(item);
            REQUIRE(parsed.has_value());
            CHECK(parsed->text.empty());
        }
    }
}

TEST_CASE("a candidate with a space is quoted in search and stays one word") {
    InputLine line = typed(InputKind::Search, "a b");
    line.set_text("a");
    line.set_text("in:my");
    line.handle(Key::Tab);
    CHECK(line.text() == "in:\"my notes/\"");
    CHECK(search::parse_query(line.text()).has_value());
    line.set_text("x \"my no");
    line.handle(Key::Tab);
    REQUIRE(line.popup() != nullptr);
    CHECK(line.popup()->query == "my no");
    CHECK(has_item(line, "my notes/"));
}

TEST_CASE("search spans color valid arguments and mark the located invalid value") {
    const auto spans = tui::search_spans("plan sort:size çay");
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].kind == InputSpan::Kind::Key);
    CHECK(spans[0].begin == 5);
    CHECK(spans[0].end == 10);
    CHECK(spans[1].kind == InputSpan::Kind::Value);
    CHECK(spans[1].end == 14);
    CHECK(tui::search_spans("plan foo:bar http://x").empty());

    const std::string query = "çay sync:nope";
    const auto invalid = tui::search_spans(query);
    const auto parsed = search::parse_query(query);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(invalid.size() == 2);
    CHECK(invalid[0].kind == InputSpan::Kind::Key);
    CHECK(invalid[1].kind == InputSpan::Kind::Invalid);
    CHECK(invalid[1].begin == parsed.error().begin);
    CHECK(invalid[1].end == parsed.error().end);

    // Yalnız ilk hatayı değil her bozuk değeri gösterir.
    CHECK(tui::search_spans("sync:no sort:no").size() == 4);
    const auto empty_value = tui::search_spans("sort:");
    REQUIRE(empty_value.size() == 1);
    CHECK(empty_value[0].kind == InputSpan::Kind::Invalid);
    CHECK(tui::search_spans("SORT:name").size() == 2);
    CHECK(tui::search_spans("in:\"my notes/\"").size() == 2);
}

TEST_CASE("Ctrl+P and Ctrl+N walk history filtered by the current text and restore it") {
    const std::vector<std::string> recent = {"plan c", "todo", "PLAN b", "x plan"};
    InputLine line = typed(InputKind::Search, "plan");
    CHECK(line.handle(Key::CtrlN, {}, recent) == InputLine::Result::Consumed);  // daha yeni yok
    CHECK(line.handle(Key::CtrlP, {}, recent) == InputLine::Result::Edited);
    CHECK(line.text() == "plan c");
    line.handle(Key::CtrlP, {}, recent);
    CHECK(line.text() == "PLAN b");  // katlamalı alt dize: todo atlanır
    line.handle(Key::CtrlP, {}, recent);
    CHECK(line.text() == "x plan");
    CHECK(line.handle(Key::CtrlP, {}, recent) == InputLine::Result::Consumed);  // sonda kalır
    CHECK(line.text() == "x plan");
    CHECK(line.cursor() == line.text().size());
    line.handle(Key::CtrlN, {}, recent);
    line.handle(Key::CtrlN, {}, recent);
    CHECK(line.text() == "plan c");
    line.handle(Key::CtrlN, {}, recent);
    CHECK(line.text() == "plan");  // yazılan metne döner

    line.handle(Key::CtrlP, {}, recent);
    line.handle(Key::Char, "!", recent);
    CHECK(line.text() == "plan c!");
    CHECK(line.handle(Key::CtrlP, {}, recent) == InputLine::Result::Consumed);  // yazım yeni süzgeç
}

TEST_CASE("Ctrl+P and Ctrl+N are not history outside search inputs") {
    InputLine line = typed(InputKind::Find, "x");
    const std::vector<std::string> recent = {"x y"};
    CHECK(line.handle(Key::CtrlP, {}, recent) == InputLine::Result::Ignored);
    CHECK(line.handle(Key::CtrlN, {}, recent) == InputLine::Result::Ignored);
    CHECK(line.text() == "x");
}

TEST_CASE("remember_search keeps newest first, dedupes and caps") {
    std::vector<std::string> history;
    CHECK(tui::remember_search(history, "plan", nullptr));
    CHECK(tui::remember_search(history, "todo", nullptr));
    CHECK(tui::remember_search(history, "plan", nullptr));
    CHECK(history == std::vector<std::string>{"plan", "todo"});
    CHECK_FALSE(tui::remember_search(history, "plan", nullptr));
    CHECK_FALSE(tui::remember_search(history, "   ", nullptr));
    CHECK_FALSE(tui::remember_search(history, "\xFF\xFE", nullptr));
    for (int i = 0; i < 250; ++i)
        tui::remember_search(history, "q" + std::to_string(i), nullptr);
    CHECK(history.size() == tui::SearchHistoryLimits{}.entries);
    CHECK(history.front() == "q249");
}

TEST_CASE("remember_search refuses an overlong entry, the state file also drops it") {
    std::vector<std::string> history;
    const std::string longest(tui::SearchHistoryLimits{}.entry_bytes, 'q');
    CHECK(tui::remember_search(history, longest, nullptr));
    CHECK_FALSE(tui::remember_search(history, longest + "q", nullptr));
    CHECK(history.size() == 1);
    CHECK(tui::parse_search_history("[\"" + longest + "q\", \"ok\"]") == std::vector<std::string>{"ok"});
}

// L2: limits.tui.search_history ve search_entry_bytes 0 (SIZE_MAX) ise eski tavanların üstü de kalır.
TEST_CASE("search history keeps more than the old 200 entries and longer queries when unlimited") {
    const tui::SearchHistoryLimits unlimited{.entries = SIZE_MAX, .entry_bytes = SIZE_MAX};
    std::vector<std::string> history;
    for (int i = 0; i < 250; ++i)
        REQUIRE(tui::remember_search(history, "q" + std::to_string(i), nullptr, unlimited));
    CHECK(history.size() == 250);
    std::string longer;
    while (longer.size() <= tui::SearchHistoryLimits{}.entry_bytes)
        longer += "arama ";
    longer += "son";
    CHECK(tui::remember_search(history, longer, nullptr, unlimited));
    CHECK(tui::parse_search_history(tui::format_search_history(history), unlimited).size() == 251);
    CHECK(tui::parse_list_prefs("search_history = " + tui::format_search_history(history) + "\n", unlimited)
              .search_history.size() == 251);
}

TEST_CASE("remember_search refuses token-like values and note names but keeps arguments") {
    const auto src = sources();
    std::vector<std::string> history;
    for (const std::string_view secret :
         {"API_TOKEN=abc123", "x TOKEN=abc123", "export KEY=abc", "password: hunter2",
          "ghp_16C7e42F292c6912E7710c838347Ae178B4a", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.abc",
          "sort:ghp_16C7e42F292c6912E7710c838347Ae178B4a", "in:TOKEN=abc123", "api_key:abc123"})
        CHECK_MESSAGE(!tui::remember_search(history, secret, src.get()), secret);
    for (const std::string_view name : {"work/plan.md", "plan.md", "sort:name plan.md", "\"a b.md\""})
        CHECK_MESSAGE(!tui::remember_search(history, name, src.get()), name);
    CHECK(history.empty());
    for (const std::string_view fine :
         {"plan", "sort:name plan", "type:md in:work/", "size:>1mb a=", "kısa 2026"})
        CHECK_MESSAGE(tui::remember_search(history, fine, src.get()), fine);
}

TEST_CASE("search history round-trips through tui-state.toml and hostile lines are dropped") {
    const std::vector<std::string> history = {"say \"hi\"", "back\\slash", "çay 1", "sort:name plan",
                                              "type:md in:work/"};
    tui::ListPrefs prefs;
    prefs.search_history = history;
    const std::string saved = tui::format_list_prefs(prefs);
    CHECK(saved.find("search_history = [") != std::string::npos);
    CHECK(tui::parse_list_prefs(saved).search_history == history);
    // Sütun ve sıra bozulmaz; geçmişsiz eski dosya ve yazımı boş geçmiş.
    CHECK(tui::parse_list_prefs(saved).column == tui::ListColumn::SizeAndDate);
    CHECK(tui::parse_list_prefs("column = \"size\"\n").search_history.empty());
    CHECK(tui::format_list_prefs({}).find("search_history") == std::string::npos);

    CHECK(tui::parse_search_history(R"(["ok", "TOKEN=abc123", "a", "a", "\xFF", "kesik)") ==
          std::vector<std::string>{"ok", "a", "\\xFF"});
    CHECK(tui::parse_search_history("nope").empty());
    std::string big = "[";
    for (int i = 0; i < 300; ++i)
        big += "\"q" + std::to_string(i) + "\",";
    CHECK(tui::parse_search_history(big + "]").size() == tui::SearchHistoryLimits{}.entries);
}

TEST_CASE("search: Tab opens the popup, Ctrl+T toggles name and content, nothing leaves the input") {
    auto s = search_state();
    type_chars(s, "wo");
    CHECK(s.handle(Key::Tab) == Intent::None);
    CHECK(s.focus() == Focus::Search);
    CHECK(s.mode() == Mode::Name);
    REQUIRE(s.search_line().popup() != nullptr);
    CHECK(s.handle(Key::Down) == Intent::None);
    CHECK(s.selected() == 0);  // açık listede ok sonuçları değil listeyi gezer
    CHECK(s.handle(Key::Escape) == Intent::None);
    CHECK(s.focus() == Focus::Search);  // ilk Esc yalnız listeyi kapatır
    CHECK(s.query() == "wo");
    s.handle(Key::Escape);
    CHECK(s.focus() == Focus::List);

    auto toggled = search_state();
    toggled.set_content_searcher([](std::string_view) { return std::vector<search::ContentResult>{}; });
    CHECK(toggled.handle(Key::CtrlT) == Intent::None);
    CHECK(toggled.mode() == Mode::Content);
    CHECK(toggled.focus() == Focus::Search);
    toggled.handle(Key::CtrlT);
    CHECK(toggled.mode() == Mode::Name);
}

TEST_CASE("search: inserting from the popup refilters the list") {
    auto s = search_state();
    type_chars(s, "tod");
    CHECK(s.rows().size() == 1);
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    type_chars(s, "type:");
    s.handle(Key::Tab);
    REQUIRE(s.search_line().popup() != nullptr);
    const std::string chosen = s.search_line().popup()->items.front();
    s.handle(Key::Enter);
    CHECK(s.query() == chosen);
    REQUIRE(s.rows().size() == 1);
    CHECK(s.search_line().popup() == nullptr);
}

TEST_CASE("search: Ctrl+P and Ctrl+N walk history, arrows keep moving results") {
    auto s = search_state();
    type_chars(s, "plan");
    CHECK(s.handle(Key::Enter) == Intent::SavePrefs);
    CHECK(s.list_prefs().search_history == std::vector<std::string>{"plan"});
    CHECK(s.focus() == Focus::List);
    s.handle(Key::Char, "/");
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    CHECK(s.query().empty());
    s.handle(Key::CtrlP);
    CHECK(s.query() == "plan");
    CHECK(s.rows().size() == 2);  // süzgeç yenilendi: plan.md, plain.txt
    s.handle(Key::CtrlN);
    CHECK(s.query().empty());
    CHECK(s.rows().size() == 2);  // boş süzgeç: klasör görünümü, work/ ve personal/
    s.handle(Key::Down);
    CHECK(s.selected() == 1);
    s.handle(Key::Up);
    CHECK(s.selected() == 0);
    CHECK(s.focus() == Focus::Search);
}

TEST_CASE("search: Enter stores valid queries only and a new note from the query is not history") {
    auto s = search_state();
    CHECK(s.handle(Key::Enter) == Intent::None);  // boş sorgu
    s.handle(Key::Char, "/");
    type_chars(s, "sync:nope");
    s.handle(Key::Enter);
    CHECK(s.list_prefs().search_history.empty());
    s.handle(Key::Escape);

    auto none = search_state();
    type_chars(none, "zzzqq");
    CHECK(none.handle(Key::Enter) == Intent::CreateFromQuery);
    CHECK(none.list_prefs().search_history.empty());

    auto global = search_state();
    global.handle(Key::Escape);
    global.handle(Key::Char, "f");
    global.set_global_searcher([](std::string_view, std::span<const core::NoteMeta> notes)
                                   -> std::expected<std::vector<search::GlobalHit>, search::QueryError> {
        std::vector<search::GlobalHit> hits;
        hits.push_back({.index = 0, .path = notes.front().path});
        return hits;
    });
    type_chars(global, "pla");
    CHECK(global.handle(Key::Enter) == Intent::SavePrefs);
    CHECK(global.focus() == Focus::Preview);
    CHECK(global.list_prefs().search_history == std::vector<std::string>{"pla"});
}

TEST_CASE("the `a` row offers ghost and popup; Esc and Enter close the popup first") {
    tui::NewRowState row(sources());
    row.handle(Key::Char, "w");
    row.handle(Key::Char, "o");
    CHECK(row.ghost() == "rk/");
    CHECK(row.handle(Key::Right) == tui::NewRowState::Outcome::Editing);
    CHECK(row.text() == "work/");
    row.handle(Key::Tab);
    REQUIRE(row.line().popup() != nullptr);
    CHECK(row.handle(Key::Escape) == tui::NewRowState::Outcome::Editing);
    CHECK(row.line().popup() == nullptr);
    row.handle(Key::Tab);
    row.handle(Key::Down);
    CHECK(row.handle(Key::Enter) == tui::NewRowState::Outcome::Editing);
    CHECK(row.text() != "work/");
    CHECK(row.line().popup() == nullptr);
    CHECK(row.handle(Key::Enter) == tui::NewRowState::Outcome::Submit);
    CHECK(row.handle(Key::Escape) == tui::NewRowState::Outcome::Cancelled);
}

TEST_CASE("F2 offers ghost and popup and keeps its cursor rules") {
    tui::RenameState rename(core::NotePath::parse("work/plan.md").value(), sources());
    CHECK(rename.ghost().empty());  // imleç uzantıdan önce
    rename.handle(Key::End);
    for (int i = 0; i < 12; ++i)
        rename.handle(Key::Backspace);
    rename.handle(Key::Char, "p");
    rename.handle(Key::Char, "e");
    CHECK(rename.ghost() == "rsonal/");
    CHECK(rename.handle(Key::Right) == tui::RenameState::Outcome::Editing);
    CHECK(rename.text() == "personal/");
    rename.handle(Key::Char, "t");
    rename.set_error("x");
    rename.handle(Key::Tab);  // personal/todo.cpp tek aday
    CHECK(rename.text() == "personal/todo.cpp");
    CHECK(rename.error().empty());
}

TEST_CASE("editor name prompt and find get the input line") {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::EditorSession session{tui::EditorHost{
        store,
        mutex,
        internal,
        [](char32_t) { return 1; },
        std::uintmax_t{1024} * 1024,
        core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("md"); },
        [](const core::NotePath&) {},
        [] {},
        [](std::optional<std::size_t>) {},
        [](const core::Error&) {},
        [] {},
    }};
    core::init_i18n("en");
    session.layout(60, 10);
    session.begin_new_note("");
    for (const char& c : std::string("lemon lemonade "))
        session.handle(Key::Char, std::string_view(&c, 1));
    session.handle(Key::CtrlF);
    session.handle(Key::Char, "l");
    session.handle(Key::Char, "e");
    CHECK(session.find_line().ghost() == "mon");
    CHECK(session.find_label() == "1/2");
    session.handle(Key::Right);
    CHECK(session.find_query() == "lemon");
    CHECK(session.find_label() == "1/2");
    session.handle(Key::CtrlN);  // Ctrl+N/P burada eşleşme gezer, geçmiş değil
    CHECK(session.find_label() == "2/2");
    session.handle(Key::CtrlP);
    CHECK(session.find_label() == "1/2");
    session.handle(Key::Tab);
    REQUIRE(session.find_line().popup() != nullptr);
    session.handle(Key::Down);
    session.handle(Key::Enter);
    CHECK(session.find_query() == "lemonade");
    CHECK(session.find_label() == "1/1");
    CHECK(session.mode() == tui::EditorSession::Mode::Find);
    session.handle(Key::Escape);
    CHECK(session.mode() == tui::EditorSession::Mode::Editing);

    const std::vector<core::NoteMeta> notes = {meta("work/plan.md")};
    session.handle(Key::CtrlS, {}, tui::build_input_sources(notes));
    REQUIRE(session.mode() == tui::EditorSession::Mode::NamePrompt);
    for (std::size_t i = session.name_input().size(); i > 0; --i)
        session.handle(Key::Backspace);
    CHECK(session.name_input().empty());
    session.handle(Key::Char, "w");
    CHECK(session.name_line().ghost() == "ork/");
    session.handle(Key::Tab);
    REQUIRE(session.name_line().popup() != nullptr);
    session.handle(Key::Escape);
    CHECK(session.mode() == tui::EditorSession::Mode::NamePrompt);  // yalnız liste kapandı
    CHECK(session.name_input() == "w");
}

TEST_CASE("the search bar paints keys, values and invalid red around a UTF-8 cursor") {
    core::init_i18n("en");
    const int y = 23;
    auto s = search_state();
    type_chars(s, "type:md ");
    s.handle(Key::Char, "ç");
    s.handle(Key::Char, " ");
    type_chars(s, "type:cpp");
    auto screen = draw_state(s);
    const auto key_at = column_of(screen, y, "type:");
    REQUIRE(key_at.has_value());
    const auto key_color = screen.PixelAt(*key_at, y).foreground_color;
    const auto value_color = screen.PixelAt(*key_at + 5, y).foreground_color;
    CHECK(key_color != ftxui::Color(ftxui::Color::Default));
    CHECK(value_color != ftxui::Color(ftxui::Color::Default));
    CHECK(key_color != value_color);
    const auto plain_at = column_of(screen, y, "ç");
    REQUIRE(plain_at.has_value());
    CHECK(screen.PixelAt(*plain_at, y).foreground_color == ftxui::Color(ftxui::Color::Default));
    CHECK(screen.PixelAt(*plain_at - 2, y).foreground_color == value_color);

    // İmleç çok baytlı karakter ve renk sınırının içinde kalır: metin bölünmez, renkler yerinde.
    for (int i = 0; i < 9; ++i)
        s.handle(Key::Left);
    const auto moved = draw_state(s);
    const auto cursor_at = column_of(moved, y, "ç▏ type:cpp");
    REQUIRE(cursor_at.has_value());
    CHECK(moved.PixelAt(*cursor_at, y).foreground_color == ftxui::Color(ftxui::Color::Default));
    CHECK(moved.PixelAt(*cursor_at + 3, y).foreground_color == key_color);

    auto invalid = search_state();
    type_chars(invalid, "sync:nope");
    const auto red = draw_state(invalid);
    const auto bad_at = column_of(red, y, "nope");
    REQUIRE(bad_at.has_value());
    CHECK(red.PixelAt(*bad_at, y).foreground_color == ftxui::Color(ftxui::Color::Red));
}

TEST_CASE("a long query error never hides the input: the error yields to half of the line") {
    core::init_i18n("en");
    const int y = 23;
    auto s = search_state();
    type_chars(s, "plan sort:size sync:nope");
    const auto screen = draw_state(s);
    const auto bad_at = column_of(screen, y, "nope");
    REQUIRE(bad_at.has_value());
    CHECK(column_of(screen, y, "plan sort:size sync:nope▏").has_value());
    CHECK(screen.PixelAt(*bad_at, y).foreground_color == ftxui::Color(ftxui::Color::Red));
    CHECK(column_of(screen, y, "Invalid search value").has_value());  // hatanın başı kalır
}

TEST_CASE("the ghost is dim text after the cursor and leaves the typed text plain") {
    core::init_i18n("en");
    const int y = 23;
    auto s = search_state();
    type_chars(s, "pla");
    const auto screen = draw_state(s);
    const auto ghost_at = column_of(screen, y, "in.txt");
    const auto typed_at = column_of(screen, y, "pla");
    REQUIRE(ghost_at.has_value());
    REQUIRE(typed_at.has_value());
    CHECK(screen.PixelAt(*ghost_at, y).dim);
    CHECK_FALSE(screen.PixelAt(*typed_at, y).dim);
}

TEST_CASE("input width counts unsafe characters as the replacement cell that is drawn") {
    // Ham metin ölçülüp terminal_safe metin çizilirse fark ipucu sütununu kaydırır: U+202E, ESC ve bozuk
    // bayt U+FFFD (1 hücre) olarak çizilir.
    for (const std::string unsafe : {"\xE2\x80\xAE", "\x1B", "\x80"}) {
        CAPTURE(unsafe);
        InputLine line(InputKind::Path);
        line.set_text("a" + unsafe + "b" + unsafe, 1 + unsafe.size());  // imleçten önce ve sonra birer tane
        const tui::InputCells result = tui::input_text_cells(line, {}, {}, 40);
        const ftxui::Element drawn = ftxui::hbox(result.cells);
        drawn->ComputeRequirement();
        CHECK(result.width == drawn->requirement().min_x - 1);  // imleç `▏` genişliğe dahil değil
    }
}

TEST_CASE("open popups are drawn with the filter and items") {
    core::init_i18n("en");
    auto s = search_state();
    type_chars(s, "wo");
    s.handle(Key::Tab);
    auto text_of = [](const ftxui::Screen& screen) {
        std::string all;
        for (int y = 0; y < screen.dimy(); ++y)
            for (int x = 0; x < screen.dimx(); ++x)
                all += screen.PixelAt(x, y).character;
        return all;
    };
    CHECK(text_of(draw_state(s)).find("work/plain.txt") != std::string::npos);

    auto idle = search_state();
    idle.handle(Key::Escape);
    tui::NewRowState row(sources());
    row.handle(Key::Char, "t");
    row.handle(Key::Char, "o");
    CHECK(text_of(draw_state(idle, &row)).find("do.cpp") != std::string::npos);  // gölge
    row.handle(Key::Backspace);
    row.handle(Key::Backspace);
    row.handle(Key::Char, "w");
    row.handle(Key::Tab);
    CHECK(text_of(draw_state(idle, &row)).find("work/plain.txt") != std::string::npos);

    tui::RenameState rename(core::NotePath::parse("work/plan.md").value(), sources());
    rename.handle(Key::End);
    rename.handle(Key::Backspace);
    rename.handle(Key::Backspace);
    rename.handle(Key::Backspace);
    CHECK(text_of(draw_state(idle, nullptr, &rename)).find("work/plan▏.md") != std::string::npos);  // gölge
    rename.handle(Key::Home);
    rename.handle(Key::Delete);
}
