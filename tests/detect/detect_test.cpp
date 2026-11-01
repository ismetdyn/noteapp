#include "detect/detect.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace noteapp::detect;
namespace fs = std::filesystem;

namespace {

const Options kOpts{"md", 0.5, 64 * 1024};

const std::vector<std::string> kAllTypes{
    "c",        "cpp", "css",        "csv", "dockerfile", "env", "go",  "html", "ini", "java", "js",  "json",
    "makefile", "md",  "properties", "py",  "rs",         "sh",  "sql", "toml", "ts",  "txt",  "xml", "yaml"};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string type_of(const std::string& text) {
    return suggest(text, kOpts).type;
}

}  // namespace

TEST_CASE("corpus: top-1 accuracy is at least 90 percent") {
    struct Tally {
        int total = 0;
        int hit = 0;
    };
    std::map<std::string, Tally> tally;
    std::ostringstream misses;
    int total = 0, hit = 0;

    for (const auto& dir : fs::directory_iterator(NOTEAPP_DETECT_SAMPLES_DIR)) {
        if (!dir.is_directory())
            continue;
        const std::string expected = dir.path().filename().string();
        for (const auto& f : fs::directory_iterator(dir.path())) {
            const std::string got = type_of(read_file(f.path()));
            ++tally[expected].total;
            ++total;
            if (got == expected) {
                ++tally[expected].hit;
                ++hit;
            } else {
                misses << "  " << expected << "/" << f.path().filename().string() << " -> " << got << "\n";
            }
        }
    }

    std::ostringstream table;
    for (const auto& [type, t] : tally) {
        char line[64];
        std::snprintf(line, sizeof line, "%-10s %2d/%-2d %5.1f%%\n", type.c_str(), t.hit, t.total,
                      100.0 * t.hit / t.total);
        table << line;
    }
    const double overall = total ? static_cast<double>(hit) / total : 0.0;
    MESSAGE("accuracy per type:\n"
            << table.str() << "overall " << hit << "/" << total << " = " << overall * 100 << "%\n"
            << (misses.str().empty() ? "" : "misses:\n") << misses.str());
    CHECK(tally.size() == kAllTypes.size());
    for (const auto& name : kAllTypes) {
        CHECK_MESSAGE(tally[name].total >= 5, name);  // SPEC-detect kabul 1: tip başına en az 5 örnek
        // Tek bir tip genel ortalamanın arkasına saklanmasın.
        CHECK_MESSAGE(tally[name].hit * 100 >= tally[name].total * 80, name);
    }
    CHECK(overall >= 0.90);
}

TEST_CASE("must-pass: env, properties, ini and shell are told apart") {
    CHECK(type_of("DB_HOST=localhost\nDB_PORT=5432\n") == "env");
    CHECK(type_of("# prod\nexport API_URL=https://a\nTIMEOUT=3\n") == "env");
    CHECK(type_of("server.port=8080\nserver.host=a\n") == "properties");
    CHECK(type_of("server.port: 8080\nserver.host: a\n") == "properties");
    CHECK(type_of("[db]\nhost=a\nport=1\n") == "ini");
    CHECK(type_of("A=1\necho \"$A\"\n") == "sh");
    CHECK(type_of("name=a\ntarget=$name\ncp \"$target\" /tmp\n") == "sh");
}

TEST_CASE("must-pass: prose with colons or equals stays prose") {
    CHECK(type_of("Note: the meeting moved to Friday because the room was taken.\n"
                  "Reminder: bring the printed budget with you.\n") == "txt");
    CHECK(type_of("Not: yarın toplantı var\nDurum: devam ediyor\n") == "txt");
    CHECK(type_of("Formül şöyle: a = b + c demek.\nYani toplam = fiyat kere adet olur.\n") == "txt");
}

TEST_CASE("must-pass: yaml and toml are told apart from ini and properties") {
    CHECK(type_of("services:\n  web:\n    image: a\n") == "yaml");
    CHECK(type_of("---\nname: a\n") == "yaml");
    CHECK(type_of("[package]\nname = \"a\"\nversion = \"1\"\n") == "toml");
    CHECK(type_of("title = \"a\"\ncount = 3\n") == "toml");
}

TEST_CASE("must-pass: markup and code families") {
    CHECK(type_of("<?xml version=\"1.0\"?>\n<a/>\n") == "xml");
    CHECK(type_of("<!doctype html>\n<p>a</p>\n") == "html");
    CHECK(type_of("body {\n  color: red;\n}\n") == "css");
    CHECK(type_of("function f() {\n  return 1;\n}\n") == "js");
    CHECK(type_of("interface A {\n  id: number;\n}\n") == "ts");
    CHECK(type_of("SELECT a\nFROM b;\n") == "sql");
    CHECK(type_of("#include <stdio.h>\nint main(void) { return 0; }\n") == "c");
    CHECK(type_of("package main\n\nfunc main() {\n}\n") == "go");
    CHECK(type_of("fn main() {\n    println!(\"a\");\n}\n") == "rs");
    CHECK(type_of("FROM a:1\nRUN b\n") == "dockerfile");
    CHECK(type_of("all: a\n\tcc a\n") == "makefile");
}

