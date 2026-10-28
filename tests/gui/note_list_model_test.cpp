#include "gui/note_list_model.hpp"

#include <QString>
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using noteapp::core::NoteInfo;
using noteapp::core::NotePath;
using noteapp::gui::NoteListModel;

namespace {

NoteInfo make_note(const std::string& path, int mtime_minutes = 0) {
    return {*NotePath::parse(path), std::filesystem::file_time_type{} + std::chrono::minutes(mtime_minutes),
            10};
}

QStringList displays(const NoteListModel& model) {
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.index(row).data().toString();
    return out;
}

QList<int> highlights(const NoteListModel& model, int row) {
    return model.index(row).data(NoteListModel::HighlightRole).value<QList<int>>();
}

// Model sıfırlama sayacı: "yalnız değişimde sıfırla" kuralını ölçer.
struct ResetCounter {
    int resets = 0;
    explicit ResetCounter(NoteListModel& model) {
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [this] { ++resets; });
    }
};

}  // namespace

TEST_CASE("empty query lists all notes newest first") {
    NoteListModel model;
    model.set_notes({make_note("old.md", 1), make_note("new.md", 9), make_note("mid.md", 5)});
    CHECK(displays(model) == QStringList{"new.md", "mid.md", "old.md"});
    CHECK(model.total() == 3);
    CHECK(highlights(model, 0).isEmpty());
}

TEST_CASE("empty query orders by mtime even when path lengths differ") {
    NoteListModel model;
    model.set_notes({make_note("a-very-long-name.md", 9), make_note("b.md", 1)});
    CHECK(displays(model) == QStringList{"a-very-long-name.md", "b.md"});
}

TEST_CASE("query ranks by fuzzy score and exposes highlight units") {
    NoteListModel model;
    model.set_notes({make_note("alpha.md"), make_note("beta.md"), make_note("gamma.md")});
    model.set_query("bt");
    REQUIRE(model.rowCount() == 1);
    CHECK(displays(model) == QStringList{"beta.md"});
    CHECK(highlights(model, 0) == QList<int>{0, 2});
}

TEST_CASE("highlight positions are UTF-16 units, not UTF-8 bytes (Turkish names)") {
    NoteListModel model;
    // "ışık.txt": ı ve ş iki baytlık; byte ofseti 'k' için 4, UTF-16 indeksi 2.
    model.set_notes({make_note("\xC4\xB1\xC5\x9F\xC4\xB1k.txt")});  // "ışık.txt"
    model.set_query("sk");
    REQUIRE(model.rowCount() == 1);
    CHECK(model.index(0).data().toString() == QString::fromUtf8("ışık.txt"));
    CHECK(highlights(model, 0) == QList<int>{1, 3});  // ş ve k

    model.set_query("ISIK");  // katlama: büyük harf sorgu da eşleşir
    REQUIRE(model.rowCount() == 1);
    CHECK(highlights(model, 0) == QList<int>{0, 1, 2, 3});
}

TEST_CASE("highlight covers both UTF-16 units of a non-BMP character") {
    NoteListModel model;
    model.set_notes({make_note("\xF0\x9F\x98\x80x.md")});  // "😀x.md": 😀 = 2 birim, x indeks 2
    model.set_query("x");
    REQUIRE(model.rowCount() == 1);
    CHECK(highlights(model, 0) == QList<int>{2});
}

TEST_CASE("folder filter limits to the subtree and not to sibling prefixes") {
    NoteListModel model;
    model.set_notes({make_note("a/one.md", 3), make_note("a/b/two.md", 2), make_note("ab/three.md"),
                     make_note("root.md")});
    model.set_folder("a");
    CHECK(displays(model) == QStringList{"a/one.md", "a/b/two.md"});
    model.set_folder("a/b");
    CHECK(displays(model) == QStringList{"a/b/two.md"});
    model.set_folder("");
    CHECK(model.rowCount() == 4);
}

TEST_CASE("folder filter and query combine") {
    NoteListModel model;
    model.set_notes({make_note("a/one.md"), make_note("b/one.md")});
    model.set_folder("b");
    model.set_query("one");
    CHECK(displays(model) == QStringList{"b/one.md"});
}

TEST_CASE("index_of finds rows by path and survives refiltering") {
    NoteListModel model;
    model.set_notes({make_note("a.md", 3), make_note("b.md", 2), make_note("c.md", 1)});
    const NotePath b = *NotePath::parse("b.md");
    CHECK(model.index_of(b).row() == 1);
    model.set_query("c");
    CHECK_FALSE(model.index_of(b).isValid());
    CHECK(model.index_of(*NotePath::parse("c.md")).row() == 0);
    model.set_query("");
    CHECK(model.index_of(b).row() == 1);
}

TEST_CASE("model resets only when the listing or filter actually changes") {
    NoteListModel model;
    ResetCounter counter(model);
    const auto notes = std::vector{make_note("a.md", 2), make_note("b.md", 1)};

    CHECK(model.set_notes(notes));
    CHECK(counter.resets == 1);
    CHECK_FALSE(model.set_notes(notes));  // aynı listeleme: sıfırlama yok
    CHECK(counter.resets == 1);

    auto touched = notes;
    touched[0].mtime += std::chrono::minutes(1);
    CHECK(model.set_notes(touched));  // mtime değişti
    CHECK(counter.resets == 2);

    model.set_query("a");
    CHECK(counter.resets == 3);
    model.set_query("a");  // aynı sorgu
    CHECK(counter.resets == 3);
    model.set_folder("");  // aynı klasör
    CHECK(counter.resets == 3);
}

TEST_CASE("same_listing compares path, mtime and size") {
    const auto a = std::vector{make_note("a.md", 1)};
    auto b = a;
    CHECK(noteapp::gui::same_listing(a, b));
    b[0].size = 11;
    CHECK_FALSE(noteapp::gui::same_listing(a, b));
}
