#pragma once

#include "app/commands.hpp"
#include "app/data_preview.hpp"
#include "app/document.hpp"
#include "app/import_files.hpp"
#include "app/item_ops.hpp"
#include "app/listing.hpp"
#include "app/note_map.hpp"
#include "app/notices.hpp"
#include "app/sync_status_worker.hpp"
#include "app/tabs.hpp"
#include "app/trash_ops.hpp"
#include "core/config.hpp"
#include "core/error.hpp"
#include "core/file_icons.hpp"
#include "core/folder_tree.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "edit/completion.hpp"
#include "edit/input_line.hpp"
#include "edit/language_servers.hpp"
#include "edit/spellcheck.hpp"
#include "gui/browser_rows.hpp"
#include "gui/note_list_model.hpp"
#include "gui/note_editor.hpp"
#include "gui/save_dialog.hpp"
#include "gui/settings_page.hpp"
#include "sync/auto_sync.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Haritanın modeli ve yerleşimi arayüzden bağımsızdır (src/app, TUI ile ortak); çizim view_map'tedir.
using app::GraphLayout;
using app::LayoutPoint;
using app::MapEdge;
using app::MapEdgeKind;
using app::MapNode;
using app::MapNodeKind;
using app::MapOptions;
using app::NoteMap;

// Kullanıcı kararları tek yerden geçer: üretimde view gerçek diyalogları kurar, testler değiştirir.
// Varsayılan hep "vazgeç": kurulmamış karar veri kaybettirmez.
struct Deciders {
    std::function<std::optional<SaveDetails>(const SaveDialogRequest&)> save_dialog =
        [](const SaveDialogRequest&) { return std::nullopt; };  // nullopt = vazgeçti
    std::function<ConflictChoice(const std::string& path)> conflict = [](const std::string&) {
        return ConflictChoice::Cancel;
    };
    std::function<CloseChoice()> close_unsaved = [] { return CloseChoice::Cancel; };
    // Ayarlar formunda kaydedilmemiş değişiklikle kapanış.
    std::function<CloseChoice()> close_unsaved_settings = [] { return CloseChoice::Cancel; };
    // Silme onayı: true = sil. Üretimde varsayılan düğme Vazgeç.
    std::function<bool(const std::string& message)> confirm_delete = [](const std::string&) { return false; };
    // Klasör adı istemi; nullopt = vazgeçti.
    std::function<std::optional<std::string>(const std::string& title, const std::string& initial)>
        folder_name = [](const std::string&, const std::string&) { return std::nullopt; };
    // PDF çıktı yolu; `suggested` önerilen dosya adı. nullopt = vazgeçti.
    std::function<std::optional<std::string>(const std::string& suggested)> export_path =
        [](const std::string&) { return std::nullopt; };
    std::function<std::optional<std::string>(const std::string& initial)> browse_dir =
        [](const std::string&) { return std::nullopt; };
    // Senkron uzağı uyuşmuyor: true = origin değiştirilir. Üretimde varsayılan düğme Vazgeç.
    std::function<bool(const std::string& message)> confirm_replace_origin = [](const std::string&) {
        return false;
    };
    std::function<void(const std::string& message)> info = [](const std::string&) {};
    // Komut paleti: seçilen komut kimliği; nullopt = vazgeçti.
    std::function<std::optional<std::string>()> palette = [] { return std::nullopt; };
    std::function<void()> help = [] {};
    // İlişkilendirilecek not (yol ya da [[hedef]] metni); nullopt = vazgeçti.
    std::function<std::optional<std::string>(const std::string& title)> link_target = [](const std::string&) {
        return std::nullopt;
    };
    // Sistem panosuna metin (TUI `c`); false = yazılamadı. Üretimde SDL panosu.
    std::function<bool(const std::string& text)> copy_text = [](const std::string&) { return false; };
    // Sistem panosundaki metin (editörde Ctrl+V); nullopt = pano boş ya da okunamadı: iç pano yapıştırılır.
    std::function<std::optional<std::string>()> paste_text = [] { return std::nullopt; };
    // Arayüzü tutan iş (AutoSync durdurma, enable): üretimde bekleme imleciyle.
    std::function<void(const std::function<void()>& work)> while_busy =
        [](const std::function<void()>& work) { work(); };
    // Bildirimdeki "Klasörü aç": sistemin dosya yöneticisi (SDL_OpenURL). false = açılamadı.
    std::function<bool(const std::string& folder)> open_folder = [](const std::string&) { return false; };
};

enum class Tab { Notes, Settings };
// Durum çubuğundaki senkron göstergesinin anlamı (etiketin rengi ve simgesi).
enum class SyncIndicator { Off, Running, Ok, Error };
enum class FocusRequest { None, Search, Editor, List, QuickAdd, Links, Preview };
enum class BrowserView { Tree, Details, Map };
// Ayrıntılar'ın üst veri sütunları (TUI'deki sağ sütun, `column` komutları). Sıra `column` döngüsüdür; kalıcı
// adlar TUI'ninkilerle aynıdır (kDetailsColumnNames).
enum class DetailsColumn { SizeAndDate, Size, Updated, Created, Sync, Origin, None };
inline constexpr std::pair<DetailsColumn, const char*> kDetailsColumnNames[] = {
    {DetailsColumn::SizeAndDate, "size_and_date"},
    {DetailsColumn::Size, "size"},
    {DetailsColumn::Updated, "updated"},
    {DetailsColumn::Created, "created"},
    {DetailsColumn::Sync, "sync"},
    {DetailsColumn::Origin, "origin"},
    {DetailsColumn::None, "none"},
};
// Ayarlar → Görünüm'deki yazı aileleri; gui-state.toml'da bu adlarla. İlki varsayılandır (gömülü DejaVu),
// TUI'de karşılığı yoktur (terminalin fontu).
inline constexpr const char* kUiFonts[] = {"dejavu", "inter"};
inline constexpr const char* kEditorFonts[] = {"dejavu-mono", "jetbrains-mono"};
// Notun senkron durumu, bilgi panelinde ve Ayrıntılar sütununda aynı metin.
std::string sync_state_text(core::SyncState state);
// Komut paletinin satırı (app::rank_palette sırasıyla).
struct PaletteRow {
    std::string_view id;
    const char* label_key;
    const char* blocked;  // çalışamıyorsa nedeni (i18n anahtarı); nullptr = çalışır
};
// Arama kutusunun seçicisi: değeri sorguya `tag:` ya da `view:` argümanı olarak yazar (TUI'de Tab listesi).
enum class Picker { None, Tag, View };

struct PickerItem {
    std::string value;  // argümanın değeri
    std::string label;  // listede görünen
};

// Editör seçimi (bayt ofseti): [start, end).
struct TextSelection {
    std::size_t start;
    std::size_t end;
};

