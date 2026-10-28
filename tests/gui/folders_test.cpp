#include "core/i18n.hpp"
#include "gui/main_window.hpp"
#include "gui/save_dialog.hpp"
#include "window_fixture.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTreeView>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

using gui_test::WindowFixture;
using noteapp::core::tr;
using noteapp::gui::ConflictChoice;
using noteapp::gui::MainWindow;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;

namespace {

struct Ui {
    QListView* list;
    QPlainTextEdit* editor;
    QLabel* header;
    QTreeView* tree;
    explicit Ui(MainWindow& w)
        : list(w.findChild<QListView*>("noteList")), editor(w.findChild<QPlainTextEdit*>("noteEditor")),
          header(w.findChild<QLabel*>("noteHeader")), tree(w.findChild<QTreeView*>("folderTree")) {
        REQUIRE(list);
        REQUIRE(editor);
        REQUIRE(header);
        REQUIRE(tree);
    }
    std::string current() const { return list->currentIndex().data().toString().toStdString(); }
    void select(int row) { list->setCurrentIndex(list->model()->index(row, 0)); }

    // Ağaçtaki klasör yolları (kök hariç); öğe verisi Qt::UserRole.
    std::vector<std::string> folders() const {
        std::vector<std::string> out;
        collect(tree->model(), {}, out);
        return out;
    }

private:
    static void collect(const QAbstractItemModel* model, const QModelIndex& parent,
                        std::vector<std::string>& out) {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex index = model->index(row, 0, parent);
            if (const std::string path = index.data(Qt::UserRole).toString().toStdString(); !path.empty())
                out.push_back(path);
            collect(model, index, out);
        }
    }
};

struct FolderFixture : WindowFixture {
    std::optional<std::string> name_answer;
    bool delete_answer = false;
    int confirms = 0;
    QString last_confirm;
    QString last_prompt_initial;
    int prompts = 0;
    std::optional<SaveDetails> move_answer;
    int move_dialogs = 0;
    SaveDialogRequest move_request;

    void wire(MainWindow& window) {
        window.deciders.folder_name = [this](const QString&, const QString& initial) {
            ++prompts;
            last_prompt_initial = initial;
            return name_answer;
        };
        window.deciders.confirm_delete = [this](const QString& message) {
            ++confirms;
            last_confirm = message;
            return delete_answer;
        };
        window.deciders.save_dialog = [this](const SaveDialogRequest& request) {
            ++move_dialogs;
            move_request = request;
            return move_answer;
        };
        window.deciders.conflict = [](const std::string&) { return ConflictChoice::Cancel; };
    }
    bool dir_exists(const QString& relative) const { return QDir(full_path(relative)).exists(); }
    static QString status(MainWindow& w) { return w.findChild<QStatusBar*>()->currentMessage(); }
    static QString text(const char* key, const noteapp::core::Args& args = {}) {
        return QString::fromStdString(tr(key, args));
    }
};

}  // namespace

// ---- SPEC-gui kabul 4 ----

TEST_CASE_FIXTURE(FolderFixture, "deleting a non-empty folder never happens without confirmation") {
    add_note("work/a.md", "a", 10);
    add_note("work/b.md", "b", 20);
    auto window = make_window();
    wire(*window);

    delete_answer = false;
    window->delete_folder("work");
    CHECK(confirms == 1);
    CHECK(note_exists("work/a.md"));
    CHECK(note_exists("work/b.md"));
    CHECK(dir_exists("work"));
    // Onay metni klasör adını ve not sayısını taşır.
    CHECK(last_confirm == text("gui.confirm_delete_folder", {{"folder", "work"}, {"count", "2"}}));

    delete_answer = true;
    window->delete_folder("work");
    CHECK(confirms == 2);
    CHECK_FALSE(dir_exists("work"));
    Ui ui(*window);
    CHECK(ui.folders().empty());
}

TEST_CASE_FIXTURE(FolderFixture, "deleting an empty folder asks a simple confirmation") {
    auto window = make_window();
    wire(*window);
    name_answer = "boş";
    window->new_folder("");
    REQUIRE(dir_exists("boş"));

    delete_answer = false;
    window->delete_folder("boş");
    CHECK(dir_exists("boş"));
    CHECK(last_confirm == text("gui.confirm_delete_empty_folder", {{"folder", "boş"}}));

    delete_answer = true;
    window->delete_folder("boş");
    CHECK_FALSE(dir_exists("boş"));
}

TEST_CASE_FIXTURE(FolderFixture, "deleting an unknown folder reports the error and touches nothing") {
    add_note("work/a.md", "a", 10);
    auto window = make_window();
    wire(*window);
    // contains_link yolu ayrıcalık ister (Windows'ta bağ oluşturma); hata gösterimi zaten-yok klasörle
    // denenir.
    delete_answer = true;
    window->delete_folder("ghost");
    CHECK(status(*window).contains("ghost"));
    CHECK(note_exists("work/a.md"));
}

