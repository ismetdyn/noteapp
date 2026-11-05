#include "app/commands.hpp"
#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <filesystem>
#include <iterator>
#include <string>

namespace noteapp::gui {

namespace {

// pane.narrow / pane.wide adımı (ölçek 1 birimi): sürüklemeye gerek kalmadan birkaç basışta belirgin değişim.
constexpr float kPaneStep = 40.0F;

}  // namespace

// Komutun durumdan istediği karşılanmıyorsa nedeni (i18n anahtarı). TUI'nin needs_block'unun GUI karşılığı:
// seçim, bağlantı ve yank bu arayüzün modelindedir.
const char* GuiState::command_block(app::CommandNeeds needs) {
    switch (needs) {
    case app::CommandNeeds::Nothing:
        return nullptr;
    case app::CommandNeeds::Note:
        return shown_ && !new_note_ ? nullptr : "gui.cmd_why_note";
    case app::CommandNeeds::Row:
        return (trash_view_ ? trash_row_ >= 0 : !action_items().empty()) ? nullptr : "gui.cmd_why_row";
    case app::CommandNeeds::Links:
        return link_lines_.empty() ? "gui.cmd_why_links" : nullptr;
    case app::CommandNeeds::FolderView:
        return browsing() && layout_.browser_view == BrowserView::Tree ? nullptr : "gui.cmd_why_folder_view";
    case app::CommandNeeds::Yank:
        if (yank_.empty())
            return "gui.paste_nothing";
        return browsing() ? nullptr : "gui.paste_needs_folder";
    case app::CommandNeeds::Sync:
        return config_.sync.enabled ? nullptr : "gui.cmd_why_sync";
    }
    return nullptr;
}

std::vector<PaletteRow> GuiState::palette_rows(std::string_view query) {
    std::vector<PaletteRow> shown;
    std::vector<app::PaletteCandidate> candidates;
    for (const app::CommandSpec& spec : app::kCommandRegistry) {
        if (std::ranges::find(kGuiCommands, spec.id, &GuiCommand::id) == std::end(kGuiCommands))
            continue;  // bu arayüzde henüz yok (izin listesi)
        const char* reason = command_block(spec.needs);
        shown.push_back({spec.id, spec.label_key, reason});
        candidates.push_back({spec.id, spec.label_key, reason != nullptr});
    }
    std::vector<PaletteRow> rows;
    rows.reserve(shown.size());
    for (const std::size_t index : app::rank_palette(query, candidates, recent_commands_,
                                                     config_.limits.search.fuzzy_candidate_chars.cap()))
        rows.push_back(shown[index]);
    return rows;
}

void GuiState::open_palette() {
    if (const auto id = deciders.palette())
        run_palette_command(*id);
}

// Çalışamayan komut çalıştırılmaz, son kullanılanlara da girmez: nedeni durum çubuğunda. Ayarlar'dan çıkış
// sorusunda Vazgeç de komutu koşturmaz, kaydı da yazılmaz.
void GuiState::run_palette_command(std::string_view id) {
    const app::CommandSpec* spec = app::find_command(id);
    const auto command = std::ranges::find(kGuiCommands, id, &GuiCommand::id);
    if (spec == nullptr || command == std::end(kGuiCommands))
        return;
    if (const char* reason = command_block(spec->needs)) {
        status_ = core::tr(reason);
        return;
    }
    if (!leave_settings_for(*this, *command))
        return;
    app::remember_command(recent_commands_, spec->id, config_.limits.app.recent_commands);
    save_gui_state();
    command->run(*this);
}

void GuiState::show_help() {
    deciders.help();
}

// TUI `e`: seçili not editörde; önizlemedeyse düzenlemeye dönülür.
void GuiState::edit_selected() {
    if (trash_view_ || selected_folder_ || !shown_ || new_note_)
        return;
    if (showing_preview())
        toggle_preview();  // odak editöre
    else
        request_focus(FocusRequest::Editor);
}

void GuiState::select_edge(bool last) {
    if (trash_view_) {
        if (!trash_items_.empty())
            select_trash_row(last ? static_cast<int>(trash_items_.size()) - 1 : 0);
        return;
    }
    if (browsing()) {
        const int count = static_cast<int>(browser_rows().size());
        if (count > 0)
            activate_row(last ? count - 1 : 0);
        return;
    }
    if (const int count = model_.row_count(); count > 0)
        select_row(last ? count - 1 : 0);
}

// Bir satır üst üste biner: sayfa geçişinde nerede kalındığı görünsün.
void GuiState::page(int direction) {
    navigate(direction * std::max(page_rows_ - 1, 1));
}

void GuiState::scroll_preview_half(int direction) {
    preview_scroll_ += static_cast<float>(direction) * preview_rows_ / 2.0F;
}

// Kayıtlı değer ölçek 1 birimindedir; çizim katmanı pencereye göre ayrıca kırpar. En dar, sürüklemedeki
// sınırdır; en geniş pencere kadar: görünmeyen genişlik birikip geri dönüşü uzatmasın.
void GuiState::resize_pane(int direction) {
    const bool map = layout_.browser_view == BrowserView::Map && !trash_view_;
    float& width = map ? layout_.map_pane_width : layout_.left_pane_width;
    const float widest = std::max(GuiLayout::kMinLeftPaneWidth, static_cast<float>(layout_.width));
    width =
        std::clamp(width + (static_cast<float>(direction) * kPaneStep), GuiLayout::kMinLeftPaneWidth, widest);
}

// TUI gibi config.toml'a yazılır; ayarlar formunun kayıt yolundan (doğrulama, diskteki diğer ayarlar
// korunur). Formda kaydedilmemiş değişiklik varsa onları da yazmasın diye dokunulmaz.
void GuiState::toggle_icons() {
    if (settings_.has_changes()) {
        status_ = core::tr("gui.icons_unsaved");
        return;
    }
    settings_.set_field("icons", icon_style() == core::IconStyle::Off ? "nerd" : "off");
    settings_.save();
    if (!settings_.global_error().empty()) {
        status_ = settings_.global_error();
        settings_.revert();
    }
}

void GuiState::link_note() {
    if (!shown_ || new_note_)
        return;
    const core::NotePath from = shown_->path;
    const auto typed = deciders.link_target(core::tr("gui.link_title"));
    if (!typed || typed->empty())
        return;
    const auto target = store_.resolve_link(*typed);
    if (!target) {
        fail(core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", *typed}}});
        return;
    }
    if (const auto added = store_.add_relation(from, *target); !added) {
        fail(added.error());
        return;
    }
    changed();
    map_.edges_changed();
    reload_links();
    status_ = core::tr("gui.linked", {{"path", target->string()}});
}

void GuiState::unlink_note(const core::NotePath& other) {
    if (!shown_ || new_note_)
        return;
    const core::NotePath from = shown_->path;
    if (!deciders.confirm_delete(
            core::tr("gui.confirm_unlink", {{"from", from.string()}, {"to", other.string()}})))
        return;
    if (const auto removed = store_.remove_relation(from, other); !removed) {
        fail(removed.error());
        return;
    }
    changed();
    map_.edges_changed();
    reload_links();
    status_ = core::tr("gui.unlinked", {{"path", other.string()}});
}

void GuiState::focus_links() {
    if (link_lines_.empty())
        return;
    links_open_ = true;
    request_focus(FocusRequest::Links);
}

void GuiState::open_external() {
    if (external_running_) {
        status_ = core::tr("gui.external_running");
        return;
    }
    // Çöpte ve klasör satırında arkadaki canlı not hedef değildir.
    const auto path = trash_view_ || selected_folder_ ? std::nullopt : current_path();
    if (!path || !flush_for(*path))  // editör notun son hâlini görsün
        return;
    external_running_ = true;
    status_ = core::tr("gui.external_open", {{"path", path->string()}});
    // İşçi UI deposunu paylaşmaz: kendi bağlantısı, config'in kopyası. İstisna jthread'den kaçarsa
    // std::terminate olurdu: hataya çevrilir.
    external_ = std::jthread([this, config = config_, note = *path] {
        std::expected<bool, core::Error> result;
        try {
            core::NoteStore store = core::NoteStore::for_config(config);
            result = store.edit_in_temp_file(
                note, [&](const std::filesystem::path& file) { return core::run_editor(config, file); });
        } catch (const std::exception& error) {
            core::log_warn(std::string("event=external_editor_failed detail=") + error.what());
            result = std::unexpected(core::Error{
                core::Error::Code::Io, "error.io", {{"path", note.string()}, {"detail", error.what()}}});
        }
        post([this, note, result] { external_done(note, result); });
    });
}

void GuiState::external_done(const core::NotePath& path, const std::expected<bool, core::Error>& result) {
    external_running_ = false;
    rescan();  // gösterilen not temizse yeniden okunur (app::disk_change)
    if (!result) {
        fail(result.error());
        return;
    }
    if (*result)
        changed();
    status_ = core::tr(*result ? "gui.external_saved" : "gui.external_unchanged", {{"path", path.string()}});
}

void GuiState::copy_selected() {
    std::string text;
    if (trash_view_)  // arkadaki canlı not hedef değildir
        return;
    if (selected_folder_) {
        text = *selected_folder_;
    } else if (const auto path = current_path()) {
        // Özgün baytlar (editörün LF gösterimi değil). Pano ikili veriyi taşıyamaz: nedeni söylenir.
        auto content = store_.read(*path);
        if (!content) {
            fail(content.error());
            return;
        }
        if (core::looks_binary(content->text) || content->text.find('\0') != std::string::npos) {
            status_ = core::tr("gui.copy_refused_binary");
            return;
        }
        if (!core::is_valid_utf8(content->text)) {
            status_ = core::tr("gui.copy_refused_utf8");
            return;
        }
        text = std::move(content->text);
    } else {
        return;
    }
    copy_to_clipboard(text);
}

// Pano yazılamazsa (SDL_SetClipboardText false) sessiz kalınmaz: neden durum çubuğunda, hata renginde.
void GuiState::copy_to_clipboard(const std::string& text) {
    if (deciders.copy_text(text))
        status_ = core::tr("gui.copied", {{"size", core::human_size(text.size())}});
    else
        error_status_ = status_ = core::tr("gui.copy_failed");
}

}  // namespace noteapp::gui
