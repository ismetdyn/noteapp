#include "tui/editor_tabs.hpp"

#include "core/folder_tree.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace noteapp::tui {

EditorTabs::EditorTabs(Factory make, core::Limit limit)
    : make_(std::move(make)), limit_(std::move(limit)), idle_(make_(0)) {}

std::optional<core::NotePath> EditorTabs::path_of(const Tab& tab) const {
    if (tab.pending)
        return tab.pending;
    return tab.session->is_open() ? tab.session->path() : std::nullopt;
}

EditorTabs::Tab* EditorTabs::find_path(const core::NotePath& path) {
    return tabs_.find_if([&](const Tab& tab) { return path_of(tab) == path; });
}

EditorSession& EditorTabs::active() {
    Tab* tab = tabs_.active();
    return tab ? *tab->session : *idle_;
}

const EditorSession& EditorTabs::active() const {
    const Tab* tab = tabs_.active();
    return tab ? *tab->session : *idle_;
}

std::vector<TabLabel> EditorTabs::labels() const {
    std::vector<TabLabel> out;
    out.reserve(tabs_.size());
    for (const Tab& tab : tabs_) {
        const auto path = path_of(tab);
        std::string title = path ? core::last_component(path->string()) : tab.session->display_name();
        out.push_back({std::move(title), tab.id == tabs_.active_id(), tab.session->dirty(), keeps(tab)});
    }
    return out;
}

bool EditorTabs::keeps(const Tab& tab) const {
    const EditorSession& session = *tab.session;
    // Geri yüklenmiş (pending) sekme nottur; kapalı oturum boş taslak sayılır.
    const bool note = tab.pending || (session.is_open() && !session.is_draft());
    const tui::EditorBuffer* buffer = note ? nullptr : session.buffer();
    return app::keeps_tab(tab.pinned, tab.kept, !note, buffer ? std::string_view(buffer->text()) : "");
}

bool EditorTabs::leave_active() {
    Tab* tab = tabs_.active();
    return tab == nullptr || tab->session->park();
}

EditorTabs::Tab* EditorTabs::push() {
    Tab* tab = tabs_.push(limit_);
    if (tab != nullptr)
        tab->session = make_(tab->id);
    return tab;
}

EditorTabs::Tab* EditorTabs::target(bool new_tab) {
    Tab* current = tabs_.active();
    if (!new_tab && current != nullptr && !keeps(*current))
        return current;
    return push();
}

void EditorTabs::activate(Tab& tab) {
    tabs_.activate(tab.id);
    closing_ = 0;
}

EditorTabs::Result EditorTabs::open_note(const core::NotePath& path, std::size_t start_line, bool new_tab) {
    if (Tab* open = find_path(path)) {
        if (open->id != tabs_.active_id() && !leave_active())
            return Result::Blocked;
        if (new_tab)
            open->kept = true;  // zaten açık: sekme korunur, sonraki açılış yeni sekmeye
        activate(*open);
        load_active();
        return Result::Done;
    }
    const bool needs_new = new_tab || (tabs_.active() != nullptr && keeps(*tabs_.active()));
    if (needs_new && !app::can_add_tab(tabs_.size(), limit_))
        return Result::Limit;
    if (!leave_active())
        return Result::Blocked;
    Tab* tab = target(new_tab);
    if (tab == nullptr)
        return Result::Limit;
    const std::uint64_t id = tab->id;
    tab->pending.reset();
    if (new_tab)
        tab->kept = true;
    activate(*tab);
    tab->session->open(path, start_line);  // hatada (on_error) yeni sekme kapalı kalır, sweep toplar
    if (Tab* opened = tabs_.find(id); opened != nullptr && !opened->session->is_open())
        return Result::Blocked;
    return Result::Done;
}

EditorTabs::Result EditorTabs::begin_note(const std::function<void(EditorSession&)>& start, bool new_tab) {
    const bool needs_new = new_tab || (tabs_.active() != nullptr && keeps(*tabs_.active()));
    if (needs_new && !app::can_add_tab(tabs_.size(), limit_))
        return Result::Limit;
    if (!leave_active())
        return Result::Blocked;
    Tab* tab = target(new_tab);
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
    const std::uint64_t target_id = tabs_.cycled(delta);
    if (target_id != tabs_.active_id() && !leave_active())
        return false;
    // Kayıt (on_saved) sekmeleri değiştirmez; yine de kimlikle aranır.
    Tab* tab = tabs_.find(target_id);
    if (tab == nullptr)
        return false;
    activate(*tab);
    load_active();
    return tabs_.find(target_id) != nullptr && tab->session->is_open();
}

void EditorTabs::close_active() {
    Tab* tab = tabs_.active();
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
    Tab* tab = tabs_.active();
    if (tab == nullptr || !tab->pending)
        return;
    const core::NotePath path = *std::exchange(tab->pending, std::nullopt);
    tab->session->open(path, 0);  // not gittiyse on_error; sekme kapalı kalır, sweep toplar
}

EditorTabs::Swept EditorTabs::sweep() {
    Swept swept;
    // Vazgeçilen kapanış: sonraki Ctrl+Q komşuyu açmasın.
    if (const Tab* closing = tabs_.find(closing_); closing != nullptr && closing->session->is_open() &&
                                                   closing->session->mode() == EditorSession::Mode::Editing)
        closing_ = 0;
    const std::uint64_t was_active = tabs_.active_id();
    swept.active_closed =
        tabs_.close_if([](const Tab& tab) { return !tab.pending && !tab.session->is_open(); });
    if (!swept.active_closed) {
        if (closing_ != 0 && tabs_.find(closing_) == nullptr)
            closing_ = 0;
        return swept;
    }
    swept.reopen = closing_ == was_active && !tabs_.empty();
    closing_ = 0;
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
    if (Tab* tab = tabs_.active(); tab != nullptr && tab->session->dirty())
        tab->kept = true;
}

app::SavedTabs EditorTabs::saved() const {
    return tabs_.saved([this](const Tab& tab) { return path_of(tab); });
}

void EditorTabs::restore(const app::SavedTabs& saved,
                         const std::function<bool(const core::NotePath&)>& exists) {
    tabs_.restore(saved, exists, limit_, [this](Tab& tab, const app::RestoredTab& entry) {
        tab.session = make_(tab.id);
        tab.pending = entry.path;
    });
}

std::vector<std::string> EditorTabs::flush_on_exit() {
    std::vector<std::string> notes;
    for (Tab& tab : tabs_)
        if (std::string note = tab.session->flush_on_exit(); !note.empty())
            notes.push_back(std::move(note));
    return notes;
}

}  // namespace noteapp::tui
