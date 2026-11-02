#pragma once

#include "core/file_icons.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"
#include "tui/editor_buffer.hpp"
#include "tui/folder_tree.hpp"
#include "tui/input_line.hpp"
#include "tui/key.hpp"
#include "tui/note_find.hpp"
#include "tui/pane_layout.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Modelin dış dünyadan istediği iş; yan etkileri (editör, diyalog, çıkış) çağıran yürütür.
enum class Intent {
    None,
    Quit,
    OpenSelected,     // harici editör (`E`)
    CreateFromQuery,  // eşleşme yokken Enter: sorgu yeni notun ilk satırı olur
    NewNote,
    NewRow,        // `a`: listenin başında ad satırı
    MoveSelected,  // Ctrl-R: ad/klasör/tip diyaloğu
    DeleteSelected,
    SyncNow,
    ShowHelp,
    ShowSettings,
    EnterEditor,      // `e`/Enter: seçili not sağ panelde düzenlenir
    EditorKey,        // Editör odağı: tuş EditorSession'a gider
    CopyNote,         // `c`: notun özgün baytları sistem panosuna
    TrashSelected,    // `d`: onaysız çöpe; `u` geri alır
    RestoreLast,      // `u`: bu oturumda son çöpe atılan
    RestoreSelected,  // çöp görünümünde `u`
    EmptyTrash,       // çöp görünümünde `E`: onaylı
    RenameSelected,   // `F2`: altta tek satırlık yol girdisi
    ToggleTrash,      // `T`, çöp görünümünde `q` `Esc`
    SavePrefs,        // sıralama ya da sağ sütun değişti
    Paste,            // `p`: bekleyen yank geçerli klasöre
    MoveTo,           // `m`: altta hedef klasör girdisi
};

// Çoklu seçimin ya da yank'in bir öğesi.
struct ListItem {
    core::NotePath path;
    bool is_folder = false;
};

// `y`: `p`ye dek bekler; kaynaklar o anda okunur, kaybolan öğe yapıştırmada raporlanır.
struct PendingYank {
    std::vector<ListItem> items;
};

enum class Mode { Name, Content, Global };

// Tuşların kime gideceğini belirler: harfler aynı anda hem süzüp hem eylem yapamaz (SPEC-tui "Tuşlar").
enum class Focus { List, Search, Preview, PreviewFind, Editor, SortMenu };

// Listede sağ sütunun gösterdiği üst veri (`v`).
// Sıra `v` döngüsüdür.
enum class ListColumn { SizeAndDate, Size, Updated, Created, Sync, Origin, None };

// Kalıcı liste tercihleri; varsayılan: en yeni başta, boyut ve tarih.
struct ListPrefs {
    search::SortSpec sort{search::SortField::Updated, true};
    ListColumn column = ListColumn::SizeAndDate;
    std::vector<std::string> search_history = {};  // en yeni başta; yalnız yerel, senkrona girmez
    std::set<std::string> open_folders = {};       // ağaçta yerinde açık klasörler
};

// Listedeki bir satır. Ad modunda `positions` (yol içindeki bayt ofsetleri) vurgu içindir; içerik modunda
// `hit_count`/`hits` doludur.
struct Row {
    core::NotePath path;
    std::vector<std::size_t> positions = {};
    std::size_t hit_count = 0;
    std::vector<search::Hit> hits = {};
    bool show_best_line = false;  // `f`: içerik eşleşmesinin en iyi satırı yolun altında
    bool is_folder = false;       // klasör görünümü: `path` klasörün yoludur
    std::string label = {};       // klasör görünümünde gösterilen ad; boşsa tam yol
    std::size_t depth = 0;        // ağaçta girinti: geçerli klasörün çocukları 0
    std::size_t number = 0;       // gösterilen numara: kendi klasörü içinde, düz listede sıra

    std::string shown_text() const { return label.empty() ? path.string() : label; }
};

