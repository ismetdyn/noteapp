#include "app/update_controller.hpp"
#include "core/i18n.hpp"
#include "search/content_index.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "support/text_pdf.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Key;

namespace {

constexpr int kWidth = 80;
constexpr int kHeight = 15;

core::NoteInfo note(const std::string& path, int age_hours = 0) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(100000 - age_hours), 1};
}

tui::TuiState sample_state() {
    tui::TuiState s;
    s.set_notes({note("work-meeting.md", 0), note("personal-todo.md", 1), note("snippets-sort.java", 2)});
    return s;
}

tui::Preview text_preview(std::vector<std::string> lines) {
    tui::Preview p;
    p.kind = tui::Preview::Kind::Text;
    p.lines = std::move(lines);
    return p;
}

ftxui::Screen render_screen(const tui::TuiState& state, const tui::Preview& preview, int width = kWidth,
                            int height = kHeight, const std::string& message = "") {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, preview, {"sync off", message}, {width, height}));
    return screen;
}

// UTF-8 metni kod noktalarına böler (hücreler grapheme başına bir kod noktası tutar bu testlerde).
std::vector<std::string> code_points(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 1;
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

// İlk hücresinin (x, y) konumunu döner; ekran hücre ızgarası olduğundan kenarlık baytlarından etkilenmez.
std::optional<std::pair<int, int>> find_text(const ftxui::Screen& screen, const std::string& needle) {
    const auto cps = code_points(needle);
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x + static_cast<int>(cps.size()) <= screen.dimx(); ++x) {
            bool match = true;
            for (std::size_t i = 0; i < cps.size() && match; ++i)
                match = screen.PixelAt(x + static_cast<int>(i), y).character == cps[i];
            if (match)
                return std::pair{x, y};
        }
    return std::nullopt;
}

bool bold_at(const ftxui::Screen& screen, const std::string& needle, int char_index) {
    const auto pos = find_text(screen, needle);
    REQUIRE(pos.has_value());
    return screen.PixelAt(pos->first + char_index, pos->second).bold;
}

bool contains(const ftxui::Screen& screen, const std::string& needle) {
    return find_text(screen, needle).has_value();
}

}  // namespace

TEST_CASE("initial render shows list, preview, counts, sync status and key hints") {
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({"# Toplantı notları", "- bütçe onayı"}));
    CHECK(contains(screen, "work-meeting.md"));
    CHECK(contains(screen, "personal-todo.md"));
    CHECK(contains(screen, "snippets-sort.java"));
    CHECK(contains(screen, "[name] · 3 notes"));
    CHECK(contains(screen, "sync off"));
    CHECK(contains(screen, "# Toplantı notları"));
    CHECK(contains(screen, "Enter edit"));
}

TEST_CASE("filtered list highlights exactly the matched characters") {
    auto state = sample_state();
    state.handle(Key::Char, "/");
    for (const char c : std::string("tod"))
        state.handle(Key::Char, std::string(1, c));
    const auto screen = render_screen(state, text_preview({}));
    CHECK(contains(screen, "1/3"));
    CHECK_FALSE(contains(screen, "work-meeting.md"));
    CHECK(bold_at(screen, "todo.md", 0));
    CHECK(bold_at(screen, "todo.md", 1));
    CHECK(bold_at(screen, "todo.md", 2));
    CHECK_FALSE(bold_at(screen, "todo.md", 3));
    CHECK_FALSE(bold_at(screen, "personal", 0));
}

TEST_CASE("only the selected row is inverted and marked") {
    auto state = sample_state();
    state.handle(Key::Down);
    const auto screen = render_screen(state, text_preview({}));
    const auto selected = find_text(screen, "personal-todo.md");
    const auto other = find_text(screen, "work-meeting.md");
    REQUIRE(selected);
    REQUIRE(other);
    CHECK(screen.PixelAt(selected->first, selected->second).inverted);
    CHECK_FALSE(screen.PixelAt(other->first, other->second).inverted);
    CHECK(contains(screen, " 2   personal-todo.md"));
}

TEST_CASE("a window below 60x15 shows only the centered too-small message") {
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({}), 50, 10);
    CHECK(contains(screen, "Window too small"));
    CHECK_FALSE(contains(screen, "work-meeting.md"));
    const auto narrow = render_screen(state, text_preview({}), 59, 20);
    CHECK(contains(narrow, "Window too small"));
}

TEST_CASE("Turkish names render and match through the folded search") {
    tui::TuiState state;
    state.set_notes({note("çalışma-öğrenci-notları.md", 0), note("kişisel-İstanbul.md", 1)});
    const std::vector<std::string> lines = {"çğıİöşü ÇĞIÖŞÜ"};
    auto screen = render_screen(state, text_preview(lines), 120);  // liste 1/3: uzun ad sığsın
    CHECK(contains(screen, "çalışma-öğrenci-notları.md"));
    CHECK(contains(screen, "çğıİöşü ÇĞIÖŞÜ"));

    state.handle(Key::Char, "/");
    state.handle(Key::Char, "o");
    state.handle(Key::Char, "g");
    state.handle(Key::Char, "r");
    screen = render_screen(state, text_preview(lines));
    CHECK(contains(screen, "1/2"));
    CHECK(bold_at(screen, "öğrenci", 0));  // ö, 'o' sorgusuyla eşleşti
    CHECK(bold_at(screen, "öğrenci", 1));
}

TEST_CASE("Turkish catalog is used when the language is tr") {
    core::init_i18n("tr");
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({}));
    CHECK(contains(screen, "[ad] · 3 not"));
    CHECK(contains(screen, "Enter düzenle"));
    core::init_i18n("en");
}

TEST_CASE("a transient message replaces the key hints bar") {
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({}), kWidth, kHeight, "Not available yet");
    CHECK(contains(screen, "Not available yet"));
    CHECK_FALSE(contains(screen, "Enter open"));
}

TEST_CASE("preview scrolls and the list follows a selection below the fold") {
    tui::TuiState state;
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < 30; ++i)
        notes.push_back(note("n" + std::to_string(100 + i) + ".md", i));
    state.set_notes(notes);
    for (int i = 0; i < 25; ++i)
        state.handle(Key::Down);
    std::vector<std::string> lines;
    for (int i = 0; i < 40; ++i)
        lines.push_back("line" + std::to_string(1000 + i));
    state.set_preview_line_count(lines.size());
    state.scroll_preview(static_cast<int>(tui::TuiState::kPreviewPageLines));
    const auto screen = render_screen(state, text_preview(lines));
    CHECK(contains(screen, "  n125.md"));
    CHECK_FALSE(contains(screen, "n100.md"));
    CHECK(contains(screen, "line1010"));
    CHECK_FALSE(contains(screen, "line1009"));
}

