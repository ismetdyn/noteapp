#include "app/item_ops.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/gui_state_internal.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr int kDetailsColumnCount = static_cast<int>(std::size(kDetailsColumnNames));

std::vector<app::TrashTarget> trash_targets(const std::vector<app::ItemRef>& items) {
    std::vector<app::TrashTarget> targets;
    targets.reserve(items.size());
    for (const app::ItemRef& item : items)
        targets.push_back({item.path, item.is_folder});
    return targets;
}

}  // namespace

// ---- Satır komutları ----

void GuiState::toggle_row() {
    if (trash_view_) {
        toggle_info();
        return;
    }
    if (selected_folder_) {
        const std::string folder = *selected_folder_;  // kopya: açma kaydı seçimi değiştirmez ama güvenli
        toggle_expanded(folder);
        return;
    }
    if (current_path())
        toggle_info();
}

void GuiState::toggle_tree_all() {
    std::set<std::string> all;
    for (std::string& folder : model_.listing().tree().folders_under({}))
        if (!folder.empty())
            all.insert(std::move(folder));
    const bool all_open = std::ranges::includes(model_.listing().open_folders(), all);
    model_.listing().set_open_folders(all_open ? std::set<std::string>{} : std::move(all));
    rows_stale_ = true;
    save_gui_state();
}

void GuiState::toggle_details_all() {
    if (trash_view_) {
        toggle_info();
        return;
    }
    set_browser_view(layout_.browser_view == BrowserView::Details ? BrowserView::Tree : BrowserView::Details);
}

void GuiState::go_root() {
    close_trash();
    clear_query();
    if (search_mode_ != app::SearchMode::Name)
        set_search_mode(app::SearchMode::Name);
    enter_folder("");
}

void GuiState::set_details_column(DetailsColumn column) {
    layout_.column = column;
    save_gui_state();
}

void GuiState::cycle_details_column() {
    set_details_column(
        static_cast<DetailsColumn>((static_cast<int>(layout_.column) + 1) % kDetailsColumnCount));
}

// ---- Çoklu seçim ----

bool GuiState::is_marked(const std::string& path, bool is_folder) const {
    return trash_view_ ? trash_marks_.contains(path) : marks_.contains(app::mark_key(path, is_folder));
}

void GuiState::toggle_mark() {
    std::string key;
    if (trash_view_) {
        if (trash_row_ < 0)
            return;
        key = trash_items_[static_cast<std::size_t>(trash_row_)].path.string();
    } else if (selected_folder_) {
        if (selected_folder_->empty())  // kök "Tümü" her şeydir: seçilmez
            return;
        key = app::mark_key(*selected_folder_, true);
    } else if (const auto path = current_path()) {
        key = app::mark_key(path->string(), false);
    } else {
        return;
    }
    if (std::set<std::string>& marks = active_marks(); !marks.erase(key))
        marks.insert(std::move(key));
    navigate(1);  // TUI `x` gibi: art arda işaretlenebilsin
}

void GuiState::mark_all() {
    if (trash_view_) {
        for (const core::TrashItem& item : trash_items_)
            trash_marks_.insert(item.path.string());
        return;
    }
    if (browsing()) {
        for (const BrowserRow& row : browser_rows())
            if (row.kind == BrowserRowKind::Note || !row.path.empty())
                marks_.insert(app::mark_key(row.path, row.kind == BrowserRowKind::Folder));
        return;
    }
    for (int row = 0; row < model_.row_count(); ++row)
        marks_.insert(app::mark_key(model_.note_at(row).path.string(), false));
}

// Gitmiş öğenin işareti düşer (senkron, başka süreç): toplu işlem onu hata diye raporlamasın.
void GuiState::prune_marks() {
    const app::Listing& listing = model_.listing();
    std::erase_if(marks_, [&](const std::string& key) {
        if (key.ends_with('/'))
            return listing.tree().find(std::string_view(key).substr(0, key.size() - 1)) == nullptr;
        const auto path = core::NotePath::parse(key);
        return !path || listing.meta_of(*path) == nullptr;
    });
    std::erase_if(trash_marks_, [&](const std::string& key) {
        return std::ranges::none_of(trash_items_,
                                    [&](const core::TrashItem& item) { return item.path.string() == key; });
    });
}

