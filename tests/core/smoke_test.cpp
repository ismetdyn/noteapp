#include "core/version.hpp"

#include <doctest/doctest.h>
#include <expected>
#include <string>

TEST_CASE("version reports the project version") {
    CHECK(noteapp::core::version() == "0.1.0");
}

// Araç zinciri sınırı (GCC 13): spec'in hata modeli std::expected'a dayanır.
TEST_CASE("std::expected is available on the configured toolchain") {
    std::expected<int, std::string> result = std::unexpected(std::string("failure"));
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == "failure");
}
