#include "tui/tui_state.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr int kBigScrollLines = 5;  // REVIEW `J` `K`
constexpr int kPageLines = static_cast<int>(TuiState::kPreviewPageLines);
constexpr std::string_view kSpaceLabel = "Space";
// GUI'nin satır menüleri (gui/view_browser.cpp): not ve klasör. "Yeni sekmede aç" sekmelerle (T3) gelir;
// GUI'nin klasör silmesi çöpe atar (karar 1), TUI'de `trash`.
constexpr std::string_view kNoteMenu[] = {"move", "trash", "delete"};
constexpr std::string_view kFolderMenu[] = {"folder.new", "rename", "trash"};

template <Intent kIntent> Intent emit(TuiState&) {
    return kIntent;
}

// Yan etkisi modelde biten eylem: çağıran hiçbir şey yürütmez.
template <auto kMethod, auto... kArgs> Intent act(TuiState& state) {
    (state.*kMethod)(kArgs...);
    return Intent::None;
}

// Sonuç gerekiyorsa aynı metodun dönüşü.
template <auto kMethod, auto... kArgs> Intent ask(TuiState& state) {
    return (state.*kMethod)(kArgs...);
}

}  // namespace

// Satır sırası: boş süzgeçte palet sırası. Tablo TuiState üyesidir, çünkü eylemler modelin özel işleridir;
// eylemler dallanmaz (dal `needs` ve adlı metotlardadır), yeni komut tek satırdır.
std::span<const Command> TuiState::commands() {
    using enum app::CommandNeeds;
    static constexpr Command kCommands[] = {
        // Oluşturma ve düzenleme
        {"new", "n", kInList, emit<Intent::NewNote>},
        {"quick.add", "a", kInList, emit<Intent::NewRow>},
        {"folder.new", "", kInList, emit<Intent::NewFolderRow>},
        {"edit", "e", kInList, ask<&TuiState::enter_selected>, Row},
        {"edit", "e", kInReview, emit<Intent::EnterEditor>, Note},
        {"external", "E", kInBrowse, emit<Intent::OpenSelected>, Note},
        {"save", "^S", kInEditor, emit<Intent::EditorKey>, Nothing, Key::CtrlS},
        // Seçim üzerinde
        {"move", "m", kInList, emit<Intent::MoveTo>, Row},
        {"rename", "F2", kInBrowse, emit<Intent::RenameSelected>, Row, Key::F2},
        {"move.dialog", "r", kInList, ask<&TuiState::move_dialog_intent>, Row},
        {"trash", "d", kInList, emit<Intent::TrashSelected>, Row},
        {"restore", "u", kInList, emit<Intent::RestoreLast>},
        {"restore", "u", kInTrash, emit<Intent::RestoreSelected>, Note},
        {"delete", "D", kInList, emit<Intent::DeleteSelected>, Row},
        {"delete", "D", kInTrash, emit<Intent::DeleteSelected>, Note},
        {"trash.empty", "E", kInTrash, emit<Intent::EmptyTrash>, Note},
        {"copy", "c", kInList, emit<Intent::CopyNote>, Row},
        {"copy", "c", kInReview, emit<Intent::CopyNote>, Note},
        {"link", "L", kInBrowse, emit<Intent::LinkNote>, Note},
        {"export.pdf", "P", kInReview, emit<Intent::ExportPdf>, Note},
        // Çoklu seçim ve yank
        {"mark", "x", kInListing, act<&TuiState::toggle_mark>},
        {"mark.clear", "X", kInListing, act<&TuiState::clear_marks>},
        {"mark.all", "Ctrl+A", kInListing, act<&TuiState::mark_all>, Nothing, Key::CtrlA},
        {"yank", "y", kInList, act<&TuiState::begin_yank>},
        {"paste", "p", kInList, emit<Intent::Paste>, Yank},
        // Sıra, sütun, ağaç
        {"sort", "o", kInList, act<&TuiState::begin_sort_menu>},
        {"sort.name", "o n", kInList, [](TuiState& s) { return s.handle_sort_menu("n"); }},
        {"sort.updated", "o u", kInList, [](TuiState& s) { return s.handle_sort_menu("u"); }},
        {"sort.created", "o c", kInList, [](TuiState& s) { return s.handle_sort_menu("c"); }},
        {"sort.size", "o s", kInList, [](TuiState& s) { return s.handle_sort_menu("s"); }},
        {"sort.reverse", "o r", kInList, [](TuiState& s) { return s.handle_sort_menu("r"); }},
        {"column", "v", kInList, ask<&TuiState::cycle_column>},
        {"column.size_date", "", kInList, ask<&TuiState::set_column, ListColumn::SizeAndDate>},
        {"column.size", "", kInList, ask<&TuiState::set_column, ListColumn::Size>},
        {"column.updated", "", kInList, ask<&TuiState::set_column, ListColumn::Updated>},
        {"column.created", "", kInList, ask<&TuiState::set_column, ListColumn::Created>},
        {"column.sync", "", kInList, ask<&TuiState::set_column, ListColumn::Sync>},
        {"column.origin", "", kInList, ask<&TuiState::set_column, ListColumn::Origin>},
        {"column.none", "", kInList, ask<&TuiState::set_column, ListColumn::None>},
        {"toggle", kSpaceLabel, kInList, ask<&TuiState::toggle_row>},
        {"toggle", kSpaceLabel, kInTrash, act<&TuiState::toggle_selected_details>},
        {"tree.toggle_all", "", kInList, ask<&TuiState::toggle_tree_all>, FolderView},
        {"details", "i", kInListing, act<&TuiState::toggle_selected_details>},
        {"details.all", "I", kInListing, act<&TuiState::toggle_all_expanded>},
        {"view.tree", "", kInList, act<&TuiState::set_details_view, false>},
        {"view.details", "", kInList, act<&TuiState::set_details_view, true>},
        // Arama ve gitme
        {"search", "/", kInList, act<&TuiState::begin_search, Mode::Name>},
        {"search.global", "f", kInList, act<&TuiState::begin_search, Mode::Global>},
        {"search.tag", "", kInList, act<&TuiState::begin_tag_search>},
        {"search.view", "", kInList, act<&TuiState::begin_view_search>},
        {"search.mode", "Tab", kInList, act<&TuiState::toggle_content_mode>, Nothing, Key::Tab},
        {"find", "/", kInReview, act<&TuiState::begin_preview_find>},
        {"find.next", "n", kInReview, act<&TuiState::find_step, true>},
        {"find.previous", "N", kInReview, act<&TuiState::find_step, false>},
        {"go.root", "gh", kInListing, act<&TuiState::go_root>},
        {"go.trash", "gt", kInListing, emit<Intent::ToggleTrash>, Nothing, Key::Char, "T"},
        {"trash.back", "q", kInTrash, emit<Intent::ToggleTrash>},
        {"open", "l", kInList, ask<&TuiState::right_or_open>},
        {"back", "h", kInList, act<&TuiState::go_left>},
        {"back", "h", kInReview, act<&TuiState::leave_preview>},
        {"links.focus", "b", kInReview, act<&TuiState::focus_links>, Links},
        {"links.toggle", "", kInReview, act<&TuiState::toggle_links>},
        {"view.toggle", "R", kInReview, act<&TuiState::toggle_review_view>},
        // Gezinme
        {"up", "k", kInListing, act<&TuiState::move_selection, -1>},
        {"up", "k", kInReview, act<&TuiState::scroll_preview, -1>},
        {"down", "j", kInListing, act<&TuiState::move_selection, 1>},
        {"down", "j", kInReview, act<&TuiState::scroll_preview, 1>},
        {"top", "gg", kInListing, act<&TuiState::select_index, std::size_t{0}>},
        {"top", "g", kInReview, act<&TuiState::scroll_preview_top>},
        {"bottom", "G", kInListing, act<&TuiState::select_last>},
        {"bottom", "G", kInReview, act<&TuiState::scroll_preview_bottom>},
        {"page.up", "PgUp", kInList, act<&TuiState::page_list, -1>, Nothing, Key::PageUp},
        {"page.up", "PgUp", kInReview | kInTrash, act<&TuiState::scroll_preview, -kPageLines>, Nothing,
         Key::PageUp},
        {"page.down", "PgDn", kInList, act<&TuiState::page_list, 1>, Nothing, Key::PageDown},
        {"page.down", "PgDn", kInReview | kInTrash, act<&TuiState::scroll_preview, kPageLines>, Nothing,
         Key::PageDown},
        {"half.up", "^U", kInReview, act<&TuiState::scroll_half_page, -1>, Nothing, Key::CtrlU},
        {"half.down", "^D", kInReview, act<&TuiState::scroll_half_page, 1>, Nothing, Key::CtrlD},
        {"big.up", "K", kInReview, act<&TuiState::scroll_preview, -kBigScrollLines>},
        {"big.down", "J", kInReview, act<&TuiState::scroll_preview, kBigScrollLines>},
        // Panel
        {"pane.narrow", "<", kEverywhere, act<&TuiState::resize_list, -1>},
        {"pane.wide", ">", kEverywhere, act<&TuiState::resize_list, 1>},
        {"pane.zoom", "z", kEverywhere, act<&TuiState::toggle_zoom>},
        // Uygulama. F1 ve F9 her odakta app_intent'tedir; `?` ve `,` yalnız şu görünümlerde.
        {"sync.now", "s", kInList, emit<Intent::SyncNow>, Sync},
        {"icons.toggle", "", kEverywhere, emit<Intent::ToggleIcons>},
        {"settings", "F9", kInBrowse, emit<Intent::ShowSettings>, Nothing, Key::F9, ","},
        {"settings", "F9", kInTrash, emit<Intent::ShowSettings>, Nothing, Key::F9},
        {"help", "F1", kInListing, emit<Intent::ShowHelp>, Nothing, Key::F1, "?"},
        {"help", "F1", kInReview, emit<Intent::ShowHelp>, Nothing, Key::F1},
        {"quit", "q", kInBrowse, emit<Intent::Quit>},
    };
    return kCommands;
}

