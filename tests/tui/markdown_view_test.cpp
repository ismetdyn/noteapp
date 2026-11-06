#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/clipboard.hpp"
#include "edit/display_text.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/markdown_view.hpp"
#include "tui/mouse.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Key;
using tui::RenderedView;
using tui::TextLook;
using tui::TokenKind;

namespace {

constexpr int kWideView = 80;

RenderedView md(std::string_view source, int width = kWideView) {
    return tui::render_markdown(source, width, tui::terminal_cell_width);
}

std::string joined(const RenderedView& view) {
    std::string all;
    for (const std::string& line : view.lines)
        all += line + "\n";
    return all;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// `needle`ın ilk geçtiği satır; yoksa satır sayısı.
std::size_t line_with(const RenderedView& view, std::string_view needle) {
    for (std::size_t i = 0; i < view.lines.size(); ++i)
        if (contains(view.lines[i], needle))
            return i;
    return view.lines.size();
}

// `needle`ın ilk baytının görünüşü.
TextLook look_of(const RenderedView& view, std::string_view needle) {
    const std::size_t line = line_with(view, needle);
    REQUIRE(line < view.lines.size());
    const std::size_t at = view.lines[line].find(needle);
    for (const tui::StyledSpan& span : view.spans[line])
        if (at >= span.begin && at < span.end)
            return span.look;
    return {};
}

void check_fits(const RenderedView& view, int width) {
    for (const std::string& line : view.lines) {
        CAPTURE(line);
        CHECK(ftxui::string_width(line) <= width);
    }
}

void check_parallel(const RenderedView& view) {
    CHECK(view.spans.size() == view.lines.size());
    CHECK(view.source_lines.size() == view.lines.size());
    CHECK(std::ranges::is_sorted(view.source_lines));
}

// Türkçe katalog testi bitince İngilizceye döner: diğer testler onu varsayar.
struct TurkishCatalog {
    TurkishCatalog() { core::init_i18n("tr"); }
    ~TurkishCatalog() { core::init_i18n("en"); }
    TurkishCatalog(const TurkishCatalog&) = delete;
    TurkishCatalog& operator=(const TurkishCatalog&) = delete;
};

template <typename Work> long elapsed_ms(const Work& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count());
}

// Girdi boyuna göre doğrusal işin Debug derlemede bile rahat sığdığı bütçe; ikinci dereceden davranış aşar.
constexpr long kHostileBudgetMs = 5000;

std::string repeated(std::string_view unit, std::size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (std::size_t i = 0; i < count; ++i)
        out += unit;
    return out;
}

}  // namespace

TEST_CASE("markdown headings keep level markers and each level looks different") {
    const RenderedView view = md("# Bir\n\n## İki\n\n### Üç\n\ndüz\n");
    check_parallel(view);
    CHECK(line_with(view, "# Bir") < view.lines.size());
    CHECK(line_with(view, "## İki") < view.lines.size());
    CHECK(line_with(view, "### Üç") < view.lines.size());
    CHECK(look_of(view, "Bir").bold);
    CHECK(look_of(view, "İki").bold);
    CHECK(look_of(view, "Bir") != look_of(view, "İki"));
    CHECK(look_of(view, "İki") != look_of(view, "Üç"));
    CHECK(look_of(view, "düz") == TextLook{});
}

TEST_CASE("markdown inline styles drop their delimiters") {
    const RenderedView view = md("a **kalın** b *eğik* c ~~silik~~ d `kod` e\n");
    const std::string all = joined(view);
    CHECK_FALSE(contains(all, "**"));
    CHECK_FALSE(contains(all, "~~"));
    CHECK_FALSE(contains(all, "`"));
    CHECK(look_of(view, "kalın").bold);
    CHECK(look_of(view, "eğik").italic);
    CHECK(look_of(view, "silik").strike);
    CHECK(look_of(view, "kod").token == TokenKind::Code);
    CHECK(look_of(view, "a ") == TextLook{});
}

TEST_CASE("fenced code uses the syntax tokenizer of its language tag and keeps its lines") {
    const RenderedView view = md("```python\ndef f():\n\n    return 1\n```\n\n```nosuchlang\nabc\n```\n");
    const std::string all = joined(view);
    CHECK_FALSE(contains(all, "```"));
    CHECK(look_of(view, "def").token == TokenKind::Keyword);
    CHECK(look_of(view, "return").token == TokenKind::Keyword);
    CHECK(look_of(view, "abc").token == TokenKind::Code);
    CHECK(view.source_lines[line_with(view, "def")] == 1);
    CHECK(view.source_lines[line_with(view, "return")] == 3);
    CHECK(line_with(view, "return") == line_with(view, "def") + 2);
}

TEST_CASE("tabs in code become spaces and long code lines wrap inside the width") {
    const RenderedView view = md("```\n\tx\n" + repeated("uzun ", 40) + "\n```\n", 30);
    CHECK(contains(view.lines[0], "    x"));
    CHECK_FALSE(contains(joined(view), "\t"));
    check_fits(view, 30);
    CHECK(view.lines.size() > 3);
}

TEST_CASE("block quotes carry a bar on every wrapped line and show nesting") {
    const RenderedView view = md("> " + repeated("sözcük ", 30) + "\n>\n> > iç\n", 40);
    check_fits(view, 40);
    REQUIRE(view.lines.size() > 3);
    for (const std::string& line : view.lines)
        CHECK(line.starts_with("│"));
    CHECK(view.lines[line_with(view, "iç")].starts_with("│ │ "));
    CHECK(look_of(view, "│").token == TokenKind::Comment);
}

TEST_CASE("the gap around a quote carries no bar") {
    const RenderedView view = md("önce\n\n> iç\n\nsonra\n");
    const std::vector<std::string> expected = {"önce", "", "│ iç", "", "sonra"};
    CHECK(view.lines == expected);
}

TEST_CASE("lists nest, number from their start and hang wrapped lines under the text") {
    const RenderedView view =
        md("- bir\n  - iç\n    - dip\n- " + repeated("uzun ", 30) + "\n\n3. üç\n4. dört\n", 40);
    check_fits(view, 40);
    const std::size_t outer = line_with(view, "bir");
    const std::size_t inner = line_with(view, "iç");
    const std::size_t deep = line_with(view, "dip");
    CHECK(view.lines[outer].starts_with("• bir"));
    CHECK(view.lines[inner].find("iç") > view.lines[outer].find("bir"));
    CHECK(view.lines[deep].find("dip") > view.lines[inner].find("iç"));
    CHECK(inner == outer + 1);  // sıkı liste: boş satır yok
    const std::size_t wrapped = line_with(view, "uzun");
    CHECK(view.lines[wrapped].starts_with("• uzun"));
    CHECK(view.lines[wrapped + 1].starts_with("  uzun"));
    CHECK(line_with(view, "3. üç") + 1 == line_with(view, "4. dört"));
}

TEST_CASE("task lists show their checkbox state") {
    const RenderedView view = md("- [x] bitti\n- [ ] kaldı\n");
    CHECK(contains(view.lines[line_with(view, "bitti")], "[x] bitti"));
    CHECK(contains(view.lines[line_with(view, "kaldı")], "[ ] kaldı"));
}

TEST_CASE("horizontal rule fills the width") {
    const RenderedView view = md("üst\n\n---\n\nalt\n", 20);
    const std::size_t rule = line_with(view, "─");
    REQUIRE(rule < view.lines.size());
    CHECK(ftxui::string_width(view.lines[rule]) == 20);
    CHECK(look_of(view, "─").dim);
}

TEST_CASE("links show text and a dim url, never a terminal hyperlink escape") {
    const RenderedView view = md("bak [metin](https://ör.nek/yol) ve <https://oto.link>\n");
    const std::string all = joined(view);
    CHECK(contains(all, "metin (https://ör.nek/yol)"));
    CHECK(look_of(view, "metin").underline);
    CHECK(look_of(view, "https://ör").dim);
    CHECK_FALSE(look_of(view, "metin").dim);
    CHECK(contains(all, "https://oto.link"));
    CHECK_FALSE(contains(all, "(https://oto.link)"));  // otomatik bağlantı iki kez yazılmaz
    CHECK_FALSE(contains(all, "\x1b"));
}

