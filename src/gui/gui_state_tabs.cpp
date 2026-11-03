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

namespace {

// UTF-8 devam baytı 10xxxxxx'tir.
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8ContinuationBits = 0x80;

// Ofset metin sonunu geçmez, kod noktasının ortasındaysa başına çekilir: ImGui imleci bayt ofsetidir, ortaya
// yazılan karakter UTF-8'i bozardı.
std::size_t code_point_start(const std::string& text, std::size_t offset) {
    offset = std::min(offset, text.size());
    while (offset > 0 && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & kUtf8ContinuationMask) == kUtf8ContinuationBits)
        --offset;
    return offset;
}

}  // namespace

GuiState::NoteTab* GuiState::find_tab(std::uint64_t id) {
    const auto found = std::ranges::find(tabs_, id, &NoteTab::id);
    return found == tabs_.end() ? nullptr : &*found;
}

const GuiState::NoteTab* GuiState::find_tab(std::uint64_t id) const {
    const auto found = std::ranges::find(tabs_, id, &NoteTab::id);
    return found == tabs_.end() ? nullptr : &*found;
}

// Etkin sekmenin notu canlı üyelerde, diğerlerininki anlık görüntüde. Taslağın notu yoktur.
std::optional<core::NotePath> GuiState::tab_note(const NoteTab& tab) const {
    if (tab.id == active_tab_ && !parked_)
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
        const bool live = tab.id == active_tab_ && !parked_;
        const bool draft = live ? new_note_ : tab.saved.new_note;
        const std::string& text = live ? buffer_ : tab.saved.buffer;
        const auto note = tab_note(tab);
        TabSummary summary;
        summary.id = tab.id;
        summary.pinned = tab.pinned;
        summary.kept = tab.kept;
        summary.path = note ? note->string() : std::string{};
        summary.title = note ? core::last_component(summary.path) : core::tr("gui.untitled");
        summary.unsaved = draft ? !is_blank(text) : (live && dirty_);
        out.push_back(std::move(summary));
    }
    return out;
}

// Boş sekme (ne not ne yazılmış taslak) korunacak bir şey taşımaz.
bool GuiState::keeps_active_tab() const {
    const NoteTab* tab = find_tab(active_tab_);
    if (tab == nullptr)
        return false;
    const bool draft_text = !parked_ && new_note_ && !is_blank(buffer_);
    if (!parked_ && !shown_ && !draft_text)
        return false;
    return tab->pinned || tab->kept || draft_text;
}

void GuiState::claim_active_tab() {
    if (tabs_.empty()) {
        tabs_.emplace_back().id = next_tab_id_++;
        active_tab_ = tabs_.back().id;
    } else if (parked_) {
        if (NoteTab* tab = find_tab(active_tab_))
            tab->saved = {};  // yerine geçen içerik park edilen notu bırakır
    }
    parked_ = false;
    tabs_closed_ = false;
}

void GuiState::clear_live() {
    shown_.reset();
    new_note_ = false;
    doc_.reset_draft();
    preview_ = false;
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
    NoteTab* tab = find_tab(active_tab_);
    if (tab == nullptr || parked_)
        return;
    tab->saved = EditorSnapshot{.shown = shown_,
                                .buffer = std::exchange(buffer_, {}),
                                .generation = buffer_generation_,
                                .draft_folder = draft_folder_,
                                .doc = std::exchange(doc_, {}),
                                .editable = editable_,
                                .dirty = dirty_,
                                .new_note = new_note_,
                                .preview = preview_};
    tab->loaded = true;
    clear_live();
}

