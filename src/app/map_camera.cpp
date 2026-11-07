#include "app/map_camera.hpp"

#include <algorithm>

namespace noteapp::app {

MapCamera MapCamera::fit(std::span<const LayoutPoint> positions, float width, float height, float margin,
                         float node_radius, ZoomRange range) {
    if (positions.empty())
        return {};
    LayoutPoint low = positions.front();
    LayoutPoint high = positions.front();
    for (const LayoutPoint p : positions) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
    }
    low = {low.x - node_radius, low.y - node_radius};
    high = {high.x + node_radius, high.y + node_radius};
    const float room_x = std::max(width - 2.0F * margin, 1.0F);
    const float room_y = std::max(height - 2.0F * margin, 1.0F);
    const float span_x = std::max(high.x - low.x, GraphLayout::kIdealEdge);
    const float span_y = std::max(high.y - low.y, GraphLayout::kIdealEdge);
    MapCamera camera;
    camera.zoom = std::clamp(std::min(room_x / span_x, room_y / span_y), range.min, range.max);
    camera.x = width / 2.0F - (low.x + high.x) / 2.0F * camera.zoom;
    camera.y = height / 2.0F - (low.y + high.y) / 2.0F * camera.zoom;
    return camera;
}

void MapCamera::zoom_at(float anchor_x, float anchor_y, float factor, ZoomRange range) {
    const float next = std::clamp(zoom * factor, range.min, range.max);
    const float world_x = (anchor_x - x) / zoom;
    const float world_y = (anchor_y - y) / zoom;
    x = anchor_x - world_x * next;
    y = anchor_y - world_y * next;
    zoom = next;
}

}  // namespace noteapp::app
