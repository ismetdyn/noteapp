#include "tui/list_actions.hpp"

#include "app/import_files.hpp"
#include "app/map_relations.hpp"
#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "pdf/pdf_export.hpp"
#include "tui/settings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kPrefsFile = "tui-state.toml";
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

constexpr std::string_view kIconsNerd = "nerd";
constexpr std::string_view kIconsOff = "off";

std::string safe(std::string_view raw) {
    return terminal_safe(raw);
}

std::string_view unquoted(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\r\"");
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\"") - first + 1);
}

// `list_share`: 0..1 arası sonlu sayı; değilse yok sayılır, config'in oranı kalır.
std::optional<double> parse_share(std::string_view value) {
    double share = 0.0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), share);
    if (error == std::errc{} && end == value.data() + value.size() && std::isfinite(share) && share >= 0.0 &&
        share <= 1.0)
        return share;
    core::log_warn("event=tui_state_value_ignored value=" + terminal_safe(value));
    return std::nullopt;
}

std::vector<app::TrashTarget> trash_targets(const std::vector<ListItem>& items) {
    std::vector<app::TrashTarget> targets;
    targets.reserve(items.size());
    for (const ListItem& item : items)
        targets.push_back({item.path, item.is_folder});
    return targets;
}

app::Prompts confirmed() {
    return {[](const app::Confirm&) { return true; }};
}

// Onay metni ve başlığı; ne zaman sorulacağı app::TrashOps'ta.
ConfirmState confirm_state(const app::Confirm& request) {
    const std::string path = terminal_safe(request.path);
    const std::string notes = count_text("tui.count_notes", request.notes);
    const std::string folders = count_text("tui.count_subfolders", request.folders);
    const std::string items = count_text("tui.count_items", request.items);
    switch (request.kind) {
    case app::ConfirmKind::TrashFolder:
        return ConfirmState(
            core::tr("tui.confirm_trash_folder", {{"path", path}, {"notes", notes}, {"folders", folders}}),
            core::tr("tui.folder_trash_title"));
    case app::ConfirmKind::TrashBatch:
        return ConfirmState(core::tr("tui.confirm_trash_batch", {{"items", items}, {"notes", notes}}),
                            core::tr("tui.batch_trash_title"));
    case app::ConfirmKind::Delete:
        return ConfirmState(core::tr("tui.confirm_delete", {{"path", path}}));
    case app::ConfirmKind::Purge:
        return ConfirmState(core::tr("tui.confirm_purge", {{"path", path}}));
    case app::ConfirmKind::DeleteFolder:
        return ConfirmState(
            core::tr("tui.confirm_delete_folder", {{"path", path}, {"notes", notes}, {"folders", folders}}),
            core::tr("tui.folder_delete_title"));
    case app::ConfirmKind::DeleteBatch:
        return ConfirmState(core::tr("tui.confirm_delete_batch", {{"items", items}, {"notes", notes}}));
    case app::ConfirmKind::EmptyTrash:
        return ConfirmState(
            core::tr("tui.confirm_empty_trash", {{"count", count_text("tui.count_notes", request.items)}}),
            core::tr("tui.empty_trash_title"));
    }
    std::unreachable();
}

}  // namespace

std::string format_list_prefs(const ListPrefs& prefs) {
    // Satır tabanlı dosya: denetim baytlı ad satırı bölmesin; o klasör kapalı açılır.
    std::vector<std::string> open_folders;
    for (const std::string& folder : prefs.open_folders)
        if (std::ranges::none_of(folder, [](unsigned char c) { return c < 0x20; }))
            open_folders.push_back(folder);
    return "sort = \"" + std::string(name_of(app::kSortFieldNames, prefs.sort.field)) +
           "\"\nsort_descending = " + (prefs.sort.descending ? "true" : "false") + "\ncolumn = \"" +
           std::string(name_of(kColumnNames, prefs.column)) + "\"\nversion = " + std::string(kPrefsVersion) +
           "\n" +
           (prefs.search_history.empty()
                ? std::string{}
                : "search_history = " + format_search_history(prefs.search_history) + "\n") +
           (open_folders.empty() ? std::string{}
                                 : "open_folders = " + format_search_history(open_folders) + "\n") +
           (prefs.recent_commands.empty()
                ? std::string{}
                : "recent_commands = " + format_search_history(prefs.recent_commands) + "\n") +
           (prefs.list_share ? std::format("list_share = {}\n", *prefs.list_share) : std::string{}) +
           app::format_saved_tabs(prefs.tabs);  // `[[tabs]]` tabloları: TOML'da en sonda
}

