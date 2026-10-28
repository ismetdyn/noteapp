#include "gui/main_window.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
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
#include <cmath>
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
    // Üretim kararları gerçek diyaloglardır; testler bu işlevleri değiştirir.
    deciders.save_dialog = [this](const SaveDialogRequest& request) {
        return run_save_dialog(request, this);
    };
    deciders.conflict = [this](const std::string& path) { return ask_conflict(this, path); };
    deciders.close_unsaved = [this] { return ask_close_unsaved(this); };

    build_ui();
    restore_state();

    // Her tuşta yeniden başlar: ardışık düzenlemeler tek yazmaya birleşir.
    autosave_timer_.setSingleShot(true);
    autosave_timer_.setInterval(std::max(1, config_.tuning.autosave_ms));
    connect(&autosave_timer_, &QTimer::timeout, this, [this] {
        if (dirty_ && shown_ && !new_note_)
            save_existing();
    });

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

    // Sağ sütun: yol + kayıt durumu başlığı, tip kutusu ve editör (not seçilene dek salt-okunur).
    header_ = new QLabel;
    header_->setObjectName("noteHeader");
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    type_box_ = new QComboBox;
    type_box_->setObjectName("typeBox");
    type_box_->setAccessibleName(text("gui.a11y_type"));
    type_box_->setEnabled(false);
    auto* header_row = new QHBoxLayout;
    header_row->addWidget(header_, 1);
    header_row->addWidget(type_box_);
    editor_ = new QPlainTextEdit;
    editor_->setObjectName("noteEditor");
    editor_->setReadOnly(true);
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setAccessibleName(text("gui.a11y_editor"));
    auto* right = new QWidget;
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->addLayout(header_row);
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

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (loading_ || !(shown_ || new_note_))
            return;
        dirty_ = true;
        update_header();
        if (!new_note_)  // yeni not ilk kayıtta diyalog ister: autosave yok
            autosave_timer_.start();
    });
    // Programatik güncellemeler (blockSignals) tetiklemez; yalnız kullanıcı seçimi tip değiştirir.
    connect(type_box_, &QComboBox::currentTextChanged, this,
            [this](const QString& type) { change_type(type); });

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), this, [this] { new_note(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_S), this, [this] { save_now(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), this, [this] { save_as(); });
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
            // nvALT: eşleşme yoksa Enter, sorguyu ilk satır yaparak yeni not açar.
            if (current_path() || new_note_)
                editor_->setFocus();
            else
                new_note(search_->text());
            return true;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!confirm_close()) {  // vazgeçildi ya da kayıt başarısız: pencere ve tampon yerinde kalır
        event->ignore();
        return;
    }
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
    // Editördeki not esastır (seçim, başarısız bir kayıttan sonra ondan ayrışmış olabilir).
    const auto previous = shown_ ? std::optional<core::NotePath>(shown_->path) : current_path();
    suppress_ = true;
    change();
    // TUI ile aynı: önceki not hâlâ listedeyse seçili kalır, yoksa ilk satır. Yeni not yazılırken
    // seçim boş kalır: ilk satırı seçmek taslağı ezerdi.
    QModelIndex target = previous ? model_.index_of(*previous) : QModelIndex{};
    if (!target.isValid() && model_.rowCount() > 0 && !new_note_)
        target = model_.index(0);
    note_list_->setCurrentIndex(target);
    suppress_ = false;
    show_current();
    update_status();
}

void MainWindow::show_current() {
    if (in_show_)  // kayıt akışı listeyi tazelerken seçim sinyali editörü ezmesin
        return;
    in_show_ = true;
    const struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{in_show_};

    QModelIndex index = note_list_->currentIndex();
    const auto target = index.isValid() ? std::optional(model_.note_at(index.row()).path) : std::nullopt;
    const bool same = shown_ && target && shown_->path == *target;
    if (!same) {
        if (new_note_ && !target)  // yeni not yazılırken boş seçim taslağı silmez
            return;
        if (unsaved()) {
            // Başka nota/boş seçime geçmeden önce tampon kaydedilir; olmazsa geçiş iptal.
            if (!flush()) {
                restore_selection();
                return;
            }
            // Kayıt listeyi değiştirmiş olabilir (yeni not, kopya): hedefi yeniden çöz.
            index = target ? model_.index_of(*target) : QModelIndex{};
            suppress_ = true;
            note_list_->setCurrentIndex(index);
            suppress_ = false;
        }
    }

    if (!index.isValid()) {
        shown_.reset();
        set_editor({}, false);
        update_header();
        return;
    }
    const core::NoteInfo& info = model_.note_at(index.row());
    if (shown_ && shown_->path == info.path) {
        // Aynı not, aynı mtime: editör içeriğine/kaydırmasına dokunma (rescan sonrası kararlılık).
        // Kirli tampon varken dışarıdaki değişiklik tamponu ezmez; kayıt anında çakışma diyaloğu çıkar.
        if (shown_->mtime == info.mtime || dirty_)
            return;
    }
    shown_ = info;
    if (info.binary) {
        set_editor(text("gui.binary_preview"), false);
    } else if (const auto loaded = load_text(info); !loaded) {
        set_editor(text("gui.read_failed", {{"detail", core::format_error(loaded.error())}}), false);
    }
    update_header();
}

