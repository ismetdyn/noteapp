#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "gui/main_window.hpp"
#include "gui/save_dialog.hpp"
#include "window_fixture.hpp"

#include <QComboBox>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <cmath>
#include <doctest/doctest.h>

using gui_test::press;
using gui_test::WindowFixture;
using noteapp::gui::CloseChoice;
using noteapp::gui::ConflictChoice;
using noteapp::gui::MainWindow;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;

namespace {

constexpr int kAutosaveMs = 60;
constexpr const char kJavaCode[] = "public class Greeter {\n    public static void main(String[] args) {\n"
                                   "        System.out.println(\"hi\");\n    }\n}\n";

// Gerçek zaman geçirir: autosave zamanlayıcısı olay döngüsünde çalışır.
void wait_ms(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Ui {
    QLineEdit* search;
    QListView* list;
    QPlainTextEdit* editor;
    QLabel* header;
    QComboBox* type_box;
    explicit Ui(MainWindow& w)
        : search(w.findChild<QLineEdit*>("searchBox")), list(w.findChild<QListView*>("noteList")),
          editor(w.findChild<QPlainTextEdit*>("noteEditor")), header(w.findChild<QLabel*>("noteHeader")),
          type_box(w.findChild<QComboBox*>("typeBox")) {
        REQUIRE(search);
        REQUIRE(list);
        REQUIRE(editor);
        REQUIRE(header);
        REQUIRE(type_box);
    }
    std::string current() const { return list->currentIndex().data().toString().toStdString(); }
    bool header_has(const char* key) const {
        return header->text().contains(QString::fromStdString(noteapp::core::tr(key)));
    }
};

// Her test kendi kararlarını enjekte eder; çağrı sayıları da denetlenir.
struct Calls {
    int dialog = 0;
    int conflict = 0;
    int close_prompt = 0;
    SaveDialogRequest last_request;
    std::string conflict_path;
};

struct SaveFixture : WindowFixture {
    Calls calls;
    SaveFixture() { config.tuning.autosave_ms = kAutosaveMs; }

    void wire(MainWindow& window, std::optional<SaveDetails> dialog_result, ConflictChoice conflict,
              CloseChoice close_choice = CloseChoice::Cancel) {
        window.deciders.save_dialog = [this, dialog_result](const SaveDialogRequest& request) {
            ++calls.dialog;
            calls.last_request = request;
            return dialog_result;
        };
        window.deciders.conflict = [this, conflict](const std::string& path) {
            ++calls.conflict;
            calls.conflict_path = path;
            return conflict;
        };
        window.deciders.close_unsaved = [this, close_choice] {
            ++calls.close_prompt;
            return close_choice;
        };
    }
};

}  // namespace

TEST_CASE_FIXTURE(SaveFixture, "editing an existing note is autosaved once after autosave_ms") {
    add_note("alpha.md", "alpha body", 10);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.current() == "alpha.md");
    CHECK_FALSE(ui.editor->isReadOnly());
    CHECK(ui.header_has("gui.state_saved"));

    for (const char* piece : {"1", "2", "3", "4", "5"})
        ui.editor->insertPlainText(piece);  // art arda tuşlar
    CHECK(ui.header_has("gui.state_modified"));
    CHECK(window->write_count() == 0);
    CHECK(read_note("alpha.md") == "alpha body");

    wait_ms(kAutosaveMs * 4);
    CHECK(window->write_count() == 1);  // tüm tuşlar tek yazmaya birleşti
    CHECK(read_note("alpha.md") == "12345alpha body");
    CHECK(ui.header_has("gui.state_saved"));

    wait_ms(kAutosaveMs * 3);
    CHECK(window->write_count() == 1);  // değişmeyen not yeniden yazılmaz
}

