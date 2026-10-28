#include "gui/main_window.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextLayout>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>
#include <map>

namespace noteapp::gui {

namespace {

constexpr int kFolderPathRole = Qt::UserRole;
constexpr int kDefaultWindowWidth = 1000;
constexpr int kDefaultWindowHeight = 650;
constexpr int kDefaultLeftPaneWidth = 320;
constexpr int kMillisPerSecond = 1000;
const char kStateFileName[] = "gui-state.ini";

QString text(const char* key, const core::Args& args = {}) {
    return QString::fromStdString(core::tr(key, args));
}

// Yollar UTF-8/UTF-16 arasında kayıpsız taşınsın diye std::filesystem::path'ten u16string üzerinden.
QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

// Eşleşen karakterleri kalın çizen liste temsilcisi. Vurgu konumları model tarafından UTF-16 birimi olarak
// gelir.
class NoteItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString label = opt.text;
        opt.text.clear();  // arka plan/seçim stilini stil çizsin, metni biz çizeriz
        const QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect area = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        QTextLayout layout(label, opt.font);
        QTextOption no_wrap;
        no_wrap.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(no_wrap);

        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        QList<QTextLayout::FormatRange> ranges;
        for (const int unit : index.data(NoteListModel::HighlightRole).value<QList<int>>())
            ranges.append({unit, 1, bold});
        layout.setFormats(ranges);

        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();
        line.setPosition({0, 0});

        painter->save();
        painter->setClipRect(area);
        painter->setPen((opt.state & QStyle::State_Selected) ? opt.palette.highlightedText().color()
                                                             : opt.palette.text().color());
        layout.draw(painter, QPointF(area.left(), area.top() + (area.height() - line.height()) / 2));
        painter->restore();
    }
};

}  // namespace

MainWindow::MainWindow(core::Config config)
    : config_(std::move(config)), store_(config_.notes_dir, config_.tuning.slug_max_length),
      state_file_(to_qstring(core::state_dir() / kStateFileName)) {
    build_ui();
    restore_state();

    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { rescan(); });
    // Yedek tarama: izleyici olay kaçırırsa (ağ sürücüsü, dizin sınırı) liste yine güncellenir.
    rescan_timer_.setInterval(std::max(1, config_.tuning.rescan_seconds) * kMillisPerSecond);
    connect(&rescan_timer_, &QTimer::timeout, this, [this] { rescan(); });
    rescan_timer_.start();

    rescan();
    // Son seçili not (SPEC-gui "Diğer"): ilk taramadan sonra, listede hâlâ varsa seçilir.
    const QString last = QSettings(state_file_, QSettings::IniFormat).value("last_note").toString();
    if (const auto path = core::NotePath::parse(last.toStdString())) {
        if (const QModelIndex index = model_.index_of(*path); index.isValid()) {
            note_list_->setCurrentIndex(index);
            show_current();
        }
    }
}

