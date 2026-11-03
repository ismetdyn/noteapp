#include "core/json_parser.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp::core;

namespace {

std::optional<Json> parse(std::string_view text, JsonLimits limits = {}) {
    return JsonParser(text, limits).parse();
}

// Tek bir JSON dizesinin çözülmüş baytları; ayrıştırılamazsa nullopt.
std::optional<std::string> string_of(std::string_view literal) {
    auto parsed = parse(literal);
    if (!parsed || parsed->kind != Json::Kind::String)
        return std::nullopt;
    return parsed->text;
}

std::string nested_arrays(int levels) {
    return std::string(static_cast<std::size_t>(levels), '[') +
           std::string(static_cast<std::size_t>(levels), ']');
}

}  // namespace

TEST_CASE("json accepts space, tab, CR and LF between every token") {
    const auto parsed = parse(" \t\r\n{ \"a\" :\n[1 ,\r\n2\t]\r}\n");
    REQUIRE(parsed);
    REQUIRE(parsed->fields.at("a").items.size() == 2);
    CHECK(parsed->fields.at("a").items[1].text == "2");
}

TEST_CASE("json null is a value of its own kind; near misses are rejected") {
    const auto parsed = parse(R"({"a":null,"b":[null]})");
    REQUIRE(parsed);
    CHECK(parsed->fields.at("a").kind == Json::Kind::Null);
    CHECK(parsed->fields.at("b").items[0].kind == Json::Kind::Null);
    CHECK_FALSE(parse("nul"));
    CHECK_FALSE(parse("nulll"));
    CHECK_FALSE(parse("NULL"));
}

TEST_CASE("json numbers keep their raw text and carry a double, fraction and exponent included") {
    const auto integer = parse("42");
    REQUIRE(integer);
    CHECK(integer->kind == Json::Kind::Number);
    CHECK(integer->text == "42");
    CHECK(integer->number == 42.0);

    const auto fraction = parse("-0.25e+2");
    REQUIRE(fraction);
    CHECK(fraction->text == "-0.25e+2");
    CHECK(fraction->number == -25.0);

    CHECK(parse("1E3")->number == 1000.0);
    CHECK(parse("0.5")->number == 0.5);
    CHECK(parse("2e-1")->number == doctest::Approx(0.2));
    // 2^53 + 1 bir double'a sığmaz; ham metin tam tamsayıyı taşımaya devam eder.
    CHECK(parse("9007199254740993")->text == "9007199254740993");
}

TEST_CASE("json number grammar is strict") {
    for (const std::string_view bad : {"01", "-01", "1.", ".5", "+1", "1e", "1e+", "-", "--1", "0x10", "1.e3",
                                       "1-2", "Infinity", "NaN", "1e999"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse(bad));
    }
    CHECK(parse("-0"));
    CHECK(parse("0e0"));
}

TEST_CASE("json decodes every escape of RFC 8259") {
    CHECK(string_of(R"("\b\f\/\n\r\t\"\\")") == std::string("\b\f/\n\r\t\"\\"));
    CHECK(string_of(R"("çÇ")") == std::string("\xC3\xA7\xC3\x87"));
    CHECK(string_of(R"("€")") == std::string("\xE2\x82\xAC"));
    // U+1F600, surrogate çiftiyle.
    CHECK(string_of(R"("😀")") == std::string("\xF0\x9F\x98\x80"));
    CHECK(string_of(R"("a\u0000b")") == std::string("a\0b", 3));
}

TEST_CASE("json rejects lone surrogates, unknown escapes and raw control characters") {
    for (const std::string_view bad :
         {R"("\ud83d")", R"("\ud83dx")", R"("\ud83dA")", R"("\ude00")", R"("\ud83d\ud83d")", R"("\x41")",
          R"("\u12")", R"("\u12g4")", "\"a\nb\"", "\"a\tb\"", "\"\x1f\""}) {
        CAPTURE(bad);
        CHECK_FALSE(parse(bad));
    }
}

TEST_CASE("json default limits are the old import limits") {
    constexpr JsonLimits kDefaults{};
    CHECK(kDefaults.max_depth == 6);
    CHECK(kDefaults.max_values == 100000);
    CHECK(parse(nested_arrays(7)));
    CHECK_FALSE(parse(nested_arrays(8)));
}

TEST_CASE("json limits are configurable: depth, value count and input size") {
    CHECK(parse(nested_arrays(64), JsonLimits{.max_depth = 63}));
    CHECK_FALSE(parse(nested_arrays(65), JsonLimits{.max_depth = 63}));

    CHECK(parse("[1,2]", JsonLimits{.max_values = 3}));
    CHECK_FALSE(parse("[1,2,3]", JsonLimits{.max_values = 3}));

    CHECK(parse("[1, 2]", JsonLimits{.max_bytes = 6}));
    CHECK_FALSE(parse("[1,  2]", JsonLimits{.max_bytes = 6}));
}

TEST_CASE("json old import lines parse as before") {
    const auto parsed =
        parse(R"({"k":"node","id":"0123","labels":["Note"],"props":{"n":{"i":"-5"},"p":{"p":true}}})");
    REQUIRE(parsed);
    CHECK(parsed->fields.at("k").text == "node");
    CHECK(parsed->fields.at("props").fields.at("n").fields.at("i").text == "-5");
    CHECK(parsed->fields.at("props").fields.at("p").fields.at("p").text == "true");
    CHECK_FALSE(parse(R"({"k":"node",})"));
    CHECK_FALSE(parse(R"({"k" "node"})"));
}
