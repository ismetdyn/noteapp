// note find (T16): içerik araması çıktısı `yol:satır: kesit`; boruda ANSI yok.
#include "cli/highlight.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::test;

namespace {

void put(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

}  // namespace

TEST_CASE("find lists path:line: snippet for every hit (SPEC-cli acceptance 6)") {
    const Sandbox box;
    put(box.notes() / "plan.md", "proje başlangıcı\nbaşka satır\nbütçe onaylandı\n");
    put(box.notes() / "sadece-proje.md", "proje var, gerisi yok\n");

    const auto result = box.run({"find", "proje bütçe"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "plan.md:1: proje başlangıcı\nplan.md:3: bütçe onaylandı\n");
}

TEST_CASE("find joins several arguments into one query") {
    const Sandbox box;
    put(box.notes() / "plan.md", "proje başlangıcı\nbütçe onaylandı\n");
    put(box.notes() / "other.md", "proje\n");

    const auto result = box.run({"find", "proje", "bütçe"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find("plan.md:1:") == 0);
    CHECK(result.out.find("other.md") == std::string::npos);
}

TEST_CASE("find folds Turkish letters") {
    const Sandbox box;
    put(box.notes() / "k.md", "Öğrenci listesi\n");

    const auto result = box.run({"find", "ogrenci"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "k.md:1: Öğrenci listesi\n");
}

TEST_CASE("find prints no ANSI escape codes when stdout is not a terminal") {
    const Sandbox box;
    put(box.notes() / "k.md", "merhaba dünya\n");

    const auto result = box.run({"find", "dünya"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find('\x1b') == std::string::npos);
}

TEST_CASE("find with no match exits 1 and prints nothing to stdout") {
    const Sandbox box;
    put(box.notes() / "k.md", "merhaba\n");

    const auto result = box.run({"find", "yokboyle"});
    CHECK(result.exit_code == 1);
    CHECK(result.out.empty());
    CHECK(result.err.find("yokboyle") != std::string::npos);
}

TEST_CASE("find --limit caps the number of notes; usage errors exit 2") {
    const Sandbox box;
    put(box.notes() / "a.md", "kelime\n");
    put(box.notes() / "b.md", "kelime\n");

    const auto limited = box.run({"find", "kelime", "--limit", "1"});
    CHECK(limited.exit_code == 0);
    CHECK(limited.out.size() == std::string("a.md:1: kelime\n").size());

    CHECK(box.run({"find"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit", "x"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit", "0"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--limit"}).exit_code == 2);
    CHECK(box.run({"find", "kelime", "--zorla"}).exit_code == 2);
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