// gui-state.toml: pencere yerleşimi. Config değildir; bozuksa varsayılana düşülür (plan davranış 7).
struct GuiLayout {
    // Sol bölmenin en darı (ölçek 1): sürükleme ve pane.narrow aynı sınırda durur.
    static constexpr float kMinLeftPaneWidth = 180.0F;
    int width = 1000;
    int height = 650;
    float left_pane_width = 320.0F;  // bölmeler ölçek 1 biriminde: ekran değişse de oran korunur
    float map_pane_width = 600.0F;   // Harita görünümünde sol bölme: harita dar ağaç bölmesine sığmaz
    BrowserView browser_view = BrowserView::Tree;
    bool row_numbers = false;  // Ağaç ve Ayrıntılar'da sıra no sütunu (Ayarlar)
    DetailsColumn column = DetailsColumn::SizeAndDate;
    std::string ui_font = kUiFonts[0];          // kUiFonts'tan biri
    std::string editor_font = kEditorFonts[0];  // kEditorFonts'tan biri
};

// Klasör satırı seçiliyken sağ panelin özeti; sayaçlar alt ağacı kapsar.
struct FolderSummary {
    std::string path;
    std::size_t notes{};
    std::size_t folders{};
};

// Sağ panelin not bilgisi satırı: etiket ve biçimlenmiş değer (çevrilmiş).
struct InfoField {
    std::string label;
    std::string value;
};

// Editör sekmesinin çizim özeti (SPEC-gui "Sekmeler").
struct TabSummary {
    std::uint64_t id = 0;
    std::string title;  // dosya adı; taslakta "Adsız"
    std::string path;   // ipucu; taslakta boş
    bool unsaved = false;
    bool pinned = false;
    bool kept = false;  // yeni açılan not bu sekmenin yerine geçmez (düzenlendi ya da yeni sekmede açıldı)
};

// Saf GUI modeli (ImGui/SDL bilmez): not listesi, seçim, tampon, kirli durum, kaydetme, çakışma. View her
// karede bunu çizer ve kullanıcı niyetini kare DIŞINDA buraya iletir; kararlar (Deciders) kare içinde iç içe
// modal açabildiğinden akışlar eşzamanlı yazılabilir.
class GuiState {
public:
    explicit GuiState(core::Config config);
    ~GuiState();

    Deciders deciders;
    std::filesystem::path sync_note_exe;  // arka plan senkronunu koşan `note`; testler değiştirir

    // Diski yeniden tarar; yalnız listeleme değiştiyse listeyi günceller, seçimi ve editörü korur. Görünen
    // bir şey değiştiyse true (ekran yenilenmeli).
    bool rescan();
    // Süresi dolan zamanlayıcıları (autosave, rescan_seconds taraması) çalıştırır; bir iş yaptıysa true
    // (ekran yenilenmeli).
    bool tick(std::chrono::steady_clock::time_point now);
    // İşçi iş parçacığından UI iş parçacığına iş aktarır; tick çalıştırır. wake olay döngüsünü uyandırır
    // (SDL_PushEvent).
    void post(std::function<void()> work);
    // post gibi ama olay döngüsünü uyandırmaz: iş bir sonraki tick'te (boşta en geç kIdleWaitMs) çalışır ve
    // yalnız true dönerse ekran yenilenir. Değişmeyebilen arka plan sonuçları için (senkron durumları).
    void post_quiet(std::function<bool()> work);
    void set_wake(std::function<void()> wake);
    // Bekleyen autosave'in anı; yoksa boş. Tarama buraya girmez: döngü onun aralığından sık uyanır.
    // Bildirimin düşme anı da bir uyanmadır (yoklama değil): boşta kare üretilmez.
    std::optional<std::chrono::steady_clock::time_point> next_deadline() const {
        const auto autosave = autosave_.deadline();
        const auto notice = notices_.next_deadline();
        if (!autosave || !notice)
            return autosave ? autosave : notice;
        return std::min(*autosave, *notice);
    }

    // --- Liste ve arama ---
    const NoteListModel& model() const { return model_; }
    int current_row() const { return current_row_; }
    std::optional<core::NotePath> current_path() const;
    void select_row(int row);
    void move_selection(int delta);
    const std::string& query() const { return query_; }
    // Sorgu argümanı geçersizse TUI'nin metni (format_error); liste son geçerli haliyle kalır.
    const std::optional<std::string>& query_error() const { return model_.query_error(); }
    std::string& query_buffer() { return query_; }  // ImGui doğrudan yazar; ardından query_edited()
    void query_edited();
    void set_query(std::string query);
    // Sorgu programatik değiştikçe artar: view InputText'in iç kopyasını bu sayede atar.
    std::size_t query_generation() const { return query_generation_; }
    void search_enter();  // arama kutusunda Enter
    // Ad (fuzzy), içerik ya da genel (ad ve içerik birlikte, en iyi satırla) arama; TUI ile aynı kural.
    app::SearchMode search_mode() const { return search_mode_; }
    bool content_mode() const { return search_mode() == app::SearchMode::Content; }
    void set_search_mode(app::SearchMode mode);
    void set_content_mode(bool on) { set_search_mode(on ? app::SearchMode::Content : app::SearchMode::Name); }
    // Mod düğmesi; arama kutusunda Tab. TUI gibi: addan içeriğe, içerikten ve genelden ada.
    void toggle_search_mode();
    void search_global();  // genel arama kutusu: Ctrl+Shift+F

    // --- Arama satırı: TUI'nin girdi satırı (tui::InputLine, SPEC-tui "Girdi satırı") ---
    // Metni ImGui düzenler (query_buffer); view her karede metni ve imleci bildirir. Tuş metni değiştirirse
    // yeni metin ve imleç take_search_edit'le metin kutusuna yazılır (kutu odakta kalır).
    struct SearchEdit {
        std::string text;
        std::size_t cursor = 0;
    };
    void sync_search_line(std::string_view text, std::size_t cursor);
    // Tab (tamamlama), Ctrl+P/Ctrl+N (geçmiş), → (gölgeyi al), açık listede ↑↓ Enter Esc Backspace ve yazı.
    // true = tuş satırındı; metin kutusuna ya da listeye gitmez.
    bool search_key(tui::Key key, std::string_view typed = {});
    std::optional<SearchEdit> take_search_edit() { return std::exchange(search_edit_, std::nullopt); }
    // Ctrl+P geçmişte geri gider; Ctrl+N yalnız geçmişte gezerken ileri gider (yazılan metne dönünce yürüyüş
    // biter), yoksa kutuda da yeni nottur. false = tuş geçmişin değil.
    bool search_history_key(bool older);
    bool search_walking() const { return search_walk_.has_value(); }
    std::string search_ghost() const { return search_line_.ghost(search_history_); }
    const tui::InputLine::Popup* search_popup() const { return search_line_.popup(); }
    void pick_search_completion(std::size_t index);  // fareyle: listedeki aday eklenir, odak kutuya
    void close_search_popup();
    // Enter'la kullanılan aramalar, en yeni başta; gui-state.toml'da (TUI'de tui-state.toml), senkrona
    // girmez. Sınır ve sır süzgeci TUI ile aynı: limits.tui.search_history, search_entry_bytes.
    const std::vector<std::string>& search_history() const { return search_history_; }

