// SPEC-search genel arama ve sorgu argümanı bütçeleri, SPEC-sync not durumu ölçümü (T55a). Yalnız Release.
#include "app/listing.hpp"
#include "app/map_query.hpp"
#include "core/config.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "sync/note_status.hpp"
#include "sync/sync_fixture.hpp"
#include "sync/sync_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace noteapp;
namespace fs = std::filesystem;

namespace {

constexpr int kNotes = 5000;
constexpr int kRuns = 5;
using Clock = std::chrono::steady_clock;

template <typename Work> double time_ms(Work&& work) {
    const auto start = Clock::now();
    work();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

// perf_test.cpp'deki içerik araması ölçümüyle aynı korpus: 5.000 not × ~5 KB, 2.000 sözcük.
void write_corpus(const fs::path& dir) {
    std::mt19937 rng{42};
    std::vector<std::string> words;
    for (int i = 0; i < 2000; ++i) {
        std::string word;
        for (int j = 0, n = 3 + static_cast<int>(rng() % 8); j < n; ++j)
            word += static_cast<char>('a' + rng() % 26);
        words.push_back(word);
    }
    words[0] = "proje";
    words[1] = "bütçe";
    std::vector<core::ImportedNote> notes;
    for (int n = 0; n < kNotes; ++n) {
        std::string text;
        while (text.size() < 5000) {
            text += words[rng() % words.size()];
            text += (rng() % 12 == 0) ? '\n' : ' ';
        }
        notes.push_back(core::ImportedNote{
            core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md").value(),
            std::move(text)});
    }
    test::bulk_seed(dir, notes);
}

}  // namespace

// Saf işlem: sistem çağrısı yok, 5.000 not üstünde süzme + sıralama.
TEST_CASE("query apply: 5000 notes filtered and sorted within 20 ms") {
    std::vector<core::NoteMeta> notes;
    for (int i = 0; i < kNotes; ++i) {
        notes.push_back(
            core::NoteMeta{.path = core::NotePath::parse("work/projects/folder" + std::to_string(i % 40) +
                                                         "/toplantı-notu-" + std::to_string(i) +
                                                         (i % 2 == 0 ? ".md" : ".txt"))
                                       .value(),
                           .size = static_cast<std::uintmax_t>(i * 37 % 100000),
                           .updated_at = fs::file_time_type::clock::now() - std::chrono::hours(i % 1000)});
    }
    const auto parsed = search::parse_query("type:md in:work/ size:>1kb updated:30d sort:-name");
    REQUIRE(parsed.has_value());
    std::vector<double> samples;
    std::size_t kept = 0;
    for (int run = 0; run < kRuns; ++run)
        samples.push_back(
            time_ms([&] { kept = search::apply(parsed->filters, parsed->sort, notes).size(); }));
    MESSAGE("query apply(5000 notes): median " << median(samples) << " ms, " << kept << " kept");
    CHECK(kept > 0);
    CHECK(median(samples) <= 20.0);
}

// F3: harita sorgu çubuğunun okuması (app::MapQuery: sınıflandırma, Cypher, düğümlerin not yoluyla adlanması,
// tablo ve vurgu) 5.000 notu döndürürken liste süzgecinin bütçesinde (yukarıdaki query apply, 20 ms).
TEST_CASE("map query: a read returning 5000 notes, with its table and highlight, within 20 ms") {
    const test::TempDir dir;
    std::vector<core::ImportedNote> notes;
    notes.reserve(kNotes);
    for (int n = 0; n < kNotes; ++n)
        notes.push_back(core::ImportedNote{
            core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md").value(),
            "not " + std::to_string(n) + "\n"});
    test::bulk_seed(dir.path(), notes);
    core::NoteStore store{dir.path()};
    app::MapQuery query;
    constexpr std::string_view kText = "MATCH (n:Note) RETURN n";
    REQUIRE(query.run(store, kText) == app::MapQuery::Step::Ran);  // ısınma: görüntü kurulur
    std::vector<double> samples;
    std::size_t rows = 0;
    std::size_t lit = 0;
    for (int run = 0; run < kRuns; ++run)
        samples.push_back(time_ms([&] {
            REQUIRE(query.run(store, kText) == app::MapQuery::Step::Ran);
            rows = query.outcome()->table.rows.size();
            lit = query.outcome()->highlight.notes.size();
        }));
    MESSAGE("map query(5000 notes): median " << median(samples) << " ms, " << rows << " rows, " << lit << " lit");
    CHECK(rows == kNotes);
    CHECK(lit == kNotes);
    CHECK(median(samples) <= 20.0);
}

// İnceleme (F3): 100.000 satırlık, dört sütunlu sonucun (iki not düğümü, iki değer) adlandırılması ve tablosu
// ölçülür. Adlandırma sorgunun süre bütçesine (limits.query.max_duration, varsayılan 5 sn) sayılır; toplam o
// bütçenin içinde kalmalı.
TEST_CASE("map query: naming and tabling a 100000-row result stays inside the default 5 s duration budget") {
    const test::TempDir dir;
    std::vector<core::ImportedNote> notes;
    notes.reserve(kNotes);
    for (int n = 0; n < kNotes; ++n)
        notes.push_back(core::ImportedNote{
            core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md").value(),
            "not " + std::to_string(n) + "\n"});
    test::bulk_seed(dir.path(), notes);
    constexpr std::string_view kText = "MATCH (a:Note), (b:Note) RETURN a, b, a.name, b.name LIMIT 100000";
    double engine_ms = 0;
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        engine_ms = time_ms([&] { REQUIRE(raw->query(kText).has_value()); });
    }
    core::NoteStore store{dir.path()};
    REQUIRE(core::run_user_query(store, "MATCH (n:Note) RETURN n LIMIT 1", core::QueryAction::Read, {}));  // ısınma
    std::size_t rows = 0;
    const double core_ms = time_ms([&] {
        const auto done = core::run_user_query(store, kText, core::QueryAction::Read, {});
        REQUIRE(done.has_value());
        rows = done->result.rows.size();
    });
    app::MapQuery query;
    const double app_ms = time_ms([&] { REQUIRE(query.run(store, kText) == app::MapQuery::Step::Ran); });
    MESSAGE("100000 rows x 4 columns: engine " << engine_ms << " ms, + naming " << core_ms << " ms, + table "
                                               << app_ms << " ms");
    CHECK(rows == 100000);
    CHECK(app_ms <= 5000.0);
}

