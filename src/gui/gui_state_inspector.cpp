// Özellik denetçisi (F4; SPEC-gui "Özellik denetçisi"): model app::Inspector (TUI ile ortak), yazma kuralı
// core/note_props (sorgu çubuğunun koruması). Etiket notun metnindedir: bekleyen düzenleme önce kaydedilir.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"

namespace noteapp::gui {

void GuiState::set_inspector_open(bool open) {
    inspector_open_ = open;
    reload_inspector();
}

// Konu gösterilen nottur; ilişki seçildiyse not değişene dek ilişki kalır. Panel kapalıyken okunmaz.
void GuiState::reload_inspector() {
    const std::optional<core::NotePath> shown =
        shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    const auto& target = inspector_.target();
    const bool relation = target && target->kind == app::InspectTarget::Kind::Relation;
    if (!relation || shown != inspector_shown_) {
        if (shown)
            inspector_.inspect(app::InspectTarget::of_note(*shown));
        else
            inspector_.clear();
    }
    inspector_shown_ = shown;
    if (inspector_open_)
        inspector_.reload(store_);
}

void GuiState::inspect_relation(const core::NotePath& first, const core::NotePath& second,
                                const core::RelationId& relation, const std::string& name, bool directed) {
    inspector_shown_ = shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    inspector_.inspect(app::InspectTarget::of_relation(relation, first, second, name, directed));
    inspector_open_ = true;
    inspector_.reload(store_);
}

void GuiState::inspect_shown_note() {
    inspector_.clear();
    reload_inspector();
}

void GuiState::finish_inspector_write(const std::expected<std::string, core::Error>& done) {
    if (!done) {
        fail(done.error());
        return;
    }
    changed();                             // senkron yeni yazmayı gönderir
    status_ = core::terminal_safe(*done);  // anahtar ve değer güvenilmezdir
    notify(app::NoticeKind::Success, status_);
}

// Etiket gövdeyi yazar: harita kenarı, liste etiketleri ve temiz editör yeni metni alır.
bool GuiState::finish_label_write(const std::expected<std::string, core::Error>& done) {
    finish_inspector_write(done);
    if (done) {
        map_.edges_changed();
        rescan();
    }
    reload_links();
    return done.has_value();
}

// app::SavePending: etiketin notu gösteriliyorsa bekleyen düzenlemesi önce kaydedilir.
bool GuiState::flush_if_shown(const core::NotePath& note) {
    return !shown_ || new_note_ || shown_->path != note || flush_for(note);
}

bool GuiState::set_property(const std::string& key, const std::string& input,
                            std::optional<core::PropertyType> type) {
    const auto& target = inspector_.target();
    if (!target)
        return false;
    if (key.starts_with('#'))
        return finish_label_write(inspector_.add_label(
            store_, key, [this](const core::NotePath& note) { return flush_if_shown(note); }));
    const auto done = inspector_.set(store_, key, input, type);
    finish_inspector_write(done);
    return done.has_value();
}

void GuiState::remove_inspector_row(app::InspectorRow row) {
    const auto& target = inspector_.target();
    // Anahtar ve değer güvenilmezdir (senkron): soru temizlenmiş metinle.
    if (!target || row.read_only ||
        !deciders.confirm_delete(core::terminal_safe(inspector_.confirm_remove_text(row)), {}))
        return;
    if (row.kind == app::InspectorRow::Kind::Label) {
        finish_label_write(inspector_.remove_label(
            store_, row.key, [this](const core::NotePath& note) { return flush_if_shown(note); }));
        return;
    }
    finish_inspector_write(inspector_.remove(store_, row.key));
}

}  // namespace noteapp::gui
