#pragma once

// A1 eşitlik bekçisinin izin listesi: app::kCommandRegistry'de olup bir arayüzde bağı/eylemi olmayan
// komutlar. Her satırın sahibi plan-parity görevidir (P-<görev>); görev kapanınca satır silinir. Bekçi iki
// yönde düşer: listede olmayan eksik ve listede kaldığı hâlde artık bağlı olan komut (bayat satır). Liste
// yalnız kısalır.
#include <algorithm>
#include <string_view>

namespace noteapp::test {

enum class CommandUi { Tui, Gui };

struct CommandGap {
    CommandUi ui;
    std::string_view id;
    std::string_view owner;  // plan-parity görevi
};

inline constexpr CommandGap kCommandGaps[] = {
    // ---- TUI'de yok ----
    {CommandUi::Tui, "view.map", "P-T4"},      // harita
    {CommandUi::Tui, "tab.new", "P-T3"},       // sekmeler
    {CommandUi::Tui, "tab.close", "P-T3"},
    {CommandUi::Tui, "tab.next", "P-T3"},
    {CommandUi::Tui, "tab.previous", "P-T3"},
    // ---- GUI'de yok ----
    {CommandUi::Gui, "find", "P-G5"},      // not içi bul (G5a)
    {CommandUi::Gui, "find.next", "P-G5"},
    {CommandUi::Gui, "find.previous", "P-G5"},
};

// Bekçinin kararı: uyumluysa nullptr, değilse ne yapılacağı.
constexpr const char* parity_problem(CommandUi ui, std::string_view id, bool bound) {
    const bool listed = std::ranges::any_of(
        kCommandGaps, [&](const CommandGap& gap) { return gap.ui == ui && gap.id == id; });
    if (bound && listed)
        return "bound now: delete its row from tests/support/command_parity_gaps.hpp";
    if (!bound && !listed)
        return "no binding or action in this UI: bind it, or add an owned row to command_parity_gaps.hpp";
    return nullptr;
}

}  // namespace noteapp::test
