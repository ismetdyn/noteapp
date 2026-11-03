// A1 eşitlik bekçisi, GUI yarısı: kayıttaki her komutun GUI tablosunda bağı/eylemi var ya da izin listesinde
// sahipli bir eksik olarak durur (tests/support/command_parity_gaps.hpp).
#include "app/commands.hpp"
#include "gui/commands.hpp"
#include "support/command_parity_gaps.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <set>
#include <string_view>
#include <utility>

using namespace noteapp;

TEST_CASE("parity: every registry command has a GUI binding or an owned gap row") {
    for (const app::CommandSpec& spec : app::kCommandRegistry) {
        CAPTURE(spec.id);
        const bool bound = std::ranges::any_of(
            gui::kGuiCommands, [&](const gui::GuiCommand& command) { return command.id == spec.id; });
        const char* problem = test::parity_problem(test::CommandUi::Gui, spec.id, bound);
        CHECK(std::string_view(problem ? problem : "") == "");  // değerde talimat görünür
    }
}

TEST_CASE("GUI command table: every action is set, a chord means one command per scope") {
    std::set<std::pair<gui::CommandScope, ImGuiKeyChord>> chords;
    for (const gui::GuiCommand& command : gui::kGuiCommands) {
        CAPTURE(command.id);
        CHECK(command.run != nullptr);
        if (command.scope != gui::CommandScope::Widget)
            CHECK(command.chord != 0);  // Global ve List tuşla dinlenir
        if (command.chord != 0)
            CHECK(chords.emplace(command.scope, command.chord).second);
    }
}