ListPrefs parse_list_prefs(std::string_view text, const SearchHistoryLimits& history) {
    ListPrefs prefs;
    prefs.tabs = app::parse_saved_tabs(text);  // satırları aşağıdaki döngü bilinmeyen anahtar sayar
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
            value_of(app::kSortFieldNames, value, prefs.sort.field);
        else if (key == "sort_descending")
            prefs.sort.descending = value == "true";
        else if (key == "column")
            value_of(kColumnNames, value, prefs.column);
        else if (key == "version")
            versioned = true;
        else if (key == "search_history")
            prefs.search_history = parse_search_history(line.substr(equals + 1), history);
        else if (key == "open_folders")
            std::ranges::move(parse_quoted_list(line.substr(equals + 1)),
                              std::inserter(prefs.open_folders, prefs.open_folders.end()));
        else if (key == "recent_commands")
            prefs.recent_commands = parse_quoted_list(line.substr(equals + 1));  // sınır: set_list_prefs
        else if (key == "list_share")
            prefs.list_share = parse_share(value);
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
    state.set_list_prefs(parse_list_prefs(text, state.search_history_limits()));
}

struct ListActions::BatchReport {
    std::size_t done = 0;
    std::size_t failed = 0;
    std::vector<std::string> lines = {};

    void fail(std::string_view item, const std::string& why) {
        ++failed;
        lines.push_back(core::tr("tui.batch_item_failed", {{"path", safe(item)}, {"error", safe(why)}}));
    }
    void fail(std::string_view item, const core::Error& error) { fail(item, core::format_error(error)); }
    bool wrote = false;  // öğe tamamlanmasa da depo değişti: senkron haber almalı
    void renamed(std::string_view from, std::string_view to) {
        lines.push_back(core::tr("tui.batch_item_renamed", {{"from", safe(from)}, {"to", safe(to)}}));
    }
};

// Sorun yoksa tek satır; hata ya da ad değişimi varsa öğe öğe küçük pencere.
void ListActions::publish(BatchReport& report, const char* done_key) {
    if (report.lines.empty()) {
        host_.status.notify(app::NoticeKind::Success, core::tr(done_key, {{"count", count_text("tui.count_items", report.done)}}));
    } else {
        report.lines.insert(report.lines.begin(),
                            core::tr("tui.batch_counts", {{"done", std::to_string(report.done)},
                                                          {"failed", std::to_string(report.failed)}}));
        host_.state.show_summary(std::move(report.lines));
    }
    if (report.done > 0 || report.wrote)
        host_.notify_changed();
}

void ListActions::publish(const app::BatchReport& report, const char* done_key) {
    BatchReport shown{.done = report.done, .wrote = report.wrote};
    for (const app::BatchReport::Line& line : report.lines) {
        if (line.error)
            shown.fail(line.path, *line.error);
        else
            shown.renamed(line.path, line.renamed_to);
    }
    publish(shown, done_key);
}

app::Prompts ListActions::ask_later(std::optional<ConfirmState>& confirm, std::function<void()> retry) {
    return {[this, &confirm, retry = std::move(retry)](const app::Confirm& request) {
        confirm.emplace(confirm_state(request));
        pending_ = retry;
        return false;
    }};
}

void ListActions::begin_trash(std::optional<ConfirmState>& confirm) {
    if (host_.state.marked_count() > 0) {
        trash_marked(ask_later(confirm, [this] { trash_marked(confirmed()); }));
    } else if (const Row* folder = host_.state.selected_folder_row()) {
        const std::string path = folder->path.string();
        trash_folder(path, ask_later(confirm, [this, path] { trash_folder(path, confirmed()); }));
    } else {
        trash_selected();
    }
}

void ListActions::begin_delete_action(std::optional<ConfirmState>& confirm) {
    if (host_.state.marked_count() == 0) {
        begin_delete(confirm);
        return;
    }
    delete_marked(ask_later(confirm, [this] { delete_marked(confirmed()); }));
}

void ListActions::restore_action() {
    if (host_.state.marked_count() > 0)
        restore_marked();
    else
        restore_selected();
}