// Sekmenin anlık görüntüsü canlı üyelere geçer. Not dışarıda değiştiyse gösterilen notun kuralı geçerlidir
// (show_note): temizse yeniden okunur. Aynı içerikte kuşak korunur, editör kaydırması yerinde kalır.
void GuiState::restore_tab(std::uint64_t id, bool focus_editor) {
    NoteTab* tab = find_tab(id);
    if (tab == nullptr)
        return;
    active_tab_ = id;
    parked_ = false;
    tabs_closed_ = false;
    selected_folder_.reset();
    EditorSnapshot saved = std::exchange(tab->saved, {});
    const bool loaded = std::exchange(tab->loaded, true);
    const std::optional<TextSelection> cursor = tab->cursor;
    const auto note = saved.shown && !saved.new_note ? std::optional(saved.shown->path) : std::nullopt;
    if (loaded) {
        shown_ = std::move(saved.shown);
        buffer_ = std::move(saved.buffer);
        hidden_controls_ = core::count_hidden_controls(buffer_);  // set_editor'dan geçmiyor
        buffer_generation_ = saved.generation;
        draft_folder_ = std::move(saved.draft_folder);
        editable_ = saved.editable;
        dirty_ = saved.dirty;
        new_note_ = saved.new_note;
        doc_ = std::move(saved.doc);
        autosave_.cancel();
        selection_key_.clear();
        editor_selection_.reset();
    } else {
        clear_live();
        if (note)
            show_note(*saved.shown);  // gui-state'ten gelen sekme: ilk etkinleşmede okunur
    }
    preview_ = saved.preview;
    current_row_ = -1;
    if (note) {
        ensure_visible(*note);
        // Süzgeç gevşerken not listede yoksa apply_filter ilk satırı açıp başka sekmeye geçmiş olabilir:
        // düşürülecek olan o sekme değil, bu sekmedir; geçiş olduysa dokunulmaz.
        if (active_tab_ != id)
            return;
        const int row = model_.index_of(*note);
        if (row < 0) {  // taramalar arasında gitmiş
            status_ = core::tr("gui.tab_closed_missing", {{"path", note->string()}});
            drop_active_tab();
            return;
        }
        current_row_ = row;
        show_note(model_.note_at(row));
    }
    reload_links();
    if (focus_editor) {
        // Not dışarıda değişip yeniden okunduysa saklı ofset artık kod noktası sınırında olmayabilir.
        if (cursor)
            editor_selection_ = TextSelection{code_point_start(buffer_, cursor->start),
                                              code_point_start(buffer_, cursor->end)};
        request_focus(FocusRequest::Editor);
    }
}

bool GuiState::switch_tab(std::uint64_t id, bool focus_editor) {
    if (find_tab(id) == nullptr)
        return false;
    if (id == active_tab_ && !parked_) {
        if (focus_editor)
            request_focus(FocusRequest::Editor);
        return true;
    }
    if (!leave_tab())
        return false;
    if (find_tab(id) == nullptr)  // kaydın taraması sekmeyi kapatmış olabilir
        return false;
    restore_tab(id, focus_editor);
    return true;
}

bool GuiState::activate_tab(std::uint64_t id) {
    return switch_tab(id, true);
}

// Yeni boş sekme en sona eklenir (ImGui de yeni sekmeyi sona koyar; sıralar ayrışmaz) ve etkin olur.
bool GuiState::push_tab() {
    if (tabs_.size() >= kMaxTabs) {
        status_ = core::tr("gui.tab_limit", {{"max", std::to_string(kMaxTabs)}});
        return false;
    }
    if (!leave_tab())
        return false;
    tabs_.emplace_back().id = next_tab_id_++;
    active_tab_ = tabs_.back().id;
    parked_ = false;
    tabs_closed_ = false;
    current_row_ = -1;
    return true;
}

// Etkin sekmeyi atar (içerik kaydedilmiş ya da bilerek bırakılmış olmalı); sağındaki, yoksa solundaki öne
// gelir.
void GuiState::drop_active_tab() {
    const auto found = std::ranges::find(tabs_, active_tab_, &NoteTab::id);
    if (found == tabs_.end())
        return;
    const auto index = static_cast<std::size_t>(found - tabs_.begin());
    tabs_.erase(found);
    if (tabs_.empty()) {
        active_tab_ = 0;
        parked_ = false;
        clear_live();
        current_row_ = -1;
        return;
    }
    active_tab_ = tabs_[std::min(index, tabs_.size() - 1)].id;
    if (!parked_)  // park edilmişse editör zaten boş: yeni etkin sekme de park edilmiş kalır
        restore_tab(active_tab_, false);
}

void GuiState::erase_tab(std::uint64_t id) {
    if (id == active_tab_)
        drop_active_tab();
    else
        std::erase_if(tabs_, [id](const NoteTab& tab) { return tab.id == id; });
}

void GuiState::park_or_clear() {
    if (!parked_ && shown_ && !new_note_ && find_tab(active_tab_) != nullptr) {
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
        const bool live = tab.id == active_tab_ && !parked_;
        const auto note = tab_note(tab);
        if (!note || present.contains(note->string()) || (live && dirty_))
            continue;
        if (const auto content = store_.read(*note);
            content || content.error().code != core::Error::Code::NotFound)
            continue;
        gone.emplace_back(tab.id, *note);
    }
    // Gösterilen sekme en son: kapanınca öne gelen komşu, kapanacaklardan biri olmasın.
    std::ranges::stable_partition(gone, [this](const auto& item) { return item.first != active_tab_; });
    for (const auto& [id, note] : gone) {
        status_ = core::tr("gui.tab_closed_missing", {{"path", note.string()}});
        erase_tab(id);
    }
}

