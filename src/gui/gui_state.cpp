#include "gui/gui_state.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_views.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "pdf/pdf_export.hpp"
#include "search/fold.hpp"
#include "search/query.hpp"
#include "sync/note_status.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
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

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kCrlf = "\r\n";
const char kStateFileName[] = "gui-state.toml";
constexpr const char* kViewTree = "tree";
constexpr const char* kViewDetails = "details";
constexpr const char* kViewMap = "map";
constexpr int kPercent = 100;
// Elle bozulmuş gui-state.toml pencereyi ekrandan taşırmasın ya da görünmez yapmasın.
constexpr int kMinWindowSide = 200;
constexpr int kMaxWindowSide = 16384;
constexpr double kMinPaneSide = 50.0;
constexpr double kMaxPaneSide = 8192.0;
constexpr std::size_t kContentResultLimit = 1000;

struct PathParts {
    std::string folder;  // "" = kök
    std::string stem;
    std::string type;  // noktasız; uzantısızsa boş
};

PathParts split(const core::NotePath& path) {
    const std::string rel = path.string();
    const auto slash = rel.rfind('/');
    const std::string file = slash == std::string::npos ? rel : rel.substr(slash + 1);
    const auto dot = file.rfind('.');
    return {slash == std::string::npos ? "" : rel.substr(0, slash),
            dot == std::string::npos ? file : file.substr(0, dot),
            dot == std::string::npos ? "" : file.substr(dot + 1)};
}

// Tampon yalnız boşluktan ibaret mi: boş taslak sorulmadan atılır, kaybedilecek bir şey yok.
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

bool is_blank(const std::string& text) {
    return core::first_non_empty_line(text).empty();
}

std::filesystem::path state_file() {
    return core::state_dir() / kStateFileName;
}

bool is_inside(const std::string& path, const std::string& folder) {
    return path.starts_with(folder + '/');
}

std::string join_folder(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + '/' + name;
}

std::string local_hh_mm(std::chrono::system_clock::time_point when) {
    const std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
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

GuiState::GuiState(core::Config config)
    : config_(std::move(config)), store_(core::NoteStore::for_config(config_)),
      rescan_at_(std::chrono::steady_clock::now() + rescan_interval(config_)),
      sync_label_(core::tr("gui.sync_off")),
      settings_(config_, deciders,
                SettingsHost{.flush = [this] { return flush(); },
                             .applied = [this](const core::Config& before,
                                               const core::Config& after) { apply_settings(before, after); },
                             .synced = [this] { rescan(); },
                             .post = [this](std::function<void()> work) { post(std::move(work)); }}) {
    // cppcheck-suppress useInitializationList ; tek seferlik kurulum; üye sırasına bağlanmasın
    sync_note_exe = sync::note_executable_beside_self();
    model_.set_view_lookup([this](std::string_view name) { return core::view_notes(store_, name); });
    rescan();
    load_gui_state();
    start_sync();
}

GuiState::~GuiState() = default;

void GuiState::load_gui_state() {
    // Dosya yok ya da bozuk: varsayılanlar geçerlidir, açılış sürer (not verisi etkilenmez, plan davranış 7).
    std::ifstream in(state_file(), std::ios::binary);  // fs::path: Windows'ta Türkçe yol da açılır
    if (!in)
        return;
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed)
        return;
    const toml::table& table = parsed.table();

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

    // Açık sekmeler (SPEC-gui "Sekmeler"); yoksa ya da hiçbiri kalmadıysa son seçili not.
    if (const toml::array* saved = table["tabs"].as_array()) {
        std::vector<SavedTab> entries;
        for (const toml::node& node : *saved) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr)
                continue;
            if (auto path = (*entry)["path"].value<std::string>())
                entries.push_back(
                    {std::move(*path), (*entry)["pinned"].value_or(false), (*entry)["kept"].value_or(false)});
        }
        const auto active = table["active_tab"].value_or(std::int64_t{0});
        if (restore_tabs(entries, active < 0 ? 0 : static_cast<std::size_t>(active)))
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
    if (autosave_at_ && now >= *autosave_at_) {
        autosave_at_.reset();
        if (dirty_ && shown_ && !new_note_)
            save_existing();
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
        update_content_results();
        model_.set_query(query_);
    });
}

void GuiState::set_content_mode(bool on) {
    content_mode_ = on;
    if (on && content_index_)
        content_index_->refresh();  // kapalıyken rescan indeksi tazelemedi
    query_edited();
}

// İçerik modunda sonuçları ContentIndex verir; ad modunda ya da boş sorguda model fuzzy/tüm liste olur.
void GuiState::update_content_results() {
    if (!search::parse_query(query_))
        return;  // bozuk argüman: model son geçerli sonuçları tutar, hata arama kutusunun altında
    const std::string text = search_text(query_);
    if (!content_mode_ || text.empty()) {  // yalnız argüman: süzülmüş liste, içerik araması yok
        model_.set_content_results(std::nullopt);
        return;
    }
    if (!content_index_) {
        // ponytail: ilk içerik aramasında tüm notlar bir kez okunur (UI'yi kısa süre tutar); sorun olursa
        // arka plan iş parçacığına alınır.
        content_index_ = std::make_unique<search::ContentIndex>(
            store_, static_cast<std::size_t>(config_.tuning.max_note_bytes));
        content_index_->refresh();
    }
    model_.set_content_results(content_index_->search(text, kContentResultLimit));
}

