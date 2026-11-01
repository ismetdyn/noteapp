#include "gui/gui_state.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "pdf/pdf_export.hpp"
#include "search/fold.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kCrlf = "\r\n";
const char kStateFileName[] = "gui-state.toml";
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
    sync_note_exe = sync::note_executable_beside_self();
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
    layout_.folder_pane_height = pane_side("folder_pane_height", layout_.folder_pane_height);

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
    table.insert("folder_pane_height", static_cast<double>(layout_.folder_pane_height));
    if (const auto path = current_path())
        table.insert("last_note", path->string());
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
    close_trash();  // süzgeç canlı listeye aittir: yazılanın etkisi görünsün
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
    if (!content_mode_ || query_.empty()) {
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
    model_.set_content_results(content_index_->search(query_, kContentResultLimit));
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
    editor_selection_ = first_match(buffer_, query_);
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
    // nvALT: eşleşme yoksa Enter, sorguyu ilk satır yaparak yeni not açar.
    if (current_path() || new_note_)
        request_focus(FocusRequest::Editor);
    else
        new_note(query_);
}

void GuiState::set_folder(std::string folder) {
    close_trash();
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
        // Seçili klasör silindiyse köke düşer.
        if (!current_folder_.empty() && std::ranges::find(folders_, current_folder_) == folders_.end())
            current_folder_.clear();
    }
    reload_trash();
    auto notes = store_.list();
    apply_filter([&] {
        model_.set_folder(current_folder_);
        model_.set_notes(std::move(notes));
        update_content_results();
    });
}

void GuiState::apply_filter(const std::function<void()>& change) {
    // Editördeki not esastır (seçim, başarısız bir kayıttan sonra ondan ayrışmış olabilir).
    const auto previous = shown_ ? std::optional<core::NotePath>(shown_->path) : current_path();
    change();
    // TUI ile aynı: önceki not hâlâ listedeyse seçili kalır, yoksa ilk satır. Yeni not yazılırken seçim boş
    // kalır: ilk satırı seçmek taslağı ezerdi.
    int target = previous ? model_.index_of(*previous) : -1;
    if (target < 0 && model_.row_count() > 0 && !new_note_)
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
        if (!leave_shown_note(target))
            return;
    }
    if (current_path())
        show_note(model_.note_at(current_row_));
    else
        clear_editor();
}

void GuiState::clear_editor() {
    shown_.reset();
    set_editor({}, false);
}

void GuiState::show_note(core::NoteInfo info) {  // kopya: yükleme shown_'ı yeniden atar
    // Aynı not, aynı mtime: editör içeriğine/kaydırmasına dokunma (rescan sonrası kararlılık). Kirli tampon
    // varken dışarıdaki değişiklik tamponu ezmez; kayıt anında çakışma diyaloğu çıkar.
    if (shown_ && shown_->path == info.path && (shown_->revision == info.revision || dirty_))
        return;
    shown_ = info;
    binary_ = false;
    if (const auto loaded = load_text(info); !loaded)
        set_editor(core::tr("gui.read_failed", {{"detail", core::format_error(loaded.error())}}), false);
}

// Editörü doldurur; dirty/autosave durumunu sıfırlar.
void GuiState::set_editor(std::string content, bool editable) {
    autosave_at_.reset();
    selection_key_.clear();
    editor_selection_.reset();  // eski metnin ofsetleri yeni içerikte anlamsız
    buffer_ = std::move(content);
    ++buffer_generation_;
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
    return shown_->path.string() + "  ·  " + core::tr(dirty_ ? "gui.state_modified" : "gui.state_saved");
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
    if (!editable_ || !(shown_ || new_note_))
        return;
    dirty_ = true;
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
    if (!flush())
        return;
    close_trash();
    new_note_ = true;
    shown_.reset();
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
    const auto written = store_.write(shown_->path, body(true), shown_->revision);
    if (!written)
        return std::unexpected(written.error());
    ++writes_;
    changed();
    shown_->revision = *written;
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
        shown_.reset();
        return save_new();
    default:
        report(written.error());
        return false;
    }
}