TEST_CASE("content mode lists hit counts and the preview highlights the first hit") {
    test::TempDir dir;
    std::string long_note;
    for (int i = 1; i <= 30; ++i)
        long_note += (i == 20 ? "the needle is here\n" : "filler " + std::to_string(1000 + i) + "\n");
    test::upsert_note(dir.path(), "long.md", long_note);
    test::upsert_note(dir.path(), "few.md", "one needle\nsecond needle\n");
    test::upsert_note(dir.path(), "none.md", "nothing\n");
    const core::NoteStore store(dir.path());
    search::ContentIndex index(store, 1024 * 1024, 3);
    index.refresh();

    tui::TuiState state;
    state.set_notes(store.list());
    state.set_content_searcher([&index](std::string_view query) { return index.search(query, 100); });
    state.handle(Key::Tab);
    state.handle(Key::Char, "/");
    for (const char c : std::string("needle"))
        state.handle(Key::Char, std::string(1, c));

    tui::PreviewCache cache;
    // Eşleşme sayısı çoksa önce o gelir: few.md (2 satır) > long.md (1 satır)
    REQUIRE(state.rows().size() == 2);
    CHECK(state.rows()[0].path.string() == "few.md");
    state.handle(Key::Down);  // long.md: ilk eşleşme 20. satır
    const auto& preview = cache.get(store, state.selected_row());
    state.set_preview_line_count(preview.lines.size());
    const auto screen = render_screen(state, preview);

    CHECK(contains(screen, "[content] · 2/3"));
    CHECK(contains(screen, "few.md  (2)"));
    CHECK(contains(screen, "long.md  (1)"));
    CHECK_FALSE(contains(screen, "none.md"));
    CHECK(contains(screen, "the needle is here"));
    CHECK_FALSE(contains(screen, "filler 1001"));  // eşleşmeye kaydırıldı
    CHECK(bold_at(screen, "needle is here", 0));
    CHECK(bold_at(screen, "needle is here", 5));
    CHECK_FALSE(bold_at(screen, "needle is here", 7));  // "is" vurgusuz
    CHECK_FALSE(bold_at(screen, "the needle", 0));
}

TEST_CASE("PreviewCache reads text, flags binary notes and reports unreadable ones") {
    test::TempDir dir;
    {
        test::upsert_note(dir.path(), "a.md", "first\r\nsecond\tline\nlast");
        test::upsert_note(dir.path(), "b.bin", std::string("x\0y", 3));
    }
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;

    tui::Row text_row{core::NotePath::parse("a.md").value(), {}, 0, {}};
    const auto& text = cache.get(store, &text_row);
    REQUIRE(text.kind == tui::Preview::Kind::Text);
    CHECK(text.lines == std::vector<std::string>{"first", "second\tline", "last"});

    tui::Row binary_row{core::NotePath::parse("b.bin").value(), {}, 0, {}};
    CHECK(cache.get(store, &binary_row).kind == tui::Preview::Kind::Binary);
    const auto screen = render_screen(sample_state(), cache.get(store, &binary_row));
    CHECK(contains(screen, "binary file"));

    tui::Row missing_row{core::NotePath::parse("gone.md").value(), {}, 0, {}};
    CHECK(cache.get(store, &missing_row).kind == tui::Preview::Kind::Error);
    CHECK(cache.get(store, nullptr).kind == tui::Preview::Kind::Empty);
}

TEST_CASE("PreviewCache shows a PDF note's text under page separators after a loading line") {
    core::init_i18n("en");
    test::TempDir dir;
    test::upsert_note(dir.path(), "doc.pdf", test::text_pdf({"Alpha", "Beta"}));
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;
    std::atomic<int> changes{0};
    cache.set_pdf({}, NOTEAPP_NOTE_EXE, [&changes] { ++changes; });
    tui::Row row{core::NotePath::parse("doc.pdf").value(), {}, 0, {}};

    const auto& loading = cache.get(store, &row);
    REQUIRE(loading.kind == tui::Preview::Kind::Text);
    CHECK(loading.lines == std::vector<std::string>{"Loading the PDF…"});
    // Her duyurudan sonraki yeniden çizim get'i çağırır: açılıştan sonra sayfalar istenir, gelenler eklenir.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (cache.get(store, &row).text.find("Loading") != std::string::npos &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(changes >= 2);  // açılış ve ilk sayfa kesin; ikinci sayfanın duyurusu saklamadan hemen sonradır
    const auto& shown = cache.get(store, &row);
    REQUIRE(shown.lines.size() >= 4);
    CHECK(shown.lines.front() == "--- page 1/2 ---");
    CHECK(std::ranges::find(shown.lines, "--- page 2/2 ---") != shown.lines.end());
    CHECK(shown.text.find("Alpha") < shown.text.find("--- page 2/2 ---"));
    CHECK(shown.text.find("Beta") > shown.text.find("--- page 2/2 ---"));
    CHECK(shown.text.find("Loading") == std::string::npos);
    auto state = sample_state();
    state.set_preview_line_count(shown.lines.size());
    const auto screen = render_screen(state, shown);
    CHECK(contains(screen, "--- page 1/2 ---"));
    CHECK(contains(screen, "Alpha"));
    cache.close_pdf();
}

