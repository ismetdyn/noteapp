#pragma once

#include "core/config.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "gui/note_list_model.hpp"
#include "gui/save_dialog.hpp"
#include "search/content_index.hpp"
#include "sync/sync_service.hpp"

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QString>
#include <QTimer>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QSplitter;
class QStandardItemModel;
class QTabWidget;
class QTreeView;

namespace noteapp::sync {
class AutoSync;
}

namespace noteapp::gui {

class SettingsPage;

// Kullanıcı kararları tek yerden geçer: üretimde gerçek diyalogları gösterir, testler değiştirir.
// (Kalıtım hiyerarşisi değil, yalnız bir test dikişi.)
struct Deciders {
    // nullopt = vazgeçti
    std::function<std::optional<SaveDetails>(const SaveDialogRequest&)> save_dialog;
    std::function<ConflictChoice(const std::string& path)> conflict;
    std::function<CloseChoice()> close_unsaved;
    // Klasör adı istemi: (başlık, başlangıç metni) -> girilen ad; nullopt = vazgeçti
    std::function<std::optional<std::string>(const QString& title, const QString& initial)> folder_name;
    // Silme onayı (not ve klasör): true = sil. Üretimde varsayılan düğme Vazgeç.
    std::function<bool(const QString& message)> confirm_delete;
    // PDF çıktı yolu istemi: (önerilen tam yol) -> seçilen yol; nullopt = vazgeçti
    std::function<std::optional<QString>(const QString& suggested)> export_path;
    // Ayarlar: notes_dir için klasör seçimi (başlangıç yolu) -> seçilen yol; nullopt = vazgeçti
    std::function<std::optional<QString>(const QString& initial)> browse_dir;
    // Senkron `origin` uyuşmazlığı: true = değiştir. Üretimde varsayılan düğme Vazgeç.
    std::function<bool(const QString& message)> confirm_replace_origin;
    // Bilgi bildirimi (ör. dil değişimi yeniden başlatınca uygulanır)
    std::function<void(const QString& message)> info;
};

// Ana pencere: Notlar (arama + klasör ağacı + liste | düzenlenebilir not görünümü) ve Ayarlar sekmeleri.
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(core::Config config);
    ~MainWindow() override;  // AutoSync tamamlanmamış tür olarak burada yıkılır

    Deciders deciders;
    // Senkronu koşacak `note` ikilisi; testler gerçek `note`'u gösterir. Senkron açılmadan önce ayarlanır.
    std::filesystem::path sync_note_exe;

