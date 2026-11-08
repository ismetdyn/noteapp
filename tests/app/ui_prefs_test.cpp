// app::ui_prefs: TUI (tui-state.toml) ve GUI (gui-state.toml) ortak alanları tek kuralla okur ve yazar.
#include "app/ui_prefs.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using noteapp::app::ListColumn;
using noteapp::app::UiPrefs;
using noteapp::app::UiPrefsRules;

namespace {

using Strings = std::vector<std::string>;

bool always(const std::string&) {
    return true;
}

bool same(const UiPrefs& a, const UiPrefs& b) {
    return a.sort.field == b.sort.field && a.sort.descending == b.sort.descending && a.column == b.column &&
           a.search_history == b.search_history && a.open_folders == b.open_folders &&
           a.recent_commands == b.recent_commands && a.tabs == b.tabs && a.map == b.map && a.extra == b.extra;
}

}  // namespace

TEST_CASE("ui prefs: common fields and interface keys round-trip through TOML") {
    UiPrefs prefs;
    prefs.sort = {noteapp::search::SortField::Size, false};
    prefs.column = ListColumn::Origin;
    prefs.search_history = {"say \"hi\"", "back\\slash", "çay 1"};
    prefs.open_folders = {"work", "iş/alt"};
    prefs.recent_commands = {"sort", "trash"};
    prefs.tabs = {{{"a.md", true, false}, {"b.md", false, true}}, 1};
    prefs.map = {.shown = true,
                 .view = "iş günü",
                 .options = {.whole_graph = true,
                             .depth = 4,
                             .show_tags = true,
                             .show_links = false,
                             .show_related = true,
                             .relation = std::string(),  // adsız ilişkiler: "yok"tan ayrı
                             .tag = "proje"}};
    prefs.extra = {{"list_share", 0.25}, {"width", std::int64_t{900}}, {"ui_font", std::string("inter")},
                   {"row_numbers", true}};
    const UiPrefs back = noteapp::app::parse_ui_prefs(noteapp::app::format_ui_prefs(prefs, always), {});
    CHECK(same(back, prefs));
    CHECK(noteapp::app::pref_number(back, "width") == 900.0);  // tam sayı da sayıdır
    CHECK_FALSE(noteapp::app::pref_int(back, "list_share").has_value());
}

TEST_CASE("ui prefs: unknown commands drop and the list is cut at the limit like the GUI") {
    const UiPrefsRules rules{.recent_commands = {.value = 1, .text = "1"}};
    CHECK(noteapp::app::parse_ui_prefs("recent_commands = [\"bogus\", \"sort\", \"trash\"]\n", rules)
              .recent_commands == Strings{"sort"});
    CHECK(noteapp::app::parse_ui_prefs("recent_commands = [\"bogus\", \"sort\"]\n", {}).recent_commands ==
          Strings{"sort"});
}

TEST_CASE("ui prefs: open folders with control bytes or gone from the tree are not written") {
    UiPrefs prefs;
    prefs.open_folders = {"work", "gone", std::string("bad\nname")};
    const std::string text =
        noteapp::app::format_ui_prefs(prefs, [](const std::string& folder) { return folder != "gone"; });
    CHECK(noteapp::app::parse_ui_prefs(text, {}).open_folders == std::set<std::string>{"work"});
}

TEST_CASE("ui prefs: a saved tab without a path is skipped, the others stay") {
    const UiPrefs prefs = noteapp::app::parse_ui_prefs(
        "active_tab = 1\n[[tabs]]\nkept = true\n[[tabs]]\npath = \"b.md\"\n", {});
    REQUIRE(prefs.tabs.tabs.size() == 1);
    CHECK(prefs.tabs.tabs[0].path == "b.md");
}

TEST_CASE("ui prefs: broken files and unknown values fall back to the defaults") {
    CHECK(same(noteapp::app::parse_ui_prefs("sort = [", {}), UiPrefs{}));
    const UiPrefs odd = noteapp::app::parse_ui_prefs("sort = \"bogus\"\ncolumn = \"bogus\"\n", {});
    CHECK(odd.sort.field == noteapp::search::SortField::Updated);
    CHECK(odd.column == ListColumn::SizeAndDate);
    // Sürüm işareti yok: `none` seçimdir (geriye uyum yok, tek kural).
    CHECK(noteapp::app::parse_ui_prefs("column = \"none\"\n", {}).column == ListColumn::None);
    // Geçmiş arayüzün süzgecinden geçer.
    const UiPrefsRules rules{.keep_history = [](Strings entries) {
        std::erase(entries, "TOKEN=x");
        return entries;
    }};
    CHECK(noteapp::app::parse_ui_prefs("search_history = [\"a\", \"TOKEN=x\"]\n", rules).search_history ==
          Strings{"a"});
}

