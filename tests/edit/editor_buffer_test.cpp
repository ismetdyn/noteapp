#include "core/text.hpp"
#include "edit/editor_buffer.hpp"

#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using noteapp::tui::EditorBuffer;
using noteapp::tui::TextPosition;

namespace {

// Tampon genişliği dışarıdan alır: burada FTXUI'siz, sınıfları bilinen sahte bir ölçü yeter.
int test_width(char32_t cp) {
    if (cp >= 0x0300 && cp <= 0x036F)
        return 0;
    if (cp == 0x200D)  // ZWJ
        return 0;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF))
        return 2;  // CJK, bölgesel gösterge, emoji
    return 1;
}

constexpr std::string_view kCombiningCedilla = "s\xCC\xA7";  // s + U+0327: ayrışık ş
constexpr int kWide = 80;

EditorBuffer buffer_of(std::string text) {
    return EditorBuffer(std::move(text), test_width);
}

std::string selected(const EditorBuffer& b) {
    const auto range = b.selection();
    return range ? b.text().substr(range->begin, range->end - range->begin) : std::string{};
}

}  // namespace

TEST_CASE("buffer: cursor steps over multi-byte Turkish letters as one character") {
    auto b = buffer_of("aıİğş");
    b.move_doc_end(false);
    CHECK(b.cursor() == 9);
    for (const std::size_t expected : {7U, 5U, 3U, 1U, 0U}) {
        b.move_left(false);
        CHECK(b.cursor() == expected);
    }
    for (const std::size_t expected : {1U, 3U, 5U, 7U, 9U}) {
        b.move_right(false);
        CHECK(b.cursor() == expected);
    }
}

TEST_CASE("buffer: a combining mark moves and deletes together with its base") {
    auto b = buffer_of(std::string("a") + std::string(kCombiningCedilla) + "b");
    b.move_right(false);
    b.move_right(false);
    CHECK(b.cursor() == 4);
    b.move_left(false);
    CHECK(b.cursor() == 1);
    b.move_right(false);
    b.backspace();
    CHECK(b.text() == "ab");
    CHECK(b.cursor() == 1);
}

TEST_CASE("buffer: backspace and delete remove a whole multi-byte character") {
    auto b = buffer_of("ğş");
    b.move_doc_end(false);
    b.backspace();
    CHECK(b.text() == "ğ");
    b.move_doc_start(false);
    b.delete_forward();
    CHECK(b.text().empty());
    b.backspace();
    b.delete_forward();
    CHECK(b.text().empty());
}

TEST_CASE("buffer: position counts graphemes per line, not bytes") {
    auto b = buffer_of("ığ\nşs\xCC\xA7x");
    b.move_doc_end(false);
    CHECK(b.position().line == 1);
    CHECK(b.position().column == 3);
    b.move_doc_start(false);
    CHECK(b.position().line == 0);
    CHECK(b.position().column == 0);
}

TEST_CASE("buffer: typing is one undo step per word and undo restores the cursor") {
    auto b = buffer_of("");
    for (const char* letter : {"m", "e", "r", " ", "ş", "u"})
        b.type_text(letter);
    CHECK(b.text() == "mer şu");
    CHECK(b.undo());
    CHECK(b.text() == "mer ");
    CHECK(b.undo());
    CHECK(b.text().empty());
    CHECK_FALSE(b.undo());
    CHECK(b.redo());
    CHECK(b.text() == "mer ");
    CHECK(b.redo());
    CHECK(b.text() == "mer şu");
    CHECK(b.cursor() == b.text().size());
    CHECK_FALSE(b.redo());
}

TEST_CASE("buffer: moving the cursor ends the typing run") {
    auto b = buffer_of("");
    b.type_text("a");
    b.move_left(false);
    b.type_text("b");
    CHECK(b.text() == "ba");
    b.undo();
    CHECK(b.text() == "a");
}

