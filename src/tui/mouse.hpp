#pragma once

#include "edit/editor_session.hpp"
#include "tui/tui_state.hpp"

namespace noteapp::tui {

enum class MouseButton { Left, Right, WheelUp, WheelDown, WheelLeft, WheelRight, Other };
enum class MouseMotion { Press, Release, Move };

struct MouseInput {
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Other;
    MouseMotion motion = MouseMotion::Press;
    bool shift = false;  // çoğu terminal Shift+fareyi kendi seçimine ayırır, uygulamaya gelmez
};

enum class MouseResult { None, SelectionFinished, OpenTrash, SavePrefs, FollowLink, MapEdit, PdfPageNoText };

// Fare olayını modele uygular (FTXUI bilmez): bölücü sürükleme, işaretçinin altındaki paneli kaydırma, satır
// seçme, panele odaklanma. Editörden çıkan her yol Esc ile aynı kapıdan geçer: kaydedilmemiş metin yine
// sorulur. REVIEW'da sürükleme metin seçer; `SelectionFinished` seçimin panoya kopyalanacağını söyler. Üst
// satırdaki çöp göstergesine tıklama `OpenTrash` döner: çöpü listelemek modelin işi değil. REVIEW'da
// sürüklenmeyen tıklama bir [[bağlantı]] üstündeyse onu seçer ve `FollowLink` döner: depo çözer. Liste
// satırına sağ tık o satırın bağlam menüsünü açar (TuiState::open_row_menu). Haritada (MAP-R) düğümden sağ
// sürükleme (terminal geçirirse Shift+sol da) başka düğüme ilişki kurar, kenara sağ tık onu seçer; doğrudan
// yazılacak iş `MapEdit` döner. PDF sayfa görünümünde seçim yoktur: tıklama odaklar, `PdfPageNoText` döner.
MouseResult route_mouse(const MouseInput& input, TuiState& state, EditorSession& editor);

}  // namespace noteapp::tui