// SPEC-search kabul 5: refresh ve arama ayrı ayrı ≤ 100 ms (sıcak).
TEST_CASE("global search: 5000 notes of ~5 KB, warm search and refresh within 100 ms") {
    const test::TempDir dir;
    write_corpus(dir.path());
    const core::NoteStore store{dir.path()};
    const core::SearchLimits limits = core::parse_config("").config.limits.search;
    search::ContentIndex index{store, 10 * 1024 * 1024, limits.content_hits.cap()};
    index.refresh();
    std::vector<core::NoteMeta> notes = store.list_meta();
    REQUIRE(notes.size() == kNotes);

    // İlişki araması aynı bütçededir: not başına bir [[bağlantı]] ve bir adlı ilişki (10.000 kenar), her
    // onuncu notta etiket. Kenarlar arayüzdeki gibi bellekte hazırdır (NoteStore::shared_edges önbelleği).
    std::vector<core::NoteEdge> edges;
    const std::array<std::string, 4> names{"uses", "cites", "proje planı", "depends on"};
    for (std::size_t n = 0; n < notes.size(); ++n) {
        edges.push_back({notes[n].path, notes[(n * 7 + 1) % notes.size()].path});
        edges.push_back({.from = notes[n].path,
                         .to = notes[(n * 13 + 5) % notes.size()].path,
                         .kind = core::EdgeKind::RelatedTo,
                         .name = names[n % names.size()]});
        if (n % 10 == 0)
            notes[n].tags = {"proje/" + std::to_string(n % 100)};
    }

    const auto measure = [&](std::string_view query, std::optional<std::size_t> depth = 1) {
        std::vector<double> samples;
        std::size_t found = 0;
        for (int run = 0; run < kRuns; ++run) {
            samples.push_back(time_ms([&] {
                const auto hits =
                    search::search_all(query, notes, index, limits, {}, search::GraphInput{edges, depth});
                REQUIRE(hits.has_value());
                found = hits->size();
            }));
        }
        MESSAGE("search_all(" << std::string(query) << ", depth " << (depth ? std::to_string(*depth) : "off")
                              << "): median " << median(samples) << " ms, " << found << " hits");
        CHECK(found > 0);
        CHECK(median(samples) <= 100.0);
    };
    measure("proje bütçe");
    measure("n42");                        // ad ağırlıklı
    measure("e");                          // en kötü: her not eşleşir, limit kadar kesit
    measure("proje type:md sort:-size");   // argümanlı
    measure("n42", SIZE_MAX);              // en kötü komşu gezintisi: bağlı bileşenin tamamı
    measure("uses");                       // ilişki adı: 1.250 kenarın iki ucu
    measure("rel:cites n4");               // ilişki süzgeci + ad
    measure("proje bütçe", std::nullopt);  // ilişki araması kapalı: eski yol

    std::vector<double> refresh;
    for (int run = 0; run < kRuns; ++run)
        refresh.push_back(time_ms([&] {
            index.refresh();
            notes = store.list_meta();
        }));
    MESSAGE("refresh + list_meta, no changes: median " << median(refresh) << " ms");
    CHECK(median(refresh) <= 100.0);
}