TEST_CASE_FIXTURE(SaveFixture, "rescan reloads a clean open note silently when it changed on disk") {
    add_note("alpha.md", "old", 10);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.editor->toPlainText() == "old");

    add_note("alpha.md", "from outside", 0);
    window->rescan();
    CHECK(ui.editor->toPlainText() == "from outside");
    CHECK(window->write_count() == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "rescan keeps a dirty buffer when the note changed on disk") {
    add_note("alpha.md", "old", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);
    ui.editor->setPlainText("mine");

    add_note("alpha.md", "from outside", 0);
    window->rescan();
    CHECK(ui.editor->toPlainText() == "mine");
    CHECK(read_note("alpha.md") == "from outside");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: save mine as copy keeps both versions and shows the copy") {
    add_note("work/alpha.md", "original", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::SaveCopy);
    REQUIRE(ui.current() == "work/alpha.md");

    ui.editor->setPlainText("mine");
    add_note("work/alpha.md", "external", 0);  // okuma ile kayıt arasında dışarıdan değişiklik
    window->save_now();

    CHECK(calls.conflict == 1);
    CHECK(calls.conflict_path == "work/alpha.md");
    CHECK(read_note("work/alpha.md") == "external");
    CHECK(read_note("work/alpha (copy).md") == "mine");
    CHECK(ui.current() == "work/alpha (copy).md");
    CHECK(ui.editor->toPlainText() == "mine");
    CHECK(ui.header->text().contains("work/alpha (copy).md"));
    CHECK(ui.header_has("gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: a second copy gets the collision suffix") {
    add_note("alpha.md", "original", 10);
    add_note("alpha (copy).md", "older copy", 20);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::SaveCopy);
    window->rescan();
    for (int row = 0; row < ui.list->model()->rowCount(); ++row)
        if (ui.list->model()->index(row, 0).data().toString() == "alpha.md")
            ui.list->setCurrentIndex(ui.list->model()->index(row, 0));
    REQUIRE(ui.current() == "alpha.md");

    ui.editor->setPlainText("mine");
    add_note("alpha.md", "external", 0);
    window->save_now();
    CHECK(read_note("alpha (copy).md") == "older copy");  // ezilmedi
    CHECK(read_note("alpha (copy)-2.md") == "mine");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: overwrite writes the buffer over the external change") {
    add_note("alpha.md", "original", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Overwrite);

    ui.editor->setPlainText("mine");
    add_note("alpha.md", "external", 0);
    window->save_now();
    CHECK(calls.conflict == 1);
    CHECK(read_note("alpha.md") == "mine");
    CHECK_FALSE(note_exists("alpha (copy).md"));
    CHECK(ui.header_has("gui.state_saved"));

    ui.editor->insertPlainText("+");  // yeni mtime alındı: sonraki kayıt çakışmaz
    window->save_now();
    CHECK(calls.conflict == 1);
    CHECK(read_note("alpha.md") == "+mine");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: reload drops the buffer and shows the disk version") {
    add_note("alpha.md", "original", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Reload);

    ui.editor->setPlainText("mine");
    add_note("alpha.md", "external", 0);
    window->save_now();
    CHECK(ui.editor->toPlainText() == "external");
    CHECK(read_note("alpha.md") == "external");
    CHECK(ui.header_has("gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: cancel keeps the buffer dirty and the disk untouched") {
    add_note("alpha.md", "original", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);

    ui.editor->setPlainText("mine");
    add_note("alpha.md", "external", 0);
    window->save_now();
    CHECK(ui.editor->toPlainText() == "mine");
    CHECK(read_note("alpha.md") == "external");
    CHECK(ui.header_has("gui.state_modified"));
}

TEST_CASE_FIXTURE(SaveFixture, "the default conflict button is 'save mine as copy', Escape cancels") {
    auto box = noteapp::gui::make_conflict_box(nullptr, "alpha.md");
    REQUIRE(box->defaultButton());
    CHECK(box->defaultButton()->text() == QString::fromStdString(noteapp::core::tr("gui.conflict_copy")));
    REQUIRE(box->escapeButton());
    CHECK(static_cast<QPushButton*>(box->escapeButton())->text() ==
          QString::fromStdString(noteapp::core::tr("gui.cancel")));
}

TEST_CASE_FIXTURE(SaveFixture, "a vanished note is saved through the new-note dialog, nothing is lost") {
    add_note("alpha.md", "original", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"restored", "", "md"}, ConflictChoice::Cancel);

    std::filesystem::remove(std::filesystem::path(full_path("alpha.md").toStdU16String()));
    ui.editor->setPlainText("mine");
    window->save_now();
    CHECK(calls.dialog == 1);
    CHECK(read_note("restored.md") == "mine");
}

TEST_CASE_FIXTURE(SaveFixture,
                  "new note: Ctrl+N opens an empty untitled buffer, first save asks the dialog") {
    add_note("work/alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"greeter", "work", "txt"}, ConflictChoice::Cancel);

    window->new_note();
    CHECK(ui.editor->toPlainText().isEmpty());
    CHECK_FALSE(ui.editor->isReadOnly());
    CHECK(ui.header_has("gui.untitled"));

    ui.editor->setPlainText(kJavaCode);
    wait_ms(kAutosaveMs * 3);
    CHECK(calls.dialog == 0);  // yeni nota autosave yok: ilk kayıt kullanıcı kararı
    CHECK(window->write_count() == 0);

    window->save_now();
    CHECK(calls.dialog == 1);
    CHECK(read_note("work/greeter.txt") == QByteArray(kJavaCode));
    CHECK(ui.current() == "work/greeter.txt");  // artık mevcut not, listede seçili
    CHECK(ui.header_has("gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "save dialog is prefilled with the detect suggestion and the tree folder") {
    add_note("work/alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);

    window->new_note();
    ui.editor->setPlainText(kJavaCode);
    window->save_now();
    REQUIRE(calls.dialog == 1);

    const noteapp::detect::Options options{config.default_type, config.tuning.min_confidence,
                                           config.tuning.max_scan_bytes};
    const auto expected = noteapp::detect::suggest(kJavaCode, options);
    REQUIRE(expected.type == "java");
    CHECK(calls.last_request.initial.type == "java");
    CHECK(calls.last_request.initial.name == noteapp::core::slugify("public class Greeter {",
                                                                    std::chrono::system_clock::now(),
                                                                    config.tuning.slug_max_length));
    CHECK(calls.last_request.initial.folder.empty());  // "Tümü" seçili
    CHECK(calls.last_request.hint.contains("java"));
    CHECK(calls.last_request.hint.contains(QString::number(std::lround(expected.confidence * 100))));
    CHECK(calls.last_request.types == config.types);
    CHECK(calls.last_request.folders == std::vector<std::string>{"work"});
}

TEST_CASE_FIXTURE(SaveFixture, "chosen type becomes the extension") {
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"greeter", "", "java"}, ConflictChoice::Cancel);
    window->new_note();
    ui.editor->setPlainText("plain words");
    window->save_now();
    CHECK(note_exists("greeter.java"));
}

TEST_CASE_FIXTURE(SaveFixture, "cancelling the save dialog keeps the new note and its buffer") {
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);
    window->new_note();
    ui.editor->setPlainText("keep me");
    window->save_now();
    CHECK(calls.dialog == 1);
    CHECK(ui.editor->toPlainText() == "keep me");
    CHECK(ui.header_has("gui.state_unsaved"));
    CHECK_FALSE(note_exists("keep-me.md"));
}

TEST_CASE_FIXTURE(SaveFixture, "a name collision in the dialog creates a suffixed note, never overwrites") {
    add_note("same.md", "existing", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"same", "", "md"}, ConflictChoice::Cancel);
    window->new_note();
    ui.editor->setPlainText("fresh");
    window->save_now();
    CHECK(read_note("same.md") == "existing");
    CHECK(read_note("same-2.md") == "fresh");
}

TEST_CASE_FIXTURE(SaveFixture, "an invalid dialog result keeps the buffer and reports the error") {
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"bad/name", "", "md"}, ConflictChoice::Cancel);
    window->new_note();
    ui.editor->setPlainText("keep me");
    window->save_now();
    CHECK(ui.editor->toPlainText() == "keep me");
    CHECK(ui.header_has("gui.state_unsaved"));
    CHECK_FALSE(window->statusBar()->currentMessage().isEmpty());
}

