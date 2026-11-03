#include "tui/list_actions.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "tui/display_text.hpp"
#include "tui/settings.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
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

constexpr std::string_view kIconsNerd = "nerd";
constexpr std::string_view kIconsOff = "off";

// Aynı adda yer açarken denenecek en çok ad; depo aynı sınırı kullanır.
constexpr int kMaxNameTries = 1000;

// Klasör adı ya da not gövdesi: `iş-2`, `a.tar-2` (`a.tar-2.gz`).
std::string numbered_name(std::string_view name, int attempt) {
    return attempt == 1 ? std::string(name) : std::string(name) + "-" + std::to_string(attempt);
}

std::string safe(std::string_view raw) {
    return terminal_safe(raw);
}

// Baytlar aynen: create_exact satır sonuna dokunmaz. Uzantısız not yeni oluşturulamaz (depo tip ister), hata
// öğe olarak raporlanır, kaynak yerindedir.
std::expected<core::NotePath, core::Error> copy_note_free(core::NoteStore& store, const core::NotePath& from,
                                                          const std::string& dest) {
    auto content = store.read(from);
    if (!content)
        return std::unexpected(content.error());
    const std::string name = last_component(from.string());
    const core::TypedName typed = core::split_typed_name(name);
    std::expected<core::CreatedNote, core::Error> made = std::unexpected(core::Error{});
    for (int attempt = 1; attempt <= kMaxNameTries; ++attempt) {
        made = store.create_exact(core::NewNote{.folder = dest,
                                                .name = numbered_name(typed.stem, attempt),
                                                .type = std::string(typed.extension),
                                                .content = content->text,
                                                .exact_name = true});
        if (made)
            return made->path;
        if (made.error().code != core::Error::Code::AlreadyExists)
            break;
    }
    return std::unexpected(made.error());
}

// Klasörü rename_folder üst klasörleri kurmaz: hedef önce oluşur. Not için depo kendisi kurar.
std::optional<core::Error> place_item(core::NoteStore& store, const ListItem& item, const std::string& dest,
                                      const std::string& target) {
    if (!item.is_folder) {
        const auto moved = store.move(item.path, core::NotePath::parse(target).value());
        return moved ? std::nullopt : std::optional(moved.error());
    }
    if (!dest.empty())
        if (auto made = store.create_folder(dest); !made)
            return made.error();
    const auto moved = store.rename_folder(item.path.string(), target);
    return moved ? std::nullopt : std::optional(moved.error());
}

std::string_view unquoted(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\r\"");
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\"") - first + 1);
}

}  // namespace

std::string format_list_prefs(const ListPrefs& prefs) {
    // Satır tabanlı dosya: denetim baytlı ad satırı bölmesin; o klasör kapalı açılır.
    std::vector<std::string> open_folders;
    for (const std::string& folder : prefs.open_folders)
        if (std::ranges::none_of(folder, [](unsigned char c) { return c < 0x20; }))
            open_folders.push_back(folder);
    return "sort = \"" + std::string(name_of(kSortNames, prefs.sort.field)) +
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
                : "recent_commands = " + format_search_history(prefs.recent_commands) + "\n");
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
        else if (key == "search_history")
            prefs.search_history = parse_search_history(line.substr(equals + 1));
        else if (key == "open_folders")
            std::ranges::move(parse_quoted_list(line.substr(equals + 1)),
                              std::inserter(prefs.open_folders, prefs.open_folders.end()));
        else if (key == "recent_commands") {
            prefs.recent_commands = parse_quoted_list(line.substr(equals + 1));
            if (prefs.recent_commands.size() > TuiState::kMaxRecentCommands)
                prefs.recent_commands.resize(TuiState::kMaxRecentCommands);
        }
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
        host_.status.message = core::tr(done_key, {{"count", count_text("tui.count_items", report.done)}});
    } else {
        report.lines.insert(report.lines.begin(),
                            core::tr("tui.batch_counts", {{"done", std::to_string(report.done)},
                                                          {"failed", std::to_string(report.failed)}}));
        host_.state.show_summary(std::move(report.lines));
    }
    if (report.done > 0 || report.wrote)
        host_.notify_changed();
}

std::size_t ListActions::note_count_of(const std::vector<ListItem>& items) const {
    std::size_t notes = 0;
    for (const ListItem& item : items) {
        const FolderNode* node =
            item.is_folder ? host_.state.folder_tree().find(item.path.string()) : nullptr;
        notes += item.is_folder ? (node ? node->note_count : 0) : 1;
    }
    return notes;
}