// İnceleme ölçümü: çok kenarlı çizgede tuş başına liste araması (TUI ve GUI aynı app::Listing). Kenarlar
// arayüzdeki gibi bellekte hazırdır (görüntü başına paylaşılan liste); bütçe tuş başına 50 ms.
TEST_CASE("relation search: per-keystroke listing over a million edges, 5000 notes within 50 ms") {
    constexpr std::size_t kEdges = 1'000'000;
    for (const std::size_t note_count : {std::size_t{5000}, std::size_t{50'000}}) {
        std::vector<core::NoteInfo> infos;
        infos.reserve(note_count);
        for (std::size_t n = 0; n < note_count; ++n)
            infos.push_back(
                {core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md")
                     .value(),
                 fs::file_time_type{} + std::chrono::minutes(n), 100});
        auto edges = std::make_shared<std::vector<core::NoteEdge>>();
        edges->reserve(kEdges);
        const std::array<std::string, 3> names{"uses", "cites", "depends on"};
        for (std::size_t e = 0; e < kEdges; ++e) {
            const std::size_t from = e % note_count;
            const std::size_t to = (from * 31 + e / note_count + 1) % note_count;
            if (from == to)
                continue;
            if (e % 2 == 0)
                edges->push_back({infos[from].path, infos[to].path});
            else
                edges->push_back({.from = infos[from].path,
                                  .to = infos[to].path,
                                  .kind = core::EdgeKind::RelatedTo,
                                  .name = names[e % names.size()]});
        }
        app::Listing listing;
        listing.set_notes(std::move(infos));
        listing.set_edge_source([edges] { return edges; });
        // Kapalı: ilişkisiz taban çizgisi (aynı not sayısında ad araması ve satırlar).
        for (const std::optional<std::size_t> depth :
             {std::optional<std::size_t>{}, std::optional<std::size_t>{1},
              std::optional<std::size_t>{SIZE_MAX}}) {
            listing.set_relation_depth(depth);
            // "n": her yol eşleşir (ilişkisiz en çok satır: liste satırlarının kendi bedeli).
            for (const std::string_view query : {"n42", "uses", "n"}) {
                std::vector<double> samples;
                std::size_t rows = 0;
                for (int run = 0; run < kRuns; ++run)
                    samples.push_back(
                        time_ms([&] { rows = listing.search(query, app::SearchMode::Name).rows.size(); }));
                MESSAGE("listing(" << note_count << " notes, " << edges->size() << " edges, depth "
                                   << (!depth               ? std::string("off")
                                       : *depth == SIZE_MAX ? std::string("unlimited")
                                                            : std::to_string(*depth))
                                   << ", " << std::string(query) << "): median " << median(samples) << " ms, "
                                   << rows << " rows");
                CHECK((rows > 0 || !depth));
                // Bütçe SPEC'in 5.000 notudur; 50.000 not yalnız ölçülür (her notu listeleyen sorguda
                // satırların kendisi ~20 ms, ilişkiyle 30–66 ms; SPEC-search "İlişki araması").
                if (note_count == 5000)
                    CHECK(median(samples) <= 50.0);
            }
        }
    }
}

// F4 `prop:`: anahtar görüntü başına bir kez okunur (not başına bir head okuması, büyük değer yüklenmez),
// sonraki tuşlar önbellekten süzer. 10.000 notta da ölçülür (inceleme); notların %1'inin değeri 100 KB'tır.
TEST_CASE(
    "prop search: the first prop: read within 200 ms and each keystroke within 20 ms, 5000 and 10000 notes") {
    for (const int note_count : {kNotes, 2 * kNotes}) {
        CAPTURE(note_count);
        const test::TempDir dir;
        std::vector<core::ImportedNote> notes;
        for (int n = 0; n < note_count; ++n)
            notes.push_back(core::ImportedNote{
                core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md")
                    .value(),
                "not " + std::to_string(n) + "\n"});
        test::bulk_seed(dir.path(), notes);
        core::NoteStore store(dir.path());
        const graph::Params big{{"big", graph::QueryValue(std::string(100'000, 'x'))}};
        REQUIRE(
            core::run_user_query(store, "MATCH (n:Note) SET n.status = 'open'", core::QueryAction::Write, {})
                .has_value());
        REQUIRE(core::run_user_query(store, "MATCH (n:Note) WHERE n.name ENDS WITH '00' SET n.status = $big",
                                     core::QueryAction::Write, {}, big)
                    .has_value());
        REQUIRE(core::run_user_query(store,
                                     "MATCH (n:Note) WHERE n.name STARTS WITH 'n1' SET n.status = 'Done'",
                                     core::QueryAction::Write, {})
                    .has_value());
        app::Listing listing;
        listing.set_notes(store.list());
        listing.set_property_source(
            [&store](std::string_view key) { return app::search_properties(store, key); });
        std::size_t rows = 0;
        const double cold =
            time_ms([&] { rows = listing.search("prop:status=d", app::SearchMode::Name).rows.size(); });
        CHECK(rows == 0);
        std::vector<double> warm;
        for (int run = 0; run < kRuns; ++run)
            for (const std::string_view query : {"prop:status=do", "prop:status=don", "prop:status=done"})
                warm.push_back(
                    time_ms([&] { rows = listing.search(query, app::SearchMode::Name).rows.size(); }));
        MESSAGE("prop: " << note_count << " notes, first read " << cold << " ms, keystroke median "
                         << median(warm) << " ms, " << rows << " rows");
        // n1, n10–n19, n100–n199, n1000–n1999 (10.000'de n10000 yok); `n1…00` büyük değerini yitirdi.
        CHECK(rows == 1111);
        CHECK(cold <= 200.0);
        CHECK(median(warm) <= 20.0);
    }
}

// Durum, kayıtlı yazma numarasıyla karşılaştırmadır: libgit2 geçişi yok. Yine de 5.000 notta sayıya bağlanır.
TEST_CASE("sync status: 5000 notes in one pass within 500 ms") {
    const sync_test::SyncFixture fixture;
    const sync_test::Device device(fixture, "perf");
    write_corpus(device.home / "data");
    const auto synced = device.sync();
    REQUIRE_MESSAGE(synced.has_value(), sync_test::describe(synced));
    // Biraz gerçekçi kirlilik: 50 değişen, 50 yeni not.
    for (int n = 0; n < 50; ++n) {
        device.put("d0/n" + std::to_string(n * 50) + ".md", "degisti");
        device.put("d0/yeni" + std::to_string(n) + ".md", "yeni");
    }

    const sync_test::EnvGuard home("NOTEAPP_HOME", device.home);
    core::Config config = device.config();
    config.sync.enabled = true;
    std::vector<core::NoteMeta> notes = device.store().list_meta();
    REQUIRE(notes.size() == kNotes + 50);
    std::vector<double> samples;
    for (int run = 0; run < kRuns; ++run) {
        samples.push_back(time_ms([&] { REQUIRE(sync::fill_sync_status(config, notes).has_value()); }));
    }
    const auto count = [&](core::SyncState state) {
        return std::ranges::count(notes, state, &core::NoteMeta::sync_status);
    };
    MESSAGE("fill_sync_status(" << notes.size() << " notes): median " << median(samples) << " ms, first "
                                << samples.front() << " ms; synced " << count(core::SyncState::Synced)
                                << ", modified " << count(core::SyncState::Modified) << ", new "
                                << count(core::SyncState::New));
    CHECK(count(core::SyncState::Modified) == 50);
    CHECK(count(core::SyncState::New) == 50);
    CHECK(count(core::SyncState::Synced) == kNotes - 50);
    CHECK(median(samples) <= 500.0);
}

// SPEC-graph kabul 8: yeni cihaz, gerçek git ve gerçek günlüklerle. Üst sınır klon + günlük uygulama +
// senkron toplamı.
TEST_CASE("new device: clone and first sync of 5000 notes of ~5 KB within 10 s") {
    const sync_test::SyncFixture fixture;
    const sync_test::Device source(fixture, "kaynak");
    write_corpus(source.home / "data");
    const auto seeded = source.sync();
    REQUIRE_MESSAGE(seeded.has_value(), sync_test::describe(seeded));
    const sync_test::Device fresh(fixture, "yeni", false);
    const double total = time_ms([&] {
        const auto enabled = fresh.enable(sync_test::utf8(fixture.remote()));
        REQUIRE_MESSAGE(enabled.has_value(), sync_test::describe(enabled));
        const auto synced = fresh.sync();
        REQUIRE_MESSAGE(synced.has_value(), sync_test::describe(synced));
    });
    CHECK(fresh.store().list().size() == kNotes);
    std::uintmax_t repo_bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fixture.remote()))
        if (entry.is_regular_file())
            repo_bytes += entry.file_size();
    MESSAGE("new device 5000 notes: " << total << " ms; remote " << repo_bytes / 1024 << " KiB");
    CHECK(total <= 10000.0);
}

// Tek dosyaya ekleme, git'te her commit'te yeni blob demek: geçmiş büyümesi ölçülür.
TEST_CASE("git growth: 200 small edits, a sync after each ten") {
    const sync_test::SyncFixture fixture;
    const sync_test::Device device(fixture, "buyume");
    std::string text(2048, 'a');
    device.put("not.md", text);
    int syncs = 0;
    for (int edit = 0; edit < 200; ++edit) {
        text += 'b';
        device.put("not.md", text);
        if (edit % 10 == 9) {
            const auto synced = device.sync();
            REQUIRE_MESSAGE(synced.has_value(), sync_test::describe(synced));
            ++syncs;
        }
    }
    std::uintmax_t repo_bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fixture.remote()))
        if (entry.is_regular_file())
            repo_bytes += entry.file_size();
    MESSAGE("200 edits of a 2 KiB note, "
            << syncs << " syncs: log "
            << fs::file_size(device.notes / "graph" /
                             *core::own_log_name(*std::make_unique<core::NoteStore>(device.store()))) /
                   1024
            << " KiB, remote git " << repo_bytes / 1024 << " KiB");
    CHECK(repo_bytes > 0);
}
