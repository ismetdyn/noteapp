// G2: yank/yapıştır ve taşıma kuralı (TUI `y` `p` `m`, GUI aynı komutlar). Kopya deponun numarasıyla adlanır
// (`-2`), deneme tavanı yoktur; taşıma çakışmada hiçbir şeyi taşımaz.
#include "app/item_ops.hpp"
#include "app/listing.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <set>
#include <string>
#include <vector>

using noteapp::app::ItemRef;
using noteapp::core::NotePath;
using noteapp::core::NoteStore;

namespace {

NotePath path(const std::string& text) {
    return *NotePath::parse(text);
}

ItemRef note(const std::string& text) {
    return {path(text), false};
}

ItemRef folder(const std::string& text) {
    return {path(text), true};
}

struct Scene {
    noteapp::test::TempDir dir;
    NoteStore store{dir.path()};
    noteapp::app::Listing list;

    void put(const std::string& name, const std::string& body) {
        REQUIRE(noteapp::test::seed_note(store, name, body));
    }
    bool live(const std::string& name) const { return store.read(path(name)).has_value(); }
    std::string body(const std::string& name) const {
        const auto content = store.read(path(name));
        return content ? content->text : "<missing>";
    }
    const noteapp::app::Listing& listing() {
        list.set_notes(store.list());
        list.set_folders(store.folders());
        return list;
    }
};

}  // namespace

TEST_CASE("mark keys keep folders apart from notes; a marked folder takes its marked children along") {
    CHECK(noteapp::app::mark_key("a/b.md", false) == "a/b.md");
    CHECK(noteapp::app::mark_key("a/b", true) == "a/b/");
    const std::set<std::string> marks{"a/", "a/x.md", "a/sub/", "b.md", "c/"};
    const std::vector<ItemRef> items = noteapp::app::marked_items(marks);
    std::vector<std::string> shown;
    for (const ItemRef& item : items)
        shown.push_back(noteapp::app::mark_key(item.path.string(), item.is_folder));
    CHECK(shown == std::vector<std::string>{"a/", "b.md", "c/"});
}

TEST_CASE("paste copies notes byte for byte and numbers a taken name like the store, without a ceiling") {
    Scene s;
    s.put("a.md", "alpha\r\n");
    s.put("dest/a.md", "taken");
    s.put("dest/a-2.md", "taken too");
    const std::vector<ItemRef> items{note("a.md")};
    const auto first = noteapp::app::paste_items(s.store, items, "dest");
    CHECK(first.done == 1);
    CHECK(first.failed == 0);
    CHECK(s.body("dest/a-3.md") == "alpha\r\n");
    REQUIRE(first.lines.size() == 1);
    CHECK(first.lines.front().renamed_to == "dest/a-3.md");
    CHECK(s.live("a.md"));  // kopya: kaynak yerinde

    // Aynı yere bir daha: bir sonraki boş numara.
    const auto again = noteapp::app::paste_items(s.store, items, "");
    CHECK(again.done == 1);
    CHECK(s.body("a-2.md") == "alpha\r\n");
}

TEST_CASE("paste copies a folder subtree under a numbered name and refuses to copy a folder into itself") {
    Scene s;
    s.put("work/x.md", "x");
    s.put("work/deep/y.md", "y");
    s.put("dest/work/z.md", "occupied");
    const std::vector<ItemRef> items{folder("work")};
    const auto pasted = noteapp::app::paste_items(s.store, items, "dest");
    CHECK(pasted.done == 1);
    CHECK(s.body("dest/work-2/x.md") == "x");
    CHECK(s.body("dest/work-2/deep/y.md") == "y");
    CHECK(s.body("dest/work/z.md") == "occupied");

    const auto into_itself = noteapp::app::paste_items(s.store, items, "work/deep");
    CHECK(into_itself.done == 0);
    CHECK(into_itself.failed == 1);
    CHECK_FALSE(s.live("work/deep/work/x.md"));

    // Kendi üst klasörüne: aynı ad kaynağın kendisidir, kopya bir sonraki boş numarayı alır.
    const auto beside = noteapp::app::paste_items(s.store, items, "");
    CHECK(beside.done == 1);
    CHECK(s.body("work-2/x.md") == "x");
}

TEST_CASE("paste reports a vanished item and carries on with the rest") {
    Scene s;
    s.put("a.md", "a");
    const std::vector<ItemRef> items{note("gone.md"), note("a.md")};
    const auto pasted = noteapp::app::paste_items(s.store, items, "dest");
    CHECK(pasted.done == 1);
    CHECK(pasted.failed == 1);
    CHECK(s.body("dest/a.md") == "a");
}

TEST_CASE("move takes notes and folders to the destination; a taken name moves nothing") {
    Scene s;
    s.put("a.md", "a");
    s.put("work/x.md", "x");
    s.put("dest/b.md", "b");
    const std::vector<ItemRef> items{note("a.md"), folder("work")};
    const auto moved = noteapp::app::move_items(s.store, s.listing(), items, "dest");
    CHECK_FALSE(moved.failure.has_value());
    CHECK(moved.moved == 2);
    REQUIRE(moved.first.has_value());
    CHECK(moved.first->path.string() == "dest/a.md");
    CHECK(s.body("dest/a.md") == "a");
    CHECK(s.body("dest/work/x.md") == "x");
    CHECK_FALSE(s.live("a.md"));

    s.put("c.md", "c");
    s.put("b.md", "root b");
    const std::vector<ItemRef> clash{note("c.md"), note("b.md")};
    const auto refused = noteapp::app::move_items(s.store, s.listing(), clash, "dest");
    REQUIRE(refused.failure.has_value());
    CHECK(refused.failure->code == noteapp::core::Error::Code::AlreadyExists);
    CHECK(refused.moved == 0);
    CHECK(s.live("c.md"));  // çakışma varken hiçbir öğe gitmez
    CHECK(s.body("dest/b.md") == "b");
}

TEST_CASE("move refuses to put a folder inside itself") {
    Scene s;
    s.put("work/deep/y.md", "y");
    const std::vector<ItemRef> items{folder("work")};
    const auto refused = noteapp::app::move_items(s.store, s.listing(), items, "work/deep");
    REQUIRE(refused.failure.has_value());
    CHECK(refused.failure->code == noteapp::core::Error::Code::InvalidPath);
    CHECK(s.live("work/deep/y.md"));
}