std::vector<app::ItemRef> GuiState::action_items() {
    prune_marks();
    if (!marks_.empty())
        return app::marked_items(marks_);
    if (selected_folder_) {
        if (!folder_action_allowed())
            return {};
        if (auto folder = core::NotePath::parse(*selected_folder_))
            return {{std::move(*folder), true}};
        return {};
    }
    if (auto path = current_path())
        return {{std::move(*path), false}};
    return {};
}

std::vector<core::NotePath> GuiState::trash_action_items() const {
    std::vector<core::NotePath> items;
    if (trash_marks_.empty()) {
        if (trash_row_ >= 0)
            items.push_back(trash_items_[static_cast<std::size_t>(trash_row_)].path);
        return items;
    }
    for (const std::string& key : trash_marks_)
        if (auto path = core::NotePath::parse(key))
            items.push_back(std::move(*path));
    return items;
}

// Toplu işlemin iletisi: yapılan sayısı ve ilk hata (TUI'deki rapor satırı).
void GuiState::publish(const app::BatchReport& report, const char* done_key) {
    status_ = core::tr(done_key, {{"count", std::to_string(report.done)}});
    const auto failure =
        std::ranges::find_if(report.lines, [](const auto& line) { return line.error.has_value(); });
    if (failure != report.lines.end())
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- find_if hatalı satırı seçti
        status_ += " " + core::tr("gui.batch_failed", {{"count", std::to_string(report.failed)},
                                                       {"path", failure->path},
                                                       {"error", core::format_error(*failure->error)}});
}

// İşaretliler çöpe (onaysız, tek adımda geri alınır) ya da kalıcı (onaylı); notlu klasör çöpe giderken
// sorulur.
void GuiState::delete_marked(bool permanent) {
    if (!flush_shown())  // gösterilen notun son hâli gitsin
        return;
    const std::vector<app::TrashTarget> targets = trash_targets(action_items());
    if (targets.empty())
        return;
    // Gösterilen not da gidebilir: tek notluk silmedeki gibi imlecin sırası işlemden önce alınır.
    const std::optional<core::NotePath> shown = shown_ ? std::optional(shown_->path) : std::nullopt;
    const int row = shown ? std::max(0, model_.index_of(*shown)) : 0;
    const int browser_index = browsing() ? browser_row() : -1;
    const auto report = permanent
                            ? trash_.delete_items(store_, model_.listing(), targets, false, trash_prompts())
                            : trash_.trash_items(store_, model_.listing(), targets, trash_prompts());
    if (!report)
        return;  // vazgeçildi
    marks_.clear();
    if (report->wrote || report->done > 0)
        changed();
    // Giden gösterilen not açıkça bırakılır (sekmesi kapanır, imleç aynı sıradaki nota iner); taramaya
    // kalsaydı sekme "artık yok" diye kapanır, seçim listenin başına düşerdi.
    if (shown && !store_.read(*shown))
        select_after_removal(true, row, browser_index);
    else
        rescan();  // atılan notların arka plan sekmeleri kapanır
    publish(*report, permanent ? "gui.batch_deleted" : "gui.batch_trashed");
    if (!permanent && report->done > 0)
        undo_status_ = status_;
}

// ---- Yank ve yapıştır ----

void GuiState::yank() {
    if (trash_view_)
        return;
    std::vector<app::ItemRef> items = action_items();
    if (items.empty())
        return;
    yank_ = std::move(items);
    marks_.clear();
    status_ = core::tr("gui.yanked", {{"count", std::to_string(yank_.size())}});
}

void GuiState::paste() {
    if (yank_.empty()) {
        status_ = core::tr("gui.paste_nothing");
        return;
    }
    if (!browsing()) {
        status_ = core::tr("gui.paste_needs_folder");
        return;
    }
    const std::string dest = target_folder();
    const std::vector<app::ItemRef> items = std::exchange(yank_, {});
    const app::BatchReport report = app::paste_items(store_, items, dest);
    if (report.wrote)
        changed();
    rescan();
    set_expanded(dest, true);  // kopyalar ağaçta görünsün
    publish(report, "gui.paste_done");
}

// ---- Taşıma ----

bool GuiState::can_move() {
    if (trash_view_)
        return false;
    if (!marks_.empty())
        return true;
    return selected_folder_ ? folder_action_allowed() : shown_.has_value() && !new_note_;
}

