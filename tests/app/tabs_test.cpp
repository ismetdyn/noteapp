// T3: editör sekmelerinin iki arayüzde ortak kuralları (app/tabs). Sekme içeriği arayüzdedir; sıra, sınır,
// kalıcılık ve taşıma burada sınanır.
#include "app/tabs.hpp"
#include "core/config.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <optional>
#include <set>
#include <string>
#include <vector>

using noteapp::app::can_add_tab;
using noteapp::app::cycled_tab;
using noteapp::app::moved_path;
using noteapp::app::neighbour_after_close;
using noteapp::app::restorable_tabs;
using noteapp::app::SavedTab;
using noteapp::app::SavedTabs;
using noteapp::core::Limit;
using noteapp::core::NotePath;

namespace {

NotePath np(const std::string& path) {
    return NotePath::parse(path).value();
}

}  // namespace

TEST_CASE("tabs: restoring skips gone, duplicate and malformed paths and keeps the active tab") {
    const std::set<std::string> present{"a.md", "b.md", "c.md"};
    const auto exists = [&](const NotePath& path) { return present.contains(path.string()); };
    const SavedTabs saved{{{"gone.md"}, {"a.md"}, {"../x.md"}, {"a.md"}, {"c.md", true, true}}, 4};
    const auto restored = restorable_tabs(saved, exists, Limit{});
    REQUIRE(restored.tabs.size() == 2);
    CHECK(restored.tabs[0].path == np("a.md"));
    CHECK(restored.tabs[1].path == np("c.md"));
    CHECK(restored.tabs[1].pinned);
    CHECK(restored.tabs[1].kept);
    CHECK(restored.active == 1);
    // Etkin girdi atlandıysa ilk sekme.
    CHECK(restorable_tabs({{{"a.md"}, {"gone.md"}}, 1}, exists, Limit{}).active == 0);
}

TEST_CASE("tabs: the tab limit bounds restoring and adding, 0 is unlimited") {
    const auto exists = [](const NotePath&) { return true; };
    const SavedTabs saved{{{"a.md"}, {"b.md"}, {"c.md"}}, 2};
    const auto capped = restorable_tabs(saved, exists, Limit{2, "2"});
    CHECK(capped.tabs.size() == 2);
    CHECK(capped.active == 0);  // etkin sekme sınırın dışında kaldı
    CHECK(restorable_tabs(saved, exists, Limit{}).tabs.size() == 3);
    CHECK(can_add_tab(1, Limit{2, "2"}));
    CHECK_FALSE(can_add_tab(2, Limit{2, "2"}));
    CHECK(can_add_tab(100000, Limit{}));
}

TEST_CASE("tabs: next and previous wrap, a closed tab hands over to its right neighbour") {
    CHECK(cycled_tab(2, 1, 3) == 0);
    CHECK(cycled_tab(0, -1, 3) == 2);
    CHECK(cycled_tab(1, 1, 3) == 2);
    CHECK(cycled_tab(0, 1, 0) == 0);
    CHECK(neighbour_after_close(1, 2) == 1);  // sağdaki kaydı, aynı sırada
    CHECK(neighbour_after_close(2, 2) == 1);  // en sağdaydı: soldaki
    CHECK(neighbour_after_close(0, 0) == 0);
}

TEST_CASE("tabs: a moved note or folder carries the paths under it") {
    CHECK(moved_path(np("f/n.md"), "f", "g") == np("g/n.md"));
    CHECK(moved_path(np("f/sub/n.md"), "f", "g/h") == np("g/h/sub/n.md"));
    CHECK(moved_path(np("a.md"), "a.md", "b/a.md") == np("b/a.md"));
    CHECK_FALSE(moved_path(np("ff/n.md"), "f", "g"));  // önek klasör değil
    CHECK_FALSE(moved_path(np("x.md"), "f", "g"));
}

TEST_CASE("limits.app.tabs defaults to 0 (unlimited) and takes a count") {
    CHECK(noteapp::core::parse_config("").config.limits.app.tabs.unlimited());
    const auto sized = noteapp::core::parse_config("[limits.app]\ntabs = 8\n");
    CHECK(sized.config.limits.app.tabs == Limit{8, "8"});
}

