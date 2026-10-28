#pragma once

#include <ftxui/dom/elements.hpp>

namespace noteapp::tui {

// SPEC-tui yerleşiminin (sorgu çubuğu, iki panel, kısayol çubuğu) şimdilik statik hali.
// T18+ bunu saf TuiState'ten beslenen gerçek görünüme dönüştürür; imza o zaman durumu parametre alır.
ftxui::Element main_view();

}  // namespace noteapp::tui
