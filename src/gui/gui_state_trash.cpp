#include "app/document.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::gui {

void GuiState::delete_note() {
    const auto path = current_path();
    if (!path)
        return;
    const bool shown = shown_ && shown_->path == *path;
    // Kendi kaydedilmemiş düzenlemesi sormadan önce kaydedilir ve liste tazelenir: silme yalnız sorulan hâli
    // (listedeki revizyon) siler, onay açıkken senkronla değişen not kalır (app::TrashOps::remove).
    if (shown) {
        if (!flush())
            return;
        rescan();
    }
    // flush mtime'ı değiştirip satırı yukarı taşımış olabilir: komşu satır silmeden önce çözülür.
    const int row = std::max(0, model_.index_of(*path));
    const int browser_index = browsing() ? browser_row() : -1;
    const auto removed = trash_.remove(store_, model_.listing(), *path, trash_prompts());
    if (!removed) {
        fail(removed.error());
        return;
    }
    if (!*removed)
        return;  // vazgeçildi
    changed();
    select_after_removal(shown, row, browser_index);
}

app::Prompts GuiState::trash_prompts(const std::function<bool()>& prepare) {
    return {[this, prepare](const app::Confirm& request) {
        // Tek klasörün kalıcı silinmesi yalnız çöpten (karar 1); seçimdeki klasör DeleteBatch ile sorulur.
        if (request.kind == app::ConfirmKind::DeleteFolder)
            return false;
        const app::ConfirmText text = app::confirm_text(request);  // TUI ile aynı metin ve başlık
        return deciders.confirm_delete(text.message, text.title) && (!prepare || prepare());
    }};
}

void GuiState::select_after_removal(bool was_shown, int row, int browser_row) {
    status_.clear();
    if (was_shown) {
        // Notun sekmesi kapanır; başka sekme varsa komşusu öne gelir, yoksa en yakın not yeni sekmede açılır.
        drop_active_tab();
        if (!tabs_.empty()) {
            rescan();
            return;
        }
    }
    rescan();
    if (browser_row >= 0 && select_nearest_note(browser_row))
        return;
    // Ayrıntılar yalnız doğrudan çocukları listeler: modelin alt ağaçtaki notu görünmeyen bir seçim olurdu.
    if (browser_row >= 0 && layout_.browser_view == BrowserView::Details) {
        show_folder(current_folder_, false);
        return;
    }
    if (const int count = model_.row_count(); count > 0)
        select_row(std::min(row, count - 1));
}

void GuiState::trash_note() {
    const auto path = current_path();
    if (!path)
        return;
    const bool shown = shown_ && shown_->path == *path;
    if (shown && !flush())  // çöpe son hâli gitsin
        return;
    const int row = std::max(0, model_.index_of(*path));
    const int browser_index = browsing() ? browser_row() : -1;
    const auto trashed = trash_.trash_note(store_, *path);
    if (!trashed) {
        fail(trashed.error());
        return;
    }
    changed();
    select_after_removal(shown, row, browser_index);
    undo_status_ = core::tr("gui.trashed", {{"path", path->string()}});
    status_ = undo_status_;
    notify(app::NoticeKind::Success, status_);
}

void GuiState::undo_trash() {
    if (!can_undo_trash())
        return;
    const auto undone = trash_.undo(store_);
    if (!undone)
        return;
    const auto failure = std::ranges::find_if(undone->report.lines,
                                              [](const auto& line) { return line.error.has_value(); });
    std::string restored;  // geri gelen klasör ya da not
    if (!undone->folder.empty())
        restored = undone->folder;
    else if (undone->first)
        restored = undone->first->string();
    // Klasör adımı klasörleri (boş alt klasörler dahil) ve notları birlikte getirir.
    if (undone->report.done > 0 || !undone->folder.empty()) {
        changed();
        rescan();
    }
    if (undone->first) {
        ensure_visible(*undone->first);
        select_row(model_.index_of(*undone->first));
    }
    if (failure != undone->report.lines.end()) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- find_if hatalı satırı seçti
        fail(*failure->error);  // teklif hata iletisiyle kalır: kilit kalkınca yeniden denenir
    } else {
        status_ = core::tr("gui.restored", {{"path", restored}});
        notify(app::NoticeKind::Success, status_);
    }
    // Yığın (plan-parity karar 2): önceki adım kaldıysa Geri al teklifi bu iletiyle sürer.
    undo_status_ = status_;
}

