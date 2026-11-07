#pragma once

// gui_state*.cpp dosyalarının ortak iç yardımcıları. Yalnız src/gui/gui_state*.cpp içerir.

#include "app/tabs.hpp"
#include "core/text.hpp"

#include <string>
#include <string_view>

namespace noteapp::gui {

// Taslakta kaybedilecek metin var mı (kapanış sorusu, kaydedilmemiş işareti, kayıt akışı, dışa aktarma):
// sekmesini koruyan taslak, TUI ile tek kural (app::keeps_tab). Yalnız boşluk da olsa yazılmış metin sessizce
// atılmaz; boş taslak sorulmadan kapanır.
inline bool draft_needs_answer(std::string_view text) {
    return app::keeps_tab(/*pinned=*/false, /*kept=*/false, /*is_draft=*/true, text);
}

}  // namespace noteapp::gui
