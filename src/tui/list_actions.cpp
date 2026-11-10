#include "tui/list_actions.hpp"

#include "app/export.hpp"
#include "app/import_files.hpp"
#include "app/map_relations.hpp"
#include "app/settings.hpp"
#include "app/ui_prefs.hpp"
#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_views.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "tui/settings.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
#include <filesystem>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kPrefsFile = "tui-state.toml";
constexpr std::string_view kListShare = "list_share";

constexpr std::string_view kIconsNerd = "nerd";
constexpr std::string_view kIconsOff = "off";

std::string safe(std::string_view raw) {
    return terminal_safe(raw);
}

// `list_share`: 0..1 arası sonlu sayı; değilse yok sayılır, config'in oranı kalır.
std::optional<double> valid_share(std::optional<double> share) {
    if (!share || (std::isfinite(*share) && *share >= 0.0 && *share <= 1.0))
        return share;
    core::log_warn("event=tui_state_value_ignored key=list_share");
    return std::nullopt;
}

app::UiPrefsRules prefs_rules(const SearchHistoryLimits& history, const core::Limit& recent_commands,
                              const core::Limit& max_bytes) {
    return {.keep_history = [history](std::vector<std::string> entries) {
                return kept_search_history(std::move(entries), history);
            },
            .recent_commands = recent_commands,
            .max_bytes = max_bytes};
}

ListPrefs list_prefs_of(app::UiPrefs read) {
    ListPrefs prefs;
    prefs.list_share = valid_share(app::pref_number(read, kListShare));
    read.extra.erase(std::string(kListShare));
    static_cast<app::UiPrefs&>(prefs) = std::move(read);
    return prefs;
}

app::UiPrefs ui_prefs_of(const ListPrefs& prefs) {
    app::UiPrefs out = prefs;
    if (prefs.list_share)
        out.extra[std::string(kListShare)] = *prefs.list_share;
    return out;
}

app::Prompts confirmed() {
    return {[](const app::Confirm&) { return true; }};
}

// Onay metni ve başlığı app::confirm_text'te (GUI ile aynı); ne zaman sorulacağı app::TrashOps'ta.
ConfirmState confirm_state(const app::Confirm& request) {
    app::ConfirmText text = app::confirm_text(request);
    return ConfirmState(terminal_safe(text.message), std::move(text.title));
}

}  // namespace

std::string format_list_prefs(const ListPrefs& prefs) {
    return app::format_ui_prefs(ui_prefs_of(prefs), {});
}

ListPrefs parse_list_prefs(std::string_view text, const SearchHistoryLimits& history) {
    return list_prefs_of(app::parse_ui_prefs(text, prefs_rules(history, {}, {})));
}

std::string load_list_prefs(TuiState& state, const core::Limit& max_bytes) {
    app::LoadedUiPrefs loaded = app::load_ui_prefs(
        kPrefsFile, prefs_rules(state.search_history_limits(), state.recent_commands_limit(), max_bytes));
    state.set_list_prefs(list_prefs_of(std::move(loaded.prefs)));
    return std::move(loaded.notice);
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
        // Onay modaldır; bu arada tarama (senkron) listeyi değiştirebilir. Evet, sorudaki sayıları üreten listeyle
        // çalışır: gösterilmeyen not silinmez, çöpe gitmez.
        asked_listing_.emplace(host_.state.listing());
        return false;
    }};
}

void ListActions::begin_trash(std::optional<ConfirmState>& confirm) {
    if (host_.state.marked_count() > 0) {
        // Hedefler onay anında alınır: Evet canlı seçimi okumaz (senkron işaretleri düşürmüş olabilir).
        std::vector<app::TrashTarget> targets = app::trash_targets(host_.state.action_items());
        trash_marked(targets, ask_later(confirm, [this, targets] { trash_marked(targets, confirmed()); }));
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
    // Hedefler ve görünüm onay anında alınır: Evet canlı seçimi okumaz (senkron işaretleri düşürmüş olabilir).
    std::vector<app::TrashTarget> targets = app::trash_targets(host_.state.action_items());
    const bool from_trash = host_.state.trash_view();
    delete_marked(targets, from_trash,
                  ask_later(confirm, [this, targets, from_trash] { delete_marked(targets, from_trash, confirmed()); }));
}

void ListActions::restore_action() {
    if (host_.state.marked_count() > 0)
        restore_marked();
    else
        restore_selected();
}

// Her öğe denenir: biri düşerse kalanlar sürer, atılanlar `u` ile döner.
void ListActions::trash_marked(const std::vector<app::TrashTarget>& targets, const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const auto report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.trash_items(*host_.store, shown_listing(), targets, prompts);
    }();
    if (!report)
        return;  // onay bekliyor
    host_.state.clear_marks();
    host_.refresh_notes();
    host_.state.select_index(index);
    publish(*report, "tui.batch_trashed");
}

