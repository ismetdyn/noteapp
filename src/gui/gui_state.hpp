#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/file_icons.hpp"
#include "core/folder_tree.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "gui/browser_rows.hpp"
#include "gui/graph_layout.hpp"
#include "gui/note_list_model.hpp"
#include "gui/note_map.hpp"
#include "gui/save_dialog.hpp"
#include "gui/settings_page.hpp"
#include "sync/auto_sync.hpp"

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
#include <string>
#include <unordered_set>
#include <vector>

namespace noteapp::gui {

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
    // Arayüzü tutan iş (AutoSync durdurma, enable): üretimde bekleme imleciyle.
    std::function<void(const std::function<void()>& work)> while_busy =
        [](const std::function<void()>& work) { work(); };
};

enum class Tab { Notes, Settings };
enum class FocusRequest { None, Search, Editor, List };
enum class BrowserView { Tree, Details, Map };

// Editör imlecine programatik seçim (bayt ofseti): [start, end).
struct TextSelection {
    std::size_t start;
    std::size_t end;
};

// gui-state.toml: pencere yerleşimi. Config değildir; bozuksa varsayılana düşülür (plan davranış 7).
struct GuiLayout {
    int width = 1000;
    int height = 650;
    float left_pane_width = 320.0F;  // bölmeler ölçek 1 biriminde: ekran değişse de oran korunur
    float map_pane_width = 600.0F;   // Harita görünümünde sol bölme: harita dar ağaç bölmesine sığmaz
    BrowserView browser_view = BrowserView::Tree;
    bool row_numbers = false;  // Ağaç ve Ayrıntılar'da sıra no sütunu (Ayarlar)
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

    // Diski yeniden tarar; yalnız listeleme değiştiyse listeyi günceller, seçimi ve editörü korur.
    void rescan();
    // Süresi dolan zamanlayıcıları (autosave, rescan_seconds taraması) çalıştırır; bir iş yaptıysa true
    // (ekran yenilenmeli).
    bool tick(std::chrono::steady_clock::time_point now);
    // İşçi iş parçacığından UI iş parçacığına iş aktarır; tick çalıştırır. wake olay döngüsünü uyandırır
    // (SDL_PushEvent).
    void post(std::function<void()> work);
    void set_wake(std::function<void()> wake);
    // Bekleyen autosave'in anı; yoksa boş. Tarama buraya girmez: döngü onun aralığından sık uyanır.
    std::optional<std::chrono::steady_clock::time_point> next_deadline() const { return autosave_at_; }

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
    bool content_mode() const { return content_mode_; }
    void set_content_mode(bool on);
    void toggle_search_mode() { set_content_mode(!content_mode_); }  // mod düğmesi; arama kutusunda Tab

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
    bool is_expanded(const std::string& folder) const { return expanded_.contains(folder); }
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
    // Yeni not/klasörün yeri: seçili klasör, yoksa gösterilen notun klasörü, yoksa kapsam klasörü.
    std::string target_folder() const;

    // --- Not haritası (Harita görünümü, SPEC-gui "Harita") ---
    const MapOptions& map_options() const { return map_options_; }
    void set_map_options(const MapOptions& options);
    // Görünür harita ve yerleşimi; gerekirse kurulur. Depo yalnız taramada değişiklik görülünce okunur
    // (kenarlar tek okumada); kare başına okunmaz.
    const NoteMap& note_map();
    GraphLayout& map_layout();
    // Düğüm çizgesi her değiştiğinde artar (yalnız konum değişimi saymaz): çizim katmanı görünümü yeniden
    // sığdırır.
    std::size_t map_generation() {
        ensure_map();
        return map_generation_;
    }
    // Düğüm başına arama kutusunun süzgeci (listeyle aynı ayrıştırıcı, `tag:` dahil); sorgu boşken hepsi 1.
    const std::vector<char>& map_matches();
    // Düğüme tıklama: bekleyen düzenleme önce kendi notuna yazılır (flush_for), sonra not açılır (open_link).
    void open_map_note(const core::NotePath& path);
    std::size_t map_reads() const { return map_reads_; }  // test kancası: NoteStore::edges çağrısı

