#pragma once

#include <cstdint>
#include <vector>

namespace noteapp::pdfview {

// Çizilmiş sayfa: satır satır RGBA (satır adımı width * 4), düz (önceden çarpılmamış) alfa; sayfa beyaz zemine
// çizildiğinden alfa her pikselde 255'tir.
struct RenderedPage {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

}  // namespace noteapp::pdfview