void ListActions::delete_marked(const std::vector<app::TrashTarget>& targets, bool from_trash,
                                const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const auto report = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.delete_items(*host_.store, shown_listing(), targets, from_trash, prompts);
    }();
    if (!report)
        return;  // onay bekliyor
    host_.state.clear_marks();
    host_.refresh_notes();
    refresh_trash_view();
    host_.state.select_index(index);
    publish(*report, "app.batch_deleted");
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
    publish(report, "app.paste_done");
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm,
                      std::optional<RenameState>& rename) {
    if (intent == Intent::LinkNote)
        begin_link(rename);
    else if (intent == Intent::ExportPdf)
        begin_export(rename);
    else if (intent == Intent::RenameRelation)
        begin_relation_rename(rename);
    else if (intent == Intent::SaveView || intent == Intent::RemoveView)
        begin_view_name(rename, intent == Intent::SaveView);
    else
        run_intent(intent, confirm, &rename);
}

void ListActions::run(Intent intent, std::optional<ConfirmState>& confirm) {
    run_intent(intent, confirm, nullptr);
}

void ListActions::run_intent(Intent intent, std::optional<ConfirmState>& confirm,
                             std::optional<RenameState>* rename) {
    pending_ = nullptr;
    asked_listing_.reset();
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
        if (host_.state.pdf_page_view())
            host_.status.message = core::tr("tui.pdf_page_no_text");
        else
            copy_selected();
        break;
    case Intent::PdfPageNoText:
        host_.status.message = core::tr("tui.pdf_page_no_text");
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
        apply_map_edit(rename);
        break;
    case Intent::MapNotice:
        if (const auto notice = host_.state.take_map_notice())
            host_.status.notify(notice->kind, terminal_safe(notice->text));
        break;
    case Intent::MapRelationFilter: {
        const auto names = [&] {
            const std::scoped_lock lock(host_.store_mutex);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            return host_.store->relation_names();
        }();
        if (!names)
            host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(names.error())));
        else
            host_.state.begin_map_relation_filter(**names);
        break;
    }
    case Intent::TrashMapNode:  // işaretler değil: haritanın hedefi seçili satır oldu
        trash_selected();
        break;
    case Intent::InspectorLoad:
        load_inspector();
        break;
    case Intent::InspectorAdd:
    case Intent::InspectorEdit:
        if (rename != nullptr)
            begin_inspector_edit(*rename, intent == Intent::InspectorAdd);
        break;
    case Intent::InspectorDelete:
        begin_inspector_delete(confirm);
        break;
    case Intent::RunMapQuery:
        run_map_query(confirm);
        break;
    case Intent::FlipRelation:
    case Intent::UndirectRelation:
        direct_selected_relation(intent == Intent::FlipRelation);
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
        apply_map_edit(nullptr);
        break;
    case MouseResult::PdfPageNoText:
        host_.status.message = core::tr("tui.pdf_page_no_text");
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
    app::ClipboardText content = app::clipboard_text(*host_.store, row->path);
    if (content.error)
        host_.status.message = core::format_error(*content.error);
    else if (content.refused != nullptr)
        host_.status.message = core::tr(content.refused);
    else
        copy_text(std::move(content.text));
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
        host_.status.message = core::tr("app.copy_refused_utf8");
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
        return trash_.trash_folder(*host_.store, shown_listing(), folder, prompts);
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
        return from_trash ? trash_.purge(store, target, prompts)
                          : trash_.remove(store, shown_listing(), target, prompts);
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