TEST_CASE_FIXTURE(SaveFixture, "SaveDialog shows prefilled fields and rejects invalid input inline") {
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Create;
    request.initial = {"my-note", "work", "java"};
    request.folders = {"work", "home"};
    request.types = {"md", "java"};
    request.hint = "suggested: java 82%";
    noteapp::gui::SaveDialog dialog(request, nullptr);

    CHECK(dialog.details().name == "my-note");
    CHECK(dialog.details().folder == "work");
    CHECK(dialog.details().type == "java");
    CHECK(dialog.findChild<QLabel*>("suggestionLabel")->text() == "suggested: java 82%");

    dialog.findChild<QLineEdit*>("nameEdit")->setText("a/b");
    dialog.accept();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK_FALSE(dialog.findChild<QLabel*>("errorLabel")->text().isEmpty());

    dialog.findChild<QLineEdit*>("nameEdit")->setText("ok");
    dialog.findChild<QComboBox*>("folderBox")->setCurrentText("../escape");
    dialog.accept();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK_FALSE(dialog.findChild<QLabel*>("errorLabel")->text().isEmpty());

    dialog.findChild<QComboBox*>("folderBox")->setCurrentText("home");
    dialog.findChild<QComboBox*>("typeBox")->setCurrentText(
        ".txt");  // elle yazılan uzantı, baştaki nokta atılır
    dialog.accept();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.details().type == "txt");
    CHECK(dialog.details().folder == "home");
}

