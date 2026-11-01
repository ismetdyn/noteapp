#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "gui/note_list_model.hpp"
#include "gui/save_dialog.hpp"
#include "gui/settings_page.hpp"
#include "sync/auto_sync.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
    float folder_pane_height = 160.0F;
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
    std::string current_type() const;
    std::vector<std::string> type_choices() const;
    bool type_enabled() const { return shown_.has_value() && !new_note_; }
    // Editör etkinleşene dek bekleyen seçim (ImGui yalnız etkin alana seçim uygular); içerik değişince düşer.
    const std::optional<TextSelection>& editor_selection() const { return editor_selection_; }
    std::optional<TextSelection> take_editor_selection();

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
    void export_pdf();  // Ctrl+E
    void sync_now();    // F5
    // Araç çubuğu: eylem şu an bir iş yapar mı (yapmayacak düğme kapalı görünür).
    bool can_delete() const { return trash_view_ ? trash_row_ >= 0 : current_path().has_value(); }
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
    // Süzgeç değişimini (sorgu/klasör/liste) uygular; seçimi yola göre korur, yoksa ilk satıra düşer.
    void apply_filter(const std::function<void()>& change);
    void show_current();
    void show_selected();
    bool leave_shown_note(const std::optional<core::NotePath>& target);
    void show_note(core::NoteInfo info);
    void clear_editor();
    void reload_trash();
    void select_after_removal(bool was_shown, int row);

    // Kaydetme akışları. true = akış tamam, mevcut notu bırakmak güvenli; false = tampon korunur.
    bool unsaved() const { return dirty_ || new_note_; }
    bool flush();
    bool save_existing();
    bool save_new();
    bool resolve_conflict();
    bool save_copy();
    bool move_current(const SaveDetails& target);
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
    SaveDialogRequest move_request() const;
    void load_gui_state();
    void save_gui_state() const;
    bool run_posted();
    void start_sync();
    void stop_sync(std::chrono::seconds timeout);
    void changed();  // diskte not/klasör değişti: senkrona bildirilir
    void apply_settings(const core::Config& before, const core::Config& after);

    core::Config config_;
    core::NoteStore store_;
    NoteListModel model_;
    std::vector<std::string> folders_;
    std::string current_folder_;
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
    bool editable_ = false;
    bool new_note_ = false;  // editörde henüz diske yazılmamış yeni not var (shown_ boş)
    bool dirty_ = false;     // editör, diskteki sürümden farklı
    bool in_show_ = false;   // show_current yeniden girişini engeller (kayıt akışı listeyi tazeler)
    bool crlf_ = false;      // dosya CRLF'liydi: kayıtta korunur
    bool bom_ = false;       // dosya UTF-8 BOM'luydu: tamponda görünmez, kayıtta korunur
    bool binary_ = false;    // gösterilen not ikili: editörde yer tutucu var
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
