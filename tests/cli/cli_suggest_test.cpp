// cli::suggest: yazım hatasına en yakın tek aday (saf fonksiyon, birim test).
#include "cli/command_table.hpp"
#include "cli/suggest.hpp"

#include <array>
#include <doctest/doctest.h>
#include <string_view>
#include <vector>

using noteapp::cli::suggest;

namespace {

std::vector<std::string_view> command_names() {
    std::vector<std::string_view> names;
    for (const auto& command : noteapp::cli::kCommands)
        names.push_back(command.name);
    return names;
}

}  // namespace

TEST_CASE("suggest_returns_the_single_closest_command") {
    const auto commands = command_names();
    CHECK(suggest("lss", commands) == "ls");
    CHECK(suggest("shw", commands) == "show");
    CHECK(suggest("improt", commands) == "import");
}

TEST_CASE("suggest_returns_nothing_for_far_words") {
    const auto commands = command_names();
    CHECK_FALSE(suggest("zzz", commands));
    CHECK_FALSE(suggest("x", commands));
    CHECK_FALSE(suggest("", commands));
}

TEST_CASE("suggest_returns_nothing_when_two_candidates_tie") {
    constexpr std::array<std::string_view, 2> tied{"abce", "abcf"};
    CHECK_FALSE(suggest("abcd", tied));
}

TEST_CASE("suggest_counts_code_points_not_bytes") {
    // Bayt sayılsaydı "tağ" (4 bayt) "tag"dan 2 uzakta olur, eşik (2*d < uzunluk) tutmazdı.
    constexpr std::array<std::string_view, 1> ascii{"tag"};
    CHECK(suggest("tağ", ascii) == "tag");
    constexpr std::array<std::string_view, 2> turkish{"çalışma", "çizim"};
    CHECK(suggest("çalısma", turkish) == "çalışma");
}

TEST_CASE("suggest_finds_options_and_verbs") {
    constexpr std::array<std::string_view, 3> options{"--json", "--limit", "--folder"};
    CHECK(suggest("--jsno", options) == "--json");
    CHECK(suggest("--limt", options) == "--limit");
    constexpr std::array<std::string_view, 6> verbs{"status", "on", "off", "init", "login", "logout"};
    CHECK(suggest("stauts", verbs) == "status");
}