TEST_CASE("PreviewCache formats a PDF page like markdown, wraps it to the pane and copies the shown text") {
    core::init_i18n("en");
    test::TempDir dir;
    // 14 pt başlık, 6 pt gövdenin iki satırı (sarılmış tek paragraf), kalın parça; hepsi 200 pt sayfaya sığar.
    pdftest::PdfBuilder builder;
    const std::string content =
        "BT /F1 14 Tf 10 80 Td (Title) Tj ET "
        "BT /F1 6 Tf 10 60 Td (alpha beta gamma delta epsilon zeta eta theta iota) Tj ET "
        "BT /F1 6 Tf 10 53 Td (kappa lambda mu) Tj /F2 6 Tf ( nu xi) Tj ET";
    const std::string fonts =
        "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
        "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >> >> >>";
    test::upsert_note(dir.path(), "styled.pdf", test::text_pdf_detail::finish(builder, {{fonts, content}}));
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;
    cache.set_pdf({}, NOTEAPP_NOTE_EXE, [] {});
    tui::Row row{core::NotePath::parse("styled.pdf").value(), {}, 0, {}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (cache.get(store, &row).text.find("Loading") != std::string::npos &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const tui::Preview& shown = cache.get(store, &row);
    CAPTURE(shown.text);
    CHECK(shown.rendered);
    CHECK_FALSE(shown.display_only);  // gösterilen metin kopyalanır
    REQUIRE(shown.lines.size() == shown.styled.size());
    const auto heading = std::ranges::find(shown.lines, "# Title");
    REQUIRE(heading != shown.lines.end());
    const auto& heading_spans = shown.styled[static_cast<std::size_t>(heading - shown.lines.begin())];
    CHECK(std::ranges::any_of(heading_spans, [](const tui::StyledSpan& span) { return span.look.bold; }));
    // Satırlar paragrafa birleşir, varsayılan 80 sütuna tek satırda sığar; kalın parça kalın görünür.
    const auto body = std::ranges::find(shown.lines, "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi");
    REQUIRE(body != shown.lines.end());
    const std::size_t body_line = static_cast<std::size_t>(body - shown.lines.begin());
    CHECK(std::ranges::any_of(shown.styled[body_line], [&](const tui::StyledSpan& span) {
        return span.look.bold &&
               shown.lines[body_line].substr(span.begin, span.end - span.begin).find("nu xi") != std::string::npos;
    }));
    const std::string copied = tui::selected_text(shown, {.begin_line = body_line, .begin_offset = 0,
                                                          .end_line = body_line, .end_offset = 10});
    CHECK(copied == "alpha beta");

    // Panel daralınca aynı sayfa yeniden sarılır (publish genişliği bildirir).
    tui::TuiState state;
    state.set_notes({note("styled.pdf", 0)});
    state.set_window(40, 30);
    const tui::Preview& narrow = cache.throttled(state, store, std::chrono::steady_clock::now());
    CHECK(std::ranges::find(narrow.lines, "# Title") != narrow.lines.end());
    CHECK(std::ranges::none_of(narrow.lines, [](const std::string& line) { return line.size() > 40; }));
    CHECK(narrow.text.find("alpha beta") != std::string::npos);
    cache.close_pdf();
}

// Senkronla gelen PDF seçilince bellek sayfa x text_bytes'a (~10 GiB) büyümesin: metin view_bytes'ta kesilir, kalan
// sayfalar istenmez.
TEST_CASE("PreviewCache stops reading a PDF at limits.tui.view_bytes and says the text was cut") {
    core::init_i18n("en");
    test::TempDir dir;
    std::vector<std::string> texts;
    for (int page = 1; page <= 40; ++page)
        texts.push_back("Page" + std::to_string(page));
    test::upsert_note(dir.path(), "many.pdf", test::text_pdf(texts));
    const core::NoteStore store(dir.path());
    tui::ViewLimits limits;
    limits.source_bytes = 64;
    tui::PreviewCache cache(limits);
    cache.set_pdf({}, NOTEAPP_NOTE_EXE, [] {});
    tui::Row row{core::NotePath::parse("many.pdf").value(), {}, 0, {}};

    const auto finished = [&] {
        const std::string& text = cache.get(store, &row).text;
        return text.find("text cut at") != std::string::npos || text.find("Loading") == std::string::npos;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!finished() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto& shown = cache.get(store, &row);
    CHECK(shown.text.find("--- page 1/40 ---") == 0);
    CHECK(shown.lines.back().find("text cut at") != std::string::npos);
    CHECK(shown.text.find("--- page 40/40 ---") == std::string::npos);
    REQUIRE(cache.pdf() != nullptr);
    CHECK(cache.pdf()->pending() == 0);
    cache.close_pdf();
}

TEST_CASE("PreviewCache serves the cached note until invalidated") {
    test::TempDir dir;
    test::upsert_note(dir.path(), "a.md", "old");
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;
    tui::Row row{core::NotePath::parse("a.md").value(), {}, 0, {}};
    CHECK(cache.get(store, &row).lines.front() == "old");
    test::upsert_note(dir.path(), "a.md", "new");
    CHECK(cache.get(store, &row).lines.front() == "old");
    cache.invalidate();
    CHECK(cache.get(store, &row).lines.front() == "new");
}

// ---- T51: odak görünümü ----

namespace {

tui::TuiState state_in(Focus focus) {
    auto state = sample_state();
    if (focus == Focus::Search)
        state.handle(Key::Char, "/");
    if (focus == Focus::Preview)
        state.handle(Key::Right);
    return state;
}

// Sol ve sağ panelin üst sol köşesi (üst çubuk 0. satırda). Her panel yuvarlak çerçevelidir; odak
// çerçevenin renginden (odaklı) ya da solukluğundan (odaksız) okunur.
std::string frame_at(const ftxui::Screen& screen, int x) {
    const auto& corner = screen.PixelAt(x, 1);
    if (corner.character != "╭")
        return "not rounded: " + corner.character;
    if (corner.dim)
        return "blurred";
    return corner.foreground_color == ftxui::Color() ? "plain" : "focused";
}

std::string list_corner(const ftxui::Screen& screen) {
    return frame_at(screen, 0);
}

std::string preview_corner(const ftxui::Screen& screen, int width) {
    return frame_at(screen, tui::PaneLayout{}.list_width(width));
}

std::string row_text(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

constexpr std::array kHintKeys = {std::pair{Focus::List, "tui.key_hints_list"},
                                  std::pair{Focus::Preview, "tui.key_hints_preview"}};

// Yardım ipucu katalogda da durur; çubuk onu sağa sabitler.
const std::array<std::pair<const char*, std::string>, 2> kHelpHints = {
    std::pair{"en", std::string("F1 help")}, std::pair{"tr", std::string("F1 yardım")}};

// Çizilmemiş hücre (dolgu) "" taşır; satırı terminalde göründüğü gibi boşluklu verir.
std::string padded_row(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& glyph = screen.PixelAt(x, y).character;
        out += glyph.empty() ? std::string(" ") : glyph;
    }
    while (out.ends_with(' '))
        out.pop_back();
    return out;
}

std::string without_hint(std::string hints, const std::string& hint) {
    if (const std::size_t at = hints.find("  " + hint); at != std::string::npos)
        hints.erase(at, hint.size() + 2);
    else if (hints.starts_with(hint + "  "))
        hints.erase(0, hint.size() + 2);
    return hints;
}

}  // namespace

TEST_CASE("List focus: colored frame on the list, no cursor, inverted selection") {
    const auto state = state_in(Focus::List);
    const auto screen = render_screen(state, text_preview({"body"}));
    CHECK(list_corner(screen) == "focused");
    CHECK(preview_corner(screen, kWidth) == "blurred");
    CHECK_FALSE(contains(screen, "▏"));
    const auto selected = find_text(screen, "work-meeting.md");
    REQUIRE(selected);
    CHECK(screen.PixelAt(selected->first, selected->second).inverted);
}

TEST_CASE("Search focus: the input is on the bottom line, the top line has only mode, count and sync") {
    auto state = state_in(Focus::Search);
    state.handle(Key::Char, "o");
    const auto screen = render_screen(state, text_preview({"body"}));
    CHECK(list_corner(screen) == "blurred");
    CHECK(preview_corner(screen, kWidth) == "blurred");
    CHECK(contains(screen, "> o▏"));
    const auto cursor = find_text(screen, "▏");
    REQUIRE(cursor);
    CHECK(cursor->second == kHeight - 1);
    CHECK_FALSE(row_text(screen, 0).find("> ") != std::string::npos);
    CHECK(row_text(screen, 0).find("[name]") != std::string::npos);
}

TEST_CASE("a filter that stays after Search is visible on the bottom line in List") {
    auto state = state_in(Focus::Search);
    state.handle(Key::Char, "o");
    state.handle(Key::Escape);
    const auto screen = render_screen(state, text_preview({"body"}));
    CHECK(row_text(screen, kHeight - 1).starts_with("> o"));
}

TEST_CASE("Preview focus: colored frame on the preview, selection marked but not inverted") {
    const auto state = state_in(Focus::Preview);
    const auto screen = render_screen(state, text_preview({"body"}));
    CHECK(list_corner(screen) == "blurred");
    CHECK(preview_corner(screen, kWidth) == "focused");
    CHECK_FALSE(contains(screen, "▏"));
    CHECK(contains(screen, " 1   work-meeting.md"));
    const auto selected = find_text(screen, "work-meeting.md");
    REQUIRE(selected);
    CHECK_FALSE(screen.PixelAt(selected->first, selected->second).inverted);
}

TEST_CASE("Preview wraps long lines like cat instead of cutting them; blank lines keep their row") {
    const auto state = state_in(Focus::Preview);
    const std::string tail = "SON";
    const auto screen = render_screen(state, text_preview({std::string(70, 'x') + tail, "", "alt"}));
    CHECK(contains(screen, tail));
    const auto end = find_text(screen, tail);
    const auto below = find_text(screen, "alt");
    REQUIRE(end);
    REQUIRE(below);
    CHECK(below->second == end->second + 2);
}

TEST_CASE("Preview never sends control bytes to a screen cell") {
    const auto state = state_in(Focus::Preview);
    const auto screen = render_screen(state, text_preview({"a\x1b[31mX\x07\xC2\x9B"}));
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x < screen.dimx(); ++x)
            for (const char c : screen.PixelAt(x, y).character) {
                const auto byte = static_cast<unsigned char>(c);
                CHECK((byte >= 0x20 && byte != 0x7F));
            }
    CHECK(contains(screen, "[31mX"));
}

TEST_CASE("Preview shows bidi and zero-width characters as a warning marker cell, the line keeps its order") {
    const auto state = state_in(Focus::Preview);
    const auto screen = render_screen(state, text_preview({"abc\xE2\x80\xAE"
                                                           "def\xE2\x81\xA6\xE2\x80\x8B"
                                                           "g"}));
    const std::string marker = "\xEF\xBF\xBD";
    const auto at = find_text(screen, "abc" + marker + "def" + marker + marker + "g");
    REQUIRE(at.has_value());
    CHECK(screen.PixelAt(at->first + 3, at->second).background_color == ftxui::Color(ftxui::Color::Red));
    CHECK(screen.ToString().find("\xE2\x80\xAE") == std::string::npos);
}

// Kaynaklarını kovalamak yerine çizim noktası arınır: ileti, ipucu, senkron metni ve ad değiştirme hatası
// yol ya da sunucu metni taşıyabilir.
TEST_CASE("the status line, notice, sync text and rename error never draw a raw bidi or C1 character") {
    const std::string hostile = "gpj\xE2\x80\xAE"
                                "x\xC2\x9B"
                                "31m";
    const auto raw_on = [](const ftxui::Screen& screen) {
        const std::string all = screen.ToString();
        return all.find("\xE2\x80\xAE") != std::string::npos || all.find("\xC2\x9B") != std::string::npos;
    };
    const auto draw = [](const tui::StatusLine& status, const tui::RenameState* rename = nullptr) {
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(sample_state(), {}, status, {kWidth, kHeight}, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, nullptr, rename));
        return screen;
    };
    for (const tui::StatusLine& status :
         {tui::StatusLine{"sync " + hostile, ""}, tui::StatusLine{"", "moved " + hostile},
          tui::StatusLine{"", "", "notice " + hostile}}) {
        CAPTURE(status.sync + status.message + status.notice);
        const auto screen = draw(status);
        CHECK(screen.ToString().find("gpj") != std::string::npos);
        CHECK_FALSE(raw_on(screen));
    }
    tui::RenameState rename(core::NotePath::parse("a.md").value());
    rename.set_error("exists: " + hostile);
    const auto screen = draw({"", ""}, &rename);
    CHECK(screen.ToString().find("gpj") != std::string::npos);
    CHECK_FALSE(raw_on(screen));
}

TEST_CASE("PreviewCache drops a leading BOM like the editor, a BOM inside the text stays") {
    test::TempDir dir;
    test::upsert_note(dir.path(), "a.txt", "\xEF\xBB\xBFilk\nx\xEF\xBB\xBFy");
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;
    tui::Row row{core::NotePath::parse("a.txt").value(), {}, 0, {}};
    const auto& preview = cache.get(store, &row);
    REQUIRE(preview.kind == tui::Preview::Kind::Text);
    CHECK(preview.lines == std::vector<std::string>{"ilk", "x\xEF\xBB\xBFy"});
}

TEST_CASE("Preview find: highlights in the pane and 3/12-style counter in the bottom input") {
    const std::vector<std::string> lines = {"Işık ılık", "ışık"};
    auto state = state_in(Focus::Preview);
    state.set_preview_lines(&lines, 1);
    state.handle(Key::Char, "/");
    for (const std::string_view letter : {"ı", "ş", "ı", "k"})
        state.handle(Key::Char, letter);
    const auto screen = render_screen(state, text_preview(lines));
    CHECK(contains(screen, "Find: ışık▏  1/2"));
    const auto first = find_text(screen, "Işık");
    REQUIRE(first);
    CHECK(screen.PixelAt(first->first, first->second).inverted);  // geçerli eşleşme
    const auto second = find_text(screen, "ışık");
    REQUIRE(second);
    CHECK(screen.PixelAt(second->first, second->second).bold);
}

// FTXUI yan yana kutu çizgilerini birleştirir: modal kenarı alttaki panel çizgisiyle ┰┬┴ olmamalı.
TEST_CASE("an open modal keeps its own border: no box junctions with the panes under it") {
    core::init_i18n("en");
    for (const auto& [width, height] : {std::pair{120, 40}, std::pair{tui::kMinWidth, tui::kMinHeight}}) {
        CAPTURE(width);
        auto state = sample_state();
        state.handle(Key::Char, ":");
        REQUIRE(state.palette_open());
        const auto screen = render_screen(state, text_preview({"body"}), width, height);
        REQUIRE(contains(screen, "╰"));
        for (const char* junction : {"┬", "┴", "├", "┤", "┼", "┰", "┸", "┠", "┨", "╂", "┯", "┷", "┿"}) {
            CAPTURE(junction);
            CHECK_FALSE(contains(screen, junction));
        }
    }
}

TEST_CASE("the frames use the panes' own height: all rows of a small window are drawn") {
    const auto screen = render_screen(state_in(Focus::List), text_preview({"body"}), 60, 15);
    CHECK(list_corner(screen) == "focused");
    CHECK(screen.PixelAt(0, 12).character == "╰");  // altta toplamlar ve kısayol satırı
    CHECK(contains(screen, "t trash"));
}

TEST_CASE("moving the selection updates the preview in the same frame without Enter") {
    test::TempDir dir;
    test::upsert_note(dir.path(), "a.md", "body of a\n");
    test::upsert_note(dir.path(), "b.md", "body of b\n");
    test::upsert_note(dir.path(), "c.md", "body of c\n");
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache cache;
    // main.cpp'deki Renderer ile aynı çağrı: her kare seçili satırın önizlemesini okur.
    const auto frame = [&] { return render_screen(state, cache.get(store, state.selected_row())); };
    const auto shows_selected = [&](const ftxui::Screen& screen) {
        const std::string name = state.selected_row()->path.string().substr(0, 1);
        return contains(screen, "body of " + name);
    };
    CHECK(shows_selected(frame()));
    const std::string first = state.selected_row()->path.string();
    state.handle(Key::Down);
    REQUIRE(state.selected_row()->path.string() != first);
    auto screen = frame();
    CHECK(shows_selected(screen));
    CHECK_FALSE(contains(screen, "body of " + first.substr(0, 1)));
    state.handle(Key::Char, "j");
    CHECK(shows_selected(frame()));
    state.handle(Key::Char, "g");
    state.handle(Key::Char, "g");
    screen = frame();
    CHECK(shows_selected(screen));
    CHECK(contains(screen, "body of " + first.substr(0, 1)));
    CHECK(state.focus() == Focus::List);
}

TEST_CASE("key hints change with the focus") {
    const auto list = render_screen(state_in(Focus::List), text_preview({}), 120, 40);
    CHECK(contains(list, "/ filter"));
    CHECK(contains(list, "n add"));
    CHECK(contains(list, "d to trash"));
    CHECK(contains(list, "F1 help"));
    const auto search = render_screen(state_in(Focus::Search), text_preview({}), 120, 40);
    CHECK(contains(search, "Esc close"));
    CHECK_FALSE(contains(search, "a add"));
    const auto preview = render_screen(state_in(Focus::Preview), text_preview({}), 120, 40);
    CHECK(contains(preview, "← list"));
    CHECK(contains(preview, "↑↓ scroll"));
    // Liste komutları önizlemede çalışmaz: çubuk da onları önermez.
    CHECK_FALSE(contains(preview, "a add"));
    CHECK_FALSE(contains(preview, "^R"));
    CHECK_FALSE(contains(preview, "^O"));
    CHECK_FALSE(contains(preview, "^S"));
}

TEST_CASE("fit_hints drops whole hints from the end and never cuts a word") {
    CHECK(tui::fit_hints("aa b  cc d  ee f", 16) == "aa b  cc d  ee f");
    CHECK(tui::fit_hints("aa b  cc d  ee f", 15) == "aa b  cc d");
    CHECK(tui::fit_hints("aa b  cc d  ee f", 10) == "aa b  cc d");
    CHECK(tui::fit_hints("aa b  cc d  ee f", 9) == "aa b");
    CHECK(tui::fit_hints("aa b  cc d  ee f", 3).empty());
    CHECK(tui::fit_hints("", 10).empty());
    CHECK(tui::fit_hints("↑↓ git  çık", 11) == "↑↓ git  çık");
}

TEST_CASE("the shortcut bar pins F1 help right and fills whole hints left, at every width from 60") {
    for (const auto& [language, help] : kHelpHints) {
        core::init_i18n(language);
        for (const auto& [focus, key] : kHintKeys) {
            for (const int width : {60, 61, 80, 120, 160}) {
                CAPTURE(language);
                CAPTURE(key);
                CAPTURE(width);
                const auto screen = render_screen(state_in(focus), text_preview({}), width, 15);
                const std::string bar = padded_row(screen, 14);
                CAPTURE(bar);
                REQUIRE(bar.ends_with(help));
                CHECK(ftxui::string_width(bar) == width);  // sağ kenara yaslı
                std::string left = bar.substr(0, bar.size() - help.size());
                CHECK(left.ends_with("  "));  // gruplar arası en az iki boşluk
                while (left.ends_with(' '))
                    left.pop_back();
                const std::string rest = without_hint(core::tr(key), help);
                // Sıra önem sırasıdır; sığmayan ipucu ve ardındakiler bütün olarak düşer.
                CHECK(left == tui::fit_hints(rest, width - ftxui::string_width(help) - 2));
                CHECK(left.size() >= 20);  // en önemli ipuçları kalır
            }
        }
    }
    core::init_i18n("en");
}

TEST_CASE("the list hint bar starts with the keys users could not find, F1 help at the right from 60") {
    const std::pair<const char*, std::array<const char*, 7>> expected[] = {
        {"en", {": cmd", "t trash", "Space open", "o sort", "V col", "s search", "F1 help"}},
        {"tr", {": komut", "t çöp", "Space aç", "o sırala", "V sütun", "s ara", "F1 yardım"}},
    };
    for (const auto& [language, hints] : expected) {
        core::init_i18n(language);
        for (const int width : {60, 61, 79, 80, 100, 110, 120, 160}) {
            CAPTURE(language);
            CAPTURE(width);
            const std::string bar =
                row_text(render_screen(state_in(Focus::List), text_preview({}), width, 15), 14);
            CHECK(bar.ends_with(hints.back()));
            // 60 sütunda Türkçe altıncı ipucu (`s ara`) bir hücreyle sığmaz: sağdaki F1 yer tutar.
            const std::size_t whole = width == 60 && std::string_view(language) == "tr" ? 5 : 6;
            std::size_t after = 0;
            for (std::size_t i = 0; i < whole; ++i) {
                CAPTURE(hints[i]);
                const std::size_t at = bar.find(hints[i], after);
                REQUIRE(at != std::string::npos);
                after = at + 1;
            }
        }
    }
    core::init_i18n("en");
}

TEST_CASE("a long filter does not push the priority hints or F1 off the list bar at 60 columns") {
    core::init_i18n("tr");
    auto state = state_in(Focus::Search);
    for (const char c : std::string("uzun-bir-suzgec-metni-buraya-yazildi-ve-bitmedi"))
        state.handle(Key::Char, std::string_view(&c, 1));
    state.handle(Key::Escape);
    REQUIRE(state.focus() == Focus::List);
    const std::string bar = row_text(render_screen(state, text_preview({}), 60, 15), 14);
    CHECK(bar.starts_with("> "));
    CHECK(bar.find("itmedi") != std::string::npos);  // yazdığının sonu görünür
    // Sağda F1 yer tutar: süzgeç yanında ilk dört ipucu kalır.
    for (const char* hint : {": komut", "t çöp", "Space aç", "o sırala"}) {
        CAPTURE(hint);
        CHECK(bar.find(hint) != std::string::npos);
    }
    CHECK(bar.ends_with("F1 yardım"));
    core::init_i18n("en");
}

// ---- U2 T3: kip hapı, üst satır, ipucu çubuğu ----

TEST_CASE("the status line opens with a mode pill: bold black on the mode color, LIST then REVIEW") {
    core::init_i18n("en");
    const std::tuple<Focus, const char*, ftxui::Color> cases[] = {
        {Focus::List, "tui.frame_list", ftxui::Color(ftxui::Color::Cyan)},
        {Focus::Preview, "tui.frame_review", ftxui::Color(ftxui::Color::Green)}};
    for (const auto& [focus, label, color] : cases) {
        const auto screen = render_screen(state_in(focus), text_preview({}), 120, 40);
        const std::string pill = " " + core::tr(label) + " ";
        CAPTURE(pill);
        CHECK(row_text(screen, 38).starts_with(pill + " "));
        const int cells = ftxui::string_width(pill);
        for (int x = 0; x < cells; ++x) {
            const auto& cell = screen.PixelAt(x, 38);
            CHECK(cell.background_color == color);
            CHECK(cell.foreground_color == ftxui::Color(ftxui::Color::Black));
            CHECK(cell.bold);
            CHECK_FALSE(cell.inverted);
        }
        CHECK(screen.PixelAt(cells, 38).background_color == ftxui::Color());
    }
}

TEST_CASE("without color (NO_COLOR) the mode pill is inverted and bold") {
    core::init_i18n("en");
    const ftxui::Terminal::Color before = ftxui::Terminal::ColorSupport();
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::Palette1);
    const auto screen = render_screen(state_in(Focus::List), text_preview({}), 120, 40);
    ftxui::Terminal::SetColorSupport(before);
    const std::string pill = " " + core::tr("tui.frame_list") + " ";
    CHECK(row_text(screen, 38).starts_with(pill));
    for (int x = 0; x < ftxui::string_width(pill); ++x) {
        CHECK(screen.PixelAt(x, 38).inverted);
        CHECK(screen.PixelAt(x, 38).bold);
    }
    CHECK_FALSE(screen.PixelAt(ftxui::string_width(pill), 38).inverted);
}