    // --- Etiket ve görünüm seçici (search.tag, search.view) ---
    // Seçici açılırken depodan okunur (etiketler sayılarıyla, kayıtlı görünümler adla).
    void open_picker(Picker kind);
    Picker picker() const { return picker_; }
    const std::vector<PickerItem>& picker_items() const { return picker_items_; }
    // Seçilen değer ad modunda `tag:DEĞER` ya da `view:DEĞER` sorgusu olur; seçici kapanır.
    void pick(const std::string& value);
    void close_picker();

    // --- Sıralama (TUI `o` menüsü; tercih gui-state.toml'da) ---
    // Ağaç, Ayrıntılar ve metinsiz arama bu sırayla; sorgudaki `sort:` onu ezer.
    const search::SortSpec& sort() const { return model_.listing().sort(); }
    void choose_sort(search::SortField field);  // app::sort_for: ad artan, öbürleri en büyük/en yeni başta
    void reverse_sort();
    void open_sort_menu() { sort_menu_requested_ = true; }
    bool take_sort_menu_request() { return std::exchange(sort_menu_requested_, false); }

    const std::string& current_folder() const { return current_folder_; }
    const std::vector<std::string>& folders() const { return folders_; }
    void set_folder(std::string folder);

    // --- Gezgin (ağaç ve ayrıntılar) ---
    // Arama ve çöp kendi düz listelerini gösterir; gezgin yalnız ikisi de kapalıyken görünür.
    bool browsing() const { return query_.empty() && !trash_view_; }
    BrowserView browser_view() const { return layout_.browser_view; }
    void set_browser_view(BrowserView view);
    bool row_numbers() const { return layout_.row_numbers; }
    // Ayarlar'daki onay kutusu: hemen uygulanır ve yerleşimle birlikte hemen yazılır (Kaydet beklemez).
    void set_row_numbers(bool on);
    // Görünüm'deki yazı aileleri: sıra no gibi hemen uygulanır ve yazılır; listede olmayan ad yok sayılır.
    void set_ui_font(const std::string& name);
    void set_editor_font(const std::string& name);
    // Satırlar yalnız listeleme, açık klasörler ya da konum değişince yeniden kurulur; kare başına depoya
    // gidilmez.
    const std::vector<BrowserRow>& browser_rows();
    int browser_row();                                                  // seçili satır; görünür değilse -1
    std::size_t browser_rebuilds() const { return browser_rebuilds_; }  // test kancası
    // Gezgin satırlarındaki en büyük sıra no (numara sütununun genişliği).
    std::size_t max_row_number() {
        ensure_browser();
        return max_row_number_;
    }
    const std::optional<std::string>& selected_folder() const { return selected_folder_; }
    std::optional<FolderSummary> folder_summary();
    bool is_expanded(const std::string& folder) const { return model_.listing().is_open(folder); }
    void set_expanded(const std::string& folder, bool expanded);
    void toggle_expanded(const std::string& folder) { set_expanded(folder, !is_expanded(folder)); }
    // Satır seçimi: not açılır; klasörde bekleyen düzenleme önce kaydedilir, sağ panel özet gösterir.
    void open_note(const core::NotePath& path);
    void select_folder(const std::string& folder);
    void enter_folder(const std::string& folder);  // Ayrıntılar konumu; Yukarı ve yol düğmeleri de buradan
    void navigate(int delta);                      // Yukarı/Aşağı: gezgin satırları, yoksa düz liste
    void browse_right();
    void browse_left();
    void browse_enter();
    // Kare dışında çağrılır: gösterilen not değiştiyse atalarını açar ya da Ayrıntılar'da klasörüne gider.
    void reveal_selection();
    // Yeni not/klasörün yeri: TUI ile ortak kural (app::new_note_folder).
    std::string target_folder() const;
    // Space (`toggle`): klasör satırı ağaçta yerinde açılır/kapanır, notta ve çöpte bilgi paneli.
    void toggle_row();
    void toggle_tree_all();     // tüm klasörler açıksa hepsini kapatır, değilse hepsini açar (Ağaç)
    void toggle_details_all();  // Ağaç ve Ayrıntılar arası (tüm satırların üst verisi); çöpte bilgi paneli
    void go_root();             // çöp ve arama kapanır, kök seçilir (TUI `gh`)
    // Ayrıntılar sütunları (`column`): seçim hemen gui-state.toml'a yazılır.
    DetailsColumn details_column() const { return layout_.column; }
    void set_details_column(DetailsColumn column);
    void cycle_details_column();

    // --- Çoklu seçim ve yank (TUI `x` `X` `Ctrl+A` `y` `p`) ---
    // Ağaç, Ayrıntılar, düz liste ve çöpün kendi kümesi vardır (çöpte yol, listede app::mark_key).
    bool is_marked(const std::string& path, bool is_folder) const;
    std::size_t marked_count() const { return active_marks().size(); }
    void toggle_mark();  // seçili satır; seçim bir aşağı iner
    void clear_marks() { active_marks().clear(); }
    void mark_all();  // görünen satırlar (ağacın kök satırı hariç)
    void yank();      // işaretliler, yoksa seçili satır; işaretler kalkar
    bool has_yank() const { return !yank_.empty(); }
    // Yank'i hedef klasöre kopyalar (app::paste_items): ad doluysa `-2`; kaynak yerinde kalır.
    void paste();

    // --- Dosya içe alma (`import`: dosya/klasör diyaloğu, sürükle-bırak; TUI paleti ve CLI aynı kural) ---
    // Yollar target_folder()'a app::import_paths'le işçide alınır (app::ImportJob; pencere beklemez):
    // ilerleme durum çubuğunda (post ile, boşta kare yok), Durdur düğmesiyle. Bitince özet durum çubuğunda,
    // ilk not sekme kuralıyla açılır (tutulan ya da kirli sekme kalır, not yeni sekmede). Biri sürerken
    // ikincisi reddedilir.
    void import_files(std::span<const std::filesystem::path> sources);
    void import_dialog_failed(const std::string& detail);  // SDL diyaloğu açamadı (SDL_GetError)
    bool importing() const { return import_.running(); }
    void cancel_import();  // alınanlar kalır; özet durdurulan girdi sayısını söyler
    // Komut ve menü yalnız ister; yerel diyaloğu View açar (SDL), seçilenler post ile import_files'a gelir.
    enum class ImportRequest { None, Files, Folders };
    void request_import(ImportRequest request) { import_request_ = request; }
    ImportRequest take_import_request() { return std::exchange(import_request_, ImportRequest::None); }

    // --- Hızlı ekleme (TUI `a`): listenin başında ad satırı; `/` ile biten ad klasör açar ---
    void begin_quick_add();
    bool quick_add_open() const { return quick_add_open_; }
    std::string& quick_add_buffer() { return quick_add_; }  // ImGui doğrudan yazar
    const std::string& quick_add_error() const { return quick_add_error_; }
    void submit_quick_add();  // Enter; hata satırda kalır
    void cancel_quick_add();  // Esc

