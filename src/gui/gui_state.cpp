#include "gui/gui_state.hpp"

#include "app/document.hpp"
#include "app/stray_notice.hpp"
#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_views.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "search/fold.hpp"
#include "search/query.hpp"
#include "sync/note_status.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <utility>

namespace noteapp::gui {

namespace {

// Editör EditorBuffer'dır (ImGui metin kutusu değil): uzunluğun int tavanı yok, sınır yalnız max_note_bytes.
app::DocumentOptions document_options(const core::Config& config) {
    return {.max_bytes = config.tuning.max_note_bytes.cap(), .lf_editor = true};
}
const char kStateFileName[] = "gui-state.toml";
constexpr const char* kViewTree = "tree";
constexpr const char* kViewDetails = "details";
constexpr const char* kViewMap = "map";
// Elle bozulmuş gui-state.toml pencereyi ekrandan taşırmasın ya da görünmez yapmasın.
constexpr int kMinWindowSide = 200;
constexpr int kMaxWindowSide = 16384;
constexpr double kMinPaneSide = 50.0;
constexpr double kMaxPaneSide = 8192.0;

std::filesystem::path state_file() {
    return core::state_dir() / kStateFileName;
}

// Dosya yok ya da bozuk: varsayılanlar geçerlidir, açılış sürer (not verisi etkilenmez, plan davranış 7).
std::optional<toml::table> read_state_file() {
    std::ifstream in(state_file(), std::ios::binary);  // fs::path: Windows'ta Türkçe yol da açılır
    if (!in)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    toml::parse_result parsed = toml::parse(text);
    if (!parsed)
        return std::nullopt;
    return std::move(parsed).table();
}

std::string_view sort_field_name(search::SortField field) {
    const auto found =
        std::ranges::find(app::kSortFieldNames, field, [](const auto& entry) { return entry.first; });
    return found == app::kSortFieldNames.end() ? std::string_view{} : found->second;
}

std::chrono::seconds rescan_interval(const core::Config& config) {
    return std::chrono::seconds(std::max(1, config.tuning.rescan_seconds));
}

// Metinde sorgu terimlerinin en erken geçişi, bayt ofsetiyle (plan davranış 3: yalnız ilk eşleşmeye gidilir).
// Fold kod noktası başına 1:1 olduğundan katlanmış indeks özgün bayta byte_offsets ile döner.
// Sorgunun argümansız metni: içerik araması ve editör seçimi bunu arar. Bozuk argümanda boş.
std::string search_text(const std::string& query) {
    const auto parsed = search::parse_query(query);
    return parsed ? parsed->text : std::string{};
}

// Etiketler son kayıttaki haliyle (bağlantılarla aynı okuma): "#a #b". Etiket kuralı kontrol ve yön
// karakterini almaz: temizlenmeden gösterilebilir.
std::string hash_tags(const std::vector<std::string>& tags) {
    std::string text;
    for (const std::string& tag : tags)
        text += (text.empty() ? "#" : " #") + tag;
    return text;
}

std::optional<TextSelection> first_match(const std::string& text, const std::string& query) {
    const search::Folded folded = search::fold(text);
    std::optional<TextSelection> best;
    for (const auto& term : search::parse_terms(query)) {
        const auto at = folded.text.find(term);
        if (at == std::u32string::npos)
            continue;
        const std::size_t after = at + term.size();
        const TextSelection found{folded.byte_offsets[at], after < folded.byte_offsets.size()
                                                               ? folded.byte_offsets[after]
                                                               : text.size()};
        if (!best || found.start < best->start)
            best = found;
    }
    return best;
}

}  // namespace

std::string sync_state_text(core::SyncState state) {
    switch (state) {
    case core::SyncState::Synced:
        return core::tr("gui.sync_state_synced");
    case core::SyncState::Modified:
        return core::tr("gui.sync_state_modified");
    case core::SyncState::New:
        return core::tr("gui.sync_state_new");
    case core::SyncState::ConflictCopy:
        return core::tr("gui.sync_state_conflict");
    case core::SyncState::SyncOff:
        break;
    }
    return core::tr("gui.sync_off");
}

GuiState::GuiState(core::Config config)
    : config_(std::move(config)), store_(core::NoteStore::for_config(config_)),
      rescan_at_(std::chrono::steady_clock::now() + rescan_interval(config_)),
      sync_label_(core::tr("gui.sync_off")),
      // İşçi iş parçacığından gelir: durumlar UI iş parçacığına aktarılır (tick).
      sync_status_([this](app::SyncStates states) {
          post([this, states = std::move(states)]() mutable { apply_sync_states(std::move(states)); });
      }),
      settings_(config_, deciders,
                SettingsHost{.flush = [this] { return flush(); },
                             .applied = [this](const core::Config& before,
                                               const core::Config& after) { apply_settings(before, after); },
                             .synced = [this] { rescan(); },
                             .post = [this](std::function<void()> work) { post(std::move(work)); }}) {
    // cppcheck-suppress useInitializationList ; tek seferlik kurulum; üye sırasına bağlanmasın
    sync_note_exe = sync::note_executable_beside_self();
    word_tables_.set_store(store_);
    const search::ViewLookup views = [this](std::string_view name) { return core::view_notes(store_, name); };
    model_.set_view_lookup(views);
    model_.listing().set_fuzzy_candidate_chars(config_.limits.search.fuzzy_candidate_chars.cap());
    model_.listing().set_content_searcher([this](std::string_view text) {
        return content_index().search(text, config_.limits.search.content_results.cap());
    });
    model_.listing().set_global_searcher(
        [this, views](std::string_view query, std::span<const core::NoteMeta> notes) {
            return search::search_all(query, notes, content_index(), config_.limits.search,
                                      std::chrono::system_clock::now(), views);
        });
    load_list_prefs();  // ilk seçim tercih edilen sıranın ilk satırı olsun
    rescan();
    load_gui_state();
    start_sync();
    // Açılış ipucu (TUI ile aynı): yalnız sayı, UI iş parçacığı dışında. Önceki ileti (config uyarısı) kalır,
    // TUI'de de ileti ipucunun önündedir.
    stray_probe_ = std::jthread([this, config = config_](const std::stop_token& stop) {
        if (const std::size_t count = app::count_stray_files(config, stop); count > 0)
            post([this, count] {
                if (status_.empty())
                    status_ = core::tr("gui.stray_notice", {{"count", std::to_string(count)}});
            });
    });
}

GuiState::~GuiState() = default;

// TUI'nin ListPrefs'iyle aynı anahtarlar (`sort`, `sort_descending`, `open_folders`); bilinmeyen değer
// varsayılanda bırakılır.
void GuiState::load_list_prefs() {
    const std::optional<toml::table> table = read_state_file();
    if (!table)
        return;
    app::Listing& listing = model_.listing();
    search::SortSpec sort = listing.sort();
    if (const auto name = (*table)["sort"].value<std::string>()) {
        const auto found =
            std::ranges::find(app::kSortFieldNames, *name, [](const auto& entry) { return entry.second; });
        if (found != app::kSortFieldNames.end())
            sort.field = found->first;
    }
    sort.descending = (*table)["sort_descending"].value_or(sort.descending);
    listing.set_sort(sort);
    if (const toml::array* open = (*table)["open_folders"].as_array()) {
        std::set<std::string> folders;
        for (const toml::node& node : *open)
            if (auto folder = node.value<std::string>())
                folders.insert(std::move(*folder));
        listing.set_open_folders(std::move(folders));
    }
}

void GuiState::load_gui_state() {
    const std::optional<toml::table> saved = read_state_file();
    if (!saved)
        return;
    const toml::table& table = *saved;

    const auto window_side = [&table](const char* key, int fallback) {
        const auto value = table[key].value<std::int64_t>();
        return value && *value >= kMinWindowSide && *value <= kMaxWindowSide ? static_cast<int>(*value)
                                                                             : fallback;
    };
    const auto pane_side = [&table](const char* key, float fallback) {
        const auto value = table[key].value<double>();
        return value && *value >= kMinPaneSide && *value <= kMaxPaneSide ? static_cast<float>(*value)
                                                                         : fallback;
    };
    layout_.width = window_side("width", layout_.width);
    layout_.height = window_side("height", layout_.height);
    layout_.left_pane_width = pane_side("left_pane_width", layout_.left_pane_width);
    layout_.map_pane_width = pane_side("map_pane_width", layout_.map_pane_width);
    // Eski dosyadaki folder_pane_height (ayrı klasör bölmesi) okunmaz; bilinmeyen görünüm varsayılana düşer.
    const auto view_mode = table["view_mode"].value<std::string>();
    if (view_mode == kViewDetails)
        layout_.browser_view = BrowserView::Details;
    else if (view_mode == kViewMap)
        layout_.browser_view = BrowserView::Map;
    layout_.row_numbers = table["row_numbers"].value_or(layout_.row_numbers);
    // Ayrıntılar sütunu ve son komutlar TUI'nin adlarıyla; bilinmeyen değer varsayılanda kalır.
    if (const auto column = table["column"].value<std::string>()) {
        const auto found =
            std::ranges::find(kDetailsColumnNames, *column, [](const auto& entry) { return entry.second; });
        if (found != std::end(kDetailsColumnNames))
            layout_.column = found->first;
    }
    if (const toml::array* recent = table["recent_commands"].as_array()) {
        for (const toml::node& node : *recent)
            if (auto id = node.value<std::string>(); id && app::find_command(*id) != nullptr)
                recent_commands_.push_back(std::move(*id));
        if (recent_commands_.size() > config_.limits.app.recent_commands.cap())
            recent_commands_.resize(config_.limits.app.recent_commands.cap());
    }
    // TUI'nin `search_history` anahtarı ve süzgeci: dosya elle değişmiş olabilir (sır, bozuk kayıt düşer).
    if (const toml::array* history = table["search_history"].as_array()) {
        std::vector<std::string> entries;
        for (const toml::node& node : *history)
            if (auto entry = node.value<std::string>())
                entries.push_back(std::move(*entry));
        search_history_ = tui::kept_search_history(std::move(entries), search_history_limits());
    }

    // Açık sekmeler (SPEC-gui "Sekmeler"); yoksa ya da hiçbiri kalmadıysa son seçili not.
    if (const toml::array* saved = table["tabs"].as_array()) {
        app::SavedTabs entries;
        for (const toml::node& node : *saved) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;
            if (auto path = (*entry)["path"].value<std::string>())
                entries.tabs.push_back(
                    {std::move(*path), (*entry)["pinned"].value_or(false), (*entry)["kept"].value_or(false)});
        }
        const auto active = table["active_tab"].value_or(std::int64_t{0});
        entries.active = active < 0 ? 0 : static_cast<std::size_t>(active);
        if (restore_tabs(entries))
            return;
    }
    // Son seçili not (SPEC-gui "Diğer"): listede hâlâ varsa seçilir. Değer dosyadan gelir, NotePath doğrular.
    if (const auto last = table["last_note"].value<std::string>()) {
        if (const auto path = core::NotePath::parse(*last)) {
            if (const int row = model_.index_of(*path); row >= 0)
                select_row(row);
        }
    }
}