TEST_CASE_FIXTURE(SaveFixture, "switching notes saves the dirty note first") {
    add_note("alpha.md", "alpha", 20);
    add_note("beta.md", "beta", 10);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.current() == "beta.md");

    ui.editor->insertPlainText("edited ");
    press(ui.search, Qt::Key_Down);  // alpha'ya geç
    CHECK(read_note("beta.md") == "edited beta");
    CHECK(ui.current() == "alpha.md");
    CHECK(ui.editor->toPlainText() == "alpha");
    CHECK(window->write_count() == 1);
}

TEST_CASE_FIXTURE(SaveFixture, "switching away from a new note asks the dialog; cancel stays on the note") {
    add_note("alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);
    window->new_note();
    ui.editor->setPlainText("draft text");

    press(ui.search, Qt::Key_Down);
    CHECK(calls.dialog == 1);
    CHECK(ui.editor->toPlainText() == "draft text");  // vazgeçildi: taslak yerinde
    CHECK(ui.header_has("gui.state_unsaved"));
    CHECK_FALSE(ui.list->currentIndex().isValid());

    wire(*window, SaveDetails{"draft", "", "md"}, ConflictChoice::Cancel);
    press(ui.search, Qt::Key_Down);
    CHECK(read_note("draft.md") == "draft text");
    CHECK(ui.editor->toPlainText() == "alpha");
}

TEST_CASE_FIXTURE(SaveFixture, "a failed save while switching keeps the buffer and the selection") {
    add_note("alpha.md", "alpha", 20);
    add_note("beta.md", "beta", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);
    ui.editor->setPlainText("mine");
    add_note("beta.md", "external", 0);

    press(ui.search, Qt::Key_Down);  // çakışma → Cancel → geçiş iptal
    CHECK(ui.editor->toPlainText() == "mine");
    CHECK(ui.current() == "beta.md");
}

TEST_CASE_FIXTURE(SaveFixture, "close with an unsaved new note: Cancel keeps the window and the buffer") {
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Cancel);
    window->show();
    window->new_note();
    ui.editor->setPlainText("unsaved");

    CHECK_FALSE(window->close());
    CHECK(window->isVisible());
    CHECK(calls.close_prompt == 1);
    CHECK(ui.editor->toPlainText() == "unsaved");
    CHECK_FALSE(note_exists("x.md"));
}

TEST_CASE_FIXTURE(SaveFixture, "close with an unsaved new note: Discard closes, Save creates the note") {
    {
        auto window = make_window();
        Ui ui(*window);
        wire(*window, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Discard);
        window->show();
        window->new_note();
        ui.editor->setPlainText("throwaway");
        CHECK(window->close());
        CHECK_FALSE(note_exists("x.md"));
    }
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Save);
    window->show();
    window->new_note();
    ui.editor->setPlainText("kept");
    CHECK(window->close());
    CHECK(read_note("x.md") == "kept");
}

TEST_CASE_FIXTURE(SaveFixture, "close with an empty new note does not ask") {
    auto window = make_window();
    wire(*window, std::nullopt, ConflictChoice::Cancel, CloseChoice::Cancel);
    window->show();
    window->new_note();
    CHECK(window->close());
    CHECK(calls.close_prompt == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "close autosaves a dirty note; a conflict cancel aborts the close") {
    add_note("alpha.md", "alpha", 10);
    {
        auto window = make_window();
        Ui ui(*window);
        wire(*window, std::nullopt, ConflictChoice::Cancel);
        window->show();
        ui.editor->insertPlainText("x");
        CHECK(window->close());
        CHECK(read_note("alpha.md") == "xalpha");
    }
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);
    window->show();
    ui.editor->insertPlainText("y");
    add_note("alpha.md", "external", 0);
    CHECK_FALSE(window->close());
    CHECK(window->isVisible());
    CHECK(ui.editor->toPlainText() == "yxalpha");
    CHECK(read_note("alpha.md") == "external");
}

