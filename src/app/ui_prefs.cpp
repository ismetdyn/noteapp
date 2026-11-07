#include "app/ui_prefs.hpp"

#include "app/commands.hpp"
#include "app/listing.hpp"
#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <toml++/toml.hpp>

namespace noteapp::app {

namespace {

constexpr std::string_view kSort = "sort";
constexpr std::string_view kSortDescending = "sort_descending";
constexpr std::string_view kColumn = "column";
constexpr std::string_view kSearchHistory = "search_history";
constexpr std::string_view kOpenFolders = "open_folders";
constexpr std::string_view kRecentCommands = "recent_commands";
constexpr std::string_view kActiveTab = "active_tab";
constexpr std::string_view kTabs = "tabs";
constexpr std::array kCommonKeys = {kSort,           kSortDescending, kColumn,   kSearchHistory,
                                    kOpenFolders,    kRecentCommands, kActiveTab, kTabs};

template <typename Table, typename Value>
void value_of(const Table& table, const std::optional<std::string>& name, Value& out) {
    if (!name)
        return;
    const auto found = std::ranges::find(table, *name, [](const auto& entry) { return entry.second; });
    if (found != table.end())
        out = found->first;
    else
        core::log_warn("event=ui_state_value_ignored key=sort_or_column");
}

template <typename Table, typename Value> std::string_view name_of(const Table& table, Value value) {
    return std::ranges::find(table, value, [](const auto& entry) { return entry.first; })->second;
}

std::vector<std::string> strings(const toml::table& table, std::string_view key) {
    std::vector<std::string> out;
    if (const toml::array* array = table[key].as_array())
        for (const toml::node& node : *array)
            if (auto text = node.value<std::string>())
                out.push_back(std::move(*text));
    return out;
}

toml::array array_of(const auto& values) {
    toml::array array;
    for (const std::string& value : values)
        array.push_back(value);
    return array;
}

SavedTabs read_tabs(const toml::table& table) {
    SavedTabs saved;
    const auto active = table[kActiveTab].value_or(std::int64_t{0});
    saved.active = active < 0 ? 0 : static_cast<std::size_t>(active);
    if (const toml::array* tabs = table[kTabs].as_array())
        for (const toml::node& node : *tabs)
            if (const toml::table* entry = node.as_table())
                if (auto path = (*entry)["path"].value<std::string>())
                    saved.tabs.push_back(
                        {std::move(*path), (*entry)["pinned"].value_or(false), (*entry)["kept"].value_or(false)});
    return saved;
}

void read_extra(const toml::table& table, UiPrefs& prefs) {
    for (const auto& [key, node] : table) {
        if (std::ranges::find(kCommonKeys, key.str()) != kCommonKeys.end())
            continue;
        std::string name(key.str());
        if (const auto flag = node.value_exact<bool>())
            prefs.extra.emplace(std::move(name), *flag);
        else if (const auto whole = node.value_exact<std::int64_t>())
            prefs.extra.emplace(std::move(name), *whole);
        else if (const auto number = node.value_exact<double>())
            prefs.extra.emplace(std::move(name), *number);
        else if (auto text = node.value_exact<std::string>())
            prefs.extra.emplace(std::move(name), std::move(*text));
    }
}

std::filesystem::path state_file(std::string_view file_name) {
    return core::state_dir() / std::filesystem::path(file_name);
}

template <typename T> const T* extra_of(const UiPrefs& prefs, std::string_view key) {
    const auto found = prefs.extra.find(key);
    return found == prefs.extra.end() ? nullptr : std::get_if<T>(&found->second);
}

}  // namespace

namespace {

UiPrefs from_table(const toml::table& table, const UiPrefsRules& rules) {
    UiPrefs prefs;
    value_of(kSortFieldNames, table[kSort].value<std::string>(), prefs.sort.field);
    prefs.sort.descending = table[kSortDescending].value_or(prefs.sort.descending);
    value_of(kListColumnNames, table[kColumn].value<std::string>(), prefs.column);
    prefs.search_history = strings(table, kSearchHistory);
    if (rules.keep_history)
        prefs.search_history = rules.keep_history(std::move(prefs.search_history));
    std::ranges::move(strings(table, kOpenFolders), std::inserter(prefs.open_folders, prefs.open_folders.end()));
    for (std::string& id : strings(table, kRecentCommands))
        if (find_command(id) != nullptr && prefs.recent_commands.size() < rules.recent_commands.cap())
            prefs.recent_commands.push_back(std::move(id));
    prefs.tabs = read_tabs(table);
    read_extra(table, prefs);
    return prefs;
}

// Okunamayan dosya ezilmesin: tek yedek (`.bad`) eskisinin yerine geçer, birikmez.
std::string set_aside(const std::filesystem::path& file, const std::string& detail) {
    std::filesystem::path backup = file;
    backup += ".bad";
    std::error_code ec;
    std::filesystem::remove(backup, ec);  // Windows'ta rename hedefi ezmez
    std::filesystem::rename(file, backup, ec);
    if (ec) {
        core::log_warn("event=ui_state_set_aside_failed detail=" + ec.message());
        return core::tr("app.ui_state_unreadable", {{"file", file.filename().string()}, {"detail", detail}});
    }
    core::log_warn("event=ui_state_set_aside detail=" + detail);
    return core::tr("app.ui_state_set_aside", {{"file", file.filename().string()},
                                               {"backup", backup.filename().string()},
                                               {"detail", detail}});
}

}  // namespace

UiPrefs parse_ui_prefs(std::string_view text, const UiPrefsRules& rules) {
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        core::log_warn("event=ui_state_unreadable line=" + std::to_string(parsed.error().source().begin.line));
        return UiPrefs{};
    }
    return from_table(parsed.table(), rules);
}