TEST_CASE("buffer: a new edit drops the redo history") {
    auto b = buffer_of("x");
    b.move_doc_end(false);
    b.type_text("y");
    b.undo();
    b.type_text("z");
    CHECK_FALSE(b.redo());
    CHECK(b.text() == "xz");
}

TEST_CASE("buffer: consecutive backspaces coalesce into one undo step") {
    auto b = buffer_of("abc");
    b.move_doc_end(false);
    b.backspace();
    b.backspace();
    CHECK(b.text() == "a");
    b.undo();
    CHECK(b.text() == "abc");
    CHECK(b.cursor() == 3);
}

TEST_CASE("buffer: shift-move selects and typing replaces the selection in one undo step") {
    auto b = buffer_of("merhaba");
    b.move_right(true);
    b.move_right(true);
    CHECK(selected(b) == "me");
    b.type_text("X");
    CHECK(b.text() == "Xrhaba");
    CHECK_FALSE(b.selection());
    b.undo();
    CHECK(b.text() == "merhaba");
    CHECK(selected(b) == "me");
}

TEST_CASE("buffer: an unextended move collapses the selection to its edge") {
    auto b = buffer_of("abcd");
    b.move_right(true);
    b.move_right(true);
    b.move_left(false);
    CHECK_FALSE(b.selection());
    CHECK(b.cursor() == 0);
    b.move_right(true);
    b.move_right(false);
    CHECK(b.cursor() == 1);
}

TEST_CASE("buffer: selection never splits a multi-byte character") {
    auto b = buffer_of("ğü");
    b.move_right(true);
    CHECK(selected(b) == "ğ");
}

TEST_CASE("buffer: copy and cut use the selection, or the line when nothing is selected") {
    auto b = buffer_of("bir\niki\nüç");
    b.move_rows(1, kWide, false);
    CHECK(b.copy() == "iki\n");
    CHECK(b.cut() == "iki\n");
    CHECK(b.text() == "bir\nüç");
    CHECK(b.position().line == 1);
    CHECK(b.position().column == 0);
    b.paste("iki\n");
    CHECK(b.text() == "bir\niki\nüç");
    b.undo();
    b.undo();
    CHECK(b.text() == "bir\niki\nüç");

    b.move_doc_start(false);
    b.move_right(true);
    CHECK(b.copy() == "b");
    CHECK(b.cut() == "b");
    CHECK(b.text() == "ir\niki\nüç");
}

TEST_CASE("buffer: cutting the last line without a newline does not add one") {
    auto b = buffer_of("a\nb");
    b.move_doc_end(false);
    CHECK(b.cut() == "b");
    CHECK(b.text() == "a\n");
}

TEST_CASE("buffer: select_range widens to grapheme boundaries and an empty range only moves the cursor") {
    auto b = buffer_of(std::string(kCombiningCedilla) + "ğ x");
    b.select_range({0, 1});  // s'in ardında birleşen işaret var
    CHECK(selected(b) == std::string(kCombiningCedilla));
    b.select_range({4, 5});  // ğ'nin ortası
    CHECK(selected(b) == "ğ");
    b.select_range({2, 2});
    CHECK_FALSE(b.selection());
    CHECK(b.cursor() == 3);
    b.select_range({6, 100});
    CHECK(b.cursor() == b.text().size());
}

TEST_CASE("buffer: set_scroll_line puts that line at the top and the cursor stays visible") {
    std::string text;
    for (int i = 0; i < 50; ++i)
        text += "satır\n";
    auto b = buffer_of(text);
    b.move_to({20, 0});
    b.set_scroll_line(20);
    b.scroll_to_cursor(kWide, 10);
    CHECK(b.scroll_top().line == 20);
    b.set_scroll_line(1000);
    CHECK(b.scroll_top().line == b.line_count() - 1);
}