    // --- Not haritası (Harita görünümü, SPEC-gui "Harita") ---
    const MapOptions& map_options() const { return map_.options(); }
    void set_map_options(const MapOptions& options);
    int map_max_depth() const { return map_.max_depth(); }  // limits.app.map_depth: adımlayıcının üstü
    // Görünür harita ve yerleşimi; gerekirse kurulur. Depo yalnız taramada değişiklik görülünce okunur
    // (kenarlar tek okumada); kare başına okunmaz.
    const NoteMap& note_map();
    GraphLayout& map_layout();
    // Düğüm çizgesi her değiştiğinde artar (yalnız konum değişimi saymaz): çizim katmanı görünümü yeniden
    // sığdırır.
    std::size_t map_generation() {
        ensure_map();
        return map_.generation();
    }
    // Düğüm başına arama kutusunun süzgeci (listeyle aynı ayrıştırıcı, `tag:` dahil); sorgu boşken hepsi 1.
    const std::vector<char>& map_matches();
    // Düğüme tıklama: bekleyen düzenleme önce kendi notuna yazılır (flush_for), sonra not açılır (open_link).
    void open_map_note(const core::NotePath& path);
    // İki düğüm arasında kenar kurar ya da kaldırır (MAP-R; kural app::map_relations, TUI ile ortak). Gövdeyi
    // yazan işlemden (bağlantı, etiket) önce o not gösteriliyorsa bekleyen düzenlemesi kaydedilir
    // (flush_for); kayıt olmazsa işlem yapılmaz. Sonuç ya da hata durum satırında; harita hemen yeni kenarı
    // gösterir.
    void connect_map_nodes(const MapNode& from, const MapNode& to, MapEdgeKind kind);
    void disconnect_map_nodes(const MapNode& from, const MapNode& to, MapEdgeKind kind);
    std::size_t map_reads() const { return map_.reads(); }  // test kancası: NoteStore::edges çağrısı

    // --- Editör ---
    // Etkin sekmenin editörü (EditorBuffer üstünde, TUI ile aynı düzenleme kuralları). Metni değiştiren her
    // şey aşağıdaki editor_* yollarından geçer: kirli durum, otomatik kayıt ve sekme koruması orada yürür.
    const NoteEditor& editor() const { return editor_; }
    const std::string& buffer() const { return editor_.text(); }
    // TUI tuş sözlüğüyle editör tuşu: hareket, düzenleme, geri al/yinele, bul; Ctrl+C/X/V panodan
    // (deciders.copy_text / paste_text, yedek iç pano).
    void editor_key(tui::Key key, std::string_view text = {});
    // Çizimden önce: metin alanının hücre ölçüsü (numara sütunu hariç); imleç görünür kalır.
    void editor_layout(int columns, int rows) { editor_.layout(columns, rows); }
    void editor_click(std::size_t visible_row, int cell, bool extend) { editor_.click(visible_row, cell, extend); }
    void editor_wheel(int rows) { editor_.wheel(rows); }
    void find_in_note();          // Ctrl+F
    void find_step(int delta);    // F3 / Shift+F3
    // Durum çubuğunun alanları (TUI bilgi satırıyla aynı): tip, kodlama, satır sonu, BOM, satır:sütun, satır
    // sayısı.
    std::string editor_fields() const;
    // Yazım denetimi (TUI ile aynı `hunspell -a`, yalnız md ve txt): satır içi yanlış sözcük aralıkları. Tuşu
    // bekletmez: bilinmeyen satır boş döner, sonuç gelince kare istenir. hunspell yoksa hep boş.
    std::vector<tui::ByteRange> misspelled(std::string_view line);
    // Dil sunucuları (TUI ile aynı tui::LanguageServers: varsayılan açık sunucular, sıkılaştırma, uzak dosya
    // başvurusu kuralı): her kareden önce. Belgeyi açar/kapatır, boşta değişikliği gönderir, tanıları ve
    // önerileri editöre koyar. Boşta bekleyen değişiklik varsa gönderileceği an: o andaki kare gönderir.
    std::optional<std::chrono::steady_clock::time_point>
    update_language_servers(std::chrono::steady_clock::time_point now);
    // Editörün tek seferlik iletisi (dil sunucusu durdu, bulunamadı); sonraki editör tuşu siler (TUI).
    const std::string& editor_notice() const { return editor_notice_; }
    // Tüm metin, tek geri alma adımı (testler ve programatik yazım); incelemedeyse önce düzenlemeye geçer.
    void edit_text(std::string_view text);
    std::size_t buffer_generation() const { return buffer_generation_; }
    bool editable() const { return editable_; }
    bool dirty() const { return dirty_; }
    bool is_new_note() const { return new_note_; }
    std::string header() const;
    // Tampondaki görünmez bidi/sıfır genişlikli karakter sayısı (core::count_hidden_controls); içerik
    // değişince güncellenir, kare başına taranmaz. Etiket başlıktaki uyarıdır, sayı 0 iken boş.
    std::size_t hidden_controls() const { return hidden_controls_; }
    std::string hidden_controls_label() const;
    // Notun uzantısı; kaydedilmemiş taslakta canlı detect (TUI ile aynı): renk, yazım ve durum çubuğu ilk
    // kayıttan önce de çalışır.
    std::string current_type() const;
    std::vector<std::string> type_choices() const;
    bool type_enabled() const { return shown_.has_value() && !new_note_; }
    // Editördeki seçim; içerik araması ilk eşleşmeyi buraya koyar, düzenleme kaldırır.
    std::optional<TextSelection> editor_selection() const;
    std::optional<TextSelection> take_editor_selection();  // seçimi kaldırır (imleç yerinde)

    // --- Bağlantılar (editör altı panel) ---
    const core::NoteLinks& links() const { return links_; }
    const std::vector<core::LinkLine>& link_lines() const { return link_lines_; }
    bool links_open() const { return links_open_; }
    void set_links_open(bool open) { links_open_ = open; }
    void toggle_links() { links_open_ = !links_open_; }  // Ctrl+L
    // Satıra tıklama: tampon kaydedilir, hedefi gizleyen süzgeç gevşer.
    void open_link(const core::NotePath& path);