    // --- Editör ---
    const std::string& buffer() const { return buffer_; }
    std::string& buffer_for_edit() { return buffer_; }  // ImGui doğrudan yazar; ardından buffer_edited()
    void buffer_edited();
    void edit_text(std::string text);  // buffer_for_edit + buffer_edited
    std::size_t buffer_generation() const { return buffer_generation_; }
    bool editable() const { return editable_; }
    bool dirty() const { return dirty_; }
    bool is_new_note() const { return new_note_; }
    std::string header() const;
    // Tampondaki görünmez bidi/sıfır genişlikli karakter sayısı (core::count_hidden_controls); içerik
    // değişince güncellenir, kare başına taranmaz. Etiket başlıktaki uyarıdır, sayı 0 iken boş.
    std::size_t hidden_controls() const { return hidden_controls_; }
    std::string hidden_controls_label() const;
    std::string current_type() const;
    std::vector<std::string> type_choices() const;
    bool type_enabled() const { return shown_.has_value() && !new_note_; }
    // Editör etkinleşene dek bekleyen seçim (ImGui yalnız etkin alana seçim uygular); içerik değişince düşer.
    const std::optional<TextSelection>& editor_selection() const { return editor_selection_; }
    std::optional<TextSelection> take_editor_selection();

    // --- Bağlantılar (editör altı panel) ---
    const core::NoteLinks& links() const { return links_; }
    const std::vector<core::LinkLine>& link_lines() const { return link_lines_; }
    bool links_open() const { return links_open_; }
    void set_links_open(bool open) { links_open_ = open; }
    void toggle_links() { links_open_ = !links_open_; }  // Ctrl+L
    // Satıra tıklama: tampon kaydedilir, hedefi gizleyen süzgeç gevşer.
    void open_link(const core::NotePath& path);

    // --- Markdown önizlemesi ---
    // TUI'deki biçimli görünüm sınırı (SPEC-tui "REVIEW biçimli görünüm"): üstündeki not ham metinle okunur.
    static constexpr std::size_t kMaxPreviewBytes = std::size_t{256} * 1024;
    // Gösterilen kayıtlı not md mi (PDF dışa aktarmayla aynı kural); önizleme düğmesi yalnız onda görünür.
    bool previewable_type() const;
    bool can_preview() const;  // md, düzenlenebilir metin ve sınırın altında
    // Oturum boyunca tüm md notlarında (TUI'deki `R`); not önizlenemiyorsa editör görünür.
    bool showing_preview() const { return preview_ && can_preview(); }
    void toggle_preview();  // Ctrl+Shift+V; düzenlemeye dönünce odak editöre
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
    // (kept) ya da kaydedilmemiş taslaksa yeni sekmede açılır. Açık not yinelenmez, sekmesi öne gelir.
    static constexpr std::size_t kMaxTabs = 32;
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
    // Editör imleci: sekmeye dönüldüğünde geri konur.
    void remember_cursor(TextSelection cursor);

    // --- Eylemler (kısayollar) ---
    void focus_search();       // Ctrl+K / Ctrl+P
    void show_settings_tab();  // Ctrl+,
    void set_tab(Tab tab) { tab_ = tab; }
    Tab tab() const { return tab_; }
    void new_note(const std::string& first_line = {});  // Ctrl+N; nvALT: arama kutusunda Enter
    void save_now();                                    // Ctrl+S
    void save_as();                                     // Ctrl+Shift+S: ad/klasör/tip taşıma diyaloğu
    void move_note();                                   // Ctrl+M: yalnız klasör seçilir
    void delete_note();                                 // Shift+Del (listede): kalıcı, onaylı
    void trash_note();  // Del (listede), araç çubuğu Sil: onaysız, geri alınır
    void undo_trash();  // durum çubuğundaki Geri al
    // Yalnız çöpe atma iletisi durum çubuğundayken: başka ileti geldiyse teklif de gider.
    bool can_undo_trash() const { return undo_item_.has_value() && status_ == undo_status_; }
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
    bool can_move() const { return shown_.has_value() && !new_note_; }
    bool can_export() const;

