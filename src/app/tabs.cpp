#include "app/tabs.hpp"

#include "core/folder_tree.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <toml++/toml.hpp>
#include <utility>

namespace noteapp::app {

namespace {

constexpr std::string_view kActiveKey = "active_tab";
constexpr std::string_view kTabsKey = "tabs";

// `active_tab` satırının başı; yoksa npos. Yalnız satır başı sayılır: arama geçmişindeki bir dizgi
// eşleşmesin.
std::size_t active_line(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        if (text.substr(at).starts_with(kActiveKey))
            return at;
        const std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return std::string_view::npos;
}

}  // namespace

std::string format_saved_tabs(const SavedTabs& saved) {
    if (saved.tabs.empty())
        return {};
    toml::array tabs;
    for (const SavedTab& tab : saved.tabs)
        tabs.push_back(toml::table{{"path", tab.path}, {"pinned", tab.pinned}, {"kept", tab.kept}});
    // toml++ düz anahtarı tablolardan önce yazar: `active_tab` satırı bloğun başıdır (parse_saved_tabs).
    toml::table table{{kActiveKey, static_cast<std::int64_t>(saved.active)}, {kTabsKey, std::move(tabs)}};
    std::ostringstream out;
    out << table;
    return out.str() + "\n";
}

SavedTabs parse_saved_tabs(std::string_view text) {
    const std::size_t start = active_line(text);
    if (start == std::string_view::npos)
        return {};
    toml::parse_result parsed = toml::parse(text.substr(start));
    if (!parsed)
        return {};
    const toml::table& table = parsed.table();
    SavedTabs saved;
    const auto active = table[kActiveKey].value_or(std::int64_t{0});
    saved.active = active < 0 ? 0 : static_cast<std::size_t>(active);
    if (const toml::array* tabs = table[kTabsKey].as_array()) {
        for (const toml::node& node : *tabs) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;
            if (auto path = (*entry)["path"].value<std::string>())
                saved.tabs.push_back(
                    {std::move(*path), (*entry)["pinned"].value_or(false), (*entry)["kept"].value_or(false)});
        }
    }
    return saved;
}

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