void GuiState::save_gui_state() const {
    toml::table table;
    table.insert("width", layout_.width);
    table.insert("height", layout_.height);
    table.insert("left_pane_width", static_cast<double>(layout_.left_pane_width));
    table.insert("map_pane_width", static_cast<double>(layout_.map_pane_width));
    const auto view_mode = [](BrowserView view) {
        switch (view) {
        case BrowserView::Details:
            return kViewDetails;
        case BrowserView::Map:
            return kViewMap;
        case BrowserView::Tree:
            break;
        }
        return kViewTree;
    };
    table.insert("view_mode", view_mode(layout_.browser_view));
    table.insert("row_numbers", layout_.row_numbers);
    const auto column =
        std::ranges::find(kDetailsColumnNames, layout_.column, [](const auto& entry) { return entry.first; });
    table.insert("column", column->second);
    toml::array recent;
    for (const std::string& id : recent_commands_)
        recent.push_back(id);
    table.insert("recent_commands", std::move(recent));
    toml::array history;
    for (const std::string& entry : search_history_)
        history.push_back(entry);
    table.insert("search_history", std::move(history));
    const app::Listing& listing = model_.listing();
    table.insert("sort", sort_field_name(listing.sort().field));
    table.insert("sort_descending", listing.sort().descending);
    toml::array open_folders;
    for (const std::string& folder : listing.open_folders())
        if (listing.tree().find(folder) != nullptr)  // gitmiş klasör taşınmaz (TUI prune_open_folders)
            open_folders.push_back(folder);
    table.insert("open_folders", std::move(open_folders));
    if (const auto path = current_path())
        table.insert("last_note", path->string());
    // Taslakların yolu yoktur, yazılmaz.
    toml::array saved_tabs;
    std::int64_t active_index = 0;
    for (const NoteTab& tab : tabs_) {
        const auto note = tab_note(tab);
        if (!note)
            continue;
        if (tab.id == active_tab_)
            active_index = static_cast<std::int64_t>(saved_tabs.size());
        saved_tabs.push_back(
            toml::table{{"path", note->string()}, {"pinned", tab.pinned}, {"kept", tab.kept}});
    }
    table.insert("tabs", std::move(saved_tabs));
    table.insert("active_tab", active_index);
    std::ostringstream out;
    out << table;
    std::error_code ignored;  // yazma aşağıda aynı nedenle başarısız olur ve loglanır
    std::filesystem::create_directories(core::state_dir(), ignored);
    if (const auto written = core::write_file_atomically(state_file(), out.str()); !written)
        core::log_warn("event=gui_state_write_failed detail=" + core::format_error(written.error()));
}

