// T3: editör sekmelerinin iki arayüzde ortak kuralları (app/tabs). Sekme içeriği arayüzdedir; sıra, sınır,
// kalıcılık ve taşıma burada sınanır.
#include "app/tabs.hpp"
#include "core/config.hpp"

#include <doctest/doctest.h>
#include <set>
#include <string>

using noteapp::app::can_add_tab;
using noteapp::app::cycled_tab;
using noteapp::app::format_saved_tabs;
using noteapp::app::moved_path;
using noteapp::app::neighbour_after_close;
using noteapp::app::parse_saved_tabs;
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

TEST_CASE("tabs: saved tabs survive a round trip after line based preferences") {
    const SavedTabs saved{{{"a.md", false, true}, {"dir/\"q\" b.md", true, false}}, 1};
    const std::string prefs = "sort = \"name\"\nopen_folders = [\"x\x7f\"]\n" + format_saved_tabs(saved);
    CHECK(parse_saved_tabs(prefs) == saved);
}

TEST_CASE("tabs: no tabs write nothing and a broken block reads as none") {
    CHECK(format_saved_tabs({}).empty());
    CHECK(parse_saved_tabs("sort = \"name\"\n").tabs.empty());
    CHECK(parse_saved_tabs("active_tab = 0\n[[tabs]]\npath = \n").tabs.empty());
    // Yolsuz girdi atlanır, öbürü kalır.
    const SavedTabs partial =
        parse_saved_tabs("active_tab = 1\n[[tabs]]\nkept = true\n[[tabs]]\npath = \"b.md\"\n");
    REQUIRE(partial.tabs.size() == 1);
    CHECK(partial.tabs[0].path == "b.md");
}

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
