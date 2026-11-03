#include "tui/tui_state.hpp"

#include "core/note_views.hpp"
#include "search/fuzzy.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <array>
#include <iterator>
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

std::size_t initial_scroll(const Row& row) {
    if (row.hits.empty() || row.hits.front().line == 0)
        return 0;
    const std::size_t line_index = row.hits.front().line - 1;  // Hit::line 1 tabanlı
    return line_index > kHitContextLines ? line_index - kHitContextLines : 0;
}

constexpr int kListColumnCount = 7;
// Bu aralıktan sık gelen aynı yön tuşu tekrardır; daha uzun duraklama hızlanmayı sıfırlar.
constexpr auto kKeyRepeatGap = std::chrono::milliseconds(120);
constexpr int kRepeatsPerStep = 8;  // adım her bu kadar tekrarda bir büyür
constexpr int kMaxStepPageDivisor = 3;

// Bu aralıkta aynı klasör satırına ikinci tıklama içeri girer.
constexpr auto kDoubleClickGap = std::chrono::milliseconds(400);

int page_direction(Key key) {
    return key == Key::PageUp ? -1 : 1;
}

bool has_marked_ancestor(const std::set<std::string>& marks, const std::string& path) {
    for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
        if (marks.contains(path.substr(0, slash + 1)))
            return true;
    return false;
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

// Satırın seçim kimliği: not yolu `/` ile bitmez, klasör satırıyla karışmaz.
std::string TuiState::row_key(const Row& row) {
    return row.is_folder ? row.path.string() + "/" : row.path.string();
}

std::vector<ListItem> TuiState::action_items() const {
    const std::set<std::string>& active = marks();
    std::vector<ListItem> items;
    if (active.empty()) {
        if (const Row* row = selected_entry())
            items.push_back({row->path, row->is_folder});
        return items;
    }
    for (const std::string& key : active) {
        const bool is_folder = key.ends_with('/');
        const std::string path = is_folder ? key.substr(0, key.size() - 1) : key;
        if (has_marked_ancestor(active, path))
            continue;  // seçili klasörle birlikte gider, ikinci kez işlenip hata vermesin
        if (auto parsed = core::NotePath::parse(path))
            items.push_back({std::move(*parsed), is_folder});
    }
    return items;
}

void TuiState::prune_marks() {
    std::erase_if(marks_, [this](const std::string& key) {
        if (key.ends_with('/'))
            return tree_.find(std::string_view(key).substr(0, key.size() - 1)) == nullptr;
        const auto path = core::NotePath::parse(key);
        return !path || !meta_index_.contains(*path);
    });
}

void TuiState::toggle_mark() {
    const Row* row = selected_entry();
    if (!row)
        return;
    std::set<std::string>& active = active_marks();
    if (const std::string key = row_key(*row); !active.erase(key))
        active.insert(key);
    move_selection(1);
}

void TuiState::mark_all() {
    for (const Row& row : rows_)
        active_marks().insert(row_key(row));
}

void TuiState::begin_yank() {
    std::vector<ListItem> items = action_items();
    if (items.empty())
        return;
    yank_ = PendingYank{std::move(items)};
    marks_.clear();
}

// `g` öneki: süre yok; bilinmeyen tuş iptal eder ve başka iş yapmaz. `gg` `gh` `gt` tablodadır.
Intent TuiState::handle_prefix(Key key, std::string_view text) {
    const std::optional<unsigned> context = command_context();
    if (key != Key::Char || !context)
        return Intent::None;
    const Command* command = bound_command(*context, Key::Char, "g" + std::string(text));
    return command ? run_bound(*command) : Intent::None;
}

void TuiState::go_root() {
    if (trash_view_)
        leave_trash();
    search_line_.clear();
    mode_ = Mode::Name;
    go_to_folder({});
}

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.revision == y.revision && x.mtime == y.mtime && x.size == y.size;
    });
}

// Okunamayan görünüm listesi notları gizlemez: liste boş kalır, `view:` hatayı kendisi söyler.
std::vector<std::string> view_names(const core::NoteStore& store) {
    std::vector<std::string> names;
    if (const auto views = core::list_views(store))
        for (const core::SavedView& view : *views)
            names.push_back(view.name);
    return names;
}

StoreScan scan_store(const core::NoteStore& store) {
    const auto trash = store.list_trash();
    return StoreScan{store.list(), trash ? trash->size() : 0, store.folders(), store.data_version(),
                     view_names(store)};
}