// ---- Oluşturma / yeniden adlandırma ----

TEST_CASE_FIXTURE(FolderFixture,
                  "new folder is created under root or the clicked folder, Turkish names included") {
    add_note("work/a.md", "a");
    auto window = make_window();
    wire(*window);
    Ui ui(*window);

    name_answer = "Öğrenci Notları";
    window->new_folder("");
    CHECK(dir_exists("Öğrenci Notları"));

    name_answer = "ışık";
    window->new_folder("work");
    CHECK(dir_exists("work/ışık"));
    const auto folders = ui.folders();
    CHECK(std::ranges::find(folders, "work/ışık") != folders.end());
    CHECK(std::ranges::find(folders, "Öğrenci Notları") != folders.end());
}

TEST_CASE_FIXTURE(FolderFixture, "new folder: cancel and collision change nothing and report") {
    add_note("work/a.md", "a");
    auto window = make_window();
    wire(*window);

    name_answer = std::nullopt;
    window->new_folder("");
    CHECK(prompts == 1);

    name_answer = "work";
    window->new_folder("");
    CHECK(status(*window).contains(text("error.already_exists", {{"path", "work"}})));

    name_answer = "a/../b";  // geçersiz yol çekirdekte reddedilir
    window->new_folder("");
    CHECK_FALSE(dir_exists("b"));
    CHECK_FALSE(status(*window).isEmpty());
}

TEST_CASE_FIXTURE(FolderFixture,
                  "rename folder moves notes, offers the old leaf name and rejects collisions") {
    add_note("work/a.md", "a", 10);
    add_note("other/b.md", "b", 20);
    auto window = make_window();
    wire(*window);

    name_answer = "çalışma";
    window->rename_folder("work");
    CHECK(last_prompt_initial == "work");
    CHECK(note_exists("çalışma/a.md"));
    CHECK_FALSE(note_exists("work/a.md"));

    name_answer = "other";  // çakışma: hiçbir şey taşınmaz
    window->rename_folder("çalışma");
    CHECK(note_exists("çalışma/a.md"));
    CHECK(note_exists("other/b.md"));
    CHECK(status(*window).contains("other"));
}

TEST_CASE_FIXTURE(FolderFixture,
                  "renaming the folder of the shown dirty note flushes first and keeps state consistent") {
    add_note("work/a.md", "hello", 10);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    REQUIRE(ui.current() == "work/a.md");

    ui.editor->setPlainText("hello world");  // kirli tampon
    const auto writes = window->write_count();
    name_answer = "proj";
    window->rename_folder("work");

    CHECK(window->write_count() == writes + 1);  // önce kaydedildi
    CHECK(read_note("proj/a.md") == "hello world");
    CHECK(ui.current() == "proj/a.md");
    CHECK(ui.header->text().contains("proj/a.md"));
    CHECK(ui.header->text().contains(text("gui.state_saved")));
    CHECK(ui.editor->toPlainText() == "hello world");

    // shown_ tazelendi: sonraki kayıt çakışma diyaloğu istemeden yeni yola yazar.
    ui.editor->setPlainText("second");
    window->save_now();
    CHECK(read_note("proj/a.md") == "second");
}

TEST_CASE_FIXTURE(FolderFixture,
                  "deleting the folder of the shown dirty note flushes first, then clears the editor") {
    add_note("work/a.md", "hello", 10);
    add_note("keep.md", "keep", 20);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    REQUIRE(ui.current() == "work/a.md");
    ui.editor->setPlainText("edited");

    delete_answer = false;  // vazgeçildi: kaydedilmez, tampon yerinde
    window->delete_folder("work");
    CHECK(window->write_count() == 0);
    CHECK(ui.editor->toPlainText() == "edited");

    delete_answer = true;
    window->delete_folder("work");
    CHECK(window->write_count() == 1);  // silmeden önce flush edildi
    CHECK_FALSE(dir_exists("work"));
    CHECK(ui.current() == "keep.md");
    CHECK(ui.editor->toPlainText() == "keep");
    CHECK(ui.header->text().contains("keep.md"));
}

TEST_CASE_FIXTURE(FolderFixture, "a failing flush aborts folder rename and delete") {
    add_note("work/a.md", "original", 10);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    ui.editor->setPlainText("mine");
    add_note("work/a.md", "external", 0);  // çakışma; karar Vazgeç

    delete_answer = true;
    window->delete_folder("work");
    CHECK(note_exists("work/a.md"));
    name_answer = "proj";
    window->rename_folder("work");
    CHECK(note_exists("work/a.md"));
    CHECK_FALSE(dir_exists("proj"));
    CHECK(ui.editor->toPlainText() == "mine");
}

// ---- Not silme ----