bool GuiState::resolve_conflict() {
    switch (deciders.conflict(shown_->path.string())) {
    case ConflictChoice::Reload: {
        const core::NoteInfo info = *shown_;  // load_text shown_'ı yeniden atar
        const auto loaded = load_text(info);
        if (!loaded) {
            report(loaded.error());
            return false;
        }
        return true;
    }
    case ConflictChoice::Overwrite: {
        // Diskteki güncel mtime alınır; kullanıcı bilerek ezdi.
        const auto fresh = store_.read(shown_->path);
        if (!fresh) {
            report(fresh.error());
            return false;
        }
        shown_->revision = fresh->revision;
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
    const PathParts parts = split(shown_->path);
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
                       current_folder_, suggestion.type};
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
    if (!confirm_close_settings() ||
        !confirm_close())  // vazgeçildi ya da kayıt başarısız: pencere ve tampon yerinde kalır
        return false;
    save_gui_state();
    // Kayıt akışından sonra: bekleyen değişiklik için son senkron, en çok exit_timeout_seconds.
    stop_sync(std::chrono::seconds(config_.tuning.exit_timeout_seconds));
    return true;
}

SaveDialogRequest GuiState::move_request() const {
    const PathParts parts = split(shown_->path);
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
    if (!shown_ || !flush())
        return;
    if (const auto chosen = deciders.save_dialog(move_request()))
        move_current(*chosen);
}

void GuiState::change_type(const std::string& type) {
    if (!shown_ || new_note_ || type.empty())
        return;
    const PathParts parts = split(shown_->path);
    if (type == parts.type)
        return;
    if (flush())
        move_current({parts.stem, parts.folder, type});
}

// Tampon önceden kaydedilmiş olmalı (flush). Hedef varsa NoteStore::move AlreadyExists verir, hiçbir şey
// değişmez.
bool GuiState::move_current(const SaveDetails& target) {
    const auto to = core::NotePath::parse((target.folder.empty() ? "" : target.folder + "/") + target.name +
                                          "." + target.type);
    if (!to) {
        report(to.error());
        return false;
    }
    if (*to == shown_->path)
        return true;
    const auto moved = store_.move(shown_->path, *to);
    if (!moved) {
        report(moved.error());
        return false;
    }
    changed();
    shown_->path = *moved;
    if (const auto fresh = store_.read(*moved))  // taşıma mtime'ı korur; yine de güncel değeri al
        shown_->revision = fresh->revision;
    status_.clear();
    rescan();
    ensure_visible(*moved);
    return true;
}

void GuiState::move_note() {
    if (new_note_ || !shown_ || !flush() || !shown_)
        return;
    const PathParts parts = split(shown_->path);
    SaveDialogRequest request = move_request();
    request.folder_only = true;
    // Ad ve tip kilitli: karar ne dönerse dönsün yalnız klasör alınır.
    if (const auto chosen = deciders.save_dialog(request))
        move_current({parts.stem, chosen->folder, parts.type});
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
    if (const auto removed = store_.remove(*path); !removed) {
        fail(removed.error());
        return;
    }
    changed();
    select_after_removal(shown, row);
}

void GuiState::select_after_removal(bool was_shown, int row) {
    status_.clear();
    if (was_shown)
        clear_editor();
    rescan();
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
    const auto trashed = store_.trash(*path);
    if (!trashed) {
        fail(trashed.error());
        return;
    }
    changed();
    select_after_removal(shown, row);
    undo_item_ = trashed->path;
    undo_status_ = core::tr("gui.trashed", {{"path", path->string()}});
    status_ = undo_status_;
}

void GuiState::undo_trash() {
    if (!can_undo_trash())
        return;
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
    if (trash_view_)
        purge_trashed();
    else if (permanent)
        delete_note();
    else
        trash_note();
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
        if (const auto moved = core::NotePath::parse(target + shown_->path.string().substr(folder.size()))) {
            shown_->path = *moved;
            if (const auto fresh = store_.read(*moved))
                shown_->revision = fresh->revision;
        }
    }
    if (current_folder_ == folder || is_inside(current_folder_, folder))
        current_folder_ = target + current_folder_.substr(folder.size());
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
    if (shown_in(folder))
        clear_editor();
    rescan();
}

void GuiState::export_pdf() {
    if (shown_ && binary_)
        return;
    // Önce kaydet: PDF diskteki sürümden üretilir ve tampondaki düzenleme kaybolmaz. Kayıt başarısızsa ya da
    // vazgeçildiyse (tampon korunur) dışa aktarma da yapılmaz.
    if (unsaved() && !flush())
        return;
    if (!shown_)  // gösterilen not yok ya da boş taslak atıldı
        return;
    const auto output = deciders.export_path(split(shown_->path).stem + ".pdf");
    if (!output)
        return;
    const auto content = store_.read(shown_->path);
    if (!content) {
        fail(content.error());
        return;
    }
    const auto exported =
        pdf::export_pdf(content->text, pdf::format_for(shown_->path.value()), path_from_utf8(*output));
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
        shown_.reset();
        current_folder_.clear();
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

}  // namespace noteapp::gui
