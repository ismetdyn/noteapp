#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"
#include "tui/editor_buffer.hpp"
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

// Terminal kütüphanesinden bağımsız tuş soyutlaması: FTXUI olayları main.cpp'de buna çevrilir, böylece
// model hiçbir UI başlığına bağlanmadan test edilir.
enum class Key {
    Char,  // yazdırılabilir karakter; metni handle()'a ayrıca verilir
    Backspace,
    Up,
    Down,
    CtrlP,
    CtrlN,
    PageUp,
    PageDown,
    Tab,
    ShiftTab,
    Left,
    Right,
    Enter,
    Escape,
    CtrlO,
    CtrlR,
    CtrlD,
    CtrlS,
    F1,
    F2,
    CtrlC,
    // Yalnız gömülü editörün kullandığı tuşlar; Liste ve Arama bunları yok sayar.
    Home,
    End,
    Delete,
    ShiftUp,
    ShiftDown,
    ShiftLeft,
    ShiftRight,
    ShiftHome,
    ShiftEnd,
    CtrlHome,
    CtrlEnd,
    CtrlX,
    CtrlV,
    CtrlY,
    CtrlZ,
    CtrlF,
    F3,
    CtrlQ,
    CtrlSpace,
    AltLeft,   // satır üst verisini kapatır
    AltRight,  // satır üst verisini açar
    F9,
    CtrlU,
};

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
};

// Listedeki bir satır. Ad modunda `positions` (yol içindeki bayt ofsetleri) vurgu içindir; içerik modunda
// `hit_count`/`hits` doludur.
struct Row {
    core::NotePath path;
    std::vector<std::size_t> positions = {};
    std::size_t hit_count = 0;
    std::vector<search::Hit> hits = {};
    bool show_best_line = false;  // `f`: içerik eşleşmesinin en iyi satırı yolun altında
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
    // Pencerenin `line`. görsel satırındaki not satırı; liste bittiyse rows().size().
    std::size_t row_at(std::size_t visible, std::size_t line) const;
    // Pencerenin `line`. görsel satırı bir notun ilk satırı mı (ayrıntı değil): işaret yalnız orada.
    bool line_starts_row(std::size_t visible, std::size_t line) const;
    void toggle_expanded(std::size_t row);
    static constexpr std::size_t kMetaLines = 5;

    const search::SortSpec& list_sort() const { return prefs_.sort; }
    ListColumn list_column() const { return prefs_.column; }
    const ListPrefs& list_prefs() const { return prefs_; }
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
    std::size_t editor_start_line() const { return focus_ == Focus::Preview ? preview_scroll_ : 0; }
    // Önizleme metni (PreviewCache'e ait; önbellekten uzun yaşamaz). Üretim değişince not içi eşleşmeler
    // yenilenir.
    void set_preview_lines(const std::vector<std::string>* lines, std::size_t generation);

    // Fare.
    void scroll_preview(int delta);
    void move_selection(int delta);
    void click_row(std::size_t row);
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

    const std::string& query() const { return query_; }
    Mode mode() const { return mode_; }
    Focus focus() const { return focus_; }
    const std::vector<Row>& rows() const { return rows_; }
    std::size_t total() const { return trash_view_ ? trash_metas_.size() : notes_.size(); }
    std::size_t selected() const { return selected_; }
    const Row* selected_row() const { return rows_.empty() ? nullptr : &rows_[selected_]; }
    // Önizlemenin ilk görünen satırı (0 tabanlı).
    std::size_t preview_scroll() const { return preview_scroll_; }

private:
    Intent handle_trash(Key key, std::string_view text);
    Intent handle_trash_letter(std::string_view text);
    Intent intent_if_selected(Intent intent) const;
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
    Intent handle_preview(Key key, std::string_view text);
    Intent handle_preview_letter(std::string_view text);
    Intent handle_preview_find(Key key, std::string_view text);
    bool handle_layout_letter(std::string_view text);
    void leave_preview();
    void scroll_to_find_match();
    Intent handle_search(Key key, std::string_view text);
    void refilter(bool query_changed);
    std::size_t max_preview_scroll() const;

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
    ListPrefs prefs_;
    std::string query_;
    Mode mode_ = Mode::Name;
    Focus focus_ = Focus::List;
    std::vector<Row> rows_;
    std::size_t selected_ = 0;
    std::size_t preview_scroll_ = 0;
    std::size_t preview_lines_ = static_cast<std::size_t>(-1);  // bilinmiyor: üst sınır yok
    const std::vector<std::string>* preview_text_ = nullptr;
    std::size_t preview_generation_ = 0;
    PreviewFind preview_find_;
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
    int window_width_ = 80;
    int window_height_ = 24;
};

}  // namespace noteapp::tui
