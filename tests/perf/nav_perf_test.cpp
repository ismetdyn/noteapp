// T64: gezinme depo okuması yapmaz (G4'ten beri okuma graph.db'dendir; generation her gövde okumasında
// artar). Sayaç ölçülür, duvar saati değil: yük altında yanlış kırmızı vermez. Gerçek terminal sayıları
// (boşalma süresi, kare, tuş gecikmesi) tmux ölçümündedir; bkz. SPEC-tui "Değişiklik 2026-11-01".
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <mutex>
#include <optional>
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

TEST_CASE("nav perf: entering and leaving folders fast reads no note until the moves stop") {
    // `txt`: biçimli görünüme geçiş de generation artırır; burada sayaç yalnız depo okumasını saysın.
    constexpr int kFolders = 50;
    constexpr int kNotesPerFolder = 100;
    const test::TempDir dir;
    std::vector<core::ImportedNote> notes;
    for (int folder = 0; folder < kFolders; ++folder)
        for (int note = 0; note < kNotesPerFolder; ++note)
            notes.push_back(core::ImportedNote{
                core::NotePath::parse("d" + std::to_string(folder) + "/n" + std::to_string(note) + ".txt")
                    .value(),
                "metin"});
    test::bulk_seed(dir.path(), notes);
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache cache;
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    cache.throttled(state, store, now);
    // Sakin kullanıcının ilk hareketi beklemez, bir okuma yapar: sayaç ondan sonra başlar.
    state.handle(tui::Key::Right);
    cache.throttled(state, store, now);
    state.handle(tui::Key::Left);
    cache.throttled(state, store, now);
    const std::size_t start = cache.generation();
    for (int i = 0; i < kMoves; ++i) {
        now += kKeyGap;
        state.handle(i % 2 == 0 ? tui::Key::Right : tui::Key::Left);
        cache.throttled(state, store, now);
        if (i % 2 == 0)
            CHECK(state.selected_row() != nullptr);  // içerideki ilk not
        else
            CHECK(state.selected_folder_row() != nullptr);
    }
    CHECK(cache.generation() == start);
    state.handle(tui::Key::Right);
    now += std::chrono::milliseconds(tui::PreviewCache::kCoalesceMs);
    cache.throttled(state, store, now);
    now += std::chrono::milliseconds(tui::PreviewCache::kCoalesceMs);
    cache.throttled(state, store, now);
    CHECK(cache.generation() == start + 1);  // duruşta seçili not bir kez okunur
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

// T71: yapıştırma UI iş parçacığında koşar: süre donma süresidir (SPEC-tui "Çoklu seçim ve yank").
TEST_CASE("folder copy perf: 2000 notes through ListActions") {
    const test::TempDir dir;
    const test::TempDir state_home;
    const test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::vector<core::ImportedNote> notes;
    for (int i = 0; i < 2000; ++i)
        notes.push_back(core::ImportedNote{
            core::NotePath::parse("src/d" + std::to_string(i % 20) + "/n" + std::to_string(i) + ".md")
                .value(),
            std::string(1024, 'x')});
    test::bulk_seed(dir.path(), notes);
    REQUIRE(core::NoteStore(dir.path()).create_folder("dest"));
    std::optional<core::NoteStore> store(std::in_place, dir.path());
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    const auto load = [&] {
        state.set_notes(store->list());
        state.set_folders(store->folders());
    };
    tui::ListActions actions{
        tui::ListHost{store, mutex, internal, system, state, preview, status, load, [] {}}};
    load();
    std::optional<tui::ConfirmState> confirm;
    REQUIRE(state.select_folder("src"));
    REQUIRE(state.handle(tui::Key::Char, "y") == tui::Intent::None);
    REQUIRE(state.select_folder("dest"));
    state.handle(tui::Key::Enter);
    REQUIRE(state.current_folder() == "dest");
    const auto started = std::chrono::steady_clock::now();
    actions.run(state.handle(tui::Key::Char, "p"), confirm);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    MESSAGE("folder_copy_2000_ms=" << ms);
    CHECK(state.summary().empty());
    CHECK(store->list().size() == 4000);
    CHECK(ms < 200);  // UI iş parçacığında; not başına yazma 28 sn sürüyordu
}