    // --- Biçimli önizleme (md, csv, json) ---
    // Tür TUI REVIEW'la aynı kuraldan (app::preview_kind_for); önizleme düğmesi yalnız bu türlerde görünür.
    app::PreviewKind preview_kind() const;
    bool previewable_type() const { return preview_kind() != app::PreviewKind::None; }
    // Sınırlar TUI'nin biçimli görünümüyle aynı anahtarlar: limits.tui.view_bytes, view_lines, table_rows,
    // table_columns.
    const core::TuiLimits& view_limits() const { return config_.limits.tui; }
    bool can_preview() const;  // önizlenen tür ve düzenlenebilir metin
    // limits.tui.view_bytes'ın üstü: önizleme reddedilmez, TUI REVIEW gibi ham metin ve nedeni gösterilir.
    bool preview_too_large() const { return !view_limits().view_bytes.allows(buffer().size()); }
    // İnceleme (TUI REVIEW): listeden, haritadan ya da bağlantıdan açılan kayıtlı not salt okunur açılır; md,
    // csv ve json biçimli önizlemede, diğerleri kilitli editörde (renkli, imleç ve kopya var, yazı yok).
    // Düzenleme yalnız açık eylemle başlar (Düzenle düğmesi, `E`/Enter, çift tık, Ctrl+Shift+V); yeni not ve
    // taslak hep düzenlemededir. Kip sekmeyle saklanır.
    bool reviewing() const { return review_ && shown_.has_value() && !new_note_; }
    bool showing_preview() const { return reviewing() && can_preview(); }
    // İncelemedeki not düzenlenebilir mi (salt-okunur not, klasör ve çöp hayır).
    bool can_edit() const { return reviewing() && editable_ && !selected_folder_ && !trash_view_; }
    void begin_edit();  // odak editöre
    // Düzenlemeden incelemeye: bekleyen düzenleme kayıt kuralıyla yazılır (flush_shown); kayıt olmazsa
    // düzenlemede kalınır. Taslakta etkisiz (kaydedilene dek düzenlemededir).
    void end_edit();
    void toggle_preview();  // Ctrl+Shift+V: inceleme ↔ düzenleme
    // Önizlemedeki [[hedef]]: bekleyen düzenleme önce kendi notuna yazılır, hedef sonra çözülür.
    void open_wiki_link(const std::string& target, bool new_tab = false);
    // Önizlemedeki [[hedef]] canlı bir nota çözülüyor mu (çözülmeyen soluk, üstü çizili ve tıklanmaz).
    bool resolves_link(std::string_view target) const;

    // --- Not bilgisi (editör altı panel) ---
    // TUI'nin ayrıntı alanları (boyut, güncelleme, oluşturma, senkron, kaynak), tip ve etiketler. Gösterilen
    // kayıtlı not yoksa boş. Bağlantılarla aynı anlarda (gösterim, tarama, kayıt) tazelenir.
    const std::vector<InfoField>& note_info() const { return note_info_; }
    bool info_open() const { return info_open_; }
    void set_info_open(bool open) { info_open_ = open; }
    void toggle_info() { info_open_ = !info_open_; }  // Ctrl+I

    // --- Editör sekmeleri (SPEC-gui "Sekmeler") ---
    // Etkin sekmenin içeriği editör üyelerindedir (shown_, buffer_ ...); diğerleri kendi anlık görüntüsünü
    // tutar. Ağaçtan/aramadan/haritadan açılan not etkin sekmenin yerine geçer; sekme iğnelenmiş, korunuyor
    // (kept) ya da kaydedilmemiş taslaksa yeni sekmede açılır. Açık not yinelenmez, sekmesi öne gelir. Sekme
    // sayısı limits.app.tabs ile sınırlıdır (0 = sınırsız; kural app/tabs, TUI ile ortak).
    std::vector<TabSummary> tabs() const;
    std::uint64_t active_tab() const { return active_tab_; }  // 0 = sekme yok
    // Sekmeden ayrılırken bekleyen düzenleme kendi notuna yazılır, taslak sekmesinde kalır. Kayıt olmazsa
    // geçiş iptal: false. Sekme çubuğu ve kısayollar: odak editöre.
    bool activate_tab(std::uint64_t id);
    void next_tab(int delta);  // Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PageDown / Ctrl+PageUp
    void new_tab();            // Ctrl+T: yeni sekmede boş taslak
    void close_tab(std::uint64_t id);
    void close_active_tab();  // Ctrl+W
    void set_tab_pinned(std::uint64_t id, bool pinned);
    // Sekme çubuğunda sürükleyerek sıralama: verilen kimlik sırası (bilinmeyen atlanır, eksik sona).
    void reorder_tabs(const std::vector<std::uint64_t>& order);
    // Orta tık / Ctrl+tık / Ctrl+Enter. Not etkin sekmedeyse sekme korunur (sonraki açılış yeni sekmeye).
    void open_in_new_tab(const core::NotePath& path);

    // --- Eylemler (kısayollar) ---
    void focus_search();       // Ctrl+K / Ctrl+P; Ayarlar'dan çıkış sorusu Vazgeç'le biterse odak değişmez
    void show_settings_tab();
    void toggle_settings_tab();  // Ctrl+, ve palet: Notlar ↔ Ayarlar
    // Ayarlar'dan Notlar'a (←, Ctrl+, , arama): kaydedilmemiş form pencere kapanışının sorusunu sorar
    // (confirm_close_settings). false = Ayarlar'da kalındı.
    bool leave_settings();
    void set_tab(Tab tab) { tab_ = tab; }
    Tab tab() const { return tab_; }
    void new_note(const std::string& first_line = {});  // Ctrl+N; nvALT: arama kutusunda Enter
    void save_now();                                    // Ctrl+S
    void save_as();                                     // Ctrl+Shift+S: ad/klasör/tip taşıma diyaloğu
    void move_note();                                   // Ctrl+M: yalnız klasör seçilir
    // Ctrl+M ve araç çubuğu Taşı: işaretliler ya da seçili klasör satırı varsa öğeler (app::move_items),
    // yoksa gösterilen not (move_note).
    void move_selected();
    void delete_note();                                 // Shift+Del (listede): kalıcı, onaylı
    void trash_note();  // Del (listede), araç çubuğu Sil: onaysız, geri alınır
    void undo_trash();  // durum çubuğundaki Geri al
    // Yığında adım var ve çöp/geri alma iletisi durum çubuğundayken: başka ileti geldiyse teklif de gider.
    bool can_undo_trash() const { return trash_.can_undo() && status_ == undo_status_; }
    // Del, Shift+Del, araç çubuğu Sil: çöp görünümünde hepsi onaylı kalıcı silme.
    void delete_selected(bool permanent);

    // --- Çöp görünümü (liste panelinde) ---
    void open_trash();
    void close_trash() { trash_view_ = false; }
    void toggle_trash();
    bool trash_view() const { return trash_view_; }
    const std::vector<core::TrashItem>& trash_items() const { return trash_items_; }
    std::size_t trash_count() const { return trash_items_.size(); }
    int trash_row() const { return trash_row_; }
    void select_trash_row(int row);
    // Seçili çöp notunun metni (salt okunur önizleme); okunamazsa boş ve durum çubuğunda hata.
    const std::string& trash_preview() const { return trash_preview_; }
    void restore_trashed();
    void purge_trashed();                        // onaylı
    void empty_trash();                          // onaylı
    void change_type(const std::string& type);   // tip kutusu: uzantıyı değiştirir
    void new_folder(const std::string& parent);  // "" = kök; klasör bağlam menüsü
    void rename_folder(const std::string& folder);
    void delete_folder(const std::string& folder);
    void rename_selected();  // araç çubuğu: klasör satırında klasörü, not satırında notu
    void export_pdf();       // Ctrl+E
    void sync_now();         // F5
    // Araç çubuğu: eylem şu an bir iş yapar mı (yapmayacak düğme kapalı görünür).
    // Klasörde yalnız vurgulu satır: satırlar gerekirse kurulur, bu yüzden const değil.
    bool can_delete();
    bool can_rename();
    bool can_move();
    bool can_export() const;

