#pragma once

#include <string>
#include <string_view>

namespace noteapp::core {

enum class IconStyle { Off, Nerd };

enum class IconColor { Default, Blue, Cyan, Green, Yellow, Red, Magenta };

struct FileIcon {
    char32_t nerd{};  // Nerd Fonts v3.5.1 kod noktası; tablo o sürümün cmap'ine karşı doğrulandı
    IconColor color{};
};

// `name` yol da olabilir; yalnız son parça bakılır. Eşleşme büyük/küçük harf duyarsız; bilinmeyen →
// varsayılan.
FileIcon file_icon(std::string_view name);
FileIcon folder_icon(bool is_open);
FileIcon trash_icon();

// Stile göre UTF-8 metin; Off'ta boş.
std::string icon_text(const FileIcon& icon, IconStyle style);

// `off` dışındaki her değer Nerd Font'tur (`auto` eski addır): terminalin fontu sorulamaz, font beklenir.
IconStyle terminal_icon_style(std::string_view setting);

}  // namespace noteapp::core
