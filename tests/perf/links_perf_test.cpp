// T78: bağlantı paneli sorgusu. 5.000 not, 20.000 kenar; her not hub'a ve üç nota gider, yani hub'ın binlerce
// geleni vardır. Sorgu not başına değil seçim başına çalışır (önizlemeyle ertelenmiş); yine de 50 ms kare
// bütçesinin (SPEC.md §10/3) küçük bir parçası kalmalıdır.
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/tui_state.hpp"
#include "wine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <string>
#include <variant>
#include <vector>

using namespace noteapp;
namespace fs = std::filesystem;

namespace {

constexpr int kNotes = 5000;
constexpr int kLinksPerNote = 4;
constexpr int kRuns = 21;
constexpr double kNoteBudgetMs = 5.0;
constexpr double kHubBudgetMs = 25.0;
using Clock = std::chrono::steady_clock;

std::string note_path(int index) {
    return "folder" + std::to_string(index % 40) + "/note-" + std::to_string(index) + ".md";
}

// extra her gövdeye eklenir: hedefi henüz olmayan bağlantı (T92 ölçümü).
void seed_corpus(const fs::path& directory, const std::string& extra = {}) {
    std::vector<core::ImportedNote> notes;
    notes.reserve(kNotes);
    for (int i = 0; i < kNotes; ++i) {
        std::string body(1024, 'x');
        body += "[[" + note_path(0) + "]]";
        for (int step = 1; step < kLinksPerNote; ++step)
            body += " [[" + note_path((i + (step * 37)) % kNotes) + "]]";
        body += extra;
        notes.push_back(core::ImportedNote{core::NotePath::parse(note_path(i)).value(), std::move(body)});
    }
    test::bulk_seed(directory, notes);
    core::NoteStore store(directory);
    REQUIRE(store.relink_notes().has_value());
}

double median_lookup_ms(const core::NoteStore& store, const core::NotePath& path) {
    std::vector<double> samples;
    for (int run = 0; run < kRuns; ++run) {
        const auto start = Clock::now();
        const auto links = store.links(path);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        REQUIRE(links.has_value());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

}  // namespace

TEST_CASE("links perf: lookup on 5000 notes with 20000 links stays within the budget") {
    const test::TempDir dir;
    seed_corpus(dir.path());
    const core::NoteStore store(dir.path());

    const auto hub = core::NotePath::parse(note_path(0)).value();
    const auto ordinary = core::NotePath::parse(note_path(2500)).value();
    const auto hub_links = store.links(hub);
    REQUIRE(hub_links.has_value());
    CHECK(hub_links->incoming.size() == kNotes - 1);
    const auto note_links = store.links(ordinary);
    REQUIRE(note_links.has_value());
    CHECK(note_links->outgoing.size() == kLinksPerNote);

    const double hub_ms = median_lookup_ms(store, hub);
    const double note_ms = median_lookup_ms(store, ordinary);
    std::printf("links lookup: ordinary note %.2f ms, hub with %zu incoming %.2f ms\n", note_ms,
                hub_links->incoming.size(), hub_ms);
    CHECK(note_ms <= kNoteBudgetMs);
    CHECK(hub_ms <= kHubBudgetMs);
}

TEST_CASE("links perf: paging the hub in the panel is proportional to the window") {
    const test::TempDir dir;
    seed_corpus(dir.path());
    const core::NoteStore store(dir.path());
    const auto hub = core::NotePath::parse(note_path(0)).value();
    const auto links = store.links(hub);
    REQUIRE(links.has_value());

    tui::TuiState state;
    state.set_notes(store.list());
    state.set_window(120, 40);
    state.set_links(*links, 1, hub);
    REQUIRE(state.select_note(hub));
    state.focus_preview();
    state.handle(tui::Key::Char, "b");
    REQUIRE(state.focus() == tui::Focus::Links);
    const auto start = Clock::now();
    constexpr int kPages = 2000;
    for (int i = 0; i < kPages; ++i)
        state.handle(tui::Key::PageDown);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("links paging: %d pages %.2f ms\n", kPages, ms);
    CHECK(ms / kPages <= 1.0);
}

// T92: sonradan oluşan hedefin kenarları oluşturma yazmasında kurulur. Aday kaynaklar not başına saklanan
// bağlantı hedeflerinden bulunur; gövdeler taranmaz. Karşılaştırma için tam gövde taraması da ölçülür.
TEST_CASE("links perf: creating a note on 5000 notes stays bounded, also when every note links to it") {
    constexpr int kCreates = 7;
    // Yerli sınır. T93: Wine'da aynı commit ve boş makinede düz oluşturma medyanı 58,6-63,1 ms, Linux'ta
    // 25,7-30,2 ms (20'şer koşu): Wine'da çarpanlı bütçe. Hub ölçümü yerli sınırda kalır (Wine 235-246 ms).
    constexpr double kCreateBudgetMs = 60.0;
    constexpr double kHubCreateBudgetMs = 400.0;
    const test::TempDir dir;
    seed_corpus(dir.path(), " [[later]]");
    core::NoteStore store(dir.path());
    REQUIRE(store.list().size() == kNotes);

    std::vector<double> plain;
    for (int run = 0; run < kCreates; ++run) {
        const auto start = Clock::now();
        REQUIRE(test::seed_note(store, "fresh-" + std::to_string(run) + ".md", "body\n"));
        plain.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::vector<double> hub;
    const auto later = core::NotePath::parse("later.md").value();
    for (int run = 0; run < kCreates; ++run) {
        const auto start = Clock::now();
        REQUIRE(test::seed_note(store, "later.md", "later\n"));
        hub.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        const auto links = store.links(later);
        REQUIRE(links.has_value());
        CHECK(links->incoming.size() == kNotes);
        REQUIRE(store.remove(later).has_value());
    }
    std::ranges::sort(plain);
    std::ranges::sort(hub);

    // Karşılaştırma: oluşturma başına tüm gövdeleri okuyup taramak (seçilmeyen yol).
    const auto start = Clock::now();
    std::size_t sources = 0;
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        auto txn = raw->read();
        REQUIRE(txn.has_value());
        auto cursor = txn->nodes_with_label("Note");
        REQUIRE(cursor.has_value());
        while (true) {
            auto next = cursor->next();
            REQUIRE(next.has_value());
            if (!*next)
                break;
            auto body = txn->property(**next, "body");
            REQUIRE(body.has_value());
            const auto* bytes = std::get_if<graph::Bytes>(&*body);
            if (bytes != nullptr && bytes->data.find("[[later]]") != std::string::npos)
                ++sources;
        }
    }
    const double scan_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    CHECK(sources == kNotes);
    std::printf("create on 5000 notes: plain median %.2f ms, linked by 5000 median %.2f ms; full body scan "
                "%.2f ms\n",
                plain[plain.size() / 2], hub[hub.size() / 2], scan_ms);
    const double create_budget_ms =
        test::running_under_wine() ? kCreateBudgetMs * test::kWineFactor : kCreateBudgetMs;
    CHECK(plain[plain.size() / 2] <= create_budget_ms);
    CHECK(hub[hub.size() / 2] <= kHubCreateBudgetMs);
}

// T92: yeni cihazın ilk ingest'inde her not yeni yoldadır; kenar türetmesi not sayısıyla doğrusal kalmalı
// (SPEC-graph kabul 8: 5.000 notluk yeni cihaz ≤ 10 sn).
TEST_CASE("links perf: a fresh device ingesting 5000 linked notes derives the edges within the budget") {
    constexpr double kFreshDeviceBudgetMs = 10000.0;
    const test::TempDir source;
    const test::TempDir logs;
    const test::TempDir fresh;
    seed_corpus(source.path(), " [[later]]");
    {
        core::NoteStore store(source.path());
        REQUIRE(core::export_note_log(store, logs.path()).has_value());
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(logs.path()))
        files.push_back(entry.path());
    core::NoteStore device(fresh.path());
    const auto start = Clock::now();
    REQUIRE(core::ingest_note_logs(device, files).has_value());
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto hub = device.links(core::NotePath::parse(note_path(0)).value());
    REQUIRE(hub.has_value());
    CHECK(hub->incoming.size() == kNotes - 1);
    std::printf("fresh device ingest of 5000 linked notes: %.0f ms\n", ms);
    CHECK(ms <= kFreshDeviceBudgetMs);
}