bool same_scan(const StoreScan& a, const StoreScan& b) {
    return same_listing(a.notes, b.notes) && a.trash == b.trash && a.folders == b.folders &&
           a.graph_version == b.graph_version;
}

bool right_pane_focused(Focus focus) {
    return focus == Focus::Preview || focus == Focus::PreviewFind || focus == Focus::Editor ||
           focus == Focus::Links;
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

void TuiState::set_views(std::vector<std::string> names) {
    if (names == views_)
        return;
    views_ = std::move(names);
    input_sources_.reset();
}

void TuiState::set_folders(std::vector<std::string> folders) {
    if (folders == folders_)
        return;
    folders_ = std::move(folders);
    rebuild_tree();
    refilter(false);
}

void TuiState::rebuild_tree() {
    tree_.rebuild(metas_, folders_);
    prune_marks();
}

void TuiState::rebuild_metas() {
    metas_.clear();
    meta_index_.clear();
    input_sources_.reset();
    relative_sources_.reset();
    metas_.reserve(notes_.size());
    for (const core::NoteInfo& note : notes_) {
        core::NoteMeta meta{.path = note.path,
                            .size = note.size,
                            .updated_at = note.mtime,
                            .created_at = note.created_at,
                            .origin = note.origin,
                            .local_seq = note.local_seq,
                            .created_local_seq = note.created_local_seq,
                            .tags = note.tags};
        if (const auto found = sync_states_.find(note.path); found != sync_states_.end())
            meta.sync_status = found->second;
        meta_index_.emplace(note.path, metas_.size());
        metas_.push_back(std::move(meta));
    }
    rebuild_tree();
}

void TuiState::set_sync_states(std::map<core::NotePath, core::SyncState> states) {
    sync_states_ = std::move(states);
    rebuild_metas();
    refilter(false);
}

const SharedInputSources& TuiState::input_sources() const {
    if (!input_sources_)
        input_sources_ = build_input_sources(metas_, views_);
    return input_sources_;
}

SharedInputSources TuiState::name_input_sources() const {
    const std::string base = name_base_folder();
    if (base.empty())
        return input_sources();
    if (relative_sources_ && relative_sources_folder_ == base)
        return relative_sources_;
    std::vector<core::NoteMeta> relative;
    for (const core::NoteMeta& meta : metas_) {
        const std::string path = meta.path.string();
        if (!in_folder(path, base))
            continue;
        core::NoteMeta copy = meta;
        if (auto parsed = core::NotePath::parse(std::string_view(path).substr(base.size() + 1))) {
            copy.path = std::move(*parsed);
            relative.push_back(std::move(copy));
        }
    }
    relative_sources_ = build_input_sources(relative);
    relative_sources_folder_ = base;
    return relative_sources_;
}

Row TuiState::folder_row(const FolderNode& node) const {
    Row row{core::NotePath::parse(node.path).value()};
    row.is_folder = true;
    row.label = last_component(node.path) + "/";
    return row;
}

// Açık klasörün çocukları kendi satırının altına girer; numara her seviyede yeniden başlar. Liste yalnız
// değişimde kurulur, gezinme buraya gelmez.
void TuiState::append_folder_rows(const FolderNode& node, std::size_t depth, std::vector<Row>& rows) const {
    std::size_t number = 0;
    for (const FolderNode* folder : search::sorted_subfolders(tree_, node, prefs_.sort)) {
        Row row = folder_row(*folder);
        row.depth = depth;
        row.number = ++number;
        rows.push_back(std::move(row));
        if (is_folder_open(folder->path))
            append_folder_rows(*folder, depth + 1, rows);
    }
    std::vector<std::size_t> notes = node.notes;
    search::sort_notes(notes, prefs_.sort, metas_);
    for (const std::size_t index : notes) {
        Row row{metas_[index].path};
        row.label = last_component(row.path.string());
        row.depth = depth;
        row.number = ++number;
        rows.push_back(std::move(row));
    }
}

std::vector<Row> TuiState::rows_in_folder() {
    current_folder_ = tree_.nearest_existing(std::move(current_folder_));
    std::vector<Row> rows;
    append_folder_rows(*tree_.find(current_folder_), 0, rows);
    return rows;
}

void TuiState::enter_folder(std::string folder) {
    current_folder_ = std::move(folder);
    refilter(true);
    select_index(0);
}

void TuiState::go_to_folder(std::string folder) {
    const std::string from = std::exchange(current_folder_, std::move(folder));
    leave_preview();
    refilter(true);
    if (!in_folder(from, current_folder_) || from == current_folder_)
        return;
    const std::size_t begin = current_folder_.empty() ? 0 : current_folder_.size() + 1;
    const std::size_t end = from.find('/', begin);
    select_key(from.substr(0, end) + "/");
}

void TuiState::select_key(const std::string& key) {
    const auto found = std::ranges::find_if(rows_, [&](const Row& row) { return row_key(row) == key; });
    if (found != rows_.end())
        select_index(static_cast<std::size_t>(found - rows_.begin()));
}

bool TuiState::select_folder(const std::string& folder) {
    search_line_.clear();
    mode_ = Mode::Name;
    current_folder_ = parent_folder(folder);
    focus_ = Focus::List;
    refilter(true);
    select_key(folder + "/");
    const Row* row = selected_folder_row();
    return row && row->path.string() == folder;
}

bool TuiState::open_selected_folder() {
    const Row* row = selected_folder_row();
    if (!row)
        return false;
    enter_folder(row->path.string());
    return true;
}

// Düz sonuçta notun klasörüne gider; klasör görünümünde üst klasör, kökte etkisiz.
void TuiState::go_left() {
    if (!in_folder_view()) {
        if (const Row* row = selected_row()) {
            const core::NotePath path = row->path;
            reveal(path);
        }
        return;
    }
    if (!current_folder_.empty())
        go_to_folder(parent_folder(current_folder_));
}

std::vector<Row>::const_iterator TuiState::find_note_row(const core::NotePath& path) const {
    return std::ranges::find_if(rows_, [&](const Row& row) { return !row.is_folder && row.path == path; });
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
    search_line_.clear();
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
    std::set<std::string> present;
    for (const core::NoteMeta& meta : trash_metas_)
        present.insert(meta.path.string());
    std::erase_if(trash_marks_, [&](const std::string& key) { return !present.contains(key); });
    refilter(false);
}

void TuiState::leave_trash() {
    trash_view_ = false;
    trash_metas_.clear();
    trash_marks_.clear();
    selected_ = 0;
    saved_spot_.reset();  // çöpten dönüş ilk satıra iner (arama dönüşünden farklı)
    refilter(true);
}

void TuiState::set_list_prefs(const ListPrefs& prefs) {
    prefs_ = prefs;
    refilter(false);
}

std::size_t TuiState::row_height(std::size_t row) const {
    ++work_.row_visits;
    const Row& shown = rows_[row];
    const std::size_t details = shown.is_folder ? kFolderMetaLines : kMetaLines;
    return 1 + (shown.show_best_line ? 1 : 0) + (is_expanded(shown) ? details : 0);
}

bool TuiState::is_expanded(const Row& row) const {
    return (row.is_folder ? expanded_folders_ : expanded_).contains(row.path);
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

void TuiState::toggle_tree(std::size_t row) {
    last_click_row_.reset();
    if (row >= rows_.size() || !rows_[row].is_folder)
        return;
    const std::string folder = rows_[row].path.string();
    if (!prefs_.open_folders.erase(folder))
        prefs_.open_folders.insert(folder);
    refilter(false);
}

void TuiState::prune_open_folders() {
    std::erase_if(prefs_.open_folders, [this](const std::string& folder) { return !tree_.find(folder); });
}

void TuiState::toggle_expanded(std::size_t row) {
    last_click_row_.reset();  // işaret tıklaması çift tıklamanın ilk yarısı sayılmaz
    if (row >= rows_.size())
        return;
    const core::NotePath& path = rows_[row].path;
    std::set<core::NotePath>& open = rows_[row].is_folder ? expanded_folders_ : expanded_;
    if (!open.erase(path))
        open.insert(path);
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
    case Key::AltRight:
    case Key::AltLeft:
        set_selected_expanded(key == Key::AltRight);
        return Intent::None;
    case Key::Escape:
        return Intent::ToggleTrash;
    case Key::Char:
        if (text == "g")
            awaiting_prefix_ = true;
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::intent_if_selected(Intent intent) const {
    return selected_row() ? intent : Intent::None;
}

Intent TuiState::intent_if_any_row(Intent intent) const {
    return selected_entry() || marked_count() > 0 ? intent : Intent::None;
}

void TuiState::set_selected_expanded(bool expanded) {
    const Row* row = selected_entry();
    if (!row)
        return;
    std::set<core::NotePath>& open = row->is_folder ? expanded_folders_ : expanded_;
    if (expanded)
        open.insert(row->path);
    else
        open.erase(row->path);
}

void TuiState::toggle_all_expanded() {
    const bool all_open = std::ranges::all_of(rows_, [this](const Row& row) { return is_expanded(row); });
    for (const Row& row : rows_) {
        std::set<core::NotePath>& open = row.is_folder ? expanded_folders_ : expanded_;
        if (all_open)
            open.erase(row.path);
        else
            open.insert(row.path);
    }
}

// `Alt+ok` her terminalde gelmez: Space ve `i` seçili satırı, `I` tümünü açar/kapatır.
void TuiState::toggle_selected_details() {
    toggle_expanded(selected_);
}

// Space: klasör yerinde açılır (açık küme `tui-state.toml`da kalır), notta ayrıntı.
Intent TuiState::toggle_row() {
    if (!selected_folder_row()) {
        toggle_selected_details();
        return Intent::None;
    }
    toggle_tree(selected_);
    return Intent::SavePrefs;
}

// Tümü açıksa kapatır, değilse hepsini açar (`I` gibi).
Intent TuiState::toggle_tree_all() {
    std::set<std::string> all;
    for (std::string& folder : tree_.folders_under({}))
        if (!folder.empty())
            all.insert(std::move(folder));
    const bool all_open = std::ranges::includes(prefs_.open_folders, all);
    prefs_.open_folders = all_open ? std::set<std::string>{} : std::move(all);
    refilter(false);
    return Intent::SavePrefs;
}

Intent TuiState::cycle_column() {
    return set_column(static_cast<ListColumn>((static_cast<int>(prefs_.column) + 1) % kListColumnCount));
}

Intent TuiState::set_column(ListColumn column) {
    prefs_.column = column;
    return Intent::SavePrefs;
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
    auto hits = global_searcher_(query(), metas_);
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
    if (in_folder_view()) {
        query_error_.reset();
        query_has_arguments_ = false;
        rows = rows_in_folder();
        return true;
    }
    auto parsed = search::parse_query(query(), view_lookup_);
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

void TuiState::number_rows() {
    ++work_.rebuilds;
    max_row_number_ = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].number == 0)
            rows_[i].number = i + 1;  // düz liste: sonuç sırası
        max_row_number_ = std::max(max_row_number_, rows_[i].number);
    }
}

void TuiState::refilter(bool query_changed) {
    std::optional<std::string> previous;
    if (const Row* row = selected_entry())
        previous = row_key(*row);
    const bool now_folders = in_folder_view();
    if (showing_folders_ && !now_folders)
        saved_spot_ = previous;

    std::vector<Row> rows;
    if (!filter_rows(rows))
        return;  // bozuk argüman: metin sayılmaz, liste son geçerli haliyle kalır
    rows_ = std::move(rows);
    number_rows();
    if (!showing_folders_ && now_folders)
        previous = std::exchange(saved_spot_, std::nullopt);
    showing_folders_ = now_folders;

    selected_ = 0;
    if (previous) {
        const auto it =
            std::ranges::find_if(rows_, [&](const Row& row) { return row_key(row) == *previous; });
        if (it != rows_.end())
            selected_ = static_cast<std::size_t>(it - rows_.begin());
    }
    const Row* now = selected_entry();
    const bool same_row = previous && now && row_key(*now) == *previous;
    if (!same_row || query_changed)
        scroll_to_source_line(now ? initial_scroll(*now) : 0);
}

bool TuiState::reveal(const core::NotePath& path) {
    search_line_.clear();
    mode_ = Mode::Name;
    if (meta_index_.contains(path))
        current_folder_ = folder_of(path);
    refilter(true);
    const auto it = find_note_row(path);
    focus_ = Focus::List;  // yeni/taşınan not üzerinde hemen harf kısayolları çalışsın
    if (it == rows_.end())
        return false;
    selected_ = static_cast<std::size_t>(it - rows_.begin());
    scroll_to_source_line(0);
    return true;
}

bool TuiState::select_note(const core::NotePath& path) {
    auto it = find_note_row(path);
    if (it == rows_.end() && (!query().empty() || in_folder_view())) {
        search_line_.clear();
        mode_ = Mode::Name;
        if (meta_index_.contains(path))
            current_folder_ = folder_of(path);  // panelde düzenlenen not listede de görünsün
        refilter(true);
        it = find_note_row(path);
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
        scroll_to_source_line(0);
    }
    if (!preview_line) {
        focus_ = Focus::List;
        return;
    }
    focus_ = Focus::Preview;
    scroll_to_source_line(*preview_line);
    scroll_preview(0);
}

std::size_t TuiState::source_line_at(std::size_t display_line) const {
    if (view_source_lines_.empty())
        return display_line;
    return view_source_lines_[std::min(display_line, view_source_lines_.size() - 1)];
}

// Satır bir bloğun ortasına düşebilir: o bloğun ilk görüntü satırı.
std::size_t TuiState::display_line_of(std::size_t source_line) const {
    if (view_source_lines_.empty())
        return source_line;
    const auto after = std::ranges::upper_bound(view_source_lines_, source_line);
    if (after == view_source_lines_.begin())
        return 0;
    const auto block = std::ranges::lower_bound(view_source_lines_, *std::prev(after));
    return static_cast<std::size_t>(block - view_source_lines_.begin());
}

void TuiState::scroll_to_source_line(std::size_t line) {
    preview_scroll_ = display_line_of(line);
    pending_source_line_ = line;
}

void TuiState::set_preview_view(const std::vector<std::size_t>* source_lines, std::size_t generation) {
    if (generation == view_generation_ && !pending_source_line_)
        return;
    const std::size_t top = pending_source_line_ ? *pending_source_line_ : source_line_at(preview_scroll_);
    pending_source_line_.reset();
    if (generation != view_generation_) {
        view_generation_ = generation;
        if (source_lines)
            view_source_lines_ = *source_lines;
        else
            view_source_lines_.clear();
    }
    preview_scroll_ = display_line_of(top);
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

void TuiState::set_links(const core::NoteLinks& links, std::size_t generation,
                         const std::optional<core::NotePath>& note) {
    if (generation == links_generation_)
        return;
    links_generation_ = generation;
    const bool same_note = links_note_ == note;
    links_note_ = note;
    links_.set(links, same_note);
    if (links_.empty() && focus_ == Focus::Links)
        focus_ = Focus::Preview;  // son ilişki kalktı: odak görünmez bölümde kalmasın
}

int TuiState::links_reserved_rows(int pane_rows) const {
    return trash_view_ || focus_ == Focus::Editor ? 0 : links_.reserved_rows(pane_rows);
}

void TuiState::click_link(std::size_t row) {
    const auto visible = static_cast<std::size_t>(links_.visible_rows(pane_rows()));
    const std::size_t line = links_.top(visible) + row;
    if (line >= links_.lines().size())
        return;
    focus_ = Focus::Links;
    if (links_.lines()[line].is_header)
        return;
    links_.select(line);
    if (links_.selected_path())
        jump_to_selected_link();
}

void TuiState::scroll_links(int delta) {
    links_.move(delta);
}

// Hedef süzgeçte ya da başka klasörde olabilir: select_note gevşetir, boş genel aramada reveal.
Intent TuiState::jump_to_selected_link() {
    const core::NotePath* selected = links_.selected_path();
    if (!selected)
        return Intent::LinkMissing;
    const core::NotePath target = *selected;
    if (!select_note(target) && !reveal(target))
        return Intent::LinkMissing;
    focus_ = Focus::Preview;
    scroll_to_source_line(0);
    return Intent::None;
}

void TuiState::begin_preview_selection(PreviewPoint point, int x, int y) {
    preview_selection_ = PreviewSelection{point, point, x, y};
}

// cppcheck-suppress passedByValue ; üç size_t, kopyası ucuz
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

void TuiState::note_row_click(std::size_t row) {
    const auto now = clock_();
    const bool again = last_click_row_ == row && now - last_click_at_ < kDoubleClickGap;
    last_click_row_ = row;
    last_click_at_ = now;
    if (again && row < rows_.size() && rows_[row].is_folder) {
        last_click_row_.reset();
        open_selected_folder();
    }
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
    scroll_to_source_line(initial_scroll(rows_[selected_]));
}

void TuiState::move_selection(int delta) {
    if (rows_.empty())
        return;
    const auto last = static_cast<int>(rows_.size()) - 1;
    const int next = std::clamp(static_cast<int>(selected_) + delta, 0, last);
    if (static_cast<std::size_t>(next) != selected_) {
        selected_ = static_cast<std::size_t>(next);
        scroll_to_source_line(initial_scroll(rows_[selected_]));
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
    const bool prefixed = std::exchange(awaiting_prefix_, false);
    if (const auto intent = app_intent(key)) {
        palette_.reset();
        return *intent;
    }
    if (palette_)
        return handle_palette(key, text);
    if (!summary_.empty()) {
        summary_.clear();
        return Intent::None;
    }
    if (prefixed)
        return handle_prefix(key, text);
    return handle_focused(key, text);
}

Intent TuiState::handle_focused(Key key, std::string_view text) {
    if (const auto intent = handle_command_key(key, text))
        return *intent;
    if (trash_view_)
        return handle_trash(key, text);
    if (focus_ == Focus::SortMenu) {
        if (key == Key::Escape)
            focus_ = Focus::List;
        return key == Key::Char ? handle_sort_menu(text) : Intent::None;
    }
    if (focus_ == Focus::Preview)
        return handle_preview(key);
    if (focus_ == Focus::PreviewFind)
        return handle_preview_find(key, text);
    if (focus_ == Focus::Links)
        return handle_links(key, text);
    if (focus_ == Focus::Search)
        if (const auto intent = handle_search_line(key, text))
            return *intent;
    if (const auto intent = handle_shared(key))
        return *intent;
    return focus_ == Focus::Search ? handle_search(key) : handle_list(key, text);
}

void TuiState::toggle_content_mode() {
    mode_ = mode_ == Mode::Name ? Mode::Content : Mode::Name;
    refilter(true);
}

// Metin, imleç, öneri, Tab listesi ve geçmiş girdinindir; geri kalan tuşlar aramanın ortak yoluna düşer.
std::optional<Intent> TuiState::handle_search_line(Key key, std::string_view text) {
    search_line_.set_sources(input_sources());
    const InputLine::Result result = search_line_.handle(key, text, prefs_.search_history);
    if (result == InputLine::Result::Edited)
        refilter(true);
    return result == InputLine::Result::Ignored ? std::nullopt : std::optional(Intent::None);
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
        toggle_content_mode();
        return Intent::None;
    case Key::CtrlO:
        return Intent::NewNote;
    case Key::CtrlR:
        return move_dialog_intent();
    case Key::CtrlD:
        return intent_if_any_row(Intent::TrashSelected);
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
        if (text == "g")
            awaiting_prefix_ = true;
        return Intent::None;
    case Key::Up:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
        move_selection(1);
        return Intent::None;
    case Key::Left:
        go_left();
        return Intent::None;
    case Key::Right:
        return right_or_open();
    case Key::Enter:
        return enter_selected();
    case Key::Escape:
        escape_list();
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::right_or_open() {
    if (!open_selected_folder())
        focus_ = Focus::Preview;
    return Intent::None;
}

Intent TuiState::enter_selected() {
    if (open_selected_folder())
        return Intent::None;
    return selected_row() ? Intent::EnterEditor : Intent::None;
}

// Klasörün tipi yok: diyalog değil, yol girdisi.
Intent TuiState::move_dialog_intent() const {
    if (selected_folder_row())
        return Intent::RenameSelected;
    return selected_row() ? Intent::MoveSelected : Intent::None;
}

void TuiState::page_list(int direction) {
    move_selection(direction * static_cast<int>(page_rows()));
}

void TuiState::begin_sort_menu() {
    focus_ = Focus::SortMenu;
}

void TuiState::select_last() {
    select_index(rows_.size());  // son satıra kenetlenir
}

void TuiState::resize_list(int direction) {
    layout_.resize(direction * PaneLayout::kKeyStep, window_width_);
}

void TuiState::toggle_zoom() {
    layout_.toggle_zoom();
}

void TuiState::focus_links() {
    focus_ = Focus::Links;
}

void TuiState::toggle_review_view() {
    review_rendered_ = !review_rendered_;
}

void TuiState::scroll_preview_top() {
    preview_scroll_ = 0;
}

void TuiState::scroll_preview_bottom() {
    preview_scroll_ = max_preview_scroll();
}

void TuiState::scroll_half_page(int direction) {
    scroll_preview(direction * (static_cast<int>(page_rows()) / 2));
}

void TuiState::begin_preview_find() {
    preview_find_.clear();
    focus_ = Focus::PreviewFind;
}

void TuiState::find_step(bool forward) {
    if (forward)
        preview_find_.next();
    else
        preview_find_.previous();
    scroll_to_find_match();
}

// Önce bekleyen yank, süzgeç ikinci Esc'te. Kullanıcı kararı: çıkış `q`; boş süzgeçte tekrarlanan Esc
// uygulamayı kapatmamalı.
void TuiState::escape_list() {
    if (yank_) {
        yank_.reset();
        return;
    }
    if (query().empty() && mode_ != Mode::Global)
        return;
    search_line_.clear();
    if (mode_ == Mode::Global)
        mode_ = Mode::Name;
    refilter(true);
}

void TuiState::begin_search(Mode mode) {
    focus_ = Focus::Search;
    if (mode == Mode::Global) {
        search_line_.clear();
        mode_ = Mode::Global;
        refilter(true);
    } else if (mode_ == Mode::Global) {
        mode_ = Mode::Name;
        refilter(true);
    }
}

// Palet: ad araması `tag:` ile açılır, Tab listesi notların etiketleridir; tek etiket varsa tamamlanır.
void TuiState::begin_tag_search() {
    begin_search(Mode::Name);
    search_line_.set_text(std::string(kTagArgument));
    refilter(true);
    handle_search_line(Key::Tab, {});
}

// Palet "Run view": `tag:` gibi; Tab listesi kayıtlı görünümlerdir, seçilen `view:AD` süzgecini kurar.
void TuiState::begin_view_search() {
    begin_search(Mode::Name);
    search_line_.set_text(std::string(kViewArgument));
    refilter(true);
    handle_search_line(Key::Tab, {});
}

Intent TuiState::search_enter() {
    if (mode_ == Mode::Global) {
        if (!selected_row() || query_error_)
            return Intent::None;
        focus_ = Focus::Preview;  // satır seçilince önizleme eşleşmeye kaydı
        return remember_query();
    }
    if (selected_entry()) {
        focus_ = Focus::List;  // seçim kalır; açmak `e` ya da Enter ile Listeden
        return query_error_ ? Intent::None : remember_query();
    }
    // Argümanlı sorgu metin değildir: taslağın ilk satırı olamaz.
    return query().empty() || query_error_ || query_has_arguments_ ? Intent::None : Intent::CreateFromQuery;
}

// Enter'la kullanılan arama geçmişe girer; eşleşmesiz sorgu notun ilk satırı olur, oraya girmez.
Intent TuiState::remember_query() {
    return remember_search(prefs_.search_history, query(), input_sources().get()) ? Intent::SavePrefs
                                                                                  : Intent::None;
}

Intent TuiState::handle_search(Key key) {
    switch (key) {
    case Key::CtrlT:
        toggle_content_mode();
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

// Harfler ve sayfa tuşları tablodadır. Liste komutları burada bilerek yok: içerik panelinde çalışmazlar
// (Ctrl-P/N ve Tab dahil).
Intent TuiState::handle_preview(Key key) {
    switch (key) {
    case Key::Up:
        scroll_accelerated(-1);
        return Intent::None;
    case Key::Down:
        scroll_accelerated(1);
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

Intent TuiState::handle_links(Key key, std::string_view text) {
    const int page = std::max(links_.visible_rows(pane_rows()), 1);
    switch (key) {
    case Key::Char:
        return handle_links_letter(text);
    case Key::Up:
        links_.move(-1);
        return Intent::None;
    case Key::Down:
        links_.move(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        links_.move(page_direction(key) * page);
        return Intent::None;
    case Key::Enter:
        return jump_to_selected_link();
    case Key::Escape:
        focus_ = Focus::Preview;
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_links_letter(std::string_view text) {
    if (handle_layout_letter(text))
        return Intent::None;
    if (text == "b") {
        focus_ = Focus::Preview;
    } else if (text == "j") {
        links_.move(1);
    } else if (text == "k") {
        links_.move(-1);
    } else if (text == "L") {
        const core::LinkLine* line = links_.selected_line();
        return line && line->group == core::LinkGroup::Related ? Intent::UnlinkSelected
                                                               : intent_if_selected(Intent::LinkNote);
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