TEST_CASE("buffer: long lines wrap into visual rows and vertical moves follow them") {
    auto b = buffer_of("0123456789abcd\nxy");
    const auto rows = b.rows_of_line(0, 10);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].begin == 0);
    CHECK(rows[0].end == 10);
    CHECK_FALSE(rows[0].last_of_line);
    CHECK(rows[1].begin == 10);
    CHECK(rows[1].end == 14);
    CHECK(rows[1].last_of_line);

    b.move_right(false);
    b.move_right(false);
    b.move_rows(1, 10, false);
    CHECK(b.cursor() == 12);
    CHECK(b.position().line == 0);
    CHECK(b.cursor_cell(10).row == 1);
    CHECK(b.cursor_cell(10).cell == 2);
    b.move_rows(1, 10, false);
    CHECK(b.position().line == 1);
    CHECK(b.position().column == 2);
    b.move_rows(1, 10, false);  // son satırda kalır
    CHECK(b.position().line == 1);
    b.move_rows(-1, 10, false);
    CHECK(b.cursor() == 12);
    b.move_rows(-1, 10, false);
    CHECK(b.cursor() == 2);
}

TEST_CASE("buffer: a row filled exactly leaves room for the end-of-line cursor") {
    auto b = buffer_of("abcd");
    b.move_doc_end(false);
    CHECK(b.rows_of_line(0, 4).size() == 2);
    CHECK(b.cursor_cell(4).row == 1);
    CHECK(b.cursor_cell(4).cell == 0);
}

TEST_CASE("buffer: wide characters wrap before they would overflow, tabs reach the next stop") {
    auto b = buffer_of("漢字");
    const auto rows = b.rows_of_line(0, 3);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].end == 3);
    CHECK(b.glyph_at(0, 0).cells == 2);

    auto tabbed = buffer_of("a\tb");
    CHECK(tabbed.glyph_at(1, 1).cells == EditorBuffer::kTabCells - 1);
    tabbed.move_doc_end(false);
    CHECK(tabbed.cursor_cell(kWide).cell == EditorBuffer::kTabCells + 1);
}

TEST_CASE("buffer: control bytes never reach the screen but stay in the text") {
    const std::string raw = "a\x1b[31m\x07\r\xC2\x9B"
                            "b";
    auto b = buffer_of(raw);
    std::string shown;
    for (std::size_t at = 0; at < raw.size();) {
        const auto glyph = b.glyph_at(at, 0);
        shown += glyph.shown;
        at = glyph.next;
    }
    for (const char c : shown) {
        const auto byte = static_cast<unsigned char>(c);
        CHECK((byte >= 0x20 && byte != 0x7F));
    }
    CHECK(shown.find("\xC2\x9B") == std::string::npos);  // C1 CSI de kaçış dizisi başlatabilir
    CHECK(b.text() == raw);
    CHECK_FALSE(b.dirty());
}

// Terminal bidi ve sıfır genişlikli karakterleri hücresiz çizer: sınama en kötü ölçüyle (0 hücre) yapılır.
int zero_width_hidden(char32_t cp) {
    constexpr char32_t kRloCode = 0x202E;
    constexpr char32_t kZwspCode = 0x200B;
    return cp == kRloCode || cp == kZwspCode ? 0 : test_width(cp);
}

