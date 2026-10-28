#include "core/i18n.hpp"
#include "support/test_support.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace noteapp;
using tui::Key;

namespace {

constexpr int kWidth = 80;
constexpr int kHeight = 15;

core::NoteInfo note(const std::string& path, int age_hours = 0, bool binary = false) {
    return core::NoteInfo{core::NotePath::parse(path).value(),
                          std::filesystem::file_time_type{} + std::chrono::hours(100000 - age_hours), 1,
                          binary};
}

tui::TuiState sample_state() {
    tui::TuiState s;
    s.set_notes({note("work/meeting.md", 0), note("personal/todo.md", 1), note("snippets/sort.java", 2)});
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
    CHECK(contains(screen, "> "));
    CHECK(contains(screen, "work/meeting.md"));
    CHECK(contains(screen, "personal/todo.md"));
    CHECK(contains(screen, "snippets/sort.java"));
    CHECK(contains(screen, "[name] 3/3"));
    CHECK(contains(screen, "sync off"));
    CHECK(contains(screen, "# Toplantı notları"));
    CHECK(contains(screen, "Enter open"));
}

TEST_CASE("filtered list highlights exactly the matched characters") {
    auto state = sample_state();
    for (const char c : std::string("tod"))
        state.handle(Key::Char, std::string(1, c));
    const auto screen = render_screen(state, text_preview({}));
    CHECK(contains(screen, "1/3"));
    CHECK_FALSE(contains(screen, "work/meeting.md"));
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
    const auto selected = find_text(screen, "personal/todo.md");
    const auto other = find_text(screen, "work/meeting.md");
    REQUIRE(selected);
    REQUIRE(other);
    CHECK(screen.PixelAt(selected->first, selected->second).inverted);
    CHECK_FALSE(screen.PixelAt(other->first, other->second).inverted);
    CHECK(contains(screen, "▸ personal/todo.md"));
}

TEST_CASE("a window below 60x15 shows only the centered too-small message") {
    const auto state = sample_state();
    const auto screen = render_screen(state, text_preview({}), 50, 10);
    CHECK(contains(screen, "Window too small"));
    CHECK_FALSE(contains(screen, "work/meeting.md"));
    const auto narrow = render_screen(state, text_preview({}), 59, 20);
    CHECK(contains(narrow, "Window too small"));
}

TEST_CASE("Turkish names render and match through the folded search") {
    tui::TuiState state;
    state.set_notes({note("çalışma/öğrenci-notları.md", 0), note("kişisel/İstanbul.md", 1)});
    const std::vector<std::string> lines = {"çğıİöşü ÇĞIÖŞÜ"};
    auto screen = render_screen(state, text_preview(lines));
    CHECK(contains(screen, "çalışma/öğrenci-notları.md"));
    CHECK(contains(screen, "çğıİöşü ÇĞIÖŞÜ"));

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
    CHECK(contains(screen, "[ad] 3/3"));
    CHECK(contains(screen, "Enter aç"));
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
    state.handle(Key::PageDown);
    const auto screen = render_screen(state, text_preview(lines));
    CHECK(contains(screen, "▸ n125.md"));
    CHECK_FALSE(contains(screen, "n100.md"));
    CHECK(contains(screen, "line1010"));
    CHECK_FALSE(contains(screen, "line1009"));
}

TEST_CASE("PreviewCache reads text, flags binary notes and reports unreadable ones") {
    test::TempDir dir;
    {
        std::ofstream(dir.path() / "a.md", std::ios::binary) << "first\r\nsecond\tline\nlast";
        std::ofstream(dir.path() / "b.bin", std::ios::binary) << std::string("x\0y", 3);
    }
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;

    tui::Row text_row{core::NotePath::parse("a.md").value(), {}, 0, {}, false};
    const auto& text = cache.get(store, &text_row);
    REQUIRE(text.kind == tui::Preview::Kind::Text);
    CHECK(text.lines == std::vector<std::string>{"first", "second\tline", "last"});

    tui::Row binary_row{core::NotePath::parse("b.bin").value(), {}, 0, {}, true};
    CHECK(cache.get(store, &binary_row).kind == tui::Preview::Kind::Binary);
    const auto screen = render_screen(sample_state(), cache.get(store, &binary_row));
    CHECK(contains(screen, "binary file"));

    tui::Row missing_row{core::NotePath::parse("gone.md").value(), {}, 0, {}, false};
    CHECK(cache.get(store, &missing_row).kind == tui::Preview::Kind::Error);
    CHECK(cache.get(store, nullptr).kind == tui::Preview::Kind::Empty);
}

TEST_CASE("PreviewCache serves the cached note until invalidated") {
    test::TempDir dir;
    std::ofstream(dir.path() / "a.md", std::ios::binary) << "old";
    const core::NoteStore store(dir.path());
    tui::PreviewCache cache;
    tui::Row row{core::NotePath::parse("a.md").value(), {}, 0, {}, false};
    CHECK(cache.get(store, &row).lines.front() == "old");
    std::ofstream(dir.path() / "a.md", std::ios::binary | std::ios::trunc) << "new";
    CHECK(cache.get(store, &row).lines.front() == "old");
    cache.invalidate();
    CHECK(cache.get(store, &row).lines.front() == "new");
}

#ifdef NDEBUG
// SPEC-tui kabul 5: 5.000 notla tuş başına süzme ≤ 50 ms. Yalnız Release'te anlamlı.
TEST_CASE("5000 notes: filtering per keystroke stays within the budget") {
    constexpr int kNotes = 5000;
    constexpr double kBudgetMs = 50.0;
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(note("folder" + std::to_string(i % 40) + "/note-" + std::to_string(i) + ".md", i));

    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    tui::TuiState state;
    const auto t0 = clock::now();
    state.set_notes(std::move(notes));
    const auto t1 = clock::now();
    const auto screen = render_screen(state, text_preview({}), 120, 40);
    const auto t2 = clock::now();

    double worst = 0;
    for (const char c : std::string("note-42")) {
        const auto a = clock::now();
        state.handle(Key::Char, std::string(1, c));
        const auto b = clock::now();
        worst = std::max(worst, ms(a, b));
    }
    std::cout << "[perf] set_notes " << ms(t0, t1) << " ms, first render " << ms(t1, t2)
              << " ms, worst keystroke " << worst << " ms\n";
    CHECK(worst <= kBudgetMs);
    CHECK(contains(screen, "5000/5000"));
}

// Açılış = NoteStore::list() (diskte stat) + set_notes + ilk render; yalnız raporlanır (list() ayrıca
// iyileştirilmektedir), eşik uygulanmaz.
TEST_CASE("5000 notes on disk: report startup time") {
    constexpr int kNotes = 5000;
    test::TempDir dir;
    for (int i = 0; i < kNotes; ++i) {
        const auto folder = dir.path() / ("folder" + std::to_string(i % 40));
        std::filesystem::create_directories(folder);
        std::ofstream(folder / ("note-" + std::to_string(i) + ".md"), std::ios::binary) << "# note " << i;
    }
    const core::NoteStore store(dir.path());
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache cache;
    const auto screen = render_screen(state, cache.get(store, state.selected_row()), 120, 40);
    const auto elapsed = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    std::cout << "[perf] startup (list + set_notes + first render, 5000 notes on disk) " << elapsed
              << " ms\n";
    CHECK(contains(screen, "5000/5000"));
}
#endif