// Her öğe denenir: biri düşerse kalanlar sürer, atılanlar `u` ile döner.
void ListActions::trash_marked(const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const std::vector<app::TrashTarget> targets = trash_targets(host_.state.action_items());
    const auto report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.trash_items(*host_.store, host_.state.listing(), targets, prompts);
    }();
    if (!report)
        return;  // onay bekliyor
    host_.state.clear_marks();
    host_.refresh_notes();
    host_.state.select_index(index);
    publish(*report, "tui.batch_trashed");
}

void ListActions::delete_marked(const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const bool from_trash = host_.state.trash_view();
    const std::vector<app::TrashTarget> targets = trash_targets(host_.state.action_items());
    const auto report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.delete_items(*host_.store, host_.state.listing(), targets, from_trash, prompts);
    }();
    if (!report)
        return;  // onay bekliyor
    host_.state.clear_marks();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    publish(*report, "tui.batch_deleted");
}

void ListActions::restore_marked() {
    const std::size_t index = host_.state.selected();
    std::vector<core::NotePath> items;
    for (const ListItem& item : host_.state.action_items())
        items.push_back(item.path);
    const app::BatchReport report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.restore_items(*host_.store, items);
    }();
    host_.state.clear_marks();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    publish(report, "tui.batch_restored");
}

// Yalnız kopya: kaynak yerinde kalır; biri düşerse kalanlar sürer, hiçbir şey ezilmez.
void ListActions::paste() {
    if (!host_.state.yank()) {
        host_.status.message = core::tr("tui.paste_nothing");
        return;
    }
    if (!host_.state.in_folder_view()) {
        host_.status.message = core::tr("tui.paste_needs_folder");
        return;
    }
    const PendingYank yank = *host_.state.yank();
    const std::string dest = host_.state.current_folder();
    host_.state.clear_yank();
    const app::BatchReport report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return app::paste_items(*host_.store, yank.items, dest);
    }();
    host_.refresh_notes();
    publish(report, "tui.paste_done");
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm,
                      std::optional<RenameState>& rename) {
    if (intent == Intent::LinkNote)
        begin_link(rename);
    else if (intent == Intent::ExportPdf)
        begin_export(rename);
    else
        run(intent, confirm);
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm) {
    pending_ = nullptr;
    relation_target_.reset();
    switch (intent) {
    case Intent::UnlinkSelected:
        begin_unlink(confirm);
        break;
    case Intent::LinkMissing:
        host_.status.message = core::tr("tui.link_missing");
        break;
    case Intent::FollowLink:
        follow_wiki_link();
        break;
    case Intent::CopyNote:
        copy_selected();
        break;
    case Intent::TrashSelected:
        begin_trash(confirm);
        break;
    case Intent::Paste:
        paste();
        break;
    case Intent::RestoreLast:
        restore_last();
        break;
    case Intent::RestoreSelected:
        restore_action();
        break;
    case Intent::ToggleTrash:
        toggle_trash();
        break;
    case Intent::DeleteSelected:
        begin_delete_action(confirm);
        break;
    case Intent::EmptyTrash:
        begin_empty_trash(confirm);
        break;
    case Intent::SavePrefs:
        save_prefs();
        break;
    case Intent::ToggleIcons:
        toggle_icons();
        break;
    case Intent::MapEdit:
        apply_map_edit();
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
    case MouseResult::SavePrefs:
        save_prefs();
        break;
    case MouseResult::FollowLink:
        follow_wiki_link();
        break;
    case MouseResult::MapEdit:
        apply_map_edit();
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

void ListActions::copy_selected() {
    if (const Row* folder = host_.state.selected_folder_row())
        copy_text(folder->path.string());
    else
        copy_selected_note();
}

// Ham baytlar: önizleme arındırılmış olabilir. Terminal ve araçlar ikili veriyi taşıyamaz, neden söylenir.
void ListActions::copy_selected_note() {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
    host_.preview.get(*host_.store, host_.state.selected_row());
    const auto selection = host_.state.preview_selection_range();
    if (!selection)
        return;
    // Sürüklerken not değişti: uçlar başka metne ait, yanlış parça kopyalanmasın.
    if (host_.preview.generation() != shown_generation) {
        host_.state.clear_preview_selection();
        return;
    }
    // Biçimli görünümde seçim görüntülenen metindir; `c` özgün baytları kopyalar.
    const Preview& preview = host_.preview.shown();
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
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.trash_note(*host_.store, path);
    }();
    if (!trashed) {
        host_.status.notify(app::NoticeKind::Error, core::format_error(trashed.error()));
        return;
    }
    host_.refresh_notes();
    host_.state.select_index(index);
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.trashed", {{"path", terminal_safe(path.string())}}));
    host_.notify_changed();
}

void ListActions::restore_last() {
    auto undone = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.undo(*host_.store);
    }();
    if (!undone) {
        host_.status.message = core::tr("tui.nothing_to_restore");
        return;
    }
    if (undone->single) {
        if (!undone->first) {
            for (const app::BatchReport::Line& line : undone->report.lines)
                if (line.error)
                    host_.status.message = core::format_error(*line.error);
            return;
        }
        host_.refresh_notes();
        host_.state.select_note(*undone->first);
        host_.status.notify(app::NoticeKind::Success, core::tr("tui.restored", {{"path", terminal_safe(undone->first->string())}}));
        host_.notify_changed();
        return;
    }
    host_.refresh_notes();
    if (!undone->folder.empty())
        host_.state.select_folder(undone->folder);
    else if (undone->first)
        host_.state.select_note(*undone->first);
    if (undone->folder.empty() || !undone->report.lines.empty()) {
        publish(undone->report, "tui.batch_restored");
        return;
    }
    host_.status.message =
        core::tr("tui.folder_restored", {{"path", terminal_safe(undone->folder)},
                                         {"count", count_text("tui.count_notes", undone->report.done)}});
    host_.notify_changed();
}

