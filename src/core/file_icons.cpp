#include "core/file_icons.hpp"

#include "core/file_icon_table.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace noteapp::core {

namespace {

FileIcon by_extension(std::string_view extension) {
    const auto* const found =
        std::ranges::find(icon_table::kByExtension, extension, &icon_table::Entry::extension);
    return found == std::end(icon_table::kByExtension) ? icon_table::kDefaultFile : found->icon;
}

}  // namespace

FileIcon file_icon(std::string_view name) {
    name = name.substr(name.rfind('/') + 1);
    std::string lowered(name);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return std::tolower(c); });
    const std::size_t dot = lowered.rfind('.');
    if (dot != std::string::npos)
        return by_extension(std::string_view(lowered).substr(dot + 1));
    // Uzantısız adlardan yalnız yerleşik iki derleme dosyası tanınır; "md" adlı dosya markdown sayılmaz.
    return lowered == "dockerfile" || lowered == "makefile" ? by_extension(lowered)
                                                            : icon_table::kDefaultFile;
}

FileIcon folder_icon(bool is_open) {
    return is_open ? icon_table::kFolderOpen : icon_table::kFolderClosed;
}

FileIcon trash_icon() {
    return icon_table::kTrash;
}

std::string icon_text(const FileIcon& icon, IconStyle style) {
    return style == IconStyle::Nerd ? encode_utf8(std::u32string_view(&icon.nerd, 1)) : std::string{};
}

IconStyle terminal_icon_style(std::string_view setting) {
    return setting == "off" ? IconStyle::Off : IconStyle::Nerd;
}

}  // namespace noteapp::core