TEST_CASE("bare addresses appear once and a short link text does not hide its url") {
    const std::string all = joined(md("www.ornek.com ve [x](http://x)\n"));
    CHECK(contains(all, "www.ornek.com"));
    CHECK_FALSE(contains(all, "(http://www.ornek.com)"));
    CHECK(contains(all, "x (http://x)"));
}

TEST_CASE("images become a placeholder with their alt text and no address") {
    const TurkishCatalog turkish;
    const std::string all = joined(md("![kedi fotoğrafı](http://iz.le/k.png)\n\n![](yerel.png)\n"));
    CHECK(contains(all, "[görsel: kedi fotoğrafı]"));
    CHECK(contains(all, "[görsel]"));
    CHECK_FALSE(contains(all, "iz.le"));
    CHECK_FALSE(contains(all, "yerel.png"));
}

TEST_CASE("html stays literal text") {
    const std::string all = joined(md("<div onclick=\"x\">a</div>\n\nsatır <b>içi</b>\n"));
    CHECK(contains(all, "<div onclick=\"x\">a</div>"));
    CHECK(contains(all, "<b>içi</b>"));
}

TEST_CASE("tables wrap their cells and fit every width with aligned borders") {
    const std::string source = "| Ad | Açıklama uzun başlık | Not |\n|---|---|---|\n"
                               "| Çağrı | şu İstanbul'daki çok uzun açıklama metni sarılmalı | **kalın** |\n"
                               "| Ayşe | kısa | |\n";
    for (const int width : {20, 30, 40, 60, 80, 120}) {
        CAPTURE(width);
        const RenderedView view = md(source, width);
        check_parallel(view);
        check_fits(view, width);
        const std::string all = joined(view);
        CHECK(contains(all, "Ayşe"));
        if (width >= 30)  // daha dar sütun sözcüğü de böler
            CHECK(contains(all, "kalın"));
        // Her satırda sütun kenarları aynı hücrede: tablo satırlarının genişliği eşit.
        const int cells = ftxui::string_width(view.lines[0]);
        for (const std::string& line : view.lines)
            CHECK(ftxui::string_width(line) == cells);
    }
    const RenderedView view = md(source);
    CHECK(look_of(view, "Ad").bold);
    CHECK(line_with(view, "┼") == line_with(view, "Ad") + 1);
    CHECK(look_of(view, "kalın").bold);
}

TEST_CASE("columns that cannot fit are dropped and said so") {
    std::string header = "|";
    std::string rule = "|";
    std::string row = "|";
    for (int i = 0; i < 40; ++i) {
        header += " h" + std::to_string(i) + " |";
        rule += "---|";
        row += " c |";
    }
    const RenderedView view = md(header + "\n" + rule + "\n" + row + "\n", 40);
    check_fits(view, 40);
    CHECK(contains(joined(view), "34"));  // 40 sütunun 6sı sığar, gerisi notta
    CHECK(look_of(view, "h0").bold);
    CHECK(line_with(view, "h39") == view.lines.size());
}

TEST_CASE("wrapping is by cells and never splits a Turkish code point") {
    const std::string source = repeated("Çığ İşçi ÖĞÜŞ ", 40) + "\n";
    for (const int width : {10, 17, 33, 80}) {
        CAPTURE(width);
        const RenderedView view = md(source, width);
        check_fits(view, width);
        for (const std::string& line : view.lines)
            CHECK(core::is_valid_utf8(line));
    }
    const RenderedView narrow = md(repeated("ş", 50) + "\n", 10);  // boşluksuz sözcük hücrede kesilir
    CHECK(narrow.lines.size() == 5);
    CHECK(ftxui::string_width(narrow.lines[0]) == 10);
    const RenderedView wide = md(repeated("界", 11) + "\n", 10);  // geniş karakter sınırı aşmaz
    check_fits(wide, 10);
}

TEST_CASE("markdown view: bidi and zero-width characters render as a visible marker in their place") {
    const std::string joined_text = joined(md("# ba\xe2\x80\xaeşlık\n\nmetin\xe2\x80\x8b"
                                              "x \xe2\x81\xa6y `k\xe2\x80\x8dz`\n"));
    CHECK_FALSE(contains(joined_text, "\xe2\x80\xae"));
    CHECK_FALSE(contains(joined_text, "\xe2\x80\x8b"));
    CHECK_FALSE(contains(joined_text, "\xe2\x81\xa6"));
    CHECK(contains(joined_text, "k\xe2\x80\x8dz"));  // ZWJ gizli karakter sayılmaz
    CHECK(contains(joined_text, "ba\xef\xbf\xbdşlık"));
    CHECK(contains(joined_text, "metin\xef\xbf\xbdx"));
    CHECK(contains(joined_text, "\xef\xbf\xbdy"));
}

// NY-P inceleme: varlıkla yazılan denetim karakteri (C1 CSI U+009B, DEL, bidi U+202E) de terminale ulaşmaz; gövde
// ve bağlantı adresi (soluk URL) aynı arındırmadan geçer.
TEST_CASE("entity-encoded control and bidi characters never reach the terminal") {
    const RenderedView view = md("x &#x9b;2J &#x202E; &#x7f; &#X1b;[31m\n\n[l](http://a&#x9b;2J/&#x202e;)\n");
    for (const std::string& line : view.lines) {
        CAPTURE(line);
        CHECK(line == tui::terminal_safe(line));
        CHECK_FALSE(contains(line, "\xc2\x9b"));
        CHECK_FALSE(contains(line, "\xe2\x80\xae"));
        CHECK_FALSE(contains(line, "\x7f"));
    }
}

TEST_CASE("control and bidi bytes never reach the terminal") {
    const std::string hostile =
        "# ba\x1b[31mşlık\n\nmetin \x1b]8;;http://x\x07y\xc2\x9b z \xe2\x80\xae\x07\n\n"
        "```\nkod\x1b[2J\n```\n\n[l\x1b[0m](http://a\x1b/b)\n";
    const RenderedView view = md(hostile);
    for (const std::string& line : view.lines) {
        CAPTURE(line);
        CHECK(line == tui::terminal_safe(line));
        CHECK_FALSE(contains(line, "\xe2\x80\xae"));
        CHECK_FALSE(contains(line, "\x1b"));
    }
}

TEST_CASE("entities decode and a leading bom does not eat the heading") {
    const RenderedView view = md("\xEF\xBB\xBF# başlık &amp; &#351;ey\n");
    CHECK(view.lines[0] == "# başlık & şey");
    CHECK(view.source_lines[0] == 0);
    CHECK(look_of(view, "başlık").bold);
}

TEST_CASE("every block maps to its source line") {
    const std::string source = "# başlık\n"      // 0
                               "\n"              // 1
                               "paragraf bir\n"  // 2
                               "ikinci satır\n"  // 3
                               "\n"              // 4
                               "- öğe\n"         // 5
                               "- öğe2\n"        // 6
                               "\n"              // 7
                               "---\n"           // 8
                               "\n"              // 9
                               "> alıntı\n"      // 10
                               "\n"              // 11
                               "| a |\n"         // 12
                               "|---|\n"         // 13
                               "| hücre |\n"     // 14
                               "\n"              // 15
                               "```\n"           // 16
                               "kod\n"           // 17
                               "```\n"           // 18
                               "\n"              // 19
                               "***\n"           // 20
                               "\n"              // 21
                               "son\n";          // 22
    const RenderedView view = md(source);
    check_parallel(view);
    const auto line_of = [&](std::string_view text) { return view.source_lines[line_with(view, text)]; };
    CHECK(line_of("başlık") == 0);
    CHECK(line_of("paragraf") == 2);
    CHECK(line_of("öğe2") == 6);
    CHECK(line_of("─") == 8);
    CHECK(line_of("alıntı") == 10);
    CHECK(line_of("hücre") == 14);
    CHECK(line_of("kod") == 17);
    CHECK(line_of("son") == 22);
    const auto second_rule = std::find_if(view.lines.begin() + static_cast<long>(line_with(view, "─") + 1),
                                          view.lines.end(), [](const std::string& line) {
                                              return line.starts_with("─");  // tablo çizgisi "├" ile başlar
                                          });
    REQUIRE(second_rule != view.lines.end());
    CHECK(view.source_lines[static_cast<std::size_t>(second_rule - view.lines.begin())] == 20);
}

