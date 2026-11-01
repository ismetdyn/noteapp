#include "tui/tui_state.hpp"

#include "search/fuzzy.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace noteapp::tui {

namespace {

// UTF-8 devam baytı (10xxxxxx): Backspace bir kod noktasının tamamını silsin.
bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Kontrol karakterleri sorguya girmez (terminal olaylarından sızabilir).
bool is_printable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20;
}

// İçerik modunda önizleme ilk eşleşmeye kayar; üstte birkaç satır bağlam kalır.
constexpr std::size_t kHitContextLines = 2;

// Listede Ctrl/ok karşılığı olan harfler: aynı yoldan geçip aynı davransınlar.
constexpr std::pair<std::string_view, Key> kListLetterKeys[] = {
    {"k", Key::Up},    {"j", Key::Down},  {"l", Key::Right}, {"n", Key::CtrlO}, {"d", Key::CtrlD},
    {"r", Key::CtrlR}, {"s", Key::CtrlS}, {"?", Key::F1},    {"e", Key::Enter},
};

std::size_t initial_scroll(const Row& row) {
    if (row.hits.empty() || row.hits.front().line == 0)
        return 0;
    const std::size_t line_index = row.hits.front().line - 1;  // Hit::line 1 tabanlı
    return line_index > kHitContextLines ? line_index - kHitContextLines : 0;
}

constexpr int kListColumnCount = 7;
constexpr int kBigScrollLines = 5;  // REVIEW `J` `K`
// Bu aralıktan sık gelen aynı yön tuşu tekrardır; daha uzun duraklama hızlanmayı sıfırlar.
constexpr auto kKeyRepeatGap = std::chrono::milliseconds(120);
constexpr int kRepeatsPerStep = 8;  // adım her bu kadar tekrarda bir büyür
constexpr int kMaxStepPageDivisor = 3;

// Seçili not ister; boş listede etkisiz.
constexpr std::pair<std::string_view, Intent> kListSelectionLetters[] = {
    {"E", Intent::OpenSelected},
    {"c", Intent::CopyNote},
    {"D", Intent::DeleteSelected},
};
constexpr std::pair<std::string_view, Intent> kListPlainLetters[] = {
    {"u", Intent::RestoreLast}, {"T", Intent::ToggleTrash},  {"a", Intent::NewRow},
    {"q", Intent::Quit},        {",", Intent::ShowSettings},
};
constexpr std::pair<std::string_view, Intent> kPreviewSelectionLetters[] = {
    {"e", Intent::EnterEditor},
    {"E", Intent::OpenSelected},
    {"c", Intent::CopyNote},
};
constexpr std::pair<std::string_view, Intent> kTrashLetters[] = {
    {"u", Intent::RestoreSelected},
    {"D", Intent::DeleteSelected},
    {"E", Intent::EmptyTrash},
};

// REVIEW'da satır adımları.
constexpr std::pair<std::string_view, int> kPreviewScrollLetters[] = {
    {"j", 1},
    {"k", -1},
    {"J", kBigScrollLines},
    {"K", -kBigScrollLines},
};

int page_direction(Key key) {
    return key == Key::PageUp ? -1 : 1;
}

// Her odakta çalışan uygulama tuşları: hiçbir panelin içeriğine bağlı değiller.
std::optional<Intent> app_intent(Key key) {
    switch (key) {
    case Key::CtrlC:
        return Intent::Quit;
    case Key::F1:
        return Intent::ShowHelp;
    case Key::F9:
        return Intent::ShowSettings;
    default:
        return std::nullopt;
    }
}

}  // namespace

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.revision == y.revision && x.mtime == y.mtime && x.size == y.size;
    });
}

bool right_pane_focused(Focus focus) {
    return focus == Focus::Preview || focus == Focus::PreviewFind || focus == Focus::Editor;
}

bool passes_to_terminal(Key key, Focus focus) {
    return key == Key::CtrlZ && focus != Focus::Editor;
}