TEST_CASE_FIXTURE(SaveFixture, "type combobox moves the note to the new extension") {
    add_note("work/alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.type_box->currentText() == "md");

    ui.editor->insertPlainText("x");  // kirli not önce kaydedilir, sonra taşınır
    ui.type_box->setCurrentText("txt");
    CHECK(read_note("work/alpha.txt") == "xalpha");
    CHECK_FALSE(note_exists("work/alpha.md"));
    CHECK(ui.current() == "work/alpha.txt");
    CHECK(ui.editor->toPlainText() == "xalpha");

    ui.editor->insertPlainText("y");  // taşımadan sonra kayıt yeni yola gider
    window->save_now();
    CHECK(read_note("work/alpha.txt") == "xyalpha");  // imleç "x"in ardında
}

TEST_CASE_FIXTURE(SaveFixture, "type combobox refuses to overwrite an existing target") {
    add_note("alpha.md", "alpha", 10);
    add_note("alpha.txt", "other", 20);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.current() == "alpha.md");

    ui.type_box->setCurrentText("txt");
    CHECK(read_note("alpha.md") == "alpha");
    CHECK(read_note("alpha.txt") == "other");
    CHECK(ui.current() == "alpha.md");
    CHECK(ui.type_box->currentText() == "md");  // kutu geri alınır
    CHECK_FALSE(window->statusBar()->currentMessage().isEmpty());
}

TEST_CASE_FIXTURE(SaveFixture, "Ctrl+Shift+S moves with the dialog prefilled from the current note") {
    add_note("alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, SaveDetails{"renamed", "sub", "txt"}, ConflictChoice::Cancel);

    window->save_as();
    CHECK(calls.last_request.mode == SaveDialogRequest::Mode::Move);
    CHECK(calls.last_request.initial.name == "alpha");
    CHECK(calls.last_request.initial.type == "md");
    CHECK(read_note("sub/renamed.txt") == "alpha");
    CHECK_FALSE(note_exists("alpha.md"));
    CHECK(ui.current() == "sub/renamed.txt");
}

TEST_CASE_FIXTURE(SaveFixture, "Enter in the search box with no match starts a new note from the query") {
    add_note("alpha.md", "alpha", 10);
    auto window = make_window();
    Ui ui(*window);
    wire(*window, std::nullopt, ConflictChoice::Cancel);

    ui.search->setText("zzzqqq");
    REQUIRE(ui.list->model()->rowCount() == 0);
    press(ui.search, Qt::Key_Return);
    CHECK(ui.editor->toPlainText().startsWith("zzzqqq"));
    CHECK_FALSE(ui.editor->isReadOnly());
    CHECK(ui.header_has("gui.untitled"));
    CHECK(window->focusWidget() == ui.editor);

    window->save_now();
    CHECK(calls.last_request.initial.name == "zzzqqq");
}

TEST_CASE_FIXTURE(SaveFixture, "binary and non-UTF-8 notes stay read-only and are never rewritten") {
    add_note("blob.bin", QByteArray("ab\0cd", 5), 10);
    add_note("latin.txt", QByteArray("caf\xe9"), 20);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.current() == "blob.bin");
    CHECK(ui.editor->isReadOnly());
    press(ui.search, Qt::Key_Down);
    REQUIRE(ui.current() == "latin.txt");
    CHECK(ui.editor->isReadOnly());
    window->save_now();
    CHECK(read_note("latin.txt") == QByteArray("caf\xe9"));
    CHECK(window->write_count() == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "CRLF line endings survive an edit") {
    add_note("win.txt", "one\r\ntwo\r\n", 10);
    auto window = make_window();
    Ui ui(*window);
    ui.editor->moveCursor(QTextCursor::End);
    ui.editor->insertPlainText("three\n");
    window->save_now();
    CHECK(read_note("win.txt") == "one\r\ntwo\r\nthree\r\n");
}

// Karakterizasyon: yükleme CRLF'i doğrusal sürede LF'ye indirir (eskiden satır başına erase, O(n^2)); süre
// değil içerik doğrulanır.
TEST_CASE_FIXTURE(SaveFixture, "a large CRLF note loads with its content intact and keeps CRLF on save") {
    QByteArray crlf;
    QString expected;
    for (int line = 0; line < 20000; ++line) {
        crlf += "satır " + QByteArray::number(line) + "\r\n";
        expected += QString("satır %1\n").arg(line);
    }
    add_note("big.txt", crlf, 10);
    auto window = make_window();
    Ui ui(*window);
    REQUIRE(ui.current() == "big.txt");
    CHECK(ui.editor->toPlainText().replace(QChar::ParagraphSeparator, '\n') == expected);

    ui.editor->moveCursor(QTextCursor::End);
    ui.editor->insertPlainText("son\n");
    window->save_now();
    CHECK(read_note("big.txt") == crlf + "son\r\n");
}