    void request_focus(FocusRequest focus) { focus_request_ = focus; }
    FocusRequest take_focus_request();

    // --- Durum çubuğu ---
    const std::string& status() const { return status_; }
    std::string count_label() const;
    // Açılışta config yükleme uyarılarını gösterir (bozuk config.toml sessizce varsayılana düşmesin).
    void show_config_warnings(const std::vector<core::Error>& warnings);
    const std::string& sync_label() const { return sync_label_; }
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
    // Etkin olmayan sekmenin editör durumu; etkin sekmede boştur (canlı değerler üyelerdedir).
    struct EditorSnapshot {
        std::optional<core::NoteInfo> shown;
        std::string buffer;
        std::size_t generation = 0;
        std::string draft_folder;
        bool editable = false;
        bool dirty = false;
        bool new_note = false;
        bool crlf = false;
        bool bom = false;
        bool binary = false;
        bool preview = false;
    };
    struct NoteTab {
        std::uint64_t id = 0;
        bool pinned = false;
        bool kept = false;
        bool loaded = true;  // false: gui-state'ten geri yüklendi, içerik ilk etkinleşmede okunur
        std::optional<TextSelection> cursor;
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
    std::expected<void, core::Error> try_write();
    std::expected<void, core::Error> load_text(const core::NoteInfo& info);
    std::string body(bool crlf_aware) const;
    void set_editor(std::string content, bool editable);
    void ensure_visible(const core::NotePath& path);
    void restore_selection();
    void clear_query();
    void update_content_results();
    void update_editor_selection();
    bool shown_in(const std::string& folder) const;
    void report(const core::Error& error);
    void fail(const core::Error& error);
    SaveDialogRequest move_request(const core::NotePath& note) const;
    bool flush_for(const core::NotePath& note);
    bool flush_shown();
    // Kayıt yolu yalnız gösterilen not varken girer (save_now, flush, tick). İhlal bug'dır: durur.
    auto& shown_note(this auto& self) {
        if (!self.shown_)
            std::abort();
        return *self.shown_;
    }
    void ensure_listing();  // metas_ ve tree_ yalnız listeleme değiştiyse depodan
    void ensure_browser();
    void ensure_map();
    bool show_folder(const std::string& folder, bool scope);
    bool select_nearest_note(int from);
    void activate_row(int index);
    bool folder_action_allowed();
    void load_gui_state();
    void save_gui_state() const;
    bool run_posted();
    void start_sync();
    void stop_sync(std::chrono::seconds timeout);
    void changed();  // diskte not/klasör değişti: senkrona bildirilir
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
    struct SavedTab {  // gui-state.toml'daki [[tabs]] girdisi
        std::string path;
        bool pinned = false;
        bool kept = false;
    };
    bool restore_tabs(const std::vector<SavedTab>& saved, std::size_t active_index);

