// G4 kabulü: graph üzerindeki NoteStore (SPEC-graph kabul 8) ve yazma büyümesi ölçümü. Kurulum geçişin toplu
// yolu.
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "core/process.hpp"
#include "support/cli_sandbox.hpp"
#include "support/note_seed.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace noteapp;
namespace fs = std::filesystem;

namespace {

constexpr int kNotes = 5000;
constexpr std::size_t kBodyBytes = 2048;
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

void seed_corpus(const fs::path& directory) {
    std::vector<core::ImportedNote> notes;
    notes.reserve(kNotes);
    const std::string body(kBodyBytes, 'x');
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(core::ImportedNote{
            core::NotePath::parse("folder" + std::to_string(i % 40) + "/note-" + std::to_string(i) + ".md")
                .value(),
            body + std::to_string(i)});
    test::bulk_seed(directory, notes);
}

// Gerçekçi düzenleme: 30 dakika, yazma kümeleri ve araları (saniye). Otomatik kayıt her "kümeden sonra ara >=
// gecikme"de bir yazar: GUI'nin zamanlayıcısı her tuşta yeniden kurulur.
std::vector<int> session_pauses() {
    std::mt19937 rng{7};
    std::vector<int> pauses;
    int elapsed = 0;
    while (elapsed < 30 * 60) {
        const int burst = 2 + static_cast<int>(rng() % 14);
        const int roll = static_cast<int>(rng() % 100);
        const int pause = roll < 50   ? 1 + static_cast<int>(rng() % 2)
                          : roll < 85 ? 3 + static_cast<int>(rng() % 5)
                                      : 8 + static_cast<int>(rng() % 30);
        pauses.push_back(pause);
        elapsed += burst + pause;
    }
    return pauses;
}

struct Growth {
    int saves = 0;
    std::uintmax_t log_bytes = 0;
};

Growth measure_session(int debounce_seconds, std::size_t note_bytes) {
    const test::TempDir data;
    const test::TempDir logs;
    core::NoteStore store(data.path());
    std::string text(note_bytes, 'a');
    REQUIRE(test::seed_note(store, "n.md", text));
    const auto path = core::NotePath::parse("n.md").value();
    auto revision = store.read(path)->revision;
    Growth growth;
    for (const int pause : session_pauses()) {
        text.append(30, 'b');  // küme başına ~30 karakter
        if (pause < debounce_seconds)
            continue;
        auto written = store.write(path, text, revision);
        REQUIRE(written.has_value());
        revision = *written;
        ++growth.saves;
    }
    REQUIRE(core::export_note_log(store, logs.path()).has_value());
    growth.log_bytes = fs::file_size(logs.path() / *core::own_log_name(store));
    return growth;
}

}  // namespace

TEST_CASE("note store at 5000 notes: note \"x\" within 200 ms (median of 7)") {
    const test::Sandbox box;
    seed_corpus(box.data());
    std::vector<double> samples;
    for (int i = 0; i < 7; ++i) {
        int code = -1;
        samples.push_back(
            time_ms([&] { code = core::run_and_wait({NOTEAPP_NOTE_EXE, "x"}, {}, true).value_or(-1); }));
        CHECK(code == 0);
    }
    MESSAGE("note \"x\" at 5000 notes: median " << median(samples) << " ms, first " << samples.front()
                                                << " ms");
    CHECK(median(samples) <= 200.0);
}

TEST_CASE("note store at 5000 notes: fresh process open and list within 300 ms (median of 5)") {
    const test::Sandbox box;
    seed_corpus(box.data());
    const fs::path result = box.data().parent_path() / "probe.txt";
    std::vector<double> wall;
    for (int run = 0; run < 5; ++run) {
        int code = -1;
        wall.push_back(time_ms([&] {
            code = core::run_and_wait({NOTEAPP_TUI_STARTUP_PROBE, test::utf8(result)}, {}, true).value_or(-1);
        }));
        CHECK(code == 0);
    }
    MESSAGE("open at 5000 notes: median " << median(wall) << " ms, first " << wall.front() << " ms");
    CHECK(median(wall) <= 300.0);
}

TEST_CASE("note store at 5000 notes: an unchanged rescan is cheap, an external change is seen") {
    const test::TempDir dir;
    seed_corpus(dir.path());
    core::NoteStore store(dir.path());
    const double cold = time_ms([&] { CHECK(store.list().size() == kNotes); });
    std::vector<double> rescans;
    for (int run = 0; run < 7; ++run)
        rescans.push_back(time_ms([&] { CHECK(store.list().size() == kNotes); }));
    test::upsert_note(dir.path(), "yeni.md", "dış");
    const double after_change = time_ms([&] { CHECK(store.list().size() == kNotes + 1); });
    MESSAGE("list cold " << cold << " ms; unchanged rescan median " << median(rescans)
                         << " ms; after external change " << after_change << " ms");
    CHECK(median(rescans) <= 10.0);
}

TEST_CASE("write amplification: a 30 minute editing session under autosave delays") {
    for (const std::size_t note_bytes : {std::size_t{2} * 1024, std::size_t{20} * 1024}) {
        for (const int debounce : {1, 3, 5}) {
            const Growth growth = measure_session(debounce, note_bytes);
            MESSAGE("note " << note_bytes / 1024 << " KiB, autosave " << debounce << " s: " << growth.saves
                            << " saves, log " << growth.log_bytes / 1024 << " KiB");
            CHECK(growth.saves > 0);
        }
    }
}