void GuiState::post(std::function<void()> work) {
    const std::scoped_lock lock(posted_mutex_);
    posted_.push_back(std::move(work));
    if (wake_)
        wake_();
}

void GuiState::set_wake(std::function<void()> wake) {
    const std::scoped_lock lock(posted_mutex_);
    wake_ = std::move(wake);
}

bool GuiState::run_posted() {
    std::vector<std::function<void()>> work;
    {
        const std::scoped_lock lock(posted_mutex_);
        work.swap(posted_);
    }
    // Kilit dışında: iş diyalog açabilir, işçi o sırada post edebilmeli.
    for (const auto& item : work)
        item();
    return !work.empty();
}

bool GuiState::tick(std::chrono::steady_clock::time_point now) {
    bool worked = run_posted();
    if (autosave_.due(now)) {
        autosave_.cancel();
        if (dirty_ && shown_ && !new_note_ && doc_.autosaves())
            autosave();
        worked = true;
    }
    if (now >= rescan_at_) {
        // SDL'de dosya izleyici yok (plan T40): dış değişiklik bu aralıkla ve pencere odağına dönüşte
        // görülür. Tarama anlık görüntü karşılaştırdığından arada kaç olay olduğu önemsizdir, hiçbiri
        // kaybolmaz.
        rescan_at_ = now + rescan_interval(config_);
        rescan();
        worked = true;
    }
    return worked;
}

std::optional<core::NotePath> GuiState::current_path() const {
    if (current_row_ < 0 || current_row_ >= model_.row_count())
        return std::nullopt;
    return model_.note_at(current_row_).path;
}

void GuiState::select_row(int row) {
    if (row == current_row_ || row < 0 || row >= model_.row_count())
        return;
    selected_folder_.reset();
    current_row_ = row;
    show_current();
}

void GuiState::move_selection(int delta) {
    if (trash_view_) {
        const int last = static_cast<int>(trash_items_.size()) - 1;
        if (last >= 0)
            trash_row_ = std::clamp(trash_row_ + delta, 0, last);
        return;
    }
    const int count = model_.row_count();
    if (count == 0)
        return;
    const int row = current_row_ >= 0 ? current_row_ : (delta > 0 ? -1 : count);
    select_row(std::clamp(row + delta, 0, count - 1));
}

void GuiState::query_edited() {
    close_trash();        // süzgeç canlı listeye aittir: yazılanın etkisi görünsün
    if (!query_.empty())  // sonuç seçilsin: Enter seçili klasörde yeni not açmasın, bulunanı açsın
        selected_folder_.reset();
    apply_filter([this] {
        model_.set_mode(search_mode_);
        model_.set_query(query_);
    });
}

void GuiState::set_search_mode(app::SearchMode mode) {
    if (mode != app::SearchMode::Name && content_index_)
        content_index_->refresh();  // ad modundayken rescan indeksi tazelemedi
    search_mode_ = mode;
    query_edited();
}

void GuiState::toggle_search_mode() {
    set_search_mode(search_mode_ == app::SearchMode::Name ? app::SearchMode::Content : app::SearchMode::Name);
}

void GuiState::search_global() {
    set_search_mode(app::SearchMode::Global);
    focus_search();
}

void GuiState::open_picker(Picker kind) {
    picker_items_.clear();
    if (kind == Picker::Tag) {
        for (core::TagCount& tag : store_.tags())
            picker_items_.push_back(
                {std::move(tag.name), "#" + tag.display + "  (" + std::to_string(tag.count) + ")"});
    } else if (kind == Picker::View) {
        for (std::string& name : app::view_names(store_))
            picker_items_.push_back({name, name});
    }
    picker_ = kind;
}