// REVIEW'da fare seçiminin bir ucu: `preview_lines` satırı ve o satırdaki hücrenin bayt aralığı. İşaretçinin
// altındaki karakter seçime girer: yön ne olursa olsun bir uç `begin`, öbürü `end` kullanır.
struct PreviewPoint {
    std::size_t line = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};
// Sıralı aralık: [begin_line:begin_offset, end_line:end_offset).
struct PreviewRange {
    std::size_t begin_line = 0;
    std::size_t begin_offset = 0;
    std::size_t end_line = 0;
    std::size_t end_offset = 0;
};

// İçerik araması modele dışarıdan verilir: ContentIndex'in G/Ç'si ve tembel kurulumu modelin işi değil.
using ContentSearcher = std::function<std::vector<search::ContentResult>(std::string_view query)>;
// `f`: ad ve içerik birlikte.
using GlobalSearcher = std::function<std::expected<std::vector<search::GlobalHit>, search::QueryError>(
    std::string_view query, std::span<const core::NoteMeta> notes)>;

// İki listeleme aynı notları (yol, mtime, boyut) aynı sırada mı içeriyor? Yeniden tarama "değişti mi" kararı
// için.
bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b);

// Sağ panelde mi çalışılıyor (yakınlaştırmada hangi panelin gösterileceği buna bağlı).
bool right_pane_focused(Focus focus);

// Ctrl-Z editörde geri almadır; başka odakta terminale kalır (askıya alma).
bool passes_to_terminal(Key key, Focus focus);
// Modal açıkken Ctrl-C çıkar; yardım editörün üstünde açıksa çıkmaz (kaydedilmemiş metin).
bool quits_from_modal(Key key, Focus focus);

class TuiState {
public:
    // Önizleme sayfa kaydırma adımı (satır).
    static constexpr std::size_t kPreviewPageLines = 10;
    // Panel dışındaki satırlar: üst çubuk, panel çerçevesi (2), toplamlar ya da editör durumu, kısayollar.
    static constexpr int kChromeLines = 5;
    // Panelin satır sayısı: `PgUp` `PgDn` adımı, REVIEW'da yarım sayfa ve hızlanma sınırı buna göredir.
    std::size_t page_rows() const;

    void set_notes(std::vector<core::NoteInfo> notes);
    void set_content_searcher(ContentSearcher searcher) { content_searcher_ = std::move(searcher); }
    void set_global_searcher(GlobalSearcher searcher) { global_searcher_ = std::move(searcher); }
    // İşçiden gelen senkron durumları (UI iş parçacığında): yeniden taramada da korunur.
    void set_sync_states(std::map<core::NotePath, core::SyncState> states);
    std::span<const core::NoteMeta> metas() const { return metas_; }
    // Boş klasörler de görünsün diye deponun klasör listesi; notlardan türeyenlerle birleşir.
    void set_folders(std::vector<std::string> folders);

    // Klasör görünümü: süzgeç ve çöp yokken liste geçerli klasörün çocuklarıdır (önce klasörler).
    bool in_folder_view() const { return !trash_view_ && query().empty() && mode_ != Mode::Global; }
    const std::string& current_folder() const { return current_folder_; }
    const FolderTree& folder_tree() const { return tree_; }
    void enter_folder(std::string folder);
    // Ata klasöre dönüşte çıkılan yolun çocuğu seçili kalır; kök üstü yoktur.
    void go_to_folder(std::string folder);
    // Seçimi klasöre taşır: klasörün üstüne gider, o satırı seçer. Süzgeç varsa temizlenir.
    bool select_folder(const std::string& folder);
    // `a` ve `F2` adayları klasöre göreli; kökte ve düz listede tam yol.
    SharedInputSources name_input_sources() const;
    // `a`, `F2`: yazılan ad bu klasöre göredir (düz listede kök).
    std::string name_base_folder() const { return in_folder_view() ? current_folder_ : std::string{}; }
    // Görünümdeki notun üst verisi (çöpte çöpün); yoksa nullptr.
    const core::NoteMeta* meta_of(const core::NotePath& path) const;
    // Sorgunun bozuk argümanı: metin sayılmaz, satırlar son geçerli haliyle kalır.
    const std::optional<search::QueryError>& query_error() const { return query_error_; }