std::optional<Intent> TuiState::handle_command_key(Key key, std::string_view text) {
    const std::optional<unsigned> context = command_context();
    if (!context)
        return std::nullopt;
    if (key == Key::Char && text == kPaletteKey) {
        palette_.emplace();
        return Intent::None;
    }
    const Command* command = bound_command(*context, key, text);
    return command ? std::optional(run_bound(*command)) : std::nullopt;
}

void TuiState::open_row_menu() {
    const Row* row = selected_entry();
    if (!row || trash_view_)
        return;
    if (!is_marked(*row))
        clear_marks();
    focus_ = Focus::List;
    palette_.emplace();
    palette_->only = row->is_folder ? std::span<const std::string_view>(kFolderMenu)
                                    : std::span<const std::string_view>(kNoteMenu);
}

std::optional<unsigned> TuiState::command_context() const {
    if (trash_view_)
        return kInTrash;
    if (focus_ == Focus::List)
        return kInList;
    if (focus_ == Focus::Preview)
        return kInReview;
    return std::nullopt;
}

const Command* TuiState::bound_command(unsigned context, Key key, std::string_view text) const {
    const std::string_view letter = text == " " ? kSpaceLabel : text;
    for (const Command& command : commands()) {
        if ((command.contexts & context) == 0)
            continue;
        const bool bound = key == Key::Char
                               ? !letter.empty() && (command.keys == letter || command.alias == letter)
                               : command.key == key;
        if (bound)
            return &command;
    }
    return nullptr;
}