void ListActions::trash_folder(const std::string& folder, const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const auto trashed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.trash_folder(*host_.store, host_.state.listing(), folder, prompts);
    }();
    if (!trashed)
        return;  // onay bekliyor
    host_.refresh_notes();
    host_.state.select_index(index);
    host_.status.message =
        trashed->failure
            ? core::format_error(*trashed->failure)
            : core::tr("tui.folder_trashed", {{"path", terminal_safe(folder)},
                                              {"count", count_text("tui.count_notes", trashed->notes)}});
    host_.notify_changed();
}

void ListActions::restore_selected() {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    const core::NotePath item = row->path;
    const std::size_t index = host_.state.selected();
    const auto restored = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.restore(*host_.store, item);
    }();
    if (!restored) {
        host_.status.notify(app::NoticeKind::Error, core::format_error(restored.error()));
        return;
    }
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.restored", {{"path", terminal_safe(restored->string())}}));
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
    if (const Row* folder = host_.state.selected_folder_row()) {
        const std::string path = folder->path.string();
        delete_folder(path, ask_later(confirm, [this, path] { delete_folder(path, confirmed()); }));
        return;
    }
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    const core::NotePath target = row->path;
    delete_note(target, ask_later(confirm, [this, target] { delete_note(target, confirmed()); }));
}

// Çöp görünümünde çöpten (purge), değilse canlı not kalıcı silinir.
void ListActions::delete_note(const core::NotePath& target, const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const bool from_trash = host_.state.trash_view();
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);  // yeniden tarama iş parçacığı aynı store'u listeler
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        core::NoteStore& store = *host_.store;
        return from_trash ? trash_.purge(store, target, prompts) : trash_.remove(store, target, prompts);
    }();
    if (removed && !*removed)
        return;  // onay bekliyor
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    if (!removed) {
        host_.status.notify(app::NoticeKind::Error, core::format_error(removed.error()));
        return;
    }
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.deleted", {{"path", terminal_safe(target.string())}}));
    host_.notify_changed();
}

void ListActions::begin_empty_trash(std::optional<ConfirmState>& confirm) {
    empty_trash(ask_later(confirm, [this] { empty_trash(confirmed()); }));
}

void ListActions::empty_trash(const app::Prompts& prompts) {
    const auto emptied = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.empty_trash(*host_.store, prompts);
    }();
    if (emptied && !*emptied)
        return;  // onay bekliyor
    host_.refresh_notes();
    refresh_trash_view();
    if (!emptied) {
        host_.status.notify(app::NoticeKind::Error, core::format_error(emptied.error()));
        return;
    }
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.trash_emptied"));
    host_.notify_changed();
}

