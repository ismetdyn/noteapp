#pragma once

#include "core/file_icons.hpp"

#include <string_view>

// İkon tablosu, tek kaynak. GUI buradan atlas aralığını, scripts/generate-icon-font.sh
// buradan alt küme fontunu üretir; `note` ve `notes` tablodaki kullanmadıkları sabitler için büyümez.
namespace noteapp::core::icon_table {

inline constexpr FileIcon kDefaultFile{0xF0214, IconColor::Default};
inline constexpr FileIcon kFolderClosed{0xF024B, IconColor::Blue};
inline constexpr FileIcon kFolderOpen{0xF0770, IconColor::Blue};
inline constexpr FileIcon kTrash{0xF0A79, IconColor::Default};

struct Entry {
    std::string_view extension;
    FileIcon icon{};
};

// Kod noktaları Material Design (Apache-2.0) ve Seti UI (MIT) kümelerinden; gömülen 30 glifin lisansı
// resources/fonts/LICENSE-NoteappIcons.txt. Yeni kod noktası fontu yeniden üretmeyi gerektirir.
inline constexpr Entry kByExtension[] = {
    {"md", {0xF0354, IconColor::Cyan}},          {"markdown", {0xF0354, IconColor::Cyan}},
    {"txt", {0xF0219, IconColor::Default}},      {"csv", {0xF0218, IconColor::Green}},
    {"json", {0xE60B, IconColor::Yellow}},       {"yaml", {0xE6A8, IconColor::Magenta}},
    {"yml", {0xE6A8, IconColor::Magenta}},       {"xml", {0xE619, IconColor::Cyan}},
    {"toml", {0xE615, IconColor::Yellow}},       {"ini", {0xE615, IconColor::Yellow}},
    {"properties", {0xE615, IconColor::Yellow}}, {"env", {0xF107B, IconColor::Yellow}},
    {"html", {0xE60E, IconColor::Red}},          {"css", {0xE614, IconColor::Blue}},
    {"js", {0xE60C, IconColor::Yellow}},         {"ts", {0xE628, IconColor::Blue}},
    {"c", {0xE649, IconColor::Magenta}},         {"h", {0xE649, IconColor::Magenta}},
    {"cpp", {0xE646, IconColor::Magenta}},       {"hpp", {0xE646, IconColor::Magenta}},
    {"java", {0xE66D, IconColor::Red}},          {"py", {0xE606, IconColor::Green}},
    {"go", {0xE627, IconColor::Cyan}},           {"rs", {0xE68B, IconColor::Red}},
    {"sh", {0xE691, IconColor::Green}},          {"dockerfile", {0xE650, IconColor::Blue}},
    {"makefile", {0xE673, IconColor::Red}},      {"sql", {0xE64D, IconColor::Cyan}},
    {"pdf", {0xE67D, IconColor::Red}},           {"png", {0xE60D, IconColor::Magenta}},
    {"jpg", {0xE60D, IconColor::Magenta}},       {"jpeg", {0xE60D, IconColor::Magenta}},
    {"svg", {0xE698, IconColor::Yellow}},        {"zip", {0xE6AA, IconColor::Yellow}},
    {"tar", {0xE6AA, IconColor::Yellow}},        {"gz", {0xE6AA, IconColor::Yellow}},
};

}  // namespace noteapp::core::icon_table
