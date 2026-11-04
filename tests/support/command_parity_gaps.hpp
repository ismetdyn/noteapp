#pragma once

// A1 eşitlik bekçisinin izin listesi: app::kCommandRegistry'de olup bir arayüzde bağı/eylemi olmayan
// komutlar. Her satırın sahibi plan-parity görevidir (P-<görev>); görev kapanınca satır silinir. Bekçi iki
// yönde düşer: listede olmayan eksik ve listede kaldığı hâlde artık bağlı olan komut (bayat satır). Liste
// yalnız kısalır.
#include <algorithm>
#include <array>
#include <string_view>

namespace noteapp::test {

enum class CommandUi { Tui, Gui };

struct CommandGap {
    CommandUi ui;
    std::string_view id;
    std::string_view owner;  // plan-parity görevi
};

// T3, T4 ve G5'ten sonra iki arayüz de kaydın tamamını bağlar: liste boştur. Satır gerekirse dizinin boyu
// satır sayısıdır (`std::array<CommandGap, N>`); bayat satırı bekçi yakalar.
inline constexpr std::array<CommandGap, 0> kCommandGaps{};

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