namespace {

struct Entry {
    std::uint64_t id = 0;
    bool pinned = false;
    bool kept = false;
    std::string path;
};

using List = noteapp::app::TabList<Entry>;

std::vector<std::string> list_paths(const List& list) {
    std::vector<std::string> out;
    for (const Entry& tab : list)
        out.push_back(tab.path);
    return out;
}

List three() {
    List list;
    for (const char* path : {"a.md", "b.md", "c.md"})
        list.push(Limit{})->path = path;
    return list;
}

}  // namespace

// SD11b: TUI EditorTabs ve GUI GuiState aynı sekme listesini kullanır.
TEST_CASE("tab list: push stops at the limit and ids stay unique after closing") {
    List list;
    const Limit two{2, "2"};
    REQUIRE(list.push(two));
    const std::uint64_t second = list.push(two)->id;
    CHECK_FALSE(list.push(two));
    CHECK(list.size() == 2);
    CHECK(list.active_id() == 0);  // push etkinleştirmez
    list.close(second);
    CHECK(list.push(two)->id != second);
}

TEST_CASE("tab list: closing the active tab activates the right neighbour, else the left") {
    List list = three();
    const std::vector<std::uint64_t> ids = {
        list.find_if([](const Entry& t) { return t.path == "a.md"; })->id,
        list.find_if([](const Entry& t) { return t.path == "b.md"; })->id,
        list.find_if([](const Entry& t) { return t.path == "c.md"; })->id};
    list.activate(ids[1]);
    CHECK(list.close(ids[1]));
    CHECK(list.active_id() == ids[2]);
    CHECK(list.close(ids[2]));
    CHECK(list.active_id() == ids[0]);
    CHECK_FALSE(list.close(12345));  // bilinmeyen: değişmez
    CHECK(list.close(ids[0]));
    CHECK(list.active_id() == 0);
    CHECK(list.empty());

    // Birden çok sekme birden kapanır: etkinin solundakiler sayılır.
    List many = three();
    many.activate(many.find_if([](const Entry& t) { return t.path == "b.md"; })->id);
    CHECK(many.close_if([](const Entry& t) { return t.path != "c.md"; }));
    CHECK(many.active()->path == "c.md");
    // Etkin olmayanın kapanması etkin sekmeye dokunmaz.
    List other = three();
    other.activate(other.front().id);
    CHECK_FALSE(other.close_if([](const Entry& t) { return t.path == "b.md"; }));
    CHECK(other.active()->path == "a.md");
}

TEST_CASE("tab list: cycling wraps, reorder keeps unknown and missing ids sane") {
    List list = three();
    list.activate(list.front().id);
    CHECK(list.find(list.cycled(-1))->path == "c.md");
    CHECK(list.find(list.cycled(1))->path == "b.md");
    CHECK(List{}.cycled(1) == 0);

    const std::uint64_t a = list.front().id;
    const std::uint64_t c = list.find_if([](const Entry& t) { return t.path == "c.md"; })->id;
    const std::vector<std::uint64_t> order = {c, 999, c, a};
    list.reorder(order);
    CHECK(list_paths(list) == std::vector<std::string>{"c.md", "a.md", "b.md"});
}

TEST_CASE("tab list: saved skips drafts and keeps the active index; restore replaces the list") {
    List list = three();
    list.find_if([](const Entry& t) { return t.path == "b.md"; })->path.clear();  // taslak
    Entry* last = list.find_if([](const Entry& t) { return t.path == "c.md"; });
    last->pinned = true;
    list.activate(last->id);
    const auto saved = list.saved(
        [](const Entry& tab) { return tab.path.empty() ? std::nullopt : std::optional(np(tab.path)); });
    CHECK(saved == SavedTabs{{{"a.md"}, {"c.md", true, false}}, 1});

    const auto exists = [](const NotePath& path) { return path.string() != "gone.md"; };
    List back;
    CHECK_FALSE(back.restore({{{"gone.md"}}, 0}, exists, Limit{}, [](Entry&, const auto&) {}));
    CHECK(back.empty());
    REQUIRE(back.restore(saved, exists, Limit{}, [](Entry& tab, const noteapp::app::RestoredTab& entry) {
        tab.path = entry.path.string();
    }));
    CHECK(list_paths(back) == std::vector<std::string>{"a.md", "c.md"});
    CHECK(back.active()->path == "c.md");
    CHECK(back.active()->pinned);
}