void ListActions::begin_link(std::optional<RenameState>& rename) {
    if (const Row* row = host_.state.selected_row())
        rename.emplace(RenameState::LinkTarget{}, row->path, host_.state.input_sources());
}

void ListActions::begin_import(std::optional<RenameState>& rename, std::string folder) {
    rename.emplace(RenameState::ImportSource{}, std::move(folder));
}

void ListActions::submit_import(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    const std::string& typed = input.text();
    if (typed.empty())
        return;
    if (import_.running()) {
        input.set_error(core::tr("cli.import_busy"));
        return;
    }
    std::vector<std::filesystem::path> sources{
        std::filesystem::path(std::u8string(typed.begin(), typed.end()))};
    const core::Config defaults;
    app::ImportOptions options =
        app::import_options(host_.config ? *host_.config : defaults, input.import_folder());
    // Kopya aynı bağlantıyı paylaşır (thread-safe); kilit yalnız notes_dir değişiminde yeniden kurulan
    // optional'a karşı.
    core::NoteStore store = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return *host_.store;
    }();
    // `rename` main'in (ya da testin) girdisidir, ListActions'tan uzun yaşar; rapor onu geri açabilir. Rapor
    // UI iş parçacığına post ile gelir: girdiyi start'tan sonra taşımak yarışmaz.
    const bool started = import_.start(
        std::move(store), std::move(sources), std::move(options),
        [this](const app::ImportProgress& progress) {
            host_.status.message = terminal_safe(app::import_progress_text(progress) + "  " +
                                                 core::tr("tui.import_cancel_hint"));
        },
        [this, &rename](const app::ImportReport& report) { finish_import(rename, report); });
    if (!started) {
        input.set_error(core::tr("cli.import_start_failed"));
        return;
    }
    import_prompt_ = std::move(rename);
    rename.reset();
}

void ListActions::finish_import(std::optional<RenameState>& rename, const app::ImportReport& report) {
    std::optional<RenameState> prompt = std::exchange(import_prompt_, std::nullopt);
    if (report.wrote) {  // hiçbir not alınmasa da klasör kurulmuş olabilir
        host_.refresh_notes();
        host_.notify_changed();
    }
    const std::string status = terminal_safe(app::import_status(report));
    // Hiçbir şey alınamadı: yol düzeltilebilsin diye girdi yazılanla geri açılır (başka girdi açık değilse).
    // Depo değiştiyse (klasör kuruldu) açılmaz: yeniden deneme ikinci bir `-2` kopyası kurardı.
    if (report.imported.empty() && report.cancelled == 0 && !report.wrote && prompt && !rename) {
        rename = std::move(prompt);
        rename->set_error(status);
        host_.status.message.clear();  // ilerleme satırı kalmasın
        return;
    }
    if (!report.imported.empty() && host_.state.focus() != Focus::Editor)  // yazan kullanıcının odağı kalır
        host_.state.open_note_in_review(report.imported.front().note);
    host_.status.notify(app::NoticeKind::Success, status);
}

bool ListActions::cancel_import_on(Key key, Focus focus) {
    if (!import_.running())
        return false;
    // Ctrl+C editör dışında uygulamayı kapatırdı: önce içe almayı durdurur. Esc yalnız liste ve önizleme
    // odağında; girdide (arama, bul) kendi işi sürer.
    const bool esc =
        key == Key::Escape && (focus == Focus::List || focus == Focus::Preview || focus == Focus::Links);
    const bool ctrl_c = key == Key::CtrlC && focus != Focus::Editor;
    if (!esc && !ctrl_c)
        return false;
    import_.cancel();
    host_.status.message = core::tr("cli.import_cancelling");
    return true;
}

void ListActions::submit_link(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    const auto target = core::NotePath::parse(input.target());
    if (!target) {
        input.set_error(core::format_error(target.error()));
        return;
    }
    const auto added = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->add_relation(input.from(), *target);
    }();
    if (!added) {
        input.set_error(core::format_error(added.error()));
        return;
    }
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.related", {{"path", terminal_safe(target->string())}}));
    rename.reset();
    host_.refresh_notes();
    host_.notify_changed();
}

void ListActions::follow_wiki_link() {
    const std::optional<std::string> target = host_.state.selected_wiki_target();
    if (!target)
        return;
    const std::optional<core::NotePath> path = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->resolve_link(*target);
    }();
    if (!path || !host_.state.open_note_in_review(*path))
        host_.status.message = core::tr("tui.link_missing");
}