void ListActions::begin_trash(std::optional<ConfirmState>& confirm) {
    if (host_.state.marked_count() > 0) {
        const std::vector<ListItem> items = host_.state.action_items();
        const bool has_notes_in_folder = std::ranges::any_of(items, [&](const ListItem& item) {
            const FolderNode* node =
                item.is_folder ? host_.state.folder_tree().find(item.path.string()) : nullptr;
            return node && node->note_count > 0;
        });
        if (!has_notes_in_folder) {
            trash_marked();  // geri dönüşlü, klasörde kaybedilecek not yok: onaysız, `d` kuralı
            return;
        }
        batch_pending_ = BatchAction::Trash;
        confirm.emplace(core::tr("tui.confirm_trash_batch",
                                 {{"items", count_text("tui.count_items", items.size())},
                                  {"notes", count_text("tui.count_notes", note_count_of(items))}}),
                        core::tr("tui.batch_trash_title"));
    } else if (const Row* folder = host_.state.selected_folder_row()) {
        begin_trash_folder(folder->path.string(), confirm);
    } else {
        trash_selected();
    }
}

void ListActions::begin_delete_action(std::optional<ConfirmState>& confirm) {
    if (host_.state.marked_count() == 0) {
        begin_delete(confirm);
        return;
    }
    const std::vector<ListItem> items = host_.state.action_items();
    batch_pending_ = BatchAction::Delete;
    confirm.emplace(core::tr("tui.confirm_delete_batch",
                             {{"items", count_text("tui.count_items", items.size())},
                              {"notes", count_text("tui.count_notes", note_count_of(items))}}));
}

void ListActions::restore_action() {
    if (host_.state.marked_count() > 0)
        restore_marked();
    else
        restore_selected();
}

// Her öğe denenir: biri düşerse kalanlar sürer, atılanlar `u` ile döner.
void ListActions::trash_marked() {
    const std::size_t index = host_.state.selected();
    TrashBatch batch;
    BatchReport report;
    for (const ListItem& item : host_.state.action_items()) {
        const std::string path = item.path.string();
        std::optional<core::Error> failure;
        if (item.is_folder) {
            failure = trash_folder_into(batch, path);
        } else {
            const std::scoped_lock lock(host_.store_mutex);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            if (auto trashed = host_.store->trash(item.path))
                batch.notes.push_back(std::move(trashed->path));
            else
                failure = trashed.error();
        }
        if (failure)
            report.fail(path, *failure);
        else
            ++report.done;
    }
    report.wrote = !batch.notes.empty() || !batch.folders.empty();
    if (report.wrote)
        trashed_.push_back(std::move(batch));
    host_.state.clear_marks();
    host_.refresh_notes();
    host_.state.select_index(index);
    publish(report, "tui.batch_trashed");
}

void ListActions::delete_marked() {
    const std::size_t index = host_.state.selected();
    const bool from_trash = host_.state.trash_view();
    BatchReport report;
    for (const ListItem& item : host_.state.action_items()) {
        const auto removed = [&] {
            const std::scoped_lock lock(host_.store_mutex);
            if (item.is_folder)
                return host_.store->remove_folder(item.path.string(), true);
            return from_trash ? host_.store->purge(item.path) : host_.store->remove(item.path);
        }();
        if (!removed) {
            report.fail(item.path.string(), removed.error());
            continue;
        }
        ++report.done;
        if (from_trash)
            forget_trashed(item.path);
    }
    host_.state.clear_marks();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    publish(report, "tui.batch_deleted");
}

void ListActions::restore_marked() {
    const std::size_t index = host_.state.selected();
    BatchReport report;
    for (const ListItem& item : host_.state.action_items()) {
        const auto restored = [&] {
            const std::scoped_lock lock(host_.store_mutex);
            return host_.store->restore(item.path);
        }();
        if (!restored) {
            report.fail(item.path.string(), restored.error());
            continue;
        }
        ++report.done;
        if (*restored != item.path)
            report.renamed(item.path.string(), restored->string());
        forget_trashed(item.path);
    }
    host_.state.clear_marks();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    publish(report, "tui.batch_restored");
}

