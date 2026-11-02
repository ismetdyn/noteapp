#include "gui/icon_font.hpp"

#include "core/file_icon_table.hpp"

#include <algorithm>

namespace noteapp::gui {

std::vector<ImWchar> icon_glyph_ranges() {
    namespace table = core::icon_table;
    std::vector<ImWchar> points = {table::kDefaultFile.nerd, table::kFolderClosed.nerd,
                                   table::kFolderOpen.nerd, table::kTrash.nerd};
    for (const table::Entry& entry : table::kByExtension)
        points.push_back(entry.icon.nerd);
    std::ranges::sort(points);
    const auto duplicates = std::ranges::unique(points);
    points.erase(duplicates.begin(), duplicates.end());

    std::vector<ImWchar> ranges;
    ranges.reserve(points.size() * 2 + 1);
    for (const ImWchar point : points) {
        ranges.push_back(point);
        ranges.push_back(point);
    }
    ranges.push_back(0);
    return ranges;
}

}  // namespace noteapp::gui