TEST_CASE("structural decisions keep the shebang first") {
    CHECK(type_of("#!/bin/sh\nA=1\nB=2\n") == "sh");
}

TEST_CASE("must-pass: markdown with a fenced cpp block is md") {
    CHECK(type_of("# Örnek\n\nŞu koda bak:\n\n```cpp\n#include <iostream>\nint main() {\n"
                  "    std::cout << 1;\n}\n```\n\nBitti.\n") == "md");
}

TEST_CASE("must-pass: #include plus class is cpp") {
    CHECK(type_of("#include <string>\n\nclass A {\n    std::string s;\n};\n") == "cpp");
}

TEST_CASE("must-pass: public class plus import java.util is java") {
    CHECK(type_of("import java.util.*;\n\npublic class A {\n    int x;\n}\n") == "java");
}

TEST_CASE("must-pass: Turkish prose lines ending with a colon are not py") {
    CHECK(type_of("Alışveriş listesi:\nsüt ve ekmek\nToplantı notları:\nyarın konuşulacak\n") == "txt");
}

TEST_CASE("must-pass: comma-heavy prose paragraph is not csv") {
    const auto got = type_of("Dün eve geç geldim, yemek yedim, kitap okudum ve uyudum, ama\n"
                             "sabah yine yorgundum, çünkü gece boyunca, sürekli rüya gördüm.\n"
                             "Kahvaltıda, çay içerken, reçeli denedim, çok güzeldi.\n");
    CHECK(got != "csv");
    CHECK(got == "txt");
}

TEST_CASE("must-pass: bash shebang is sh with confidence 1") {
    const auto s = suggest("#!/usr/bin/env bash\necho hi\n", kOpts);
    CHECK(s.type == "sh");
    CHECK(s.confidence == doctest::Approx(1.0));
    CHECK(s.detected);
    CHECK(type_of("#!/usr/bin/env python3\nprint(1)\n") == "py");
}

TEST_CASE("json structural check") {
    CHECK(type_of("{\n  \"a\": 1,\n  \"b\": [1, 2]\n}\n") == "json");
    CHECK(type_of("[\n  1,\n  2\n]") == "json");
    // Dengesiz parantez json değildir.
    CHECK(type_of("{\n  \"a\": 1,\n  \"b\": [1, 2\n}\n") != "json");
}

TEST_CASE("csv needs a consistent delimiter") {
    CHECK(type_of("a,b,c\n1,2,3\n4,5,6\n") == "csv");
    CHECK(type_of("a;b\n1;2\n3;4\n") == "csv");
}

TEST_CASE("single line or empty input falls back to default_type") {
    for (const char* text : {"", "   \n\n", "tek satır", "tek satır\n", "#include <vector>", "# Başlık\n"}) {
        const auto s = suggest(text, Options{"txt", 0.5, 1024});
        CHECK_MESSAGE(s.type == "txt", text);
        CHECK_MESSAGE(!s.detected, text);
    }
    CHECK(suggest("x", Options{"md", 0.5, 1024}).type == "md");
}

TEST_CASE("ranked holds all types sorted by score and confidence is in [0,1]") {
    for (const char* text : {"#include <a>\nint main() {}\n", "a,b\n1,2\n", "düz yazı\nikinci satır\n",
                             "# Başlık\n- madde\n- madde\n", "x\n"}) {
        const auto s = suggest(text, kOpts);
        REQUIRE(s.ranked.size() == kAllTypes.size());
        CHECK(std::is_sorted(s.ranked.begin(), s.ranked.end(),
                             [](const Candidate& a, const Candidate& b) { return a.score > b.score; }));
        CHECK(s.confidence >= 0.0);
        CHECK(s.confidence <= 1.0);
        std::vector<std::string> names;
        for (const auto& c : s.ranked)
            names.push_back(c.type);
        std::sort(names.begin(), names.end());
        CHECK(names == kAllTypes);
    }
}

TEST_CASE("suggest is deterministic") {
    const std::string text = "import os\n\ndef f(x):\n    return x\n";
    const auto a = suggest(text, kOpts);
    const auto b = suggest(text, kOpts);
    CHECK(a.type == b.type);
    CHECK(a.confidence == b.confidence);
    REQUIRE(a.ranked.size() == b.ranked.size());
    for (std::size_t i = 0; i < a.ranked.size(); ++i) {
        CHECK(a.ranked[i].type == b.ranked[i].type);
        CHECK(a.ranked[i].score == b.ranked[i].score);
    }
}

TEST_CASE("max_scan_bytes never splits a UTF-8 sequence or crashes") {
    const std::string text = "çğış çğış çğış\nöüşçı öüşçı\nsatır üç\n";
    for (std::size_t n = 0; n <= text.size() + 2; ++n) {
        const auto s = suggest(text, Options{"md", 0.5, n});
        CHECK(s.ranked.size() == kAllTypes.size());
    }
}