    // --- Komut paleti, yardım ve diğer komutlar (G3) ---
    // Palet: kayıttaki her GUI komutu (app::kCommandRegistry sırası), app::rank_palette ile sıralı; son
    // kullanılanlar gui-state.toml'da, sınırı limits.app.recent_commands.
    void open_palette();  // Ctrl+Shift+P: deciders.palette seçilen kimliği döner
    std::vector<PaletteRow> palette_rows(std::string_view query);
    void run_palette_command(std::string_view id);  // çalışamıyorsa nedeni durum çubuğunda
    const std::vector<std::string>& recent_commands() const { return recent_commands_; }
    void show_help();             // F1
    void edit_selected();         // seçili not editörde, odak editöre
    void select_edge(bool last);  // Home / End: listenin ilk ya da son satırı
    void page(int direction);     // PageUp / PageDown: görünen satır kadar
    void set_page_rows(int rows) { page_rows_ = std::max(rows, 1); }  // view her karede ölçer
    // Önizleme kaydırma isteği (satır; ±büyük = başa/sona); view sonraki karede uygular.
    void scroll_preview(float lines) { preview_scroll_ += lines; }
    void scroll_preview_half(int direction);
    float take_preview_scroll() { return std::exchange(preview_scroll_, 0.0F); }
    void set_preview_rows(float rows) { preview_rows_ = rows; }
    void resize_pane(int direction);  // pane.narrow / pane.wide: sol bölme
    void toggle_zoom() { zoomed_ = !zoomed_; }
    bool zoomed() const { return zoomed_; }
    void toggle_icons();  // icons ayarı: config.toml'a yazılır (TUI ile aynı)
    void request_quit() { quit_requested_ = true; }
    bool take_quit_request() { return std::exchange(quit_requested_, false); }
    // İlişki (TUI `L`): hedef not sorulur, RELATED_TO kurulur; satırdaki ilişki onayla kaldırılır.
    void link_note();
    void unlink_note(const core::NotePath& other);
    void focus_links();
    // TUI `E`: seçili not config'deki dış editörde (core::run_editor). Bekleyen düzenleme önce yazılır;
    // editör işçide beklenir (pencere donmaz), not NoteStore::edit_in_temp_file ile çakışma denetimli geri
    // yazılır, dönüşte tarama gösterilen notu yeniden okur. Aynı anda tek editör; açıkken kapanış ertelenir.
    void open_external();
    bool external_running() const { return external_running_; }
    // TUI `c`: klasör satırında yol, notta özgün baytlar panoya (deciders.copy_text); ikili ve geçersiz UTF-8
    // reddedilir, nedeni durum çubuğunda.
    void copy_selected();
    // Verilen metni panoya yazar; sonucu (kopyalandı ya da yazılamadı, hata) durum çubuğunda söyler.
    void copy_to_clipboard(const std::string& text);

    void request_focus(FocusRequest focus) { focus_request_ = focus; }
    FocusRequest take_focus_request();

    // --- Bildirimler (toast; SPEC-gui "Bildirimler", model app::NoticeQueue, TUI ile ortak) ---
    // İşlem sonucu (dışa aktarma, içe alma, çöp, taşıma, senkron, ayar, bağlantı testi, harita ilişkisi) sağ
    // altta görünür; bilgi `tuning.notice_ms` sonra düşer (tick), hata kapatılana dek kalır.
    const app::NoticeQueue& notices() const { return notices_; }
    void dismiss_notice(std::uint64_t id) { notices_.dismiss(id); }
    void open_notice_folder(std::uint64_t id);  // dışa aktarmanın klasörü; açılamazsa hata bildirimi

    // --- Durum çubuğu ---
    const std::string& status() const { return status_; }
    // İleti bir hata mı (kayıt, işlem, içe alma, config ve depo uyarısı): durum çubuğunda danger rengi.
    bool status_is_error() const { return !status_.empty() && status_ == error_status_; }
    std::string count_label() const;
    // Açılışta config yükleme uyarılarını gösterir (bozuk config.toml sessizce varsayılana düşmesin).
    void show_config_warnings(const std::vector<core::Error>& warnings);
    // Açılışta okunan depo sorunu (geçiş gerekli ya da açılamıyor); liste üstündeki şerit bunu gösterir.
    const std::optional<core::Error>& store_issue() const { return store_issue_; }
    const std::string& sync_label() const { return sync_label_; }
    SyncIndicator sync_indicator() const { return sync_indicator_; }  // etiketin rengi ve simgesi
    // Koşu bitince (Running -> Ok/Error) sonuç bildirimi.
    void show_sync_status(const sync::SyncStatus& status, bool running);

    SettingsPage& settings() { return settings_; }
    // Pencerenin kendi ikon fontu var: terminaldeki font şartı burada yok.
    core::IconStyle icon_style() const {
        return config_.icons == "off" ? core::IconStyle::Off : core::IconStyle::Nerd;
    }

    // --- Yaşam döngüsü ---
    // Kaydedilmemiş iş çözülür ve durum yazılır; false = kapanış iptal, tampon yerinde.
    bool request_close();
    GuiLayout& layout() { return layout_; }

    // Test kancası: başarılı NoteStore::write sayısı (autosave birleşmesini ölçer).
    std::size_t write_count() const { return writes_; }
    std::size_t change_count() const { return changes_; }  // senkrona bildirilen değişiklik sayısı
    std::size_t sync_requests() const { return sync_requests_; }
    std::size_t sync_restarts() const { return sync_restarts_; }

private:
    // Diğer notların sözcükleri (gölge öneri, Ctrl+Space, bul tamamlaması); ilk editör kullanımında kurulur.
    std::shared_ptr<const tui::WordTable> word_table();
    // Etkin olmayan sekmenin editör durumu; etkin sekmede boştur (canlı değerler üyelerdedir).
    struct EditorSnapshot {
        std::optional<core::NoteInfo> shown;
        NoteEditor editor;  // metin, geri alma geçmişi, imleç ve seçim sekmeyle saklanır
        std::size_t generation = 0;
        std::string draft_folder;
        app::Document doc;
        bool editable = false;
        bool dirty = false;
        bool new_note = false;
        bool review = true;
    };
    struct NoteTab {
        std::uint64_t id = 0;
        bool pinned = false;
        bool kept = false;
        bool loaded = true;  // false: gui-state'ten geri yüklendi, içerik ilk etkinleşmede okunur
        EditorSnapshot saved;
    };