void ListActions::copy_folder_item(const std::string& folder, const std::string& dest, BatchReport& report) {
    if (dest == folder || in_folder(dest, folder)) {
        report.fail(folder, core::tr("tui.paste_into_itself"));
        return;
    }
    const std::scoped_lock lock(host_.store_mutex);
    const std::string name = last_component(folder);
    std::expected<void, core::Error> copied = std::unexpected(core::Error{});
    std::string target;
    for (int attempt = 1; attempt <= kMaxNameTries; ++attempt) {
        target = join_folder(dest, numbered_name(name, attempt));
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        copied = host_.store->copy_folder(folder, target);
        if (copied || copied.error().code != core::Error::Code::AlreadyExists)
            break;
    }
    if (!copied) {
        if (copied.error().code == core::Error::Code::PartialCopy)
            report.wrote = true;  // yarım kopya yerinde: senkron da görmeli
        report.fail(folder, copied.error());
        return;
    }
    ++report.done;
    if (target != join_folder(dest, name))
        report.renamed(folder, target);
}

void ListActions::paste_item(const ListItem& item, const std::string& dest, BatchReport& report) {
    const std::string source = item.path.string();
    if (item.is_folder) {
        copy_folder_item(source, dest, report);
        return;
    }
    const std::scoped_lock lock(host_.store_mutex);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
    const auto placed = copy_note_free(*host_.store, item.path, dest);
    if (!placed) {
        report.fail(source, placed.error());
        return;
    }
    ++report.done;
    if (placed->string() != join_folder(dest, last_component(source)))
        report.renamed(source, placed->string());
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
    BatchReport report;
    for (const ListItem& item : yank.items)
        paste_item(item, dest, report);
    host_.refresh_notes();
    publish(report, "tui.paste_done");
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm,
                      std::optional<RenameState>& rename) {
    if (intent == Intent::LinkNote)
        begin_link(rename);
    else
        run(intent, confirm);
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm) {
    batch_pending_ = BatchAction::None;
    relation_target_.reset();
    switch (intent) {
    case Intent::UnlinkSelected:
        begin_unlink(confirm);
        break;
    case Intent::LinkMissing:
        host_.status.message = core::tr("tui.link_missing");
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
        return host_.store->trash(path);
    }();
    if (!trashed) {
        host_.status.message = core::format_error(trashed.error());
        return;
    }
    trashed_.push_back({{trashed->path}});
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
    const TrashBatch& last = trashed_.back();
    if (!last.folder.empty() || !last.folders.empty() || last.notes.size() > 1) {
        restore_batch();
        return;
    }
    const core::NotePath item = trashed_.back().notes.back();
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

// Adımın tamamı: önce klasörler (boş alt klasörler de), sonra notlar. Biri düşerse kalanlar sürer; kalan hata
// `u`da kalır, çöpte artık olmayan not adımı kilitlemesin diye düşer.
void ListActions::restore_batch() {
    const TrashBatch batch = std::move(trashed_.back());
    trashed_.pop_back();
    BatchReport report;
    TrashBatch retry{.folders = batch.folders, .folder = batch.folder};
    std::optional<core::NotePath> first;
    {
        const std::scoped_lock lock(host_.store_mutex);
        for (const std::string& path : batch.folders)
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            if (auto made = host_.store->create_folder(path); !made)
                report.fail(path, made.error());
        for (const core::NotePath& item : batch.notes | std::views::reverse) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            auto restored = host_.store->restore(item);
            if (!restored) {
                report.fail(item.string(), restored.error());
                if (restored.error().code != core::Error::Code::NotFound)
                    retry.notes.push_back(item);
                continue;
            }
            ++report.done;
            if (*restored != item)
                report.renamed(item.string(), restored->string());
            if (!first)
                first = *restored;
        }
    }
    if (!retry.notes.empty())
        trashed_.push_back(std::move(retry));
    host_.refresh_notes();
    if (!batch.folder.empty())
        host_.state.select_folder(batch.folder);
    else if (first)
        host_.state.select_note(*first);
    if (batch.folder.empty() || !report.lines.empty()) {
        publish(report, "tui.batch_restored");
        return;
    }
    host_.status.message =
        core::tr("tui.folder_restored", {{"path", terminal_safe(batch.folder)},
                                         {"count", count_text("tui.count_notes", report.done)}});
    host_.notify_changed();
}

// Kalıcı silinen çöp öğesi geri alınamaz: adımdan düşer. Klasör adımı klasörleri için kalır.
void ListActions::forget_trashed(const core::NotePath& item) {
    for (TrashBatch& batch : trashed_)
        std::erase(batch.notes, item);
    std::erase_if(trashed_, [](const TrashBatch& batch) {
        return batch.notes.empty() && batch.folder.empty() && batch.folders.empty();
    });
}