TEST_CASE("hint keys are bold in the mode color, their words in the default color, nothing dim") {
    core::init_i18n("en");
    const std::tuple<Focus, const char*, ftxui::Color> cases[] = {
        {Focus::List, ": cmd", ftxui::Color(ftxui::Color::Cyan)},
        {Focus::Preview, "← list", ftxui::Color(ftxui::Color::Green)}};
    for (const auto& [focus, hint, color] : cases) {
        CAPTURE(hint);
        const auto screen = render_screen(state_in(focus), text_preview({}), 120, 40);
        const auto at = find_text(screen, hint);
        REQUIRE(at);
        CHECK(at->second == 39);
        const auto& key = screen.PixelAt(at->first, 39);
        CHECK(key.bold);
        CHECK_FALSE(key.dim);
        CHECK(key.foreground_color == color);
        const auto& word = screen.PixelAt(at->first + 2, 39);
        CHECK_FALSE(word.bold);
        CHECK_FALSE(word.dim);
        CHECK(word.foreground_color == ftxui::Color());
        const auto help = find_text(screen, "F1 help");
        REQUIRE(help);
        CHECK(screen.PixelAt(help->first, 39).bold);
        CHECK(screen.PixelAt(help->first + 1, 39).foreground_color == color);
        CHECK_FALSE(screen.PixelAt(help->first + 3, 39).bold);
    }
}

