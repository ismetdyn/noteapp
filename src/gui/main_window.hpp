#pragma once

#include "core/config.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "gui/note_list_model.hpp"
#include "gui/save_dialog.hpp"

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QString>
#include <QTimer>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QSplitter;
class QStandardItemModel;
class QTabWidget;
class QTreeView;

namespace noteapp::gui {

// Kullanıcı kararları tek yerden geçer: üretimde gerçek diyalogları gösterir, testler değiştirir.
// (Kalıtım hiyerarşisi değil, yalnız bir test dikişi.)
struct Deciders {
    // nullopt = vazgeçti
    std::function<std::optional<SaveDetails>(const SaveDialogRequest&)> save_dialog;
    std::function<ConflictChoice(const std::string& path)> conflict;
    std::function<CloseChoice()> close_unsaved;
};

// Ana pencere: Notlar (arama + klasör ağacı + liste | düzenlenebilir not görünümü) ve Ayarlar sekmeleri.
// Ayarlar sayfası T32'de gelir.
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(core::Config config);

    Deciders deciders;

    // Diski yeniden tarar; yalnız listeleme değiştiyse listeyi günceller, seçimi ve editörü korur.
    void rescan();
    // Kısayol eylemleri public: testler QShortcut yerine doğrudan çağırır.
    void focus_search();
    void show_settings_tab();
    void new_note(const QString& first_line = {});  // Ctrl+N; nvALT: arama kutusunda Enter
    void save_now();                                // Ctrl+S
    void save_as();                                 // Ctrl+Shift+S: ad/klasör/tip taşıma diyaloğu
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
    void update_status();
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
    std::size_t writes_ = 0;
    QString type_box_for_;  // tip kutusunun son doldurulduğu yol (gereksiz yeniden doldurmayı önler)

    QTabWidget* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
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
};

}  // namespace noteapp::gui
