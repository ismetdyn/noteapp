#include "tui/list_actions.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "tui/display_text.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kPrefsFile = "tui-state.toml";
constexpr std::array<std::pair<search::SortField, std::string_view>, 4> kSortNames = {{
    {search::SortField::Name, "name"},
    {search::SortField::Size, "size"},
    {search::SortField::Updated, "updated"},
    {search::SortField::Created, "created"},
}};
constexpr std::string_view kPrefsVersion = "2";
constexpr std::array<std::pair<ListColumn, std::string_view>, 7> kColumnNames = {{
    {ListColumn::SizeAndDate, "size_and_date"},
    {ListColumn::None, "none"},
    {ListColumn::Size, "size"},
    {ListColumn::Updated, "updated"},
    {ListColumn::Created, "created"},
    {ListColumn::Sync, "sync"},
    {ListColumn::Origin, "origin"},
}};

template <typename Table, typename Value> std::string_view name_of(const Table& table, Value value) {
    const auto found = std::ranges::find(table, value, [](const auto& entry) { return entry.first; });
    return found == table.end() ? std::string_view{} : found->second;
}

template <typename Table, typename Value>
void value_of(const Table& table, std::string_view name, Value& out) {
    const auto found = std::ranges::find(table, name, [](const auto& entry) { return entry.second; });
    if (found != table.end())
        out = found->first;
    else
        core::log_warn("event=tui_state_value_ignored value=" + terminal_safe(name));
}

std::filesystem::path prefs_path() {
    return core::state_dir() / std::filesystem::path(kPrefsFile);
}

std::string_view unquoted(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\r\"");
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\"") - first + 1);
}

}  // namespace

std::string format_list_prefs(const ListPrefs& prefs) {
    return "sort = \"" + std::string(name_of(kSortNames, prefs.sort.field)) +
           "\"\nsort_descending = " + (prefs.sort.descending ? "true" : "false") + "\ncolumn = \"" +
           std::string(name_of(kColumnNames, prefs.column)) + "\"\nversion = " + std::string(kPrefsVersion) +
           "\n";
}

ListPrefs parse_list_prefs(std::string_view text) {
    ListPrefs prefs;
    bool versioned = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = unquoted(line.substr(0, equals));
        const std::string_view value = unquoted(line.substr(equals + 1));
        if (key == "sort")
            value_of(kSortNames, value, prefs.sort.field);
        else if (key == "sort_descending")
            prefs.sort.descending = value == "true";
        else if (key == "column")
            value_of(kColumnNames, value, prefs.column);
        else if (key == "version")
            versioned = true;
    }
    if (!versioned && prefs.column == ListColumn::None)
        prefs.column = ListColumn::SizeAndDate;
    return prefs;
}

void load_list_prefs(TuiState& state) {
    std::ifstream in(prefs_path(), std::ios::binary);
    if (!in)
        return;  // ilk çalıştırma
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    state.set_list_prefs(parse_list_prefs(text));
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm) {
    switch (intent) {
    case Intent::CopyNote:
        copy_selected_note();
        break;
    case Intent::TrashSelected:
        trash_selected();
        break;
    case Intent::RestoreLast:
        restore_last();
        break;
    case Intent::RestoreSelected:
        restore_selected();
        break;
    case Intent::ToggleTrash:
        toggle_trash();
        break;
    case Intent::DeleteSelected:
        begin_delete(confirm);
        break;
    case Intent::EmptyTrash:
        begin_empty_trash(confirm);
        break;
    case Intent::SavePrefs:
        save_prefs();
        break;
    default:
        break;
    }
}

void ListActions::on_mouse(const MouseInput& input, EditorSession& editor) {
    switch (route_mouse(input, host_.state, editor)) {
    case MouseResult::SelectionFinished:
        copy_preview_selection();
        break;
    case MouseResult::OpenTrash:
        toggle_trash();
        break;
    case MouseResult::None:
        break;
    }
}

void ListActions::copy_text(std::string text) {
    const SystemClipboard::CopyResult result = host_.system_clipboard.copy(text);
    host_.status.message = copy_message(result, text.size());
    if (result != SystemClipboard::CopyResult::Binary)
        host_.clipboard = std::move(text);
}

// Ham baytlar: önizleme arındırılmış olabilir. Terminal ve araçlar ikili veriyi taşıyamaz, neden söylenir.
void ListActions::copy_selected_note() {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    auto content = host_.store->read(row->path);
    if (!content) {
        host_.status.message = core::format_error(content.error());
        return;
    }
    if (core::looks_binary(content->text) || content->text.find('\0') != std::string::npos) {
        host_.status.message = core::tr("tui.copy_refused_binary");
        return;
    }
    if (!core::is_valid_utf8(content->text)) {
        host_.status.message = core::tr("tui.copy_refused_utf8");
        return;
    }
    copy_text(std::move(content->text));
}

void ListActions::copy_preview_selection() {
    const std::size_t shown_generation = host_.preview.generation();
    const Preview& preview = host_.preview.get(*host_.store, host_.state.selected_row());
    const auto selection = host_.state.preview_selection_range();
    if (!selection)
        return;
    // Sürüklerken not değişti: uçlar başka metne ait, yanlış parça kopyalanmasın.
    if (host_.preview.generation() != shown_generation) {
        host_.state.clear_preview_selection();
        return;
    }
    if (preview.display_only) {
        host_.status.message = core::tr("tui.copy_refused_utf8");
        return;
    }
    copy_text(selected_text(preview, *selection));
}

