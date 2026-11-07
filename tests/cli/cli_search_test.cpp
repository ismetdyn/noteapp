// note find (T16): içerik araması çıktısı `yol:satır: kesit`; boruda ANSI yok.
#include "cli/highlight.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::test;

namespace {}  // namespace

TEST_CASE("find lists path:line: snippet for every hit (SPEC-cli acceptance 6)") {
    const Sandbox box;
    box.put("plan.md", "proje başlangıcı\nbaşka satır\nbütçe onaylandı\n");
    box.put("sadece-proje.md", "proje var, gerisi yok\n");

    const auto result = box.run({"find", "proje bütçe"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "plan.md:1: proje başlangıcı\nplan.md:3: bütçe onaylandı\n");
}

TEST_CASE("find joins several arguments into one query") {
    const Sandbox box;
    box.put("plan.md", "proje başlangıcı\nbütçe onaylandı\n");
    box.put("other.md", "proje\n");

    const auto result = box.run({"find", "proje", "bütçe"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find("plan.md:1:") == 0);
    CHECK(result.out.find("other.md") == std::string::npos);
}

TEST_CASE("find folds Turkish letters") {
    const Sandbox box;
    box.put("k.md", "Öğrenci listesi\n");

    const auto result = box.run({"find", "ogrenci"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "k.md:1: Öğrenci listesi\n");
}

TEST_CASE("find prints no ANSI escape codes when stdout is not a terminal") {
    const Sandbox box;
    box.put("k.md", "merhaba dünya\n");

    const auto result = box.run({"find", "dünya"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find('\x1b') == std::string::npos);
}

TEST_CASE("find with no match exits 1 and prints nothing to stdout") {
    const Sandbox box;
    box.put("k.md", "merhaba\n");

    const auto result = box.run({"find", "yokboyle"});
    CHECK(result.exit_code == 1);
    CHECK(result.out.empty());
    CHECK(result.err.find("yokboyle") != std::string::npos);
}

TEST_CASE("find --limit caps the number of notes; usage errors exit 2") {
    const Sandbox box;
    box.put("a.md", "kelime\n");
    box.put("b.md", "kelime\n");

    const auto limited = box.run({"find", "kelime", "--limit", "1"});
    CHECK(limited.exit_code == 0);
    CHECK(limited.out.size() == std::string("a.md:1: kelime\n").size());

    CHECK(box.run({"find"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit", "x"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit", "0"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--zorla"}).exit_code == 2);
}

// İlişki araması (SPEC-cli): içerikte geçmeyen ama etiketi, ilişkisi ya da bağlı notunun adı eşleşen not
// içerik satırlarından sonra gelir; yalnız terminalde, -l ile ya da --json'da. Düz boru çıktısı eskisi gibi.
TEST_CASE("find also lists notes found through relations after the content lines") {
    const Sandbox box;
    box.put("budget.md", "rakamlar\n");
    box.put("plan.md", "budget kalemleri\n");
    box.put("team.md", "ekip\n");
    box.put("other.md", "yok\n");
    REQUIRE(box.run({"relation", "add", "team.md", "budget.md", "--name", "funds", "--directed"}).exit_code ==
            0);

    const auto plain = box.run({"find", "budget"});
    CHECK(plain.exit_code == 0);
    CHECK(plain.out == "plan.md:1: budget kalemleri\n");

    const auto long_form = box.run({"find", "budget", "-l"});
    CHECK(long_form.exit_code == 0);
    CHECK(long_form.out == "plan.md:1: budget kalemleri\nteam.md\tbağlı: budget.md (funds)\n");
    CHECK(box.run({"find", "budget", "--long"}).out == long_form.out);

    const auto json = box.run({"find", "budget", "--json"});
    CHECK(json.exit_code == 0);
    CHECK(json.out ==
          "[{\"path\":\"plan.md\",\"line\":1,\"snippet\":\"budget kalemleri\",\"ranges\":[[0,6]],"
          "\"reason\":\"content\"},{\"path\":\"team.md\",\"reason\":\"linked\",\"label\":\"funds\","
          "\"other\":\"budget.md\",\"hops\":1}]\n");

    // `ls` süzgeçleri: ilişki adı ve bağlı not.
    const auto by_relation = box.run({"ls", "rel:funds"});
    CHECK(by_relation.exit_code == 0);
    CHECK((by_relation.out == "team.md\nbudget.md\n" || by_relation.out == "budget.md\nteam.md\n"));
    CHECK(box.run({"ls", "linked:budget"}).out == "team.md\n");

    // find de sorgu argümanlarını alır (TUI/GUI içerik aramasıyla aynı hat); yalnız argüman metin değildir.
    CHECK(box.run({"find", "budget", "type:txt"}).exit_code == 1);
    CHECK(box.run({"find", "type:md"}).exit_code == 2);
}

// Eşitlik (NY-S): `ls` ad aramasında da ilişkiyle bulunan not ad eşleşmelerinin altında, TUI/GUI'deki nedeniyle
// gelir; düz boru çıktısı (yalın yollar) `find` gibi değişmez.
TEST_CASE("ls also lists notes found through relations under the name matches") {
    const Sandbox box;
    box.put("budget.md", "rakamlar\n");
    box.put("team.md", "ekip\n");
    box.put("other.md", "yok\n");
    REQUIRE(box.run({"relation", "add", "team.md", "budget.md", "--name", "funds", "--directed"}).exit_code ==
            0);

    CHECK(box.run({"ls", "budget"}).out == "budget.md\n");
    CHECK(box.run({"ls", "budget", "-0"}).out == std::string("budget.md\0", 10));

    const auto long_form = box.run({"ls", "budget", "-l"});
    CHECK(long_form.exit_code == 0);
    CHECK(long_form.out.find("  budget.md\n") != std::string::npos);
    CHECK(long_form.out.find("  team.md\tbağlı: budget.md (funds)\n") != std::string::npos);

    const auto json = box.run({"ls", "budget", "--json"});
    CHECK(json.exit_code == 0);
    CHECK(json.out.find("{\"path\":\"budget.md\",\"type\":\"md\"") != std::string::npos);
    CHECK(json.out.find(",\"reason\":\"linked\",\"label\":\"funds\",\"other\":\"budget.md\",\"hops\":1}") !=
          std::string::npos);
    CHECK(json.out.find("other.md") == std::string::npos);
    CHECK(box.run({"ls", "budget", "--json", "-n", "1"}).out.find("team.md") == std::string::npos);
    // -0 her zaman yalnız yol: -l ile de NUL akışına ilişki satırı girmez.
    CHECK(box.run({"ls", "budget", "-l", "-0"}).out.find("team.md") == std::string::npos);
}

// --limit süzgeçten sonra keser: en üst içerik isabeti süzülen tipteyse kalan eşleşme kaybolmaz.
TEST_CASE("find applies --limit after the search arguments filter") {
    const Sandbox box;
    // İki isabet satırı: içerik sırasında txt'den önce gelir (isabet satırı sayısı azalan).
    box.put("a1.md", "kelime\nkelime\n");
    box.put("a2.md", "kelime\nkelime\n");
    box.put("a3.md", "kelime\nkelime\n");
    box.put("z.txt", "kelime\n");
    const auto result = box.run({"find", "kelime", "type:txt", "-n", "1"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "z.txt:1: kelime\n");
}

// Bilinmeyen anahtar (`a:b`, `http://x`) düz metindir; bozuk değer argümanın yeriyle bildirilir (TUI gibi).
TEST_CASE("unknown keys stay text and a bad argument names its column") {
    const Sandbox box("en");
    box.put("link.md", "see http://x and a:b\n");
    CHECK(box.run({"find", "a:b"}).out == "link.md:1: see http://x and a:b\n");
    CHECK(box.run({"find", "http://x"}).out == "link.md:1: see http://x and a:b\n");

    const auto bad = box.run({"ls", "plan sort:bogus"});
    CHECK(bad.exit_code == 2);
    CHECK(bad.err.find("Column 11:") != std::string::npos);
    CHECK(box.run({"find", "plan sort:bogus"}).err.find("Column 11:") != std::string::npos);
}

TEST_CASE("find succeeds on relation hits alone; tags and relation names carry their own fields") {
    const Sandbox box;
    box.put("a.md", "#Proje notu\n");
    box.put("b.md", "b\n");
    box.put("c.md", "c\n");
    REQUIRE(box.run({"relation", "add", "b.md", "c.md", "--name", "uses", "--directed"}).exit_code == 0);

    // Düz boruda çıkış kodu içeriğe bağlı kalır: yalnız ilişkiyle eşleşme 1.
    const auto piped = box.run({"find", "uses"});
    CHECK(piped.exit_code == 1);
    CHECK(piped.out.empty());
    CHECK(box.run({"find", "uses", "-l"}).exit_code == 0);

    const auto named = box.run({"find", "uses", "--json"});
    CHECK(named.exit_code == 0);
    CHECK(named.out.find("{\"path\":\"b.md\",\"reason\":\"relation\",\"label\":\"uses\",\"other\":\"c.md\","
                         "\"direction\":\"outgoing\"}") != std::string::npos);
    CHECK(named.out.find("\"direction\":\"incoming\"") != std::string::npos);

    // Etiket metinde de geçer: içerik satırı kazanır, ilişki satırı yinelenmez.
    CHECK(box.run({"find", "proje"}).out == "a.md:1: #Proje notu\n");
    CHECK(box.run({"find", "zzz"}).exit_code == 1);
}

// -l boru çıktısında neden tek satırdır: satır sonu ya da sekme satırı ve TAB ayracını bozmasın.
TEST_CASE("single_line turns line breaks and tabs into spaces") {
    CHECK(noteapp::cli::single_line("x\ny\rz\tw") == "x y z w");
    CHECK(noteapp::cli::single_line("bağlı: b.md") == "bağlı: b.md");
}

TEST_CASE("highlight wraps byte ranges in ANSI bold colour and tolerates bad ranges") {
    using noteapp::cli::highlight_ranges;
    const std::string on = "\x1b[1;33m";
    const std::string off = "\x1b[0m";

    CHECK(highlight_ranges("abc def", {{0, 3}, {4, 7}}) == on + "abc" + off + " " + on + "def" + off);
    CHECK(highlight_ranges("abc", {}) == "abc");
    CHECK(highlight_ranges("abc", {{2, 99}}) == "ab" + on + "c" + off);              // taşan aralık kırpılır
    CHECK(highlight_ranges("abc", {{1, 2}, {1, 3}}) == "a" + on + "b" + off + "c");  // çakışan atlanır
    CHECK(highlight_ranges("abc", {{2, 2}}) == "abc");                               // boş aralık
}

// Senkronla gelen bir not terminale OSC 52 (pano yazma) ya da başlık değiştiren dizi yollayamamalı.
TEST_CASE("sanitize_for_terminal neutralises control sequences but keeps text, tabs and newlines") {
    using noteapp::cli::sanitize_for_terminal;
    const std::string r = "\xEF\xBF\xBD";  // U+FFFD

    CHECK(sanitize_for_terminal("düz metin\tsekme\n") == "düz metin\tsekme\n");
    CHECK(sanitize_for_terminal("a\r\nb\r") == "a\nb");  // CR atılır
    CHECK(sanitize_for_terminal("\x1b]52;c;ZXZpbA==\x07") == r + "]52;c;ZXZpbA==" + r);
    CHECK(sanitize_for_terminal("\x1b]0;başlık\x1b\\") == r + "]0;başlık" + r + "\\");
    CHECK(sanitize_for_terminal(std::string("a\0b\x7f", 4)) == "a" + r + "b" + r);
    // C1 (U+0080–U+009F) UTF-8 kodlu tek yer tutucu olur: CSI (U+009B), NEL (U+0085).
    CHECK(sanitize_for_terminal("\xC2\x9B[31m") == r + "[31m");
    CHECK(sanitize_for_terminal("\xC2\x85x") == r + "x");
    // Geçersiz UTF-8: çıplak 8 bit CSI ve kesik dizi.
    CHECK(sanitize_for_terminal("\x9B[31m") == r + "[31m");
    CHECK(sanitize_for_terminal("ab\xE2\x82") == "ab" + r + r);
    // C1'in hemen üstü (NBSP) ve çok baytlı karakterler kalır.
    CHECK(sanitize_for_terminal("\xC2\xA0ğ€😀") == "\xC2\xA0ğ€😀");
}

// T91: TUI ve GUI ile aynı liste (core::hidden_controls): metin saklandığından farklı sırayla görünmesin.
TEST_CASE("sanitize_for_terminal shows bidi and zero-width characters as U+FFFD, ZWJ and ZWNJ stay") {
    using noteapp::cli::sanitize_for_terminal;
    const std::string r = "\xEF\xBF\xBD";
    CHECK(sanitize_for_terminal("gpj\xE2\x80\xAE"
                                "exe.md") == "gpj" + r + "exe.md");
    CHECK(sanitize_for_terminal("a\xE2\x81\xA6"
                                "b\xE2\x80\x8B"
                                "c\xD8\x9C"
                                "d\xEF\xBB\xBF"
                                "e\xE2\x81\xA0"
                                "f") == "a" + r + "b" + r + "c" + r + "d" + r + "e" + r + "f");
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    CHECK(sanitize_for_terminal(family) == family);
    CHECK(sanitize_for_terminal("\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE") ==
          "\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE");
}

TEST_CASE("highlight sanitises the text around and inside the ranges") {
    using noteapp::cli::highlight_ranges;
    const std::string r = "\xEF\xBF\xBD";
    CHECK(highlight_ranges("a\x1b[2Jb", {{0, 1}}) == "\x1b[1;33ma\x1b[0m" + r + "[2Jb");
    CHECK(highlight_ranges("x\x07y", {{0, 3}}) == "\x1b[1;33mx" + r + "y\x1b[0m");
}
