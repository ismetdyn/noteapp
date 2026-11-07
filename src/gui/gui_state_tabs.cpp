#include "app/document.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/gui_state_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace noteapp::gui {

// Etkin sekmenin notu canlı üyelerde, diğerlerininki anlık görüntüde. Taslağın notu yoktur.
std::optional<core::NotePath> GuiState::tab_note(const NoteTab& tab) const {
    if (tab.id == tabs_.active_id() && !parked_)
        return shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    return tab.saved.shown && !tab.saved.new_note ? std::optional(tab.saved.shown->path) : std::nullopt;
}

std::optional<std::uint64_t> GuiState::tab_of(const core::NotePath& path) const {
    for (const NoteTab& tab : tabs_) {
        if (tab_note(tab) == path)
            return tab.id;
    }
    return std::nullopt;
}

std::vector<TabSummary> GuiState::tabs() const {
    std::vector<TabSummary> out;
    out.reserve(tabs_.size());
    for (const NoteTab& tab : tabs_) {
        const bool live = tab.id == tabs_.active_id() && !parked_;
        const bool draft = live ? new_note_ : tab.saved.new_note;
        const std::string& text = live ? editor_.text() : tab.saved.editor.text();
        const auto note = tab_note(tab);
        TabSummary summary;
        summary.id = tab.id;
        summary.pinned = tab.pinned;
        summary.kept = tab.kept;
        summary.path = note ? note->string() : std::string{};
        summary.title = note ? core::last_component(summary.path) : core::tr("gui.untitled");
        summary.unsaved = draft ? draft_needs_answer(text) : (live && dirty_);
        out.push_back(std::move(summary));
    }
    return out;
}

bool GuiState::keeps_active_tab() const {
    const NoteTab* tab = tabs_.active();
    if (tab == nullptr)
        return false;
    // Park edilmiş sekme nottur; ne not ne taslak olan boş editör boş taslak sayılır.
    const bool note = parked_ || (shown_ && !new_note_);
    return app::keeps_tab(tab->pinned, tab->kept, !note, note ? std::string_view{} : editor_.text());
}

void GuiState::claim_active_tab() {
    if (tabs_.empty()) {
        // Boş listede ilk sekme sınırsız push'tur (limits.app.tabs en az 1 ya da 0 = sınırsız): hiç boş
        // kalmaz.
        tabs_.activate(tabs_.push(core::Limit{})->id);
    } else if (parked_) {
        if (NoteTab* tab = tabs_.active())
            tab->saved = {};  // yerine geçen içerik park edilen notu bırakır
    }
    parked_ = false;
    tabs_closed_ = false;
}

void GuiState::clear_live() {
    shown_.reset();
    new_note_ = false;
    doc_.reset_draft();
    review_ = true;
    draft_folder_.clear();
    set_editor({}, false);
    reload_links();
}

// Kayıtlı notun bekleyen düzenlemesi kendi notuna yazılır (flush_for); taslak sekmesinde kaydedilmeden kalır.
bool GuiState::leave_tab() {
    if (parked_ || tabs_.empty())
        return true;
    if (dirty_ && shown_ && !new_note_) {
        const core::NotePath note = shown_->path;  // kopya: kayıt akışı shown_'ı değiştirebilir
        if (!flush_for(note))
            return false;
    }
    stash();
    return true;
}

void GuiState::stash() {
    NoteTab* tab = tabs_.active();
    if (tab == nullptr || parked_)
        return;
    tab->saved = EditorSnapshot{.shown = shown_,
                                .editor = std::exchange(editor_, {}),
                                .generation = buffer_generation_,
                                .draft_folder = draft_folder_,
                                .doc = std::exchange(doc_, {}),
                                .editable = editable_,
                                .dirty = dirty_,
                                .new_note = new_note_,
                                .review = review_};
    tab->loaded = true;
    clear_live();
}