void ListActions::begin_view_name(std::optional<RenameState>& rename, bool save) {
    view_removal_.clear();
    view_shown_.reset();
    if (!save) {
        rename.emplace(RenameState::ViewName{}, std::string{}, host_.state.views());
        return;
    }
    const std::string& query = host_.state.map_query().read_query();
    if (query.empty()) {
        host_.status.message = core::tr("error.view_needs_query");
        return;
    }
    rename.emplace(RenameState::ViewName{}, query, std::vector<std::string>{});
}

void ListActions::submit_view(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    const std::string name = input.text();
    const bool save = !input.view_query().empty();
    if (!save && view_removal_ != name) {  // silme geri alınamaz: aynı adda ikinci Enter
        auto shown = [&] {
            const std::scoped_lock lock(host_.store_mutex);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            return core::find_view(*host_.store, name);
        }();
        if (!shown) {
            input.set_error(terminal_safe(core::format_error(shown.error())));
            return;
        }
        view_removal_ = name;
        view_shown_ = std::move(*shown);  // uyarının gösterdiği görünüm; silme onu denetler
        input.set_error(core::tr("tui.view_remove_again", {{"name", terminal_safe(name)}}));
        return;
    }
    const auto done = [&]() -> std::expected<void, core::Error> {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        core::NoteStore& store = *host_.store;
        auto written = save ? host_.state.map_query().save_view(store, name)
                            : core::remove_view(store, name, view_shown_);
        if (written)
            host_.state.set_views(app::view_names(store));
        return written;
    }();
    view_removal_.clear();
    view_shown_.reset();
    if (!done) {
        input.set_error(terminal_safe(core::format_error(done.error())));
        return;
    }
    rename.reset();
    host_.status.notify(
        app::NoticeKind::Success,
        terminal_safe(core::tr(save ? "app.view_saved" : "app.view_removed", {{"name", name}})));
    host_.notify_changed();
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
    const bool esc = key == Key::Escape && (focus == Focus::List || focus == Focus::Preview ||
                                            focus == Focus::Links || focus == Focus::Inspector);
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
    // Hedef GUI'deki gibi [[bağlantı]] kuralıyla çözülür (tam yol, uzantısız yol, dosya adı, yalın ad).
    const std::optional<core::NotePath> target = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->resolve_link(input.target());
    }();
    if (!target) {
        input.set_error(core::format_error(
            core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", input.target()}}}));
        return;
    }
    // Hedef ad sorulmadan önce doğrulanır: yanlış yol ad yazıldıktan sonra değil, burada söylenir.
    const auto names = [&]() -> std::expected<RenameState::SharedRelationNames, core::Error> {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        if (const auto links = host_.store->links(*target); !links)
            return std::unexpected(links.error());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->relation_names();
    }();
    if (!names) {
        input.set_error(core::format_error(names.error()));
        return;
    }
    const core::NotePath from = input.from();
    rename.emplace(RenameState::RelationName{}, from, *target, *names);
}

void ListActions::open_relation_line(std::optional<RenameState>& rename, const core::NotePath& from,
                                     const core::NotePath& to, std::optional<core::RelationId> renaming,
                                     std::string current) {
    const auto names = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return host_.store->relation_names();
    }();
    if (!names) {
        host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(names.error())));
        return;
    }
    rename.emplace(RenameState::RelationName{}, from, to, *names, renaming, std::move(current));
}

// Bağlantı bölümünün seçili ilişkisi: `shown` gösterilen not, satır o ilişkinin kimliğini taşır.
std::optional<std::pair<core::NotePath, core::RelatedNote>> ListActions::selected_relation() const {
    const Row* row = host_.state.selected_row();
    const core::LinkLine* line = host_.state.link_panel().selected_line();
    if (!row || !line || line->is_header || line->group != core::LinkGroup::Related)
        return std::nullopt;
    return std::pair(row->path, host_.state.link_panel().links().related[line->index]);
}

void ListActions::begin_relation_rename(std::optional<RenameState>& rename) {
    if (const auto selected = selected_relation())
        open_relation_line(rename, selected->first, selected->second.path, selected->second.id,
                           selected->second.name);
}

