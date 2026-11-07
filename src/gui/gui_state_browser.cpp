#include "app/document.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace noteapp::gui {

void GuiState::set_browser_view(BrowserView view) {
    if (layout_.browser_view == view)
        return;
    layout_.browser_view = view;
    rows_stale_ = true;
    revealed_note_.clear();  // Ayrıntılar'a geçince konum gösterilen notun klasörüne gelsin
}

void GuiState::set_row_numbers(bool on) {
    layout_.row_numbers = on;
    save_gui_state();
}

void GuiState::set_ui_font(const std::string& name) {
    if (std::ranges::find(kUiFonts, name) == std::end(kUiFonts))
        return;
    layout_.ui_font = name;
    save_gui_state();
}

void GuiState::set_editor_font(const std::string& name) {
    if (std::ranges::find(kEditorFonts, name) == std::end(kEditorFonts))
        return;
    layout_.editor_font = name;
    save_gui_state();
}

void GuiState::ensure_browser() {
    const bool details = layout_.browser_view == BrowserView::Details;
    if (details && built_folder_ != current_folder_)
        rows_stale_ = true;
    if (!rows_stale_)
        return;
    // Karar 6: ağaç ve Ayrıntılar da kullanıcının sırasıyla (sort()).
    browser_rows_ = details ? folder_rows(model_.listing(), current_folder_) : tree_rows(model_.listing());
    built_folder_ = current_folder_;
    rows_stale_ = false;
    ++browser_rebuilds_;
    max_row_number_ = 0;
    for (const BrowserRow& row : browser_rows_)
        max_row_number_ = std::max(max_row_number_, row.number);
}

const std::vector<BrowserRow>& GuiState::browser_rows() {
    ensure_browser();
    return browser_rows_;
}

int GuiState::browser_row() {
    BrowserRowKind kind{};
    std::string path;
    if (selected_folder_) {
        kind = BrowserRowKind::Folder;
        path = *selected_folder_;
    } else if (shown_ && !new_note_) {
        kind = BrowserRowKind::Note;
        path = shown_->path.string();
    } else {
        return -1;
    }
    const auto& rows = browser_rows();
    // ponytail: doğrusal arama, satır başına bir karşılaştırma; çok büyük ağaçta seçim indeksi önbelleğe
    // alınır.
    const auto found = std::ranges::find_if(
        rows, [&](const BrowserRow& row) { return row.kind == kind && row.path == path; });
    return found == rows.end() ? -1 : static_cast<int>(found - rows.begin());
}

std::optional<app::FolderSummary> GuiState::folder_summary() {
    if (!selected_folder_)
        return std::nullopt;
    ensure_browser();
    return app::folder_summary(model_.listing(), *selected_folder_, std::chrono::system_clock::now());
}

void GuiState::set_expanded(const std::string& folder, bool expanded) {
    if (folder.empty())  // kök hep açık
        return;
    if (expanded == is_expanded(folder))
        return;
    model_.listing().set_open(folder, expanded);
    rows_stale_ = true;
    save_gui_state();  // TUI gibi her değişimde: çökme ya da kill açık klasörleri kaybettirmesin
}

void GuiState::open_note(const core::NotePath& path) {
    close_trash();
    ensure_visible(path);
    if (const int row = model_.index_of(path); row >= 0)
        select_row(row);
}

void GuiState::select_folder(const std::string& folder) {
    show_folder(folder, layout_.browser_view == BrowserView::Tree);
}

void GuiState::enter_folder(const std::string& folder) {
    show_folder(folder, true);
}

// Kayıt olmazsa ya da vazgeçilirse not açık kalır.
bool GuiState::show_folder(const std::string& folder, bool scope) {
    if (!flush_shown())
        return false;
    close_trash();
    park_or_clear();  // sekme notunu tutar: sekmeye tıklayınca geri gelir
    selected_folder_ = folder;
    current_row_ = -1;
    if (scope) {
        apply_filter([&] {
            current_folder_ = folder;
            model_.set_folder(current_folder_);
        });
    }
    clear_editor();
    return true;
}