void GuiState::move_selected() {
    if (trash_view_)
        return;
    if (marks_.empty() && !selected_folder_) {
        move_note();
        return;
    }
    const std::vector<app::ItemRef> items = action_items();
    if (items.empty() || !flush_shown())
        return;
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Move;
    request.folder_only = true;
    request.initial = {core::last_component(items.front().path.string()), current_folder_, {}};
    request.folders = folders_;
    request.types = config_.types;
    const auto chosen = deciders.save_dialog(request);
    if (!chosen)
        return;
    const app::MoveResult moved = app::move_items(store_, model_.listing(), items, chosen->folder);
    if (moved.moved > 0)
        changed();
    // Açık notlar ve sekmeler yeni yolu izler: kayıt eski yola gitmesin. Öğeler sırayla taşınır, yerinde
    // kalan (hedefi kendisi) sayılmaz; hata ilk taşınmayanda durur.
    std::size_t left = moved.moved;
    for (const app::ItemRef& item : items) {
        const std::string from = item.path.string();
        const std::string to = core::join_folder(chosen->folder, core::last_component(from));
        if (left == 0)
            break;
        if (to == from)
            continue;
        follow_moved(from, to);
        --left;
    }
    marks_.clear();
    rescan();
    if (moved.first) {
        if (moved.first->is_folder)
            select_folder(moved.first->path.string());
        else
            open_note(moved.first->path);
    }
    if (moved.failure)
        fail(*moved.failure);
    else
        status_ = core::tr("gui.moved_to", {{"folder", chosen->folder.empty() ? "/" : chosen->folder}});
}

// ---- Hızlı ekleme ----

void GuiState::begin_quick_add() {
    if (trash_view_)
        return;
    quick_add_open_ = true;
    quick_add_.clear();
    quick_add_error_.clear();
    request_focus(FocusRequest::QuickAdd);
}

void GuiState::cancel_quick_add() {
    quick_add_open_ = false;
    quick_add_.clear();
    quick_add_error_.clear();
    request_focus(FocusRequest::List);
}

// TUI `a` satırı: ad gezgindeki klasöre göredir; `/` ile biten ad klasör açar, var olan klasör seçilir. Dolu
// not adı `-2` almaz, hata satırda kalır.
void GuiState::submit_quick_add() {
    const std::size_t first = quick_add_.find_first_not_of(" \t");
    const std::string typed = first == std::string::npos
                                  ? std::string()
                                  : quick_add_.substr(first, quick_add_.find_last_not_of(" \t") - first + 1);
    if (typed.empty()) {
        cancel_quick_add();
        return;
    }
    if (typed.starts_with('/')) {  // baştaki `/` sessizce atılmasın: yol notların köküne görelidir
        quick_add_error_ = core::format_error(core::NotePath::parse(typed).error());
        return;
    }
    const std::string base = browsing() ? target_folder() : config_.default_folder;
    if (app::names_folder(typed)) {
        const std::string folder =
            core::join_folder(base, std::string_view(typed).substr(0, typed.size() - 1));
        if (model_.listing().tree().find(folder) == nullptr) {
            if (const auto made = store_.create_folder(folder); !made) {
                quick_add_error_ = core::format_error(made.error());
                return;
            }
            changed();
            rescan();
        }
        quick_add_open_ = false;
        set_expanded(core::parent_folder(folder), true);
        select_folder(folder);
        return;
    }
    if (!flush_shown())
        return;
    const auto created = app::create_named_note(store_, core::join_folder(base, typed), base,
                                                config_.default_type, config_.tuning.slug_max_length);
    if (!created) {
        quick_add_error_ = core::format_error(created.error());
        return;
    }
    quick_add_open_ = false;
    changed();
    rescan();
    open_note(created->path);
    request_focus(FocusRequest::Editor);
}

// ---- Çöp önizlemesi ----

void GuiState::load_trash_preview() {
    trash_preview_.clear();
    if (trash_row_ < 0)
        return;
    auto content = store_.read_trash(trash_items_[static_cast<std::size_t>(trash_row_)].path);
    if (!content) {
        fail(content.error());
        return;
    }
    trash_preview_ = std::move(content->text);
}

}  // namespace noteapp::gui
