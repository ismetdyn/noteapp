#pragma once

#include "core/config.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "gui/note_list_model.hpp"

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QString>
#include <QTimer>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QSplitter;
class QStandardItemModel;
class QTabWidget;
class QTreeView;

namespace noteapp::gui {

// Ana pencere: Notlar (arama + klasör ağacı + liste | salt-okunur not görünümü) ve Ayarlar sekmeleri.
// Düzenleme/kaydetme T30'da, ayarlar sayfası T32'de gelir.
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(core::Config config);

    // Diski yeniden tarar; yalnız listeleme değiştiyse listeyi günceller, seçimi ve editörü korur.
    void rescan();
    // Kısayol eylemleri public: testler QShortcut yerine doğrudan çağırır.
    void focus_search();
    void show_settings_tab();

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

    core::Config config_;
    core::NoteStore store_;
    NoteListModel model_;
    std::vector<std::string> folders_;
    std::string current_folder_;
    std::optional<core::NoteInfo>
        shown_;              // editörde şu an görünen not (yol + mtime): gereksiz yeniden yüklemeyi önler
    bool suppress_ = false;  // süzgeç uygulanırken seçim sinyalleri yok sayılır

    QTabWidget* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeView* folder_tree_ = nullptr;
    QListView* note_list_ = nullptr;
    QLabel* header_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QSplitter* main_splitter_ = nullptr;
    QSplitter* left_splitter_ = nullptr;
    QStandardItemModel* folder_model_ = nullptr;
    QLabel* count_label_ = nullptr;
    QFileSystemWatcher watcher_;
    QTimer rescan_timer_;
    QString state_file_;
};

}  // namespace noteapp::gui