// Gösterilen notta ilk eşleşmeyi seçtirir. Aynı (not, sorgu) için yeniden çalışmaz: yedek tarama kullanıcının
// imlecini geri atmasın.
void GuiState::update_editor_selection() {
    if (!content_mode_ || query_.empty() || !shown_ || new_note_ || binary_) {
        if (!selection_key_.empty()) {
            selection_key_.clear();
            editor_selection_.reset();
        }
        return;
    }
    std::string key = shown_->path.string() + '\n' + query_;
    if (key == selection_key_)
        return;
    selection_key_ = std::move(key);
    editor_selection_ = first_match(buffer_, search_text(query_));
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
    if (current_path() || new_note_)
        request_focus(FocusRequest::Editor);
    else if (const auto parsed = search::parse_query(query_);
             !parsed || !parsed->filters.empty() || parsed->sort)
        return;
    else
        new_note(query_);
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
    if (content_index_ && content_mode_)
        content_index_->refresh();
    if (auto folders = store_.folders(); folders != folders_) {
        folders_ = std::move(folders);
        listing_stale_ = true;
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
        listing_stale_ = true;
        map_edges_stale_ = true;
    }
    update_content_results();
    current_row_ = selected ? model_.index_of(*selected) : -1;
    close_missing_tabs(present);
    apply_filter([] {});
    // Yalnız ilişki değiştiren başka bağlantı (CLI, senkron) listeyi değiştirmez; harita kenarları yine
    // tazelenir.
    if (const std::uint64_t version = store_.data_version(); version != data_version_) {
        data_version_ = version;
        map_edges_stale_ = true;
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

bool GuiState::previewable_type() const {
    return shown_ && pdf::format_for(shown_->path.value()) == pdf::NoteFormat::Markdown;
}

// Düzenlenemeyen metin (geçersiz UTF-8, NUL) md4c'ye verilmez: ayrıştırıcı geçerli UTF-8 bekler.
bool GuiState::can_preview() const {
    return previewable_type() && !binary_ && editable_ && buffer_.size() <= kMaxPreviewBytes;
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
    // Aynı not, aynı mtime: editör içeriğine/kaydırmasına dokunma (rescan sonrası kararlılık). Kirli tampon
    // varken dışarıdaki değişiklik tamponu ezmez; kayıt anında çakışma diyaloğu çıkar.
    if (shown_ && shown_->path == info.path && (shown_->revision == info.revision || dirty_))
        return;
    claim_active_tab();
    if (!(shown_ && shown_->path == info.path)) {
        if (NoteTab* tab = find_tab(active_tab_))
            tab->cursor.reset();  // başka notun imleci
    }
    shown_ = info;
    binary_ = false;
    if (const auto loaded = load_text(info); !loaded)
        set_editor(core::tr("gui.read_failed", {{"detail", core::format_error(loaded.error())}}), false);
    reload_links();
}

// Editörü doldurur; dirty/autosave durumunu sıfırlar.
void GuiState::set_editor(std::string content, bool editable) {
    autosave_at_.reset();
    selection_key_.clear();
    editor_selection_.reset();  // eski metnin ofsetleri yeni içerikte anlamsız
    buffer_ = std::move(content);
    hidden_controls_ = core::count_hidden_controls(buffer_);
    buffer_generation_ = ++editor_generations_;
    editable_ = editable;
    dirty_ = false;
}

std::expected<void, core::Error> GuiState::load_text(const core::NoteInfo& info) {
    auto content = store_.read(info.path);
    if (!content)
        return std::unexpected(std::move(content.error()));
    shown_ = info;
    shown_->revision = content->revision;  // write()'ın beklediği, içerikle aynı anda okunan değer
    std::string& raw = content->text;
    // ImGui uzunluğu int tutar: 2 GiB üstü metni editöre vermek taşardı. Yer tutucu dosyayı salt-okunur
    // bırakır.
    binary_ = core::looks_binary(raw) || raw.size() >= static_cast<std::size_t>(INT_MAX);
    if (binary_) {
        crlf_ = bom_ = false;
        set_editor(core::tr("gui.binary_preview"), false);
        return {};
    }
    bom_ = raw.starts_with(kBom);
    if (bom_)
        raw.erase(0, kBom.size());
    crlf_ = raw.find(kCrlf) != std::string::npos;
    if (crlf_)
        raw = core::normalize_line_endings(raw);
    // Salt-okunur: geçersiz UTF-8 geri yazılırsa bozuk baytlar sessizce değişirdi; NUL'dan sonrasını ImGui
    // görmez (C dizgisi), düzenleme kuyruğu keserdi.
    const bool editable = core::is_valid_utf8(raw) && raw.find('\0') == std::string::npos;
    set_editor(std::move(raw), editable);
    return {};
}

// Kayıt gövdesi: editör her zaman '\n' kullanır; CRLF'li dosya CRLF olarak yazılır (git farkı şişmesin). BOM
// tamponda görünmez ama dosyanın parçasıdır.
std::string GuiState::body(bool crlf_aware) const {
    std::string out = bom_ ? std::string(kBom) : std::string{};
    if (crlf_aware && crlf_) {
        out.reserve(out.size() + buffer_.size() + buffer_.size() / 16);
        for (const char c : buffer_) {
            if (c == '\n')
                out += kCrlf;
            else
                out += c;
        }
        return out;
    }
    return out + buffer_;
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
    return shown_ ? split(shown_->path).type : std::string{};
}

std::vector<std::string> GuiState::type_choices() const {
    std::vector<std::string> choices = config_.types;
    if (const std::string type = current_type(); std::ranges::find(choices, type) == choices.end())
        choices.push_back(type);
    return choices;
}

std::optional<TextSelection> GuiState::take_editor_selection() {
    return std::exchange(editor_selection_, std::nullopt);
}

FocusRequest GuiState::take_focus_request() {
    return std::exchange(focus_request_, FocusRequest::None);
}

void GuiState::buffer_edited() {
    hidden_controls_ = core::count_hidden_controls(buffer_);
    if (!editable_ || !(shown_ || new_note_))
        return;
    dirty_ = true;
    if (NoteTab* tab = find_tab(active_tab_))
        tab->kept = true;       // düzenlenen sekme, sonra açılan notla değişmez
    editor_selection_.reset();  // düzenleme ofsetleri kaydırır
    if (new_note_)              // yeni not ilk kayıtta diyalog ister: autosave yok
        return;
    // Her tuşta yeniden başlar: ardışık düzenlemeler tek yazmaya birleşir.
    autosave_at_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, config_.tuning.autosave_ms));
}