    core::Config config_;
    core::NoteStore store_;
    NoteListModel model_;
    std::vector<std::string> folders_;
    std::string current_folder_;
    std::optional<std::string> selected_folder_;  // doluyken gösterilen not ve taslak yoktur
    std::string draft_folder_;                    // yeni notun kayıt diyaloğunda önerilen klasör
    core::FolderTree tree_;
    std::vector<core::NoteMeta> metas_;
    std::vector<BrowserRow> browser_rows_;
    std::set<std::string> expanded_;  // yalnız oturum boyunca
    std::string built_folder_;        // Ayrıntılar satırlarının kurulduğu konum
    std::string revealed_note_;  // son açığa çıkarılan not: kullanıcının kapattığı klasör yeniden açılmaz
    // Harita: depodan okunan kenarlar (taramada değişiklik görülünce bayatlar) ve onlardan kurulan görünür
    // harita (seçenek ya da merkez değişince). Konumlar GraphLayout'ta anahtarla korunur.
    MapOptions map_options_;
    std::vector<core::NoteEdge> map_edges_;
    NoteMap map_;
    GraphLayout map_layout_;
    std::optional<core::NotePath> map_center_;
    std::vector<char> map_matches_;
    std::size_t map_matches_resets_ = 0;  // eşleşmelerin kurulduğu model_.resets()
    std::uint64_t data_version_ = 0;
    bool map_edges_stale_ = true;
    bool map_stale_ = true;
    bool map_matches_stale_ = true;
    std::size_t map_reads_ = 0;
    std::size_t map_generation_ = 0;
    bool listing_stale_ = true;
    bool rows_stale_ = true;
    std::size_t browser_rebuilds_ = 0;
    std::size_t max_row_number_ = 0;
    std::string query_;
    std::size_t query_generation_ = 0;
    bool content_mode_ = false;
    std::unique_ptr<search::ContentIndex> content_index_;  // ilk içerik aramasında kurulur
    std::string selection_key_;  // seçimin kurulduğu (not, sorgu): yedek tarama kullanıcının imlecini ezmesin
    int current_row_ = -1;
    std::vector<core::TrashItem> trash_items_;
    bool trash_view_ = false;
    int trash_row_ = -1;
    std::optional<core::NotePath> undo_item_;
    std::string undo_status_;
    std::optional<core::NoteInfo>
        shown_;  // editörde şu an görünen not (yol + mtime): gereksiz yüklemeyi önler
    std::string buffer_;
    std::size_t buffer_generation_ = 0;
    std::size_t hidden_controls_ = 0;
    core::NoteLinks links_;
    std::vector<core::LinkLine> link_lines_;
    bool links_open_ = true;
    std::vector<InfoField> note_info_;
    bool info_open_ = false;  // yalnız oturum boyunca; kapalı başlar, editöre yer kalsın
    bool preview_ = false;    // yalnız oturum boyunca
    bool editable_ = false;
    bool new_note_ = false;  // editörde henüz diske yazılmamış yeni not var (shown_ boş)
    bool dirty_ = false;     // editör, diskteki sürümden farklı
    bool in_show_ = false;   // show_current yeniden girişini engeller (kayıt akışı listeyi tazeler)
    bool crlf_ = false;      // dosya CRLF'liydi: kayıtta korunur
    bool bom_ = false;       // dosya UTF-8 BOM'luydu: tamponda görünmez, kayıtta korunur
    bool binary_ = false;    // gösterilen not ikili: editörde yer tutucu var
    std::size_t editor_generations_ = 0;  // buffer_generation_ değerleri: sekmeler arasında da benzersiz
    std::vector<NoteTab> tabs_;
    std::uint64_t active_tab_ = 0;
    std::uint64_t next_tab_id_ = 1;
    // Etkin sekmenin içeriği anlık görüntüsünde, editör boş (klasör seçili ya da süzgeç notu gizledi).
    bool parked_ = false;
    bool tabs_closed_ = false;  // kullanıcı son sekmeyi kapattı: süzgeç kendiliğinden ilk notu açmaz
    std::optional<std::chrono::steady_clock::time_point> autosave_at_;
    std::chrono::steady_clock::time_point rescan_at_;
    std::size_t writes_ = 0;
    std::size_t changes_ = 0;
    std::size_t sync_requests_ = 0;
    std::size_t sync_restarts_ = 0;
    std::size_t sync_generation_ = 0;  // durdurulan AutoSync'in kuyrukta kalan durumu yeni göstergeyi ezmesin
    std::string sync_label_;
    Tab tab_ = Tab::Notes;
    FocusRequest focus_request_ = FocusRequest::None;
    std::optional<TextSelection> editor_selection_;
    std::string status_;
    GuiLayout layout_;

    std::mutex posted_mutex_;
    std::vector<std::function<void()>> posted_;
    std::function<void()> wake_;
    // Sıra önemli: bu ikisinin işçileri post çağırır, yukarıdaki üyelerden önce yıkılmalı.
    SettingsPage settings_;
    std::unique_ptr<sync::AutoSync> auto_sync_;
};

}  // namespace noteapp::gui