bool quits_from_modal(Key key, Focus focus) {
    return key == Key::CtrlC && focus != Focus::Editor;
}

void TuiState::set_notes(std::vector<core::NoteInfo> notes) {
    notes_ = std::move(notes);
    rebuild_metas();
    refilter(false);
}

void TuiState::rebuild_metas() {
    metas_.clear();
    meta_index_.clear();
    metas_.reserve(notes_.size());
    for (const core::NoteInfo& note : notes_) {
        core::NoteMeta meta{.path = note.path,
                            .size = note.size,
                            .updated_at = note.mtime,
                            .created_at = note.created_at,
                            .origin = note.origin,
                            .local_seq = note.local_seq,
                            .created_local_seq = note.created_local_seq};
        if (const auto found = sync_states_.find(note.path); found != sync_states_.end())
            meta.sync_status = found->second;
        meta_index_.emplace(note.path, metas_.size());
        metas_.push_back(std::move(meta));
    }
}

void TuiState::set_sync_states(std::map<core::NotePath, core::SyncState> states) {
    sync_states_ = std::move(states);
    rebuild_metas();
    refilter(false);
}

const core::NoteMeta* TuiState::meta_of(const core::NotePath& path) const {
    if (trash_view_) {
        const auto found = std::ranges::find(trash_metas_, path, &core::NoteMeta::path);
        return found == trash_metas_.end() ? nullptr : &*found;
    }
    const auto found = meta_index_.find(path);
    return found == meta_index_.end() ? nullptr : &metas_[found->second];
}

void TuiState::enter_trash(std::vector<core::TrashItem> items) {
    trash_view_ = true;
    query_.clear();
    mode_ = Mode::Name;
    query_error_.reset();
    selected_ = 0;
    preview_find_.clear();
    focus_ = Focus::List;  // fareyle önizlemeden de gelinir: tuşlar çöp listesine gitsin
    set_trash(std::move(items));
}

void TuiState::set_trash(std::vector<core::TrashItem> items) {
    trash_metas_.clear();
    for (core::TrashItem& item : items)
        trash_metas_.push_back({.path = std::move(item.path),
                                .size = item.size,
                                .updated_at = item.trashed_at,
                                .created_at = item.created_at,
                                .origin = std::move(item.origin)});
    refilter(false);
}

void TuiState::leave_trash() {
    trash_view_ = false;
    trash_metas_.clear();
    selected_ = 0;
    refilter(true);
}

void TuiState::set_list_prefs(const ListPrefs& prefs) {
    prefs_ = prefs;
    refilter(false);
}

std::size_t TuiState::row_height(std::size_t row) const {
    const Row& shown = rows_[row];
    return 1 + (shown.show_best_line ? 1 : 0) + (is_expanded(shown.path) ? kMetaLines : 0);
}

bool TuiState::line_starts_row(std::size_t visible, std::size_t line) const {
    std::size_t walked = 0;
    for (std::size_t row = list_top(visible); row < rows_.size() && walked <= line; ++row) {
        if (walked == line)
            return true;
        walked += row_height(row);
    }
    return false;
}

void TuiState::toggle_expanded(std::size_t row) {
    if (row >= rows_.size())
        return;
    const core::NotePath& path = rows_[row].path;
    if (!expanded_.erase(path))
        expanded_.insert(path);
}

std::size_t TuiState::row_at(std::size_t visible, std::size_t line) const {
    std::size_t walked = 0;
    for (std::size_t row = list_top(visible); row < rows_.size(); ++row) {
        walked += row_height(row);
        if (line < walked)
            return row;
    }
    return rows_.size();
}

