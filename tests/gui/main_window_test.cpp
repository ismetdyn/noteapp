#include "core/config.hpp"
#include "core/i18n.hpp"
#include "gui/main_window.hpp"

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>

using noteapp::gui::MainWindow;

namespace {

// Geçici NOTEAPP_HOME + notes_dir; pencere testleri gerçek kullanıcı dizinine dokunmaz.
struct WindowFixture {
    int argc = 1;
    char arg0[10] = "gui_tests";
    char* argv[2] = {arg0, nullptr};
    QApplication app{argc, argv};
    QTemporaryDir home;
    noteapp::core::Config config;

    WindowFixture() {
        REQUIRE(home.isValid());
        qputenv("NOTEAPP_HOME", home.path().toUtf8());
        noteapp::core::init_i18n("en");
        config = noteapp::core::load_config().config;
        config.notes_dir = std::filesystem::path(home.filePath("notes").toStdU16String());
        std::filesystem::create_directories(config.notes_dir);
    }

    // age_minutes büyük = eski; sıralama mtime'a bağlı olduğundan açıkça ayarlanır.
    void add_note(const QString& relative, const QByteArray& content, int age_minutes = 0) {
        const QString full = QString::fromStdU16String(config.notes_dir.u16string()) + "/" + relative;
        std::filesystem::create_directories(std::filesystem::path(full.toStdU16String()).parent_path());
        QFile file(full);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(content);
        file.close();
        std::filesystem::last_write_time(full.toStdU16String(),
                                         std::filesystem::file_time_type::clock::now() -
                                             std::chrono::minutes(age_minutes));
    }

    std::unique_ptr<MainWindow> make_window() { return std::make_unique<MainWindow>(config); }
};

void press(QWidget* target, int key, const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(target, &event);
}

struct Widgets {
    QLineEdit* search;
    QListView* list;
    QPlainTextEdit* editor;
    QTreeView* tree;
    QTabWidget* tabs;
    explicit Widgets(MainWindow& w)
        : search(w.findChild<QLineEdit*>("searchBox")), list(w.findChild<QListView*>("noteList")),
          editor(w.findChild<QPlainTextEdit*>("noteEditor")), tree(w.findChild<QTreeView*>("folderTree")),
          tabs(w.findChild<QTabWidget*>()) {
        REQUIRE(search);
        REQUIRE(list);
        REQUIRE(editor);
        REQUIRE(tree);
        REQUIRE(tabs);
    }
    std::string current() const { return list->currentIndex().data().toString().toStdString(); }
};

}  // namespace

TEST_CASE_FIXTURE(WindowFixture, "window lists notes, selects the newest and shows its content") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    add_note("gamma.md", "gamma body", 10);
    auto window = make_window();
    Widgets w(*window);

    CHECK(w.list->model()->rowCount() == 3);
    CHECK(w.current() == "gamma.md");
    CHECK(w.editor->toPlainText() == "gamma body");
    CHECK(w.editor->isReadOnly());
}

TEST_CASE_FIXTURE(WindowFixture, "typing in the search box filters the list and keeps the selection valid") {
    add_note("alpha.md", "a", 30);
    add_note("beta.md", "b", 20);
    add_note("gamma.md", "g", 10);
    auto window = make_window();
    Widgets w(*window);

    press(w.search, Qt::Key_B, "b");
    press(w.search, Qt::Key_E, "e");
    CHECK(w.search->text() == "be");
    CHECK(w.list->model()->rowCount() == 1);
    CHECK(w.current() == "beta.md");
    CHECK(w.editor->toPlainText() == "b");

    w.search->clear();
    CHECK(w.list->model()->rowCount() == 3);
    CHECK(w.current() == "beta.md");  // seçim yola göre korunur (TUI ile aynı)
}

TEST_CASE_FIXTURE(WindowFixture, "Up/Down in the search box move the list selection and update the view") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    add_note("gamma.md", "gamma body", 10);
    auto window = make_window();
    Widgets w(*window);

    press(w.search, Qt::Key_Down);
    CHECK(w.current() == "beta.md");
    CHECK(w.editor->toPlainText() == "beta body");
    press(w.search, Qt::Key_Down);
    press(w.search, Qt::Key_Down);  // sınırda kenetlenir
    CHECK(w.current() == "alpha.md");
    press(w.search, Qt::Key_Up);
    CHECK(w.current() == "beta.md");
}