void GuiState::edit_text(std::string text) {
    if (!editable_)
        return;
    buffer_ = std::move(text);
    buffer_edited();
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

void GuiState::new_note(const std::string& first_line) {
    std::string folder = target_folder();  // kayıt seçimi değiştirmeden önce
    // Korunan sekme kalır (taslak kaydedilmeden, not kaydedilerek); taslak yeni sekmede açılır.
    if (keeps_active_tab() ? !push_tab() : !flush())
        return;
    claim_active_tab();
    if (NoteTab* tab = find_tab(active_tab_))
        tab->cursor.reset();
    close_trash();
    selected_folder_.reset();
    draft_folder_ = std::move(folder);
    new_note_ = true;
    shown_.reset();
    reload_links();
    current_row_ = -1;
    crlf_ = bom_ = binary_ = false;
    const std::string initial = first_line.empty() ? std::string{} : first_line + '\n';
    if (!first_line.empty())
        clear_query();  // new_note_ açıkken süzgeç yeniden seçim yapmaz
    set_editor(initial, true);
    editor_selection_ = TextSelection{buffer_.size(), buffer_.size()};
    request_focus(FocusRequest::Editor);
}

bool GuiState::flush() {
    if (new_note_) {
        if (is_blank(buffer_)) {  // boş taslakta kaybedilecek bir şey yok
            new_note_ = false;
            return true;
        }
        return save_new();
    }
    return dirty_ && shown_ ? save_existing() : true;
}

void GuiState::save_now() {
    if (new_note_)
        save_new();
    else if (dirty_ && shown_)
        save_existing();
}

std::expected<void, core::Error> GuiState::try_write() {
    autosave_at_.reset();
    const auto written = store_.write(shown_note().path, body(true), shown_note().revision);
    if (!written)
        return std::unexpected(written.error());
    ++writes_;
    changed();
    shown_note().revision = *written;
    dirty_ = false;
    status_.clear();
    // ponytail: her otomatik kayıtta tam yeniden tarama (>= 1 sn boşta sonrası, 5.000 notta ~20 ms) kabul
    // edilebilir; yükseltme yolu: write()'ın döndürdüğü mtime ile yalnız ilgili model satırını güncellemek.
    rescan();  // listedeki mtime tazelenir: yoksa aynı not "dışarıdan değişmiş" sanılıp yeniden yüklenirdi
    return {};
}

bool GuiState::save_existing() {
    const auto written = try_write();
    if (written)
        return true;
    switch (written.error().code) {
    case core::Error::Code::ModifiedExternally:
        return resolve_conflict();
    case core::Error::Code::NotFound:
        // Not dışarıdan silinmiş: tamponu yeni not olarak sakla, kullanıcı yeniden adlandırıp kaydetsin.
        new_note_ = true;
        draft_folder_ = current_folder_;
        shown_.reset();
        return save_new();
    default:
        report(written.error());
        return false;
    }
}

bool GuiState::resolve_conflict() {
    switch (deciders.conflict(shown_note().path.string())) {
    case ConflictChoice::Reload: {
        const core::NoteInfo info = shown_note();  // load_text shown_'ı yeniden atar
        const auto loaded = load_text(info);
        if (!loaded) {
            report(loaded.error());
            return false;
        }
        return true;
    }
    case ConflictChoice::Overwrite: {
        // Diskteki güncel mtime alınır; kullanıcı bilerek ezdi.
        const auto fresh = store_.read(shown_note().path);
        if (!fresh) {
            report(fresh.error());
            return false;
        }
        shown_note().revision = fresh->revision;
        if (const auto written = try_write(); !written) {
            report(written.error());
            return false;
        }
        return true;
    }
    case ConflictChoice::SaveCopy:
        return save_copy();
    case ConflictChoice::Cancel:
        break;
    }
    return false;
}

bool GuiState::save_copy() {
    const PathParts parts = split(shown_note().path);
    const auto created =
        store_.create(core::NewNote{parts.folder, parts.stem + " (copy)", parts.type, body(false)});
    if (!created) {
        report(created.error());
        return false;
    }
    changed();
    const bool adopted = adopt_saved(*created);
    if (adopted)
        status_ = core::tr("gui.saved_copy", {{"path", created->string()}});
    return adopted;
}

bool GuiState::save_new() {
    autosave_at_.reset();
    const std::string content = body(false);
    const detect::Options options{config_.default_type, config_.tuning.min_confidence,
                                  config_.tuning.max_scan_bytes};
    const detect::Suggestion suggestion = detect::suggest(content, options);

    SaveDialogRequest request;
    request.initial = {core::slugify(core::note_name_source(content, suggestion.type),
                                     std::chrono::system_clock::now(), config_.tuning.slug_max_length),
                       draft_folder_, suggestion.type};
    request.folders = folders_;
    request.types = config_.types;
    request.hint =
        suggestion.detected
            ? core::tr("gui.suggestion",
                       {{"type", suggestion.type},
                        {"percent", std::to_string(std::lround(suggestion.confidence * kPercent))}})
            : core::tr("gui.suggestion_default", {{"type", suggestion.type}});
    const auto chosen = deciders.save_dialog(request);
    if (!chosen)
        return false;  // vazgeç: tampon ve yeni-not durumu aynen kalır

    const auto created = store_.create(core::NewNote{chosen->folder, chosen->name, chosen->type, content});
    if (!created) {
        report(created.error());
        return false;
    }
    changed();
    return adopt_saved(*created);
}

// Diske yazılmış notu "mevcut not" yapar: mtime alınır, liste tazelenir, not seçili kalır.
bool GuiState::adopt_saved(const core::NotePath& path) {
    const auto content = store_.read(path);
    if (!content) {
        report(content.error());  // not diskte, ama tampon kirli kalır: kullanıcı yeniden deneyebilir
        return false;
    }
    shown_ = core::NoteInfo{path, content->mtime, content->text.size(), content->revision};
    binary_ = false;  // editörden kaydedilen metin
    new_note_ = false;
    dirty_ = false;
    crlf_ = false;  // create() satır sonlarını LF'ye normalleştirir
    autosave_at_.reset();
    status_.clear();
    // Liste tazelenirken seçim süzgeçteki başka nota kayıp editörü ezmesin: kaydedilen not gösterilir.
    const struct Restore {
        bool& flag;
        bool value;
        ~Restore() { flag = value; }
    } restore{in_show_, std::exchange(in_show_, true)};
    rescan();
    ensure_visible(path);
    return true;
}

bool GuiState::confirm_close() {
    if (new_note_) {
        if (is_blank(buffer_))
            return true;
        switch (deciders.close_unsaved()) {
        case CloseChoice::Save:
            return save_new();
        case CloseChoice::Discard:
            return true;
        case CloseChoice::Cancel:
            return false;
        }
    }
    return dirty_ && shown_ ? save_existing() : true;
}

// Kapanmazsa Ayarlar sekmesi öne gelir: kullanıcı değişikliği ya da alanın altındaki hatayı görür.
bool GuiState::confirm_close_settings() {
    if (!settings_.has_changes())
        return true;
    switch (deciders.close_unsaved_settings()) {
    case CloseChoice::Save:
        settings_.save();
        if (!settings_.has_changes())
            return true;
        break;
    case CloseChoice::Discard:
        settings_.revert();
        return true;
    case CloseChoice::Cancel:
        break;
    }
    show_settings_tab();
    return false;
}

bool GuiState::request_close() {
    // Ayarlar önce: kayıtta notes_dir değişirse açık notu kendisi kaydeder (host flush), not sorusu boşa
    // çıkar.
    if (!confirm_close_settings() || !confirm_close() ||
        !confirm_close_drafts())  // vazgeçildi ya da kayıt başarısız: pencere ve tampon yerinde kalır
        return false;
    save_gui_state();
    // Kayıt akışından sonra: bekleyen değişiklik için son senkron, en çok exit_timeout_seconds.
    stop_sync(std::chrono::seconds(config_.tuning.exit_timeout_seconds));
    return true;
}

SaveDialogRequest GuiState::move_request(const core::NotePath& note) const {
    const PathParts parts = split(note);
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Move;
    // `a.tar.gz`: gövde de uzantı gibi görünür; adın tamamı gösterilir, form onu aynen geri verir.
    const bool dotted_stem = !core::split_typed_name(parts.stem).extension.empty();
    request.initial = {dotted_stem ? parts.stem + "." + parts.type : parts.stem, parts.folder, parts.type};
    request.folders = folders_;
    request.types = config_.types;
    return request;
}

void GuiState::save_as() {
    if (new_note_) {
        save_new();
        return;
    }
    if (!shown_)
        return;
    const core::NotePath note = shown_->path;
    if (!flush_for(note))
        return;
    if (const auto chosen = deciders.save_dialog(move_request(note)))
        move_saved(note, *chosen);
}

void GuiState::change_type(const std::string& type) {
    if (!shown_ || new_note_ || type.empty())
        return;
    const core::NotePath note = shown_->path;
    const PathParts parts = split(note);
    if (type == parts.type)
        return;
    if (flush_for(note))
        move_saved(note, {parts.stem, parts.folder, type});
}

// flush listeyi tazeler: not aramadan düşerse editör başka nota geçer ya da boşalır, seçim artık hedef
// değildir. İşlem flush'tan önce alınan nota uygulanır; o not yoksa (dışarıdan silinmiş) iletiyle durur.
bool GuiState::flush_for(const core::NotePath& note) {
    if (!flush())
        return false;
    const auto content = store_.read(note);
    if (!content)
        fail(content.error());
    return content.has_value();
}

// Bekleyen düzenleme önce kendi notuna yazılır (flush_for): kayıt listeyi tazeler, seçim kayıttan sonra
// değişirse düzenleme yanlış nota giderdi. false = kayıt olmadı ya da vazgeçildi.
bool GuiState::flush_shown() {
    if (!unsaved())
        return true;
    // Kopya: kayıt akışı shown_'ı sıfırlayabilir.
    const auto note = shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    return note ? flush_for(*note) : flush();
}

// Tampon önceden kaydedilmiş olmalı (flush). Hedef varsa NoteStore::move AlreadyExists verir, hiçbir şey
// değişmez.
bool GuiState::move_saved(const core::NotePath& note, const SaveDetails& target) {
    const auto to = core::NotePath::parse((target.folder.empty() ? "" : target.folder + "/") + target.name +
                                          "." + target.type);
    if (!to) {
        report(to.error());
        return false;
    }
    if (*to == note)
        return true;
    const auto moved = store_.move(note, *to);
    if (!moved) {
        report(moved.error());
        return false;
    }
    changed();
    if (shown_ && shown_->path == note) {
        shown_->path = *moved;
        if (const auto fresh = store_.read(*moved))  // taşıma mtime'ı korur; yine de güncel değeri al
            shown_->revision = fresh->revision;
    }
    status_.clear();
    rescan();
    ensure_visible(*moved);
    return true;
}

void GuiState::move_note() {
    if (new_note_ || !shown_)
        return;
    const core::NotePath note = shown_->path;
    if (!flush_for(note))
        return;
    const PathParts parts = split(note);
    SaveDialogRequest request = move_request(note);
    request.folder_only = true;
    // Ad ve tip kilitli: karar ne dönerse dönsün yalnız klasör alınır.
    if (const auto chosen = deciders.save_dialog(request))
        move_saved(note, {parts.stem, chosen->folder, parts.type});
}

void GuiState::delete_note() {
    const auto path = current_path();
    if (!path)
        return;
    if (!deciders.confirm_delete(core::tr("gui.confirm_delete_note", {{"path", path->string()}})))
        return;
    const bool shown = shown_ && shown_->path == *path;
    if (shown && !flush())
        return;
    // flush mtime'ı değiştirip satırı yukarı taşımış olabilir: komşu satır silmeden önce çözülür.
    const int row = std::max(0, model_.index_of(*path));
    const int browser_index = browsing() ? browser_row() : -1;
    if (const auto removed = store_.remove(*path); !removed) {
        fail(removed.error());
        return;
    }
    changed();
    select_after_removal(shown, row, browser_index);
}

void GuiState::select_after_removal(bool was_shown, int row, int browser_row) {
    status_.clear();
    if (was_shown) {
        // Notun sekmesi kapanır; başka sekme varsa komşusu öne gelir, yoksa en yakın not yeni sekmede açılır.
        drop_active_tab();
        if (!tabs_.empty()) {
            rescan();
            return;
        }
    }
    rescan();
    if (browser_row >= 0 && select_nearest_note(browser_row))
        return;
    // Ayrıntılar yalnız doğrudan çocukları listeler: modelin alt ağaçtaki notu görünmeyen bir seçim olurdu.
    if (browser_row >= 0 && layout_.browser_view == BrowserView::Details) {
        show_folder(current_folder_, false);
        return;
    }
    if (const int count = model_.row_count(); count > 0)
        select_row(std::min(row, count - 1));
}

void GuiState::trash_note() {
    const auto path = current_path();
    if (!path)
        return;
    const bool shown = shown_ && shown_->path == *path;
    if (shown && !flush())  // çöpe son hâli gitsin
        return;
    const int row = std::max(0, model_.index_of(*path));
    const int browser_index = browsing() ? browser_row() : -1;
    const auto trashed = store_.trash(*path);
    if (!trashed) {
        fail(trashed.error());
        return;
    }
    changed();
    select_after_removal(shown, row, browser_index);
    undo_item_ = trashed->path;
    undo_status_ = core::tr("gui.trashed", {{"path", path->string()}});
    status_ = undo_status_;
}

void GuiState::undo_trash() {
    if (!can_undo_trash())
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- can_undo_trash undo_item_'ı denetledi
    const auto restored = store_.restore(*undo_item_);
    if (!restored) {
        fail(restored.error());
        undo_status_ = status_;  // teklif hata iletisiyle kalır: kilit kalkınca yeniden denenir
        return;
    }
    undo_item_.reset();
    changed();
    status_.clear();
    rescan();
    ensure_visible(*restored);
    select_row(model_.index_of(*restored));
}

void GuiState::delete_selected(bool permanent) {
    if (trash_view_) {
        purge_trashed();
    } else if (selected_folder_) {
        if (!can_delete())  // kök ya da vurgulu satırı olmayan klasör
            return;
        const std::string folder = *selected_folder_;  // kopya: silme seçimi atasına taşır
        delete_folder(folder);
    } else if (permanent) {
        delete_note();
    } else {
        trash_note();
    }
}

void GuiState::reload_trash() {
    auto items = store_.list_trash();
    if (!items) {
        fail(items.error());
        items.emplace();
    }
    trash_items_ = std::move(*items);
    const int count = static_cast<int>(trash_items_.size());
    trash_row_ = count == 0 ? -1 : std::clamp(trash_row_, 0, count - 1);
}

void GuiState::open_trash() {
    reload_trash();
    trash_view_ = true;
    request_focus(FocusRequest::List);
}

void GuiState::toggle_trash() {
    if (trash_view_)
        close_trash();
    else
        open_trash();
}

void GuiState::select_trash_row(int row) {
    if (row >= 0 && row < static_cast<int>(trash_items_.size()))
        trash_row_ = row;
}

void GuiState::restore_trashed() {
    if (trash_row_ < 0)
        return;
    const core::NotePath item = trash_items_[static_cast<std::size_t>(trash_row_)].path;
    const auto restored = store_.restore(item);
    if (!restored) {
        fail(restored.error());
        return;
    }
    changed();
    if (undo_item_ == item)
        undo_item_.reset();
    rescan();
    status_ = core::tr("gui.restored", {{"path", restored->string()}});
}

void GuiState::purge_trashed() {
    if (trash_row_ < 0)
        return;
    const core::NotePath item = trash_items_[static_cast<std::size_t>(trash_row_)].path;
    if (!deciders.confirm_delete(core::tr("gui.confirm_purge", {{"path", item.string()}})))
        return;
    if (const auto purged = store_.purge(item); !purged) {
        fail(purged.error());
        return;
    }
    changed();
    if (undo_item_ == item)
        undo_item_.reset();
    status_.clear();
    rescan();
}

void GuiState::empty_trash() {
    if (trash_items_.empty())
        return;
    if (!deciders.confirm_delete(
            core::tr("gui.confirm_empty_trash", {{"count", std::to_string(trash_items_.size())}})))
        return;
    if (const auto emptied = store_.empty_trash(); !emptied) {
        fail(emptied.error());
        return;
    }
    changed();
    undo_item_.reset();
    status_.clear();
    rescan();
}

bool GuiState::shown_in(const std::string& folder) const {
    return shown_ && is_inside(shown_->path.string(), folder);
}

void GuiState::new_folder(const std::string& parent) {
    const auto name = deciders.folder_name(core::tr("gui.new_folder_title"), {});
    if (!name)
        return;
    const std::string folder = join_folder(parent, *name);
    // create_folder yinelenebilir (idempotent) olduğundan var olan klasör çakışması burada yakalanır.
    if (std::ranges::find(folders_, folder) != folders_.end()) {
        fail(core::Error{core::Error::Code::AlreadyExists, "error.already_exists", {{"path", folder}}});
        return;
    }
    if (const auto created = store_.create_folder(folder); !created) {
        fail(created.error());
        return;
    }
    changed();
    status_.clear();
    set_expanded(parent, true);  // yeni klasör ağaçta görünsün
    rescan();
}

void GuiState::rename_folder(const std::string& folder) {
    if (folder.empty())
        return;
    const auto slash = folder.rfind('/');
    const std::string parent = slash == std::string::npos ? "" : folder.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? folder : folder.substr(slash + 1);
    const auto name = deciders.folder_name(core::tr("gui.rename_folder_title"), leaf);
    if (!name || *name == leaf)
        return;
    const std::string target = join_folder(parent, *name);

    if (shown_in(folder) && !flush())  // kirli tampon yeni yola değil, taşınmadan önce kaydedilir
        return;
    if (const auto renamed = store_.rename_folder(folder, target); !renamed) {
        fail(renamed.error());
        rescan();
        return;
    }
    changed();
    status_.clear();
    // flush sonrası bakılır: dışarıdan silinen not kaydet diyaloğuyla başka klasöre gitmiş olabilir.
    if (shown_in(folder)) {
        if (const auto moved =
                core::NotePath::parse(target + shown_note().path.string().substr(folder.size()))) {
            shown_note().path = *moved;
            if (const auto fresh = store_.read(*moved))
                shown_note().revision = fresh->revision;
        }
    }
    const auto renamed_path = [&](const std::string& path) {
        return path == folder || is_inside(path, folder) ? target + path.substr(folder.size()) : path;
    };
    for (NoteTab& tab : tabs_) {  // arka plandaki sekmeler yeni yolu izler; gösterilen not yukarıda
        if (!tab.saved.shown || !is_inside(tab.saved.shown->path.string(), folder))
            continue;
        if (const auto moved = core::NotePath::parse(renamed_path(tab.saved.shown->path.string())))
            tab.saved.shown->path = *moved;
    }
    current_folder_ = renamed_path(current_folder_);
    if (selected_folder_)
        selected_folder_ = renamed_path(*selected_folder_);
    std::set<std::string> expanded;
    for (const std::string& path : expanded_)
        expanded.insert(renamed_path(path));
    expanded_ = std::move(expanded);
    rows_stale_ = true;
    rescan();
}

void GuiState::delete_folder(const std::string& folder) {
    if (folder.empty())
        return;
    // Not listesi gizli ya da geçersiz adlı dosyaları göstermez: onaydaki sayıyı listeye bakarak değil, silme
    // işleminin kendisine (NotEmpty'nin {count}'u) sordururuz; sayılmayan şey sessizce silinmez. Listede not
    // varsa klasör kesin dolu; yalnız "boş görünen" klasörde önce sade onay sorulur (boş klasör onaysız
    // silinmesin diye).
    const auto notes = store_.list();
    const bool has_notes = std::ranges::any_of(
        notes, [&folder](const core::NoteInfo& info) { return is_inside(info.path.string(), folder); });
    if (!has_notes &&
        !deciders.confirm_delete(core::tr("gui.confirm_delete_empty_folder", {{"folder", folder}})))
        return;

    auto removed = store_.remove_folder(folder, false);  // dolu klasörde hiçbir şey silmez
    if (!removed && removed.error().code == core::Error::Code::NotEmpty) {
        std::string count = "?";
        for (const auto& [key, value] : removed.error().args) {
            if (key == "count")
                count = value;
        }
        if (!deciders.confirm_delete(
                core::tr("gui.confirm_delete_folder", {{"folder", folder}, {"count", count}})) ||
            (shown_in(folder) && !flush()))
            return;
        removed = store_.remove_folder(folder, true);
    }
    if (!removed) {  // contains_link dahil: hiçbir şey silinmedi
        fail(removed.error());
        rescan();
        return;
    }
    changed();
    status_.clear();
    rescan();  // klasördeki notların sekmeleri (gösterilen dahil) iletiyle kapanır: close_missing_tabs
}

void GuiState::export_pdf() {
    if (shown_ && binary_)
        return;
    // Önce kaydet: PDF diskteki sürümden üretilir ve tampondaki düzenleme kaybolmaz. Kayıt başarısızsa ya da
    // vazgeçildiyse (tampon korunur) dışa aktarma da yapılmaz.
    std::optional<core::NotePath> note = shown_ ? std::optional(shown_->path) : std::nullopt;
    if (note ? !flush_for(*note) : (unsaved() && !flush()))
        return;
    if (!note && shown_)  // taslak: yolu ancak kayıttan sonra belli olur
        note = shown_->path;
    if (!note)  // gösterilen not yok ya da boş taslak atıldı
        return;
    const auto output = deciders.export_path(split(*note).stem + ".pdf");
    if (!output)
        return;
    const auto content = store_.read(*note);
    if (!content) {
        fail(content.error());
        return;
    }
    const auto exported =
        pdf::export_pdf(content->text, pdf::format_for(note->value()), path_from_utf8(*output));
    if (!exported) {
        fail(exported.error());
        return;
    }
    status_ = core::tr("gui.exported", {{"path", *output}});
}

bool GuiState::can_export() const {
    // Boş taslakta export_pdf taslağı atar ve sessizce çıkar; ikili notu reddeder.
    if (new_note_)
        return !is_blank(buffer_);
    return shown_.has_value() && !binary_;
}

void GuiState::changed() {
    ++changes_;
    if (auto_sync_)
        auto_sync_->notify_changed();
}

void GuiState::sync_now() {
    ++sync_requests_;
    if (auto_sync_)
        auto_sync_->request_now();
}

void GuiState::start_sync() {
    if (auto_sync_) {
        // Yıkıcı stop(0) çağırır ve süren birleştirmeyi öldürürdü: önce bekleyen son senkron bitsin.
        deciders.while_busy([this] { stop_sync(std::chrono::seconds(config_.tuning.exit_timeout_seconds)); });
    }
    if (!config_.sync.enabled) {
        show_sync_status({}, false);
        return;
    }
    // Çağrı İŞÇİ iş parçacığından gelir: durum kopyalanıp UI iş parçacığına aktarılır. AutoSync bu nesneden
    // önce yıkılır, `this` geri çağrı sürdükçe geçerlidir. Çekilen notlar sonraki taramayı beklemeden listeye
    // yansır.
    auto_sync_ = std::make_unique<sync::AutoSync>(
        config_, sync_note_exe,
        [this, generation = sync_generation_](const sync::SyncStatus& status, bool running) {
            post([this, generation, status, running] {
                if (generation != sync_generation_)
                    return;
                show_sync_status(status, running);
                if (!running)
                    rescan();
            });
        });
    ++sync_restarts_;
    show_sync_status({}, true);
    auto_sync_->start();
}

void GuiState::stop_sync(std::chrono::seconds timeout) {
    if (!auto_sync_)
        return;
    auto_sync_->stop(timeout);
    auto_sync_.reset();
    ++sync_generation_;
}

void GuiState::show_sync_status(const sync::SyncStatus& status, bool running) {
    if (!auto_sync_ && !running && !status.last_success && !status.last_error)
        sync_label_ = core::tr("gui.sync_off");
    else if (running)
        // NOLINTNEXTLINE(bugprone-branch-clone) -- hata ve son başarı denetiminden önce gelmeli
        sync_label_ = core::tr("gui.sync_running");
    else if (status.last_error)
        sync_label_ = core::tr("gui.sync_error", {{"detail", core::format_error(*status.last_error)}});
    else if (!status.log_issues.empty())  // başarılı ama bir cihazın günlüğü duruyor: sessiz kalmaz
        sync_label_ = core::tr("gui.sync_error", {{"detail", core::format_error(status.log_issues.front())}});
    else if (status.last_success)
        sync_label_ = core::tr("gui.sync_ok", {{"time", local_hh_mm(*status.last_success)}});
    else
        sync_label_ = core::tr("gui.sync_running");  // açıldı, henüz sonuç yok
}

// Kaydedilen ayarları canlı uygular. Dil değişimi canlı çevrilmez (SPEC-gui kapsam dışı): yalnız bildirilir.
void GuiState::apply_settings(const core::Config& before, const core::Config& after) {
    config_ = after;
    if (before.notes_dir != after.notes_dir) {
        // SettingsPage kaydetmeden önce flush ettirdi: tampon temiz, eski dizinin notu bırakılır.
        store_ = core::NoteStore::for_config(config_);
        content_index_.reset();
        // Eski dizinin notlarının sekmeleri kapanır; taslaklar dizine bağlı değildir, kalır.
        std::erase_if(tabs_, [](const NoteTab& tab) { return !tab.saved.new_note; });
        active_tab_ = tabs_.empty() ? 0 : tabs_.front().id;
        parked_ = !tabs_.empty();
        shown_.reset();
        current_folder_.clear();
        selected_folder_.reset();
        expanded_.clear();
        listing_stale_ = true;
        folders_.clear();
        set_editor({}, false);
    }
    rescan();
    // AutoSync yalnız bu alanları kurulurken okur; yazar adı/e-postasını `note sync --background` config'ten
    // taze okur, değişince çalışan senkronu öldürmek gerekmez.
    if (before.sync.enabled != after.sync.enabled || before.sync.remote != after.sync.remote ||
        before.sync.branch != after.sync.branch || before.sync.idle_seconds != after.sync.idle_seconds ||
        before.notes_dir != after.notes_dir)
        start_sync();
    if (before.language != after.language)
        deciders.info(core::tr("gui.restart_needed"));
}

std::string GuiState::count_label() const {
    if (trash_view_)
        return core::tr("gui.trash_count_label", {{"count", std::to_string(trash_items_.size())}});
    return core::tr("gui.note_count", {{"shown", std::to_string(model_.row_count())},
                                       {"total", std::to_string(model_.total())}});
}

void GuiState::show_config_warnings(const std::vector<core::Error>& warnings) {
    std::string messages;
    for (const auto& warning : warnings) {
        if (!messages.empty())
            messages += "; ";
        messages += core::format_error(warning);
    }
    // Geçiş bekleyen ya da açılamayan depoda liste boştur: kullanıcı bunu notların kaybı sanmasın.
    if (const auto issue = store_.health()) {
        if (!messages.empty())
            messages += "; ";
        messages += core::format_error(*issue);
    }
    if (!messages.empty())
        status_ = std::move(messages);
}

void GuiState::set_browser_view(BrowserView view) {
    if (layout_.browser_view == view)
        return;
    layout_.browser_view = view;
    rows_stale_ = true;
    revealed_note_.clear();  // Ayrıntılar'a geçince konum gösterilen notun klasörüne gelsin
}

void GuiState::set_map_options(const MapOptions& options) {
    MapOptions clamped = options;
    clamped.depth = std::clamp(options.depth, MapOptions::kMinDepth, MapOptions::kMaxDepth);
    if (clamped == map_options_)
        return;
    map_options_ = clamped;
    map_stale_ = true;
}

const NoteMap& GuiState::note_map() {
    ensure_map();
    return map_;
}

GraphLayout& GuiState::map_layout() {
    ensure_map();
    return map_layout_;
}

// Kenarlar yalnız taramada değişiklik görülünce (listeleme ya da başka bağlantının yazması) tek okumayla
// gelir; görünür harita seçenek ya da çevre merkezi değişince bellekten kurulur.
void GuiState::ensure_map() {
    if (map_edges_stale_) {
        ensure_listing();
        ++map_reads_;
        if (auto edges = store_.edges()) {
            map_edges_ = std::move(*edges);
        } else {
            map_edges_.clear();
            core::log_warn("event=map_edges_failed detail=" + core::format_error(edges.error()));
        }
        map_edges_stale_ = false;
        map_stale_ = true;
    }
    auto center = shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    if (center != map_center_) {
        map_center_ = std::move(center);
        if (!map_options_.whole_graph)  // bütün çizgede merkez yalnız vurgudur
            map_stale_ = true;
    }
    if (!map_stale_)
        return;
    map_stale_ = false;
    map_ = build_note_map(metas_, map_edges_, map_center_, map_options_);
    std::vector<std::string> keys;
    keys.reserve(map_.nodes.size());
    for (const MapNode& node : map_.nodes)  // önek: "#x.md" adlı not, x.md etiketiyle karışmasın
        keys.push_back((node.kind == MapNodeKind::Tag ? "t:" : "n:") + node.key);
    std::vector<GraphLayout::Edge> edges;
    edges.reserve(map_.edges.size());
    for (const MapEdge& edge : map_.edges)
        edges.push_back({edge.from, edge.to});
    if (map_layout_.set_graph(std::move(keys), std::move(edges)))
        ++map_generation_;
    map_matches_stale_ = true;
}

// Süzgeç listeyle aynıdır (model_): eşleşme yalnız süzgeç ya da harita değişince kurulur, kare başına değil.
const std::vector<char>& GuiState::map_matches() {
    ensure_map();
    if (!map_matches_stale_ && map_matches_resets_ == model_.resets())
        return map_matches_;
    map_matches_stale_ = false;
    map_matches_resets_ = model_.resets();
    map_matches_.assign(map_.nodes.size(), query_.empty() ? 1 : 0);
    if (query_.empty())
        return map_matches_;
    std::unordered_set<std::string> listed;
    listed.reserve(static_cast<std::size_t>(model_.row_count()));
    for (int row = 0; row < model_.row_count(); ++row)
        listed.insert(model_.note_at(row).path.string());
    for (std::size_t i = 0; i < map_.nodes.size(); ++i)
        map_matches_[i] =
            map_.nodes[i].kind == MapNodeKind::Note && listed.contains(map_.nodes[i].key) ? 1 : 0;
    for (const MapEdge& edge : map_.edges) {  // etiket, eşleşen bir notunda yanar
        if (edge.kind == MapEdgeKind::Tag && map_matches_[edge.from] != 0)
            map_matches_[edge.to] = 1;
    }
    return map_matches_;
}

void GuiState::open_map_note(const core::NotePath& path) {
    if (flush_shown())
        open_link(path);
}

void GuiState::set_row_numbers(bool on) {
    layout_.row_numbers = on;
    save_gui_state();
}

void GuiState::ensure_listing() {
    if (!listing_stale_)
        return;
    metas_ = store_.list_meta();
    tree_.rebuild(metas_, folders_);
    listing_stale_ = false;
    rows_stale_ = true;
}

void GuiState::ensure_browser() {
    ensure_listing();
    const bool details = layout_.browser_view == BrowserView::Details;
    if (details && built_folder_ != current_folder_)
        rows_stale_ = true;
    if (!rows_stale_)
        return;
    browser_rows_ =
        details ? folder_rows(tree_, metas_, current_folder_) : tree_rows(tree_, metas_, expanded_);
    built_folder_ = current_folder_;
    rows_stale_ = false;
    ++browser_rebuilds_;
    max_row_number_ = 0;
    for (const BrowserRow& row : browser_rows_)
        max_row_number_ = std::max(max_row_number_, row.number);
}

const std::vector<BrowserRow>& GuiState::browser_rows() {
    ensure_browser();
    return browser_rows_;
}

int GuiState::browser_row() {
    BrowserRowKind kind{};
    std::string path;
    if (selected_folder_) {
        kind = BrowserRowKind::Folder;
        path = *selected_folder_;
    } else if (shown_ && !new_note_) {
        kind = BrowserRowKind::Note;
        path = shown_->path.string();
    } else {
        return -1;
    }
    const auto& rows = browser_rows();
    // ponytail: doğrusal arama, satır başına bir karşılaştırma; çok büyük ağaçta seçim indeksi önbelleğe
    // alınır.
    const auto found = std::ranges::find_if(
        rows, [&](const BrowserRow& row) { return row.kind == kind && row.path == path; });
    return found == rows.end() ? -1 : static_cast<int>(found - rows.begin());
}

std::optional<FolderSummary> GuiState::folder_summary() {
    if (!selected_folder_)
        return std::nullopt;
    ensure_browser();
    const core::FolderNode* node = tree_.find(*selected_folder_);
    if (node == nullptr)
        return FolderSummary{*selected_folder_, 0, 0};
    return FolderSummary{node->path, node->note_count, node->folder_count};
}

void GuiState::set_expanded(const std::string& folder, bool expanded) {
    if (folder.empty())  // kök hep açık
        return;
    const bool changed = expanded ? expanded_.insert(folder).second : expanded_.erase(folder) > 0;
    if (changed)
        rows_stale_ = true;
}

void GuiState::open_note(const core::NotePath& path) {
    close_trash();
    ensure_visible(path);
    if (const int row = model_.index_of(path); row >= 0)
        select_row(row);
}

void GuiState::select_folder(const std::string& folder) {
    show_folder(folder, layout_.browser_view == BrowserView::Tree);
}

void GuiState::enter_folder(const std::string& folder) {
    show_folder(folder, true);
}

// Kayıt olmazsa ya da vazgeçilirse not açık kalır.
bool GuiState::show_folder(const std::string& folder, bool scope) {
    if (!flush_shown())
        return false;
    close_trash();
    park_or_clear();  // sekme notunu tutar: sekmeye tıklayınca geri gelir
    selected_folder_ = folder;
    current_row_ = -1;
    if (scope) {
        apply_filter([&] {
            current_folder_ = folder;
            model_.set_folder(current_folder_);
        });
    }
    clear_editor();
    return true;
}

void GuiState::activate_row(int index) {
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];  // kopya: seçim satırları yeniler
    if (row.kind == BrowserRowKind::Folder)
        select_folder(row.path);
    else if (const auto path = core::NotePath::parse(row.path))
        open_note(*path);
}