// Note, Row, Links, FolderView tuşta sessiz etkisizliktir; Yank ve Sync'i eylem kendisi söyler.
Intent TuiState::run_bound(const Command& command) {
    const bool soft = command.needs == CommandNeeds::Yank || command.needs == CommandNeeds::Sync;
    return !soft && command_block(command) ? Intent::None : command.run(*this);
}

const char* TuiState::command_block(const Command& command) const {
    if ((command.contexts & command_context().value_or(0)) != 0)
        return needs_block(command.needs);
    if ((command.contexts & kInList) != 0)
        return "tui.cmd_why_list";
    if ((command.contexts & kInReview) != 0)
        return "tui.cmd_why_review";
    return (command.contexts & kInTrash) != 0 ? "tui.cmd_why_trash" : "tui.cmd_why_editor";
}

const char* TuiState::needs_block(CommandNeeds needs) const {
    const auto unless = [](bool satisfied, const char* reason) { return satisfied ? nullptr : reason; };
    // Çöpte seçim tek başına yetmez: görünen satır yoksa işlenecek bir şey de yoktur.
    const bool has_row = selected_entry() || (!trash_view_ && marked_count() > 0);
    switch (needs) {
    case CommandNeeds::Nothing:
        return nullptr;
    case CommandNeeds::Note:
        return unless(selected_row(), "tui.cmd_why_note");
    case CommandNeeds::Row:
        return unless(has_row, "tui.cmd_why_row");
    case CommandNeeds::Links:
        return unless(!links_.empty() && !links_hidden_, "tui.cmd_why_links");
    case CommandNeeds::FolderView:
        return unless(in_folder_view(), "tui.cmd_why_folder_view");
    case CommandNeeds::Yank:
        return yank_ ? unless(in_folder_view(), "tui.paste_needs_folder") : "tui.paste_nothing";
    case CommandNeeds::Sync:
        return unless(sync_enabled_, "tui.sync_disabled_hint");
    }
    return nullptr;
}

