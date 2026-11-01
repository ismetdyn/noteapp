#include "core/note_path.hpp"

#include <doctest/doctest.h>
#include <map>
#include <set>

using namespace noteapp::core;

namespace {

// Girdinin reddedildiğini, InvalidPath kodu ve beklenen `reason` argümanını taşıdığını doğrular.
void check_rejected(std::string_view input, std::string_view reason) {
    CAPTURE(input);
    auto result = NotePath::parse(input);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPath);
    CHECK(result.error().message_key == "error.invalid_path");
    std::string found;
    for (const auto& [key, value] : result.error().args) {
        if (key == "reason")
            found = value;
    }
    CHECK(found == reason);
}

}  // namespace

TEST_CASE("accepts valid relative paths") {
    for (const char* ok : {"plan.md", "work/2026/plan.md", "toplantı-notları.md",
                           "a b/c (conflict 2026-10-28 1432).md", "console.txt", "com10.md", "a.b.c"}) {
        CAPTURE(ok);
        auto result = NotePath::parse(ok);
        REQUIRE(result.has_value());
        CHECK(result->string() == ok);
    }
}

TEST_CASE("keeps UTF-8 intact in the filesystem path") {
    auto result = NotePath::parse("klasör/toplantı.md");
    REQUIRE(result.has_value());
    CHECK(result->value().generic_u8string() == u8"klasör/toplantı.md");
}

TEST_CASE("NotePath is comparable and usable as a map key") {
    auto a = *NotePath::parse("a.md");
    auto b = *NotePath::parse("b.md");
    CHECK(a == *NotePath::parse("a.md"));
    CHECK(a != b);
    CHECK(a < b);
    std::map<NotePath, int> map{{b, 2}, {a, 1}};
    CHECK(map.begin()->first == a);
}

TEST_CASE("rejects empty path") {
    check_rejected("", "empty");
}

TEST_CASE("rejects absolute paths") {
    check_rejected("/etc/passwd", "absolute");
    check_rejected("//server/share", "absolute");
}

TEST_CASE("rejects drive letters") {
    check_rejected("C:", "drive_letter");
    check_rejected("C:/x.md", "drive_letter");
    check_rejected("z:x.md", "drive_letter");
}

TEST_CASE("rejects UNC paths") {
    check_rejected("\\\\server\\share", "unc");
}

TEST_CASE("rejects any backslash") {
    check_rejected("a\\b.md", "backslash");
    check_rejected("a\\", "backslash");
}

TEST_CASE("rejects dot components") {
    check_rejected(".", "dot");
    check_rejected("a/./b.md", "dot");
}

TEST_CASE("rejects dot-dot components") {
    check_rejected("..", "dot_dot");
    check_rejected("../x.md", "dot_dot");
    check_rejected("a/../b.md", "dot_dot");
}

TEST_CASE("rejects empty components") {
    check_rejected("a//b.md", "empty_component");
    check_rejected("a/", "empty_component");
}

TEST_CASE("rejects components starting with a dot") {
    check_rejected(".hidden.md", "hidden");
    check_rejected("a/.keep", "hidden");
    check_rejected("a/.git/config", "hidden");
}

TEST_CASE("rejects reserved characters") {
    for (const char* bad : {"a<b", "a>b", "ab:c", "a\"b", "a|b", "a?b", "a*b"}) {
        check_rejected(bad, "invalid_char");
    }
}

TEST_CASE("rejects control characters") {
    check_rejected(std::string_view("a\nb", 3), "control_char");
    check_rejected(std::string_view("a\tb", 3), "control_char");
    check_rejected(std::string_view("a\0b", 3), "control_char");
    check_rejected("a\x1F"
                   "b",
                   "control_char");
    check_rejected("a\x7F"
                   "b",
                   "control_char");
}

TEST_CASE("rejects Windows reserved device names, case-insensitive, with extensions") {
    for (const char* bad : {"CON", "con", "Prn", "AUX", "nul", "COM1", "com9", "LPT1", "lpt9", "con.txt",
                            "Lpt1.md", "a/NUL.md", "aux.tar.gz"}) {
        check_rejected(bad, "reserved_name");
    }
}

// Windows COM0/LPT0'ı ve üst simge rakamlı (¹ ² ³; UTF-8'de iki bayt) biçimleri de aygıt sayar.
TEST_CASE("rejects COM0/LPT0 and the superscript-digit device names") {
    for (const char* bad : {"COM0", "lpt0.txt", "COM\xC2\xB9", "com\xC2\xB2.md", "Com\xC2\xB3", "LPT\xC2\xB9",
                            "lpt\xC2\xB2", "LPT\xC2\xB3.tar.gz", "a/com\xC2\xB9 .md"}) {
        check_rejected(bad, "reserved_name");
    }
    for (const char* ok : {"COM\xC2\xB4", "COM¹0", "LPT\xC2\xBC", "COMA.md"}) {
        CAPTURE(ok);
        CHECK(NotePath::parse(ok).has_value());
    }
}

TEST_CASE("rejects trailing space or dot in a component") {
    check_rejected("a.md ", "trailing_space");
    check_rejected("a /b.md", "trailing_space");
    check_rejected("a.", "trailing_dot");
    check_rejected("a./b.md", "trailing_dot");
}

TEST_CASE("rejects components longer than 255 bytes") {
    CHECK(NotePath::parse(std::string(255, 'a')).has_value());
    check_rejected(std::string(256, 'a'), "too_long");
    // 128 x 'ı' (2 bayt) = 256 bayt: sınır karakter değil bayt cinsinden.
    std::string utf8;
    for (int i = 0; i < 128; ++i)
        utf8 += "ı";
    check_rejected(utf8, "too_long");
}

TEST_CASE("rejects invalid UTF-8") {
    check_rejected("a\xFF.md", "invalid_utf8");
    check_rejected("a\xC3", "invalid_utf8");
}

TEST_CASE("every rejection carries the offending path") {
    auto result = NotePath::parse("a/../b.md");
    REQUIRE_FALSE(result.has_value());
    bool has_path = false;
    for (const auto& [key, value] : result.error().args) {
        if (key == "path")
            has_path = (value == "a/../b.md");
    }
    CHECK(has_path);
}

TEST_CASE("the console device names CONIN$ and CONOUT$ are reserved too, in any case and with an extension") {
    for (const char* name : {"CONIN$", "conout$", "CONIN$.txt", "ConOut$.md", "alt/CONIN$"}) {
        CAPTURE(name);
        const auto parsed = NotePath::parse(name);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == Error::Code::InvalidPath);
    }
    CHECK(NotePath::parse("CONIN.md").has_value());
}