TEST_CASE("top line: the right group is dim and split by middle dots, the sync state has its color") {
    core::init_i18n("en");
    using State = tui::SyncIndicator::State;
    const std::tuple<State, std::string, ftxui::Color, bool> cases[] = {
        {State::Disabled, "sync off", ftxui::Color(), true},
        {State::Running, "⟳", ftxui::Color(ftxui::Color::Blue), false},
        {State::Ok, "✓ 23:45", ftxui::Color(ftxui::Color::Green), false},
        {State::Failed, "⚠ disk full", ftxui::Color(ftxui::Color::Red), false}};
    for (const auto& [sync_state, sync, color, dim] : cases) {
        CAPTURE(sync);
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        const tui::StatusLine status{.sync = sync, .message = {}, .notice = {}, .sync_state = sync_state};
        ftxui::Render(screen, tui::render(sample_state(), text_preview({}), status, {kWidth, kHeight}));
        const std::string top = row_text(screen, 0);
        CHECK(top.ends_with("↓ · [name] · 3 notes · " + sync + " · " + app::version_label()));
        const auto shown = find_text(screen, sync);
        REQUIRE(shown);
        CHECK(shown->second == 0);
        CHECK(screen.PixelAt(shown->first, 0).foreground_color == color);
        CHECK(screen.PixelAt(shown->first, 0).dim == dim);
        CHECK(screen.PixelAt(shown->first - 2, 0).character == "·");
        CHECK(screen.PixelAt(shown->first - 2, 0).dim);
        const auto mode = find_text(screen, "[name]");
        REQUIRE(mode);
        CHECK(screen.PixelAt(mode->first, 0).dim);
    }
}

