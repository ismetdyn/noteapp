// A2: TUI ve GUI'nin ortak listeleme kuralı (ağaç, Ayrıntılar, düz arama). TUI list_tree/tui_state ve GUI
// browser/note_list_model/search_args testlerinin özü buraya taşındı; farklar plan-parity kararlarıyla
// çözüldü (sıralama tercihe göre, bozuk sorguda son geçerli sorgu bu listede yeniden kurulur).
#include "app/listing.hpp"
#include "core/config.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using noteapp::app::ContentSearcher;
using noteapp::app::GlobalSearcher;
using noteapp::app::Listing;
using noteapp::app::ListRow;
using noteapp::app::same_listing;
using noteapp::app::SearchMode;
using noteapp::core::NoteInfo;
using noteapp::core::NoteMeta;
using noteapp::core::NotePath;
using noteapp::core::SyncState;
using noteapp::search::SortField;
using noteapp::search::SortSpec;

namespace {

using Strings = std::vector<std::string>;
using Offsets = std::vector<std::size_t>;
using Numbered = std::vector<std::pair<std::string, std::size_t>>;

constexpr SortSpec kByName{SortField::Name, false};

NoteInfo make_note(const std::string& path, int mtime_minutes = 0, std::uintmax_t size = 10) {
    return {*NotePath::parse(path), std::filesystem::file_time_type{} + std::chrono::minutes(mtime_minutes),
            size};
}

NoteInfo tagged(const std::string& path, int mtime_minutes, std::vector<std::string> tags) {
    NoteInfo note = make_note(path, mtime_minutes);
    note.tags = std::move(tags);
    return note;
}

std::vector<NoteInfo> sample() {
    return {make_note("a.md", 1), make_note("work/b.txt", 2)};
}

Strings names(const std::vector<ListRow>& rows) {
    Strings out;
    for (const ListRow& row : rows)
        out.push_back(row.is_folder ? row.name + "/" : row.name);
    return out;
}

Strings paths(const std::vector<ListRow>& rows) {
    Strings out;
    for (const ListRow& row : rows)
        out.push_back(row.path.string());
    return out;
}

Numbered numbers(const std::vector<ListRow>& rows) {
    Numbered out;
    for (const ListRow& row : rows)
        out.emplace_back(row.path.string(), row.number);
    return out;
}

// TUI list_tree_test düzeni: ad sırasıyla.
Listing tree_listing() {
    Listing listing;
    listing.set_sort(kByName);
    listing.set_notes({make_note("work/a.md"), make_note("work/sub/deep/d.md"), make_note("work/sub/b.md"),
                       make_note("c.md"), make_note("archive/old.md")});
    return listing;
}

// TUI tui_state_test düzeni: büyük mtime = yeni.
std::vector<NoteInfo> trio() {
    return {make_note("work-meeting.md", 30), make_note("personal-todo.md", 20),
            make_note("snippets-sort.java", 10)};
}

// Sahte içerik araması: todo.md (3 eşleşme, ilk satır 12), sonra listede olmayan gone.md, sonra sort.java
// (1).
ContentSearcher fake_content(std::vector<std::string>* seen = nullptr) {
    return [seen](std::string_view text) {
        if (seen)
            seen->emplace_back(text);
        std::vector<noteapp::search::ContentResult> results;
        results.push_back({*NotePath::parse("personal-todo.md"),
                           3,
                           {noteapp::search::Hit{12, "buy milk", {{4, 8}}},
                            noteapp::search::Hit{20, "milk again", {{0, 4}}}}});
        results.push_back({*NotePath::parse("gone.md"), 5, {}});
        results.push_back(
            {*NotePath::parse("snippets-sort.java"), 1, {noteapp::search::Hit{1, "milk", {{0, 4}}}}});
        return results;
    };
}

}  // namespace

TEST_CASE("same_listing is true only for the same notes with the same path, revision, mtime and size") {
    const auto a = sample();
    CHECK(same_listing(a, sample()));

    auto b = sample();
    b[1].size = 11;
    CHECK_FALSE(same_listing(a, b));

    b = sample();
    b[0].mtime += std::chrono::minutes(1);
    CHECK_FALSE(same_listing(a, b));

    b = sample();
    b[0].revision.seq = 7;
    CHECK_FALSE(same_listing(a, b));

    b = sample();
    b[0].path = *NotePath::parse("c.md");
    CHECK_FALSE(same_listing(a, b));
}