void ListActions::direct_selected_relation(bool flip) {
    const auto selected = selected_relation();
    if (!selected)
        return;
    const auto& [shown, relation] = *selected;
    const std::optional<core::NotePath> source =
        flip ? std::optional(app::flipped_source(shown, relation.path, relation.direction)) : std::nullopt;
    finish_relation_edit([&](core::NoteStore& store) {
        return app::direct_map_relation(store, shown, relation.path, relation.id, source);
    });
}

// İlişki işinin ortak sonu: ileti bildirime, harita kenarları ve liste tazelenir, senkron haber alır.
bool ListActions::finish_relation_edit(
    const std::function<std::expected<std::string, core::Error>(core::NoteStore&)>& edit,
    RenameState* input) {
    const auto done = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return edit(*host_.store);
    }();
    if (!done) {
        if (input != nullptr)
            input->set_error(core::format_error(done.error()));
        else
            host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(done.error())));
        return false;
    }
    host_.status.notify(app::NoticeKind::Success, terminal_safe(*done));
    host_.state.map_edges_changed();  // ilişki listeyi değiştirmez: kenarlar sonraki çizimde okunur
    host_.refresh_notes();
    host_.notify_changed();
    return true;
}

// Ad satırının Enter'ı: yeni ilişki (ad, "yönlü" from → to) ya da var olanın adı. Hata satırda kalır.
void ListActions::submit_relation(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    const bool done = finish_relation_edit(
        [&](core::NoteStore& store) {
            if (const auto& renaming = input.renaming())
                return app::rename_map_relation(store, input.from(), input.relation_to(), *renaming,
                                                input.text());
            return app::relate_map_notes(store, {app::MapNodeKind::Note, input.from().string()},
                                         {app::MapNodeKind::Note, input.relation_to().string()},
                                         {input.text(), input.directed()});
        },
        &input);
    if (done)
        rename.reset();
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
    rename.emplace(RenameState::ExportTarget{}, row->path, app::default_export_name(row->path));
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
    if (!warned && app::export_target_exists(output)) {
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
    host_.status.notify(app::NoticeKind::Success, core::tr("app.exported", {{"path", shown_path}}));
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
    return app::export_note_pdf(content->text, note, output);
}

void ListActions::begin_unlink(std::optional<ConfirmState>& confirm) {
    const Row* row = host_.state.selected_row();
    const LinkPanel& panel = host_.state.link_panel();
    const core::LinkLine* line = panel.selected_line();
    if (!row || !line || line->is_header || line->group != core::LinkGroup::Related)
        return;
    const core::RelatedNote& relation = panel.links().related[line->index];
    relation_target_ = std::pair(row->path, relation);
    confirm.emplace(terminal_safe(app::unlink_question(row->path, relation)), core::tr("tui.unlink_title"));
}

// GUI ile tek yol (app::unrelate_map_notes): onay açıkken ilişki gittiyse relation_not_found.
void ListActions::finish_unlink() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- finish_delete denetledi
    const auto [from, relation] = *std::exchange(relation_target_, std::nullopt);
    finish_relation_edit([&](core::NoteStore& store) {
        return app::unrelate_map_notes(store, {app::MapNodeKind::Note, from.string()},
                                       {app::MapNodeKind::Note, relation.path.string()}, relation.id);
    });
}