TEST_CASE("buffer: a hidden bidi or zero-width character is its own one-cell marker glyph") {
    constexpr std::string_view kRlo = "\xE2\x80\xAE";   // U+202E
    constexpr std::string_view kZwsp = "\xE2\x80\x8B";  // U+200B
    const std::string raw = "a" + std::string(kRlo) + "b" + std::string(kZwsp) + "c";
    EditorBuffer b(raw, zero_width_hidden);
    // Hareket: her biri tek adım, önceki harfe yapışmaz.
    for (const std::size_t expected : {1U, 4U, 5U, 8U, 9U}) {
        b.move_right(false);
        CHECK(b.cursor() == expected);
    }
    for (const std::size_t expected : {8U, 5U, 4U, 1U, 0U}) {
        b.move_left(false);
        CHECK(b.cursor() == expected);
    }
    // Çizim: işaret, tek hücre, kod noktası Glyph'te; ham bayt ekrana gitmez.
    const auto marker = b.glyph_at(1, 1);
    CHECK(marker.next == 4);
    CHECK(marker.cells == 1);
    CHECK(marker.shown == "\xEF\xBF\xBD");
    CHECK(marker.hidden == char32_t{0x202E});
    CHECK(b.glyph_at(0, 0).shown == "a");
    CHECK(b.glyph_at(0, 0).hidden == 0);
    CHECK(b.glyph_at(5, 3).hidden == char32_t{0x200B});
    // Sütun ve hücre hesabı tutarlı: 5 karakter, 5 hücre.
    b.move_end(false);
    CHECK(b.position().column == 5);
    CHECK(b.cursor_cell(kWide).cell == 5);
    const auto rows = b.rows_of_line(0, 3);  // 3 hücrede sarılır: "a?b" + "?c"
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].end == 5);
    // Seç ve sil: yalnız o kod noktasının baytları gider.
    b.move_to({0, 1});
    b.move_right(true);
    CHECK(b.selection()->begin == 1);
    CHECK(b.selection()->end == 4);
    b.delete_forward();
    CHECK(b.text() == "ab" + std::string(kZwsp) + "c");
    b.move_to({0, 3});
    b.backspace();
    CHECK(b.text() == "abc");
    CHECK(b.undo());
    CHECK(b.undo());
    CHECK(b.text() == raw);
}

TEST_CASE("buffer: CRLF is one step, new lines follow the file and other lines stay untouched") {
    auto b = buffer_of("a\r\nb\nc\r\n");
    b.move_right(false);
    b.move_right(false);
    CHECK(b.cursor() == 3);
    CHECK(b.position().line == 1);
    b.move_left(false);
    CHECK(b.cursor() == 1);
    b.move_right(false);
    b.backspace();  // CRLF birlikte silinir
    CHECK(b.text() == "ab\nc\r\n");
    b.undo();
    CHECK(b.text() == "a\r\nb\nc\r\n");
    b.newline();
    CHECK(b.text() == "a\r\n\r\nb\nc\r\n");
    b.paste("x\ny\r\nz");
    CHECK(b.text() == "a\r\n\r\nx\r\ny\r\nzb\nc\r\n");
    CHECK(b.line_count() == 7);
}

TEST_CASE("buffer: an LF file keeps LF for new lines and a CRLF paste is normalized") {
    auto b = buffer_of("a\nb");
    b.paste("x\r\ny");
    CHECK(b.text() == "x\ny"
                      "a\nb");
    b.newline();
    CHECK(b.text() == "x\ny\n"
                      "a\nb");
}

TEST_CASE("buffer: the line ending of a row is not part of the row") {
    auto b = buffer_of("ab\r\ncd");
    CHECK(b.rows_of_line(0, kWide)[0].end == 2);
    b.move_end(false);
    CHECK(b.cursor() == 2);
    b.move_home(false);
    CHECK(b.cursor() == 0);
}

TEST_CASE("buffer: no trailing newline is ever added by editing") {
    auto b = buffer_of("son");
    b.move_doc_end(false);
    b.type_text("!");
    CHECK(b.text() == "son!");
}

TEST_CASE("buffer: dirty follows content: type then undo, or retype the same bytes, is clean") {
    auto b = buffer_of("a");
    CHECK_FALSE(b.dirty());
    b.type_text("x");
    CHECK(b.dirty());
    b.backspace();
    CHECK_FALSE(b.dirty());
    b.type_text("y");
    b.mark_saved();
    CHECK_FALSE(b.dirty());
    b.undo();
    CHECK(b.dirty());
}

