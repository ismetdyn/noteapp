#include "core/i18n.hpp"
#include "search/fuzzy.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr int kBigScrollLines = 5;  // REVIEW `J` `K`
constexpr int kPageLines = static_cast<int>(TuiState::kPreviewPageLines);
constexpr std::string_view kSpaceLabel = "Space";

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
    using enum CommandNeeds;
    static constexpr Command kCommands[] = {
        // Oluşturma ve düzenleme
        {"new", "tui.cmd_new", "n", kInList, emit<Intent::NewNote>},
        {"quick.add", "tui.cmd_quick_add", "a", kInList, emit<Intent::NewRow>},
        {"edit", "tui.cmd_edit", "e", kInList, ask<&TuiState::enter_selected>, Row},
        {"edit", "tui.cmd_edit", "e", kInReview, emit<Intent::EnterEditor>, Note},
        {"external", "tui.cmd_external", "E", kInBrowse, emit<Intent::OpenSelected>, Note},
        // Seçim üzerinde
        {"move", "tui.cmd_move", "m", kInList, emit<Intent::MoveTo>, Row},
        {"rename", "tui.cmd_rename", "F2", kInBrowse, emit<Intent::RenameSelected>, Row, Key::F2},
        {"move.dialog", "tui.cmd_move_dialog", "r", kInList, ask<&TuiState::move_dialog_intent>, Row},
        {"trash", "tui.cmd_trash", "d", kInList, emit<Intent::TrashSelected>, Row},
        {"restore", "tui.cmd_restore", "u", kInList, emit<Intent::RestoreLast>},
        {"restore", "tui.cmd_restore", "u", kInTrash, emit<Intent::RestoreSelected>, Note},
        {"delete", "tui.cmd_delete", "D", kInList, emit<Intent::DeleteSelected>, Row},
        {"delete", "tui.cmd_delete", "D", kInTrash, emit<Intent::DeleteSelected>, Note},
        {"trash.empty", "tui.cmd_trash_empty", "E", kInTrash, emit<Intent::EmptyTrash>, Note},
        {"copy", "tui.cmd_copy", "c", kInList, emit<Intent::CopyNote>, Row},
        {"copy", "tui.cmd_copy", "c", kInReview, emit<Intent::CopyNote>, Note},
        {"link", "tui.cmd_link", "L", kInBrowse, emit<Intent::LinkNote>, Note},
        // Çoklu seçim ve yank
        {"mark", "tui.cmd_mark", "x", kInListing, act<&TuiState::toggle_mark>},
        {"mark.clear", "tui.cmd_mark_clear", "X", kInListing, act<&TuiState::clear_marks>},
        {"mark.all", "tui.cmd_mark_all", "Ctrl+A", kInListing, act<&TuiState::mark_all>, Nothing, Key::CtrlA},
        {"yank", "tui.cmd_yank", "y", kInList, act<&TuiState::begin_yank>},
        {"paste", "tui.cmd_paste", "p", kInList, emit<Intent::Paste>, Yank},
        // Sıra, sütun, ağaç
        {"sort", "tui.cmd_sort", "o", kInList, act<&TuiState::begin_sort_menu>},
        {"sort.name", "tui.cmd_sort_name", "o n", kInList,
         [](TuiState& s) { return s.handle_sort_menu("n"); }},
        {"sort.updated", "tui.cmd_sort_updated", "o u", kInList,
         [](TuiState& s) { return s.handle_sort_menu("u"); }},
        {"sort.created", "tui.cmd_sort_created", "o c", kInList,
         [](TuiState& s) { return s.handle_sort_menu("c"); }},
        {"sort.size", "tui.cmd_sort_size", "o s", kInList,
         [](TuiState& s) { return s.handle_sort_menu("s"); }},
        {"sort.reverse", "tui.cmd_sort_reverse", "o r", kInList,
         [](TuiState& s) { return s.handle_sort_menu("r"); }},
        {"column", "tui.cmd_column", "v", kInList, ask<&TuiState::cycle_column>},
        {"column.size_date", "tui.cmd_column_size_date", "", kInList,
         ask<&TuiState::set_column, ListColumn::SizeAndDate>},
        {"column.size", "tui.cmd_column_size", "", kInList, ask<&TuiState::set_column, ListColumn::Size>},
        {"column.updated", "tui.cmd_column_updated", "", kInList,
         ask<&TuiState::set_column, ListColumn::Updated>},
        {"column.created", "tui.cmd_column_created", "", kInList,
         ask<&TuiState::set_column, ListColumn::Created>},
        {"column.sync", "tui.cmd_column_sync", "", kInList, ask<&TuiState::set_column, ListColumn::Sync>},
        {"column.origin", "tui.cmd_column_origin", "", kInList,
         ask<&TuiState::set_column, ListColumn::Origin>},
        {"column.none", "tui.cmd_column_none", "", kInList, ask<&TuiState::set_column, ListColumn::None>},
        {"toggle", "tui.cmd_toggle", kSpaceLabel, kInList, ask<&TuiState::toggle_row>},
        {"toggle", "tui.cmd_toggle", kSpaceLabel, kInTrash, act<&TuiState::toggle_selected_details>},
        {"tree.toggle_all", "tui.cmd_tree_toggle_all", "", kInList, ask<&TuiState::toggle_tree_all>,
         FolderView},
        {"details", "tui.cmd_details", "i", kInListing, act<&TuiState::toggle_selected_details>},
        {"details.all", "tui.cmd_details_all", "I", kInListing, act<&TuiState::toggle_all_expanded>},
        // Arama ve gitme
        {"search", "tui.cmd_search", "/", kInList, act<&TuiState::begin_search, Mode::Name>},
        {"search.global", "tui.cmd_search_global", "f", kInList, act<&TuiState::begin_search, Mode::Global>},
        {"search.tag", "tui.cmd_search_tag", "", kInList, act<&TuiState::begin_tag_search>},
        {"search.view", "tui.cmd_search_view", "", kInList, act<&TuiState::begin_view_search>},
        {"search.mode", "tui.cmd_search_mode", "Tab", kInList, act<&TuiState::toggle_content_mode>, Nothing,
         Key::Tab},
        {"find", "tui.cmd_find", "/", kInReview, act<&TuiState::begin_preview_find>},
        {"find.next", "tui.cmd_find_next", "n", kInReview, act<&TuiState::find_step, true>},
        {"find.previous", "tui.cmd_find_previous", "N", kInReview, act<&TuiState::find_step, false>},
        {"go.root", "tui.cmd_go_root", "gh", kInListing, act<&TuiState::go_root>},
        {"go.trash", "tui.cmd_go_trash", "gt", kInListing, emit<Intent::ToggleTrash>, Nothing, Key::Char,
         "T"},
        {"trash.back", "tui.cmd_trash_back", "q", kInTrash, emit<Intent::ToggleTrash>},
        {"open", "tui.cmd_open", "l", kInList, ask<&TuiState::right_or_open>},
        {"back", "tui.cmd_back", "h", kInList, act<&TuiState::go_left>},
        {"back", "tui.cmd_back", "h", kInReview, act<&TuiState::leave_preview>},
        {"links.focus", "tui.cmd_links_focus", "b", kInReview, act<&TuiState::focus_links>, Links},
        {"view.toggle", "tui.cmd_view_toggle", "R", kInReview, act<&TuiState::toggle_review_view>},
        // Gezinme
        {"up", "tui.cmd_up", "k", kInListing, act<&TuiState::move_selection, -1>},
        {"up", "tui.cmd_up", "k", kInReview, act<&TuiState::scroll_preview, -1>},
        {"down", "tui.cmd_down", "j", kInListing, act<&TuiState::move_selection, 1>},
        {"down", "tui.cmd_down", "j", kInReview, act<&TuiState::scroll_preview, 1>},
        {"top", "tui.cmd_top", "gg", kInListing, act<&TuiState::select_index, std::size_t{0}>},
        {"top", "tui.cmd_top", "g", kInReview, act<&TuiState::scroll_preview_top>},
        {"bottom", "tui.cmd_bottom", "G", kInListing, act<&TuiState::select_last>},
        {"bottom", "tui.cmd_bottom", "G", kInReview, act<&TuiState::scroll_preview_bottom>},
        {"page.up", "tui.cmd_page_up", "PgUp", kInList, act<&TuiState::page_list, -1>, Nothing, Key::PageUp},
        {"page.up", "tui.cmd_page_up", "PgUp", kInReview | kInTrash,
         act<&TuiState::scroll_preview, -kPageLines>, Nothing, Key::PageUp},
        {"page.down", "tui.cmd_page_down", "PgDn", kInList, act<&TuiState::page_list, 1>, Nothing,
         Key::PageDown},
        {"page.down", "tui.cmd_page_down", "PgDn", kInReview | kInTrash,
         act<&TuiState::scroll_preview, kPageLines>, Nothing, Key::PageDown},
        {"half.up", "tui.cmd_half_up", "^U", kInReview, act<&TuiState::scroll_half_page, -1>, Nothing,
         Key::CtrlU},
        {"half.down", "tui.cmd_half_down", "^D", kInReview, act<&TuiState::scroll_half_page, 1>, Nothing,
         Key::CtrlD},
        {"big.up", "tui.cmd_big_up", "K", kInReview, act<&TuiState::scroll_preview, -kBigScrollLines>},
        {"big.down", "tui.cmd_big_down", "J", kInReview, act<&TuiState::scroll_preview, kBigScrollLines>},
        // Panel
        {"pane.narrow", "tui.cmd_pane_narrow", "<", kEverywhere, act<&TuiState::resize_list, -1>},
        {"pane.wide", "tui.cmd_pane_wide", ">", kEverywhere, act<&TuiState::resize_list, 1>},
        {"pane.zoom", "tui.cmd_pane_zoom", "z", kEverywhere, act<&TuiState::toggle_zoom>},
        // Uygulama. F1 ve F9 her odakta app_intent'tedir; `?` ve `,` yalnız şu görünümlerde.
        {"sync.now", "tui.cmd_sync_now", "s", kInList, emit<Intent::SyncNow>, Sync},
        {"icons.toggle", "tui.cmd_icons_toggle", "", kEverywhere, emit<Intent::ToggleIcons>},
        {"settings", "tui.cmd_settings", "F9", kInBrowse, emit<Intent::ShowSettings>, Nothing, Key::F9, ","},
        {"settings", "tui.cmd_settings", "F9", kInTrash, emit<Intent::ShowSettings>, Nothing, Key::F9},
        {"help", "tui.cmd_help", "F1", kInListing, emit<Intent::ShowHelp>, Nothing, Key::F1, "?"},
        {"help", "tui.cmd_help", "F1", kInReview, emit<Intent::ShowHelp>, Nothing, Key::F1},
        {"quit", "tui.cmd_quit", "q", kInBrowse, emit<Intent::Quit>},
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
    return (command.contexts & kInReview) != 0 ? "tui.cmd_why_review" : "tui.cmd_why_trash";
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
        return unless(!links_.empty(), "tui.cmd_why_links");
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
    struct Ranked {
        PaletteRow row;
        double score;
        std::size_t recency;
    };
    const std::string& query = palette_->line.text();
    const unsigned context = command_context().value_or(0);
    const std::vector<std::string>& recent = prefs_.recent_commands;
    std::vector<Ranked> ranked;
    for (const Command& command : commands()) {
        if (!shown_in_palette(command, context))
            continue;
        const auto by_id = search::fuzzy_match(query, command.id);
        const auto by_label = search::fuzzy_match(query, core::tr(command.label_key));
        if (!by_id && !by_label)
            continue;
        const auto used = std::ranges::find(recent, command.id);
        ranked.push_back(
            {{&command, command_block(command)},
             std::max(by_id ? by_id->score : std::numeric_limits<double>::lowest(),
                      by_label ? by_label->score : std::numeric_limits<double>::lowest()),
             used == recent.end() ? recent.size() : static_cast<std::size_t>(used - recent.begin())});
    }
    // Boş süzgeçte puan 0'dır: sırayı son kullanılan belirler. Çalışmayan satır aşağıda ama görünür kalır.
    std::ranges::stable_sort(ranked, [](const Ranked& a, const Ranked& b) {
        const auto key = [](const Ranked& r) {
            return std::tuple(r.row.reason != nullptr, -r.score, r.recency);
        };
        return key(a) < key(b);
    });
    std::vector<PaletteRow> rows;
    rows.reserve(ranked.size());
    for (const Ranked& item : ranked)
        rows.push_back(item.row);
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
    std::vector<std::string>& recent = prefs_.recent_commands;
    std::erase(recent, id);
    recent.insert(recent.begin(), std::string(id));
    if (recent.size() > kMaxRecentCommands)
        recent.resize(kMaxRecentCommands);
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