// Editörü doldurur; dirty/autosave durumunu sıfırlar. UTF-8 olmayan dosya salt-okunur kalır: tampon
// geri yazılırsa bozuk baytlar sessizce değişirdi.
void MainWindow::set_editor(const QString& content, bool editable) {
    autosave_timer_.stop();
    loading_ = true;
    editor_->setPlainText(content);
    editor_->setReadOnly(!editable);
    loading_ = false;
    dirty_ = false;
}

std::expected<void, core::Error> MainWindow::load_text(const core::NoteInfo& info) {
    auto content = store_.read(info.path);
    if (!content)
        return std::unexpected(std::move(content.error()));
    shown_ = info;
    shown_->mtime = content->mtime;  // write()'ın beklediği mtime, içerikle aynı anda okunan değerdir
    std::string& raw = content->text;
    crlf_ = raw.find("\r\n") != std::string::npos;
    if (crlf_) {
        for (std::size_t at = 0; (at = raw.find("\r\n", at)) != std::string::npos; ++at)
            raw.erase(at, 1);
    }
    set_editor(QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size())), core::is_valid_utf8(raw));
    return {};
}

// Kayıt gövdesi: editör her zaman '\n' kullanır; CRLF'li dosya CRLF olarak yazılır (git farkı şişmesin).
std::string MainWindow::body(bool crlf_aware) const {
    QString content = editor_->toPlainText();
    if (crlf_aware && crlf_)
        content.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    const QByteArray utf8 = content.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

void MainWindow::restore_selection() {
    suppress_ = true;
    note_list_->setCurrentIndex(shown_ ? model_.index_of(shown_->path) : QModelIndex{});
    suppress_ = false;
}

void MainWindow::report(const core::Error& error) {
    statusBar()->showMessage(text("gui.save_failed", {{"detail", core::format_error(error)}}));
}

namespace {

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

std::string first_content_line(const std::string& content) {
    std::size_t begin = 0;
    while (begin < content.size()) {
        std::size_t end = content.find('\n', begin);
        if (end == std::string::npos)
            end = content.size();
        const std::string line = content.substr(begin, end - begin);
        if (line.find_first_not_of(" \t\r") != std::string::npos)
            return line;
        begin = end + 1;
    }
    return {};
}

}  // namespace

void MainWindow::update_header() {
    if (new_note_) {
        header_->setText(text("gui.untitled") + "  ·  " + text("gui.state_unsaved"));
        type_box_->setEnabled(false);
        return;
    }
    if (!shown_) {
        header_->clear();
        type_box_->setEnabled(false);
        type_box_for_.clear();
        return;
    }
    const QString path = QString::fromStdString(shown_->path.string());
    header_->setText(path + "  ·  " + text(dirty_ ? "gui.state_modified" : "gui.state_saved"));
    if (type_box_for_ != path) {  // tip kutusu yalnız not/yol değişince yeniden doldurulur
        type_box_for_ = path;
        const QString type = QString::fromStdString(split(shown_->path).type);
        const QSignalBlocker block(type_box_);
        type_box_->clear();
        for (const std::string& known : config_.types)
            type_box_->addItem(QString::fromStdString(known));
        if (type_box_->findText(type) < 0)
            type_box_->addItem(type);
        type_box_->setCurrentText(type);
    }
    type_box_->setEnabled(true);
}

// Listede görünmeyen (arama/klasör süzgeci) bir notu kaydettikten sonra süzgeç gevşetilir; aksi hâlde
// yeni not kaybolmuş gibi görünür ve seçim başka nota düşerdi.
void MainWindow::ensure_visible(const core::NotePath& path) {
    if (!model_.index_of(path).isValid())
        search_->clear();
    if (!model_.index_of(path).isValid())
        folder_tree_->setCurrentIndex(folder_model_->index(0, 0));
}

void MainWindow::new_note(const QString& first_line) {
    if (!flush())
        return;
    new_note_ = true;
    shown_.reset();
    suppress_ = true;
    note_list_->setCurrentIndex({});
    suppress_ = false;
    if (!first_line.isEmpty())
        search_->clear();  // new_note_ açıkken süzgeç yeniden seçim yapmaz
    set_editor(first_line.isEmpty() ? QString{} : first_line + QLatin1Char('\n'), true);
    update_header();
    editor_->moveCursor(QTextCursor::End);
    editor_->setFocus();
}

bool MainWindow::flush() {
    if (new_note_) {
        if (editor_->toPlainText().trimmed().isEmpty()) {  // boş taslakta kaybedilecek bir şey yok
            new_note_ = false;
            return true;
        }
        return save_new();
    }
    return dirty_ && shown_ ? save_existing() : true;
}

void MainWindow::save_now() {
    if (new_note_)
        save_new();
    else if (dirty_ && shown_)
        save_existing();
}

std::expected<void, core::Error> MainWindow::try_write() {
    autosave_timer_.stop();
    const auto written = store_.write(shown_->path, body(true), shown_->mtime);
    if (!written)
        return std::unexpected(written.error());
    ++writes_;
    shown_->mtime = *written;
    dirty_ = false;
    statusBar()->clearMessage();
    update_header();
    rescan();  // listedeki mtime tazelenir: yoksa aynı not "dışarıdan değişmiş" sanılıp yeniden yüklenirdi
    return {};
}

bool MainWindow::save_existing() {
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
        update_header();
        return save_new();
    default:
        report(written.error());
        return false;
    }
}