// En dar pencerede uzun senkron hatası sağ grubu kenardan itmez: önce senkron metni "…" ile kısalır; sıra,
// kip ve sayaç tam kalır. Geniş (CJK) karakter iki hücredir, kesim hücreye göre yapılır.
TEST_CASE("top line: a long sync error is cut with an ellipsis at 60 columns, sort, mode and counts stay") {
    constexpr int kNotes = 1000;
    constexpr int kHits = 100;
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(note("n" + std::to_string(i) + ".md", i));
    const std::string errors[] = {"⚠ 同步失败：无法连接到远程服务器，请检查网络设置后再试一次",
                                  "⚠ remote rejected the push: the repository is locked by another client"};
    for (const auto& [language, fixed] : {std::pair{"tr", "oluşturma↑ · [içerik] · 100/1000"},
                                          std::pair{"en", "created↑ · [content] · 100/1000"}}) {
        core::init_i18n(language);
        tui::TuiState state;
        state.set_notes(notes);
        tui::ListPrefs prefs;
        prefs.sort = {search::SortField::Created, false};
        state.set_list_prefs(prefs);
        state.set_content_searcher([&notes](std::string_view) {
            std::vector<search::ContentResult> results;
            for (int i = 0; i < kHits; ++i)
                results.push_back({notes[static_cast<std::size_t>(i)].path, 1, {}});
            return results;
        });
        state.handle(Key::Tab);
        state.handle(Key::Char, "/");
        state.handle(Key::Char, "x");
        CHECK(state.rows().size() == kHits);
        for (const std::string& error : errors) {
            CAPTURE(language);
            CAPTURE(error);
            auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(tui::kMinWidth),
                                                ftxui::Dimension::Fixed(tui::kMinHeight));
            const tui::StatusLine status{
                .sync = error, .message = {}, .notice = {}, .sync_state = tui::SyncIndicator::State::Failed};
            ftxui::Render(screen,
                          tui::render(state, text_preview({}), status, {tui::kMinWidth, tui::kMinHeight}));
            // REQUIRE değil: test yarıda kalırsa dil Türkçe kalır, sonraki testler İngilizce katalog bekler.
            const std::string top = row_text(screen, 0);
            CAPTURE(top);
            const auto group = find_text(screen, fixed);
            const auto sync = find_text(screen, error.substr(0, error.find(' ') + 1));
            CHECK(group);
            CHECK(sync);
            if (!group || !sync)
                continue;
            CHECK(group->second == 0);
            CHECK(sync->second == 0);
            CHECK(sync->first > group->first);
            CHECK(row_text(screen, 0).ends_with("…"));
            CHECK(screen.PixelAt(sync->first, 0).foreground_color == ftxui::Color(ftxui::Color::Red));
        }
    }
    core::init_i18n("en");
}

TEST_CASE("the top line shows a non-empty trash as text, at the far left") {
    auto state = sample_state();
    CHECK_FALSE(contains(render_screen(state, text_preview({})), "trash:"));
    state.set_trash_count(3);
    const auto screen = render_screen(state, text_preview({}), 60, 15);
    CHECK(row_text(screen, 0).starts_with("trash: 3"));
    core::init_i18n("tr");
    CHECK(row_text(render_screen(state, text_preview({}), 60, 15), 0).starts_with("çöp: 3"));
    core::init_i18n("en");
}

TEST_CASE("an open note row is an indented block, the row itself has no marker") {
    auto state = sample_state();
    const auto closed = render_screen(state, text_preview({}));
    CHECK(contains(closed, " 1   work-meeting.md"));
    CHECK(contains(closed, " 2   personal-todo.md"));
    state.handle(Key::AltRight);
    const auto open = render_screen(state, text_preview({}), kWidth, 24);
    CHECK(contains(open, " 1   work-meeting.md"));
    CHECK(contains(open, "    size: "));
    CHECK_FALSE(contains(open, "▾"));
    CHECK(contains(open, " 2   personal-todo.md"));
}

TEST_CASE("the search input keeps whole hints beside it at 60 columns in both languages") {
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        const auto screen = render_screen(state_in(Focus::Search), text_preview({}), 60, 15);
        const std::string bar = row_text(screen, 14);
        CAPTURE(language);
        CHECK(bar.starts_with("> ▏"));
        const std::string full = core::tr("tui.key_hints_search");
        const std::size_t first = bar.find(full.substr(0, 5));
        REQUIRE(first != std::string::npos);
        const std::string hints = bar.substr(first);
        CHECK(full.starts_with(hints));
        CHECK(full.substr(hints.size(), 2) == "  ");
    }
    core::init_i18n("en");
}

TEST_CASE("help has sections for trash, row details, sorting and filter arguments in both languages") {
    const std::pair<const char*, std::array<const char*, 4>> expected[] = {
        {"en", {"Trash (t", "Row details", "Sort and column", "Filter arguments"}},
        {"tr", {"Çöp kutusu (t", "Satır ayrıntısı", "Sıralama ve sütun", "Süzme argümanları"}},
    };
    for (const auto& [language, sections] : expected) {
        core::init_i18n(language);
        std::string all;
        for (const auto& line : tui::help_lines())
            all += "\n" + line;
        for (const char* section : sections) {
            CAPTURE(section);
            CHECK(all.find(std::string("\n") + section) != std::string::npos);
        }
        for (const char* needle : {"\nE ", "sort:-size", "type:md", "in:work/", "updated:7d", "▸", "▾"}) {
            CAPTURE(needle);
            CHECK(all.find(needle) != std::string::npos);
        }
    }
    core::init_i18n("en");
}

TEST_CASE("every help line fits the help window in both languages") {
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        for (const auto& line : tui::help_lines()) {
            CAPTURE(line);
            CHECK(ftxui::string_width(line) <= 50);  // pencere 52 - kenarlık 2
        }
    }
    core::init_i18n("en");
}

