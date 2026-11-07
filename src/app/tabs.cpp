#include "app/tabs.hpp"

#include "core/folder_tree.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace noteapp::app {

RestoredTabs restorable_tabs(const SavedTabs& saved, const std::function<bool(const core::NotePath&)>& exists,
                             const core::Limit& limit) {
    RestoredTabs restored;
    std::optional<std::size_t> active;
    for (std::size_t index = 0; index < saved.tabs.size() && can_add_tab(restored.tabs.size(), limit);
         ++index) {
        const SavedTab& entry = saved.tabs[index];
        const auto path = core::NotePath::parse(entry.path);
        if (!path || !exists(*path) ||
            std::ranges::any_of(restored.tabs, [&](const RestoredTab& tab) { return tab.path == *path; }))
            continue;  // elle bozulmuş, silinmiş, taşınmış ya da yinelenen girdi
        if (index == saved.active)
            active = restored.tabs.size();
        restored.tabs.push_back({*path, entry.pinned, entry.kept});
    }
    restored.active = active.value_or(0);
    return restored;
}

bool keeps_tab(bool pinned, bool kept, bool is_draft, std::string_view draft_text) {
    return is_draft ? !draft_text.empty() : pinned || kept;
}

bool can_add_tab(std::size_t count, const core::Limit& limit) {
    return count < std::numeric_limits<std::size_t>::max() && limit.allows(count + 1);
}

std::size_t cycled_tab(std::size_t index, int delta, std::size_t count) {
    if (count == 0)
        return 0;
    const auto size = static_cast<long long>(count);
    const long long target = ((static_cast<long long>(index) + delta) % size + size) % size;
    return static_cast<std::size_t>(target);
}

std::size_t neighbour_after_close(std::size_t index, std::size_t remaining) {
    return remaining == 0 ? 0 : std::min(index, remaining - 1);
}

std::optional<core::NotePath> moved_path(const core::NotePath& path, std::string_view from,
                                         std::string_view to) {
    const std::string current = path.string();
    if (current != from && !core::in_folder(current, from))
        return std::nullopt;
    auto moved = core::NotePath::parse(std::string(to) + current.substr(from.size()));
    return moved ? std::optional(std::move(*moved)) : std::nullopt;
}

}  // namespace noteapp::app
