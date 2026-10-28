#include "core/i18n.hpp"
#include "gui/main_window.hpp"
#include "window_fixture.hpp"

#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTreeView>
#include <algorithm>
#include <doctest/doctest.h>
#include <utility>
#include <vector>

using gui_test::press;
using gui_test::WindowFixture;
using noteapp::gui::MainWindow;

namespace {

// Emoji (2 UTF-16 birimi, 4 UTF-8 baytı) ve Türkçe harfler: bayt, UTF-16 ve kod noktası konumları ayrışır.
constexpr const char kAlpha[] = "😀 İstanbul güzel. istanbul'a git\nbir satır daha\nISTANBUL";

struct Ui {
    QLineEdit* search;
    QListView* list;
    QPlainTextEdit* editor;
    QPushButton* mode;
    explicit Ui(MainWindow& w)
        : search(w.findChild<QLineEdit*>("searchBox")), list(w.findChild<QListView*>("noteList")),
          editor(w.findChild<QPlainTextEdit*>("noteEditor")), mode(w.findChild<QPushButton*>("modeButton")) {
        REQUIRE(search);
        REQUIRE(list);
        REQUIRE(editor);
        REQUIRE(mode);
    }
    std::vector<std::string> rows() const {
        std::vector<std::string> out;
        for (int row = 0; row < list->model()->rowCount(); ++row)
            out.push_back(list->model()->index(row, 0).data().toString().toStdString());
        return out;
    }
    // (başlangıç, uzunluk) çiftleri, konuma göre sıralı.
    std::vector<std::pair<int, int>> highlights() const {
        std::vector<std::pair<int, int>> out;
        for (const auto& selection : editor->extraSelections())
            out.emplace_back(selection.cursor.selectionStart(),
                             selection.cursor.selectionEnd() - selection.cursor.selectionStart());
        std::ranges::sort(out);
        return out;
    }
};

struct ContentFixture : WindowFixture {
    void seed() {
        add_note("alpha.md", kAlpha, 10);
        add_note("beta.md", "istanbul tek", 20);
        add_note("gamma.md", "hiçbir şey", 30);
    }
};

}  // namespace

TEST_CASE_FIXTURE(ContentFixture, "content mode lists matches with their line counts, best first") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    ui.search->setText("istanbul");  // ad modu: yola göre fuzzy, içerik değil
    CHECK(ui.rows().empty());

    window->toggle_search_mode();
    CHECK(ui.rows() == std::vector<std::string>{"alpha.md  (2)", "beta.md  (1)"});  // alpha: 2 satır
    CHECK(ui.list->currentIndex().isValid());                                       // ilk sonuç seçili
}

TEST_CASE_FIXTURE(ContentFixture, "toggle button, API and Tab from the search box switch the mode") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    const QString name_label = ui.mode->text();
    CHECK_FALSE(ui.mode->isChecked());

    ui.search->setText("istanbul");
    ui.mode->click();
    CHECK(ui.mode->isChecked());
    CHECK(ui.mode->text() != name_label);
    CHECK(ui.rows().size() == 2);

    press(ui.search, Qt::Key_Tab);  // Tab geri çevirir
    CHECK_FALSE(ui.mode->isChecked());
    CHECK(ui.mode->text() == name_label);
    CHECK(ui.rows().empty());  // ad modunda "istanbul" hiçbir yola uymaz
    ui.search->setText("alp");
    CHECK(ui.rows() == std::vector<std::string>{"alpha.md"});

    press(ui.search, Qt::Key_Tab);
    CHECK(ui.mode->isChecked());
    CHECK(ui.rows().empty());  // içerikte "alp" yok
}

TEST_CASE_FIXTURE(ContentFixture, "empty query in content mode lists every note without counts") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    CHECK(ui.rows() == std::vector<std::string>{"alpha.md", "beta.md", "gamma.md"});
}