    // Diski yeniden tarar; yalnız listeleme değiştiyse listeyi günceller, seçimi ve editörü korur.
    void rescan();
    // Kısayol eylemleri public: testler QShortcut yerine doğrudan çağırır.
    void focus_search();
    void show_settings_tab();
    void new_note(const QString& first_line = {});  // Ctrl+N; nvALT: arama kutusunda Enter
    void save_now();                                // Ctrl+S
    void save_as();                                 // Ctrl+Shift+S: ad/klasör/tip taşıma diyaloğu
    // Klasör ağacı bağlam menüsü eylemleri; yollar '/' ayırıcılı göreli klasör yolu ("" = kök).
    void new_folder(const std::string& parent);
    void rename_folder(const std::string& folder);
    void delete_folder(const std::string& folder);
    void delete_note();         // Del (listede): onaylı
    void export_pdf();          // Ctrl+E: gösterilen notu önce kaydeder, sonra PDF'e aktarır
    void move_note();           // Ctrl+M: yalnız klasör seçilir
    void toggle_search_mode();  // Ad <-> İçerik
    // F5: arka plan senkronunu hemen ister (senkron kapalıysa yalnız sayılır).
    void sync_now();
    // Durum çubuğu: kapalı / ⟳ / ✓ SS:DD / ⚠ hata. Arka plan çağrısı kuyruktan bu işleve gelir.
    void show_sync_status(const sync::SyncStatus& status, bool running);
    std::size_t sync_requests() const { return sync_requests_; }
    // Test kancası: Q_OBJECT'siz olduğundan findChild ile bulunamaz.
    SettingsPage* settings_page() const { return settings_; }
    // Test kancası: başarılı NoteStore::write sayısı (autosave birleşmesini ölçer).
    std::size_t write_count() const { return writes_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void build_ui();
    void rebuild_folders();
    void watch_directories();
    // Süzgeç değişimini (sorgu/klasör/liste) uygular; seçimi yola göre korur, yoksa ilk satıra düşer.
    void apply_filter(const std::function<void()>& change);
    std::optional<core::NotePath> current_path() const;
    void move_selection(int delta);
    void show_current();
    void show_selected();
    void update_status();
    // İçerik modunda sorguya göre model sonuçlarını ayarlar (ad modunda temizler); apply_filter içinden
    // çağrılır.
    void update_content_results();
    void apply_query();  // süzgeci güncel sorgu + moda göre yeniden uygular
    void set_content_mode(bool on);
    void update_highlights();
    void show_folder_menu(const QPoint& pos);
    void show_note_menu(const QPoint& pos);
    void fail(const core::Error& error);
    // Senkron: yalnız config_.sync.enabled iken AutoSync kurulur; her yazma/taşıma/silme bildirir.
    void start_sync();
    void stop_sync(std::chrono::seconds timeout);
    void changed();
    // Ayarlar kaydedildi: config_ güncellenir ve değişenler canlı uygulanır.
    void apply_settings(const core::Config& before, const core::Config& after);
    bool shown_in(const std::string& folder) const;
    void restore_state();
    void save_state() const;

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
    void change_type(const QString& type);
    std::expected<void, core::Error> try_write();
    std::expected<void, core::Error> load_text(const core::NoteInfo& info);
    std::string body(bool crlf_aware) const;
    void set_editor(const QString& content, bool editable);
    void update_header();
    void ensure_visible(const core::NotePath& path);
    void restore_selection();
    void report(const core::Error& error);

    core::Config config_;
    core::NoteStore store_;
    NoteListModel model_;
    std::vector<std::string> folders_;
    std::string current_folder_;
    std::optional<core::NoteInfo>
        shown_;              // editörde şu an görünen not (yol + mtime): gereksiz yeniden yüklemeyi önler
    bool suppress_ = false;  // süzgeç uygulanırken seçim sinyalleri yok sayılır
    bool new_note_ = false;  // editörde henüz diske yazılmamış yeni not var (shown_ boş)
    bool dirty_ = false;     // editör, diskteki sürümden farklı
    bool loading_ = false;   // editör programatik doldurulurken textChanged yok sayılır
    bool in_show_ = false;   // show_current yeniden girişini engeller (kayıt akışı listeyi tazeler)
    bool crlf_ = false;      // dosya CRLF'liydi: kayıtta korunur
    bool binary_ = false;    // gösterilen not ikili: editörde yer tutucu var, vurgulanmaz/dışa aktarılmaz
    std::size_t writes_ = 0;
    bool content_mode_ = false;
    std::unique_ptr<search::ContentIndex> content_index_;  // ilk içerik aramasında kurulur
    QString highlight_key_;  // vurgunun uygulandığı (yol, sorgu); aynıysa yeniden kaydırılmaz
    QString type_box_for_;   // tip kutusunun son doldurulduğu yol (gereksiz yeniden doldurmayı önler)

    std::size_t sync_requests_ = 0;
    SettingsPage* settings_ = nullptr;
    QLabel* sync_label_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPushButton* mode_button_ = nullptr;
    QTreeView* folder_tree_ = nullptr;
    QListView* note_list_ = nullptr;
    QLabel* header_ = nullptr;
    QComboBox* type_box_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QSplitter* main_splitter_ = nullptr;
    QSplitter* left_splitter_ = nullptr;
    QStandardItemModel* folder_model_ = nullptr;
    QLabel* count_label_ = nullptr;
    QFileSystemWatcher watcher_;
    QTimer rescan_timer_;
    QTimer autosave_timer_;
    QString state_file_;
    std::unique_ptr<sync::AutoSync> auto_sync_;  // en sonda: yıkımda önce işçi durur, sonra widget'lar gider
};

}  // namespace noteapp::gui