void ListActions::begin_trash_folder(const std::string& folder, std::optional<ConfirmState>& confirm) {
    const FolderNode* node = host_.state.folder_tree().find(folder);
    if (!node || node->note_count == 0) {
        trash_folder(folder);  // içinde not yok: kaybedilecek bir şey yok, `u` klasörü geri getirir
        return;
    }
    folder_target_ = folder;
    folder_permanent_ = false;
    delete_target_.reset();
    empty_pending_ = false;
    confirm.emplace(core::tr("tui.confirm_trash_folder",
                             {{"path", terminal_safe(folder)},
                              {"notes", count_text("tui.count_notes", node->note_count)},
                              {"folders", count_text("tui.count_subfolders", node->folder_count)}}),
                    core::tr("tui.folder_trash_title"));
}

// Depo klasörü not başına çöpe atmaz (remove_folder force notları kalıcı siler): notlar tek tek atılır, sonra
// boşalan klasör kaldırılır. Arada gelen yeni not NotEmpty verir, klasör ve o not yerinde kalır. Hiç not
// atılamadıysa klasör adımı yazılmaz: `u` senkronun kaldırdığı klasörü geri getirmesin.
std::optional<core::Error> ListActions::trash_folder_into(TrashBatch& batch, const std::string& folder) {
    const FolderTree& tree = host_.state.folder_tree();
    std::vector<std::string> folders = tree.folders_under(folder);
    std::vector<core::NotePath> notes;
    for (const std::size_t note : tree.notes_under(folder))
        notes.push_back(host_.state.metas()[note].path);
    std::optional<core::Error> failure;
    std::size_t trashed_count = 0;
    {
        const std::scoped_lock lock(host_.store_mutex);
        for (const core::NotePath& note : notes) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            auto trashed = host_.store->trash(note);
            if (!trashed) {
                failure = failure ? failure : trashed.error();
                continue;  // kalanlar sürer, klasör yerinde kalır
            }
            batch.notes.push_back(std::move(trashed->path));
            ++trashed_count;
        }
        if (!failure)
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            if (auto removed = host_.store->remove_folder(folder, false); !removed)
                failure = removed.error();
    }
    if (!failure || trashed_count > 0)
        batch.folders.insert(batch.folders.end(), folders.begin(), folders.end());
    return failure;
}