bool GuiState::confirm_close_drafts() {
    std::vector<std::uint64_t> drafts;
    for (const NoteTab& tab : tabs_) {
        if (!(tab.id == active_tab_ && !parked_) && tab.saved.new_note && !is_blank(tab.saved.buffer))
            drafts.push_back(tab.id);
    }
    for (const std::uint64_t id : drafts) {
        if (!switch_tab(id, true) || !confirm_close())
            return false;
    }
    return true;
}

bool GuiState::restore_tabs(const std::vector<SavedTab>& saved, std::size_t active_index) {
    std::vector<NoteTab> restored;
    std::uint64_t active = 0;
    for (std::size_t index = 0; index < saved.size() && restored.size() < kMaxTabs; ++index) {
        const auto path = core::NotePath::parse(saved[index].path);
        if (!path)
            continue;
        const int row = model_.index_of(*path);
        const bool duplicate =
            std::ranges::any_of(restored, [&](const NoteTab& tab) { return tab.saved.shown->path == *path; });
        if (row < 0 || duplicate)  // silinmiş, taşınmış ya da elle bozulmuş girdi
            continue;
        NoteTab tab;
        tab.id = next_tab_id_++;
        tab.pinned = saved[index].pinned;
        tab.kept = saved[index].kept;
        tab.loaded = false;
        tab.saved.shown = model_.note_at(row);
        if (index == active_index)
            active = tab.id;
        restored.push_back(std::move(tab));
    }
    if (restored.empty())
        return false;
    // Açılıştaki ilk satır gösterimi bırakılır: henüz düzenlenmedi.
    clear_live();
    tabs_ = std::move(restored);
    active_tab_ = active != 0 ? active : tabs_.front().id;
    parked_ = true;
    restore_tab(active_tab_, false);
    return true;
}

void GuiState::next_tab(int delta) {
    if (tabs_.empty())
        return;
    const auto count = static_cast<int>(tabs_.size());
    const auto index = static_cast<int>(std::ranges::find(tabs_, active_tab_, &NoteTab::id) - tabs_.begin());
    const int target = (((index + delta) % count) + count) % count;
    activate_tab(tabs_[static_cast<std::size_t>(target)].id);
}

void GuiState::new_tab() {
    std::string folder = target_folder();
    if (!push_tab())
        return;
    new_note();
    draft_folder_ = std::move(folder);
}

void GuiState::close_tab(std::uint64_t id) {
    const NoteTab* tab = find_tab(id);
    if (tab == nullptr)
        return;
    if (id != active_tab_ || parked_) {
        if (!tab->saved.new_note || is_blank(tab->saved.buffer)) {
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
    if (active_tab_ != 0)
        close_tab(active_tab_);
}

void GuiState::set_tab_pinned(std::uint64_t id, bool pinned) {
    if (NoteTab* tab = find_tab(id))
        tab->pinned = pinned;
}

void GuiState::reorder_tabs(const std::vector<std::uint64_t>& order) {
    std::vector<NoteTab> sorted;
    sorted.reserve(tabs_.size());
    for (const std::uint64_t id : order) {
        const auto found = std::ranges::find(tabs_, id, &NoteTab::id);
        if (found != tabs_.end() && std::ranges::find(sorted, id, &NoteTab::id) == sorted.end())
            sorted.push_back(std::move(*found));
    }
    for (NoteTab& tab : tabs_) {  // taşınmış öğenin kimliği yerinde kalır: ikinci kez alınmaz
        if (std::ranges::find(sorted, tab.id, &NoteTab::id) == sorted.end())
            sorted.push_back(std::move(tab));
    }
    tabs_ = std::move(sorted);
}

void GuiState::open_in_new_tab(const core::NotePath& path) {
    close_trash();
    ensure_visible(path);  // süzgeç gevşer: seçim sonradan bu satıra kurulur
    if (model_.index_of(path) < 0) {
        report(core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", path.string()}}});
        return;
    }
    if (!parked_ && shown_ && !new_note_ && shown_->path == path) {
        if (NoteTab* tab = find_tab(active_tab_))
            tab->kept = true;  // zaten burada: sekme korunur, sonraki açılış yeni sekmeye
        return;
    }
    if (const auto open = tab_of(path)) {
        switch_tab(*open, false);
        return;
    }
    if (!push_tab())
        return;
    const std::uint64_t id = active_tab_;
    select_row(model_.index_of(path));
    if (NoteTab* tab = find_tab(id))
        tab->kept = true;
}

void GuiState::remember_cursor(TextSelection cursor) {
    if (parked_)
        return;
    if (NoteTab* tab = find_tab(active_tab_))
        tab->cursor = cursor;
}

}  // namespace noteapp::gui