    // Çöp görünümü (`T`): liste çöpü gösterir, yalnız gezinme, `u`, `D`, `E`, `T`, `q`, `Esc` çalışır.
    void enter_trash(std::vector<core::TrashItem> items);
    void set_trash(std::vector<core::TrashItem> items);
    void leave_trash();
    // Üst satırdaki gösterge için; değeri çağıran diskten okur (model G/Ç yapmaz).
    void set_trash_count(std::size_t count) { trash_count_ = count; }
    std::size_t trash_count() const { return trash_count_; }
    bool trash_view() const { return trash_view_; }

    // Satır yüksekliği: not + içerik eşleşmesinin en iyi satırı + açık üst veri satırları.
    std::size_t row_height(std::size_t row) const;
    bool is_expanded(const core::NotePath& path) const { return expanded_.contains(path); }
    bool is_expanded(const Row& row) const;
    // Pencerenin `line`. görsel satırındaki not satırı; liste bittiyse rows().size().
    std::size_t row_at(std::size_t visible, std::size_t line) const;
    // Pencerenin `line`. görsel satırı bir notun ilk satırı mı (ayrıntı değil): işaret yalnız orada.
    bool line_starts_row(std::size_t visible, std::size_t line) const;
    void toggle_expanded(std::size_t row);
    // Klasör satırını ağaçta yerinde açar/kapatır; açık küme tercihlerdedir (`tui-state.toml`).
    void toggle_tree(std::size_t row);
    bool is_folder_open(const std::string& folder) const { return prefs_.open_folders.contains(folder); }
    // Yazmadan önce: kalmayan klasörler dosyada birikmesin. Taramada değil, senkron geri getirebilir.
    void prune_open_folders();
    // Numara sütununun genişliği için: yeniden süzmede hesaplanır, kare başına satır yürünmez.
    std::size_t max_row_number() const { return max_row_number_; }
    // İş sayacı: kare başına görünene orantılı kalsın diye test eder, duvar saati yerine.
    struct ListWork {
        std::size_t rebuilds = 0;    // liste kaç kez kuruldu
        std::size_t row_visits = 0;  // çizim ve kaydırma kaç satıra baktı
    };
    const ListWork& list_work() const { return work_; }
    void note_row_drawn() const { ++work_.row_visits; }
    static constexpr std::size_t kMetaLines = 5;
    static constexpr std::size_t kFolderMetaLines = 4;

    const search::SortSpec& list_sort() const { return prefs_.sort; }
    ListColumn list_column() const { return prefs_.column; }
    const ListPrefs& list_prefs() const { return prefs_; }
    core::IconStyle icon_style() const { return icon_style_; }
    void set_icon_style(core::IconStyle style) { icon_style_ = style; }
    void set_list_prefs(const ListPrefs& prefs);
    // Önizlemenin satır sayısı: kaydırma ofseti son satırın ötesine geçemez.
    void set_preview_line_count(std::size_t lines);

    Intent handle(Key key, std::string_view text = {});
    // REVIEW'da tuş tekrarını ayırt eder; testler kendi saatini verir.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    void set_clock(Clock clock) { clock_ = std::move(clock); }

    // Sorguyu temizleyip notu seçer (yeni kaydedilen not süzgeçte kalmasın); yoksa false.
    bool reveal(const core::NotePath& path);

    // Seçimi nota taşır, odak değişmez (editörde kaydedilen not). Süzgeç notu gizliyorsa süzgeç temizlenir:
    // panelde düzenlenen not listede de görünsün. Yoksa false.
    bool select_note(const core::NotePath& path);