TEST_CASE("buffer: undo memory stays within the limit and the oldest steps go first") {
    constexpr std::size_t kLimit = 1024;
    EditorBuffer b("", test_width, kLimit);
    const std::string chunk(100, 'x');
    for (int i = 0; i < 50; ++i) {
        b.paste(chunk);
        CHECK(b.undo_bytes() <= kLimit);
    }
    int steps = 0;
    while (b.undo())
        ++steps;
    CHECK(steps > 0);
    CHECK(steps < 50);
    CHECK(b.text().size() == (50 - steps) * chunk.size());
}

TEST_CASE("buffer: an edit bigger than the limit is applied but cannot be undone") {
    constexpr std::size_t kLimit = 16;
    EditorBuffer b("", test_width, kLimit);
    b.paste(std::string(100, 'x'));
    CHECK(b.text().size() == 100);
    CHECK(b.undo_bytes() <= kLimit);
    CHECK_FALSE(b.undo());
    CHECK(b.text().size() == 100);
}

TEST_CASE("buffer: scrolling keeps the cursor inside the window, also after a resize") {
    std::string text;
    for (int i = 0; i < 100; ++i)
        text += "satır " + std::to_string(i) + "\n";
    auto b = buffer_of(text);
    b.move_rows(50, kWide, false);
    b.scroll_to_cursor(kWide, 10);
    CHECK(b.scroll_top().line == 41);
    auto visible = b.visible_rows(kWide, 10);
    REQUIRE(visible.size() == 10);
    CHECK(visible.back().line == 50);

    b.scroll_to_cursor(kWide, 3);  // pencere küçüldü
    CHECK(b.scroll_top().line == 48);
    b.scroll_to_cursor(kWide, 3);  // zaten görünür: oynamaz
    CHECK(b.scroll_top().line == 48);

    b.move_doc_start(false);
    b.scroll_to_cursor(kWide, 3);
    CHECK(b.scroll_top().line == 0);
}

TEST_CASE("buffer: scrolling counts wrapped rows") {
    auto b = buffer_of(std::string(25, 'x') + "\nkısa\nson");
    b.move_doc_end(false);
    b.scroll_to_cursor(10, 3);
    CHECK(b.scroll_top().line == 0);  // ilk satırın son görsel satırı + iki kısa satır = 3
    CHECK(b.scroll_top().row == 2);
    CHECK(b.visible_rows(10, 3).size() == 3);
    b.move_to({0, 5});
    b.scroll_to_cursor(10, 3);
    CHECK(b.scroll_top().line == 0);
    CHECK(b.scroll_top().row == 0);
}

TEST_CASE("buffer: reload keeps line and column when possible and clears history") {
    auto b = buffer_of("bir\niki\nüç");
    b.move_to({1, 2});
    b.reload("yeni\nsatırlar\nburada\n");
    CHECK_FALSE(b.dirty());
    CHECK(b.position().line == 1);
    CHECK(b.position().column == 2);
    CHECK_FALSE(b.undo());
    b.move_to({2, 3});
    b.reload("tek");
    CHECK(b.position().line == 0);
    CHECK(b.position().column == 3);
}

