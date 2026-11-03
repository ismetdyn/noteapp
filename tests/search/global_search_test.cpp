// Genel arama (T55a): ad + içerik birlikte, sorgu argümanlarıyla.
#include "core/config.hpp"
#include "search/global_search.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace noteapp;
using namespace noteapp::search;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

using Paths = std::vector<std::string>;

core::SearchLimits limits_with(std::optional<std::uint64_t> results) {
    core::SearchLimits limits = core::parse_config("").config.limits.search;
    if (results)
        limits.global_results = core::Limit{*results, std::to_string(*results)};
    return limits;
}

// Geçici not klasörü + güncel indeks + üst veri.
struct World {
    TempDir dir;
    core::NoteStore store{dir.path()};
    ContentIndex index{store, 1024 * 1024, 3};
    std::vector<core::NoteMeta> notes;

    void note(std::string_view path, std::string_view content) {
        noteapp::test::upsert_note(dir.path(), path, content);
    }

    void refresh() {
        index.refresh();
        notes = store.list_meta();
    }

    // limit verilmezse limits.search.global_results'ın varsayılanı (defaults.toml).
    std::vector<GlobalHit> search(std::string_view query, std::optional<std::uint64_t> limit = std::nullopt) {
        refresh();
        auto hits = search_all(query, notes, index, limits_with(limit));
        REQUIRE(hits.has_value());
        return *hits;
    }

    Paths paths(std::string_view query, std::optional<std::uint64_t> limit = std::nullopt) {
        Paths result;
        for (const GlobalHit& hit : search(query, limit))
            result.push_back(hit.path.string());
        return result;
    }
};

const GlobalHit& only(const std::vector<GlobalHit>& hits) {
    REQUIRE(hits.size() == 1);
    return hits.front();
}

}  // namespace

// L3: sonuç sayısı limits.search.global_results'tır; 0 hepsini verir (eski 200 tavanı yok).
TEST_CASE("global results follow limits.search.global_results and 0 returns every match") {
    World w;
    constexpr std::size_t kNotes = 250;
    for (std::size_t i = 0; i < kNotes; ++i)
        w.note("plan" + std::to_string(i) + ".md", "x");
    CHECK(w.search("plan").size() == 200);
    CHECK(w.search("plan", 7).size() == 7);
    CHECK(w.search("plan", 0).size() == kNotes);
}

TEST_CASE("a name match carries its positions in the path and no content") {
    World w;
    w.note("work/meeting-notes.md", "başka bir şey");
    w.note("diger.txt", "yok");

    const auto hits = w.search("wmn");
    const GlobalHit& hit = only(hits);
    CHECK(hit.path.string() == "work/meeting-notes.md");
    CHECK(hit.name_matched);
    CHECK_FALSE(hit.content_matched);
    CHECK(hit.name_positions == std::vector<std::size_t>{0, 5, 13});
    CHECK(hit.content_hits == 0);
    CHECK_FALSE(hit.best_line.has_value());
    CHECK(w.notes[hit.index].path == hit.path);
}

TEST_CASE("a content match carries the best line, its snippet and ranges") {
    World w;
    w.note("a.md", "ilk satır\nikinci bütçe satırı\n");

    const auto hits = w.search("bütçe");
    const GlobalHit& hit = only(hits);
    CHECK_FALSE(hit.name_matched);
    CHECK(hit.content_matched);
    CHECK(hit.name_positions.empty());
    CHECK(hit.content_hits == 1);
    REQUIRE(hit.best_line.has_value());
    CHECK(hit.best_line->line == 2);
    CHECK(hit.best_line->snippet == "ikinci bütçe satırı");
    REQUIRE(hit.best_line->ranges.size() == 1);
    REQUIRE(hit.best_line.has_value());
    const auto [begin, end] = hit.best_line->ranges[0];
    REQUIRE(hit.best_line.has_value());
    CHECK(hit.best_line->snippet.substr(begin, end - begin) == "bütçe");
}

TEST_CASE("a note matching in both name and content gives one result with both") {
    World w;
    w.note("butce.md", "bu yılın bütçesi\n");
    const auto hits = w.search("bütçe");
    const GlobalHit& hit = only(hits);
    CHECK(hit.name_matched);
    CHECK(hit.content_matched);
    CHECK_FALSE(hit.name_positions.empty());
    CHECK(hit.best_line.has_value());
}

TEST_CASE("the best line prefers more distinct terms over repetition and earlier lines") {
    World w;
    w.note("a.md", "proje proje proje\nbaşka\nproje ve bütçe\nbütçe proje yine\n");
    const auto hits = w.search("proje bütçe");
    const GlobalHit& hit = only(hits);
    CHECK(hit.content_hits == 3);
    REQUIRE(hit.best_line.has_value());
    CHECK(hit.best_line->line == 3);  // iki terim; 4. satır da iki terim ama daha geç
    CHECK(hit.best_line->ranges.size() == 2);
}

TEST_CASE("strong name matches lead, content hits follow, weak name matches come last") {
    World w;
    w.note("zzz/plan.md", "içerik yok\n");          // güçlü ad
    w.note("planlama-notlari.md", "içerik yok\n");  // güçlü ad
    w.note("icerik.md", "plan plan\nplan\n");       // yalnız içerik, çok satır
    w.note("yine.md", "plan\n");                    // yalnız içerik
    w.note("pilot-kayin.md", "içerik yok\n");       // zayıf ad (dağınık alt dizi)

    const Paths order = w.paths("plan");
    REQUIRE(order.size() == 5);
    CHECK(order[0] == "zzz/plan.md");
    CHECK(order[1] == "planlama-notlari.md");
    CHECK(order[2] == "icerik.md");
    CHECK(order[3] == "yine.md");
    CHECK(order[4] == "pilot-kayin.md");
}

