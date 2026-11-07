#include "app/document.hpp"
#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "core/note_views.hpp"
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
bool GuiState::edit_map(const MapNode& from, const MapNode& to, MapEdgeKind kind, bool connect,
                        const core::RelationId& relation) {
    const auto note = app::map_edit_note(from, to, kind);
    if (note && shown_ && !new_note_ && shown_->path == *note && !flush_for(*note))
        return false;
    const auto done = connect ? app::connect_map_nodes(store_, from, to, kind)
                              : app::disconnect_map_nodes(store_, from, to, kind, relation);
    if (!done) {
        fail(done.error());
        return false;
    }
    changed();
    map_.edges_changed();  // ilişki listeyi değiştirmez
    rescan();              // gövde yazıldıysa: liste etiketleri ve temiz editör yeni metni alır
    status_ = *done;
    notify(app::NoticeKind::Success, status_);
    return true;
}

// ---- Keşif (F2) ----

void GuiState::toggle_map_neighbours(const MapNode& node) {
    if (node.kind != MapNodeKind::Note)
        return;
    if (map_.options().whole_graph) {
        status_ = core::tr("map.whole_graph_open");
        notify(app::NoticeKind::Info, status_);
        return;
    }
    map_.toggle_neighbours(node.key, app::expansion_for(map_.options()));
}

void GuiState::expand_map_node(const MapNode& node, const app::MapExpansion& spec) {
    if (node.kind == MapNodeKind::Note)
        map_.expand(node.key, spec);
}

void GuiState::collapse_map_node(const MapNode& node) {
    if (node.kind == MapNodeKind::Note)
        map_.collapse(node.key);
}

void GuiState::find_map_path(const MapNode& from, const MapNode& to, bool directed) {
    if (from.kind != MapNodeKind::Note || to.kind != MapNodeKind::Note)
        return;
    ensure_map();  // kenarlar güncel olsun
    const app::MapPathResult result = map_.find_path(model_.listing().metas(), from.key, to.key, directed);
    // Yollar güvenilmezdir (senkron): durum satırına ve bildirime çıplak gitmez.
    status_ = app::map_path_text(result, tui::terminal_safe(from.key), tui::terminal_safe(to.key), directed);
    notify(result.status == app::MapPathResult::Status::Found ? app::NoticeKind::Success : app::NoticeKind::Info,
           status_);
}

void GuiState::clear_map_path() {
    map_.clear_path();
}

void GuiState::toggle_map_pin(std::size_t node) {
    GraphLayout& layout = map_layout();
    if (node >= layout.size())
        return;
    if (layout.pinned(node))
        layout.unpin(node);
    else
        layout.pin(node, layout.positions()[node]);
}

void GuiState::unpin_map() {
    map_layout().unpin_all();
}

void GuiState::begin_map_note(app::LayoutPoint at) {
    begin_quick_add();
    if (quick_add_open_)
        map_new_at_ = at;
}

void GuiState::trash_map_note(const core::NotePath& path) {
    if (current_path() == path) {
        trash_note();
        return;
    }
    const auto trashed = trash_.trash_note(store_, path);
    if (!trashed) {
        fail(trashed.error());
        return;
    }
    changed();
    rescan();
    undo_status_ = core::tr("gui.trashed", {{"path", path.string()}});
    status_ = undo_status_;
    notify(app::NoticeKind::Success, status_);
}

// ---- Sorgu çubuğu (F3) ----

void GuiState::toggle_map_query() {
    map_query_open_ = !map_query_open_;
    if (map_query_open_)
        return;
    map_query_.clear();
    map_.set_highlight(std::nullopt);
}

void GuiState::run_map_query() {
    const std::string text = map_query_text_;
    if (text.find_first_not_of(" \t") == std::string::npos)
        return;
    // Geçmiş arama geçmişinin yardımcısıyla (sır benzeri değer yazılmaz); oturumluktur, dosyaya girmez.
    tui::remember_search(map_query_history_, text, nullptr, search_history_limits());
    map_query_line_.set_text({});
    finish_map_query(map_query_.run(store_, text));
}

void GuiState::run_map_view(const std::string& name) {
    finish_map_query(map_query_.run_view(store_, name));
}

std::optional<std::string> GuiState::map_query_history_step(std::string_view current, bool older) {
    if (map_query_line_.text() != current)
        map_query_line_.set_text(std::string(current));  // yazıldı: yürüyüş yazılandan başlar
    const auto result = map_query_line_.handle(older ? tui::Key::CtrlP : tui::Key::CtrlN, {}, map_query_history_);
    if (result != tui::InputLine::Result::Edited)
        return std::nullopt;
    return map_query_line_.text();
}

std::vector<std::string> GuiState::map_view_names() {
    return app::view_names(store_);
}

void GuiState::save_map_view() {
    if (map_query_.read_query().empty()) {
        fail(core::Error{core::Error::Code::InvalidQuery, "error.view_needs_query", {}});
        return;
    }
    const auto name = deciders.view_name(core::tr("command.view_save"));
    if (!name)
        return;
    if (const auto saved = map_query_.save_view(store_, *name); !saved) {
        fail(saved.error());
        return;
    }
    status_ = tui::terminal_safe(core::tr("app.view_saved", {{"name", *name}}));
    notify(app::NoticeKind::Success, status_);
    changed();
}

void GuiState::remove_saved_view(const std::string& name) {
    // Sorulan görünüm: onay açıkken senkron aynı adı başka sorguyla yeniden kurarsa o silinmez.
    const auto shown = core::find_view(store_, name);
    if (!shown) {
        fail(shown.error());
        return;
    }
    if (!deciders.confirm_delete(core::tr("app.view_remove_question", {{"name", tui::terminal_safe(name)}}),
                                 core::tr("command.view_remove")))
        return;
    if (const auto removed = core::remove_view(store_, name, *shown); !removed) {
        fail(removed.error());
        return;
    }
    status_ = tui::terminal_safe(core::tr("app.view_removed", {{"name", name}}));
    notify(app::NoticeKind::Success, status_);
    changed();
}

// Yazma onaysız uygulanmaz; vazgeçilen yazmanın metni çubukta kalır. Hata çubuğun altındadır, önceki sonuç ve
// vurgusu kalır.
void GuiState::finish_map_query(app::MapQuery::Step step) {
    using Step = app::MapQuery::Step;
    if (step == Step::Confirm) {
        if (!deciders.confirm_query(map_query_.confirm_text())) {
            map_query_.cancel();
            return;
        }
        step = map_query_.apply(store_);
    }
    if (step != Step::Ran)
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Ran sonucu doldurur (MapQuery::finish)
    const app::QueryOutcome& outcome = *map_query_.outcome();
    if (outcome.wrote) {
        changed();
        map_.edges_changed();
        rescan();
        reload_links();
    }
    map_.set_highlight(outcome.highlight);
    map_query_text_.clear();
    ++map_query_generation_;
    status_ = tui::terminal_safe(outcome.summary);
    notify(outcome.wrote ? app::NoticeKind::Success : app::NoticeKind::Info, status_);
}

std::vector<core::RelationName> GuiState::map_relation_names() {
    const auto names = store_.relation_names();
    if (!names) {
        fail(names.error());
        return {};
    }
    return **names;
}

}  // namespace noteapp::gui