void GuiState::navigate(int delta) {
    if (!browsing()) {
        move_selection(delta);
        return;
    }
    const int count = static_cast<int>(browser_rows().size());
    if (count == 0)
        return;
    const int from = browser_row();
    const int to = from < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(from + delta, 0, count - 1);
    if (to != from)
        activate_row(to);
}

void GuiState::browse_right() {
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    if (row.kind != BrowserRowKind::Folder)
        return;
    if (layout_.browser_view == BrowserView::Details)
        enter_folder(row.path);
    else if (!row.expanded)
        set_expanded(row.path, true);
    else if (row.has_children)
        activate_row(index + 1);  // açık klasörün ilk çocuğu hemen altındadır
}

void GuiState::browse_left() {
    if (layout_.browser_view == BrowserView::Details) {
        if (!current_folder_.empty())
            enter_folder(core::parent_folder(current_folder_));
        return;
    }
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    const bool folder = row.kind == BrowserRowKind::Folder;
    if (folder && row.path.empty())
        return;
    if (folder && row.expanded)
        set_expanded(row.path, false);
    else
        select_folder(core::parent_folder(row.path));
}

void GuiState::browse_enter() {
    const int index = browser_row();
    if (index < 0)
        return;
    const BrowserRow row = browser_rows()[static_cast<std::size_t>(index)];
    if (row.kind == BrowserRowKind::Note)
        request_focus(FocusRequest::Editor);
    else if (layout_.browser_view == BrowserView::Details)
        enter_folder(row.path);
    else
        toggle_expanded(row.path);
}