// TUI'deki `tag:`/`view:` tamamlamasının sonucu: ad araması, değer argüman olarak.
void GuiState::pick(const std::string& value) {
    if (picker_ == Picker::None)
        return;
    std::string query = picker_ == Picker::Tag ? "tag:" : "view:";
    // Boşluklu değer tek sözcük kalsın; anahtar tırnağın dışında olmalı, yoksa argüman sayılmaz.
    query += value.find(' ') == std::string::npos ? value : '"' + value + '"';
    close_picker();
    search_mode_ = app::SearchMode::Name;
    set_query(std::move(query));
    focus_search();
}

void GuiState::close_picker() {
    picker_ = Picker::None;
    picker_items_.clear();
}

void GuiState::choose_sort(search::SortField field) {
    set_sort(app::sort_for(field));
}

void GuiState::reverse_sort() {
    set_sort({sort().field, !sort().descending});
}

// TUI gibi seçimde yazılır: kapanış beklenmez.
void GuiState::set_sort(search::SortSpec sort) {
    model_.listing().set_sort(sort);
    rows_stale_ = true;
    apply_filter([this] { model_.refresh(); });
    save_gui_state();
}

void GuiState::request_sync_states() {
    if (config_.sync.enabled)
        sync_status_.request(config_, model_.listing().metas());
}

void GuiState::apply_sync_states(app::SyncStates states) {
    model_.listing().set_sync_states(std::move(states));
    rows_stale_ = true;                          // Ayrıntılar'ın senkron sütunu
    apply_filter([this] { model_.refresh(); });  // `sync:` süzgeci yeni durumları görsün
}

const search::ContentIndex& GuiState::content_index() {
    if (!content_index_) {
        // ponytail: ilk içerik aramasında tüm notlar bir kez okunur (UI'yi kısa süre tutar); sorun olursa
        // arka plan iş parçacığına alınır.
        content_index_ = std::make_unique<search::ContentIndex>(store_, config_.tuning.max_note_bytes.cap(),
                                                                config_.limits.search.content_hits.cap());
        content_index_->refresh();
    }
    return *content_index_;
}

// Gösterilen notta ilk eşleşmeyi seçtirir. Aynı (not, sorgu) için yeniden çalışmaz: yedek tarama kullanıcının
// imlecini geri atmasın.
void GuiState::update_editor_selection() {
    if (search_mode_ == app::SearchMode::Name || query_.empty() || !shown_ || new_note_ || binary()) {
        if (!selection_key_.empty()) {
            selection_key_.clear();
            editor_.clear_selection();
        }
        return;
    }
    std::string key = shown_->path.string() + '\n' + query_;
    if (key == selection_key_)
        return;
    selection_key_ = std::move(key);
    if (const auto match = first_match(editor_.text(), search_text(query_)))
        editor_.select({match->start, match->end});
}

void GuiState::set_query(std::string query) {
    query_ = std::move(query);
    ++query_generation_;
    query_edited();
}

void GuiState::clear_query() {
    if (!query_.empty())
        set_query({});
}

void GuiState::search_enter() {
    // nvALT: eşleşme yoksa Enter, sorguyu ilk satır yaparak yeni not açar. Argümanlı ya da bozuk sorgu not
    // metni değildir (TUI kuralı).
    if (current_path() || new_note_) {
        if (current_path() && !query_error())  // TUI: bulunan, bozuk olmayan arama geçmişe girer
            remember_query();
        request_focus(FocusRequest::Editor);
        return;
    }
    if (search_mode_ == app::SearchMode::Global)  // TUI: genel aramada eşleşmesiz Enter not açmaz
        return;
    if (const auto parsed = search::parse_query(query_); !parsed || !parsed->filters.empty() || parsed->sort)
        return;
    new_note(query_);
}

// ---- Arama satırı ----

const tui::SharedInputSources& GuiState::input_sources() {
    if (!input_sources_)
        input_sources_ = tui::build_input_sources(model_.listing().metas(), app::view_names(store_));
    return input_sources_;
}

tui::SearchHistoryLimits GuiState::search_history_limits() const {
    return {.entries = config_.limits.tui.search_history.cap(),
            .entry_bytes = config_.limits.tui.search_entry_bytes.cap()};
}

void GuiState::sync_search_line(std::string_view text, std::size_t cursor) {
    search_line_.set_sources(input_sources());
    if (search_line_.text() != text) {
        search_line_.set_text(std::string(text), cursor);  // yazıldı: liste ve geçmiş yürüyüşü biter
        search_walk_.reset();
    } else {
        search_line_.set_cursor(cursor);
    }
}

bool GuiState::search_history_key(bool older) {
    if (!older && !search_walk_)
        return false;
    if (!search_walk_)
        search_walk_ = search_line_.text();
    search_key(older ? tui::Key::CtrlP : tui::Key::CtrlN);
    if (!older && search_line_.text() == *search_walk_)
        search_walk_.reset();
    return true;
}

bool GuiState::search_key(tui::Key key, std::string_view typed) {
    search_line_.set_sources(input_sources());
    const tui::InputLine::Result result = search_line_.handle(key, typed, search_history_);
    if (result == tui::InputLine::Result::Edited)
        search_edit_ = SearchEdit{search_line_.text(), search_line_.cursor()};
    return result != tui::InputLine::Result::Ignored;
}

// Kutu odakta değildir (fare listeye tıkladı): metin sorguya doğrudan yazılır, odak kutuya döner.
void GuiState::pick_search_completion(std::size_t index) {
    const tui::InputLine::Popup* popup = search_popup();
    if (popup == nullptr || index >= popup->items.size())
        return;
    const std::size_t from = popup->selected;
    for (std::size_t step = from; step < index; ++step)
        search_key(tui::Key::Down);
    for (std::size_t step = index; step < from; ++step)
        search_key(tui::Key::Up);
    search_key(tui::Key::Enter);
    if (auto edit = take_search_edit())
        set_query(std::move(edit->text));
    focus_search();
}

void GuiState::close_search_popup() {
    if (search_popup() != nullptr)
        search_key(tui::Key::Escape);
}