TEST_CASE("same_listing treats a reordered or shorter listing as changed") {
    const auto a = sample();
    CHECK_FALSE(same_listing(a, std::vector{a[1], a[0]}));
    CHECK_FALSE(same_listing(a, std::vector{a[0]}));
}

TEST_CASE("listing: set_notes and set_folders report a change only when the input differs") {
    Listing listing;
    CHECK(listing.set_notes(sample()));
    CHECK_FALSE(listing.set_notes(sample()));
    auto touched = sample();
    touched[0].mtime += std::chrono::minutes(1);
    CHECK(listing.set_notes(touched));

    CHECK(listing.set_folders({"empty"}));
    CHECK_FALSE(listing.set_folders({"empty"}));
    CHECK(listing.set_folders({}));
}

TEST_CASE("tree: folders first, open folders show their children in place at any depth") {
    Listing listing = tree_listing();
    CHECK(names(listing.tree_rows("")) == Strings{"archive/", "work/", "c.md"});
    listing.set_open("work", true);
    CHECK(names(listing.tree_rows("")) == Strings{"archive/", "work/", "sub/", "a.md", "c.md"});
    listing.set_open("work/sub", true);
    CHECK(names(listing.tree_rows("")) ==
          Strings{"archive/", "work/", "sub/", "deep/", "b.md", "a.md", "c.md"});
    listing.set_open("work/sub/deep", true);
    const auto rows = listing.tree_rows("");
    REQUIRE(rows.size() == 8);
    CHECK(rows[3].depth == 2);
    CHECK(rows[4].depth == 3);
    CHECK(rows[4].path.string() == "work/sub/deep/d.md");
    CHECK(rows[1].is_folder);
    CHECK(rows[1].open);
    CHECK(rows[1].has_children);
    CHECK_FALSE(rows[0].open);

    listing.set_open("work", false);  // kapanınca alt açıklık kalır, yeniden açınca görünür
    CHECK(names(listing.tree_rows("")) == Strings{"archive/", "work/", "c.md"});
    CHECK(listing.is_open("work/sub"));
    listing.set_open("work", true);
    CHECK(listing.tree_rows("").size() == 8);
}

TEST_CASE("tree: numbers restart in every folder; folders and notes share the count") {
    Listing listing = tree_listing();
    listing.set_open("work", true);
    CHECK(numbers(listing.tree_rows("")) ==
          Numbered{{"archive", 1}, {"work", 2}, {"work/sub", 1}, {"work/a.md", 2}, {"c.md", 3}});
    // GUI row_numbers_test: Türkçe klasör adı.
    Listing gui;
    gui.set_sort(kByName);
    gui.set_notes({make_note("b.md"), make_note("a.md"), make_note("iş/c.md"), make_note("iş/d.md")});
    gui.set_open("iş", true);
    CHECK(numbers(gui.tree_rows("")) ==
          Numbered{{"iş", 1}, {"iş/c.md", 1}, {"iş/d.md", 2}, {"a.md", 2}, {"b.md", 3}});
}

TEST_CASE("tree: a subfolder's rows start at depth 0 and an unknown folder has no rows") {
    Listing listing = tree_listing();
    listing.set_open("work/sub", true);
    const auto rows = listing.tree_rows("work");
    REQUIRE(names(rows) == Strings{"sub/", "deep/", "b.md", "a.md"});
    CHECK(rows[0].depth == 0);
    CHECK(rows[1].depth == 1);
    CHECK(listing.tree_rows("nowhere").empty());
}

TEST_CASE("tree: the preferred sort orders folders and notes on every level") {
    Listing listing;
    listing.set_notes({make_note("old.md", 1, 300), make_note("new.md", 9, 100), make_note("mid.md", 5, 200),
                       make_note("f1/x.md", 2), make_note("f2/y.md", 8)});
    // Varsayılan: en yeni başta (TUI ListPrefs ve GUI'nin boş sorgu sırası).
    CHECK(names(listing.tree_rows("")) == Strings{"f2/", "f1/", "new.md", "mid.md", "old.md"});
    listing.set_sort({SortField::Size, true});
    CHECK(names(listing.tree_rows("")) == Strings{"f1/", "f2/", "old.md", "mid.md", "new.md"});
    listing.set_sort({SortField::Name, true});
    CHECK(names(listing.tree_rows("")) == Strings{"f2/", "f1/", "old.md", "new.md", "mid.md"});
    listing.set_sort(kByName);  // katlanmış ad: büyük harf ve Türkçe harf sırayı bozmaz
    listing.set_notes({make_note("Zeta.md"), make_note("alpha.md"), make_note("İz.md"), make_note("ıso.md")});
    CHECK(names(listing.tree_rows("")) == Strings{"alpha.md", "ıso.md", "İz.md", "Zeta.md"});
}