// Aynı adın birden çok satırı varsa palet o görünümde çalışanı, yoksa ilkini gösterir.
bool TuiState::shown_in_palette(const Command& command, unsigned context) const {
    const std::span<const Command> table = commands();
    const auto same_id = [&](const Command& other) { return other.id == command.id; };
    const auto bound = [&](const Command& other) {
        return same_id(other) && (other.contexts & context) != 0;
    };
    const auto found = std::ranges::find_if(table, bound);
    return &(found != table.end() ? *found : *std::ranges::find_if(table, same_id)) == &command;
}

std::vector<PaletteRow> TuiState::palette_rows() const {
    if (!palette_)
        return {};
    const unsigned context = command_context().value_or(0);
    std::vector<PaletteRow> shown;
    std::vector<app::PaletteCandidate> candidates;
    const std::span<const std::string_view> only = palette_->only;
    for (const Command& command : commands()) {
        if (!shown_in_palette(command, context) ||
            (!only.empty() && std::ranges::find(only, command.id) == only.end()))
            continue;
        const char* reason = command_block(command);
        shown.push_back({&command, reason});
        candidates.push_back({command.id, command.label_key, reason != nullptr});
    }
    std::vector<PaletteRow> rows;
    for (const std::size_t index : app::rank_palette(
             palette_->line.text(), candidates, prefs_.recent_commands, listing_.fuzzy_candidate_chars()))
        rows.push_back(shown[index]);
    return rows;
}

const Command* TuiState::palette_selected() const {
    const std::vector<PaletteRow> rows = palette_rows();
    if (rows.empty())
        return nullptr;
    const auto found = std::ranges::find(rows, palette_ ? palette_->selected : nullptr, &PaletteRow::command);
    return found != rows.end() ? found->command : rows.front().command;
}

void TuiState::step_palette(int delta) {
    const std::vector<PaletteRow> rows = palette_rows();
    if (rows.empty())
        return;
    const auto current = std::ranges::find(rows, palette_selected(), &PaletteRow::command);
    const auto index = static_cast<int>(current - rows.begin());
    const int next = std::clamp(index + delta, 0, static_cast<int>(rows.size()) - 1);
    palette_->selected =
        rows[static_cast<std::size_t>(next)]
            .command;  // NOLINT(bugprone-unchecked-optional-access) -- handle_palette denetledi
}

void TuiState::remember_command(std::string_view id) {
    app::remember_command(prefs_.recent_commands, id, recent_commands_limit_);
    prefs_changed_ = true;
}

// Karar yeniden hesaplanır: çizimden sonra gelen tarama seçimi değiştirdiyse soluk komut çalışmaz.
Intent TuiState::run_palette_selection() {
    const Command* command = palette_selected();
    if (!command || command_block(*command))
        return Intent::None;
    remember_command(command->id);
    palette_.reset();
    return command->run(*this);
}

Intent TuiState::handle_palette(Key key, std::string_view text) {
    switch (key) {
    case Key::Escape:
        palette_.reset();
        return Intent::None;
    case Key::Up:
        step_palette(-1);
        return Intent::None;
    case Key::Down:
        step_palette(1);
        return Intent::None;
    case Key::Enter:
        return run_palette_selection();
    case Key::Tab:
        return Intent::None;  // aday listesi yok: süzgeç zaten listedir
    default:
        break;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- handle yalnız palet açıkken çağırır
    if (palette_->line.handle(key, text) == InputLine::Result::Edited)
        palette_->selected = nullptr;  // NOLINT(bugprone-unchecked-optional-access) -- üstteki gibi
    return Intent::None;
}

}  // namespace noteapp::tui
