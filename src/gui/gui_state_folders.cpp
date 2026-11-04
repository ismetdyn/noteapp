#include "app/document.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::gui {

bool GuiState::shown_in(const std::string& folder) const {
    return shown_ && core::in_folder(shown_->path.string(), folder);
}

void GuiState::new_folder(const std::string& parent) {
    const auto name = deciders.folder_name(core::tr("gui.new_folder_title"), {});
    if (!name)
        return;
    const std::string folder = core::join_folder(parent, *name);
    // create_folder yinelenebilir (idempotent) olduğundan var olan klasör çakışması burada yakalanır.
    if (std::ranges::find(folders_, folder) != folders_.end()) {
        fail(core::Error{core::Error::Code::AlreadyExists, "error.already_exists", {{"path", folder}}});
        return;
    }
    if (const auto created = store_.create_folder(folder); !created) {
        fail(created.error());
        return;
    }
    changed();
    status_.clear();
    set_expanded(parent, true);  // yeni klasör ağaçta görünsün
    rescan();
}

void GuiState::rename_folder(const std::string& folder) {
    if (folder.empty())
        return;
    const auto slash = folder.rfind('/');
    const std::string parent = slash == std::string::npos ? "" : folder.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? folder : folder.substr(slash + 1);
    const auto name = deciders.folder_name(core::tr("gui.rename_folder_title"), leaf);
    if (!name || *name == leaf)
        return;
    const std::string target = core::join_folder(parent, *name);

    if (shown_in(folder) && !flush())  // kirli tampon yeni yola değil, taşınmadan önce kaydedilir
        return;
    if (const auto renamed = store_.rename_folder(folder, target); !renamed) {
        fail(renamed.error());
        rescan();
        return;
    }
    changed();
    status_.clear();
    follow_moved(folder, target);
    rescan();
}

// Taşınan klasör ya da not: gösterilen not, sekmeler, konum, seçim ve açık klasörler yeni yolu izler. Tampon
// önceden kaydedilmiş olmalı.
void GuiState::follow_moved(const std::string& from, const std::string& to) {
    const auto renamed_path = [&](const std::string& path) {
        return path == from || core::in_folder(path, from) ? to + path.substr(from.size()) : path;
    };
    // flush sonrası bakılır: dışarıdan silinen not kaydet diyaloğuyla başka klasöre gitmiş olabilir.
    if (shown_ && renamed_path(shown_->path.string()) != shown_->path.string()) {
        if (const auto moved = core::NotePath::parse(renamed_path(shown_note().path.string()))) {
            shown_note().path = *moved;
            if (const auto fresh = store_.read(*moved))
                doc_.moved(*moved, fresh->revision);
        }
    }
    for (NoteTab& tab : tabs_) {  // arka plandaki sekmeler yeni yolu izler; gösterilen not yukarıda
        if (!tab.saved.shown)
            continue;
        // Kural TUI sekmeleriyle ortak (app::moved_path).
        if (const auto moved = app::moved_path(tab.saved.shown->path, from, to)) {
            tab.saved.shown->path = *moved;
            // Belge de yeni yolu izler: yoksa kayıt eski yola gider (Gone ya da orada sonradan açılan nota
            // çakışma). Taşıma gövde revizyonunu değiştirmez. Henüz yüklenmemiş sekmenin belgesi yoktur.
            if (tab.saved.doc.path())
                tab.saved.doc.moved(*moved, tab.saved.doc.revision());
        }
    }
    current_folder_ = renamed_path(current_folder_);
    if (selected_folder_)
        selected_folder_ = renamed_path(*selected_folder_);
    std::set<std::string> expanded;
    for (const std::string& path : model_.listing().open_folders())
        expanded.insert(renamed_path(path));
    model_.listing().set_open_folders(std::move(expanded));
    rows_stale_ = true;
}

// Klasör çöpe gider (plan-parity karar 1): notlar tek tek atılır, boşalan klasör kalkar; Geri al hepsini (boş
// alt klasörler dahil) getirir. Kalıcı silme yalnız çöpten. Notlu klasör sorulur, notsuz klasör sorulmaz.
void GuiState::delete_folder(const std::string& folder) {
    if (folder.empty())
        return;
    const std::size_t depth = trash_.undo_depth();
    // Onaydan sonra, notlar atılmadan önce: gösterilen notun son hâli çöpe gitsin.
    const auto save_shown = [&] { return !shown_in(folder) || flush(); };
    const auto trashed = trash_.trash_folder(store_, model_.listing(), folder, trash_prompts(save_shown));
    if (!trashed)
        return;  // vazgeçildi ya da kayıt olmadı
    const bool undoable = trash_.undo_depth() != depth;
    if (undoable)
        changed();
    rescan();  // klasördeki notların sekmeleri (gösterilen dahil) kapanır: close_missing_tabs
    // İleti taramadan sonra: Geri al teklifi ona bağlıdır (trash_note gibi).
    if (trashed->failure)
        fail(*trashed->failure);  // klasör ve atılamayan not yerinde
    else
        status_ =
            core::tr("gui.folder_trashed", {{"folder", folder}, {"count", std::to_string(trashed->notes)}});
    if (undoable)
        undo_status_ = status_;
}

std::string GuiState::target_folder() const {
    return app::new_note_folder({.selected_folder = selected_folder_ ? &*selected_folder_ : nullptr,
                                 .scope_folder = current_folder_,
                                 .note = shown_ && !new_note_ ? &shown_->path : nullptr,
                                 .default_folder = config_.default_folder});
}

void GuiState::rename_selected() {
    if (!can_rename())
        return;
    if (selected_folder_) {
        const std::string folder = *selected_folder_;  // kopya: yeniden adlandırma seçimi de günceller
        rename_folder(folder);
    } else {
        save_as();
    }
}

// Klasöre yıkıcı ya da yeniden adlandıran eylem yalnız vurgulu satırına uygulanır: Ayrıntılar'da görüntülenen
// klasörün ve kapalı bir klasörün altındaki seçimin satırı yoktur, kullanıcı neyi sileceğini görmez.
bool GuiState::folder_action_allowed() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıranlar selected_folder_'ı denetledi
    return !selected_folder_->empty() && browsing() && browser_row() >= 0;
}

bool GuiState::can_delete() {
    if (trash_view_)
        return trash_row_ >= 0;
    return selected_folder_ ? folder_action_allowed() : current_path().has_value();
}

bool GuiState::can_rename() {
    if (trash_view_)
        return false;
    return selected_folder_ ? folder_action_allowed() : shown_.has_value() && !new_note_;
}

}  // namespace noteapp::gui