TEST_CASE("folder rows: only the direct children, even when a child folder is open") {
    Listing listing = tree_listing();
    listing.set_open("work/sub", true);
    const auto rows = listing.folder_rows("work");
    REQUIRE(names(rows) == Strings{"sub/", "a.md"});
    CHECK(numbers(rows) == Numbered{{"work/sub", 1}, {"work/a.md", 2}});
    CHECK(rows[0].depth == 0);
    CHECK_FALSE(rows[0].open);
    CHECK(listing.folder_rows("nowhere").empty());
}

TEST_CASE("tree: an empty folder from the store shows with no children; pruning drops vanished folders") {
    Listing listing = tree_listing();
    listing.set_folders({"empty"});
    const auto rows = listing.tree_rows("");
    REQUIRE(names(rows) == Strings{"archive/", "empty/", "work/", "c.md"});
    CHECK_FALSE(rows[1].has_children);

    listing.set_open("empty", true);
    listing.set_open("work", true);
    listing.set_open("gone", true);
    listing.set_folders({});
    listing.prune_open_folders();
    CHECK(listing.open_folders() == std::set<std::string>{"work"});
}

TEST_CASE("search: an empty query lists every note in the preferred order, numbered by position") {
    Listing listing;
    listing.set_notes({make_note("old.md", 1), make_note("new.md", 9), make_note("mid.md", 5)});
    auto result = listing.search("", SearchMode::Name);
    CHECK_FALSE(result.error);
    REQUIRE(paths(result.rows) == Strings{"new.md", "mid.md", "old.md"});
    CHECK(numbers(result.rows) == Numbered{{"new.md", 1}, {"mid.md", 2}, {"old.md", 3}});
    CHECK(result.rows[0].positions.empty());
    CHECK(result.rows[0].name.empty());  // düz liste: tam yol gösterilir

    listing.set_sort(kByName);
    CHECK(paths(listing.search("", SearchMode::Content).rows) == Strings{"mid.md", "new.md", "old.md"});
}

TEST_CASE("search: name mode ranks by fuzzy score with highlight byte offsets") {
    Listing listing;
    listing.set_notes({make_note("alpha.md"), make_note("beta.md"), make_note("gamma.md")});
    auto rows = listing.search("bt", SearchMode::Name).rows;
    REQUIRE(paths(rows) == Strings{"beta.md"});
    CHECK(rows[0].positions == Offsets{0, 2});

    listing.set_notes({make_note("ışık.txt")});
    rows = listing.search("sk", SearchMode::Name).rows;
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].positions == Offsets{2, 6});  // ş ve k
    rows = listing.search("ISIK", SearchMode::Name).rows;
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].positions == Offsets{0, 2, 4, 6});
}

TEST_CASE("search: the scope keeps the folder's subtree and not sibling prefixes") {
    Listing listing;
    listing.set_notes({make_note("a/one.md", 3), make_note("a/b/two.md", 2), make_note("ab/three.md"),
                       make_note("root.md")});
    CHECK(paths(listing.search("", SearchMode::Name, "a").rows) == Strings{"a/one.md", "a/b/two.md"});
    CHECK(paths(listing.search("", SearchMode::Name, "a/b").rows) == Strings{"a/b/two.md"});
    CHECK(listing.search("", SearchMode::Name).rows.size() == 4);
    CHECK(paths(listing.search("one", SearchMode::Name, "a").rows) == Strings{"a/one.md"});
}

TEST_CASE("search: arguments filter and the remaining text ranks by name; sort: overrides the order") {
    Listing listing;
    listing.set_notes({tagged("plan.md", 40, {"proje"}), tagged("alt/plan2.md", 30, {"proje/alt"}),
                       tagged("ev.txt", 20, {"ev"}), make_note("kod.py", 10, 5)});
    CHECK(paths(listing.search("tag:proje", SearchMode::Name).rows) == Strings{"plan.md", "alt/plan2.md"});
    CHECK(paths(listing.search("tag:proje/alt", SearchMode::Name).rows) == Strings{"alt/plan2.md"});
    CHECK(paths(listing.search("type:md", SearchMode::Name).rows) == Strings{"plan.md", "alt/plan2.md"});
    CHECK(paths(listing.search("plan2 type:md", SearchMode::Name).rows) == Strings{"alt/plan2.md"});
    CHECK(paths(listing.search("type:py kod", SearchMode::Name).rows) == Strings{"kod.py"});
    CHECK(paths(listing.search("sort:size", SearchMode::Name).rows) ==
          Strings{"kod.py", "plan.md", "alt/plan2.md", "ev.txt"});  // eşitlikte gelen sıra
    CHECK(paths(listing.search("sort:name", SearchMode::Name).rows) ==
          Strings{"ev.txt", "kod.py", "plan.md", "alt/plan2.md"});
    CHECK(paths(listing.search("p sort:-name", SearchMode::Name).rows) ==
          Strings{"alt/plan2.md", "plan.md", "kod.py"});
}