void ListActions::begin_export(std::optional<RenameState>& rename) {
    const Row* row = host_.state.selected_row();
    if (!row)
        return;
    export_replace_.clear();
    rename.emplace(RenameState::ExportTarget{}, row->path,
                   core::split_note_path(row->path.string()).name + ".pdf");
}

void ListActions::submit_export(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    const std::string& typed = input.text();
    if (typed.empty())
        return;
    const std::filesystem::path given(std::u8string(typed.begin(), typed.end()));
    std::error_code ec;
    std::filesystem::path output = std::filesystem::absolute(given, ec);
    if (ec)
        output = given;
    const std::u8string shown = output.u8string();
    const std::string shown_path = terminal_safe(std::string(shown.begin(), shown.end()));
    // Uyarı satırda durdukça (düzenleme onu siler) aynı yola ikinci Enter onaydır.
    const bool warned = !input.error().empty() && export_replace_ == output;
    // symlink_status: kırık bağ da var olan addır (exists bağı izler, onu "yok" sayıp sormadan yazardı).
    if (std::filesystem::exists(std::filesystem::symlink_status(output, ec)) && !warned) {
        export_replace_ = output;
        input.set_error(core::tr("tui.export_exists", {{"path", shown_path}}));
        return;
    }
    export_replace_.clear();
    if (const auto exported = export_note(input.from(), output); !exported) {
        input.set_error(core::format_error(exported.error()));
        return;
    }
    rename.reset();
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.exported", {{"path", shown_path}}));
}

std::expected<void, core::Error> ListActions::export_note(const core::NotePath& note,
                                                          const std::filesystem::path& output) const {
    const auto content = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->read(note);
    }();
    if (!content)
        return std::unexpected(content.error());
    if (core::looks_binary(content->text))
        return std::unexpected(
            core::Error{core::Error::Code::InvalidPath, "tui.export_binary", {{"path", note.string()}}});
    return pdf::export_pdf(content->text, pdf::format_for(note.value()), output);
}

void ListActions::begin_unlink(std::optional<ConfirmState>& confirm) {
    const Row* row = host_.state.selected_row();
    const core::NotePath* other = host_.state.link_panel().selected_path();
    if (!row || !other)
        return;
    relation_target_ = std::pair(row->path, *other);
    confirm.emplace(core::tr("tui.confirm_unlink", {{"from", terminal_safe(row->path.string())},
                                                    {"to", terminal_safe(other->string())}}),
                    core::tr("tui.unlink_title"));
}

void ListActions::finish_unlink() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- finish_delete denetledi
    const auto [first, second] = *std::exchange(relation_target_, std::nullopt);
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->remove_relation(first, second);
    }();
    if (!removed) {
        host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(removed.error())));
        return;
    }
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.unlinked", {{"path", terminal_safe(second.string())}}));
    host_.refresh_notes();
    host_.notify_changed();
}

// Liste odağına editör park edilerek (kayıtla) gelinir, ayrılan sekme de kaydedilir: yazılacak notun
// bekleyen düzenlemesi yoktur. refresh_notes'tan sonra main her olayda sekmeleri tazeler (settle_tabs →
// EditorTabs::on_listing): temiz sekme yeni metni okur, yine de kirli kalmış sekme ezilmez, kaydı çakışma
// sorar.
void ListActions::apply_map_edit() {
    const auto request = host_.state.take_map_edit();
    if (!request)
        return;
    if (!request->kind) {
        host_.status.message = terminal_safe(
            core::tr("tui.map_no_edge", {{"from", request->from.key}, {"to", request->to.key}}));
        return;
    }
    const auto done = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        core::NoteStore& store = *host_.store;
        return request->connect
                   ? app::connect_map_nodes(store, request->from, request->to, *request->kind)
                   : app::disconnect_map_nodes(store, request->from, request->to, *request->kind);
    }();
    if (!done) {
        host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(done.error())));
        return;
    }
    host_.status.notify(app::NoticeKind::Success, terminal_safe(*done));
    host_.state.map_edges_changed();  // ilişki listeyi değiştirmez: kenarlar sonraki çizimde okunur
    host_.refresh_notes();
    host_.notify_changed();
}