TEST_CASE_FIXTURE(FolderFixture, "delete confirmation box defaults to Cancel") {
    const auto box = noteapp::gui::make_delete_box(nullptr, "x");
    REQUIRE(box->defaultButton());
    CHECK(box->defaultButton()->text() == text("gui.cancel"));
    CHECK(box->escapeButton() == static_cast<QAbstractButton*>(box->defaultButton()));
    CHECK(box->text() == "x");
}

TEST_CASE_FIXTURE(FolderFixture,
                  "delete note asks confirmation, deletes only when confirmed, selects the neighbour") {
    add_note("n1.md", "one", 0);
    add_note("n2.md", "two", 10);
    add_note("n3.md", "three", 20);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    ui.select(1);
    REQUIRE(ui.current() == "n2.md");

    delete_answer = false;
    window->delete_note();
    CHECK(last_confirm == text("gui.confirm_delete_note", {{"path", "n2.md"}}));
    CHECK(note_exists("n2.md"));
    CHECK(ui.current() == "n2.md");

    delete_answer = true;
    window->delete_note();
    CHECK_FALSE(note_exists("n2.md"));
    CHECK(ui.current() == "n3.md");  // komşu satır
    CHECK(ui.editor->toPlainText() == "three");

    window->delete_note();  // son satır silinince bir üstteki
    CHECK(ui.current() == "n1.md");
    window->delete_note();
    CHECK(ui.current().empty());
    CHECK(ui.editor->toPlainText().isEmpty());
    CHECK(ui.header->text().isEmpty());
}

TEST_CASE_FIXTURE(FolderFixture, "delete shortcut and context path go through the confirmation decider") {
    add_note("n1.md", "one");
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    delete_answer = false;
    bool found = false;
    for (auto* shortcut : window->findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(Qt::Key_Delete)) {
            found = true;
            QMetaObject::invokeMethod(shortcut, "activated");
        }
    }
    CHECK(found);
    CHECK(confirms == 1);
    CHECK(note_exists("n1.md"));
}

TEST_CASE_FIXTURE(FolderFixture,
                  "deleting the shown dirty note flushes first and keeps it on a failing flush") {
    add_note("n1.md", "original", 10);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    ui.editor->setPlainText("mine");
    add_note("n1.md", "external", 0);  // çakışma, karar Vazgeç -> flush başarısız
    delete_answer = true;
    window->delete_note();
    CHECK(note_exists("n1.md"));
    CHECK(ui.editor->toPlainText() == "mine");
}

// ---- Ctrl+M ----

TEST_CASE_FIXTURE(FolderFixture, "move note changes only the folder (name and type are locked)") {
    add_note("work/a.md", "alpha", 10);
    add_note("dest/x.md", "x", 20);
    auto window = make_window();
    wire(*window);
    Ui ui(*window);
    ui.select(0);
    REQUIRE(ui.current() == "work/a.md");

    move_answer = std::nullopt;  // vazgeç
    window->move_note();
    CHECK(note_exists("work/a.md"));
    CHECK(move_request.mode == SaveDialogRequest::Mode::Move);
    CHECK(move_request.folder_only);
    CHECK(move_request.initial.folder == "work");

    ui.editor->setPlainText("alpha edited");
    move_answer = SaveDetails{"ignored", "dest", "txt"};
    window->move_note();
    CHECK(read_note("dest/a.md") == "alpha edited");  // flush edildi, ad/tip korundu
    CHECK_FALSE(note_exists("work/a.md"));
    CHECK(ui.current() == "dest/a.md");
    CHECK(ui.header->text().contains("dest/a.md"));

    // shown_ tutarlı: sonraki kayıt yeni yola gider.
    ui.editor->setPlainText("again");
    window->save_now();
    CHECK(read_note("dest/a.md") == "again");
}

TEST_CASE_FIXTURE(FolderFixture, "Ctrl+M shortcut opens the move dialog") {
    add_note("a.md", "a");
    auto window = make_window();
    wire(*window);
    bool found = false;
    for (auto* shortcut : window->findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_M)) {
            found = true;
            QMetaObject::invokeMethod(shortcut, "activated");
        }
    }
    CHECK(found);
    CHECK(move_dialogs == 1);
}

TEST_CASE_FIXTURE(FolderFixture, "folder-only save dialog locks name and type") {
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Move;
    request.initial = {"a", "work", "md"};
    request.folders = {"work"};
    request.types = {"md"};
    request.folder_only = true;
    noteapp::gui::SaveDialog dialog(request, nullptr);
    CHECK_FALSE(dialog.findChild<QLineEdit*>("nameEdit")->isEnabled());
    CHECK_FALSE(dialog.findChild<QComboBox*>("typeBox")->isEnabled());
    CHECK(dialog.findChild<QComboBox*>("folderBox")->isEnabled());
}