TEST_CASE("help documents the single-key shortcuts in both languages") {
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        std::string all;
        for (const auto& line : tui::help_lines())
            all += line + "\n";
        for (const std::string_view key :
             {"\n/ ", "\nd ", "\na ", "\nn ", "\nr ", "\ns ", "\n? ", "\nq ", "g G", "k j", "→ l"}) {
            CAPTURE(key);
            CHECK(all.find(key) != std::string::npos);
        }
    }
    core::init_i18n("en");
}

namespace {

ftxui::Screen render_new_row(const tui::TuiState& state, const tui::NewRowState& row, int width, int height) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, text_preview({}), {"sync off", "old message"}, {width, height},
                                      nullptr, nullptr, nullptr, nullptr, &row));
    return screen;
}

tui::NewRowState typed_row(std::string_view text) {
    tui::NewRowState row;
    for (const char c : text)
        row.handle(Key::Char, std::string_view(&c, 1));
    return row;
}

// Satırda `part`ın başladığı hücre.
int cell_of(const std::string& row, std::string_view part) {
    const std::size_t at = row.find(part);
    REQUIRE(at != std::string::npos);
    return ftxui::string_width(row.substr(0, at));
}

}  // namespace

TEST_CASE("a: the name row is the first row of the list pane, not the bottom line") {
    const auto state = sample_state();
    const auto screen = render_new_row(state, typed_row("deneme.cpp"), 100, 24);
    const std::string first = row_text(screen, 2);
    CHECK(first.find("deneme.cpp▏") != std::string::npos);
    CHECK(first.starts_with("│ "));
    CHECK(first.find('>') == std::string::npos);
    CHECK(row_text(screen, 3).find("work-meeting.md") != std::string::npos);
    const std::string bottom = row_text(screen, 23);
    CHECK(bottom.find("deneme") == std::string::npos);
    CHECK(bottom.find("Esc cancel") != std::string::npos);
    CHECK_FALSE(contains(screen, "old message"));
}

TEST_CASE("a: the number column stays aligned and the path starts in the same cell") {
    tui::TuiState state;
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < 12; ++i)
        notes.push_back(note("n" + std::to_string(i) + ".md", i));
    state.set_notes(notes);
    const auto screen = render_new_row(state, typed_row("yeni"), 100, 24);
    CHECK(cell_of(row_text(screen, 2), "yeni") == cell_of(row_text(screen, 3), "n0.md"));
    CHECK(cell_of(row_text(screen, 3), "n0.md") == cell_of(row_text(screen, 12), "n9.md"));
    CHECK(cell_of(row_text(screen, 12), "n9.md") == cell_of(row_text(screen, 13), "n10.md"));
}

TEST_CASE("a: the list scrolls so the name row and the selected note are both visible") {
    tui::TuiState state;
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < 100; ++i)
        notes.push_back(note("n" + std::to_string(i) + ".md", i));
    state.set_notes(notes);
    state.handle(Key::Char, "G");
    const auto screen = render_new_row(state, typed_row("yeni"), 60, 15);
    CHECK(row_text(screen, 2).find("yeni▏") != std::string::npos);
    CHECK(contains(screen, "n99.md"));
}

TEST_CASE("a: the name row also opens on an empty list") {
    tui::TuiState state;
    const auto screen = render_new_row(state, typed_row("ilk"), 60, 15);
    CHECK(row_text(screen, 2).find("ilk▏") != std::string::npos);
}

TEST_CASE("a: a long name keeps its end visible inside the pane at the minimum width") {
    const auto state = sample_state();
    tui::NewRowState row;
    for (int i = 0; i < 100; ++i)
        row.handle(Key::Char, "ş");
    row.handle(Key::Char, "Z");
    const auto screen = render_new_row(state, row, 60, 15);
    CHECK(row_text(screen, 2).find("Z▏") != std::string::npos);
    CHECK(row_text(screen, 2).find("…") != std::string::npos);
}

TEST_CASE("a: an error shows inline under the row in the list pane and keeps the text") {
    const auto state = sample_state();
    tui::NewRowState row = typed_row("x");
    row.set_error("disk full");
    const auto screen = render_new_row(state, row, 100, 24);
    CHECK(row_text(screen, 2).find("x▏") != std::string::npos);
    CHECK(row_text(screen, 3).find("disk") != std::string::npos);
    CHECK(row_text(screen, 3).find("full") != std::string::npos);
    CHECK(row_text(screen, 4).find("work-meeting.md") != std::string::npos);
    CHECK(row_text(screen, 23).find("disk") == std::string::npos);
}

TEST_CASE("mode frame: the list has a LIST label and its own border color") {
    core::init_i18n("en");
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({"içerik"}));
    const auto label = find_text(screen, core::tr("tui.frame_list"));
    REQUIRE(label);
    CHECK(screen.PixelAt(label->first, label->second).foreground_color == ftxui::Color(ftxui::Color::Cyan));
    // Çerçeve: üst sol köşe etiketle aynı renkte, etiket kalın; sağ panel odaksız (soluk), etiketsiz.
    CHECK(screen.PixelAt(0, 1).character == "╭");
    CHECK(screen.PixelAt(label->first, label->second).bold);
    CHECK(preview_corner(screen, kWidth) == "blurred");
    CHECK(screen.PixelAt(0, 1).foreground_color == ftxui::Color(ftxui::Color::Cyan));
    CHECK_FALSE(find_text(screen, core::tr("tui.frame_review")));
    CHECK_FALSE(find_text(screen, core::tr("tui.frame_edit")));
}

TEST_CASE("mode frame: the preview has a REVIEW label and a color different from the list") {
    core::init_i18n("en");
    auto state = sample_state();
    state.handle(Key::Right);
    const auto screen = render_screen(state, text_preview({"içerik"}));
    const auto label = find_text(screen, core::tr("tui.frame_review"));
    REQUIRE(label);
    const ftxui::Color review = screen.PixelAt(label->first, label->second).foreground_color;
    CHECK(review == ftxui::Color(ftxui::Color::Green));
    CHECK(screen.PixelAt(label->first - 1, label->second).foreground_color == review);
    CHECK_FALSE(find_text(screen, core::tr("tui.frame_list")));
}

TEST_CASE("mode frame: labels come from the catalog in both languages") {
    core::init_i18n("tr");
    auto state = sample_state();
    CHECK(find_text(render_screen(state, text_preview({"x"})), core::tr("tui.frame_list")));
    CHECK(core::tr("tui.frame_review") != "tui.frame_review");
    CHECK(core::tr("tui.frame_edit") != "tui.frame_edit");
    core::init_i18n("en");
}

TEST_CASE("review syntax: the preview colors by language and search wins over the token color") {
    core::init_i18n("en");
    auto state = sample_state();
    state.handle(Key::Right);
    auto preview = text_preview({"int a; /* yorum", "hala yorum */ int b;"});
    preview.language = tui::SyntaxLanguage::Java;
    auto screen = render_screen(state, preview);
    const auto keyword = find_text(screen, "int");
    REQUIRE(keyword);
    CHECK(screen.PixelAt(keyword->first, keyword->second).foreground_color ==
          ftxui::Color(ftxui::Color::Cyan));
    const auto comment = find_text(screen, "hala");
    REQUIRE(comment);
    CHECK(screen.PixelAt(comment->first, comment->second).dim);  // yorum soluk, renksiz

    state.handle(Key::Char, "/");
    state.set_preview_lines(&preview.lines, 1);
    for (const char* letter : {"h", "a", "l", "a"})
        state.handle(Key::Char, letter);
    state.handle(Key::Enter);
    screen = render_screen(state, preview);
    const auto hit = find_text(screen, "hala");
    REQUIRE(hit);
    // Geçerli eşleşme ters çizilir: yorumun solukluğu aramanın önüne geçmez.
    CHECK(screen.PixelAt(hit->first, hit->second).inverted);
    CHECK_FALSE(screen.PixelAt(hit->first, hit->second).dim);
}