TEST_CASE_FIXTURE(ContentFixture,
                  "selecting a result highlights every occurrence at correct UTF-16 positions") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    ui.search->setText("istanbul");
    REQUIRE(ui.rows().front() == "alpha.md  (2)");
    REQUIRE(ui.editor->toPlainText() == QString::fromUtf8(kAlpha));

    // Beklenen konumlar bayt aritmetiğiyle değil Qt'nin kendi aramasıyla (UTF-16) bulunur.
    const QString text = ui.editor->toPlainText();
    const int first = text.indexOf(QStringLiteral("İstanbul"));
    const int second = text.indexOf(QStringLiteral("istanbul"));
    const int third = text.indexOf(QStringLiteral("ISTANBUL"));
    REQUIRE(first == 3);  // emoji 2 birim + boşluk
    REQUIRE(second > first);
    REQUIRE(third > second);
    const std::vector<std::pair<int, int>> expected{{first, 8}, {second, 8}, {third, 8}};
    CHECK(ui.highlights() == expected);
    CHECK(ui.editor->textCursor().position() == first);  // ilk eşleşmeye kaydırıldı
    CHECK_FALSE(ui.editor->document()->isModified());    // vurgu belgeyi kirletmez
}

TEST_CASE_FIXTURE(ContentFixture, "folding matches Turkish letters and phrases highlight as one range") {
    add_note("tr.md", "Öğrenci ogrenci ÖĞRENCİ\nışık ISIK güzel. istanbul", 0);
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();

    ui.search->setText("ogrenci");
    const std::vector<std::pair<int, int>> students{{0, 7}, {8, 7}, {16, 7}};
    CHECK(ui.highlights() == students);

    ui.search->setText("\"güzel. istanbul\"");
    const QString text = ui.editor->toPlainText();
    const std::vector<std::pair<int, int>> phrase{
        {static_cast<int>(text.indexOf(QStringLiteral("güzel"))), 15}};
    CHECK(ui.highlights() == phrase);
}

TEST_CASE_FIXTURE(ContentFixture, "highlights clear on edit, query change and mode change") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    ui.search->setText("istanbul");
    REQUIRE(ui.highlights().size() == 3);

    ui.editor->insertPlainText("x");  // kullanıcı düzenlemesi
    CHECK(ui.highlights().empty());

    ui.search->setText("istanbul tek");  // sorgu değişti: beta seçilir, yeni vurgu
    CHECK(ui.list->currentIndex().data().toString().startsWith("beta.md"));
    CHECK(ui.highlights().size() == 2);

    ui.search->setText("zzz");  // sonuç yok
    CHECK(ui.highlights().empty());

    ui.search->setText("istanbul");
    REQUIRE_FALSE(ui.highlights().empty());
    window->toggle_search_mode();  // ad moduna dönüş
    CHECK(ui.highlights().empty());
}

TEST_CASE_FIXTURE(ContentFixture, "highlight stays stable across rescans and uses a palette colour") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    ui.search->setText("istanbul");
    REQUIRE(ui.highlights().size() == 3);

    // Kullanıcı kaydırdı/imleç taşıdı; yedek tarama onu ilk eşleşmeye geri atmamalı.
    QTextCursor cursor = ui.editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui.editor->setTextCursor(cursor);
    const int where = ui.editor->textCursor().position();
    window->rescan();
    CHECK(ui.editor->textCursor().position() == where);
    CHECK(ui.highlights().size() == 3);

    const auto selections = ui.editor->extraSelections();
    CHECK(selections.front().format.background().color().alpha() > 0);
    CHECK(selections.front().format.background().color().rgb() ==
          ui.editor->palette().highlight().color().rgb());
}

TEST_CASE_FIXTURE(ContentFixture, "content index follows external changes after a rescan") {
    seed();
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    ui.search->setText("istanbul");
    REQUIRE(ui.rows().size() == 2);

    add_note("new.md", "yeni istanbul istanbul", 0);  // dışarıdan eklendi
    window->rescan();
    CHECK(ui.rows() == std::vector<std::string>{"alpha.md  (2)", "new.md  (1)", "beta.md  (1)"});

    add_note("beta.md", "artık yok", 0);  // dışarıdan içerik değişti
    window->rescan();
    CHECK(ui.rows() == std::vector<std::string>{"alpha.md  (2)", "new.md  (1)"});
}

TEST_CASE_FIXTURE(ContentFixture, "content results respect the selected folder") {
    add_note("work/a.md", "istanbul a", 0);
    add_note("b.md", "istanbul b", 10);
    auto window = make_window();
    Ui ui(*window);
    window->toggle_search_mode();
    ui.search->setText("istanbul");
    REQUIRE(ui.rows().size() == 2);
    auto* tree = window->findChild<QTreeView*>("folderTree");
    REQUIRE(tree);
    tree->setCurrentIndex(tree->model()->index(0, 0, tree->model()->index(0, 0)));  // work
    CHECK(ui.rows() == std::vector<std::string>{"work/a.md  (1)"});
}
