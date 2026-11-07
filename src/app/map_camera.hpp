#pragma once

#include "app/graph_layout.hpp"

#include <span>

namespace noteapp::app {

// Haritanın kamerası (TUI braille tuvali, GUI ImGui tuvali): tuval noktası = (x, y) + yerleşim konumu * zoom.
// Birim arayüzündür (TUI braille noktası, GUI piksel); sınırlar ve paylar arayüzden gelir.
struct MapCamera {
    struct ZoomRange {
        float min = 0.0F;
        float max = 0.0F;
    };

    float x = 0.0F;
    float y = 0.0F;
    float zoom = 1.0F;

    // Tüm düğümler `width` x `height` tuvale her yandan `margin` payla sığar; düğümün kendisi `node_radius`
    // (yerleşim birimi) kadar yer tutar. Düğüm yoksa varsayılan kamera.
    static MapCamera fit(std::span<const LayoutPoint> positions, float width, float height, float margin,
                         float node_radius, ZoomRange range);
    // Yakınlık `factor` kat değişir, `range`e kırpılır; tuvaldeki (anchor_x, anchor_y) altındaki yerleşim
    // noktası yerinde kalır.
    void zoom_at(float anchor_x, float anchor_y, float factor, ZoomRange range);
};

}  // namespace noteapp::app