TEST_CASE_FIXTURE(WindowFixture, "Enter in the search box focuses the editor") {
    add_note("alpha.md", "alpha body");
    auto window = make_window();
    Widgets w(*window);

    window->focus_search();
    CHECK(window->focusWidget() == w.search);
    press(w.search, Qt::Key_Return);
    CHECK(window->focusWidget() == w.editor);
}

TEST_CASE_FIXTURE(WindowFixture, "focus_search selects the existing query; settings tab switches") {
    add_note("alpha.md", "alpha body");
    auto window = make_window();
    Widgets w(*window);

    w.search->setText("alp");
    window->focus_search();
    CHECK(w.search->selectedText() == "alp");

    CHECK(w.tabs->currentIndex() == 0);
    window->show_settings_tab();
    CHECK(w.tabs->currentIndex() == 1);
    window->focus_search();  // Ctrl+K her sekmeden Notlar'a döner
    CHECK(w.tabs->currentIndex() == 0);
}

TEST_CASE_FIXTURE(WindowFixture, "binary note shows the binary placeholder instead of its bytes") {
    add_note("blob.bin", QByteArray("ab\0cd", 5));
    auto window = make_window();
    Widgets w(*window);

    CHECK(w.editor->toPlainText() == QString::fromStdString(noteapp::core::tr("gui.binary_preview")));
}

TEST_CASE_FIXTURE(WindowFixture, "folder tree limits the list to the subtree") {
    add_note("root.md", "r", 30);
    add_note("work/one.md", "1", 20);
    add_note("work/deep/two.md", "2", 10);
    add_note("home/three.md", "3", 5);
    auto window = make_window();
    Widgets w(*window);
    REQUIRE(w.list->model()->rowCount() == 4);

    auto* tree_model = qobject_cast<QStandardItemModel*>(w.tree->model());
    REQUIRE(tree_model);
    const QModelIndex all = tree_model->index(0, 0);
    CHECK(all.data().toString() == QString::fromStdString(noteapp::core::tr("gui.all_notes")));
    QModelIndex work;
    for (int row = 0; row < tree_model->rowCount(all); ++row)
        if (tree_model->index(row, 0, all).data().toString() == "work")
            work = tree_model->index(row, 0, all);
    REQUIRE(work.isValid());

    w.tree->setCurrentIndex(work);
    CHECK(w.list->model()->rowCount() == 2);
    w.tree->setCurrentIndex(all);
    CHECK(w.list->model()->rowCount() == 4);
}

TEST_CASE_FIXTURE(WindowFixture,
                  "rescan picks up new notes, keeps selection and editor, resets only on change") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    auto window = make_window();
    Widgets w(*window);
    press(w.search, Qt::Key_Down);
    REQUIRE(w.current() == "alpha.md");

    int resets = 0;
    QObject::connect(w.list->model(), &QAbstractItemModel::modelReset, w.list, [&resets] { ++resets; });
    window->rescan();
    CHECK(resets == 0);  // değişiklik yok: sıfırlama yok

    add_note("newest.md", "fresh", 0);
    window->rescan();
    CHECK(resets == 1);
    CHECK(w.list->model()->rowCount() == 3);
    CHECK(w.current() == "alpha.md");                // seçim yola göre korundu
    CHECK(w.editor->toPlainText() == "alpha body");  // editör sabit
}

TEST_CASE_FIXTURE(WindowFixture, "selected note is restored from gui-state.ini after close") {
    add_note("alpha.md", "alpha body", 30);
    add_note("beta.md", "beta body", 20);
    {
        auto window = make_window();
        Widgets w(*window);
        press(w.search, Qt::Key_Down);
        REQUIRE(w.current() == "alpha.md");
        window->show();
        window->close();
    }
    CHECK(QFile::exists(home.filePath("state/gui-state.ini")));

    auto reopened = make_window();
    Widgets w(*reopened);
    CHECK(w.current() == "alpha.md");
    CHECK(w.editor->toPlainText() == "alpha body");
}