void ListActions::finish_delete(std::optional<ConfirmState>& confirm) {
    confirm.reset();
    if (relation_target_) {
        finish_unlink();
        return;
    }
    if (const std::function<void()> retry = std::exchange(pending_, nullptr))
        retry();
}

void ListActions::delete_folder(const std::string& folder, const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.remove_folder(*host_.store, host_.state.listing(), folder, prompts);
    }();
    if (removed && !*removed)
        return;  // onay bekliyor
    host_.refresh_notes();
    host_.state.select_index(index);
    if (!removed) {
        host_.status.message = core::format_error(removed.error());
        return;
    }
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.deleted", {{"path", terminal_safe(folder)}}));
    host_.notify_changed();
}

void ListActions::begin_rename(std::optional<RenameState>& rename) {
    if (const Row* row = host_.state.selected_entry())
        rename.emplace(row->path, host_.state.name_input_sources(), host_.state.name_base_folder(),
                       row->is_folder);
}

void ListActions::begin_move_to(std::optional<RenameState>& rename) {
    std::vector<ListItem> items = host_.state.action_items();
    if (items.empty())
        return;
    // Boş klasör de hedeftir: aday listesi notlardan değil ağaçtan.
    auto sources = std::make_shared<InputSources>(*host_.state.input_sources());
    sources->folders.clear();
    for (const std::string& folder : host_.state.folder_tree().folders_under({}))
        if (!folder.empty())
            sources->folders.push_back(folder + "/");
    std::ranges::sort(sources->folders);
    const std::string current = host_.state.current_folder();
    rename.emplace(std::move(items), current.empty() ? std::string{} : current + "/", std::move(sources));
}

void ListActions::submit_move_to(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız girdi açıkken çağırır
    RenameState& input = *rename;
    const std::string dest = input.target();
    const std::vector<ListItem>& items = input.items();
    const app::MoveResult result = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return app::move_items(*host_.store, host_.state.listing(), items, dest);
    }();
    if (result.moved > 0) {
        // Yerinde kalan öğe taşınmaz; kalanlar sırayla yürür, ilk `moved` tanesi gitti.
        std::size_t followed = 0;
        for (const ListItem& item : items) {
            const std::string to = join_folder(dest, last_component(item.path.string()));
            if (to == item.path.string() || followed == result.moved)
                continue;
            ++followed;
            moved(item.path.string(), to);
        }
        host_.refresh_notes();
        host_.notify_changed();
    }
    if (result.failure) {
        // Taşınanlar taşındı, kalanı girdide; hata satırda. Yerinde kalan öğe sayılmaz: taşınacaklar sırayla
        // yürür, ilk `moved` tanesi gitti.
        std::vector<ListItem> left;
        for (const ListItem& item : items)
            if (join_folder(dest, last_component(item.path.string())) != item.path.string())
                left.push_back(item);
        left.erase(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(result.moved));
        input.set_items(std::move(left));
        input.set_error(core::format_error(*result.failure));
        return;
    }
    host_.state.clear_marks();
    rename.reset();
    if (!result.first)
        return;
    if (result.first->is_folder)
        host_.state.select_folder(result.first->path.string());
    else
        host_.state.reveal(result.first->path);
    host_.status.notify(app::NoticeKind::Success, core::tr("tui.moved_to", {{"to", terminal_safe(dest)}}));
}

void ListActions::create_folder_from_row(std::optional<NewRowState>& row) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
    std::string_view typed = row->text();
    typed.remove_suffix(1);
    const std::string folder = join_folder(host_.state.name_base_folder(), typed);
    if (host_.state.folder_tree().find(folder)) {
        row.reset();
        host_.state.select_folder(folder);
        host_.status.message = core::tr("tui.folder_exists", {{"path", terminal_safe(folder)}});
        return;
    }
    const auto made = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->create_folder(folder);
    }();
    if (!made) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
        row->set_error(core::format_error(made.error()));
        return;
    }
    row.reset();
    host_.refresh_notes();
    host_.state.select_folder(folder);
    host_.status.message = core::tr("tui.folder_created", {{"path", terminal_safe(folder)}});
    host_.notify_changed();
}

