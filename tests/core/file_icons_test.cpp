#include "core/file_icons.hpp"
#include "core/text.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp::core;

namespace {

struct Expected {
    const char* name;
    char32_t nerd;
};

// Kod noktaları Nerd Fonts v3.5.1'in SymbolsNerdFontMono cmap'inde bulundu (SPEC-tui "Dosya ikonları").
constexpr Expected kCovered[] = {
    {"a.md", 0xF0354},      {"a.txt", 0xF0219},   {"a.csv", 0xF0218},       {"a.json", 0xE60B},
    {"a.yaml", 0xE6A8},     {"a.yml", 0xE6A8},    {"a.toml", 0xE615},       {"a.ini", 0xE615},
    {"a.env", 0xF107B},     {".env", 0xF107B},    {"a.properties", 0xE615}, {"a.xml", 0xE619},
    {"a.html", 0xE60E},     {"a.css", 0xE614},    {"a.js", 0xE60C},         {"a.ts", 0xE628},
    {"a.c", 0xE649},        {"a.cpp", 0xE646},    {"a.h", 0xE649},          {"a.hpp", 0xE646},
    {"a.java", 0xE66D},     {"a.py", 0xE606},     {"a.sh", 0xE691},         {"a.go", 0xE627},
    {"a.rs", 0xE68B},       {"a.sql", 0xE64D},    {"a.dockerfile", 0xE650}, {"Dockerfile", 0xE650},
    {"a.makefile", 0xE673}, {"Makefile", 0xE673}, {"a.pdf", 0xE67D},        {"a.png", 0xE60D},
    {"a.jpg", 0xE60D},      {"a.jpeg", 0xE60D},   {"a.svg", 0xE698},        {"a.zip", 0xE6AA},
    {"a.tar", 0xE6AA},      {"a.gz", 0xE6AA},     {"a.tar.gz", 0xE6AA},
};
constexpr char32_t kDefaultFile = 0xF0214;

}  // namespace

TEST_CASE("every covered file type maps to its verified Nerd Font code point") {
    for (const Expected& expected : kCovered) {
        CAPTURE(expected.name);
        const FileIcon icon = file_icon(expected.name);
        CHECK(static_cast<std::uint32_t>(icon.nerd) == static_cast<std::uint32_t>(expected.nerd));
    }
}

TEST_CASE("file_icon looks only at the last path part and ignores case") {
    CHECK(file_icon("work/notes/A.MD").nerd == 0xF0354);
    CHECK(file_icon("build/DOCKERFILE").nerd == 0xE650);
    CHECK(file_icon("md/readme").nerd == kDefaultFile);  // klasör adı uzantı sayılmaz
}

TEST_CASE("unknown extensions and names without one get the default file icon") {
    for (const char* name : {"a.unknown", "notes", "a.", ""}) {
        CAPTURE(name);
        CHECK(file_icon(name).nerd == kDefaultFile);
    }
}

TEST_CASE("folders and the trash have their own icons") {
    CHECK(folder_icon(false).nerd == 0xF024B);
    CHECK(folder_icon(true).nerd == 0xF0770);
    CHECK(trash_icon().nerd == 0xF0A79);
    CHECK(folder_icon(false).color == IconColor::Blue);
    CHECK(folder_icon(true).color == IconColor::Blue);
}

TEST_CASE("icon_text follows the style") {
    const FileIcon icon = file_icon("a.md");
    CHECK(icon_text(icon, IconStyle::Off).empty());
    CHECK(icon_text(icon, IconStyle::Nerd) == "\xF3\xB0\x8D\x94");             // U+F0354
    CHECK(icon_text(file_icon("a.json"), IconStyle::Nerd) == "\xEE\x98\x8B");  // U+E60B
}

TEST_CASE("the terminal style never assumes a Nerd Font on auto") {
    CHECK(terminal_icon_style("auto") == IconStyle::Nerd);
    CHECK(terminal_icon_style("nerd") == IconStyle::Nerd);
    CHECK(terminal_icon_style("off") == IconStyle::Off);
}

TEST_CASE("common types have distinct colors, like yazi; every color is one of the 16 ANSI set") {
    const IconColor md = file_icon("a.md").color;
    const IconColor cpp = file_icon("a.cpp").color;
    const IconColor py = file_icon("a.py").color;
    const IconColor json = file_icon("a.json").color;
    const IconColor folder = folder_icon(false).color;
    const std::vector<IconColor> colors = {md, cpp, py, json, folder};
    for (std::size_t i = 0; i < colors.size(); ++i)
        for (std::size_t j = i + 1; j < colors.size(); ++j)
            CHECK(colors[i] != colors[j]);
}