    // Süzgeç değişimini (sorgu/klasör/liste) uygular; seçimi yola göre korur, yoksa ilk satıra düşer.
    void apply_filter(const std::function<void()>& change);
    void show_current();
    void show_selected();
    bool leave_shown_note(const std::optional<core::NotePath>& target);
    void show_note(core::NoteInfo info);
    void clear_editor();
    void reload_links();
    void reload_info();
    void reload_trash();
    void select_after_removal(bool was_shown, int row, int browser_row);
    // Çöp işlemlerinin onayı deciders.confirm_delete'ten. Evet'ten sonra, depoya gitmeden önce `prepare`
    // koşar (açık notu kaydet); false dönerse işlem yapılmaz.
    app::Prompts trash_prompts(const std::function<bool()>& prepare = {});

    // Kaydetme akışları. true = akış tamam, mevcut notu bırakmak güvenli; false = tampon korunur.
    bool unsaved() const { return dirty_ || new_note_; }
    bool flush();
    bool save_existing();
    bool save_new();
    bool resolve_conflict();
    bool save_copy();
    bool move_saved(const core::NotePath& note, const SaveDetails& target);
    bool adopt_saved(const core::NotePath& path);
    bool confirm_close();
    bool confirm_close_settings();
    std::expected<app::Document::Saved, core::Error> try_write();
    void wrote();
    void autosave();
    std::expected<void, core::Error> load_text(const core::NoteInfo& info);
    void set_document_text(std::string text);
    bool binary() const { return doc_.read_only() == app::ReadOnly::Binary; }
    void set_editor(std::string content, bool editable);
    void lock_for_review() { editor_.set_writable(editable_ && !reviewing()); }
    EditorLimits editor_limits() const;
    void editor_changed(std::uint64_t edits_before);            // editor_key ve edit_text'in ortak sonu
    std::string detected_type(std::string_view content) const;  // config'in detect ayarlarıyla
    void refresh_draft_type();                                  // taslak düzenlendi (app::DraftTypeTracker)
    void copy_from_editor(std::string text);
    void paste_into_editor();
    void ensure_visible(const core::NotePath& path);
    void restore_selection();
    void clear_query();
    const search::ContentIndex& content_index();  // ilk içerik aramasında kurulur
    void update_editor_selection();
    bool shown_in(const std::string& folder) const;
    void report(const core::Error& error);
    void fail(const core::Error& error);
    void notify(app::NoticeKind kind, std::string text, std::optional<std::string> folder = std::nullopt);
    SaveDialogRequest move_request(const core::NotePath& note) const;
    bool flush_for(const core::NotePath& note);
    bool flush_shown();
    // Kayıt yolu yalnız gösterilen not varken girer (save_now, flush, tick). İhlal bug'dır: durur.
    auto& shown_note(this auto& self) {
        if (!self.shown_)
            std::abort();
        return *self.shown_;
    }
    void ensure_browser();
    // Çoklu seçim: çöpte çöp kümesi, listede liste kümesi.
    const std::set<std::string>& active_marks() const { return trash_view_ ? trash_marks_ : marks_; }
    std::set<std::string>& active_marks() { return trash_view_ ? trash_marks_ : marks_; }
    // Eylemin öğeleri: işaretliler, yoksa seçili satır (klasör ya da not).
    std::vector<app::ItemRef> action_items();
    std::vector<core::NotePath> trash_action_items() const;
    void prune_marks();  // taramadan sonra gitmiş öğelerin işaretleri düşer
    void publish(const app::BatchReport& report, const char* done_key);
    void delete_marked(bool permanent);
    void follow_moved(const std::string& from, const std::string& to);
    void load_trash_preview();
    const char* command_block(app::CommandNeeds needs);
    void set_sort(search::SortSpec sort);
    void request_sync_states();
    void show_sync_label(const sync::SyncStatus& status, bool running);
    bool apply_sync_states(app::SyncStates states);  // durumlar değiştiyse true
    void ensure_map();
    void edit_map(const MapNode& from, const MapNode& to, MapEdgeKind kind, bool connect);
    bool show_folder(const std::string& folder, bool scope);
    bool select_nearest_note(int from);
    void activate_row(int index);
    bool folder_action_allowed();
    void load_list_prefs();
    void load_gui_state();
    void save_gui_state() const;
    bool run_posted();
    void start_sync();
    void stop_sync(std::chrono::seconds timeout);
    void changed();  // diskte not/klasör değişti: senkrona bildirilir
    void external_done(const core::NotePath& path, const std::expected<bool, core::Error>& result);
    const tui::SharedInputSources& input_sources();  // notlar değişince ilk istekte kurulur
    tui::SearchHistoryLimits search_history_limits() const;
    void remember_query();
    void apply_settings(const core::Config& before, const core::Config& after);

    // Sekmeler. Kimlikle aranır: kayıt akışı tarar, tarama sekme kapatabilir; sıra numarası kayardı.
    NoteTab* find_tab(std::uint64_t id);
    const NoteTab* find_tab(std::uint64_t id) const;
    std::optional<core::NotePath> tab_note(const NoteTab& tab) const;
    std::optional<std::uint64_t> tab_of(const core::NotePath& path) const;
    bool keeps_active_tab() const;
    void claim_active_tab();  // canlı içerik etkin sekmeye yazılacak: sekme yoksa açılır
    bool leave_tab();
    void stash();
    void restore_tab(std::uint64_t id, bool focus_editor);
    bool switch_tab(std::uint64_t id, bool focus_editor);
    bool push_tab();
    void drop_active_tab();
    void erase_tab(std::uint64_t id);
    void park_or_clear();
    void clear_live();
    void close_missing_tabs(const std::unordered_set<std::string>& present);
    bool confirm_close_drafts();
    bool restore_tabs(const app::SavedTabs& saved);  // gui-state.toml'daki [[tabs]] ve active_tab