void ListActions::trash_folder(const std::string& folder) {
    const std::size_t index = host_.state.selected();
    TrashBatch batch{.folder = folder};
    const std::optional<core::Error> failure = trash_folder_into(batch, folder);
    const std::size_t count = batch.notes.size();
    if (!batch.notes.empty() || !failure)
        trashed_.push_back(std::move(batch));
    host_.refresh_notes();
    host_.state.select_index(index);
    host_.status.message =
        failure ? core::format_error(*failure)
                : core::tr("tui.folder_trashed", {{"path", terminal_safe(folder)},
                                                  {"count", count_text("tui.count_notes", count)}});
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
    folder_target_.reset();
    if (const Row* folder = host_.state.selected_folder_row()) {
        const std::string path = folder->path.string();
        const FolderNode* node = host_.state.folder_tree().find(path);
        folder_target_ = path;
        folder_permanent_ = true;
        delete_target_.reset();
        empty_pending_ = false;
        confirm.emplace(
            core::tr("tui.confirm_delete_folder",
                     {{"path", terminal_safe(path)},
                      {"notes", count_text("tui.count_notes", node ? node->note_count : 0)},
                      {"folders", count_text("tui.count_subfolders", node ? node->folder_count : 0)}}),
            core::tr("tui.folder_delete_title"));
        return;
    }
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
    folder_target_.reset();
    empty_pending_ = true;
    confirm.emplace(core::tr("tui.confirm_empty_trash",
                             {{"count", count_text("tui.count_notes", host_.state.rows().size())}}),
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

void ListActions::begin_link(std::optional<RenameState>& rename) {
    if (const Row* row = host_.state.selected_row())
        rename.emplace(RenameState::LinkTarget{}, row->path, host_.state.input_sources());
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
    host_.status.message = core::tr("tui.related", {{"path", terminal_safe(target->string())}});
    rename.reset();
    host_.refresh_notes();
    host_.notify_changed();
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
        host_.status.message = terminal_safe(core::format_error(removed.error()));
        return;
    }
    host_.status.message = core::tr("tui.unlinked", {{"path", terminal_safe(second.string())}});
    host_.refresh_notes();
    host_.notify_changed();
}

void ListActions::finish_delete(std::optional<ConfirmState>& confirm) {
    confirm.reset();
    if (relation_target_) {
        finish_unlink();
        return;
    }
    if (const BatchAction batch = std::exchange(batch_pending_, BatchAction::None);
        batch != BatchAction::None) {
        if (batch == BatchAction::Trash)
            trash_marked();
        else
            delete_marked();
        return;
    }
    if (std::exchange(empty_pending_, false)) {
        empty_trash();
        return;
    }
    if (folder_target_) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- üstteki if denetledi
        const std::string folder = *std::exchange(folder_target_, std::nullopt);
        if (folder_permanent_)
            finish_folder_delete(folder);
        else
            trash_folder(folder);
        return;
    }
    if (!delete_target_)
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- üstteki if denetledi
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

void ListActions::finish_folder_delete(const std::string& folder) {
    const std::size_t index = host_.state.selected();
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        return host_.store->remove_folder(folder, true);
    }();
    host_.refresh_notes();
    host_.state.select_index(index);
    if (!removed) {
        host_.status.message = core::format_error(removed.error());
        return;
    }
    host_.status.message = core::tr("tui.deleted", {{"path", terminal_safe(folder)}});
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

// Hedefte ad doluysa hiçbir öğe taşınmaz: hata girdide kalır, ezme yok.
std::optional<core::Error> ListActions::move_collision(const RenameState& rename,
                                                       const std::string& dest) const {
    for (const ListItem& item : rename.items()) {
        const std::string source = item.path.string();
        const std::string target = join_folder(dest, last_component(source));
        if (target == source)
            continue;
        // Hedef klasörler oluşmadan: reddedilen taşıma ağaçta iz bırakmasın.
        if (item.is_folder && (dest == source || in_folder(dest, source)))
            return core::Error{core::Error::Code::InvalidPath,
                               "error.invalid_path",
                               {{"reason", "into_itself"}, {"path", dest}}};
        const auto parsed = core::NotePath::parse(target);
        if (!parsed)
            return parsed.error();
        if (host_.state.meta_of(*parsed) || host_.state.folder_tree().find(target))
            return core::Error{core::Error::Code::AlreadyExists, "error.already_exists", {{"path", target}}};
    }
    return std::nullopt;
}

void ListActions::submit_move_to(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız girdi açıkken çağırır
    RenameState& input = *rename;
    const std::string dest = input.target();
    if (!dest.empty())
        if (const auto parsed = core::NotePath::parse(dest); !parsed) {
            input.set_error(core::format_error(parsed.error()));
            return;
        }
    if (const auto collision = move_collision(input, dest)) {
        input.set_error(core::format_error(*collision));
        return;
    }
    std::vector<ListItem> left = input.items();
    std::optional<ListItem> first;
    std::optional<core::Error> failure;
    {
        const std::scoped_lock lock(host_.store_mutex);
        while (!left.empty() && !failure) {
            const ListItem& item = left.front();
            const std::string source = item.path.string();
            const std::string target = join_folder(dest, last_component(source));
            if (target != source) {
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
                failure = place_item(*host_.store, item, dest, target);
                if (!failure && !first)
                    first = ListItem{core::NotePath::parse(target).value(), item.is_folder};
            }
            if (!failure)
                left.erase(left.begin());
        }
    }
    if (first) {
        host_.refresh_notes();
        host_.notify_changed();
    }
    if (failure) {
        input.set_items(std::move(left));  // taşınanlar taşındı, kalanı girdide; hata satırda
        input.set_error(core::format_error(*failure));
        return;
    }
    host_.state.clear_marks();
    rename.reset();
    if (!first)
        return;
    if (first->is_folder)
        host_.state.select_folder(first->path.string());
    else
        host_.state.reveal(first->path);
    host_.status.message = core::tr("tui.moved_to", {{"to", terminal_safe(dest)}});
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
    host_.refresh_notes();
    host_.state.select_note(*moved);
    host_.status.message = core::tr(
        "tui.moved", {{"from", terminal_safe(from.string())}, {"to", terminal_safe(moved->string())}});
    host_.notify_changed();
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