std::function<bool(const std::string&)> open_folder_filter(const core::FolderTree& tree) {
    const core::FolderNode* root = tree.find("");
    if (root == nullptr || root->item_count() == 0)
        return {};
    return [&tree](const std::string& folder) { return tree.find(folder) != nullptr; };
}

std::string format_ui_prefs(const UiPrefs& prefs, const std::function<bool(const std::string&)>& folder_exists) {
    toml::table table;
    table.insert(kSort, name_of(kSortFieldNames, prefs.sort.field));
    table.insert(kSortDescending, prefs.sort.descending);
    table.insert(kColumn, name_of(kListColumnNames, prefs.column));
    table.insert(kSearchHistory, array_of(prefs.search_history));
    // Denetim baytlı ad elle düzenlemede satırı bozar; ağaçta olmayan klasör (silindi, taşındı) taşınmaz.
    std::vector<std::string> open;
    for (const std::string& folder : prefs.open_folders)
        if (std::ranges::none_of(folder, [](unsigned char c) { return c < 0x20 || c == 0x7F; }) &&
            (!folder_exists || folder_exists(folder)))
            open.push_back(folder);
    table.insert(kOpenFolders, array_of(open));
    table.insert(kRecentCommands, array_of(prefs.recent_commands));
    toml::array tabs;
    for (const SavedTab& tab : prefs.tabs.tabs)
        tabs.push_back(toml::table{{"path", tab.path}, {"pinned", tab.pinned}, {"kept", tab.kept}});
    table.insert(kActiveTab, static_cast<std::int64_t>(prefs.tabs.active));
    table.insert(kTabs, std::move(tabs));
    for (const auto& [key, value] : prefs.extra)
        if (std::ranges::find(kCommonKeys, key) == kCommonKeys.end())
            std::visit([&table, &key](const auto& held) { table.insert(key, held); }, value);
    std::ostringstream out;
    out << table;
    return out.str() + "\n";
}

LoadedUiPrefs load_ui_prefs(std::string_view file_name, const UiPrefsRules& rules) {
    const std::filesystem::path file = state_file(file_name);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec)
        return {};  // ilk çalıştırma
    // Var olan ama okunamayan dosya (izin, dizin, G/Ç) da sonraki kayıtla ezilmesin: kenara alınır.
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec)
        return {.prefs = {}, .notice = set_aside(file, ec.message())};
    if (!rules.max_bytes.allows(size))
        return {.prefs = {}, .notice = set_aside(file, core::tr("app.ui_state_too_large",
                                                                {{"limit", rules.max_bytes.text}}))};
    std::ifstream in(file, std::ios::binary);  // fs::path: Windows'ta Türkçe yol da açılır
    if (!in)
        return {.prefs = {}, .notice = set_aside(file, std::error_code(errno, std::generic_category()).message())};
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    in.close();
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::source_position at = parsed.error().source().begin;
        return {.prefs = {},
                .notice = set_aside(file, std::to_string(at.line) + ":" + std::to_string(at.column) + " " +
                                              std::string(parsed.error().description()))};
    }
    return {.prefs = from_table(parsed.table(), rules)};
}

void save_ui_prefs(std::string_view file_name, const UiPrefs& prefs,
                   const std::function<bool(const std::string&)>& folder_exists) {
    std::error_code ignored;  // yazma aşağıda aynı nedenle başarısız olur ve loglanır
    std::filesystem::create_directories(core::state_dir(), ignored);
    constexpr bool kOwnerOnly = true;
    if (const auto written =
            core::write_file_atomically(state_file(file_name), format_ui_prefs(prefs, folder_exists), kOwnerOnly);
        !written)
        core::log_warn("event=ui_state_write_failed detail=" + core::format_error(written.error()));
}

std::optional<bool> pref_bool(const UiPrefs& prefs, std::string_view key) {
    const bool* value = extra_of<bool>(prefs, key);
    return value ? std::optional(*value) : std::nullopt;
}

std::optional<std::int64_t> pref_int(const UiPrefs& prefs, std::string_view key) {
    const std::int64_t* value = extra_of<std::int64_t>(prefs, key);
    return value ? std::optional(*value) : std::nullopt;
}

std::optional<double> pref_number(const UiPrefs& prefs, std::string_view key) {
    if (const double* value = extra_of<double>(prefs, key))
        return *value;
    if (const std::int64_t* whole = extra_of<std::int64_t>(prefs, key))
        return static_cast<double>(*whole);
    return std::nullopt;
}

std::optional<std::string> pref_string(const UiPrefs& prefs, std::string_view key) {
    const std::string* value = extra_of<std::string>(prefs, key);
    return value ? std::optional(*value) : std::nullopt;
}

}  // namespace noteapp::app