    void enter_editor() { focus_ = Focus::Editor; }
    // `n`: boş editör tüm terminali alır (panel yakınlaştırması); liste ve üst çubuk gizlenir. Editör
    // kapanınca normal iki panele Liste odağıyla dönülür.
    void open_new_note_view();
    bool new_note_view() const { return new_note_view_; }
    // Editör kapandı: satır varsa Önizleme o satırdan sürer, yoksa (kaydedilmemiş taslak, silinen not) Liste.
    void leave_editor(std::optional<std::size_t> preview_line);
    // `e`/Enter ile açılan editörün ilk satırı: Önizlemede görünen satır, Listede baş.
    // Biçimli görünümde kaydırma görüntü satırıdır: üstteki bloğun kaynak satırına çevrilir.
    std::size_t editor_start_line() const {
        return focus_ == Focus::Preview ? source_line_at(preview_scroll_) : 0;
    }
    // REVIEW'un görünümü (`R`, oturum boyunca): biçimli mi, ham mı.
    bool review_rendered() const { return review_rendered_; }
    // Gösterilen görünümün görüntü satırı -> kaynak satırı eşlemesi (ham metinde nullptr: ikisi aynıdır).
    // Üretim değiştiyse kaydırma aynı kaynak bloğunda kalacak şekilde çevrilir; önbellekten kopyalanır, ömrü
    // ona bağlanmaz.
    void set_preview_view(const std::vector<std::size_t>* source_lines, std::size_t generation);
    // Önizleme metni (PreviewCache'e ait; önbellekten uzun yaşamaz). Üretim değişince not içi eşleşmeler
    // yenilenir.
    void set_preview_lines(const std::vector<std::string>* lines, std::size_t generation);

    // Fare.
    void scroll_preview(int delta);
    void move_selection(int delta);
    void click_row(std::size_t row);
    // Tıklama sonrası: aynı klasör satırına kısa aralıkla ikinci tıklama içeri girer.
    void note_row_click(std::size_t row);
    void focus_preview();
    // Pencerenin ilk liste satırı: seçili satır görünür kalacak şekilde.
    std::size_t list_top(std::size_t visible) const;

    // REVIEW fare seçimi: uçlar mantıksal konumdur, sarma ya da kaydırma seçimi bozmaz.
    void begin_preview_selection(PreviewPoint point, int x, int y);
    void extend_preview_selection(PreviewPoint point, int x, int y);
    // Sürükleme bitti; işaretçi başka hücreye gittiyse true (kopyalanacak metin var).
    bool finish_preview_selection();
    void clear_preview_selection() { preview_selection_.reset(); }
    bool preview_selection_dragging() const { return preview_selection_ && preview_selection_->dragging; }
    int preview_selection_x() const { return preview_selection_ ? preview_selection_->x : 0; }
    int preview_selection_y() const { return preview_selection_ ? preview_selection_->y : 0; }
    std::optional<PreviewRange> preview_selection_range() const;
    // Seçimin `line` satırına düşen kısmı; yoksa nullopt.
    std::optional<ByteRange> preview_selection_on_line(std::size_t line, std::size_t line_size) const;
    const std::vector<std::string>* preview_text() const { return preview_text_; }

    // Son çizilen pencere: `<` `>` sınırları ve fare konumları buna göre hesaplanır.
    void set_window(int width, int height);
    int window_width() const { return window_width_; }
    int window_height() const { return window_height_; }
    PaneLayout& layout() { return layout_; }
    const PaneLayout& layout() const { return layout_; }
    const PreviewFind& preview_find() const { return preview_find_; }

    // Seçimi satır numarasına koyar (son satıra kenetlenir): silmeden sonra komşu satırda kalmak için.
    void select_index(std::size_t index);

