#include "app/document.hpp"
#include "app/map_relations.hpp"
#include "edit/display_text.hpp"
#include "gui/gui_state.hpp"

#include <unordered_set>

namespace noteapp::gui {

// Model ortak (app::MapModel, TUI de kullanır); burada yalnız GUI'nin girdileri bağlanır: kenarları depo,
// merkezi gösterilen not, süzgeci liste modeli verir.
void GuiState::set_map_options(const MapOptions& options) {
    map_.set_options(options);
}

const NoteMap& GuiState::note_map() {
    ensure_map();
    return map_.map();
}

GraphLayout& GuiState::map_layout() {
    ensure_map();
    return map_.layout();
}

// Kenarlar yalnız taramada değişiklik görülünce (listeleme ya da başka bağlantının yazması) tek okumayla
// gelir; görünür harita seçenek ya da çevre merkezi değişince bellekten kurulur.
void GuiState::ensure_map() {
    map_.update([this] { return store_.edges(); }, model_.listing().metas(),
                shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt);
}

// Süzgeç listeyle aynıdır (model_): eşleşme yalnız süzgeç ya da harita değişince kurulur, kare başına değil.
const std::vector<char>& GuiState::map_matches() {
    ensure_map();
    return map_.refresh_matches(!query_.empty(), model_.resets(), [this] {
        std::unordered_set<std::string> listed;
        listed.reserve(static_cast<std::size_t>(model_.row_count()));
        for (int row = 0; row < model_.row_count(); ++row)
            listed.insert(model_.note_at(row).path.string());
        return listed;
    });
}

void GuiState::open_map_note(const core::NotePath& path) {
    if (flush_shown())
        open_link(path);
}

void GuiState::connect_map_nodes(const MapNode& from, const MapNode& to, MapEdgeKind kind) {
    edit_map(from, to, kind, true);
}

void GuiState::disconnect_map_nodes(const MapNode& from, const MapNode& to, MapEdgeKind kind,
                                    const core::RelationId& relation) {
    edit_map(from, to, kind, false, relation);
}

std::optional<core::RelationSpec> GuiState::ask_relation(const std::string& from, const std::string& to,
                                                         std::string initial, bool rename) {
    const auto names = store_.relation_names();
    if (!names) {
        fail(names.error());
        return std::nullopt;
    }
    return deciders.relation(RelationRequest{from, to, *names, std::move(initial), rename,
                                             config_.limits.graph.relation_name_bytes.cap()});
}

void GuiState::finish_relation(const std::expected<std::string, core::Error>& done) {
    if (!done) {
        fail(done.error());
        return;
    }
    changed();
    map_.edges_changed();  // ilişki listeyi değiştirmez
    reload_links();
    // Ad güvenilmezdir (senkron): bidi ve sıfır genişlikli karakter durum satırına ve bildirime çıplak gitmez.
    status_ = tui::terminal_safe(*done);
    notify(app::NoticeKind::Success, status_);
}

void GuiState::relate_map_notes(const MapNode& from, const MapNode& to) {
    const auto spec = ask_relation(from.key, to.key, {}, false);
    if (!spec)
        return;
    finish_relation(app::relate_map_notes(store_, from, to, *spec));
}

void GuiState::rename_relation(const core::NotePath& first, const core::NotePath& second,
                               const core::RelationId& relation, const std::string& current) {
    const auto spec = ask_relation(first.string(), second.string(), current, true);
    if (!spec)
        return;
    finish_relation(app::rename_map_relation(store_, first, second, relation, spec->name));
}

void GuiState::direct_relation(const core::NotePath& first, const core::NotePath& second,
                               const core::RelationId& relation, const std::optional<core::NotePath>& source) {
    finish_relation(app::direct_map_relation(store_, first, second, relation, source));
}

// Yazılacak not editördeyse önce kaydedilir: yoksa işlem tampondaki düzenlemeyi diskteki eski metne ekler ve
// sonraki kayıt çakışırdı. Başka not ya da taslak kaydedilmez (işlem onlara dokunmaz).
void GuiState::edit_map(const MapNode& from, const MapNode& to, MapEdgeKind kind, bool connect,
                        const core::RelationId& relation) {
    const auto note = app::map_edit_note(from, to, kind);
    if (note && shown_ && !new_note_ && shown_->path == *note && !flush_for(*note))
        return;
    const auto done = connect ? app::connect_map_nodes(store_, from, to, kind)
                              : app::disconnect_map_nodes(store_, from, to, kind, relation);
    if (!done) {
        fail(done.error());
        return;
    }
    changed();
    map_.edges_changed();  // ilişki listeyi değiştirmez
    rescan();              // gövde yazıldıysa: liste etiketleri ve temiz editör yeni metni alır
    status_ = *done;
    notify(app::NoticeKind::Success, status_);
}

}  // namespace noteapp::gui