void ListActions::submit_new_row(std::optional<NewRowState>& row, const NewRowDefaults& defaults,
                                 const std::function<void(const core::NotePath&)>& open) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
    if (names_folder(row->text())) {
        create_folder_from_row(row);
        return;
    }
    // Baştaki `/` sessizce atılıp köke yazılmasın: yol notların köküne görelidir, mutlak yol hatadır.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
    if (row->text().starts_with('/')) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
        row->set_error(core::format_error(core::NotePath::parse(row->text()).error()));
        return;
    }
    // Klasör görünümünde ad geçerli klasöre göredir; düz listede eski kural.
    const bool in_folder = host_.state.in_folder_view();
    const std::string base = host_.state.name_base_folder();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
    const std::string name = in_folder ? join_folder(base, row->text()) : row->text();
    const auto created = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return create_named_note(*host_.store, name, in_folder ? base : defaults.folder, defaults.type,
                                 defaults.slug_max_length);
    }();
    if (!created) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
        row->set_error(core::format_error(created.error()));
        return;
    }
    row.reset();
    host_.refresh_notes();
    host_.state.select_note(created->path);  // süzgeç notu gizliyorsa temizlenir
    open(created->path);
    host_.notify_changed();
}

void ListActions::submit_folder_rename(std::optional<RenameState>& rename, const std::string& target) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    const std::string from = rename->from().string();
    const auto moved = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->rename_folder(from, target);
    }();
    if (!moved) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
        rename->set_error(core::format_error(moved.error()));
        return;
    }
    rename.reset();
    this->moved(from, target);
    host_.refresh_notes();
    host_.state.select_folder(target);
    host_.status.message =
        core::tr("tui.moved", {{"from", terminal_safe(from)}, {"to", terminal_safe(target)}});
    host_.notify_changed();
}

void ListActions::handle_rename_key(std::optional<RenameState>& rename, Key key, std::string_view text) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_move_to()) {
        submit_move_to(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_link()) {
        submit_link(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_export()) {
        submit_export(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_import()) {
        submit_import(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    const core::NotePath from = rename->from();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    const std::string target = rename->target();
    if (target == from.string()) {
        rename.reset();
        return;
    }
    const auto to = core::NotePath::parse(target);
    if (!to) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
        rename->set_error(core::format_error(to.error()));
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_folder()) {
        submit_folder_rename(rename, target);
        return;
    }
    const auto moved = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->move(from, *to);
    }();
    if (!moved) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
        rename->set_error(core::format_error(moved.error()));
        return;
    }
    rename.reset();
    this->moved(from.string(), moved->string());
    host_.refresh_notes();
    host_.state.select_note(*moved);
    host_.status.message = core::tr(
        "tui.moved", {{"from", terminal_safe(from.string())}, {"to", terminal_safe(moved->string())}});
    host_.notify_changed();
}

void ListActions::moved(const std::string& from, const std::string& to) const {
    if (host_.on_moved)
        host_.on_moved(from, to);
}

void ListActions::save_prefs_if_changed() {
    if (host_.state.take_prefs_changed())
        save_prefs();
}

// Ayarlar ekranıyla aynı kural: okunamayan config.toml ezilmez; yazılamazsa eski değer kalır.
void ListActions::toggle_icons() {
    if (!host_.config)
        return;
    if (user_config_broken()) {
        const std::u8string path = core::config_file().u8string();
        host_.status.message = core::tr("tui.settings_config_broken",
                                        {{"path", terminal_safe(std::string(path.begin(), path.end()))}});
        return;
    }
    core::Config next = *host_.config;
    next.icons = next.icons == kIconsOff ? kIconsNerd : kIconsOff;
    if (const auto saved = core::save_config(next); !saved) {
        host_.status.message = terminal_safe(core::format_error(saved.error()));
        return;
    }
    host_.config->icons = next.icons;
    host_.state.set_icon_style(core::terminal_icon_style(next.icons));
}

void ListActions::save_prefs() {
    host_.state.prune_open_folders();
    std::error_code ec;
    std::filesystem::create_directories(core::state_dir(), ec);
    // Arama geçmişi taşır: yalnız sahibi okuyabilsin.
    constexpr bool kOwnerOnly = true;
    const auto written =
        core::write_file_atomically(prefs_path(), format_list_prefs(host_.state.list_prefs()), kOwnerOnly);
    if (!written)
        core::log_warn("event=tui_state_write_failed detail=" + core::format_error(written.error()));
}

}  // namespace noteapp::tui