TEST_CASE("buffer: a random edit sequence undoes back to the exact original bytes") {
    static constexpr std::array kSnippets = {
        std::string_view("a"),
        std::string_view("ş"),
        std::string_view("İ"),
        std::string_view(" "),
        std::string_view("漢"),
        std::string_view(kCombiningCedilla),
        std::string_view("\xE2\x80\xA8"),
    };
    const std::string original = "\xEF\xBF\xBD"
                                 "başlık\r\nkarışık\nsatır\x1b[0m\ts\xCC\xA7\r\n漢字 son";
    for (const unsigned seed : {1U, 7U, 42U, 2026U}) {
        CAPTURE(seed);
        std::mt19937 random(seed);
        auto pick = [&](std::size_t bound) {
            return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
        };
        auto b = buffer_of(original);
        std::string clipboard;
        for (int step = 0; step < 2000; ++step) {
            switch (pick(14)) {
            case 0:
            case 1:
            case 2:
                b.type_text(kSnippets[pick(kSnippets.size())]);
                break;
            case 3:
                b.backspace();
                break;
            case 4:
                b.delete_forward();
                break;
            case 5:
                b.newline();
                break;
            case 6:
                b.move_left(pick(2) == 0);
                break;
            case 7:
                b.move_right(pick(2) == 0);
                break;
            case 8:
                b.move_rows(pick(2) == 0 ? 1 : -1, 1 + static_cast<int>(pick(12)), pick(2) == 0);
                break;
            case 9:
                b.move_home(pick(2) == 0);
                break;
            case 10:
                b.move_end(pick(2) == 0);
                break;
            case 11:
                clipboard = b.cut();
                break;
            case 12:
                b.paste(clipboard);
                break;
            default:
                b.select_range({pick(b.text().size() + 1), pick(b.text().size() + 1)});
                break;
            }
            REQUIRE(noteapp::core::is_valid_utf8(b.text()));
            REQUIRE(b.cursor() <= b.text().size());
            REQUIRE((b.cursor() == b.text().size() ||
                     (static_cast<unsigned char>(b.text()[b.cursor()]) & 0xC0) != 0x80));
        }
        const std::string edited = b.text();
        while (b.undo()) {
        }
        CHECK(b.text() == original);
        CHECK_FALSE(b.dirty());
        while (b.redo()) {
        }
        CHECK(b.text() == edited);
    }
}

namespace {

// Önbelleksiz, grapheme grapheme kaynak hesap (glyph_at): konum ve sarmanın toplu ve artımlı yolları buna
// eşit.
TextPosition position_by_glyphs(const EditorBuffer& b) {
    std::size_t line = 0;
    while (line + 1 < b.line_count() && b.line_start(line + 1) <= b.cursor())
        ++line;
    std::size_t column = 0;
    for (std::size_t at = b.line_start(line); at < b.cursor(); at = b.glyph_at(at, 0).next)
        ++column;
    return {line, column};
}

std::vector<noteapp::tui::VisualRow> rows_by_glyphs(const EditorBuffer& b, std::size_t line, int columns) {
    const int limit = std::max(columns, EditorBuffer::kMinColumns);
    const std::size_t end = b.line_start(line) + b.line_text(line).size();
    std::vector<noteapp::tui::VisualRow> rows;
    std::size_t row_begin = b.line_start(line);
    int cell = 0;
    for (std::size_t at = row_begin; at < end;) {
        const noteapp::tui::Glyph glyph = b.glyph_at(at, cell);
        int cells = glyph.cells;
        if (cell > 0 && cell + cells > limit) {
            rows.push_back({line, row_begin, at, false});
            row_begin = at;
            cell = 0;
            cells = b.glyph_at(at, cell).cells;
        }
        cell += cells;
        at = glyph.next;
    }
    if (cell >= limit) {
        rows.push_back({line, row_begin, end, false});
        row_begin = end;
    }
    rows.push_back({line, row_begin, end, true});
    return rows;
}

}  // namespace