// Sekmenin anlık görüntüsü canlı üyelere geçer. Not dışarıda değiştiyse gösterilen notun kuralı geçerlidir
// (show_note): temizse yeniden okunur. Aynı içerikte kuşak korunur, editör kaydırması yerinde kalır.
void GuiState::restore_tab(std::uint64_t id, bool focus_editor) {
    NoteTab* tab = tabs_.find(id);
    if (tab == nullptr)
        return;
    tabs_.activate(id);
    parked_ = false;
    tabs_closed_ = false;
    selected_folder_.reset();
    EditorSnapshot saved = std::exchange(tab->saved, {});
    const bool loaded = std::exchange(tab->loaded, true);
    const auto note = saved.shown && !saved.new_note ? std::optional(saved.shown->path) : std::nullopt;
    if (loaded) {
        shown_ = std::move(saved.shown);
        editor_ = std::move(saved.editor);  // imleç, seçim ve geri alma geçmişi sekmeyle döner
        hidden_controls_ = core::count_hidden_controls(editor_.text());  // set_editor'dan geçmiyor
        buffer_generation_ = saved.generation;
        draft_folder_ = std::move(saved.draft_folder);
        editable_ = saved.editable;
        dirty_ = saved.dirty;
        new_note_ = saved.new_note;
        draft_size_ = editor_.text().size();  // taslağın tipi editörle döner; sayaç bu boydan sürer
        doc_ = std::move(saved.doc);
        autosave_.cancel();
        selection_key_.clear();
    } else {
        clear_live();
        if (note)
            show_note(*saved.shown);  // gui-state'ten gelen sekme: ilk etkinleşmede okunur
    }
    review_ = saved.review;
    lock_for_review();
    current_row_ = -1;
    if (note) {
        ensure_visible(*note);
        // Süzgeç gevşerken not listede yoksa apply_filter ilk satırı açıp başka sekmeye geçmiş olabilir:
        // düşürülecek olan o sekme değil, bu sekmedir; geçiş olduysa dokunulmaz.
        if (tabs_.active_id() != id)
            return;
        const int row = model_.index_of(*note);
        if (row < 0) {  // taramalar arasında gitmiş
            status_ = core::tr("app.tab_closed_missing", {{"path", note->string()}});
            drop_active_tab();
            return;
        }
        current_row_ = row;
        show_note(model_.note_at(row));
    }
    reload_links();
    if (focus_editor)
        request_focus(FocusRequest::Editor);
}

bool GuiState::switch_tab(std::uint64_t id, bool focus_editor) {
    if (tabs_.find(id) == nullptr)
        return false;
    if (id == tabs_.active_id() && !parked_) {
        if (focus_editor)
            request_focus(FocusRequest::Editor);
        return true;
    }
    if (!leave_tab())
        return false;
    if (tabs_.find(id) == nullptr)  // kaydın taraması sekmeyi kapatmış olabilir
        return false;
    restore_tab(id, focus_editor);
    return true;
}

bool GuiState::activate_tab(std::uint64_t id) {
    return switch_tab(id, true);
}

// Yeni boş sekme en sona eklenir (ImGui de yeni sekmeyi sona koyar; sıralar ayrışmaz) ve etkin olur.
bool GuiState::push_tab() {
    if (!app::can_add_tab(tabs_.size(), config_.limits.app.tabs)) {
        status_ = core::tr("app.tab_limit", {{"max", config_.limits.app.tabs.text}});
        return false;
    }
    if (!leave_tab())
        return false;
    NoteTab* tab = tabs_.push(config_.limits.app.tabs);
    if (tab == nullptr)  // yukarıda denetlendi; leave_tab'in kaydı sekme açmaz
        return false;
    tabs_.activate(tab->id);
    parked_ = false;
    tabs_closed_ = false;
    current_row_ = -1;
    return true;
}

// Etkin sekmeyi atar (içerik kaydedilmiş ya da bilerek bırakılmış olmalı); sağındaki, yoksa solundaki öne
// gelir.
void GuiState::drop_active_tab() {
    if (!tabs_.close(tabs_.active_id()))
        return;
    if (tabs_.empty()) {
        parked_ = false;
        clear_live();
        current_row_ = -1;
        return;
    }
    if (!parked_)  // park edilmişse editör zaten boş: yeni etkin sekme de park edilmiş kalır
        restore_tab(tabs_.active_id(), false);
}

void GuiState::erase_tab(std::uint64_t id) {
    if (id == tabs_.active_id())
        drop_active_tab();
    else
        tabs_.close(id);
}

void GuiState::park_or_clear() {
    if (!parked_ && shown_ && !new_note_ && tabs_.active() != nullptr) {
        stash();
        parked_ = true;
        return;
    }
    clear_editor();
}

