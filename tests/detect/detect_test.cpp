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

using namespace noteapp::detect;
namespace fs = std::filesystem;

namespace {

const Options kOpts{"md", 0.5, 64 * 1024};

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
        std::snprintf(line, sizeof line, "%-5s %2d/%-2d %5.1f%%\n", type.c_str(), t.hit, t.total,
                      100.0 * t.hit / t.total);
        table << line;
    }
    const double overall = total ? static_cast<double>(hit) / total : 0.0;
    MESSAGE("accuracy per type:\n"
            << table.str() << "overall " << hit << "/" << total << " = " << overall * 100 << "%\n"
            << (misses.str().empty() ? "" : "misses:\n") << misses.str());
    CHECK(tally.size() == 8);
    CHECK(overall >= 0.90);
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

TEST_CASE("ranked holds all 8 types sorted by score and confidence is in [0,1]") {
    for (const char* text : {"#include <a>\nint main() {}\n", "a,b\n1,2\n", "düz yazı\nikinci satır\n",
                             "# Başlık\n- madde\n- madde\n", "x\n"}) {
        const auto s = suggest(text, kOpts);
        REQUIRE(s.ranked.size() == 8);
        CHECK(std::is_sorted(s.ranked.begin(), s.ranked.end(),
                             [](const Candidate& a, const Candidate& b) { return a.score > b.score; }));
        CHECK(s.confidence >= 0.0);
        CHECK(s.confidence <= 1.0);
        std::vector<std::string> names;
        for (const auto& c : s.ranked)
            names.push_back(c.type);
        std::sort(names.begin(), names.end());
        CHECK(names == std::vector<std::string>{"cpp", "csv", "java", "json", "md", "py", "sh", "txt"});
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
        CHECK(s.ranked.size() == 8);
    }
}