TEST_CASE("an unclosed fence ended by dedent does not eat the next line") {
    for (const std::string_view source : {"- ```\n  kod\n---\n\nson\n", "> ```\n> kod\n---\n\nson\n"}) {
        CAPTURE(source);
        const RenderedView view = md(source);
        CHECK(view.source_lines[line_with(view, "kod")] == 1);
        CHECK(view.source_lines[line_with(view, "─")] == 2);
        CHECK(view.source_lines[line_with(view, "son")] == 4);
    }
}

TEST_CASE("a real closing fence is consumed in containers and after inner blank lines") {
    for (const std::string_view source :
         {"```\nkod\n\n```\n\n---\n", "> ~~~\n> kod\n> ~~~~~\n\n---\n", "- ```\n  kod\n  ```\n\n---\n"}) {
        CAPTURE(source);
        const RenderedView view = md(source);
        const std::string text(source);
        const auto rule =
            static_cast<std::size_t>(std::ranges::count(text.substr(0, text.rfind("---")), '\n'));
        CHECK(view.source_lines[line_with(view, "─")] == rule);
    }
}

TEST_CASE("link addresses show decoded entities and stay terminal safe") {
    const std::string all = joined(md("[a](http://x/?a=1&amp;b=2) [b](http://y/&#x202E;z&#27;w)\n"));
    CHECK(contains(all, "(http://x/?a=1&b=2)"));
    CHECK_FALSE(contains(all, "&amp;"));
    CHECK_FALSE(contains(all, "\xe2\x80\xae"));
    CHECK_FALSE(contains(all, "\x1b"));
    CHECK(contains(all, "(http://y/\xEF\xBF\xBDz\xEF\xBF\xBDw)"));
}

TEST_CASE("hostile nesting stays bounded, inside the width and fast") {
    const std::vector<std::string> inputs = {
        repeated(">", 200000) + " dip\n",
        repeated("> ", 20000) + "x\n" + repeated("> ", 20000) + "y\n",
        [] {
            std::string nested;
            for (std::size_t depth = 0; depth < 1000; ++depth)
                nested += std::string(depth * 2, ' ') + "- x\n";
            return nested;
        }(),
        repeated("- ", 100000) + "x\n",
        repeated("[", 200000),
        repeated("*a ", 200000),
        repeated("**a*", 100000),
        repeated("![", 100000),
        repeated("`", 200000),
        repeated("<a ", 100000),
        repeated("&amp", 100000),
    };
    for (const std::string& input : inputs) {
        CAPTURE(input.substr(0, 16));
        const long ms = elapsed_ms([&] {
            const RenderedView view = md(input, 40);
            check_parallel(view);
            check_fits(view, 40);
            CHECK(view.lines.size() <= tui::ViewLimits{}.lines + 1);
        });
        if (noteapp::test::kWallClockBudgets)
            CHECK(ms < kHostileBudgetMs);
    }
}

// Varsayılan sınırsızdır (NY-P); kullanıcının verdiği limits.tui.table_rows tabloyu keser ve kalanı söyler.
constexpr std::size_t kTableRowsCap = 5000;

TEST_CASE("huge tables are bounded by a configured row limit and say how much is hidden") {
    std::string rows;
    for (int i = 0; i < 100000; ++i)
        rows += "| a" + std::to_string(i) + " | b |\n";
    const std::string source = "| x | y |\n|---|---|\n" + rows;
    tui::ViewLimits capped;
    capped.table_rows = kTableRowsCap;
    const long ms = elapsed_ms([&] {
        const RenderedView view = tui::render_markdown(source, 40, tui::terminal_cell_width, capped);
        check_fits(view, 40);
        CHECK(view.lines.size() <= kTableRowsCap + 3);
        CHECK(contains(joined(view), std::to_string(100000 + 1 - kTableRowsCap)));
    });
    if (noteapp::test::kWallClockBudgets)
        CHECK(ms < kHostileBudgetMs);

    std::string wide_row = "|";
    std::string wide_rule = "|";
    for (int i = 0; i < 20000; ++i) {
        wide_row += "a|";
        wide_rule += "-|";
    }
    const long wide_ms = elapsed_ms([&] {
        const RenderedView view = md(wide_row + "\n" + wide_rule + "\n" + wide_row + "\n", 40);
        check_fits(view, 40);
    });
    if (noteapp::test::kWallClockBudgets)
        CHECK(wide_ms < kHostileBudgetMs);
}

TEST_CASE("very long paragraph is cut at the line limit with a note") {
    const RenderedView view = md(repeated("ab ", 400000) + "\n", 4);
    CHECK(view.lines.size() == tui::ViewLimits{}.lines + 1);
    CHECK(look_of(view, view.lines.back()).dim);
    check_parallel(view);
}

// L2: limits.tui.view_lines/table_rows/table_columns 0 (SIZE_MAX) ise eski tavanların üstü kesilmez.
TEST_CASE("rendered views above the old line, row and column caps are whole when the limits are unlimited") {
    constexpr std::size_t kUnlimited = SIZE_MAX;
    const tui::ViewLimits unlimited{
        .lines = kUnlimited, .table_rows = kUnlimited, .table_columns = kUnlimited};

    const RenderedView paragraph =
        tui::render_markdown(repeated("ab ", 400000) + "\n", 4, tui::terminal_cell_width, unlimited);
    CHECK(paragraph.lines.size() > tui::ViewLimits{}.lines + 1);
    CHECK_FALSE(look_of(paragraph, paragraph.lines.back()).dim);

    std::string rows;
    for (int i = 0; i < 6000; ++i)
        rows += "| a" + std::to_string(i) + " | b |\n";
    const RenderedView table =
        tui::render_markdown("| x | y |\n|---|---|\n" + rows, 40, tui::terminal_cell_width, unlimited);
    CHECK(contains(joined(table), "a5999"));

    std::string csv = "a,b\n";
    for (int i = 0; i < 6000; ++i)
        csv += std::to_string(i) + ",2\n";
    const auto whole = tui::render_csv(csv, 40, tui::terminal_cell_width, unlimited);
    REQUIRE(whole);
    CHECK(contains(joined(*whole), "5999"));

    // 130 sütun (eski tavan 128): saklanan sütun sayısı genişliğe sığanı kısıtlamaz; dar ekranda sığmayan
    // sütun yine soluk satırla söylenir, ama 128'in ötesi atılmaz.
    std::string header;
    std::string values;
    for (int c = 0; c < 130; ++c) {
        header += (c == 0 ? "" : ",") + std::string("h") + std::to_string(c);
        values += (c == 0 ? "" : ",") + std::string("v") + std::to_string(c);
    }
    const auto wide =
        tui::render_csv(header + "\n" + values + "\n", 2000, tui::terminal_cell_width, unlimited);
    REQUIRE(wide);
    CHECK(contains(joined(*wide), "v129"));
}

TEST_CASE("empty markdown is one empty line") {
    const RenderedView view = md("");
    REQUIRE(view.lines.size() == 1);
    CHECK(view.lines[0].empty());
}

