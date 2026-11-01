#include "core/version.hpp"
#include "support/cli_sandbox.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <iostream>
#include <string>

using namespace noteapp::test;

TEST_CASE("quick add: text becomes md note, byte-exact, fast") {
    const Sandbox box;
    const auto result = box.run({"merhaba dünya"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Kaydedildi: merhaba-dunya.md (md, önerilen)\n");
    CHECK(box.body("merhaba-dunya.md") == "merhaba dünya");
    // SPEC-cli kabul 1: <= 200 ms hedefi Release içindir; Debug+kabuk açılışında yalnız sağlık sınırı
    // denetlenir.
    // İlk çalıştırma soğuk olabilir (virüs tarayıcı/disk önbelleği); en iyi 5 deneme de raporlanır.
    auto best = result.elapsed;
    for (int i = 0; i < 5; ++i)
        best = std::min(best, box.run({"zamanlama " + std::to_string(i)}).elapsed);
    std::cout << "[timing] quick add: first " << result.elapsed.count() << " ms, best of 5 " << best.count()
              << " ms (incl. cmd.exe spawn)\n";
    CHECK(best.count() < 2000);
}

TEST_CASE("quick add: several text arguments are joined with single spaces") {
    const Sandbox box;
    CHECK(box.run({"a", "b", "c"}).exit_code == 0);
    CHECK(box.body("a-b-c.md") == "a b c");
}

TEST_CASE("quick add: a value on the first line never reaches the file name") {
    const std::pair<std::string, std::string> cases[] = {
        {"AWS_SECRET=sk-live-123", "aws-secret"},
        {"export TOKEN=x", "token"},
        {"API_KEY: abc", "api-key"},
    };
    for (const auto& [text, stem] : cases) {
        CAPTURE(text);
        const Sandbox box;
        const auto result = box.run({text});
        CHECK(result.exit_code == 0);
        CHECK(result.out.starts_with("Kaydedildi: " + stem + "."));
        CHECK(result.out.find("sk-live") == std::string::npos);
    }
    const Sandbox prose;
    CHECK(prose.run({"Toplantı: bütçe"}).out.starts_with("Kaydedildi: toplanti-butce."));
}

TEST_CASE("stdin: a comment-first env note is named by its first key") {
    const Sandbox box;
    const auto result = box.run({"-t", "env"}, "# db ayarlari\nAPI_TOKEN=gizli\n");
    CHECK(result.exit_code == 0);
    CHECK(box.body("api-token.env") == "# db ayarlari\nAPI_TOKEN=gizli\n");
}

TEST_CASE("stdin: java code is detected, -t overrides") {
    const std::string code = "public class A {}\nimport java.util.*;";
    {
        const Sandbox box;
        const auto result = box.run({}, code);
        CHECK(result.exit_code == 0);
        CHECK(result.out.find("(java, önerilen)") != std::string::npos);
        CHECK(box.body("public-class-a.java") == code);
    }
    {
        const Sandbox box;
        const auto result = box.run({"-t", "txt"}, code);
        CHECK(result.exit_code == 0);
        CHECK(result.out.find("(txt, seçilen)") != std::string::npos);
        CHECK(box.body("public-class-a.txt") == code);
    }
}

TEST_CASE("stdin content is stored as read; NoteStore only normalizes CRLF to LF") {
    const Sandbox box;
    CHECK(box.run({"-t", "md", "-n", "ham"}, "satır bir\r\nsatır iki\r\n").exit_code == 0);
    CHECK(box.body("ham.md") == "satır bir\nsatır iki\n");
}

TEST_CASE("empty stdin and no text: exit 1, no file") {
    const Sandbox box;
    const auto result = box.run({}, "  \n");
    CHECK(result.exit_code == 1);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(box.has_any_note());
}

TEST_CASE("folder and name flags") {
    const Sandbox box;
    const auto result = box.run({"-f", "work/2026", "-n", "plan", "x"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Kaydedildi: work/2026/plan.md (md, önerilen)\n");
    CHECK(box.body("work/2026/plan.md") == "x");
}

TEST_CASE("invalid name or folder: exit 2, nothing created") {
    // InvalidPath kullanıcının argümanından gelir => kullanım hatası (2).
    const Sandbox box;
    const auto bad_name = box.run({"-n", "../kacis", "x"});
    CHECK(bad_name.exit_code == 2);
    CHECK_FALSE(bad_name.err.empty());
    const auto bad_folder = box.run({"-f", "../disari", "x"});
    CHECK(bad_folder.exit_code == 2);
    CHECK_FALSE(box.has_any_note());
    CHECK_FALSE(fs::exists(box.notes().parent_path() / "kacis.md"));
}

TEST_CASE("double dash lets a command name be note text") {
    const Sandbox box;
    const auto result = box.run({"--", "ls"});
    CHECK(result.exit_code == 0);
    CHECK(box.body("ls.md") == "ls");
}

TEST_CASE("usage errors exit 2") {
    const Sandbox box;
    CHECK(box.run({"--bilinmeyen"}).exit_code == 2);
    CHECK(box.run({"-t"}).exit_code == 2);
}

TEST_CASE("--version prints the core version") {
    const Sandbox box;
    const auto result = box.run({"--version"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find(std::string(noteapp::core::version())) != std::string::npos);
}

TEST_CASE("--help lists every command") {
    const Sandbox box;
    for (const char* flag : {"--help", "help"}) {
        const auto result = box.run({flag});
        CHECK(result.exit_code == 0);
        for (const char* command : {"note ls", "note find", "note show", "note edit", "note rm", "note mv",
                                    "note export", "note sync", "note config"})
            CHECK(result.out.find(command) != std::string::npos);
    }
}

TEST_CASE("Turkish argument round-trips into content and file name") {
    const Sandbox box;
    const auto result = box.run({"çğıİöşü notu"});
    CHECK(result.exit_code == 0);
    CHECK(box.body("cgiiosu-notu.md") == "çğıİöşü notu");
    CHECK(result.out.find("cgiiosu-notu.md") != std::string::npos);
}

TEST_CASE("language comes from the config") {
    const Sandbox box("en");
    const auto result = box.run({"hello"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Saved: hello.md (md, suggested)\n");
    CHECK(box.run({"export"}).err.find("Usage: note export") != std::string::npos);
}

TEST_CASE("note -n: a typed extension stays as typed; detect only runs when none was typed") {
    for (const std::string_view name : {"deneme.cpp", "deneme.xyz", "a.tar.gz", "notlar.v2", "Kod.CPP"}) {
        CAPTURE(name);
        const Sandbox box;
        const auto result = box.run({"-n", std::string(name), "# baslik"});
        CHECK(result.exit_code == 0);
        CHECK(box.body(std::string(name)) == "# baslik");
    }
    const Sandbox box;
    CHECK(box.run({"-n", "ad", "# baslik"}).exit_code == 0);
    CHECK(box.body("ad.md") == "# baslik");
    // Ad boşluk taşır: slug uygulanmaz, kuralı aynı kaldı.
    CHECK(box.run({"-n", "bir not.txt", "x"}).exit_code == 0);
    CHECK(box.body("bir not.txt") == "x");
}

TEST_CASE("note -n: the type graph property follows the typed extension") {
    const Sandbox box;
    REQUIRE(box.run({"-n", "a.tar.gz", "x"}).exit_code == 0);
    REQUIRE(box.run({"-n", "notlar.v2", "x"}).exit_code == 0);
    const auto rows = box.run({"query", "MATCH (n:Note) RETURN n.name, n.type ORDER BY n.type"});
    CHECK(rows.exit_code == 0);
    CHECK(rows.out.find("a.tar") != std::string::npos);
    CHECK(rows.out.find("gz") != std::string::npos);
    CHECK(rows.out.find("v2") != std::string::npos);
    CHECK(rows.out.find("md") == std::string::npos);
}
