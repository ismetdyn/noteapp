#include "gui/note_list_model.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

using noteapp::core::NoteInfo;
using noteapp::core::NotePath;
using noteapp::gui::NoteListModel;

namespace {

using Offsets = std::vector<std::size_t>;
using Strings = std::vector<std::string>;

NoteInfo make_note(const std::string& path, int mtime_minutes = 0) {
    return {*NotePath::parse(path), std::filesystem::file_time_type{} + std::chrono::minutes(mtime_minutes),
            10};
}

Strings displays(const NoteListModel& model) {
    Strings out;
    for (int row = 0; row < model.row_count(); ++row)
        out.push_back(model.display(row));
    return out;
}

}  // namespace

TEST_CASE("empty query lists all notes newest first") {
    NoteListModel model;
    model.set_notes({make_note("old.md", 1), make_note("new.md", 9), make_note("mid.md", 5)});
    CHECK(displays(model) == Strings{"new.md", "mid.md", "old.md"});
    CHECK(model.total() == 3);
    CHECK(model.highlights(0).empty());
}

TEST_CASE("empty query orders by mtime even when path lengths differ") {
    NoteListModel model;
    model.set_notes({make_note("a-very-long-name.md", 9), make_note("b.md", 1)});
    CHECK(displays(model) == Strings{"a-very-long-name.md", "b.md"});
}

TEST_CASE("query ranks by fuzzy score and exposes highlight offsets") {
    NoteListModel model;
    model.set_notes({make_note("alpha.md"), make_note("beta.md"), make_note("gamma.md")});
    model.set_query("bt");
    REQUIRE(model.row_count() == 1);
    CHECK(displays(model) == Strings{"beta.md"});
    CHECK(model.highlights(0) == Offsets{0, 2});
}

// ImGui UTF-8 çizer: vurgu artık UTF-16 birimi değil, yol içindeki bayt ofsetidir (kod noktası başı).
TEST_CASE("highlight offsets point at code point starts in Turkish names") {
    NoteListModel model;
    model.set_notes({make_note("ışık.txt")});
    model.set_query("sk");
    REQUIRE(model.row_count() == 1);
    CHECK(model.display(0) == "ışık.txt");
    CHECK(model.highlights(0) == Offsets{2, 6});  // ş ve k

    model.set_query("ISIK");  // katlama: büyük harf sorgu da eşleşir
    REQUIRE(model.row_count() == 1);
    CHECK(model.highlights(0) == Offsets{0, 2, 4, 6});
}

TEST_CASE("highlight offset skips a whole non-BMP character") {
    NoteListModel model;
    model.set_notes({make_note("\xF0\x9F\x98\x80x.md")});  // 😀 dört bayt, x ofset 4
    model.set_query("x");
    REQUIRE(model.row_count() == 1);
    CHECK(model.highlights(0) == Offsets{4});
}

TEST_CASE("folder filter limits to the subtree and not to sibling prefixes") {
    NoteListModel model;
    model.set_notes({make_note("a/one.md", 3), make_note("a/b/two.md", 2), make_note("ab/three.md"),
                     make_note("root.md")});
    model.set_folder("a");
    CHECK(displays(model) == Strings{"a/one.md", "a/b/two.md"});
    model.set_folder("a/b");
    CHECK(displays(model) == Strings{"a/b/two.md"});
    model.set_folder("");
    CHECK(model.row_count() == 4);
}

TEST_CASE("folder filter and query combine") {
    NoteListModel model;
    model.set_notes({make_note("a/one.md"), make_note("b/one.md")});
    model.set_folder("b");
    model.set_query("one");
    CHECK(displays(model) == Strings{"b/one.md"});
}

TEST_CASE("index_of finds rows by path and survives refiltering") {
    NoteListModel model;
    model.set_notes({make_note("a.md", 3), make_note("b.md", 2), make_note("c.md", 1)});
    const NotePath b = *NotePath::parse("b.md");
    CHECK(model.index_of(b) == 1);
    model.set_query("c");
    CHECK(model.index_of(b) == -1);
    CHECK(model.index_of(*NotePath::parse("c.md")) == 0);
    model.set_query("");
    CHECK(model.index_of(b) == 1);
}

TEST_CASE("model resets only when the listing or filter actually changes") {
    NoteListModel model;
    const auto notes = std::vector{make_note("a.md", 2), make_note("b.md", 1)};

    CHECK(model.set_notes(notes));
    CHECK(model.resets() == 1);
    CHECK_FALSE(model.set_notes(notes));  // aynı listeleme: sıfırlama yok
    CHECK(model.resets() == 1);

    auto touched = notes;
    touched[0].mtime += std::chrono::minutes(1);
    CHECK(model.set_notes(touched));  // mtime değişti
    CHECK(model.resets() == 2);

    model.set_query("a");
    CHECK(model.resets() == 3);
    model.set_query("a");  // aynı sorgu
    CHECK(model.resets() == 3);
    model.set_folder("");  // aynı klasör
    CHECK(model.resets() == 3);
}

TEST_CASE("same_listing compares path, mtime and size") {
    const auto a = std::vector{make_note("a.md", 1)};
    auto b = a;
    CHECK(noteapp::gui::same_listing(a, b));
    b[0].size = 11;
    CHECK_FALSE(noteapp::gui::same_listing(a, b));
}

// İnceleme bulgusu: bozuk sorgu açıkken kapsam yeniden kurulursa satırlar yeni kapsamın dışını göstermemeli.
TEST_CASE("an invalid query keeps rows index-safe when the notes or the folder change") {
    NoteListModel model;
    model.set_notes({make_note("a/x.md", 1), make_note("a/y.md", 2), make_note("b/z.md", 3)});
    model.set_query("size:abc");
    REQUIRE(model.query_error().has_value());
    CHECK(model.row_count() == 3);  // son geçerli liste kalır

    model.set_notes({make_note("a/x.md", 1)});  // senkron/yeniden tarama: daha az not
    REQUIRE(model.row_count() == 1);
    CHECK(model.note_at(0).path.string() == "a/x.md");
    CHECK(model.display(0) == "a/x.md");
    CHECK(model.query_error().has_value());

    model.set_notes({make_note("a/x.md", 1), make_note("b/z.md", 3)});
    model.set_folder("b");
    REQUIRE(model.row_count() == 1);
    CHECK(model.display(0) == "b/z.md");
}

// İlişki adı güvenilmez: senkronla bozuk ad gelse de satırın nedeni tek satırlık görünür metindir.
TEST_CASE("a relation reason is drawn without raw control characters") {
    NoteListModel model;
    model.set_notes({make_note("a.md", 1), make_note("b.md", 2)});
    const auto edges = std::make_shared<const std::vector<noteapp::core::NoteEdge>>(
        std::vector{noteapp::core::NoteEdge{*NotePath::parse("a.md"), *NotePath::parse("b.md"),
                                            noteapp::core::EdgeKind::RelatedTo, "x\ny", true}});
    model.listing().set_edge_source([edges] { return edges; });
    model.set_query("x");
    REQUIRE(model.row_count() == 2);
    for (int row = 0; row < model.row_count(); ++row) {
        CHECK(model.reason(row).find('\n') == std::string::npos);
        CHECK(model.reason(row).find('x') != std::string::npos);
    }
}
