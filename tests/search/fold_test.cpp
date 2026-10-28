#include "search/fold.hpp"

#include <doctest/doctest.h>

using namespace noteapp::search;

TEST_CASE("fold: Turkish letters collapse to ASCII, both cases") {
    CHECK(fold_to_utf8("Öğrenci") == "ogrenci");
    CHECK(fold_to_utf8("ÇĞÖŞÜ çğöşü") == "cgosu cgosu");
    CHECK(fold_to_utf8("I İ ı i") == "i i i i");
    CHECK(fold_to_utf8("ISIK") == "isik");
    CHECK(fold_to_utf8("ışık") == "isik");
    CHECK(fold_to_utf8("İSTANBUL") == "istanbul");
}

TEST_CASE("fold: ASCII and other Latin letters are lowercased, rest kept") {
    CHECK(fold_to_utf8("Hello World_1.md") == "hello world_1.md");
    CHECK(fold_to_utf8("ÀÉÑ Ā Ž") == "àéñ ā ž");
    CHECK(fold_to_utf8("日本語 ß") == "日本語 ß");
}

TEST_CASE("fold: byte offsets map back to the original characters") {
    auto f = fold("aÖb日");
    REQUIRE(f.text.size() == 4);
    CHECK(f.byte_offsets == std::vector<std::size_t>{0, 1, 3, 4});
    CHECK(f.original[1] == U'Ö');
    CHECK(f.text[1] == U'o');
}

TEST_CASE("fold: invalid UTF-8 does not crash, each bad byte becomes U+FFFD") {
    auto f = fold("a\xFF"
                  "b\xE2\x82");
    CHECK(f.text == std::u32string{U'a', 0xFFFD, U'b', 0xFFFD, 0xFFFD});
    CHECK(f.byte_offsets == std::vector<std::size_t>{0, 1, 2, 3, 4});
    CHECK(fold("\xC0\xAF").text.size() == 2);  // fazla uzun kodlama
    CHECK(fold("").text.empty());
}