TEST_CASE("search: sync: and view: see the states and the lookup given to the listing") {
    Listing listing;
    listing.set_notes(trio());
    CHECK(listing.search("sync:modified", SearchMode::Name).rows.empty());
    listing.set_sync_states({{*NotePath::parse("personal-todo.md"), SyncState::Modified}});
    CHECK(paths(listing.search("sync:modified", SearchMode::Name).rows) == Strings{"personal-todo.md"});
    REQUIRE(listing.meta_of(*NotePath::parse("personal-todo.md")) != nullptr);
    CHECK(listing.meta_of(*NotePath::parse("personal-todo.md"))->sync_status == SyncState::Modified);
    CHECK(listing.meta_of(*NotePath::parse("nope.md")) == nullptr);

    CHECK(listing.search("view:mine", SearchMode::Name).rows.empty());  // bağlanmamış görünüm: not geçmez
    listing.set_view_lookup(
        [](std::string_view name) -> std::expected<std::vector<NotePath>, noteapp::core::Error> {
            if (name == "mine")
                return std::vector{*NotePath::parse("work-meeting.md")};
            return std::unexpected(noteapp::core::Error{
                noteapp::core::Error::Code::NotFound, "error.view_not_found", {{"name", std::string(name)}}});
        });
    CHECK(paths(listing.search("view:mine", SearchMode::Name).rows) == Strings{"work-meeting.md"});
    const auto missing = listing.search("view:nope", SearchMode::Name);
    CHECK(missing.error.has_value());
}

TEST_CASE("search: an invalid argument reports the error and rebuilds the last valid query on this listing") {
    Listing listing;
    listing.set_notes({make_note("a/x.md", 1), make_note("a/y.md", 2), make_note("b/z.md", 3)});
    REQUIRE(listing.search("x", SearchMode::Name).rows.size() == 1);
    auto result = listing.search("x size:abc", SearchMode::Name);
    REQUIRE(result.error.has_value());
    CHECK(result.error->begin == noteapp::search::parse_query("x size:abc").error().begin);
    CHECK(paths(result.rows) == Strings{"a/x.md"});  // son geçerli sorgu: "x"

    listing.set_notes({make_note("b/z.md", 3)});  // senkron: not gitti, satırlar yeni listeden kurulur
    result = listing.search("x size:abc", SearchMode::Name);
    CHECK(result.error.has_value());
    CHECK(result.rows.empty());

    listing.set_notes({make_note("a/x.md", 1), make_note("b/x2.md", 3)});
    result = listing.search("x size:abc", SearchMode::Name, "b");
    CHECK(paths(result.rows) == Strings{"b/x2.md"});
}

TEST_CASE("search: content mode lists the searcher's results that pass the arguments and the listing") {
    Listing listing;
    listing.set_notes(trio());
    std::vector<std::string> seen;
    CHECK(listing.search("milk", SearchMode::Content).rows.empty());  // arama bağlanmadı: çökmez, boş

    listing.set_content_searcher(fake_content(&seen));
    CHECK(listing.search("", SearchMode::Content).rows.size() == 3);  // metin yok: arama çağrılmaz
    CHECK(listing.search("type:java", SearchMode::Content).rows.size() == 1);
    CHECK(seen.empty());

    auto rows = listing.search("milk", SearchMode::Content).rows;
    CHECK(seen == Strings{"milk"});
    REQUIRE(paths(rows) == Strings{"personal-todo.md", "snippets-sort.java"});  // gone.md listede yok
    CHECK(rows[0].hit_count == 3);
    REQUIRE(rows[0].hits.size() == 2);
    CHECK(rows[0].hits[0].line == 12);
    CHECK(rows[1].hit_count == 1);
    CHECK(numbers(rows) == Numbered{{"personal-todo.md", 1}, {"snippets-sort.java", 2}});

    rows = listing.search("milk type:java", SearchMode::Content).rows;
    CHECK(seen == Strings{"milk", "milk"});  // argümansız metin aranır
    CHECK(paths(rows) == Strings{"snippets-sort.java"});
    CHECK(paths(listing.search("milk sort:name", SearchMode::Content).rows) ==
          Strings{"personal-todo.md", "snippets-sort.java"});
    CHECK(paths(listing.search("milk sort:-name", SearchMode::Content).rows) ==
          Strings{"snippets-sort.java", "personal-todo.md"});
    CHECK(listing.search("milk", SearchMode::Content, "work").rows.empty());
}