TEST_CASE("csv becomes an aligned table with the detected delimiter") {
    const auto view = tui::render_csv("ad;yaş;şehir\nÇağrı;30;\"İstanbul; merkez\"\nAli;4;Van\n", 60,
                                      tui::terminal_cell_width);
    REQUIRE(view);
    check_fits(*view, 60);
    CHECK(look_of(*view, "ad").bold);
    CHECK(contains(joined(*view), "İstanbul; merkez"));
    const std::size_t first = line_with(*view, "Çağrı");
    const std::size_t second = line_with(*view, "Ali");
    CHECK(ftxui::string_width(view->lines[first]) == ftxui::string_width(view->lines[second]));
    CHECK(view->source_lines[first] == 1);
    CHECK(view->source_lines[second] == 2);
}

TEST_CASE("csv handles quotes, crlf, short rows and hostile input") {
    const auto view = tui::render_csv("a,b\r\n\"x\"\"y\",1\r\n" + repeated("3,4\r\n", 10) + "2\r\n", 40,
                                      tui::terminal_cell_width);
    REQUIRE(view);
    const std::string all = joined(*view);
    CHECK(contains(all, "x\"y"));
    CHECK_FALSE(contains(all, "\r"));
    CHECK_FALSE(tui::render_csv("tek sütun\nbaşka satır\n", 40, tui::terminal_cell_width));
    CHECK_FALSE(tui::render_csv("", 40, tui::terminal_cell_width));

    const auto escape = tui::render_csv("a,b\nx\x1b[31m,y\n", 40, tui::terminal_cell_width);
    REQUIRE(escape);
    CHECK_FALSE(contains(joined(*escape), "\x1b"));

    std::string big = "a,b\n";
    for (int i = 0; i < 100000; ++i)
        big += "1,2\n";
    tui::ViewLimits limits;
    limits.table_rows = kTableRowsCap;
    const long ms = elapsed_ms([&] {
        const auto capped = tui::render_csv(big, 40, tui::terminal_cell_width, limits);
        REQUIRE(capped);
        CHECK(capped->lines.size() <= kTableRowsCap + 3);
    });
    if (noteapp::test::kWallClockBudgets)
        CHECK(ms < kHostileBudgetMs);
}

TEST_CASE("valid json is pretty printed with a source line per value") {
    const auto view = tui::render_json("{\"ad\":\"Çağrı\",\n\"liste\":[1,2,{}],\"boş\":[], \"n\":null}", 40,
                                       tui::terminal_cell_width);
    REQUIRE(view);
    const std::vector<std::string> expected = {
        "{",    "  \"ad\": \"Çağrı\",", "  \"liste\": [", "    1,", "    2,", "    {}",
        "  ],", "  \"boş\": [],",       "  \"n\": null",  "}",
    };
    CHECK(view->lines == expected);
    CHECK(view->source_lines[line_with(*view, "liste")] == 1);
    CHECK(look_of(*view, "\"ad\"").token == TokenKind::Key);
    check_parallel(*view);
}

TEST_CASE("invalid json falls back") {
    for (const std::string_view bad :
         {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "{1:2}", "[1 2]", "tru", "01", "1.", "-", "\"a",
          "\"\\q\"", "\"\\u12\"", "[1]x", "{\"a\":1,}", "\"a\nb\""}) {
        CAPTURE(bad);
        CHECK_FALSE(tui::render_json(bad, 40, tui::terminal_cell_width));
    }
    for (const std::string_view good :
         {"1", "-1.5e+3", "\"\\u00e7\\n\"", "true", " [ ] ", "\xEF\xBB\xBF{}"}) {
        CAPTURE(good);
        CHECK(tui::render_json(good, 40, tui::terminal_cell_width));
    }
}

TEST_CASE("deep json renders above the old depth cap without blowing the stack and long values wrap") {
    CHECK_FALSE(tui::render_json(repeated("[", 1000000), 40, tui::terminal_cell_width));
    // Eski sınır 128 iç içelikti; yazıcı özyinelemesiz olunca sınır yok.
    CHECK(tui::render_json(repeated("[", 10000) + repeated("]", 10000), 40, tui::terminal_cell_width));
    const auto wrapped =
        tui::render_json("{\"a\":\"" + repeated("uzun ", 100) + "\"}", 30, tui::terminal_cell_width);
    REQUIRE(wrapped);
    check_fits(*wrapped, 30);
    const auto sanitized = tui::render_json("\"\xe2\x80\xae x\"", 40, tui::terminal_cell_width);
    REQUIRE(sanitized);
    CHECK_FALSE(contains(joined(*sanitized), "\xe2\x80\xae"));
}

TEST_CASE("view kind comes from the extension") {
    CHECK(tui::view_kind_for("md") == tui::ViewKind::Markdown);
    CHECK(tui::view_kind_for("MARKDOWN") == tui::ViewKind::Markdown);
    CHECK(tui::view_kind_for("csv") == tui::ViewKind::Csv);
    CHECK(tui::view_kind_for("json") == tui::ViewKind::Json);
    CHECK(tui::view_kind_for("txt") == tui::ViewKind::None);
    CHECK(tui::view_kind_for("") == tui::ViewKind::None);
}

// Liste + REVIEW: gerçek model ve önbellek; main.cpp'nin sync_preview'ı gibi çizim öncesi eşitler.
namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

int fixture_cell_width(char32_t) {
    return 1;
}

struct ReviewFixture {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    std::optional<std::size_t> editor_returned;
    tui::EditorSession editor{tui::EditorHost{
        *store,
        mutex,
        internal,
        fixture_cell_width,
        1024 * 1024,
        core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("txt"); },
        [](const core::NotePath&) {},
        [] {},
        [](std::optional<std::size_t>) {},
        [](const core::Error&) {},
        [] {},
    }};
    tui::ListActions actions{
        tui::ListHost{store, mutex, internal, system, state, preview, status, [] {}, [] {}}};
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    const tui::Preview* shown = nullptr;

    ReviewFixture() { core::init_i18n("en"); }

    void put(const std::string& name, std::string_view bytes) const {
        test::upsert_note(dir.path(), name, bytes);
    }
    void open(const std::string& name, std::string_view bytes) {
        put(name, bytes);
        state.set_window(kWidth, kHeight);
        state.set_notes(store->list());
        sync();
    }
    void sync() {
        now += std::chrono::seconds(1);
        shown = &preview.throttled(state, *store, now);
        if (!preview.deferred())
            state.set_preview_lines(&shown->lines, preview.generation(), shown->partial);
    }
    void press(Key key, std::string_view text = {}) {
        const tui::Intent intent = state.handle(key, text);
        if (intent == tui::Intent::CopyNote) {
            std::optional<tui::ConfirmState> confirm;
            actions.run(intent, confirm);
        }
        sync();
    }
    void letter(std::string_view text) { press(Key::Char, text); }
    void find(std::string_view query) {
        letter("/");
        for (const char c : query)
            press(Key::Char, std::string_view(&c, 1));
        press(Key::Enter);
    }
    int right_x() const { return state.layout().geometry(kWidth, true).right_x + 2; }  // kenar, iç boşluk
    static constexpr int kTopRow = 2;
    void mouse(int x, int y, tui::MouseButton button, tui::MouseMotion motion) {
        actions.on_mouse({x, y, button, motion}, editor);
        sync();
    }
    void drag(int x0, int y0, int x1, int y1) {
        mouse(x0, y0, tui::MouseButton::Left, tui::MouseMotion::Press);
        mouse(x1, y1, tui::MouseButton::Left, tui::MouseMotion::Move);
        mouse(x1, y1, tui::MouseButton::Left, tui::MouseMotion::Release);
    }
    std::string screen_text() const {
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(state, *shown, status, {kWidth, kHeight}));
        return screen.ToString();
    }
};

const std::string_view kNote = "# Başlık\n\n**kalın** yazı\n\n- bir\n- iki\n\nson satır\n";

}  // namespace

TEST_CASE("markdown notes open rendered and R toggles to raw and back, in the label too") {
    ReviewFixture f;
    f.open("a.md", kNote);
    CHECK(f.shown->lines[2] == "kalın yazı");
    f.state.focus_preview();
    CHECK(contains(f.screen_text(), "REVIEW · rendered"));
    f.letter("R");
    CHECK(f.state.focus() == Focus::Preview);
    CHECK(f.shown->lines[2] == "**kalın** yazı");
    CHECK(contains(f.screen_text(), "REVIEW · raw"));
    f.letter("R");
    CHECK(f.shown->lines[2] == "kalın yazı");
}