void GuiState::remember_query() {
    if (tui::remember_search(search_history_, query_, input_sources().get(), search_history_limits()))
        save_gui_state();
}

void GuiState::set_folder(std::string folder) {
    close_trash();
    selected_folder_.reset();
    apply_filter([&] {
        current_folder_ = std::move(folder);
        model_.set_folder(current_folder_);
    });
}

void GuiState::rescan() {
    if (content_index_ && search_mode_ != app::SearchMode::Name)
        content_index_->refresh();
    if (auto folders = store_.folders(); folders != folders_) {
        folders_ = std::move(folders);
        model_.listing().set_folders(folders_);
        rows_stale_ = true;
        const auto exists = [this](const std::string& folder) {
            return folder.empty() || std::ranges::find(folders_, folder) != folders_.end();
        };
        // Kapsam klasörü silindiyse köke düşer; seçili klasör satırı duran en yakın atasına.
        if (!exists(current_folder_))
            current_folder_.clear();
        while (selected_folder_ && !exists(*selected_folder_))
            selected_folder_ = core::parent_folder(*selected_folder_);
    }
    reload_trash();
    auto notes = store_.list();
    std::unordered_set<std::string> present;
    present.reserve(notes.size());
    for (const core::NoteInfo& info : notes)
        present.insert(info.path.string());
    // Önce model, sonra sekmeler: kapanan sekmenin yerine öne gelen komşu güncel listeyle çözülür (eski
    // listede örneğin yeniden adlandırılan klasördeki yeni yolu yoktur). Seçim yola göre korunur, satır
    // sırası kaymış olabilir; süzgeç kuralı (apply_filter) sekmeler kapandıktan sonra uygulanır.
    const auto selected = current_path();
    model_.set_folder(current_folder_);
    if (model_.set_notes(std::move(notes))) {
        word_tables_.request();
        language_servers_.notes_changed();
        rows_stale_ = true;
        map_.edges_changed();
        input_sources_.reset();  // tamamlama adayları (adlar, etiketler) sonraki istekte
        request_sync_states();
    }
    if (search_mode_ != app::SearchMode::Name && !search_text(query_).empty())
        model_.refresh();  // içerik indeksi tazelendi
    current_row_ = selected ? model_.index_of(*selected) : -1;
    close_missing_tabs(present);
    apply_filter([] {});
    // Yalnız ilişki değiştiren başka bağlantı (CLI, senkron) listeyi değiştirmez; harita kenarları yine
    // tazelenir.
    if (const std::uint64_t version = store_.data_version(); version != data_version_) {
        data_version_ = version;
        map_.edges_changed();
    }
    reload_links();  // kayıt ya da senkron başka notun bağlantısını değiştirmiş olabilir
}

void GuiState::apply_filter(const std::function<void()>& change) {
    // Editördeki not esastır (seçim, başarısız bir kayıttan sonra ondan ayrışmış olabilir).
    const auto previous = shown_ ? std::optional<core::NotePath>(shown_->path) : current_path();
    change();
    // TUI ile aynı: önceki not hâlâ listedeyse seçili kalır, yoksa ilk satır. Yeni not yazılırken seçim boş
    // kalır: ilk satırı seçmek taslağı ezerdi. Kullanıcı son sekmeyi kapattıysa da boş kalır.
    int target = previous ? model_.index_of(*previous) : -1;
    if (target < 0 && model_.row_count() > 0 && !new_note_ && !selected_folder_ && !tabs_closed_)
        target = 0;
    current_row_ = target;
    show_current();
}

void GuiState::show_current() {
    if (in_show_)  // kayıt akışı listeyi tazelerken seçim editörü ezmesin
        return;
    in_show_ = true;
    const struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{in_show_};
    show_selected();
    update_editor_selection();
}

// Başka nota/boş seçime geçmeden önce tampon kaydedilir; olmazsa geçiş iptal (false). Kayıt listeyi
// değiştirmiş olabilir (yeni not, kopya): hedef yeniden çözülür.
bool GuiState::leave_shown_note(const std::optional<core::NotePath>& target) {
    if (!unsaved())
        return true;
    if (!flush()) {
        restore_selection();
        return false;
    }
    current_row_ = target ? model_.index_of(*target) : -1;
    return true;
}

void GuiState::show_selected() {
    const auto target = current_path();
    if (!(shown_ && target && shown_->path == *target)) {
        if (new_note_ && !target)  // yeni not yazılırken boş seçim taslağı silmez
            return;
        if (target) {
            // Başka sekmede açık not yinelenmez, o sekme öne gelir.
            if (const auto open = tab_of(*target)) {
                if (!switch_tab(*open, false))
                    restore_selection();
                return;
            }
        }
        if (target && keeps_active_tab()) {
            if (!push_tab()) {
                restore_selection();
                return;
            }
            current_row_ = model_.index_of(*target);  // ayrılırken kayıt listeyi tazelemiş olabilir
        } else if (!leave_shown_note(target)) {
            return;
        }
    }
    if (current_path())
        show_note(model_.note_at(current_row_));
    else
        park_or_clear();
}

void GuiState::clear_editor() {
    shown_.reset();
    set_editor({}, false);
    reload_links();
}

void GuiState::reload_links() {
    links_ = {};
    if (shown_) {
        if (auto links = store_.links(shown_->path))
            links_ = std::move(*links);
        else if (links.error().code != core::Error::Code::NotFound)
            core::log_warn("note links: " + core::format_error(links.error()));
    }
    link_lines_ = core::link_lines(links_);
    reload_info();  // etiketler bağlantı okumasından gelir
}