void GuiState::delete_selected(bool permanent) {
    if (trash_view_) {
        purge_trashed();
    } else if (!marks_.empty()) {
        delete_marked(permanent);
    } else if (selected_folder_) {
        if (!can_delete())  // kök ya da vurgulu satırı olmayan klasör
            return;
        const std::string folder = *selected_folder_;  // kopya: silme seçimi atasına taşır
        delete_folder(folder);
    } else if (permanent) {
        delete_note();
    } else {
        trash_note();
    }
}

void GuiState::reload_trash() {
    auto items = store_.list_trash();
    if (!items) {
        fail(items.error());
        items.emplace();
    }
    trash_items_ = std::move(*items);
    const int count = static_cast<int>(trash_items_.size());
    trash_row_ = count == 0 ? -1 : std::clamp(trash_row_, 0, count - 1);
    if (trash_view_)
        load_trash_preview();
}

void GuiState::open_trash() {
    trash_view_ = true;
    reload_trash();
    request_focus(FocusRequest::List);
}

void GuiState::toggle_trash() {
    if (trash_view_)
        close_trash();
    else
        open_trash();
}

void GuiState::select_trash_row(int row) {
    if (row >= 0 && row < static_cast<int>(trash_items_.size()) && row != trash_row_) {
        trash_row_ = row;
        load_trash_preview();
    }
}

void GuiState::restore_trashed() {
    if (!trash_marks_.empty()) {
        const app::BatchReport report = trash_.restore_items(store_, trash_action_items());
        trash_marks_.clear();
        if (report.done > 0)
            changed();
        rescan();
        publish(report, "gui.batch_restored");
        return;
    }
    if (trash_row_ < 0)
        return;
    const core::NotePath item = trash_items_[static_cast<std::size_t>(trash_row_)].path;
    const auto restored = trash_.restore(store_, item);
    if (!restored) {
        fail(restored.error());
        return;
    }
    changed();
    rescan();
    status_ = core::tr("gui.restored", {{"path", restored->string()}});
    notify(app::NoticeKind::Success, status_);
}

void GuiState::purge_trashed() {
    if (!trash_marks_.empty()) {
        std::vector<app::TrashTarget> targets;
        for (core::NotePath& item : trash_action_items())
            targets.push_back({std::move(item), false});
        const auto report = trash_.delete_items(store_, model_.listing(), targets, true, trash_prompts());
        if (!report)
            return;  // vazgeçildi
        trash_marks_.clear();
        if (report->done > 0)
            changed();
        rescan();
        publish(*report, "app.batch_deleted");
        return;
    }
    if (trash_row_ < 0)
        return;
    const core::NotePath item = trash_items_[static_cast<std::size_t>(trash_row_)].path;
    const auto purged = trash_.purge(store_, item, trash_prompts());
    if (!purged) {
        fail(purged.error());
        return;
    }
    if (!*purged)
        return;  // vazgeçildi
    changed();
    status_.clear();
    rescan();
}

void GuiState::empty_trash() {
    if (trash_items_.empty())
        return;
    const auto emptied = trash_.empty_trash(store_, trash_prompts());
    if (!emptied) {
        fail(emptied.error());
        return;
    }
    if (!*emptied)
        return;  // vazgeçildi
    changed();
    status_.clear();
    rescan();
}

}  // namespace noteapp::gui
