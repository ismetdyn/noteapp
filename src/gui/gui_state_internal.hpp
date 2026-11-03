#pragma once

// gui_state*.cpp dosyalarının ortak iç yardımcıları. Yalnız src/gui/gui_state*.cpp içerir.

#include "core/text.hpp"

#include <string>

namespace noteapp::gui {

// Tampon yalnız boşluktan ibaret mi: boş taslak sorulmadan atılır, kaybedilecek bir şey yok.
inline bool is_blank(const std::string& text) {
    return core::first_non_empty_line(text).empty();
}

}  // namespace noteapp::gui
