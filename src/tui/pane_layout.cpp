#include "tui/pane_layout.hpp"

#include <algorithm>

namespace noteapp::tui {

namespace {

int clamped(int width, int window_width) {
    const int maximum = std::max(window_width - PaneLayout::kMinRightWidth, PaneLayout::kMinListWidth);
    return std::clamp(width, PaneLayout::kMinListWidth, maximum);
}

}  // namespace

int PaneLayout::list_width(int window_width) const {
    return clamped(width_.value_or(static_cast<int>(window_width * share_)), window_width);
}

void PaneLayout::set_list_width(int width, int window_width) {
    width_ = clamped(width, window_width);
}

std::optional<double> PaneLayout::chosen_share(int window_width) const {
    if (!width_ || window_width <= 0)
        return std::nullopt;
    // Sütunun ortası: list_width oranı tamsayıya keser, kayan nokta hatası aynı pencerede sütun kaydırmaz.
    constexpr double kColumnMiddle = 0.5;
    return std::min((list_width(window_width) + kColumnMiddle) / window_width, 1.0);
}

void PaneLayout::resize(int delta, int window_width) {
    set_list_width(list_width(window_width) + delta, window_width);
}

PaneGeometry PaneLayout::geometry(int window_width, bool right_focused) const {
    if (zoomed_)
        return right_focused ? PaneGeometry{false, true, 0, 0, window_width}
                             : PaneGeometry{true, false, window_width, window_width, 0};
    const int list = list_width(window_width);
    return {true, true, list, list, window_width - list};
}

bool PaneLayout::on_divider(int x, int window_width) const {
    if (zoomed_)
        return false;
    const int list = list_width(window_width);
    return x == list - 1 || x == list;  // iki panelin bitişik kenarları
}

}  // namespace noteapp::tui
