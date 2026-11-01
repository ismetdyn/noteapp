// T64: gezinme depo okuması yapmaz (G4'ten beri okuma graph.db'dendir; generation her gövde okumasında
// artar). Sayaç ölçülür, duvar saati değil: yük altında yanlış kırmızı vermez. Gerçek terminal sayıları
// (boşalma süresi, kare, tuş gecikmesi) tmux ölçümündedir; bkz. SPEC-tui "Değişiklik 2026-11-01".
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp;
namespace fs = std::filesystem;

namespace {

constexpr int kNotes = 5000;
constexpr int kMoves = 2000;
constexpr auto kKeyGap = std::chrono::milliseconds(1);

void fill(const fs::path& dir) {
    constexpr int kSizes[] = {80, 900, 4000, 20000};
    std::vector<core::ImportedNote> notes;
    for (int i = 0; i < kNotes; ++i) {
        notes.push_back(
            core::ImportedNote{core::NotePath::parse("n" + std::to_string(10000 + i) + ".md").value(),
                               std::string(static_cast<std::size_t>(kSizes[i % 4]), 'x')});
    }
    test::bulk_seed(dir, notes);
}

}  // namespace

TEST_CASE("nav perf: 2000 fast moves over 5000 notes read the store once, for the final note") {
    const test::TempDir dir;
    fill(dir.path());
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache eager;
    tui::PreviewCache lazy;
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    lazy.throttled(state, store, now);
    const std::size_t eager_start = eager.generation();
    const std::size_t lazy_start = lazy.generation();
    for (int i = 0; i < kMoves; ++i) {
        now += kKeyGap;
        state.handle(tui::Key::Down);
        eager.current(state, store);
        lazy.throttled(state, store, now);
        CHECK(lazy.deferred());
    }
    CHECK(lazy.generation() == lazy_start);
    now += std::chrono::milliseconds(tui::PreviewCache::kCoalesceMs);
    const tui::Preview& final_note = lazy.throttled(state, store, now);
    CHECK_FALSE(lazy.deferred());
    CHECK(lazy.generation() == lazy_start + 1);
    CHECK(final_note.kind == tui::Preview::Kind::Text);
    MESSAGE("reads for 2000 moves: eager ", eager.generation() - eager_start, ", coalesced ",
            lazy.generation() - lazy_start);
}

TEST_CASE("nav perf: scrolling a 1 MiB note in REVIEW reads it once") {
    const test::TempDir dir;
    std::string big;
    for (int i = 0; i < 21000; ++i)
        big += "L" + std::to_string(i) + " **metin** `kod` satiri bir miktar uzun icerik burada\n";
    test::upsert_note(dir.path(), "big.md", big);
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache cache;
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    const tui::Preview& shown = cache.throttled(state, store, now);
    state.set_preview_lines(&shown.lines, cache.generation());
    state.handle(tui::Key::Right);
    const std::size_t start = cache.generation();
    for (int i = 0; i < kMoves; ++i) {
        now += kKeyGap;
        state.handle(tui::Key::Down);
        cache.throttled(state, store, now);
    }
    CHECK(cache.generation() == start);
    CHECK(state.preview_scroll() > 0);
}
