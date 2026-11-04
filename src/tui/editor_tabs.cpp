#include "tui/editor_tabs.hpp"

#include "core/folder_tree.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace noteapp::tui {

EditorTabs::EditorTabs(Factory make, core::Limit limit)
    : make_(std::move(make)), limit_(std::move(limit)), idle_(make_(0)) {}

EditorTabs::Tab* EditorTabs::find(std::uint64_t id) {
    const auto found = std::ranges::find(tabs_, id, &Tab::id);
    return found == tabs_.end() ? nullptr : &*found;
}

const EditorTabs::Tab* EditorTabs::find(std::uint64_t id) const {
    const auto found = std::ranges::find(tabs_, id, &Tab::id);
    return found == tabs_.end() ? nullptr : &*found;
}

std::optional<core::NotePath> EditorTabs::path_of(const Tab& tab) const {
    if (tab.pending)
        return tab.pending;
    return tab.session->is_open() ? tab.session->path() : std::nullopt;
}

EditorTabs::Tab* EditorTabs::find_path(const core::NotePath& path) {
    const auto found = std::ranges::find_if(tabs_, [&](const Tab& tab) { return path_of(tab) == path; });
    return found == tabs_.end() ? nullptr : &*found;
}

EditorSession& EditorTabs::active() {
    Tab* tab = find(active_);
    return tab ? *tab->session : *idle_;
}

const EditorSession& EditorTabs::active() const {
    const Tab* tab = find(active_);
    return tab ? *tab->session : *idle_;
}

std::vector<TabLabel> EditorTabs::labels() const {
    std::vector<TabLabel> out;
    out.reserve(tabs_.size());
    for (const Tab& tab : tabs_) {
        const auto path = path_of(tab);
        std::string title = path ? core::last_component(path->string()) : tab.session->display_name();
        out.push_back({std::move(title), tab.id == active_, tab.session->dirty(), keeps(tab)});
    }
    return out;
}

// Boş sekme (ne not ne yazılmış taslak) korunacak bir şey taşımaz.
bool EditorTabs::keeps(const Tab& tab) const {
    const EditorSession& session = *tab.session;
    const bool written_draft = session.is_open() && session.is_draft() && session.buffer() != nullptr &&
                               !session.buffer()->text().empty();
    return tab.pinned || tab.kept || written_draft;
}

bool EditorTabs::leave_active() {
    Tab* tab = find(active_);
    return tab == nullptr || tab->session->park();
}

EditorTabs::Tab& EditorTabs::push() {
    Tab tab;
    tab.id = next_id_++;
    tab.session = make_(tab.id);
    tabs_.push_back(std::move(tab));
    return tabs_.back();
}

EditorTabs::Tab* EditorTabs::target(bool new_tab, bool& limited) {
    limited = false;
    Tab* current = find(active_);
    if (!new_tab && current != nullptr && !keeps(*current))
        return current;
    if (!app::can_add_tab(tabs_.size(), limit_)) {
        limited = true;
        return nullptr;
    }
    return &push();
}

void EditorTabs::activate(Tab& tab) {
    active_ = tab.id;
    closing_ = 0;
}

EditorTabs::Result EditorTabs::open_note(const core::NotePath& path, std::size_t start_line, bool new_tab) {
    if (Tab* open = find_path(path)) {
        if (open->id != active_ && !leave_active())
            return Result::Blocked;
        if (new_tab)
            open->kept = true;  // zaten açık: sekme korunur, sonraki açılış yeni sekmeye
        activate(*open);
        load_active();
        return Result::Done;
    }
    const bool needs_new = new_tab || (find(active_) != nullptr && keeps(*find(active_)));
    if (needs_new && !app::can_add_tab(tabs_.size(), limit_))
        return Result::Limit;
    if (!leave_active())
        return Result::Blocked;
    bool limited = false;
    Tab* tab = target(new_tab, limited);
    if (tab == nullptr)
        return Result::Limit;
    const std::uint64_t id = tab->id;
    tab->pending.reset();
    if (new_tab)
        tab->kept = true;
    activate(*tab);
    tab->session->open(path, start_line);  // hatada (on_error) yeni sekme kapalı kalır, sweep toplar
    if (Tab* opened = find(id); opened != nullptr && !opened->session->is_open())
        return Result::Blocked;
    return Result::Done;
}

EditorTabs::Result EditorTabs::begin_note(const std::function<void(EditorSession&)>& start, bool new_tab) {
    const bool needs_new = new_tab || (find(active_) != nullptr && keeps(*find(active_)));
    if (needs_new && !app::can_add_tab(tabs_.size(), limit_))
        return Result::Limit;
    if (!leave_active())
        return Result::Blocked;
    bool limited = false;
    Tab* tab = target(new_tab, limited);
    if (tab == nullptr)
        return Result::Limit;
    tab->pending.reset();
    tab->kept = false;
    activate(*tab);
    start(*tab->session);
    return Result::Done;
}