void GuiState::reveal_selection() {
    if (!shown_ || new_note_) {
        revealed_note_.clear();
        return;
    }
    if (!browsing())
        return;
    std::string path = shown_->path.string();
    if (path == revealed_note_)
        return;
    const std::string folder = core::folder_of(shown_->path);
    if (layout_.browser_view == BrowserView::Details) {
        if (folder != current_folder_) {
            apply_filter([&] {
                current_folder_ = folder;
                model_.set_folder(current_folder_);
            });
        }
    } else {
        for (std::string walk = folder; !walk.empty(); walk = core::parent_folder(walk))
            set_expanded(walk, true);
    }
    revealed_note_ = std::move(path);
}

std::string GuiState::target_folder() const {
    if (selected_folder_)
        return *selected_folder_;
    if (shown_ && !new_note_)
        return core::folder_of(shown_->path);
    return current_folder_;
}

void GuiState::rename_selected() {
    if (!can_rename())
        return;
    if (selected_folder_) {
        const std::string folder = *selected_folder_;  // kopya: yeniden adlandırma seçimi de günceller
        rename_folder(folder);
    } else {
        save_as();
    }
}

// Klasöre yıkıcı ya da yeniden adlandıran eylem yalnız vurgulu satırına uygulanır: Ayrıntılar'da görüntülenen
// klasörün ve kapalı bir klasörün altındaki seçimin satırı yoktur, kullanıcı neyi sileceğini görmez.
bool GuiState::folder_action_allowed() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıranlar selected_folder_'ı denetledi
    return !selected_folder_->empty() && browsing() && browser_row() >= 0;
}