TEST_CASE("the turkish label reads görünüm and ham") {
    ReviewFixture f;
    const TurkishCatalog turkish;
    f.open("a.md", kNote);
    f.state.focus_preview();
    CHECK(contains(f.screen_text(), " · görünüm"));
    f.letter("R");
    CHECK(contains(f.screen_text(), " · ham"));
}

TEST_CASE("notes without a rendered view keep the plain label and R changes nothing") {
    ReviewFixture f;
    f.open("a.txt", "**düz**\n");
    f.state.focus_preview();
    f.letter("R");
    CHECK(f.shown->lines[0] == "**düz**");
    const std::string screen = f.screen_text();
    CHECK(contains(screen, "REVIEW"));
    CHECK_FALSE(contains(screen, "· raw"));
    CHECK_FALSE(contains(screen, "· rendered"));
}

TEST_CASE("the rendered choice lasts for the session across notes") {
    ReviewFixture f;
    f.put("b.md", "**iki**\n");
    f.open("a.md", "**bir**\n");
    f.state.focus_preview();
    f.letter("R");
    CHECK(contains(f.shown->lines[0], "**"));
    f.press(Key::Left);
    f.press(Key::Down);
    CHECK(contains(f.shown->lines[0], "**"));
    f.letter("l");
    f.letter("R");
    CHECK_FALSE(contains(f.shown->lines[0], "**"));
}

TEST_CASE("editor keys never reach the review toggle") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    f.state.enter_editor();
    CHECK(f.state.handle(Key::Char, "R") == tui::Intent::EditorKey);
    CHECK(f.state.review_rendered());
}

TEST_CASE("search works on the displayed text and follows the toggle") {
    ReviewFixture f;
    f.open("a.md", "**Şalter** burada\n\n*şalter* orada\n");
    f.state.focus_preview();
    f.find("salter");
    CHECK(f.state.preview_find().count() == 2);
    CHECK(f.state.preview_find().ranges_on_line(0).front().begin == 0);
    f.press(Key::Escape);
    f.find("**");
    CHECK(f.state.preview_find().count() == 0);
    f.press(Key::Escape);
    f.letter("R");
    f.find("**");
    CHECK(f.state.preview_find().count() == 2);
}

TEST_CASE("search highlight is painted on the rendered line") {
    ReviewFixture f;
    f.open("a.md", "**kalın** yazı\n");
    f.state.focus_preview();
    f.find("yazı");
    const std::string screen = f.screen_text();
    CHECK(contains(screen, "kalın"));
    CHECK(contains(screen, "yazı"));
}

TEST_CASE("mouse selection copies the displayed text and c copies the original bytes") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    const int x = f.right_x();
    f.drag(x, ReviewFixture::kTopRow + 2, x + 4, ReviewFixture::kTopRow + 2);
    CHECK(f.internal == "kalın");
    f.letter("c");
    CHECK(f.internal == kNote);
    f.letter("R");
    f.drag(x, ReviewFixture::kTopRow + 2, x + 4, ReviewFixture::kTopRow + 2);
    CHECK(f.internal == "**kal");
}

TEST_CASE("a multi-line rendered selection copies displayed lines joined by newline") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    const int x = f.right_x();
    f.drag(x, ReviewFixture::kTopRow + 4, x + 4, ReviewFixture::kTopRow + 5);
    CHECK(f.internal == "• bir\n• iki");
}

TEST_CASE("invalid utf8 note renders and its displayed text can be selected") {
    ReviewFixture f;
    f.open("a.md", "**a\xff"
                   "b**\n");
    f.state.focus_preview();
    CHECK(f.shown->lines[0] == "a\xEF\xBF\xBD"
                               "b");
    const int x = f.right_x();
    f.drag(x, ReviewFixture::kTopRow, x + 2, ReviewFixture::kTopRow);
    CHECK(f.internal == "a\xEF\xBF\xBD"
                        "b");
}

TEST_CASE("e opens the source line of the top visible block") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    CHECK(f.state.editor_start_line() == 0);
    f.letter("j");  // boş satır: yukarıdaki bloğa değil, sıradaki bloğa ait
    f.letter("j");
    CHECK(f.shown->lines[f.state.preview_scroll()] == "kalın yazı");
    CHECK(f.state.editor_start_line() == 2);
    f.letter("j");
    f.letter("j");
    CHECK(f.shown->lines[f.state.preview_scroll()] == "• bir");
    CHECK(f.state.editor_start_line() == 4);
    f.letter("j");
    CHECK(f.state.editor_start_line() == 5);
    f.letter("R");  // ham görünümde kaydırma zaten kaynak satırıdır
    CHECK(f.state.preview_scroll() == 5);
    CHECK(f.state.editor_start_line() == 5);
    f.letter("R");
    CHECK(f.shown->lines[f.state.preview_scroll()] == "• iki");
}

TEST_CASE("returning from the editor keeps the block in view") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    f.state.enter_editor();
    f.state.leave_editor(5);
    f.sync();
    CHECK(f.shown->lines[f.state.preview_scroll()] == "• iki");
    f.state.enter_editor();
    f.state.leave_editor(3);  // bloklar arası boş satır: üstündeki blok görünür kalır
    f.sync();
    CHECK(f.shown->lines[f.state.preview_scroll() + 1] == "kalın yazı");
    CHECK(f.state.editor_start_line() == 2);
}

TEST_CASE("a burst of width changes keeps the previous layout until it settles") {
    ReviewFixture f;
    f.open("a.md", repeated("sözcük ", 200));
    const auto at = [&](int ms) -> const tui::Preview& {
        f.now += std::chrono::milliseconds(ms);
        return f.preview.throttled(f.state, *f.store, f.now);
    };
    const auto columns = [&] {
        return tui::editor_viewport({f.state.window_width(), kHeight}, f.state.layout()).columns;
    };
    constexpr int kSettle = tui::PreviewCache::kResizeSettleMs;

    const int before = columns();
    f.state.set_window(kWidth - 10, kHeight);
    REQUIRE(columns() != before);
    at(kSettle * 10);  // sakin kullanıcı: tek değişim beklemez
    CHECK_FALSE(f.preview.deferred());
    const std::size_t settled = f.preview.generation();
    const std::vector<std::string> layout = f.preview.shown().lines;

    f.state.set_window(kWidth - 20, kHeight);
    CHECK(at(1).lines == layout);
    CHECK(f.preview.deferred());
    f.state.set_window(kWidth - 30, kHeight);
    CHECK(at(kSettle - 1).lines == layout);
    CHECK(f.preview.deferred());
    f.state.set_window(kWidth - 40, kHeight);
    CHECK(at(kSettle - 1).lines == layout);
    CHECK(f.preview.deferred());
    CHECK(f.preview.generation() == settled);

    const tui::Preview& rebuilt = at(kSettle);
    CHECK_FALSE(f.preview.deferred());
    CHECK(f.preview.generation() != settled);
    check_fits(RenderedView{rebuilt.lines, rebuilt.styled, rebuilt.source_lines}, columns());

    // Bekleme sürerken eski genişliğe dönüş yeniden üretmez, sonraki değişim de sakin sayılır.
    const std::size_t again = f.preview.generation();
    const int settled_width = f.state.window_width();
    f.state.set_window(settled_width + 5, kHeight);
    at(kSettle * 10);
    const std::size_t after_single = f.preview.generation();
    CHECK(after_single != again);
    f.state.set_window(settled_width, kHeight);
    at(1);
    CHECK(f.preview.deferred());
    f.state.set_window(settled_width + 5, kHeight);
    at(1);
    CHECK_FALSE(f.preview.deferred());
    CHECK(f.preview.generation() == after_single);
}