void GuiState::activate_row(int index) {
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];  // kopya: seçim satırları yeniler
    if (row.kind == BrowserRowKind::Folder)
        select_folder(row.path);
    else if (const auto path = core::NotePath::parse(row.path))
        open_note(*path);
}

void GuiState::navigate(int delta) {
    if (trash_view_) {
        const int count = static_cast<int>(trash_items_.size());
        if (count > 0)
            select_trash_row(std::clamp(trash_row_ + delta, 0, count - 1));
        return;
    }
    if (!browsing()) {
        move_selection(delta);
        return;
    }
    const int count = static_cast<int>(browser_rows().size());
    if (count == 0)
        return;
    const int from = browser_row();
    const int to = from < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(from + delta, 0, count - 1);
    if (to != from)
        activate_row(to);
}

// Düz listede (arama, çöp) ağaç satırı görünmez: sağ ve sol ok ona dokunmaz, Enter notu editörde açar.
void GuiState::browse_right() {
    if (!browsing())
        return;
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    if (row.kind != BrowserRowKind::Folder)
        return;
    if (layout_.browser_view == BrowserView::Details)
        enter_folder(row.path);
    else if (!row.expanded)
        set_expanded(row.path, true);
    else if (row.has_children)
        activate_row(index + 1);  // açık klasörün ilk çocuğu hemen altındadır
}

void GuiState::browse_left() {
    if (!browsing())
        return;
    if (layout_.browser_view == BrowserView::Details) {
        if (!current_folder_.empty())
            enter_folder(core::parent_folder(current_folder_));
        return;
    }
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    const bool folder = row.kind == BrowserRowKind::Folder;
    if (folder && row.path.empty())
        return;
    if (folder && row.expanded)
        set_expanded(row.path, false);
    else
        select_folder(core::parent_folder(row.path));
}

void GuiState::browse_enter() {
    if (!browsing()) {
        edit_selected();  // TUI listede Enter: düzenle
        return;
    }
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    if (row.kind == BrowserRowKind::Note)
        edit_selected();
    else if (layout_.browser_view == BrowserView::Details)
        enter_folder(row.path);
    else
        toggle_expanded(row.path);
}

void GuiState::reveal_selection() {
    if (!shown_ || new_note_) {
        revealed_note_.clear();
        return;
    }
    if (!browsing())
        return;
    std::string path = shown_->path.string();
    if (path == revealed_note_)
        return;
    const std::string folder = core::folder_of(shown_->path);
    if (layout_.browser_view == BrowserView::Details) {
        if (folder != current_folder_) {
            apply_filter([&] {
                current_folder_ = folder;
                model_.set_folder(current_folder_);
            });
        }
    } else {
        for (std::string walk = folder; !walk.empty(); walk = core::parent_folder(walk))
            set_expanded(walk, true);
    }
    revealed_note_ = std::move(path);
}

// Silinen satırın yerine gelen ya da üstündeki ilk görünür not: modelin sırası (mtime) ağacın sırası
// değildir, seçim kapalı bir klasördeki nota atlamasın. Görünür not yoksa false.
bool GuiState::select_nearest_note(int from) {
    const auto& rows = browser_rows();
    const int count = static_cast<int>(rows.size());
    const auto is_note = [&](int index) {
        return rows[static_cast<std::size_t>(index)].kind == BrowserRowKind::Note;
    };
    for (int index = std::max(0, std::min(from, count - 1)); index < count; ++index) {
        if (is_note(index)) {
            activate_row(index);
            return true;
        }
    }
    for (int index = std::min(from, count) - 1; index >= 0; --index) {
        if (is_note(index)) {
            activate_row(index);
            return true;
        }
    }
    return false;
}

}  // namespace noteapp::gui