bool GuiState::can_delete() {
    if (trash_view_)
        return trash_row_ >= 0;
    return selected_folder_ ? folder_action_allowed() : current_path().has_value();
}

bool GuiState::can_rename() {
    if (trash_view_)
        return false;
    return selected_folder_ ? folder_action_allowed() : shown_.has_value() && !new_note_;
}

// Silinen satırın yerine gelen ya da üstündeki ilk görünür not: modelin sırası (mtime) ağacın sırası
// değildir, seçim kapalı bir klasördeki nota atlamasın. Görünür not yoksa false.
bool GuiState::select_nearest_note(int from) {
    const auto& rows = browser_rows();
    const int count = static_cast<int>(rows.size());
    const auto is_note = [&](int index) {
        return rows[static_cast<std::size_t>(index)].kind == BrowserRowKind::Note;
    };
    for (int index = std::max(0, std::min(from, count - 1)); index < count; ++index) {
        if (is_note(index)) {
            activate_row(index);
            return true;
        }
    }
    for (int index = std::min(from, count) - 1; index >= 0; --index) {
        if (is_note(index)) {
            activate_row(index);
            return true;
        }
    }
    return false;
}

// --- Editör sekmeleri ---

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
    crlf_ = bom_ = binary_ = false;
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
                                .editable = editable_,
                                .dirty = dirty_,
                                .new_note = new_note_,
                                .crlf = crlf_,
                                .bom = bom_,
                                .binary = binary_,
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
        crlf_ = saved.crlf;
        bom_ = saved.bom;
        binary_ = saved.binary;
        autosave_at_.reset();
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
