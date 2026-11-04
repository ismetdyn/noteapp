#pragma once

#include "app/graph_layout.hpp"

#include <span>

namespace noteapp::tui {

// Harita braille tuvaline çizilir: hücre 2x4 noktadır (FTXUI Canvas).
inline constexpr int kMapDotsPerCellX = 2;
inline constexpr int kMapDotsPerCellY = 4;

// Haritanın kamerası: yerleşim birimi -> tuval noktası. `x`, `y` öteleme (nokta), `zoom` birim başına nokta.
struct MapCamera {
    float x = 0.0F;
    float y = 0.0F;
    float zoom = 1.0F;
};

// Tüm düğümler `dots_x` x `dots_y` tuvale payla sığar (GUI'nin `0`ı); düğüm yoksa varsayılan kamera.
MapCamera fit_map_camera(std::span<const app::LayoutPoint> positions, int dots_x, int dots_y);

}  // namespace noteapp::tui