TEST_CASE("review syntax: PreviewCache picks the language from the extension") {
    core::init_i18n("en");
    const test::TempDir dir;
    test::upsert_note(dir.path(), "a.java", "int a;\n");
    test::upsert_note(dir.path(), "b.xyz", "int a;\n");
    test::upsert_note(dir.path(), "big.java", std::string(tui::kDefaultHighlightBytes + 1, 'x'));
    const core::NoteStore store(dir.path());
    const auto language_of = [&](const std::string& name) {
        tui::PreviewCache cache;
        const tui::Row row{core::NotePath::parse(name).value(), {}, 0, {}};
        return cache.get(store, &row).language;
    };
    CHECK(language_of("a.java") == tui::SyntaxLanguage::Java);
    CHECK(language_of("b.xyz") == tui::SyntaxLanguage::Plain);
    CHECK(language_of("big.java") == tui::SyntaxLanguage::Plain);

    // limits.tui.highlight_bytes 0: eski 2 MiB eşiğinin üstü de renklenir.
    tui::PreviewCache unlimited({}, tui::SyntaxLimits{.note_bytes = SIZE_MAX});
    const tui::Row big{core::NotePath::parse("big.java").value(), {}, 0, {}};
    CHECK(unlimited.get(store, &big).language == tui::SyntaxLanguage::Java);
}

constexpr int kFirstRowY = 2;  // üst çubuk, panel kenarı

TEST_CASE("list rows show no icon when icons are off") {
    auto state = sample_state();
    state.set_icon_style(core::IconStyle::Off);
    CHECK(contains(render_screen(state, text_preview({})), " 1   work-meeting.md"));
}

TEST_CASE("list rows show the icon in one cell before the name, keeping the arrow column") {
    auto state = sample_state();
    const int marker = tui::row_marker_column(state);
    state.set_icon_style(core::IconStyle::Nerd);
    CHECK(tui::row_marker_column(state) == marker);
    const auto screen = render_screen(state, text_preview({}));
    const std::string md = core::icon_text(core::file_icon("a.md"), core::IconStyle::Nerd);
    const std::string java = core::icon_text(core::file_icon("a.java"), core::IconStyle::Nerd);
    CHECK(contains(screen, " 1   " + md + "  work-meeting.md"));
    CHECK(contains(screen, " 3   " + java + "  snippets-"));  // dar panelde boyut sütunu adı kısaltır
    CHECK_FALSE(contains(screen, "snippets-sort.java"));
    CHECK(screen.PixelAt(marker, kFirstRowY).character == " ");  // notta ok yok
    CHECK(screen.PixelAt(marker + 2, kFirstRowY).character == md);
    CHECK(screen.PixelAt(marker + 5, kFirstRowY).character == "w");
}

TEST_CASE("Nerd Font icons take one cell") {
    for (const char* name : {"a.md", "a.json", "a.env", "a.unknown"}) {
        CAPTURE(name);
        CHECK(ftxui::string_width(core::icon_text(core::file_icon(name), core::IconStyle::Nerd)) == 1);
    }
    auto state = sample_state();
    state.set_icon_style(core::IconStyle::Nerd);
    const int marker = tui::row_marker_column(state);
    const auto screen = render_screen(state, text_preview({}));
    CHECK(screen.PixelAt(marker + 2, kFirstRowY).character ==
          core::icon_text(core::file_icon("a.md"), core::IconStyle::Nerd));
    CHECK(screen.PixelAt(marker + 3, kFirstRowY).character == " ");  // glifin taştığı hücre
    CHECK(screen.PixelAt(marker + 4, kFirstRowY).character == " ");
    CHECK(screen.PixelAt(marker + 5, kFirstRowY).character == "w");
}

TEST_CASE("every file type keeps one terminal cell") {
    for (const char* name :
         {"a.md",         "a.txt",    "a.csv",  "a.json", "a.yaml", "a.toml", "a.ini",    "a.env",
          "a.properties", "a.xml",    "a.html", "a.css",  "a.js",   "a.ts",   "a.c",      "a.cpp",
          "a.h",          "a.hpp",    "a.java", "a.py",   "a.sh",   "a.go",   "a.rs",     "a.sql",
          "Dockerfile",   "Makefile", "a.pdf",  "a.png",  "a.svg",  "a.zip",  "a.unknown"}) {
        CAPTURE(name);
        const core::FileIcon icon = core::file_icon(name);
        CHECK(ftxui::string_width(core::icon_text(icon, core::IconStyle::Nerd)) == 1);
    }
}

TEST_CASE("the right column gives way two cells earlier when an icon takes room") {
    tui::TuiState plain;
    const std::string name(32, 'a');
    plain.set_notes({note(name + ".md")});
    const auto column_shown = [](const tui::TuiState& s, int width) {
        return contains(render_screen(s, text_preview({}), width), "1 B");
    };
    int without = 0;
    for (int width = 20; width < 200 && !without; ++width)
        if (column_shown(plain, width))
            without = width;
    REQUIRE(without > 0);
    plain.set_icon_style(core::IconStyle::Nerd);
    CHECK_FALSE(column_shown(plain, without));
}

TEST_CASE("the trash indicator and title carry the trash icon") {
    core::init_i18n("en");
    auto state = sample_state();
    state.set_trash_count(3);
    state.set_icon_style(core::IconStyle::Nerd);
    const std::string icon = core::icon_text(core::trash_icon(), core::IconStyle::Nerd);
    CHECK(row_text(render_screen(state, text_preview({}), 60, 15), 0).starts_with(icon + "  trash: 3"));
    state.set_icon_style(core::IconStyle::Off);
    CHECK(row_text(render_screen(state, text_preview({}), 60, 15), 0).starts_with("trash: 3"));
}

// NY-G: işlem bildirimi (GUI ile ortak app::NoticeQueue) iki alt satırın üstünde sağa yaslı, en yenisi; alt
// çubuk aynı metni yinelemez, ipuçları kalır. Hata ve başarı önekle ayrılır (renk tek sinyal değil).
TEST_CASE("render: an operation notice is a toast above the bottom bar, shown once") {
    tui::StatusLine status{"sync off", ""};
    status.notify(app::NoticeKind::Success, "PDF yazıldı: a.pdf");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(sample_state(), text_preview({}), status, {kWidth, kHeight}));
    const auto at = find_text(screen, core::tr("notice.success") + "  PDF yazıldı: a.pdf");
    REQUIRE(at.has_value());
    CHECK(at->second == kHeight - 3);
    for (int y = kHeight - 2; y < kHeight; ++y) {  // alt satırlarda yinelenmez
        std::string row;
        for (int x = 0; x < kWidth; ++x)
            row += screen.PixelAt(x, y).character;
        CHECK(row.find("PDF yazıldı") == std::string::npos);
    }

    status.notify(app::NoticeKind::Error, "senkron başarısız");
    auto again = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(again, tui::render(sample_state(), text_preview({}), status, {kWidth, kHeight}));
    CHECK(contains(again, core::tr("notice.error") + "  senkron başarısız"));
    CHECK(contains(again, core::tr("notice.more", {{"count", "1"}})));
}