void MainWindow::build_ui() {
    setWindowTitle(QStringLiteral("noteapp"));
    tabs_ = new QTabWidget(this);
    setCentralWidget(tabs_);

    // Sol sütun: arama + klasör ağacı + liste (dikey splitter).
    search_ = new QLineEdit;
    search_->setObjectName("searchBox");
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(text("gui.search_placeholder"));
    search_->setAccessibleName(text("gui.a11y_search"));
    search_->installEventFilter(this);

    folder_model_ = new QStandardItemModel(this);
    folder_tree_ = new QTreeView;
    folder_tree_->setObjectName("folderTree");
    folder_tree_->setModel(folder_model_);
    folder_tree_->setHeaderHidden(true);
    folder_tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    folder_tree_->setAccessibleName(text("gui.a11y_folders"));

    note_list_ = new QListView;
    note_list_->setObjectName("noteList");
    note_list_->setModel(&model_);
    note_list_->setItemDelegate(new NoteItemDelegate(note_list_));
    note_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    note_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    note_list_->setUniformItemSizes(true);  // 5.000 satırda yerleşim maliyetini sabitler
    note_list_->setAccessibleName(text("gui.a11y_notes"));

    left_splitter_ = new QSplitter(Qt::Vertical);
    left_splitter_->addWidget(search_);
    left_splitter_->addWidget(folder_tree_);
    left_splitter_->addWidget(note_list_);
    left_splitter_->setChildrenCollapsible(false);
    left_splitter_->setStretchFactor(0, 0);
    left_splitter_->setStretchFactor(1, 1);
    left_splitter_->setStretchFactor(2, 3);

    // Sağ sütun: yol başlığı + salt-okunur editör (düzenleme T30'da).
    header_ = new QLabel;
    header_->setObjectName("noteHeader");
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    editor_ = new QPlainTextEdit;
    editor_->setObjectName("noteEditor");
    editor_->setReadOnly(true);
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setAccessibleName(text("gui.a11y_editor"));
    auto* right = new QWidget;
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->addWidget(header_);
    right_layout->addWidget(editor_, 1);

    main_splitter_ = new QSplitter(Qt::Horizontal);
    main_splitter_->addWidget(left_splitter_);
    main_splitter_->addWidget(right);
    main_splitter_->setStretchFactor(0, 0);
    main_splitter_->setStretchFactor(1, 1);
    main_splitter_->setSizes({kDefaultLeftPaneWidth, kDefaultWindowWidth - kDefaultLeftPaneWidth});

    tabs_->addTab(main_splitter_, text("gui.tab_notes"));
    tabs_->addTab(new QLabel(text("gui.settings_placeholder")), text("gui.tab_settings"));  // T32

    count_label_ = new QLabel;
    statusBar()->addWidget(count_label_, 1);
    statusBar()->addPermanentWidget(new QLabel(text("gui.sync_off")));  // T32 gerçek durumu bağlar

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString& query) { apply_filter([&] { model_.set_query(query.toStdString()); }); });
    connect(folder_tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) {
                if (suppress_ || !current.isValid())
                    return;
                const std::string folder = current.data(kFolderPathRole).toString().toStdString();
                apply_filter([&] {
                    current_folder_ = folder;
                    model_.set_folder(folder);
                });
            });
    connect(note_list_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (!suppress_)
            show_current();
    });

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this, [this] { focus_search(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_P), this, [this] { focus_search(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this, [this] { show_settings_tab(); });

    rebuild_folders();
}

void MainWindow::focus_search() {
    tabs_->setCurrentWidget(main_splitter_);
    search_->setFocus();
    search_->selectAll();
}

void MainWindow::show_settings_tab() {
    tabs_->setCurrentIndex(1);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == search_ && event->type() == QEvent::KeyPress) {
        switch (static_cast<QKeyEvent*>(event)->key()) {
        case Qt::Key_Up:
            move_selection(-1);
            return true;
        case Qt::Key_Down:
            move_selection(1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (current_path())
                editor_->setFocus();
            return true;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    save_state();
    QMainWindow::closeEvent(event);
}

void MainWindow::restore_state() {
    const QSettings settings(state_file_, QSettings::IniFormat);
    if (!restoreGeometry(settings.value("geometry").toByteArray()))
        resize(kDefaultWindowWidth, kDefaultWindowHeight);
    main_splitter_->restoreState(settings.value("splitter_main").toByteArray());
    left_splitter_->restoreState(settings.value("splitter_left").toByteArray());
}

void MainWindow::save_state() const {
    std::error_code ignored;
    std::filesystem::create_directories(core::state_dir(), ignored);  // QSettings üst dizini kendisi açmaz
    QSettings settings(state_file_, QSettings::IniFormat);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("splitter_main", main_splitter_->saveState());
    settings.setValue("splitter_left", left_splitter_->saveState());
    if (const auto path = current_path())
        settings.setValue("last_note", QString::fromStdString(path->string()));
}

std::optional<core::NotePath> MainWindow::current_path() const {
    const QModelIndex index = note_list_->currentIndex();
    if (!index.isValid())
        return std::nullopt;
    return model_.note_at(index.row()).path;
}

void MainWindow::move_selection(int delta) {
    const int count = model_.rowCount();
    if (count == 0)
        return;
    const QModelIndex current = note_list_->currentIndex();
    const int row = current.isValid() ? current.row() : (delta > 0 ? -1 : count);
    note_list_->setCurrentIndex(model_.index(std::clamp(row + delta, 0, count - 1)));
}

void MainWindow::apply_filter(const std::function<void()>& change) {
    const auto previous = current_path();
    suppress_ = true;
    change();
    // TUI ile aynı: önceki not hâlâ listedeyse seçili kalır, yoksa ilk satır.
    QModelIndex target = previous ? model_.index_of(*previous) : QModelIndex{};
    if (!target.isValid() && model_.rowCount() > 0)
        target = model_.index(0);
    note_list_->setCurrentIndex(target);
    suppress_ = false;
    show_current();
    update_status();
}

void MainWindow::show_current() {
    const QModelIndex index = note_list_->currentIndex();
    if (!index.isValid()) {
        shown_.reset();
        header_->clear();
        editor_->clear();
        return;
    }
    const core::NoteInfo& info = model_.note_at(index.row());
    // Aynı not, aynı mtime: editör içeriğine/kaydırmasına dokunma (rescan sonrası kararlılık).
    if (shown_ && shown_->path == info.path && shown_->mtime == info.mtime)
        return;
    shown_ = info;
    header_->setText(QString::fromStdString(info.path.string()));
    if (info.binary) {
        editor_->setPlainText(text("gui.binary_preview"));
    } else if (const auto content = store_.read(info.path)) {
        editor_->setPlainText(
            QString::fromUtf8(content->text.data(), static_cast<qsizetype>(content->text.size())));
    } else {
        editor_->setPlainText(text("gui.read_failed", {{"detail", core::format_error(content.error())}}));
    }
}

void MainWindow::update_status() {
    count_label_->setText(text("gui.note_count", {{"shown", std::to_string(model_.rowCount())},
                                                  {"total", std::to_string(model_.total())}}));
}

void MainWindow::rebuild_folders() {
    suppress_ = true;
    folder_model_->clear();
    auto* root = new QStandardItem(text("gui.all_notes"));
    root->setData(QString{}, kFolderPathRole);
    folder_model_->appendRow(root);

    std::map<std::string, QStandardItem*> by_path{{"", root}};
    for (const std::string& folder : folders_) {  // sıralı: ebeveyn çocuktan önce gelir
        const auto slash = folder.rfind('/');
        const std::string parent = slash == std::string::npos ? "" : folder.substr(0, slash);
        auto* item = new QStandardItem(
            QString::fromStdString(slash == std::string::npos ? folder : folder.substr(slash + 1)));
        item->setData(QString::fromStdString(folder), kFolderPathRole);
        const auto parent_it = by_path.find(parent);
        (parent_it != by_path.end() ? parent_it->second : root)->appendRow(item);
        by_path[folder] = item;
    }
    folder_tree_->expandAll();

    // Seçili klasör silindiyse köke düşer.
    const auto selected = by_path.find(current_folder_);
    if (selected == by_path.end())
        current_folder_.clear();
    folder_tree_->setCurrentIndex((selected == by_path.end() ? root : selected->second)->index());
    suppress_ = false;
}

void MainWindow::watch_directories() {
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    // Var olmayan dizin eklenemez (sessizce reddedilir); yedek zamanlayıcı onu yakalar.
    QStringList paths{to_qstring(config_.notes_dir)};
    for (const std::string& folder : folders_)
        paths << to_qstring(config_.notes_dir /
                            std::filesystem::path(std::u8string(folder.begin(), folder.end())));
    watcher_.addPaths(paths);
}

void MainWindow::rescan() {
    if (auto folders = store_.folders(); folders != folders_ || watcher_.directories().isEmpty()) {
        const bool changed = folders != folders_;
        folders_ = std::move(folders);
        if (changed)
            rebuild_folders();
        watch_directories();
    }
    auto notes = store_.list();
    apply_filter([&] {
        model_.set_folder(current_folder_);
        model_.set_notes(std::move(notes));
    });
}

}  // namespace noteapp::gui