// Uzun satırda konum ve sarma önbelleği düzenlemeden sonra yalnız eskiyen kısmı yeniden hesaplar, ASCII
// koşusunu toplu atlar: her adımda grapheme grapheme kaynak hesapla aynı sonucu vermeli.
TEST_CASE("buffer: cached position and wrapped rows match a glyph-by-glyph walk after every edit") {
    static constexpr std::array kSnippets = {
        std::string_view("a"),
        std::string_view("abc d"),
        std::string_view("ş"),
        std::string_view("\t"),
        std::string_view("漢"),
        std::string_view(kCombiningCedilla),
        std::string_view("\xCC\xA7"),
        std::string_view("\x1b"),
        std::string_view("\xE2\x80\xAE"),
        std::string_view("\r"),
        std::string_view("字"),                                            // 3 bayt, geniş
        std::string_view("\xF0\x9F\x87\xB9\xF0\x9F\x87\xB7"),              // U+1F1F9 U+1F1F7: bayrak
        std::string_view("\xF0\x9F\x87\xA6"),                              // U+1F1E6: tek bölgesel gösterge
        std::string_view("\xF0\x9F\x98\x80"),                              // U+1F600
        std::string_view("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"),  // U+1F468 ZWJ U+1F469
        std::string_view("\xE2\x80\x8D"),                                  // tek ZWJ
    };
    constexpr int kColumns = 7;
    const auto same_rows = [](const std::vector<noteapp::tui::VisualRow>& a,
                              const std::vector<noteapp::tui::VisualRow>& b) {
        return std::ranges::equal(a, b, [](const auto& x, const auto& y) {
            return x.line == y.line && x.begin == y.begin && x.end == y.end &&
                   x.last_of_line == y.last_of_line;
        });
    };
    for (const unsigned seed : {3U, 11U, 99U}) {
        CAPTURE(seed);
        std::mt19937 random(seed);
        auto pick = [&](std::size_t bound) {
            return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
        };
        auto b = buffer_of("uzun bir satır\tsekmeli s\xCC\xA7 漢字 ve daha fazlası\r\nikinci\nson");
        std::string clipboard;
        const auto apply_step = [&] {
            switch (pick(13)) {
            case 0:
            case 1:
            case 2:
            case 3:
                b.type_text(kSnippets[pick(kSnippets.size())]);
                break;
            case 4:
                b.backspace();
                break;
            case 5:
                b.delete_forward();
                break;
            case 6:
                b.newline();
                break;
            case 7:
                b.move_left(false);
                break;
            case 8:
                b.undo();
                break;
            case 9:
                b.redo();
                break;
            case 10:
                clipboard = b.cut();
                break;
            case 11:
                b.paste(clipboard);
                break;
            default:
                b.select_range({pick(b.text().size() + 1), pick(b.text().size() + 1)});
                break;
            }
        };
        for (int step = 0; step < 3000; ++step) {
            // Bazen iki-üç düzenleme arada sorgu olmadan: önbellek eskime noktalarının en küçüğünden kurulur.
            const std::size_t burst = pick(5) == 0 ? 2 + pick(2) : 1;
            for (std::size_t i = 0; i < burst; ++i)
                apply_step();
            const TextPosition got = b.position();
            const TextPosition want = position_by_glyphs(b);
            REQUIRE(got.line == want.line);
            REQUIRE(got.column == want.column);
            REQUIRE(same_rows(b.rows_of_line(want.line, kColumns), rows_by_glyphs(b, want.line, kColumns)));
            if (pick(8) == 0) {  // başka satır ya da genişlik önbelleği değiştirir: tam yeniden kurulum yolu
                const std::size_t line = pick(b.line_count());
                const int columns = pick(2) == 0 ? kColumns : 3;
                REQUIRE(same_rows(b.rows_of_line(line, columns), rows_by_glyphs(b, line, columns)));
            }
        }
    }
}

TEST_CASE("edit_count grows on every text change and not on motion") {
    EditorBuffer b = buffer_of("ab\ncd");
    const auto start = b.edit_count();
    b.move_right(false);
    b.move_rows(1, kWide, true);
    b.clear_selection();
    CHECK(b.edit_count() == start);
    b.type_text("x");
    b.type_text("y");  // aynı geri alma adımına birleşir ama iki değişikliktir
    CHECK(b.edit_count() == start + 2);
    b.undo();
    CHECK(b.edit_count() == start + 3);
    b.redo();
    b.backspace();
    b.newline();
    b.paste("p");
    CHECK(b.edit_count() == start + 7);
    b.reload("fresh");
    CHECK(b.edit_count() == start + 8);
}