// Boş ağaç (ilk tarama bitmedi) açık klasörleri süzmez; kurulmuş ağaç olmayanı düşürür.
TEST_CASE("ui prefs: the open folder filter keeps everything while the tree is empty") {
    noteapp::core::FolderTree empty;
    CHECK_FALSE(noteapp::app::open_folder_filter(empty));
    noteapp::core::FolderTree tree;
    const std::vector<std::string> folders{"work"};
    tree.rebuild({}, folders);
    const auto keep = noteapp::app::open_folder_filter(tree);
    REQUIRE(keep);
    CHECK(keep("work"));
    CHECK_FALSE(keep("gone"));
}

// Bozuk ya da sınırdan büyük dosya ezilmeden önce tek yedeğe alınır; yedek birikmez.
TEST_CASE("ui prefs: a broken or oversized state file is set aside once and replaces the old backup") {
    const noteapp::test::TempDir home;
    const noteapp::test::ScopedEnv env{"NOTEAPP_HOME", noteapp::test::utf8(home.path())};
    noteapp::core::init_i18n("en");
    std::filesystem::create_directories(noteapp::core::state_dir());
    const auto file = noteapp::core::state_dir() / "x-state.toml";
    const auto backup = noteapp::core::state_dir() / "x-state.toml.bad";
    std::ofstream(file) << "sort = [\n";
    auto loaded = noteapp::app::load_ui_prefs("x-state.toml", {});
    CHECK_FALSE(loaded.notice.empty());
    CHECK(std::filesystem::exists(backup));
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(noteapp::app::load_ui_prefs("x-state.toml", {}).notice.empty());  // yeni dosya yok: bir kez

    std::ofstream(file) << "sort = \"name\"\n";
    loaded = noteapp::app::load_ui_prefs("x-state.toml", {.max_bytes = {.value = 4, .text = "4"}});
    CHECK(loaded.notice.find("4") != std::string::npos);
    std::ifstream kept(backup);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "sort = \"name\"\n");
    std::size_t copies = 0;  // durum dizininde günlük de olabilir: yalnız bu dosyanın kopyaları sayılır
    for (const auto& entry : std::filesystem::directory_iterator(noteapp::core::state_dir()))
        copies += entry.path().filename().string().starts_with("x-state.toml") ? 1 : 0;
    CHECK(copies == 1);
}

// Var olan ama okunamayan dosya (burada aynı adlı dizin; izin hatası root'ta sınanamaz) da sonraki kayıtla
// ezilmesin: kenara alınır ve söylenir.
TEST_CASE("ui prefs: a state file that exists but cannot be read is set aside too") {
    const noteapp::test::TempDir home;
    const noteapp::test::ScopedEnv env{"NOTEAPP_HOME", noteapp::test::utf8(home.path())};
    noteapp::core::init_i18n("en");
    const auto file = noteapp::core::state_dir() / "y-state.toml";
    std::filesystem::create_directories(file);
    const auto loaded = noteapp::app::load_ui_prefs("y-state.toml", {});
    CHECK_FALSE(loaded.notice.empty());
    CHECK(std::filesystem::exists(noteapp::core::state_dir() / "y-state.toml.bad"));
    CHECK_FALSE(std::filesystem::exists(file));
}

// Harita oturumu (`[map]`, TUI ve GUI ortak): yokken varsayılan; elle bozulmuş tür varsayılanda kalır,
// derinlik en az 1'dir (üstünü MapModel config'le keser); ilişki süzgeci yazılmamışsa yoktur.
TEST_CASE("ui prefs: the map session defaults when missing and keeps defaults for values of the wrong type") {
    using noteapp::app::MapOptions;
    CHECK(noteapp::app::parse_ui_prefs("sort = \"name\"\n", {}).map == noteapp::app::MapSession{});
    const UiPrefs odd = noteapp::app::parse_ui_prefs(
        "[map]\nshown = \"yes\"\nview = 3\ndepth = -5\nwhole_graph = 1\nrelation = 2\n", {});
    CHECK_FALSE(odd.map.shown);
    CHECK(odd.map.view.empty());
    CHECK(odd.map.options.depth == MapOptions::kMinDepth);
    CHECK_FALSE(odd.map.options.whole_graph);
    CHECK_FALSE(odd.map.options.relation.has_value());
    const UiPrefs deep = noteapp::app::parse_ui_prefs("[map]\nshown = true\ndepth = 99999999999\n", {});
    CHECK(deep.map.shown);
    CHECK(deep.map.options.depth > MapOptions::kDefaultDepth);
    UiPrefs unfiltered;
    unfiltered.map.shown = true;
    CHECK_FALSE(noteapp::app::parse_ui_prefs(noteapp::app::format_ui_prefs(unfiltered, always), {})
                    .map.options.relation.has_value());
}
