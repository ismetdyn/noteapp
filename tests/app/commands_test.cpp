// A1: iki arayüzün ortak komut kaydı, palet sıralaması ve son kullanılanlar.
#include "app/commands.hpp"
#include "core/i18n.hpp"
#include "search/fuzzy.hpp"
#include "support/command_parity_gaps.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace noteapp;
using app::PaletteCandidate;

namespace {

std::vector<std::string_view> ranked_ids(std::string_view query,
                                         const std::vector<PaletteCandidate>& candidates,
                                         const std::vector<std::string>& recent = {}) {
    std::vector<std::string_view> ids;
    for (const std::size_t index :
         app::rank_palette(query, candidates, recent, search::kDefaultFuzzyCandidateChars))
        ids.push_back(candidates.at(index).id);
    return ids;
}

}  // namespace

TEST_CASE("registry ids are unique English names with a translated label in both languages") {
    std::set<std::string_view> seen;
    for (const char* language : {"en", "tr"}) {
        CAPTURE(language);
        core::init_i18n(language);
        for (const app::CommandSpec& spec : app::kCommandRegistry) {
            CAPTURE(spec.id);
            CHECK(std::ranges::all_of(spec.id, [](unsigned char c) { return c < 0x80 && c != ' '; }));
            CHECK(core::tr(spec.label_key) != spec.label_key);
            CHECK(spec.contexts != 0);
            CHECK((spec.contexts & ~app::kAnywhere) == 0);
            seen.insert(spec.id);
        }
    }
    CHECK(seen.size() == std::size(app::kCommandRegistry));
    core::init_i18n("en");
}

TEST_CASE("find_command returns the registry row, nullptr for an unknown id") {
    const app::CommandSpec* paste = app::find_command("paste");
    REQUIRE(paste != nullptr);
    CHECK(paste->needs == app::CommandNeeds::Yank);
    CHECK(app::find_command("no.such.command") == nullptr);
    static_assert(app::find_command("new") != nullptr);
}

TEST_CASE("rank_palette keeps candidates matching the id or the translated label, best score first") {
    core::init_i18n("en");
    const std::vector<PaletteCandidate> candidates = {
        {"quit", "command.quit", false},
        {"sort.name", "command.sort_name", false},
        {"new", "command.new", false},
    };
    CHECK(ranked_ids("sort", candidates) == std::vector<std::string_view>{"sort.name"});
    // "New note" yalnız etiketle eşleşir.
    CHECK(ranked_ids("note", candidates) == std::vector<std::string_view>{"new"});
    CHECK(ranked_ids("zzz", candidates).empty());
}

TEST_CASE("rank_palette puts blocked commands last, then orders by score, then by recency") {
    core::init_i18n("en");
    const std::vector<PaletteCandidate> candidates = {
        {"new", "command.new", true},
        {"quit", "command.quit", false},
        {"help", "command.help", false},
        {"settings", "command.settings", false},
    };
    // Boş süzgeç: çalışanlar giriş sırasında, son kullanılan öne; engelli satır görünür ama sonda.
    CHECK(ranked_ids("", candidates) == std::vector<std::string_view>{"quit", "help", "settings", "new"});
    CHECK(ranked_ids("", candidates, {"settings", "new", "help"}) ==
          std::vector<std::string_view>{"settings", "help", "quit", "new"});
}

TEST_CASE("remember_command moves the id to the front without duplicates and honours the limit") {
    std::vector<std::string> recent = {"a", "b", "c"};
    app::remember_command(recent, "c", core::Limit{3, "3"});
    CHECK(recent == std::vector<std::string>{"c", "a", "b"});
    app::remember_command(recent, "d", core::Limit{3, "3"});
    CHECK(recent == std::vector<std::string>{"d", "c", "a"});
}

TEST_CASE("remember_command with limit 0 keeps every command") {
    std::vector<std::string> recent;
    constexpr int kMany = 500;
    for (int i = 0; i < kMany; ++i)
        app::remember_command(recent, "cmd" + std::to_string(i), core::Limit{0, "0"});
    CHECK(recent.size() == kMany);
    CHECK(recent.front() == "cmd499");
}

TEST_CASE("parity gap rows name registry commands, carry a plan-parity owner and appear once") {
    std::set<std::pair<test::CommandUi, std::string_view>> seen;
    for (const test::CommandGap& gap : test::kCommandGaps) {
        CAPTURE(gap.id);
        CHECK(app::find_command(gap.id) != nullptr);
        CHECK(gap.owner.starts_with("P-"));
        CHECK(seen.emplace(gap.ui, gap.id).second);
    }
}