    const std::string& query() const { return search_line_.text(); }
    const InputLine& search_line() const { return search_line_; }
    // Bellekteki notlardan aday listeleri; notlar değişince ilk istekte yeniden kurulur.
    const SharedInputSources& input_sources() const;
    std::string search_ghost() const { return search_line_.ghost(prefs_.search_history); }
    Mode mode() const { return mode_; }
    Focus focus() const { return focus_; }
    const std::vector<Row>& rows() const { return rows_; }
    std::size_t total() const { return trash_view_ ? trash_metas_.size() : notes_.size(); }
    std::size_t selected() const { return selected_; }
    // Seçili not; klasör satırında nullptr: not isteyen her eylem klasörde kendiliğinden etkisizdir.
    const Row* selected_row() const {
        return rows_.empty() || rows_[selected_].is_folder ? nullptr : &rows_[selected_];
    }
    const Row* selected_folder_row() const {
        return rows_.empty() || !rows_[selected_].is_folder ? nullptr : &rows_[selected_];
    }
    const Row* selected_entry() const { return rows_.empty() ? nullptr : &rows_[selected_]; }
    // Önizlemenin ilk görünen satırı (0 tabanlı).
    std::size_t preview_scroll() const { return preview_scroll_; }

    // Çoklu seçim (`x`): klasörler arasında kalır, yeniden taramada kaybolan öğe düşer. Çöpün ayrı kümesi
    // vardır: çöp ve canlı ağaç ayrı ad uzayıdır.
    bool is_marked(const Row& row) const { return marks().contains(row_key(row)); }
    std::size_t marked_count() const { return marks().size(); }
    void clear_marks() { active_marks().clear(); }
    // Eylemin öğeleri: seçim varsa o (seçili klasörün içindekiler tek sayılır), yoksa geçerli satır.
    std::vector<ListItem> action_items() const;
    // `y` ile bekleyen kopya.
    const std::optional<PendingYank>& yank() const { return yank_; }
    void clear_yank() { yank_.reset(); }
    // Liste `g` öneki: sıradaki tuşu bekliyor, alt satır ne beklediğini söyler.
    bool awaiting_prefix() const { return awaiting_prefix_; }
    // Toplu işlemin öğe öğe sonucu: küçük pencere, sıradaki tuş kapatır.
    const std::vector<std::string>& summary() const { return summary_; }
    void show_summary(std::vector<std::string> lines) { summary_ = std::move(lines); }

private:
    static std::string row_key(const Row& row);
    const std::set<std::string>& marks() const { return trash_view_ ? trash_marks_ : marks_; }
    std::set<std::string>& active_marks() { return trash_view_ ? trash_marks_ : marks_; }
    void prune_marks();
    void toggle_mark();
    void mark_all();
    void begin_yank();
    std::optional<Intent> handle_mark_letter(std::string_view text);
    Intent handle_prefix(Key key, std::string_view text);
    Intent handle_focused(Key key, std::string_view text);
    void escape_list();

    Intent handle_trash(Key key, std::string_view text);
    Intent handle_trash_letter(std::string_view text);
    Intent intent_if_selected(Intent intent) const;
    Intent intent_if_any_row(Intent intent) const;
    std::vector<Row> rows_in_folder();
    void append_folder_rows(const FolderNode& node, std::size_t depth, std::vector<Row>& rows) const;
    Row folder_row(const FolderNode& node) const;
    void rebuild_tree();
    bool open_selected_folder();
    void go_left();
    void select_key(const std::string& key);
    std::vector<Row>::const_iterator find_note_row(const core::NotePath& path) const;
    void set_selected_expanded(bool expanded);
    void toggle_all_expanded();
    bool handle_details_letter(std::string_view text);
    void scroll_accelerated(int direction);
    Intent handle_sort_menu(std::string_view text);
    bool filter_rows(std::vector<Row>& rows);
    std::vector<Row> rows_by_name(const search::ParsedQuery& parsed) const;
    std::vector<Row> rows_by_content(const search::ParsedQuery& parsed);
    std::vector<Row> rows_global();
    void sort_rows(std::vector<Row>& rows, const search::SortSpec& sort) const;
    void rebuild_metas();
    void begin_search(Mode mode);
    Intent search_enter();
    // Liste ve Arama'da ortak tuşlar; ortak olmayan tuşta nullopt.
    std::optional<Intent> handle_shared(Key key);
    Intent handle_list(Key key, std::string_view text);
    Intent handle_list_letter(std::string_view text);
    void handle_list_jump_letter(std::string_view text);
    Intent handle_preview(Key key, std::string_view text);
    Intent handle_preview_letter(std::string_view text);
    Intent handle_preview_find(Key key, std::string_view text);
    bool handle_layout_letter(std::string_view text);
    void leave_preview();
    void scroll_to_find_match();
    std::optional<Intent> handle_search_line(Key key, std::string_view text);
    Intent remember_query();
    void toggle_content_mode();
    Intent handle_search(Key key);
    void refilter(bool query_changed);
    void number_rows();
    std::size_t max_preview_scroll() const;
    std::size_t source_line_at(std::size_t display_line) const;
    std::size_t display_line_of(std::size_t source_line) const;
    // Kaydırma kaynak satırı olarak geldi (eşleşme, editör dönüşü): görüntü satırına sıradaki eşitlemede
    // iner.
    void scroll_to_source_line(std::size_t line);

