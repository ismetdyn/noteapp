#pragma once

#include <imgui.h>
#include <optional>
#include <string_view>
#include <vector>

namespace noteapp::gui {

// Yalnız notes-gui'ye gömülü ikon fontu (resources/fonts/NoteappIcons.ttf); `note` ve `notes` bu veriyi
// taşımaz.
std::optional<std::string_view> embedded_icon_font(std::string_view relative_path);
// Yalnız notes-gui'ye gömülü seçilebilir yazı aileleri (Inter, JetBrains Mono; Ayarlar → Görünüm).
std::optional<std::string_view> embedded_ui_font(std::string_view relative_path);

// İkon tablosundaki her kod noktası için tek elemanlı ImGui aralıkları; sonda 0. Sıralı ve yinelenmesiz.
std::vector<ImWchar> icon_glyph_ranges();

}  // namespace noteapp::gui