// Liste odağına editör park edilerek (kayıtla) gelinir, ayrılan sekme de kaydedilir: yazılacak notun
// bekleyen düzenlemesi yoktur. refresh_notes'tan sonra main her olayda sekmeleri tazeler (settle_tabs →
// EditorTabs::on_listing): temiz sekme yeni metni okur, yine de kirli kalmış sekme ezilmez, kaydı çakışma
// sorar.
// İlişki kurma ve yeniden adlandırma ad satırı açar (F1); `rename` yoksa (fare yolu) ilişki adsız kurulur.
void ListActions::apply_map_edit(std::optional<RenameState>* rename) {
    using Action = TuiState::MapEditAction;
    const auto request = host_.state.take_map_edit();
    if (!request)
        return;
    if (!request->kind) {
        host_.status.message = terminal_safe(
            core::tr("tui.map_no_edge", {{"from", request->from.key}, {"to", request->to.key}}));
        return;
    }
    const bool asks_name = request->action == Action::Rename ||
                           (request->action == Action::Connect && *request->kind == app::MapEdgeKind::Related);
    if (asks_name && rename != nullptr) {
        const auto from = core::NotePath::parse(request->from.key);
        const auto to = core::NotePath::parse(request->to.key);
        if (from && to)
            open_relation_line(*rename, *from, *to,
                               request->action == Action::Rename ? std::optional(request->relation) : std::nullopt,
                               request->name);
        return;
    }
    if (request->action == Action::Rename)
        return;
    finish_relation_edit([&](core::NoteStore& store) -> std::expected<std::string, core::Error> {
        if (request->action == Action::Connect)
            return app::connect_map_nodes(store, request->from, request->to, *request->kind);
        if (request->action == Action::Remove)
            return app::disconnect_map_nodes(store, request->from, request->to, *request->kind,
                                             request->relation);
        // Direct: uçlar not düğümüdür (yalnız ilişkide istenir).
        const auto from = core::NotePath::parse(request->from.key);
        const auto to = core::NotePath::parse(request->to.key);
        if (!from || !to)
            return std::unexpected(from ? to.error() : from.error());
        std::optional<core::NotePath> source;
        if (request->source) {
            auto parsed = core::NotePath::parse(request->source->key);
            if (!parsed)
                return std::unexpected(parsed.error());
            source = std::move(*parsed);
        }
        return app::direct_map_relation(store, *from, *to, request->relation, source);
    });
}

// Harita sorgusu (F3; akış app::MapQuery, GUI ile ortak): okuma hemen; yazma önizlenir ve onaya sorulur
// (varsayılan Hayır), Evet'te finish_delete `pending_`ı koşar. Depo işi kilit altında.
void ListActions::run_map_query(std::optional<ConfirmState>& confirm) {
    app::MapQuery& query = host_.state.map_query();
    const app::MapQuery::Step step = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return query.run(*host_.store, host_.state.map_query_text());
    }();
    if (step != app::MapQuery::Step::Confirm) {
        show_map_query(step);
        return;
    }
    confirm.emplace(query.confirm_text(), core::tr("app.query_confirm_title"), core::tr("app.query_apply"));
    pending_ = [this] {
        const app::MapQuery::Step applied = [&] {
            const std::scoped_lock lock(host_.store_mutex);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
            return host_.state.map_query().apply(*host_.store);
        }();
        show_map_query(applied);
    };
}

// Sonuç: yazmadan sonra liste ve kenarlar yeniden okunur, vurgu haritaya, özet alt satıra. Hata sorgu satırını
// metin ve konumlu iletiyle yeniden açar; dar liste panelinde ileti kesilebilir, tamamı alt satırdadır.
void ListActions::show_map_query(app::MapQuery::Step step) {
    if (step == app::MapQuery::Step::Failed) {
        host_.state.map_query_failed();
        host_.status.notify(app::NoticeKind::Error, terminal_safe(host_.state.map_query().error()));
        return;
    }
    const auto& outcome = host_.state.map_query().outcome();
    if (step != app::MapQuery::Step::Ran || !outcome)
        return;
    if (outcome->wrote) {
        host_.refresh_notes();
        host_.state.map_edges_changed();  // ilişki listeyi değiştirmez
        host_.notify_changed();
    }
    host_.state.map_query_finished();
    host_.status.notify(outcome->wrote ? app::NoticeKind::Success : app::NoticeKind::Info,
                        terminal_safe(outcome->summary));
}

void ListActions::finish_delete(std::optional<ConfirmState>& confirm) {
    confirm.reset();
    if (relation_target_) {
        finish_unlink();
        return;
    }
    if (const std::function<void()> retry = std::exchange(pending_, nullptr))
        retry();
    asked_listing_.reset();
}

const app::Listing& ListActions::shown_listing() const {
    return asked_listing_ ? *asked_listing_ : host_.state.listing();
}

// --- Özellik denetçisi (F4; model app::Inspector, GUI ile ortak) ---

void ListActions::load_inspector() {
    const std::scoped_lock lock(host_.store_mutex);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
    host_.state.inspector().reload(*host_.store);
}

void ListActions::refresh_inspector() {
    if (host_.state.focus() == Focus::Inspector && host_.state.inspector().target())
        load_inspector();
}