TEST_CASE("search: global mode passes the whole query and the scope's notes; rows carry the best line") {
    Listing listing;
    listing.set_notes({make_note("work/a.md", 2), make_note("work/b.md", 1), make_note("c.md", 3)});
    std::string seen_query;
    Strings seen_notes;
    listing.set_global_searcher(
        [&](std::string_view query, std::span<const NoteMeta> notes)
            -> std::expected<std::vector<noteapp::search::GlobalHit>, noteapp::search::QueryError> {
            seen_query = query;
            seen_notes.clear();
            for (const NoteMeta& meta : notes)
                seen_notes.push_back(meta.path.string());
            std::vector<noteapp::search::GlobalHit> hits;
            hits.push_back(
                {1, notes[1].path, true, true, {0}, 4, noteapp::search::Hit{41, "aranan kelime", {}}});
            hits.push_back({0, notes[0].path, true, false, {1, 2}});
            return hits;
        });
    CHECK(listing.search("", SearchMode::Global).rows.size() == 3);  // metin yok: arama çağrılmaz
    CHECK(seen_query.empty());

    const auto rows = listing.search("aranan type:md", SearchMode::Global, "work").rows;
    CHECK(seen_query == "aranan type:md");
    CHECK(seen_notes == Strings{"work/a.md", "work/b.md"});
    REQUIRE(paths(rows) == Strings{"work/b.md", "work/a.md"});  // sırayı arama verir
    CHECK(rows[0].positions == Offsets{0});
    CHECK(rows[0].hit_count == 4);
    REQUIRE(rows[0].hits.size() == 1);
    CHECK(rows[0].hits[0].line == 41);
    CHECK(rows[0].show_best_line);
    CHECK_FALSE(rows[1].show_best_line);
    CHECK(rows[1].hits.empty());
    CHECK(rows[1].number == 2);
}

TEST_CASE("search: a global searcher error is reported and the last valid rows are rebuilt") {
    Listing listing;
    listing.set_notes(trio());
    listing.set_global_searcher(
        [](std::string_view query, std::span<const NoteMeta> notes)
            -> std::expected<std::vector<noteapp::search::GlobalHit>, noteapp::search::QueryError> {
            if (query == "zzz")
                return std::unexpected(noteapp::search::QueryError{
                    0, 3,
                    noteapp::core::Error{
                        noteapp::core::Error::Code::InvalidPath, "error.query_invalid", {}}});
            std::vector<noteapp::search::GlobalHit> hits;
            for (std::size_t i = 0; i < notes.size(); ++i)
                if (notes[i].path.string().find(query) != std::string::npos)
                    hits.push_back({i, notes[i].path});
            return hits;
        });
    REQUIRE(listing.search("todo", SearchMode::Global).rows.size() == 1);
    const auto result = listing.search("zzz", SearchMode::Global);
    CHECK(result.error.has_value());
    CHECK(paths(result.rows) == Strings{"personal-todo.md"});
}

// TUI ve GUI aynı anahtar: limits.search.content_results (0 = tüm sonuçlar).
TEST_CASE("content_searcher caps results by limits.search.content_results and 0 returns all") {
    using namespace noteapp;
    const test::TempDir dir;
    for (const char* name : {"a.md", "b.md", "c.md"})
        test::upsert_note(dir.path(), name, "ortak kelime");
    const core::NoteStore store(dir.path());
    search::ContentIndex index(store, SIZE_MAX, 3);
    index.refresh();
    core::Config config = core::parse_config("").config;

    REQUIRE(core::set_config_value(config, "limits.search.content_results", "2"));
    CHECK(app::content_searcher([&]() -> const search::ContentIndex& { return index; },
                                config.limits.search.content_results)("kelime")
              .size() == 2);
    REQUIRE(core::set_config_value(config, "limits.search.content_results", "0"));
    CHECK(app::content_searcher([&]() -> const search::ContentIndex& { return index; },
                                config.limits.search.content_results)("kelime")
              .size() == 3);
}