TEST_CASE("content hits still appear when strong name matches exceed the limit") {
    World w;
    for (int i = 0; i < 30; ++i)
        w.note("plan-" + std::to_string(i) + ".md", "yok\n");
    w.note("icerik.md", "plan burada\n");

    const auto hits = w.search("plan", 8);
    REQUIRE(hits.size() == 8);
    CHECK(hits.front().name_matched);
    CHECK(hits.back().path.string() == "icerik.md");
    CHECK(hits.back().content_matched);

    // Az içerik sonucu varsa ayrılan yer boşa gitmez; hiç yoksa limit adlarla dolar.
    CHECK(w.search("plan", 100).size() == 31);
    CHECK(w.search("plan-", 8).size() == 8);
    CHECK(w.search("plan", 0).size() == 31);  // limits.search.global_results = 0: sınırsız (L3)
    CHECK(w.search("plan", 1).size() == 1);
}

TEST_CASE("every term must match: names by fuzzy with merged positions, content in the same note") {
    World w;
    w.note("work/meeting-notes.md", "yok\n");
    w.note("work/other.md", "yok\n");
    w.note("x.md", "toplantı ve bütçe ayrı\nsatırlarda olabilir\n");
    w.note("y.md", "yalnız toplantı\n");

    const auto named = w.search("meet notes");
    const GlobalHit& hit = only(named);
    CHECK(hit.path.string() == "work/meeting-notes.md");
    CHECK(hit.name_positions.size() == 9);
    CHECK(std::is_sorted(hit.name_positions.begin(), hit.name_positions.end()));

    CHECK(w.paths("toplantı satırlarda") == Paths{"x.md"});
    CHECK(w.paths("\"toplantı ve bütçe\"") == Paths{"x.md"});
    CHECK(w.paths("\"bütçe toplantı\"").empty());
}

TEST_CASE("search_all honors filters") {
    World w;
    w.note("work/plan.md", "plan\n");
    w.note("work/plan.txt", "plan\n");
    w.note("ev/plan.md", "plan\n");
    w.note("work/icerik.md", "plan\n");

    CHECK(w.paths("plan type:txt") == Paths{"work/plan.txt"});
    const Paths in_work = w.paths("plan in:work/ type:md");
    CHECK(in_work == Paths{"work/plan.md", "work/icerik.md"});
    CHECK(w.paths("plan sync:new").empty());  // sync_status doldurulmadı: hepsi sync_off
    // created ve origin gerçek değerdir (G4): yeni notlar son 7 gündedir, köken derleme platformudur.
    CHECK(w.paths("plan created:7d").size() == 4);
    CHECK(w.paths("plan origin:android").empty());
#ifdef _WIN32
    CHECK(w.paths("plan origin:windows").size() == 4);
#elif defined(__APPLE__)
    CHECK(w.paths("plan origin:macos").size() == 4);
#else
    CHECK(w.paths("plan origin:linux").size() == 4);
#endif
}

TEST_CASE("an explicit sort replaces relevance and ties keep relevance order") {
    World w;
    w.note("b/plan.md", "1234567");
    w.note("plan-a.md", "12");
    w.note("c.md", "plan 12345");
    CHECK(w.paths("plan sort:size") == Paths{"plan-a.md", "b/plan.md", "c.md"});
    CHECK(w.paths("plan sort:-size") == Paths{"c.md", "b/plan.md", "plan-a.md"});
    CHECK(w.paths("plan sort:name") == Paths{"c.md", "plan-a.md", "b/plan.md"});
}

TEST_CASE("arguments without text list the filtered notes with no matches") {
    World w;
    w.note("a.md", "1");
    w.note("b.txt", "22");
    w.note("c.md", "333");
    const auto hits = w.search("type:md sort:-size");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].path.string() == "c.md");
    CHECK(hits[1].path.string() == "a.md");
    CHECK_FALSE(hits[0].name_matched);
    CHECK_FALSE(hits[0].content_matched);

    CHECK(w.search("").size() == 3);
    CHECK(w.search("", 2).size() == 2);
}

TEST_CASE("an invalid argument comes back as a located error") {
    World w;
    w.note("a.md", "x");
    w.refresh();
    const auto result = search_all("plan size:abc", w.notes, w.index, limits_with(std::nullopt));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().begin == 10);
    CHECK(result.error().end == 13);
}

TEST_CASE("Turkish folding applies to names and content together") {
    World w;
    w.note("Öğrenci.md", "yok\n");
    w.note("liste.txt", "ÖĞRENCİ listesi\n");
    const Paths found = w.paths("ogrenci");
    CHECK(found == Paths{"Öğrenci.md", "liste.txt"});
}

TEST_CASE("binary notes, trashed notes and notes outside the given list are not found by content") {
    World w;
    w.note("ikili.bin", std::string("plan\0plan", 9));
    w.note("cop.md", "plan\n");
    w.note("canli.md", "plan\n");
    REQUIRE(w.store.trash(*core::NotePath::parse("cop.md")).has_value());
    CHECK(w.paths("plan") == Paths{"canli.md"});

    // İndeksten daha eski bir notes listesi: listede olmayan not dönmez.
    w.note("yeni.md", "plan\n");
    w.index.refresh();
    const auto result = search_all("plan", w.notes, w.index, limits_with(std::nullopt));
    REQUIRE(result.has_value());
    CHECK(only(*result).path.string() == "canli.md");
}
