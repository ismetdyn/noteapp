// PDF1: sayfa metninin UTF-8 denetimi ve temizliği (detail::decode_one, detail::clean_text).
#include "pdfview/page_text.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp::pdfview::detail;

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

struct DecodeCase {
    std::string_view name;
    std::string_view bytes;
    std::optional<char32_t> code_point;  // nullopt: geçersiz
    std::size_t length;                  // tüketilen bayt (geçersizde 1)
};

}  // namespace

TEST_CASE("decode_one accepts exactly the valid UTF-8 boundaries and consumes one byte of an invalid sequence") {
    const DecodeCase cases[] = {
        {"ASCII", "A", U'A', 1},
        {"NUL", std::string_view("\0", 1), U'\0', 1},
        {"two bytes", "\xC3\xA7", U'ç', 2},
        {"lowest two bytes", "\xC2\x80", U'\u0080', 2},
        {"overlong C0", "\xC0\x80", std::nullopt, 1},
        {"overlong C1", "\xC1\xBF", std::nullopt, 1},
        {"continuation byte as lead", "\x80", std::nullopt, 1},
        {"E0 overlong", "\xE0\x9F\xBF", std::nullopt, 1},
        {"E0 lowest", "\xE0\xA0\x80", U'ࠀ', 3},
        {"ED highest below surrogates", "\xED\x9F\xBF", U'퟿', 3},
        {"ED lone high surrogate", "\xED\xA0\x80", std::nullopt, 1},
        {"ED lone low surrogate", "\xED\xBF\xBF", std::nullopt, 1},
        {"three bytes highest", "\xEF\xBF\xBF", U'￿', 3},
        {"F0 overlong", "\xF0\x8F\xBF\xBF", std::nullopt, 1},
        {"F0 lowest", "\xF0\x90\x80\x80", U'\U00010000', 4},
        {"F4 highest", "\xF4\x8F\xBF\xBF", U'\U0010FFFF', 4},
        {"F4 beyond U+10FFFF", "\xF4\x90\x80\x80", std::nullopt, 1},
        {"F5 lead", "\xF5\x80\x80\x80", std::nullopt, 1},
        {"FF lead", "\xFF", std::nullopt, 1},
        {"cut two bytes", "\xC3", std::nullopt, 1},
        {"cut three bytes", "\xE2\x82", std::nullopt, 1},
        {"cut four bytes", "\xF0\x9F\x98", std::nullopt, 1},
        {"bad second byte", "\xE2\x28\xA1", std::nullopt, 1},
        {"bad third byte", "\xE2\x82\x28", std::nullopt, 1},
        {"bad fourth byte", "\xF0\x9F\x98\x28", std::nullopt, 1},
    };
    for (const DecodeCase& entry : cases) {
        CAPTURE(entry.name);
        std::size_t length = 0;
        const std::optional<char32_t> decoded = decode_one(entry.bytes, length);
        CHECK(decoded == entry.code_point);
        CHECK(length == entry.length);
    }
}

TEST_CASE("clean_text keeps text, newline and tab, and turns controls and invalid bytes into U+FFFD") {
    const std::string r(kReplacement);
    const struct {
        std::string_view name;
        std::string_view raw;
        std::string expected;
    } cases[] = {
        {"plain text, newline and tab", "a\nb\tç", "a\nb\tç"},
        {"no-break space is not a control", "\xC2\xA0", "\xC2\xA0"},
        {"ESC", "a\x1B[2J", "a" + r + "[2J"},
        {"NUL", std::string_view("\0x", 2), r + "x"},
        {"carriage return", "\r", r},
        {"DEL", "\x7F", r},
        {"C1 CSI (U+009B)", "\xC2\x9B", r},
        {"C1 highest (U+009F)", "\xC2\x9F", r},
        {"lone surrogate, byte by byte", "\xED\xA0\x80", r + r + r},
        {"cut sequence at the end", "ok\xE2\x82", "ok" + r + r},
        {"F5 lead", "\xF5", r},
        {"overlong slash", "\xC0\xAF", r + r},
    };
    for (const auto& entry : cases) {
        CAPTURE(entry.name);
        CHECK(clean_text(entry.raw) == entry.expected);
    }
}