// Ekleme anahtar satırıyla başlar; düzenleme seçili özelliğin değer satırıdır. Salt okunur satır ve etiket
// düzenlenmez: neden alt satırda.
void ListActions::begin_inspector_edit(std::optional<RenameState>& rename, bool add) {
    const app::Inspector& inspector = host_.state.inspector();
    if (!inspector.target())
        return;
    if (add) {
        rename.emplace(RenameState::PropertyLine{}, std::string(), std::string());
        return;
    }
    const app::InspectorRow* row = inspector.selected_row();
    if (row == nullptr)
        return;
    if (row->read_only || row->kind != app::InspectorRow::Kind::Property) {
        host_.status.message = core::tr("inspector.read_only");
        return;
    }
    rename.emplace(RenameState::PropertyLine{}, row->key, row->input);
}

void ListActions::begin_inspector_delete(std::optional<ConfirmState>& confirm) {
    const app::InspectorRow* selected = host_.state.inspector().selected_row();
    if (selected == nullptr)
        return;
    if (selected->read_only) {
        host_.status.message = core::tr("inspector.read_only");
        return;
    }
    const app::InspectorRow row = *selected;  // onaya dek satırlar yeniden okunabilir: kopya
    confirm.emplace(terminal_safe(host_.state.inspector().confirm_remove_text(row)),
                    core::tr("inspector.remove_title"), core::tr("inspector.delete"));
    pending_ = [this, row] {
        finish_inspector_write([&](core::NoteStore& store, const app::SavePending& save_pending) {
            app::Inspector& inspector = host_.state.inspector();
            return row.kind == app::InspectorRow::Kind::Label
                       ? inspector.remove_label(store, row.key, save_pending)
                       : inspector.remove(store, row.key);
        });
    };
}

// Anahtar satırı: `#` etiket ekler (notun metnine), başka metin değer satırını açar. Değer satırı yazar; hata
// satırda kalır, yazılmaz.
void ListActions::submit_property(std::optional<RenameState>& rename) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    RenameState& input = *rename;
    if (input.property_key().empty()) {
        const std::string key = input.text();
        if (key.empty())
            return;
        if (!key.starts_with('#')) {
            rename.emplace(RenameState::PropertyLine{}, key, std::string());
            return;
        }
        if (finish_inspector_write(
                [&](core::NoteStore& store, const app::SavePending& save_pending) {
                    return host_.state.inspector().add_label(store, key, save_pending);
                },
                &input))
            rename.reset();
        return;
    }
    const std::string key = input.property_key();
    const std::string text = input.text();
    if (finish_inspector_write(
            [&](core::NoteStore& store, const app::SavePending&) {
                return host_.state.inspector().set(store, key, text);
            },
            &input))
        rename.reset();
}

// Yazmanın ortak sonu: ileti bildirime (anahtar ve değer güvenilmez: terminal_safe), liste ve senkron haber
// alır. Etiket notun metnini değiştirir: tarama sekmeleri tazeler (temiz sekme yeni metni alır, kirli sekme
// ezilmez).
bool ListActions::finish_inspector_write(
    const std::function<std::expected<std::string, core::Error>(core::NoteStore&, const app::SavePending&)>&
        write,
    RenameState* input) {
    const auto done = [&] {
        std::unique_lock lock(host_.store_mutex);
        // Sekmenin kaydı aynı kilidi alır (ve listeyi tazeler): kaydederken kilit bırakılır.
        const app::SavePending save_pending = [&](const core::NotePath& note) {
            if (!host_.save_note)
                return true;
            lock.unlock();
            const bool saved = host_.save_note(note);
            lock.lock();
            return saved;
        };
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return write(*host_.store, save_pending);
    }();
    if (!done) {
        if (input != nullptr)
            input->set_error(terminal_safe(core::format_error(done.error())));
        else
            host_.status.notify(app::NoticeKind::Error, terminal_safe(core::format_error(done.error())));
        return false;
    }
    host_.status.notify(app::NoticeKind::Success, terminal_safe(*done));
    host_.state.map_edges_changed();  // etiket haritada kenardır
    host_.refresh_notes();
    host_.notify_changed();
    return true;
}