    std::vector<core::NoteInfo> notes_;
    ContentSearcher content_searcher_;
    GlobalSearcher global_searcher_;
    std::vector<core::NoteMeta> metas_;  // notes_ ile aynı sırada
    std::map<core::NotePath, std::size_t> meta_index_;
    std::map<core::NotePath, core::SyncState> sync_states_;
    std::optional<search::QueryError> query_error_;
    bool query_has_arguments_ = false;
    std::vector<core::NoteMeta> trash_metas_;
    std::size_t trash_count_ = 0;
    bool trash_view_ = false;
    std::set<core::NotePath> expanded_;
    std::set<core::NotePath> expanded_folders_;  // aynı adlı not ve klasör karışmasın
    std::vector<std::string> folders_;
    FolderTree tree_;
    std::string current_folder_;
    bool showing_folders_ = true;
    std::optional<std::string> saved_spot_;  // düz listeye geçerken klasördeki seçim; dönüşte geri gelir
    mutable SharedInputSources relative_sources_;
    mutable std::string relative_sources_folder_;
    std::optional<std::size_t> last_click_row_;
    std::chrono::steady_clock::time_point last_click_at_{};
    ListPrefs prefs_;
    InputLine search_line_{InputKind::Search};
    mutable SharedInputSources input_sources_;
    core::IconStyle icon_style_ = core::IconStyle::Off;  // testler ve çizim için tarafsız varsayılan
    Mode mode_ = Mode::Name;
    Focus focus_ = Focus::List;
    std::vector<Row> rows_;
    std::size_t selected_ = 0;
    std::size_t preview_scroll_ = 0;
    std::size_t preview_lines_ = static_cast<std::size_t>(-1);  // bilinmiyor: üst sınır yok
    const std::vector<std::string>* preview_text_ = nullptr;
    std::size_t preview_generation_ = 0;
    PreviewFind preview_find_;
    bool review_rendered_ = true;
    std::vector<std::size_t> view_source_lines_;  // boş: ham metin
    std::size_t view_generation_ = 0;
    std::optional<std::size_t> pending_source_line_;
    Clock clock_ = [] { return std::chrono::steady_clock::now(); };
    std::chrono::steady_clock::time_point last_scroll_key_{};
    int scroll_direction_ = 0;
    int scroll_repeats_ = 0;
    struct PreviewSelection {
        PreviewPoint anchor;
        PreviewPoint head;
        int x = 0;
        int y = 0;
        bool dragging = true;
        bool moved = false;
    };
    std::optional<PreviewSelection> preview_selection_;
    PaneLayout layout_;
    bool new_note_view_ = false;
    std::set<std::string> marks_;
    std::set<std::string> trash_marks_;
    std::optional<PendingYank> yank_;
    bool awaiting_prefix_ = false;
    std::size_t max_row_number_ = 0;
    mutable ListWork work_;
    std::vector<std::string> summary_;
    int window_width_ = 80;
    int window_height_ = 24;
};

}  // namespace noteapp::tui