TEST_CASE("resizing a raw view is never deferred") {
    ReviewFixture f;
    f.open("a.md", kNote);
    f.state.focus_preview();
    f.letter("R");
    const std::size_t generation = f.preview.generation();
    for (int i = 0; i < 3; ++i) {
        f.state.set_window(kWidth - 10 * i, kHeight);
        f.now += std::chrono::milliseconds(1);
        f.preview.throttled(f.state, *f.store, f.now);
        CHECK_FALSE(f.preview.deferred());
    }
    CHECK(f.preview.generation() == generation);
}

TEST_CASE("the rendered view follows the pane width and is cached otherwise") {
    ReviewFixture f;
    f.open("a.md", repeated("sözcük ", 200));
    f.state.focus_preview();
    const std::size_t lines = f.shown->lines.size();
    const std::size_t generation = f.preview.generation();
    const std::vector<std::string>* first = &f.shown->lines;
    for (int i = 0; i < 3; ++i) {
        f.sync();
        CHECK(&f.shown->lines == first);
        CHECK(f.preview.generation() == generation);
    }
    f.letter("<");
    CHECK(f.preview.generation() != generation);
    CHECK(f.shown->lines.size() != lines);
    const int columns = tui::editor_viewport({kWidth, kHeight}, f.state.layout()).columns;
    check_fits(RenderedView{f.shown->lines, f.shown->styled, f.shown->source_lines}, columns);
}

TEST_CASE("a moving selection does not read or render until it settles") {
    ReviewFixture f;
    for (const char* name : {"a.md", "b.md", "c.md"})
        f.put(name, name);
    f.state.set_window(kWidth, kHeight);
    f.state.set_notes(f.store->list());
    std::chrono::steady_clock::time_point wall{std::chrono::hours(2)};
    f.preview.throttled(f.state, *f.store, wall);
    wall += std::chrono::seconds(1);
    f.state.handle(Key::Down);
    f.preview.throttled(f.state, *f.store, wall);
    const std::size_t settled = f.preview.generation();
    const std::string shown = f.preview.throttled(f.state, *f.store, wall).lines.front();
    wall += std::chrono::milliseconds(10);
    f.state.handle(Key::Down);
    const tui::Preview& held = f.preview.throttled(f.state, *f.store, wall);
    CHECK(f.preview.deferred());
    CHECK(f.preview.generation() == settled);
    CHECK(held.lines.front() == shown);
    wall += std::chrono::milliseconds(tui::PreviewCache::kCoalesceMs);
    CHECK(f.preview.throttled(f.state, *f.store, wall).lines.front() ==
          f.state.selected_row()->path.string());
}

TEST_CASE("an edited note is rendered again") {
    ReviewFixture f;
    f.open("a.md", "**bir**\n");
    f.put("a.md", "**iki**\n");
    f.preview.invalidate();
    f.sync();
    CHECK(f.shown->lines[0] == "iki");
}

TEST_CASE("csv and json notes render in review") {
    ReviewFixture f;
    f.open("a.csv", "ad,yaş\nAli,4\n");
    CHECK(contains(f.shown->lines[0], "│ ad"));
    f.state.focus_preview();
    CHECK(contains(f.screen_text(), "· rendered"));
    ReviewFixture g;
    g.open("a.json", "{\"a\":1}");
    CHECK(g.shown->lines[1] == "  \"a\": 1");
}

TEST_CASE("invalid json and non-table csv stay raw highlighted with a dim note") {
    ReviewFixture f;
    f.open("a.json", "{bozuk\n");
    CHECK(f.shown->lines[0] == "{bozuk");
    CHECK(f.shown->language == tui::SyntaxLanguage::Json);
    CHECK_FALSE(f.shown->rendered);
    CHECK(f.shown->note == core::tr("tui.view_note_invalid_json"));
    f.state.focus_preview();
    CHECK(contains(f.screen_text(), core::tr("tui.view_note_invalid_json")));
    CHECK(contains(f.screen_text(), "· raw"));
    f.letter("R");  // ham istendi: artık bildirilecek bir geri düşüş yok
    CHECK(f.shown->note.empty());
    CHECK_FALSE(contains(f.screen_text(), core::tr("tui.view_note_invalid_json")));

    ReviewFixture g;
    g.open("a.csv", "tek\nsütun\n");
    CHECK(g.shown->lines[0] == "tek");
    CHECK(g.shown->note == core::tr("tui.view_note_not_table"));
}

constexpr std::size_t kOldViewBytes = std::size_t{256} * 1024;  // NY-P öncesi limits.tui.view_bytes varsayılanı

TEST_CASE("notes over a configured view limit stay raw with a note") {
    const tui::ViewLimits capped =
        tui::view_limits_of(core::parse_config("[limits.tui]\nview_bytes = \"256KiB\"\n").config.limits.tui);
    ReviewFixture f;
    f.preview = tui::PreviewCache(capped, {});
    f.open("a.json", "[" + repeated("1,", kOldViewBytes / 2) + "1]");
    CHECK_FALSE(f.shown->rendered);
    CHECK(contains(f.shown->note, "KiB"));
    ReviewFixture g;
    g.preview = tui::PreviewCache(capped, {});
    g.open("a.md", repeated("# a\n", kOldViewBytes / 4 + 1));
    CHECK_FALSE(g.shown->rendered);
    CHECK(g.shown->lines[0] == "# a");
}

// NY-P: varsayılan sınırsızdır; eski 256 KiB sınırın üstündeki md, json ve csv biçimli açılır.
TEST_CASE("notes above the old 256 KiB view limit render with the default limits") {
    ReviewFixture f;
    f.preview = tui::PreviewCache(tui::view_limits_of(core::parse_config("").config.limits.tui), {});
    f.open("a.md", repeated("# a\n", kOldViewBytes / 4 + 1));
    CHECK(f.shown->rendered);
    CHECK(f.shown->note.empty());
    ReviewFixture j;
    j.preview = tui::PreviewCache(tui::view_limits_of(core::parse_config("").config.limits.tui), {});
    j.open("a.json", "[" + repeated("1,", kOldViewBytes / 2) + "1]");
    CHECK(j.shown->rendered);
    ReviewFixture c;
    c.preview = tui::PreviewCache(tui::view_limits_of(core::parse_config("").config.limits.tui), {});
    c.open("a.csv", "a,b\n" + repeated("1,2\n", kOldViewBytes / 4 + 1));
    CHECK(c.shown->rendered);
}