bool EditorTabs::cycle(int delta) {
    if (tabs_.empty())
        return false;
    const auto current = std::ranges::find(tabs_, active_, &Tab::id);
    const auto index = static_cast<std::size_t>(current == tabs_.end() ? 0 : current - tabs_.begin());
    const std::uint64_t target_id = tabs_[app::cycled_tab(index, delta, tabs_.size())].id;
    if (target_id != active_ && !leave_active())
        return false;
    // Kayıt (on_saved) sekmeleri değiştirmez; yine de kimlikle aranır.
    Tab* tab = find(target_id);
    if (tab == nullptr)
        return false;
    activate(*tab);
    load_active();
    return find(target_id) != nullptr && tab->session->is_open();
}

void EditorTabs::close_active() {
    Tab* tab = find(active_);
    if (tab == nullptr)
        return;
    if (!tab->session->is_open()) {  // geri yüklenmiş, hiç okunmamış: sorulacak bir şey yok
        tab->pending.reset();
        closing_ = tab->id;
        return;
    }
    closing_ = tab->id;
    tab->session->handle(Key::CtrlQ);  // mevcut akış: temizse kapanır, kirliyse Kaydet/At/Vazgeç
}

void EditorTabs::load_active() {
    Tab* tab = find(active_);
    if (tab == nullptr || !tab->pending)
        return;
    const core::NotePath path = *std::exchange(tab->pending, std::nullopt);
    tab->session->open(path, 0);  // not gittiyse on_error; sekme kapalı kalır, sweep toplar
}

EditorTabs::Swept EditorTabs::sweep() {
    Swept swept;
    // Vazgeçilen kapanış: sonraki Ctrl+Q komşuyu açmasın.
    if (const Tab* closing = find(closing_); closing != nullptr && closing->session->is_open() &&
                                             closing->session->mode() == EditorSession::Mode::Editing)
        closing_ = 0;
    const auto active_at = std::ranges::find(tabs_, active_, &Tab::id);
    const auto active_index = static_cast<std::size_t>(active_at - tabs_.begin());
    std::size_t removed_before_active = 0;
    std::size_t index = 0;
    std::erase_if(tabs_, [&](const Tab& tab) {
        const bool gone = !tab.pending && !tab.session->is_open();
        if (gone && index < active_index)
            ++removed_before_active;
        if (gone && tab.id == active_)
            swept.active_closed = true;
        ++index;
        return gone;
    });
    if (!swept.active_closed) {
        if (closing_ != 0 && find(closing_) == nullptr)
            closing_ = 0;
        return swept;
    }
    swept.reopen = closing_ == active_ && !tabs_.empty();
    closing_ = 0;
    if (tabs_.empty()) {
        active_ = 0;
        return swept;
    }
    active_ = tabs_[app::neighbour_after_close(active_index - removed_before_active, tabs_.size())].id;
    return swept;
}

std::vector<EditorTabs::Closed> EditorTabs::on_listing(std::span<const core::NoteInfo> notes) {
    std::vector<Closed> closed;
    for (Tab& tab : tabs_) {
        if (tab.pending || !tab.session->is_open())
            continue;  // geri yüklenmiş sekme ilk etkinleşmede okunur
        const auto path = tab.session->path();
        tab.session->on_listing(notes);
        if (path && !tab.session->is_open())
            closed.push_back({tab.id, *path});
    }
    return closed;
}

void EditorTabs::follow_moved(const std::string& from, const std::string& to) {
    for (Tab& tab : tabs_) {
        if (tab.pending) {
            if (auto moved = app::moved_path(*tab.pending, from, to))
                tab.pending = std::move(*moved);
            continue;
        }
        if (const auto& path = tab.session->path(); path && tab.session->is_open())
            if (const auto moved = app::moved_path(*path, from, to))
                tab.session->follow_move(*moved);
    }
}

void EditorTabs::mark_edited() {
    if (Tab* tab = find(active_); tab != nullptr && tab->session->dirty())
        tab->kept = true;
}

app::SavedTabs EditorTabs::saved() const {
    app::SavedTabs saved;
    for (const Tab& tab : tabs_) {
        const auto path = path_of(tab);
        if (!path)
            continue;
        if (tab.id == active_)
            saved.active = saved.tabs.size();
        saved.tabs.push_back({path->string(), tab.pinned, tab.kept});
    }
    return saved;
}

void EditorTabs::restore(const app::SavedTabs& saved,
                         const std::function<bool(const core::NotePath&)>& exists) {
    const app::RestoredTabs restored = app::restorable_tabs(saved, exists, limit_);
    for (const app::RestoredTab& entry : restored.tabs) {
        Tab& tab = push();
        tab.pinned = entry.pinned;
        tab.kept = entry.kept;
        tab.pending = entry.path;
    }
    if (!tabs_.empty())
        active_ = tabs_[restored.active].id;
}

std::vector<std::string> EditorTabs::flush_on_exit() {
    std::vector<std::string> notes;
    for (Tab& tab : tabs_)
        if (std::string note = tab.session->flush_on_exit(); !note.empty())
            notes.push_back(std::move(note));
    return notes;
}

}  // namespace noteapp::tui