    core::Config config_;
    core::NoteStore store_;
    NoteListModel model_;
    std::vector<std::string> folders_;
    std::string current_folder_;
    std::optional<std::string> selected_folder_;  // doluyken gösterilen not ve taslak yoktur
    std::string draft_folder_;                    // yeni notun kayıt diyaloğunda önerilen klasör
    app::DraftTypeTracker draft_type_;            // taslağın tipi editörde (NoteEditor::type) durur
    std::size_t draft_size_ = 0;                  // son düzenlemedeki taslak metninin boyu
    std::vector<BrowserRow> browser_rows_;
    std::string built_folder_;        // Ayrıntılar satırlarının kurulduğu konum
    std::string revealed_note_;  // son açığa çıkarılan not: kullanıcının kapattığı klasör yeniden açılmaz
    // Harita: depodan okunan kenarlar (taramada değişiklik görülünce bayatlar) ve onlardan kurulan görünür
    // harita (seçenek ya da merkez değişince). Konumlar GraphLayout'ta anahtarla korunur. Model TUI ile
    // ortak.
    app::MapModel map_{config_.limits.app.map_nodes, config_.limits.app.map_depth};  // config_ önce kurulur
    std::uint64_t data_version_ = 0;
    bool rows_stale_ = true;
    std::size_t browser_rebuilds_ = 0;
    std::size_t max_row_number_ = 0;
    std::string query_;
    std::size_t query_generation_ = 0;
    tui::InputLine search_line_{tui::InputKind::Search};
    tui::SharedInputSources input_sources_;  // boş: notlar değişti, ilk istekte kurulur
    std::optional<SearchEdit> search_edit_;
    std::optional<std::string> search_walk_;  // geçmişte gezerken yürüyüşün başladığı (yazılan) metin
    std::vector<std::string> search_history_;
    app::SearchMode search_mode_ = app::SearchMode::Name;
    Picker picker_ = Picker::None;
    std::vector<PickerItem> picker_items_;
    bool sort_menu_requested_ = false;
    std::unique_ptr<search::ContentIndex> content_index_;  // ilk içerik aramasında kurulur
    std::string selection_key_;  // seçimin kurulduğu (not, sorgu): yedek tarama kullanıcının imlecini ezmesin
    int current_row_ = -1;
    std::vector<core::TrashItem> trash_items_;
    bool trash_view_ = false;
    int trash_row_ = -1;
    std::string trash_preview_;
    std::set<std::string> marks_;        // app::mark_key
    std::set<std::string> trash_marks_;  // çöp öğesi yolu
    std::vector<app::ItemRef> yank_;
    bool quick_add_open_ = false;
    std::string quick_add_;
    std::string quick_add_error_;
    std::vector<std::string> recent_commands_;  // palet: en yeni başta
    int page_rows_ = 1;
    float preview_rows_ = 1.0F;
    float preview_scroll_ = 0.0F;
    bool zoomed_ = false;
    bool quit_requested_ = false;
    app::TrashOps trash_{config_.limits.app.trash_undo_steps};  // çöp kuralı TUI ile ortak
    app::NoticeQueue notices_{app::NoticeQueue::lifetime_of(config_.tuning.notice_ms.value)};
    std::string undo_status_;
    // Editörde şu an görünen not (yol, liste ve sekme kimliği). Yazmanın beklediği revizyon belgededir
    // (doc_).
    std::optional<core::NoteInfo> shown_;
    app::Document doc_;  // etkin sekmenin belgesi: BOM, satır sonu, salt-okunur neden, revizyon
    NoteEditor editor_;
    std::string clipboard_;  // iç pano: sistem panosu okunamazsa Ctrl+V bunu yapıştırır (TUI ile aynı)
    std::size_t buffer_generation_ = 0;
    std::size_t hidden_controls_ = 0;
    core::NoteLinks links_;
    std::vector<core::LinkLine> link_lines_;
    bool links_open_ = true;
    std::vector<InfoField> note_info_;
    bool info_open_ = false;  // yalnız oturum boyunca; kapalı başlar, editöre yer kalsın
    bool review_ = true;      // REVIEW; düzenleme açık eylemle (reviewing)
    bool editable_ = false;
    bool new_note_ = false;  // editörde henüz diske yazılmamış yeni not var (shown_ boş)
    bool dirty_ = false;     // editör, diskteki sürümden farklı
    bool in_show_ = false;   // show_current yeniden girişini engeller (kayıt akışı listeyi tazeler)
    std::size_t editor_generations_ = 0;  // buffer_generation_ değerleri: sekmeler arasında da benzersiz
    std::vector<NoteTab> tabs_;
    std::uint64_t active_tab_ = 0;
    std::uint64_t next_tab_id_ = 1;
    // Etkin sekmenin içeriği anlık görüntüsünde, editör boş (klasör seçili ya da süzgeç notu gizledi).
    bool parked_ = false;
    bool tabs_closed_ = false;  // kullanıcı son sekmeyi kapattı: süzgeç kendiliğinden ilk notu açmaz
    app::Autosave autosave_{std::chrono::milliseconds(config_.tuning.autosave_ms)};
    std::chrono::steady_clock::time_point rescan_at_;
    std::size_t writes_ = 0;
    std::size_t changes_ = 0;
    std::size_t sync_requests_ = 0;
    std::size_t sync_restarts_ = 0;
    std::size_t sync_generation_ = 0;  // durdurulan AutoSync'in kuyrukta kalan durumu yeni göstergeyi ezmesin
    std::string sync_label_;
    SyncIndicator sync_indicator_ = SyncIndicator::Off;
    Tab tab_ = Tab::Notes;
    FocusRequest focus_request_ = FocusRequest::None;
    std::string status_;
    std::string error_status_;  // status_ bu metne eşitse ileti hatadır (danger); yeni ileti eşitliği bozar
    std::optional<core::Error> store_issue_;
    ImportRequest import_request_ = ImportRequest::None;
    GuiLayout layout_;

    std::mutex posted_mutex_;
    std::vector<std::function<bool()>> posted_;  // iş true dönerse ekran yenilenir
    std::function<void()> wake_;
    // Sıra önemli: bu üçünün işçileri post çağırır, yukarıdaki üyelerden önce yıkılmalı.
    app::SyncStatusWorker sync_status_;  // `sync:` süzgecinin not durumları, UI iş parçacığı dışında
    SettingsPage settings_;
    std::unique_ptr<sync::AutoSync> auto_sync_;
    bool external_running_ = false;
    // Dış editörü bekleyen işçi (kendi NoteStore bağlantısıyla); post çağırır, ilk yıkılır. Yıkım editörün
    // kapanmasını bekler: request_close editör açıkken kapanmaz.
    std::jthread external_;
    std::jthread stray_probe_;  // açılıştaki geçiş dışı dosya taraması (app::count_stray_files)
    app::ImportJob import_{[this](std::function<void()> work) { post(std::move(work)); }};  // post çağırır
    // Kendi depo bağlantısıyla arka planda okur; post çağırmaz, yine de UI üyelerinden önce yıkılır.
    static constexpr std::chrono::seconds kWordTableIdle{1};  // TUI ile aynı tempo
    tui::WordTableBuilder word_tables_{
        kWordTableIdle, tui::completion_limits_of(config_.limits.tui, config_.tuning.completion_min_prefix_bytes)};
    bool word_tables_enabled_ = false;
    // Sonuç gelince post eder (kare): posted_ ve wake_'ten önce yıkılmalı, bu yüzden en sonda.
    std::unique_ptr<tui::SpellChecker> spell_;
    std::string spell_dictionary_;
    std::string editor_notice_;
    // İstemcilerin işçileri yeniden çizim için post eder: en son kurulur, ilk yıkılır.
    tui::LanguageServers language_servers_{config_.lsp,
                                           tui::lsp_workspace_dir(),
                                           store_,
                                           [this] { post([] {}); },
                                           tui::LanguageServers::kChangeIdle,
                                           tui::LanguageServers::kMirrorIdle,
                                           tui::kBlockNetworkPaths,
                                           tui::lsp_limits_of(config_.limits.lsp)};
};

}  // namespace noteapp::gui