Intent TuiState::handle_trash(Key key, std::string_view text) {
    switch (key) {
    case Key::Up:
    case Key::CtrlP:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
    case Key::CtrlN:
        move_selection(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        scroll_preview(page_direction(key) * static_cast<int>(kPreviewPageLines));
        return Intent::None;
    case Key::AltRight:
    case Key::AltLeft:
        set_selected_expanded(key == Key::AltRight);
        return Intent::None;
    case Key::Escape:
        return Intent::ToggleTrash;
    case Key::Char:
        return handle_trash_letter(text);
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_trash_letter(std::string_view text) {
    for (const auto& [letter, intent] : kTrashLetters)
        if (text == letter)
            return intent_if_selected(intent);
    if (handle_details_letter(text))
        return Intent::None;
    if (text == "q" || text == "T")
        return Intent::ToggleTrash;
    if (text == "?")
        return Intent::ShowHelp;
    if (handle_layout_letter(text))
        return Intent::None;
    if (text == "k")
        move_selection(-1);
    else if (text == "j")
        move_selection(1);
    else if (text == "g")
        select_index(0);
    else if (text == "G")
        select_index(rows_.size());
    return Intent::None;
}

Intent TuiState::intent_if_selected(Intent intent) const {
    return selected_row() ? intent : Intent::None;
}

void TuiState::set_selected_expanded(bool expanded) {
    const Row* row = selected_row();
    if (!row)
        return;
    if (expanded)
        expanded_.insert(row->path);
    else
        expanded_.erase(row->path);
}

void TuiState::toggle_all_expanded() {
    const bool all_open =
        std::ranges::all_of(rows_, [this](const Row& row) { return is_expanded(row.path); });
    for (const Row& row : rows_) {
        if (all_open)
            expanded_.erase(row.path);
        else
            expanded_.insert(row.path);
    }
}

// `Alt+ok` her terminalde gelmez: Space ve `i` seçili satırı, `I` tümünü açar/kapatır.
bool TuiState::handle_details_letter(std::string_view text) {
    if (text == " " || text == "i")
        toggle_expanded(selected_);
    else if (text == "I")
        toggle_all_expanded();
    else
        return false;
    return true;
}

Intent TuiState::handle_sort_menu(std::string_view text) {
    constexpr std::array<std::pair<std::string_view, search::SortField>, 4> kFields = {{
        {"n", search::SortField::Name},
        {"u", search::SortField::Updated},
        {"c", search::SortField::Created},
        {"s", search::SortField::Size},
    }};
    if (text == "r") {
        prefs_.sort.descending = !prefs_.sort.descending;
    } else {
        const auto* found = std::ranges::find(kFields, text, [](const auto& entry) { return entry.first; });
        if (found == kFields.end())
            return Intent::None;
        // Ad doğal olarak artan; boyut ve tarihte önce en büyük ve en yeni.
        prefs_.sort = {found->second, found->second != search::SortField::Name};
    }
    focus_ = Focus::List;
    refilter(false);
    return Intent::SavePrefs;
}

void TuiState::set_preview_line_count(std::size_t lines) {
    preview_lines_ = lines;
    scroll_preview(0);  // yeni sınıra kenetle
}

// Sorguda açıkça `sort:` verildiyse o sıra; eşitlikte gelen sıra kalır.
void TuiState::sort_rows(std::vector<Row>& rows, const search::SortSpec& sort) const {
    std::vector<std::size_t> indices;
    indices.reserve(rows.size());
    std::map<std::size_t, Row*> by_meta;
    for (Row& row : rows)
        if (const auto found = meta_index_.find(row.path); found != meta_index_.end()) {
            indices.push_back(found->second);
            by_meta.emplace(found->second, &row);
        }
    search::sort_notes(indices, sort, metas_);
    std::vector<Row> sorted;
    sorted.reserve(rows.size());
    for (const std::size_t index : indices)
        sorted.push_back(std::move(*by_meta.at(index)));
    rows = std::move(sorted);
}

std::vector<Row> TuiState::rows_by_name(const search::ParsedQuery& parsed) const {
    std::vector<Row> rows;
    if (parsed.filters.empty()) {
        for (auto& ranked : search::rank_notes(parsed.text, notes_))
            rows.push_back({notes_[ranked.index].path, std::move(ranked.match.positions)});
        return rows;
    }
    std::vector<core::NoteInfo> subset;
    for (const std::size_t index : search::apply(parsed.filters, std::nullopt, metas_))
        subset.push_back(notes_[index]);
    for (auto& ranked : search::rank_notes(parsed.text, subset))
        rows.push_back({subset[ranked.index].path, std::move(ranked.match.positions)});
    return rows;
}

std::vector<Row> TuiState::rows_by_content(const search::ParsedQuery& parsed) {
    std::vector<Row> rows;
    if (!content_searcher_)
        return rows;  // henüz bağlanmadı: çökmek yerine boş liste
    std::set<core::NotePath> allowed;
    for (const std::size_t index : search::apply(parsed.filters, std::nullopt, metas_))
        allowed.insert(metas_[index].path);
    for (auto& result : content_searcher_(parsed.text))
        if (parsed.filters.empty() || allowed.contains(result.path))
            rows.push_back({std::move(result.path), {}, result.hit_count, std::move(result.first_hits)});
    return rows;
}

std::vector<Row> TuiState::rows_global() {
    std::vector<Row> rows;
    auto hits = global_searcher_(query_, metas_);
    if (!hits) {
        query_error_ = std::move(hits.error());
        return rows;
    }
    for (search::GlobalHit& hit : *hits) {
        Row row{std::move(hit.path), std::move(hit.name_positions), hit.content_hits};
        if (hit.best_line) {
            row.hits.push_back(std::move(*hit.best_line));
            row.show_best_line = true;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// Düşüş: rows dolar; argüman bozuksa false ve query_error_ dolu.
bool TuiState::filter_rows(std::vector<Row>& rows) {
    if (trash_view_) {
        for (const core::NoteMeta& meta : trash_metas_)
            rows.push_back({meta.path});
        return true;
    }
    auto parsed = search::parse_query(query_);
    if (!parsed) {
        query_error_ = std::move(parsed.error());
        return false;
    }
    query_error_.reset();
    query_has_arguments_ = !parsed->filters.empty() || parsed->sort.has_value();
    if (parsed->text.empty()) {
        for (const std::size_t index :
             search::apply(parsed->filters, parsed->sort.value_or(prefs_.sort), metas_))
            rows.push_back({metas_[index].path});
        return true;
    }
    if (mode_ == Mode::Global && global_searcher_) {
        rows = rows_global();  // sıralamayı search_all yapar
        return !query_error_;
    }
    rows = mode_ == Mode::Content ? rows_by_content(*parsed) : rows_by_name(*parsed);
    if (parsed->sort)
        sort_rows(rows, *parsed->sort);
    return true;
}

void TuiState::refilter(bool query_changed) {
    std::optional<core::NotePath> previous_path;
    if (const Row* row = selected_row())
        previous_path = row->path;

    std::vector<Row> rows;
    if (!filter_rows(rows))
        return;  // bozuk argüman: metin sayılmaz, liste son geçerli haliyle kalır
    rows_ = std::move(rows);

    selected_ = 0;
    if (previous_path) {
        const auto it = std::ranges::find(rows_, *previous_path, &Row::path);
        if (it != rows_.end())
            selected_ = static_cast<std::size_t>(it - rows_.begin());
    }
    const bool same_note = previous_path && selected_row() && selected_row()->path == *previous_path;
    if (!same_note || query_changed)
        preview_scroll_ = selected_row() ? initial_scroll(*selected_row()) : 0;
}

bool TuiState::reveal(const core::NotePath& path) {
    query_.clear();
    mode_ = Mode::Name;
    refilter(true);
    const auto it = std::ranges::find(rows_, path, &Row::path);
    focus_ = Focus::List;  // yeni/taşınan not üzerinde hemen harf kısayolları çalışsın
    if (it == rows_.end())
        return false;
    selected_ = static_cast<std::size_t>(it - rows_.begin());
    preview_scroll_ = 0;
    return true;
}

bool TuiState::select_note(const core::NotePath& path) {
    auto it = std::ranges::find(rows_, path, &Row::path);
    if (it == rows_.end() && !query_.empty()) {
        query_.clear();
        mode_ = Mode::Name;
        refilter(true);
        it = std::ranges::find(rows_, path, &Row::path);
    }
    if (it == rows_.end())
        return false;
    selected_ = static_cast<std::size_t>(it - rows_.begin());
    return true;
}

void TuiState::open_new_note_view() {
    new_note_view_ = true;
    layout_.set_zoomed(true);
}

void TuiState::leave_editor(std::optional<std::size_t> preview_line) {
    if (new_note_view_) {
        new_note_view_ = false;
        layout_.set_zoomed(false);
        preview_line.reset();  // not listede seçili kalır (on_saved), önizleme baştan
        preview_scroll_ = 0;
    }
    if (!preview_line) {
        focus_ = Focus::List;
        return;
    }
    focus_ = Focus::Preview;
    preview_scroll_ = *preview_line;
    scroll_preview(0);
}

void TuiState::set_preview_lines(const std::vector<std::string>* lines, std::size_t generation) {
    preview_text_ = lines;
    set_preview_line_count(lines->size());
    if (generation != preview_generation_) {
        preview_generation_ = generation;
        preview_selection_.reset();  // uçlar eski metne ait
        preview_find_.recompute(*lines);
    }
}

void TuiState::begin_preview_selection(PreviewPoint point, int x, int y) {
    preview_selection_ = PreviewSelection{point, point, x, y};
}

void TuiState::extend_preview_selection(PreviewPoint point, int x, int y) {
    if (!preview_selection_)
        return;
    PreviewSelection& selection = *preview_selection_;
    selection.moved = selection.moved || x != selection.x || y != selection.y;
    selection.head = point;
    selection.x = x;
    selection.y = y;
}

bool TuiState::finish_preview_selection() {
    if (!preview_selection_)
        return false;
    preview_selection_->dragging = false;
    if (preview_selection_->moved)
        return true;
    preview_selection_.reset();  // yalnız tıklama: seçim yok
    return false;
}

std::optional<PreviewRange> TuiState::preview_selection_range() const {
    if (!preview_selection_ || !preview_selection_->moved)
        return std::nullopt;
    const PreviewPoint& a = preview_selection_->anchor;
    const PreviewPoint& b = preview_selection_->head;
    const bool forward = std::pair(a.line, a.begin) <= std::pair(b.line, b.begin);
    const PreviewPoint& first = forward ? a : b;
    const PreviewPoint& last = forward ? b : a;
    return PreviewRange{first.line, first.begin, last.line, last.end};
}

std::optional<ByteRange> TuiState::preview_selection_on_line(std::size_t line, std::size_t line_size) const {
    const auto range = preview_selection_range();
    if (!range || line < range->begin_line || line > range->end_line)
        return std::nullopt;
    return ByteRange{line == range->begin_line ? range->begin_offset : 0,
                     line == range->end_line ? range->end_offset : line_size};
}

void TuiState::set_window(int width, int height) {
    window_width_ = width;
    window_height_ = height;
}

// Seçili satır (ek satırlarıyla) pencerenin altına oturacak en üstteki satır.
std::size_t TuiState::list_top(std::size_t visible) const {
    if (rows_.empty())
        return 0;
    std::size_t top = selected_;
    std::size_t used = row_height(selected_);
    while (top > 0 && used + row_height(top - 1) <= visible) {
        used += row_height(top - 1);
        --top;
    }
    return top;
}

void TuiState::click_row(std::size_t row) {
    leave_preview();
    focus_ = Focus::List;
    if (row < rows_.size())
        select_index(row);
}

void TuiState::focus_preview() {
    if (focus_ != Focus::PreviewFind)
        focus_ = Focus::Preview;
}

void TuiState::leave_preview() {
    preview_find_.clear();
    focus_ = Focus::List;
}

void TuiState::scroll_to_find_match() {
    if (const auto line = preview_find_.current_line()) {
        preview_scroll_ = *line > kHitContextLines ? *line - kHitContextLines : 0;
        scroll_preview(0);
    }
}

bool TuiState::handle_layout_letter(std::string_view text) {
    if (text == "<")
        layout_.resize(-PaneLayout::kKeyStep, window_width_);
    else if (text == ">")
        layout_.resize(PaneLayout::kKeyStep, window_width_);
    else if (text == "z")
        layout_.toggle_zoom();
    else
        return false;
    return true;
}

void TuiState::select_index(std::size_t index) {
    if (rows_.empty())
        return;
    selected_ = std::min(index, rows_.size() - 1);
    preview_scroll_ = initial_scroll(rows_[selected_]);
}

void TuiState::move_selection(int delta) {
    if (rows_.empty())
        return;
    const auto last = static_cast<int>(rows_.size()) - 1;
    const int next = std::clamp(static_cast<int>(selected_) + delta, 0, last);
    if (static_cast<std::size_t>(next) != selected_) {
        selected_ = static_cast<std::size_t>(next);
        preview_scroll_ = initial_scroll(rows_[selected_]);
    }
}

std::size_t TuiState::page_rows() const {
    return static_cast<std::size_t>(std::max(window_height_ - kChromeLines, 1));
}

// Tuş tekrarında adım büyür, en çok sayfanın üçte biri; duraklama ya da yön değişimi sıfırlar.
void TuiState::scroll_accelerated(int direction) {
    const auto now = clock_();
    const bool repeating = direction == scroll_direction_ && now - last_scroll_key_ < kKeyRepeatGap;
    scroll_repeats_ = repeating ? scroll_repeats_ + 1 : 0;
    scroll_direction_ = direction;
    last_scroll_key_ = now;
    const int max_step = std::max(static_cast<int>(page_rows()) / kMaxStepPageDivisor, 1);
    scroll_preview(direction * std::min(1 + scroll_repeats_ / kRepeatsPerStep, max_step));
}

std::size_t TuiState::max_preview_scroll() const {
    return preview_lines_ == 0 ? 0 : preview_lines_ - 1;
}

void TuiState::scroll_preview(int delta) {
    const std::size_t step = static_cast<std::size_t>(delta < 0 ? -delta : delta);
    const std::size_t next =
        delta < 0 ? (preview_scroll_ > step ? preview_scroll_ - step : 0) : preview_scroll_ + step;
    preview_scroll_ = std::min(next, max_preview_scroll());
}

Intent TuiState::handle(Key key, std::string_view text) {
    preview_selection_.reset();
    // Editörde her tuş metne aittir (Ctrl-C kopyalar, F2 kaydedilmemiş metni sorar); yalnız yardım araya
    // girer.
    if (focus_ == Focus::Editor)
        return key == Key::F1 ? Intent::ShowHelp : Intent::EditorKey;
    if (const auto intent = app_intent(key))
        return *intent;
    if (trash_view_)
        return handle_trash(key, text);
    if (focus_ == Focus::SortMenu) {
        if (key == Key::Escape)
            focus_ = Focus::List;
        return key == Key::Char ? handle_sort_menu(text) : Intent::None;
    }
    if (focus_ == Focus::Preview)
        return handle_preview(key, text);
    if (focus_ == Focus::PreviewFind)
        return handle_preview_find(key, text);
    if (const auto intent = handle_shared(key))
        return *intent;
    return focus_ == Focus::Search ? handle_search(key, text) : handle_list(key, text);
}

std::optional<Intent> TuiState::handle_shared(Key key) {
    switch (key) {
    case Key::CtrlP:
        move_selection(-1);
        return Intent::None;
    case Key::CtrlN:
        move_selection(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        move_selection(page_direction(key) * static_cast<int>(page_rows()));
        return Intent::None;
    case Key::Tab:
        mode_ = mode_ == Mode::Name ? Mode::Content : Mode::Name;
        refilter(true);
        return Intent::None;
    case Key::CtrlO:
        return Intent::NewNote;
    case Key::CtrlR:
        return selected_row() ? Intent::MoveSelected : Intent::None;
    case Key::CtrlD:
        return selected_row() ? Intent::TrashSelected : Intent::None;
    case Key::AltRight:
    case Key::AltLeft:
        set_selected_expanded(key == Key::AltRight);
        return Intent::None;
    case Key::CtrlS:
        return Intent::SyncNow;
    default:
        return std::nullopt;
    }
}

Intent TuiState::handle_list(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        return handle_list_letter(text);
    case Key::Up:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
        move_selection(1);
        return Intent::None;
    case Key::Right:
        focus_ = Focus::Preview;
        return Intent::None;
    case Key::Enter:
        return selected_row() ? Intent::EnterEditor : Intent::None;
    case Key::F2:
        return intent_if_selected(Intent::RenameSelected);
    case Key::Escape:
        // Kullanıcı kararı: çıkış `q`; boş süzgeçte tekrarlanan Esc uygulamayı kapatmamalı.
        if (!query_.empty() || mode_ == Mode::Global) {
            query_.clear();
            if (mode_ == Mode::Global)
                mode_ = Mode::Name;
            refilter(true);
        }
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_list_letter(std::string_view text) {
    // Tam eşleşme: ı gibi çok baytlı harfler ve büyük harf eylemleri (CapsLock) hiçbir şey tetiklemez.
    for (const auto& [letter, key] : kListLetterKeys)
        if (text == letter)
            return handle(key);
    if (handle_layout_letter(text) || handle_details_letter(text))
        return Intent::None;
    for (const auto& [letter, intent] : kListSelectionLetters)
        if (text == letter)
            return intent_if_selected(intent);
    for (const auto& [letter, intent] : kListPlainLetters)
        if (text == letter)
            return intent;
    if (text == "v") {
        prefs_.column = static_cast<ListColumn>((static_cast<int>(prefs_.column) + 1) % kListColumnCount);
        return Intent::SavePrefs;
    }
    if (text == "g")
        select_index(0);
    else if (text == "G")
        select_index(rows_.size());  // son satıra kenetlenir
    else if (text == "/")
        begin_search(Mode::Name);
    else if (text == "f")
        begin_search(Mode::Global);
    else if (text == "o")
        focus_ = Focus::SortMenu;
    return Intent::None;
}

void TuiState::begin_search(Mode mode) {
    focus_ = Focus::Search;
    if (mode == Mode::Global) {
        query_.clear();
        mode_ = Mode::Global;
        refilter(true);
    } else if (mode_ == Mode::Global) {
        mode_ = Mode::Name;
        refilter(true);
    }
}

Intent TuiState::search_enter() {
    if (mode_ == Mode::Global) {
        if (selected_row() && !query_error_)
            focus_ = Focus::Preview;  // satır seçilince önizleme eşleşmeye kaydı
        return Intent::None;
    }
    if (selected_row()) {
        focus_ = Focus::List;  // seçim kalır; açmak `e` ya da Enter ile Listeden
        return Intent::None;
    }
    // Argümanlı sorgu metin değildir: taslağın ilk satırı olamaz.
    return query_.empty() || query_error_ || query_has_arguments_ ? Intent::None : Intent::CreateFromQuery;
}

Intent TuiState::handle_search(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_printable(text)) {
            query_.append(text);
            refilter(true);
        }
        return Intent::None;
    case Key::Backspace:
        if (!query_.empty()) {
            while (is_continuation(query_.back()) && query_.size() > 1)
                query_.pop_back();
            query_.pop_back();  // baş bayt
            refilter(true);
        }
        return Intent::None;
    case Key::Up:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
        move_selection(1);
        return Intent::None;
    case Key::Enter:
        return search_enter();
    case Key::Escape:
        focus_ = Focus::List;  // süzgeç kalır: filtrelenmiş listede hemen `d` çalışsın
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_preview(Key key, std::string_view text) {
    // Liste komutları burada bilerek yok: içerik panelinde çalışmazlar (Ctrl-P/N ve Tab dahil); çıkış `q`
    // ortaktır.
    const int half_page = static_cast<int>(page_rows()) / 2;
    switch (key) {
    case Key::Char:
        return handle_preview_letter(text);
    case Key::Up:
        scroll_accelerated(-1);
        return Intent::None;
    case Key::Down:
        scroll_accelerated(1);
        return Intent::None;
    case Key::CtrlU:
        scroll_preview(-half_page);
        return Intent::None;
    case Key::CtrlD:
        scroll_preview(half_page);
        return Intent::None;
    case Key::F2:
        return intent_if_selected(Intent::RenameSelected);
    case Key::PageUp:
        scroll_preview(-static_cast<int>(kPreviewPageLines));
        return Intent::None;
    case Key::PageDown:
        scroll_preview(static_cast<int>(kPreviewPageLines));
        return Intent::None;
    case Key::Left:
        leave_preview();
        return Intent::None;
    case Key::Escape:
        // İlk Esc vurguyu temizler, ikincisi Listeye döner: arama yanlışlıkla panelden çıkarmasın.
        if (preview_find_.query().empty())
            leave_preview();
        else
            preview_find_.clear();
        return Intent::None;
    case Key::Enter:
        return selected_row() ? Intent::EnterEditor : Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_preview_letter(std::string_view text) {
    if (handle_layout_letter(text))
        return Intent::None;
    if (text == "q")
        return Intent::Quit;
    if (text == ",")
        return Intent::ShowSettings;
    for (const auto& [letter, intent] : kPreviewSelectionLetters)
        if (text == letter)
            return intent_if_selected(intent);
    for (const auto& [letter, lines] : kPreviewScrollLetters)
        if (text == letter) {
            scroll_preview(lines);
            return Intent::None;
        }
    if (text == "g")
        preview_scroll_ = 0;
    else if (text == "G")
        preview_scroll_ = max_preview_scroll();
    else if (text == "h")
        leave_preview();
    else if (text == "/") {
        preview_find_.clear();
        focus_ = Focus::PreviewFind;
    } else if (text == "n") {
        preview_find_.next();
        scroll_to_find_match();
    } else if (text == "N") {
        preview_find_.previous();
        scroll_to_find_match();
    }
    return Intent::None;
}

Intent TuiState::handle_preview_find(Key key, std::string_view text) {
    static const std::vector<std::string> kNoLines;
    const std::vector<std::string>& lines = preview_text_ ? *preview_text_ : kNoLines;
    std::string query = preview_find_.query();
    switch (key) {
    case Key::Char:
        if (!is_printable(text))
            return Intent::None;
        query.append(text);
        break;
    case Key::Backspace:
        if (query.empty())
            return Intent::None;
        while (is_continuation(query.back()) && query.size() > 1)
            query.pop_back();
        query.pop_back();
        break;
    case Key::Enter:
        focus_ = Focus::Preview;  // vurgular kalır; `n` `N` gezer
        return Intent::None;
    case Key::Escape:
        preview_find_.clear();
        focus_ = Focus::Preview;
        return Intent::None;
    case Key::Down:
        preview_find_.next();
        scroll_to_find_match();
        return Intent::None;
    case Key::Up:
        preview_find_.previous();
        scroll_to_find_match();
        return Intent::None;
    default:
        return Intent::None;
    }
    preview_find_.set_query(std::move(query), lines);
    scroll_to_find_match();
    return Intent::None;
}

}  // namespace noteapp::tui
