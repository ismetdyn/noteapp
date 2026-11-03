// A1 eşitlik bekçisi, TUI yarısı: kayıttaki her komutun TUI tablosunda bağı/eylemi var ya da izin listesinde
// sahipli bir eksik olarak durur (tests/support/command_parity_gaps.hpp).
#include "app/commands.hpp"
#include "support/command_parity_gaps.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string_view>

using namespace noteapp;

TEST_CASE("parity: every registry command has a TUI binding or an owned gap row") {
    for (const app::CommandSpec& spec : app::kCommandRegistry) {
        CAPTURE(spec.id);
        const bool bound = std::ranges::any_of(
            tui::TuiState::commands(), [&](const tui::Command& command) { return command.id == spec.id; });
        const char* problem = test::parity_problem(test::CommandUi::Tui, spec.id, bound);
        CHECK(std::string_view(problem ? problem : "") == "");  // değerde talimat görünür
    }
}