// Senkron ya da başka süreç notu silmiş/taşımışsa sekmesi iletiyle kapanır. Gösterilen kirli not mevcut
// akışta kalır: kayıt NotFound'u taslağa çevirir, düzenleme kaybolmaz. Liste okunamadıysa (boş) kapanma
// yalnız depo NotFound dediğinde olur.
void GuiState::close_missing_tabs(const std::unordered_set<std::string>& present) {
    if (tabs_.empty())
        return;
    std::vector<std::pair<std::uint64_t, core::NotePath>> gone;
    for (const NoteTab& tab : tabs_) {
        const bool live = tab.id == tabs_.active_id() && !parked_;
        const auto note = tab_note(tab);
        if (!note || present.contains(note->string()) || (live && dirty_))
            continue;
        if (const auto content = store_.read(*note);
            content || content.error().code != core::Error::Code::NotFound)
            continue;
        gone.emplace_back(tab.id, *note);
    }
    // Gösterilen sekme en son: kapanınca öne gelen komşu, kapanacaklardan biri olmasın.
    std::ranges::stable_partition(gone, [this](const auto& item) { return item.first != tabs_.active_id(); });
    for (const auto& [id, note] : gone) {
        status_ = core::tr("app.tab_closed_missing", {{"path", note.string()}});
        erase_tab(id);
    }
}

bool GuiState::confirm_close_drafts() {
    std::vector<std::uint64_t> drafts;
    for (const NoteTab& tab : tabs_) {
        if (!(tab.id == tabs_.active_id() && !parked_) && tab.saved.new_note &&
            draft_needs_answer(tab.saved.editor.text()))
            drafts.push_back(tab.id);
    }
    for (const std::uint64_t id : drafts) {
        if (!switch_tab(id, true) || !confirm_close())
            return false;
    }
    return true;
}

bool GuiState::restore_tabs(const app::SavedTabs& saved) {
    // Silinmiş, taşınmış, yinelenen ya da elle bozulmuş girdi atlanır; sınır limits.app.tabs (TUI ile ortak).
    // Açılıştaki ilk satır gösterimi bırakılır: henüz düzenlenmedi (açılışta tek, düzenlenmemiş sekme).
    const bool restored = tabs_.restore(
        saved, [this](const core::NotePath& path) { return model_.index_of(path) >= 0; },
        config_.limits.app.tabs,
        [this](NoteTab& tab, const app::RestoredTab& entry) {
            tab.loaded = false;
            tab.saved.shown = model_.note_at(model_.index_of(entry.path));
        });
    if (!restored)
        return false;
    clear_live();
    parked_ = true;
    restore_tab(tabs_.active_id(), false);
    return true;
}

void GuiState::next_tab(int delta) {
    if (!tabs_.empty())
        activate_tab(tabs_.cycled(delta));
}

void GuiState::new_tab() {
    std::string folder = target_folder();
    if (!push_tab())
        return;
    new_note();
    draft_folder_ = std::move(folder);
}

void GuiState::close_tab(std::uint64_t id) {
    const NoteTab* tab = tabs_.find(id);
    if (tab == nullptr)
        return;
    if (id != tabs_.active_id() || parked_) {
        if (!tab->saved.new_note || !draft_needs_answer(tab->saved.editor.text())) {
            erase_tab(id);
            return;
        }
        if (!switch_tab(id, true))  // yazılmış taslak: soru onun metniyle, onu öne getirerek sorulur
            return;
    }
    if (!confirm_close())  // kirli not kaydedilir; taslak için Kaydet/At/Vazgeç
        return;
    drop_active_tab();
    if (tabs_.empty())
        tabs_closed_ = true;
}

void GuiState::close_active_tab() {
    if (tabs_.active_id() != 0)
        close_tab(tabs_.active_id());
}

void GuiState::set_tab_pinned(std::uint64_t id, bool pinned) {
    if (NoteTab* tab = tabs_.find(id))
        tab->pinned = pinned;
}

void GuiState::reorder_tabs(const std::vector<std::uint64_t>& order) {
    tabs_.reorder(order);
}

void GuiState::open_in_new_tab(const core::NotePath& path) {
    close_trash();
    ensure_visible(path);  // süzgeç gevşer: seçim sonradan bu satıra kurulur
    if (model_.index_of(path) < 0) {
        report(core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", path.string()}}});
        return;
    }
    if (!parked_ && shown_ && !new_note_ && shown_->path == path) {
        if (NoteTab* tab = tabs_.active())
            tab->kept = true;  // zaten burada: sekme korunur, sonraki açılış yeni sekmeye
        return;
    }
    if (const auto open = tab_of(path)) {
        switch_tab(*open, false);
        return;
    }
    if (!push_tab())
        return;
    const std::uint64_t id = tabs_.active_id();
    select_row(model_.index_of(path));
    if (NoteTab* tab = tabs_.find(id))
        tab->kept = true;
}

}  // namespace noteapp::gui