void ListActions::delete_folder(const std::string& folder, const app::Prompts& prompts) {
    const std::size_t index = host_.state.selected();
    const auto removed = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return trash_.remove_folder(*host_.store, shown_listing(), folder, prompts);
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

// GUI ile tek kural (app::quick_add): ad kırpılır; klasör görünümünde geçerli klasöre, değilse yeni notun
// klasörüne göredir. Var olan klasör seçilir ve bunu söyler.
void ListActions::submit_new_row(std::optional<NewRowState>& row, const NewRowDefaults& defaults,
                                 const std::function<void(const core::NotePath&)>& open) {
    const std::string base = host_.state.in_folder_view() ? host_.state.name_base_folder() : defaults.folder;
    const auto added = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
        return app::quick_add(*host_.store, host_.state.folder_tree(), row->text(), base, defaults.type,
                              defaults.slug_max_length);
    }();
    if (!added) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız new_row doluyken çağırır
        row->set_error(core::format_error(added.error()));
        return;
    }
    row.reset();
    switch (added->kind) {
    case app::QuickAdd::Kind::Nothing:
    case app::QuickAdd::Kind::Kept:  // TUI before_note vermez
        return;
    case app::QuickAdd::Kind::FolderExists:
        host_.state.select_folder(added->folder);
        host_.status.message = terminal_safe(added->message);
        return;
    case app::QuickAdd::Kind::FolderCreated:
        host_.refresh_notes();
        host_.state.select_folder(added->folder);
        host_.status.message = terminal_safe(added->message);
        host_.notify_changed();
        return;
    case app::QuickAdd::Kind::NoteCreated:
        break;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- NoteCreated'de not dolu
    const core::NotePath path = added->note->path;
    host_.refresh_notes();
    host_.state.map_note_created(path);  // haritadan `n`: not haritada kalır, yerine sabitlenir
    host_.state.select_note(path);       // süzgeç notu gizliyorsa temizlenir
    open(path);
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
    if (rename->is_property()) {
        submit_property(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_view()) {
        submit_view(rename);
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- main yalnız rename doluyken çağırır
    if (rename->is_relation()) {
        submit_relation(rename);
        return;
    }
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

void ListActions::restore_map_view() {
    const app::MapSession& map = host_.state.list_prefs().map;
    if (!map.shown || map.view.empty())
        return;
    host_.state.map_query().begin_restore(map.view);
    if (!host_.post)
        return;
    const core::NoteStore store = [&] {
        const std::scoped_lock lock(host_.store_mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store main'de kurulu, hep dolu
        return *host_.store;
    }();
    view_restore_.start(store, map.view, [this](app::ViewRestore::Result done) {
        host_.post([this, done = std::move(done)]() mutable { finish_map_restore(std::move(done)); });
    });
}

void ListActions::finish_map_restore(app::ViewRestore::Result done) {
    const app::MapQuery::Restored restored = host_.state.map_query().restored(std::move(done));
    if (restored.kind == app::MapQuery::Restored::Kind::Failed)
        host_.status.notify(app::NoticeKind::Error, terminal_safe(restored.notice));
    else if (restored.kind == app::MapQuery::Restored::Kind::Ran)
        show_map_query(app::MapQuery::Step::Ran);
}

void ListActions::save_prefs_if_changed() {
    if (host_.state.take_prefs_changed())
        save_prefs();
}

// Ayarlar ekranı ve GUI ile tek yol (app::write_settings): yalnız `icons` diskteki dosyanın üstüne yazılır,
// okunamayan config.toml ezilmez; yazılamazsa eski değer kalır.
void ListActions::toggle_icons() {
    if (!host_.config)
        return;
    const std::string next(host_.config->icons == kIconsOff ? kIconsNerd : kIconsOff);
    if (const auto written = app::write_settings({{"icons", next}}); !written) {
        host_.status.message = terminal_safe(written.error());
        return;
    }
    host_.config->icons = next;
    host_.state.set_icon_style(core::terminal_icon_style(next));
}

void ListActions::save_prefs() {
    host_.state.prune_open_folders();
    app::save_ui_prefs(kPrefsFile, ui_prefs_of(host_.state.list_prefs()),
                       app::open_folder_filter(host_.state.folder_tree()));
}

}  // namespace noteapp::tui