// TUI'nin ayrıntısıyla aynı alanlar ve aynı "bilinmiyor": eski depodan gelen notta created ya da origin
// olmayabilir.
void GuiState::reload_info() {
    note_info_.clear();
    editor_.set_type(current_type());  // kayıt ya da taşıma tipi değiştirmiş olabilir (bul önerileri)
    if (!shown_)
        return;
    // Kendi kaydımız shown_'ın boyutunu ve zamanını tazelemez (yalnız revizyon); listedeki kayıt taramadan gelir.
    const int row = model_.index_of(shown_->path);
    core::NoteMeta meta = core::meta_of(row >= 0 ? model_.note_at(row) : *shown_);
    const std::string unknown = core::tr("gui.info_unknown");
    const std::string none = core::tr("gui.info_none");
    // ponytail: senkron açıkken tek not için depo yeniden açılır (gösterim ve tarama başına); yavaşlarsa TUI'deki
    // gibi işçi iş parçacığına alınır.
    std::string sync = unknown;
    if (const auto filled = sync::fill_sync_status(config_, std::span(&meta, 1)); filled)
        sync = sync_state_text(meta.sync_status);
    else
        core::log_warn("event=note_sync_status_failed detail=" + core::format_error(filled.error()));
    const std::string type = current_type();
    const std::string tags = hash_tags(links_.tags);
    note_info_ = {
        {core::tr("gui.info_size"), core::human_size(meta.size)},
        {core::tr("gui.info_updated"), core::local_time_text(meta.updated_at)},
        {core::tr("gui.info_created"), meta.created_at ? core::local_time_text(*meta.created_at) : unknown},
        {core::tr("gui.info_sync"), sync},
        {core::tr("gui.info_origin"), meta.origin.value_or(unknown)},
        {core::tr("gui.info_type"), type.empty() ? none : type},
        {core::tr("gui.info_tags"), tags.empty() ? none : tags},
    };
}

app::PreviewKind GuiState::preview_kind() const {
    return shown_ ? app::preview_kind_for(current_type()) : app::PreviewKind::None;
}

// Düzenlenemeyen metin (geçersiz UTF-8, NUL) ayrıştırılmaz: md4c ve tablo görünümü geçerli UTF-8 bekler.
bool GuiState::can_preview() const {
    return previewable_type() && !binary() && editable_ && view_limits().view_bytes.allows(editor_.text().size());
}

void GuiState::toggle_preview() {
    if (!can_preview())
        return;
    preview_ = !preview_;
    if (!preview_)
        request_focus(FocusRequest::Editor);
}

bool GuiState::resolves_link(std::string_view target) const {
    return store_.resolve_link(target).has_value();
}

