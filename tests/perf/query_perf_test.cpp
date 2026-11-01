// SPEC-search genel arama ve sorgu argümanı bütçeleri, SPEC-sync not durumu ölçümü (T55a). Yalnız Release.
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "sync/note_status.hpp"
#include "sync/sync_fixture.hpp"
#include "sync/sync_service.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
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

// SPEC-search kabul 5: refresh ve arama ayrı ayrı ≤ 100 ms (sıcak).
TEST_CASE("global search: 5000 notes of ~5 KB, warm search and refresh within 100 ms") {
    const test::TempDir dir;
    write_corpus(dir.path());
    const core::NoteStore store{dir.path()};
    search::ContentIndex index{store, 10 * 1024 * 1024};
    index.refresh();
    std::vector<core::NoteMeta> notes = store.list_meta();
    REQUIRE(notes.size() == kNotes);

    const auto measure = [&](std::string_view query) {
        std::vector<double> samples;
        std::size_t found = 0;
        for (int run = 0; run < kRuns; ++run) {
            samples.push_back(time_ms([&] {
                const auto hits = search::search_all(query, notes, index);
                REQUIRE(hits.has_value());
                found = hits->size();
            }));
        }
        MESSAGE("search_all(" << std::string(query) << "): median " << median(samples) << " ms, " << found
                              << " hits");
        CHECK(found > 0);
        CHECK(median(samples) <= 100.0);
    };
    measure("proje bütçe");
    measure("n42");                       // ad ağırlıklı
    measure("e");                         // en kötü: her not eşleşir, limit kadar kesit
    measure("proje type:md sort:-size");  // argümanlı

    std::vector<double> refresh;
    for (int run = 0; run < kRuns; ++run)
        refresh.push_back(time_ms([&] {
            index.refresh();
            notes = store.list_meta();
        }));
    MESSAGE("refresh + list_meta, no changes: median " << median(refresh) << " ms");
    CHECK(median(refresh) <= 100.0);
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