bool MainWindow::resolve_conflict() {
    switch (deciders.conflict(shown_->path.string())) {
    case ConflictChoice::Reload: {
        const core::NoteInfo info = *shown_;  // load_text shown_'ı yeniden atar
        const auto loaded = load_text(info);
        if (!loaded) {
            report(loaded.error());
            return false;
        }
        update_header();
        return true;
    }
    case ConflictChoice::Overwrite: {
        // Diskteki güncel mtime alınır; kullanıcı bilerek ezdi.
        const auto fresh = store_.read(shown_->path);
        if (!fresh) {
            report(fresh.error());
            return false;
        }
        shown_->mtime = fresh->mtime;
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

bool MainWindow::save_copy() {
    const PathParts parts = split(shown_->path);
    const auto created =
        store_.create(core::NewNote{parts.folder, parts.stem + " (copy)", parts.type, body(false)});
    if (!created) {
        report(created.error());
        return false;
    }
    statusBar()->showMessage(text("gui.saved_copy", {{"path", created->string()}}));
    return adopt_saved(*created);
}

bool MainWindow::save_new() {
    autosave_timer_.stop();
    const std::string content = body(false);
    const detect::Options options{config_.default_type, config_.tuning.min_confidence,
                                  config_.tuning.max_scan_bytes};
    const detect::Suggestion suggestion = detect::suggest(content, options);

    SaveDialogRequest request;
    request.initial = {core::slugify(first_content_line(content), std::chrono::system_clock::now(),
                                     config_.tuning.slug_max_length),
                       current_folder_, suggestion.type};
    request.folders = folders_;
    request.types = config_.types;
    request.hint =
        suggestion.detected
            ? text("gui.suggestion", {{"type", suggestion.type},
                                      {"percent", std::to_string(std::lround(suggestion.confidence * 100))}})
            : text("gui.suggestion_default", {{"type", suggestion.type}});
    const auto chosen = deciders.save_dialog(request);
    if (!chosen)
        return false;  // vazgeç: tampon ve yeni-not durumu aynen kalır

    const auto created = store_.create(core::NewNote{chosen->folder, chosen->name, chosen->type, content});
    if (!created) {
        report(created.error());
        return false;
    }
    return adopt_saved(*created);
}

// Diske yazılmış notu "mevcut not" yapar: mtime alınır, liste tazelenir, not seçili kalır.
bool MainWindow::adopt_saved(const core::NotePath& path) {
    const auto content = store_.read(path);
    if (!content) {
        report(content.error());  // not diskte, ama tampon kirli kalır: kullanıcı yeniden deneyebilir
        return false;
    }
    shown_ = core::NoteInfo{path, content->mtime, content->text.size(), false};
    new_note_ = false;
    dirty_ = false;
    crlf_ = false;  // create() satır sonlarını LF'ye normalleştirir
    autosave_timer_.stop();
    update_header();
    rescan();
    ensure_visible(path);
    update_header();
    return true;
}

bool MainWindow::confirm_close() {
    if (new_note_) {
        if (editor_->toPlainText().trimmed().isEmpty())
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

void MainWindow::save_as() {
    if (new_note_) {
        save_new();
        return;
    }
    if (!shown_ || !flush())
        return;
    const PathParts parts = split(shown_->path);
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Move;
    request.initial = {parts.stem, parts.folder, parts.type};
    request.folders = folders_;
    request.types = config_.types;
    if (const auto chosen = deciders.save_dialog(request))
        move_current(*chosen);
}

void MainWindow::change_type(const QString& type) {
    if (!shown_ || new_note_ || type.isEmpty())
        return;
    const PathParts parts = split(shown_->path);
    if (type.toStdString() == parts.type)
        return;
    if (!flush() || !move_current({parts.stem, parts.folder, type.toStdString()})) {
        type_box_for_.clear();  // başarısız: kutu gerçek uzantıya döner
        update_header();
    }
}

// Tampon önceden kaydedilmiş olmalı (flush). Hedef varsa NoteStore::move AlreadyExists verir, hiçbir şey
// değişmez.
bool MainWindow::move_current(const SaveDetails& target) {
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
    shown_->path = *moved;
    if (const auto fresh = store_.read(*moved))  // taşıma mtime'ı korur; yine de güncel değeri al
        shown_->mtime = fresh->mtime;
    statusBar()->clearMessage();
    type_box_for_.clear();
    update_header();
    rescan();
    ensure_visible(*moved);
    return true;
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