namespace {

// Parçalara bölünen büyük not: başlık, sarılan paragraf, wiki bağlantısı, liste, kuşatılmış kod.
std::string large_markdown(std::size_t bytes) {
    const std::string_view unit = "## Bölüm\n\nBir paragraf *vurgu* ve **kalın** metin [[hedef]] ile; satır sarılsın "
                                  "diye biraz uzun yazılmış bir cümle daha ekleniyor burada.\n\n"
                                  "- madde bir\n- madde iki\n\n```\nkod satırı\n```\n\n";
    return repeated(unit, bytes / unit.size() + 1);
}

tui::ViewLimits unlimited_view() {
    return tui::view_limits_of(
        core::parse_config("[limits.tui]\nview_bytes = 0\nview_lines = 0\n").config.limits.tui);
}

void check_same_view(const tui::Preview& shown, const tui::RenderedView& whole) {
    CHECK(shown.lines == whole.lines);
    CHECK(shown.source_lines == whole.source_lines);
    REQUIRE(shown.styled.size() == whole.spans.size());
    for (std::size_t line = 0; line < whole.spans.size(); ++line) {
        REQUIRE(shown.styled[line].size() == whole.spans[line].size());
        for (std::size_t i = 0; i < whole.spans[line].size(); ++i) {
            CHECK(shown.styled[line][i].begin == whole.spans[line][i].begin);
            CHECK(shown.styled[line][i].end == whole.spans[line][i].end);
            CHECK(shown.styled[line][i].look == whole.spans[line][i].look);
        }
    }
}


// Kesim yerlerinin her türü: setext başlık, sıralı liste ve öğedeki girintili çit (imden sonra sekmeli de), alıntı,
// tablo, görev listesi, yıldızlı çizgi, başvuru bağlantısı (tanımı sonda; etiketi satır sonlu, `\]` kaçışlı ya da
// Unicode büyük/küçük harf farklı, ß = ss), sonda kapanmamış çit. `crlf`: satır sonları CRLF.
std::string varied_markdown(std::size_t bytes, bool crlf) {
    const std::string unit = "Başlık\n======\n\n1. bir\n2. iki\n   ```\n   kod\n   ```\n\n> alıntı\n> devam\n\n"
                             "| a | b |\n|---|---|\n| 1 | 2 |\n\nAlt başlık\n---\n\n[bağ][r] ve *eğik*\n\n"
                             "- [x] görev\n- [ ] kalan\n\n***\n\n- ```sh\n  echo\n  ```\n\n-\t```\n\tsekme\n```\n\n"
                             "[çok satırlı][my\nlink] ve [kaçış][a\\]b] ve [dış][STRASSE] ve [Çağ][]\n\n";
    std::string text;
    while (text.size() < bytes)
        text += unit;
    text += "[r]: https://r.example\n[my link]: https://ml.example\n[a\\]b]: https://esc.example\n"
            "[straße]: https://ss.example\n[ÇAĞ]: https://cag.example\n\n```\nkapanmamış\n";
    if (!crlf)
        return text;
    std::string out;
    for (const char c : text) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

}  // namespace

// NX-D: büyük md parça parça üretilir (yalnız görünen pencere); `G` tümünü üretir ve sonuç tek geçişle aynıdır.
TEST_CASE("a large markdown note renders chunk by chunk and ends identical to the whole rendering") {
    const std::string note = large_markdown(3 * noteapp::app::kMarkdownChunkBytes);
    ReviewFixture f;
    f.preview = tui::PreviewCache(unlimited_view(), {});
    f.open("big.md", note);
    REQUIRE(f.shown->rendered);
    CHECK(f.shown->partial);
    const int width = tui::editor_viewport({kWidth, kHeight}, f.state.layout()).columns;
    const tui::RenderedView whole = tui::render_markdown(note, width, tui::terminal_cell_width, unlimited_view());
    CHECK(f.shown->lines.size() < whole.lines.size() / 2);
    f.state.focus_preview();
    f.letter("G");
    CHECK_FALSE(f.shown->partial);
    check_same_view(*f.shown, whole);
    CHECK(f.state.preview_scroll() == whole.lines.size() - 1);
}

// Her blok türü, LF ve CRLF: tümü üretilince tek geçişle aynı (başvuru tanımı sonda, sonda kapanmamış çit).
TEST_CASE("a chunked note of every block kind renders like the whole note, LF and CRLF") {
    for (const bool crlf : {false, true}) {
        CAPTURE(crlf);
        const std::string note = varied_markdown(3 * noteapp::app::kMarkdownChunkBytes, crlf);
        ReviewFixture f;
        f.preview = tui::PreviewCache(unlimited_view(), {});
        f.open("big.md", note);
        REQUIRE(f.shown->partial);
        f.state.focus_preview();
        f.letter("G");
        REQUIRE_FALSE(f.shown->partial);
        const int width = tui::editor_viewport({kWidth, kHeight}, f.state.layout()).columns;
        check_same_view(*f.shown, tui::render_markdown(note, width, tui::terminal_cell_width, unlimited_view()));
    }
}

// Not içi arama tüm görünümde çalışır; satır sınırı (view_lines) parçalar arasında sürer.
TEST_CASE("find over a chunked markdown note sees every chunk and the line limit spans chunks") {
    const std::string note = large_markdown(3 * noteapp::app::kMarkdownChunkBytes) + "## sonuncu\n";
    ReviewFixture f;
    f.preview = tui::PreviewCache(unlimited_view(), {});
    f.open("big.md", note);
    REQUIRE(f.shown->partial);
    f.state.focus_preview();
    f.find("sonuncu");
    CHECK_FALSE(f.shown->partial);
    CHECK(f.state.preview_find().count() == 1);
    REQUIRE(f.state.preview_find().current_line());
    CHECK(f.shown->lines[*f.state.preview_find().current_line()] == "## sonuncu");

    tui::ViewLimits limited = unlimited_view();
    limited.lines = 5000;
    ReviewFixture g;
    g.preview = tui::PreviewCache(limited, {});
    g.open("big.md", note);
    g.state.focus_preview();
    g.letter("G");
    const int width = tui::editor_viewport({kWidth, kHeight}, g.state.layout()).columns;
    check_same_view(*g.shown, tui::render_markdown(note, width, tui::terminal_cell_width, limited));
}

// NX-E F1: aynı etiketin iki tanımında belge sırasıyla ilki kazanır, sonraki parçadaki tekrar da.
// NX-F: Unicode büyük/küçük harf farkıyla yazılmış tekrar da (ÇAĞ = çağ, md4c katlaması).
TEST_CASE("the first definition of a label wins across chunks") {
    std::string note = "[a]: https://first.example\n\n[ÇAĞ]: https://first-cag.example\n\n";
    // Tekrar ve kullanımı aynı parçanın ortasında: parçanın kendi tanımı eklenen tablodan önce gelir.
    while (note.size() < 100 * 1024)
        note += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    note += "[a]: https://second.example\n\n[son][a]\n\n[çağ]: https://second-cag.example\n\n[son][Çağ]\n\n";
    while (note.size() < 200 * 1024)
        note += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    ReviewFixture f;
    f.preview = tui::PreviewCache(unlimited_view(), {});
    f.open("big.md", note);
    REQUIRE(f.shown->partial);
    f.state.focus_preview();
    f.letter("G");
    REQUIRE_FALSE(f.shown->partial);
    CHECK(contains(f.shown->text, "first.example"));
    CHECK_FALSE(contains(f.shown->text, "second.example"));
    CHECK(contains(f.shown->text, "first-cag.example"));
    CHECK_FALSE(contains(f.shown->text, "second-cag.example"));
}

// Bağlantı başvuru tanımı notun her yerinde geçerlidir: sondaki tanım ilk parçadaki kullanımı çözer (metin + URL).
TEST_CASE("a reference defined at the end of a chunked note resolves in the first chunk") {
    std::string note = "[başvuru][r] ile başlar.\n\n";
    while (note.size() < 100 * 1024)
        note += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    note += "[r]: https://example.org/r\n";
    ReviewFixture f;
    f.preview = tui::PreviewCache(unlimited_view(), {});
    f.open("big.md", note);
    REQUIRE(f.shown->partial);
    CHECK(contains(f.shown->lines.front(), "example.org/r"));
}

// EDIT'ten dönüş ve içerik araması üretilmemiş parçadaki kaynak satırına iner.
TEST_CASE("scrolling to a source line in an unrendered chunk renders up to it") {
    const std::string note = large_markdown(3 * noteapp::app::kMarkdownChunkBytes);
    ReviewFixture f;
    f.preview = tui::PreviewCache(unlimited_view(), {});
    f.open("big.md", note);
    REQUIRE(f.shown->partial);
    const std::size_t total_lines = static_cast<std::size_t>(std::ranges::count(note, '\n'));
    const std::size_t target = total_lines - 3;  // son birimin kod bloğu
    f.state.leave_editor(target);
    f.sync();
    REQUIRE(f.state.preview_scroll() < f.shown->source_lines.size());
    CHECK(f.shown->source_lines[f.state.preview_scroll()] <= target);
    CHECK(f.shown->source_lines[f.state.preview_scroll()] + 3 >= target);
}

// NX-D: ham REVIEW uzun satırın ilk ekranından sonrasını yana kaydırarak gösterir (GUI ham önizlemesi tüm satırı
// gösterir). Adım panelin genişliği kadar bayttır; görünen en uzun satırın sonunu geçmez.
TEST_CASE("Shift+Right scrolls a long raw REVIEW line sideways a pane width at a time, Shift+Left back") {
    ReviewFixture f;
    std::string line;
    for (int i = 0; line.size() < 6000; ++i)
        line += std::to_string(i % 10);
    line += "SON";
    f.open("uzun.txt", line + "\nkısa\n");
    f.state.focus_preview();
    const auto step =
        static_cast<std::size_t>(f.state.layout().geometry(kWidth, true).right_width - tui::kPaneChromeCells);
    CHECK(f.state.preview_column() == 0);
    CHECK_FALSE(contains(f.screen_text(), "SON"));
    f.press(Key::ShiftRight);
    CHECK(f.state.preview_column() == step);
    // Fare seçimi kaydırılmış satırın gösterilen baytlarını kopyalar.
    const int x = f.right_x();
    f.drag(x, ReviewFixture::kTopRow, x + 3, ReviewFixture::kTopRow);
    CHECK(f.internal == line.substr(step, 4));
    for (int i = 0; i < 300 && !contains(f.screen_text(), "SON"); ++i)
        f.press(Key::ShiftRight);
    CHECK(contains(f.screen_text(), "SON"));
    for (int i = 0; i < 300; ++i)
        f.press(Key::ShiftRight);
    CHECK(f.state.preview_column() < line.size());
    CHECK(contains(f.screen_text(), "N"));
    for (int i = 0; i < 300; ++i)
        f.press(Key::ShiftLeft);
    CHECK(f.state.preview_column() == 0);
    CHECK_FALSE(contains(f.screen_text(), "SON"));
}

// Biçimli görünümde yana kaydırma yok: Shift+tekerlek eskisi gibi dikey kaydırır.
TEST_CASE("Shift+wheel scrolls the rendered REVIEW vertically") {
    ReviewFixture f;
    f.open("a.md", repeated("# Başlık\n\nparagraf\n\n", 200));
    f.state.focus_preview();
    REQUIRE(f.shown->rendered);
    f.actions.on_mouse({f.right_x(), f.kTopRow + 1, tui::MouseButton::WheelDown, tui::MouseMotion::Press, true},
                       f.editor);
    f.sync();
    CHECK(f.state.preview_scroll() > 0);
    CHECK(f.state.preview_column() == 0);
}

// Yana kaydırma yalnız ham görünümündür (biçimli satırlar panele sığar); başka not baştan başlar. Shift+tekerlek
// ve yatay tekerlek (terminal bildirirse) aynı adımı atar.
TEST_CASE("sideways scroll is raw only, resets on another note and follows Shift+wheel") {
    ReviewFixture f;
    f.put("b.txt", "b\n");
    f.open("a.md", "# Başlık\n\n" + std::string(6000, 'a') + "\n");
    f.state.focus_preview();
    f.press(Key::ShiftRight);
    CHECK(f.state.preview_column() == 0);  // biçimli görünüm
    f.letter("R");
    f.press(Key::ShiftRight);
    const std::size_t step = f.state.preview_column();
    CHECK(step > 0);
    const int x = f.right_x();
    f.actions.on_mouse({x, f.kTopRow + 1, tui::MouseButton::WheelDown, tui::MouseMotion::Press, true}, f.editor);
    f.sync();
    CHECK(f.state.preview_column() == 2 * step);
    f.actions.on_mouse({x, f.kTopRow + 1, tui::MouseButton::WheelLeft, tui::MouseMotion::Press}, f.editor);
    f.sync();
    CHECK(f.state.preview_column() == step);
    REQUIRE(f.state.select_note(core::NotePath::parse("b.txt").value()));
    f.sync();
    CHECK(f.state.preview_column() == 0);
}

TEST_CASE("a sample rendering is written when asked") {
    const char* directory = std::getenv("NOTEAPP_T69_SAMPLE_DIR");
    if (directory == nullptr)
        return;
    const std::string sample =
        "# noteapp görünümü\n\nDüz metin, **kalın**, *eğik*, ~~silik~~ ve `satır içi kod`. "
        "[Bağlantı](https://example.org/yol) ve ![kedi](k.png). <b>html</b> olduğu gibi kalır.\n\n"
        "## Liste\n\n- bir\n  - iç içe\n    1. numaralı\n- [x] bitti\n- [ ] kaldı\n\n"
        "> Alıntı satırı, Türkçe ığüşöç İI harfleriyle epey uzun bir cümle olarak panele sarılır.\n\n"
        "### Kod\n\n```python\ndef selam(ad):\n    return f\"Merhaba {ad}\"  # yorum\n```\n\n---\n\n"
        "| Ad | Açıklama | Yaş |\n|---|---|---|\n| Çağrı | şu uzun açıklama hücresi sütuna sarılır, taşmaz | "
        "30 |\n"
        "| Ayşe | kısa | 4 |\n";
    ReviewFixture f;
    {
        const TurkishCatalog turkish;
        f.open("ornek.md", sample);
        f.state.focus_preview();
        f.sync();
        std::ofstream(std::string(directory) + "/rendered-md.ansi") << f.screen_text() << '\n';
        std::ofstream text(std::string(directory) + "/rendered-md.txt");
        for (const std::string& line : f.shown->lines)
            text << line << '\n';
        // Sınırın tam boyundaki girdiler: yüksüz en kötü süre, UI iş parçacığı bütçesiyle (50 ms)
        // karşılaştırılır.
        const auto filled = [](std::string_view unit) {
            return repeated(unit, tui::ViewLimits{}.source_bytes / unit.size());
        };
        const std::pair<const char*, std::string> inputs[] = {
            {"mixed", filled(sample)},
            {"prose", filled("Düz metin **kalın** ve `kod` ile sıradan bir paragraf satırı.\n\n")},
            {"one_paragraph", filled("sözcük ")},
            {"list", filled("- öğe bir\n  - iç öğe\n")},
            {"code", filled("```python\ndef f(a):\n    return a + 1  # yorum\n```\n\n")},
            {"table", "| a | b |\n|---|---|\n" + filled("| hücre | değer |\n")},
            {"links", filled("[metin](http://ornek.org/yol?a=1&amp;b=2) ")},
            {"hostile_brackets", filled("[")},
            {"hostile_emphasis", filled("*a ")},
            {"hostile_quotes", filled(">>>>>>>> x\n")},
            {"hostile_images", filled("![")},
            {"hostile_backticks", filled("`")},
            {"hostile_html", filled("<a ")},
            {"hostile_entities", filled("&amp")},
            {"hostile_strong", filled("**a*")},
        };
        std::ofstream timing(std::string(directory) + "/render-timing-ms.txt");
        timing << "limit_bytes " << tui::ViewLimits{}.source_bytes << '\n';
        const auto worst_of_three = [](const auto& work) {
            long worst = 0;
            for (int run = 0; run < 3; ++run)
                worst = std::max(worst, elapsed_ms(work));
            return worst;
        };
        for (const auto& [name, input] : inputs)
            timing << name << ' ' << worst_of_three([&] { md(input, kWideView); }) << '\n';
        const std::string csv = "a,b\n" + filled("hücre,değer\n");
        timing << "csv " << worst_of_three([&] { tui::render_csv(csv, kWideView, tui::terminal_cell_width); })
               << '\n';
        // Uçtan uca genişlik değişimi: yeniden sarma + görüntü metni + eşlemenin modele kopyası.
        ReviewFixture resized;
        resized.open("buyuk.md", inputs[0].second.substr(0, tui::ViewLimits{}.source_bytes));
        timing << "publish_mixed " << worst_of_three([&] {
            resized.state.set_window(resized.state.window_width() == kWidth ? kWidth - 10 : kWidth, kHeight);
            resized.now += std::chrono::seconds(1);
            resized.preview.throttled(resized.state, *resized.store, resized.now);
        }) << '\n';
        const std::string json = "[" + filled("{\"a\":1},") + "1]";
        timing << "json "
               << worst_of_three([&] { tui::render_json(json, kWideView, tui::terminal_cell_width); })
               << '\n';
    }
}
