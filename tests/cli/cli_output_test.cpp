// Makine ve uzun çıktı biçimleri (SPEC-cli "Çıktı biçimleri"): ls --json/-l/-0/--limit, find --json. Varsayılan
// çıktılar bayt bayt aynı kalır.
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp::test;

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("ls default output is unchanged by the new flags") {
    const Sandbox box("en");
    box.put("old.md", "o", 300);
    box.put("w/mid.txt", "mm", 200);
    box.put("new.md", "nnn", 100);
    CHECK(box.run({"ls"}).out == "new.md\nw/mid.txt\nold.md\n");
}

TEST_CASE("ls --json is an empty array on an empty store") {
    const Sandbox box("en");
    const auto result = box.run({"ls", "--json"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "[]\n");
}

TEST_CASE("ls --json prints path, type, size and updated_at in ls order") {
    const Sandbox box("en");
    box.put("old.md", "o", 300);
    box.put("w/mid.txt", "mm", 200);
    const auto result = box.run({"ls", "--json"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.starts_with("[{\"path\":\"w/mid.txt\",\"type\":\"txt\",\"size\":2,\"updated_at\":"));
    CHECK(contains(result.out, "},{\"path\":\"old.md\",\"type\":\"md\",\"size\":1,\"updated_at\":"));
    CHECK(result.out.ends_with("}]\n"));
}

TEST_CASE("ls --limit and -n cap the rows; invalid limits are usage errors") {
    const Sandbox box("en");
    box.put("a.md", "a", 300);
    box.put("b.md", "b", 200);
    box.put("c.md", "c", 100);
    CHECK(box.run({"ls", "--limit", "2"}).out == "c.md\nb.md\n");
    CHECK(box.run({"ls", "-n", "1"}).out == "c.md\n");
    CHECK(box.run({"ls", "--limit=5"}).out == "c.md\nb.md\na.md\n");
    for (const char* bad : {"0", "x", "-1"}) {
        CAPTURE(bad);
        const auto result = box.run({"ls", "--limit", bad});
        CHECK(result.exit_code == 2);
        CHECK(result.out.empty());
    }
}

TEST_CASE("ls -0 separates paths with NUL bytes") {
    const Sandbox box("en");
    box.put("a b.md", "a", 300);
    box.put("c.md", "c", 100);
    for (const char* flag : {"-0", "--print0"}) {
        CAPTURE(flag);
        CHECK(box.run({"ls", flag}).out == std::string("c.md\0a b.md\0", 12));
    }
}

TEST_CASE("ls -l aligns time, size, type and path columns") {
    const Sandbox box("en");
    box.put("a.md", "aa", 300);
    box.put("big.txt", std::string(120, 'x'), 100);
    const auto result = box.run({"ls", "--long"});
    CHECK(result.exit_code == 0);
    CHECK(box.run({"ls", "-l"}).out == result.out);
    const auto big_end = result.out.find('\n');
    REQUIRE(big_end != std::string::npos);
    const std::string big = result.out.substr(0, big_end);
    const std::string small = result.out.substr(big_end + 1, result.out.size() - big_end - 2);
    CHECK(big.ends_with("  120  txt  big.txt"));
    CHECK(small.ends_with("    2  md   a.md"));
    CHECK(big.size() - std::string_view("big.txt").size() == small.size() - std::string_view("a.md").size());
    // "YYYY-MM-DD HH:MM" yerel saat
    CHECK(big[4] == '-');
    CHECK(big[10] == ' ');
    CHECK(big[13] == ':');
}

TEST_CASE("find --json prints path, line, snippet and byte ranges; no match is [] with exit 1") {
    const Sandbox box("en");
    box.put("hello.md", "say hello world\n");
    const auto result = box.run({"find", "--json", "hello"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "[{\"path\":\"hello.md\",\"line\":1,\"snippet\":\"say hello world\",\"ranges\":[[4,9]]}]\n");
    const auto none = box.run({"find", "--json", "zzzz"});
    CHECK(none.exit_code == 1);
    CHECK(none.out == "[]\n");
    // Varsayılan biçim aynı.
    CHECK(box.run({"find", "hello"}).out == "hello.md:1: say hello world\n");
}