// Onaysız: geri dönüşlüdür (`u`). Seçim komşu satırda kalır.
void ListActions::trash_selected() {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    const core::NotePath path = row->path;
    const std::size_t index = host_.state.selected();
    const auto trashed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->trash(path);
    }();
    if (!trashed) {
        host_.status.message = core::format_error(trashed.error());
        return;
    }
    trashed_.push_back(trashed->path);
    host_.refresh_notes();
    host_.state.select_index(index);
    host_.status.message = core::tr("tui.trashed", {{"path", terminal_safe(path.string())}});
    host_.notify_changed();
}

void ListActions::restore_last() {
    if (trashed_.empty()) {
        host_.status.message = core::tr("tui.nothing_to_restore");
        return;
    }
    const core::NotePath item = trashed_.back();
    const auto restored = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->restore(item);
    }();
    if (!restored) {
        host_.status.message = core::format_error(restored.error());
        return;
    }
    trashed_.pop_back();
    host_.refresh_notes();
    host_.state.select_note(*restored);
    host_.status.message = core::tr("tui.restored", {{"path", terminal_safe(restored->string())}});
    host_.notify_changed();
}

void ListActions::forget_trashed(const core::NotePath& item) {
    std::erase(trashed_, item);
}

void ListActions::restore_selected() {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    const core::NotePath item = row->path;
    const std::size_t index = host_.state.selected();
    const auto restored = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->restore(item);
    }();
    if (!restored) {
        host_.status.message = core::format_error(restored.error());
        return;
    }
    forget_trashed(item);
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    host_.status.message = core::tr("tui.restored", {{"path", terminal_safe(restored->string())}});
    host_.notify_changed();
}

void ListActions::toggle_trash() {
    if (host_.state.trash_view()) {
        host_.state.leave_trash();
        return;
    }
    auto items = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->list_trash();
    }();
    if (!items) {
        host_.status.message = core::format_error(items.error());
        return;
    }
    host_.state.enter_trash(std::move(*items));
}

void ListActions::refresh_trash_view() {
    if (!host_.state.trash_view())
        return;
    auto items = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->list_trash();
    }();
    if (items)
        host_.state.set_trash(std::move(*items));
}

void ListActions::begin_delete(std::optional<ConfirmState>& confirm) {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    delete_target_ = row->path;
    empty_pending_ = false;
    const char* key = host_.state.trash_view() ? "tui.confirm_purge" : "tui.confirm_delete";
    confirm.emplace(core::tr(key, {{"path", terminal_safe(row->path.string())}}));
}

void ListActions::begin_empty_trash(std::optional<ConfirmState>& confirm) {
    delete_target_.reset();
    empty_pending_ = true;
    confirm.emplace(
        core::tr("tui.confirm_empty_trash", {{"count", std::to_string(host_.state.rows().size())}}),
        core::tr("tui.empty_trash_title"));
}

void ListActions::empty_trash() {
    const auto emptied = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->empty_trash();
    }();
    host_.refresh_notes();
    refresh_trash_view();
    if (!emptied) {
        host_.status.message = core::format_error(emptied.error());
        return;
    }
    trashed_.clear();
    host_.status.message = core::tr("tui.trash_emptied");
    host_.notify_changed();
}

void ListActions::finish_delete(std::optional<ConfirmState>& confirm) {
    confirm.reset();
    if (std::exchange(empty_pending_, false)) {
        empty_trash();
        return;
    }
    if (!delete_target_)
        return;
    const core::NotePath target = *std::exchange(delete_target_, std::nullopt);
    const std::size_t index = host_.state.selected();
    const bool from_trash = host_.state.trash_view();
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);  // yeniden tarama iş parçacığı aynı store'u listeler
        return from_trash ? host_.store->purge(target) : host_.store->remove(target);
    }();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    if (!removed) {
        host_.status.message = core::format_error(removed.error());
        return;
    }
    if (from_trash)
        forget_trashed(target);
    host_.status.message = core::tr("tui.deleted", {{"path", terminal_safe(target.string())}});
    host_.notify_changed();
}

void ListActions::begin_rename(std::optional<RenameState>& rename) {
    if (const Row* row = host_.state.selected_row())
        rename.emplace(row->path);
}

void ListActions::handle_rename_key(std::optional<RenameState>& rename, Key key, std::string_view text) {
    switch (rename->handle(key, text)) {
    case RenameState::Outcome::Editing:
        break;
    case RenameState::Outcome::Cancelled:
        rename.reset();
        break;
    case RenameState::Outcome::Submit:
        submit_rename(rename);
        break;
    }
}

void ListActions::submit_rename(std::optional<RenameState>& rename) {
    const core::NotePath from = rename->from();
    if (rename->text() == from.string()) {
        rename.reset();
        return;
    }
    const auto to = core::NotePath::parse(rename->text());
    if (!to) {
        rename->set_error(core::format_error(to.error()));
        return;
    }
    const auto moved = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->move(from, *to);
    }();
    if (!moved) {
        rename->set_error(core::format_error(moved.error()));
        return;
    }
    rename.reset();
    host_.refresh_notes();
    host_.state.select_note(*moved);
    host_.status.message = core::tr(
        "tui.moved", {{"from", terminal_safe(from.string())}, {"to", terminal_safe(moved->string())}});
    host_.notify_changed();
}

void ListActions::save_prefs() {
    std::error_code ec;
    std::filesystem::create_directories(core::state_dir(), ec);
    const auto written =
        core::write_file_atomically(prefs_path(), format_list_prefs(host_.state.list_prefs()));
    if (!written)
        core::log_warn("event=tui_state_write_failed detail=" + core::format_error(written.error()));
}

}  // namespace noteapp::tui