void GuiState::open_wiki_link(const std::string& target, bool new_tab) {
    if (!flush_shown())
        return;
    const auto path = store_.resolve_link(target);
    if (!path) {
        fail(core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", target}}});
        return;
    }
    if (new_tab)
        open_in_new_tab(*path);
    else
        open_link(*path);
}

void GuiState::open_link(const core::NotePath& path) {
    close_trash();
    ensure_visible(path);
    const int row = model_.index_of(path);
    if (row < 0) {  // liste eski: not bu arada gitmiş
        report(core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", path.string()}}});
        return;
    }
    select_row(row);
}

// Kopya şart: yükleme shown_'ı yeniden atar, info onu gösterebilir.
// NOLINTNEXTLINE(performance-unnecessary-value-param) -- üstteki neden
void GuiState::show_note(core::NoteInfo info) {  // cppcheck-suppress passedByValue ; üstteki neden
    // Aynı not, aynı revizyon: editör içeriğine/kaydırmasına dokunma (rescan sonrası kararlılık). Kirli
    // tampon varken dışarıdaki değişiklik tamponu ezmez (TUI ile ortak kural, app::disk_change); kayıt anında
    // çakışma diyaloğu çıkar.
    if (shown_ && shown_->path == info.path) {
        const app::DiskChange change = app::disk_change(info.revision, doc_.revision(), dirty_);
        if (change == app::DiskChange::Changed && !doc_.changed_on_disk())
            status_ = core::tr("gui.changed_on_disk");
        if (change != app::DiskChange::Reload) {
            doc_.note_disk_change(change);
            return;
        }
    }
    claim_active_tab();
    // Aynı notun diskten yenilenmesi imleci aynı satır/sütunda tutar (TUI EditorBuffer::reload ile aynı).
    const std::optional<tui::TextPosition> kept =
        shown_ && shown_->path == info.path ? std::optional(editor_.buffer().position()) : std::nullopt;
    shown_ = info;
    if (const auto loaded = load_text(info); !loaded) {
        doc_.reset_draft();  // okunamayan notun belgesi yok: hiçbir yol yazmaz
        set_editor(core::tr("gui.read_failed", {{"detail", core::format_error(loaded.error())}}), false);
    } else if (kept) {
        editor_.move_to(*kept);
    }
    reload_links();
}

// Editörü doldurur; dirty/autosave durumunu sıfırlar.
void GuiState::set_editor(std::string content, bool editable) {
    autosave_.cancel();
    selection_key_.clear();
    hidden_controls_ = core::count_hidden_controls(content);
    std::string type = current_type();
    if (new_note_ && !shown_) {  // taslak: tip içerikten (gösterilen notun tipi uzantısıdır)
        type = detected_type(content);
        draft_type_.reset();
        draft_size_ = content.size();
    }
    editor_ = NoteEditor(std::move(content), editable, std::move(type), editor_limits());
    buffer_generation_ = ++editor_generations_;
    editable_ = editable;
    dirty_ = false;
}

// Salt-okunur kuralı TUI ile ortaktır (app::classify): ikili, NUL, geçersiz UTF-8 ve max_note_bytes üstü.
// Baştan sona CRLF olan not LF gösterilir, kayıtta CRLF yazılır (app::Document lf_editor); karışık satır sonu
// EditorBuffer'da olduğu gibi kalır.
std::expected<void, core::Error> GuiState::load_text(const core::NoteInfo& info) {
    app::Document doc(document_options(config_));  // ayar değişmiş olabilir: her açılış güncel sınırla
    auto text = doc.open(store_, info.path);
    if (!text)
        return std::unexpected(std::move(text.error()));
    doc_ = std::move(doc);
    shown_ = info;
    set_document_text(std::move(*text));
    return {};
}

// İkili notta yer tutucu; diğer salt-okunur notlar arındırılmış metinle (başıyla) okunur.
void GuiState::set_document_text(std::string text) {
    if (binary())
        set_editor(core::tr("gui.binary_preview"), false);
    else
        set_editor(std::move(text), doc_.writable());
}

void GuiState::restore_selection() {
    current_row_ = shown_ ? model_.index_of(shown_->path) : -1;
}

void GuiState::report(const core::Error& error) {
    status_ = core::tr("gui.save_failed", {{"detail", core::format_error(error)}});
}

void GuiState::fail(const core::Error& error) {
    status_ = core::tr("gui.op_failed", {{"detail", core::format_error(error)}});
}

std::string GuiState::header() const {
    if (new_note_)
        return core::tr("gui.untitled") + "  ·  " + core::tr("gui.state_unsaved");
    if (!shown_)
        return {};
    std::string text =
        shown_->path.string() + "  ·  " + core::tr(dirty_ ? "gui.state_modified" : "gui.state_saved");
    if (const std::string reason = doc_.read_only_reason(); !reason.empty())
        text += "  ·  " + reason;
    if (!links_.tags.empty())
        text += "  ·  " + hash_tags(links_.tags);
    return text;
}

std::string GuiState::hidden_controls_label() const {
    if (hidden_controls_ == 0)
        return {};
    if (hidden_controls_ == 1)
        return core::tr("gui.hidden_controls_one");
    return core::tr("gui.hidden_controls", {{"count", std::to_string(hidden_controls_)}});
}

std::string GuiState::current_type() const {
    if (new_note_ && !shown_)
        return editor_.type();
    return shown_ ? core::split_note_path(shown_->path.string()).type : std::string{};
}

std::string GuiState::detected_type(std::string_view content) const {
    return detect::suggest(
               content, {config_.default_type, config_.tuning.min_confidence, config_.tuning.max_scan_bytes})
        .type;
}

void GuiState::refresh_draft_type() {
    const std::size_t size = editor_.text().size();
    if (draft_type_.edited(std::exchange(draft_size_, size), size))
        editor_.set_type(detected_type(editor_.text()));
}

std::vector<std::string> GuiState::type_choices() const {
    std::vector<std::string> choices = config_.types;
    if (const std::string type = current_type(); std::ranges::find(choices, type) == choices.end())
        choices.push_back(type);
    return choices;
}

std::optional<TextSelection> GuiState::editor_selection() const {
    const auto range = editor_.buffer().selection();
    return range ? std::optional(TextSelection{range->begin, range->end}) : std::nullopt;
}

std::optional<TextSelection> GuiState::take_editor_selection() {
    auto selection = editor_selection();
    editor_.clear_selection();
    return selection;
}

FocusRequest GuiState::take_focus_request() {
    return std::exchange(focus_request_, FocusRequest::None);
}

// Metin değiştiyse (edit_count) kirli durum TUI kuralıyla: kayıtlı metinden farklıysa kirli, geri alıp aynı
// metne dönmek temizdir. Otomatik kayıt her düzenlemede yeniden başlar, temize dönen tampon beklemez.
void GuiState::editor_changed(std::uint64_t edits_before) {
    if (editor_.buffer().edit_count() == edits_before)
        return;
    hidden_controls_ = core::count_hidden_controls(editor_.text());
    if (!editable_ || !(shown_ || new_note_))
        return;
    dirty_ = editor_.dirty();
    if (NoteTab* tab = find_tab(active_tab_))
        tab->kept = true;  // düzenlenen sekme, sonra açılan notla değişmez
    if (new_note_ && !shown_)
        refresh_draft_type();
    // Yeni not ilk kayıtta diyalog ister; dışarıda değişen ya da silinen not açık kaydı bekler.
    if (new_note_ || !doc_.autosaves())
        return;
    if (dirty_)
        autosave_.edited(app::Autosave::Clock::now());  // ardışık düzenlemeler tek yazmaya birleşir
    else
        autosave_.cancel();
}

void GuiState::edit_text(std::string_view text) {
    const std::uint64_t edits = editor_.buffer().edit_count();
    editor_.replace_all(text);
    editor_changed(edits);
}

void GuiState::editor_key(tui::Key key, std::string_view text) {
    editor_notice_.clear();
    const std::uint64_t edits = editor_.buffer().edit_count();
    const std::shared_ptr<const tui::WordTable> words = word_table();
    if (editor_.mode() == NoteEditor::Mode::Editing) {
        switch (key) {
        case tui::Key::CtrlC:
            copy_from_editor(editor_.copy());
            return;
        case tui::Key::CtrlX:
            if (!editor_.writable())  // TUI: salt-okunurda kesme yok, kopya Ctrl+C'dir
                return;
            copy_from_editor(editor_.cut());
            editor_changed(edits);
            return;
        case tui::Key::CtrlV:
            paste_into_editor();
            editor_changed(edits);
            return;
        default:
            break;
        }
    }
    editor_.handle(key, text, words.get());
    editor_changed(edits);
}

namespace {

// GUI editörünün dil sunucusu yüzü: yol yalnız kayıtlı ve gösterilen notta, kuşak GuiState'in tampon kuşağı.
class GuiLspEditor final : public tui::LspEditor {
public:
    GuiLspEditor(NoteEditor& editor, std::optional<core::NotePath> path, bool writable, std::uint64_t generation)
        : editor_(editor), path_(std::move(path)), writable_(writable), generation_(generation) {}
    const tui::EditorBuffer* buffer() const override { return &editor_.buffer(); }
    const std::optional<core::NotePath>& path() const override { return path_; }
    bool writable() const override { return writable_; }
    std::uint64_t buffer_generation() const override { return generation_; }
    bool completing() const override { return editor_.mode() == NoteEditor::Mode::Complete; }
    std::uint64_t completion_generation() const override { return editor_.completion_generation(); }
    void add_completion_items(std::uint64_t generation,
                              std::span<const core::lsp::CompletionItem> items) override {
        editor_.add_completion_items(generation, items);
    }
    void set_diagnostics(std::vector<tui::DiagnosticMark> marks) override {
        editor_.set_diagnostics(std::move(marks));
    }

private:
    NoteEditor& editor_;
    std::optional<core::NotePath> path_;
    bool writable_;
    std::uint64_t generation_;
};

}  // namespace

bool GuiState::update_language_servers(std::chrono::steady_clock::time_point now) {
    // Taslak, klasör özeti (park) ve salt-okunur not sunucuya gitmez.
    std::optional<core::NotePath> path =
        shown_ && !new_note_ && !parked_ ? std::optional(shown_->path) : std::nullopt;
    GuiLspEditor editor(editor_, std::move(path), editable_ && doc_.writable(), buffer_generation_);
    const tui::LanguageServers::Update update = language_servers_.update(editor, now);
    if (!update.status.empty())
        editor_notice_ = update.status;
    return update.wants_frame;
}

std::shared_ptr<const tui::WordTable> GuiState::word_table() {
    if (!std::exchange(word_tables_enabled_, true))
        word_tables_.enable();  // düzenlemeyen kullanıcı bedel ödemez (TUI ile aynı)
    return word_tables_.latest();
}

std::vector<tui::ByteRange> GuiState::misspelled(std::string_view line) {
    if (!tui::spellcheck_applies_to(current_type()))
        return {};
    // Dil Ayarlar'dan değişebilir: sözlük değişince denetleyici yeniden kurulur (TUI ayar uygulaması gibi).
    if (std::string dictionary = tui::dictionary_for_language(config_.language);
        !spell_ || dictionary != spell_dictionary_) {
        spell_dictionary_ = dictionary;
        spell_ = std::make_unique<tui::SpellChecker>(
            std::move(dictionary), [this] { post([] {}); }, tui::SpellChecker::kDefaultIdle,
            tui::SpellChecker::kDefaultTimeout, config_.limits.tui.spellcheck_line_bytes.cap());
    }
    return spell_->misspelled(line);
}

void GuiState::copy_from_editor(std::string text) {
    if (text.empty())
        return;
    clipboard_ = std::move(text);
    deciders.copy_text(clipboard_);
}

// Sistem panosu önce (başka uygulamadan gelen metin); okunamazsa iç pano. Sınır TUI ile aynı anahtar:
// limits.tui.paste_bytes.
void GuiState::paste_into_editor() {
    if (!editor_.writable())
        return;
    std::string text = deciders.paste_text().value_or(clipboard_);
    if (!config_.limits.tui.paste_bytes.allows(text.size())) {
        status_ = core::tr("tui.paste_too_large", {{"limit", core::human_size(config_.limits.tui.paste_bytes.cap())}});
        return;
    }
    editor_.paste(text);
}

void GuiState::find_in_note() {
    if (!(shown_ || new_note_))
        return;
    editor_.open_find(word_table().get());
    request_focus(FocusRequest::Editor);
}

void GuiState::find_step(int delta) {
    editor_.step_find(delta);
}

EditorLimits GuiState::editor_limits() const {
    const core::TuiLimits& limits = config_.limits.tui;
    return {.undo_bytes = limits.undo_bytes.cap(),
            .syntax = {.note_bytes = limits.highlight_bytes.cap(), .line_bytes = limits.highlight_line_bytes.cap()},
            .completion = tui::completion_limits_of(limits, config_.tuning.completion_min_prefix_bytes)};
}

std::string GuiState::editor_fields() const {
    constexpr std::string_view kGap = "  ";
    constexpr std::string_view kEncoding = "UTF-8";  // tampon her zaman UTF-8; geçersiz not salt-okunur açılır
    const tui::EditorBuffer& buffer = editor_.buffer();
    const tui::TextPosition position = buffer.position();
    std::string fields;
    if (const std::string type = current_type(); !type.empty())
        fields += type + std::string(kGap);
    fields += std::string(kEncoding) + std::string(kGap) +
              (doc_.crlf_shown_as_lf() || buffer.uses_crlf() ? "CRLF" : "LF");
    if (doc_.has_bom())
        fields += std::string(kGap) + "BOM";
    fields += std::string(kGap) + std::to_string(position.line + 1) + ":" + std::to_string(position.column + 1) +
              std::string(kGap) + core::tr("tui.status_lines", {{"count", std::to_string(buffer.line_count())}});
    return fields;
}

void GuiState::focus_search() {
    tab_ = Tab::Notes;
    request_focus(FocusRequest::Search);
}

void GuiState::show_settings_tab() {
    tab_ = Tab::Settings;
}

// Listede görünmeyen (arama/klasör süzgeci) bir notu kaydettikten sonra süzgeç gevşetilir; aksi hâlde yeni
// not kaybolmuş gibi görünür ve seçim başka nota düşerdi.
void GuiState::ensure_visible(const core::NotePath& path) {
    if (model_.index_of(path) < 0)
        clear_query();
    if (model_.index_of(path) < 0)
        set_folder({});
}

std::string GuiState::count_label() const {
    std::string label =
        trash_view_ ? core::tr("gui.trash_count_label", {{"count", std::to_string(trash_items_.size())}})
                    : core::tr("gui.note_count", {{"shown", std::to_string(model_.row_count())},
                                                  {"total", std::to_string(model_.total())}});
    if (const std::size_t marked = marked_count(); marked > 0)
        label += "  " + core::tr("gui.marked_count", {{"count", std::to_string(marked)}});
    return label;
}

// --- Editör sekmeleri ---

}  // namespace noteapp::gui
