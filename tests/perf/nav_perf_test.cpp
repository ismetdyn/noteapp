// T64: gezinme depo okuması yapmaz (G4'ten beri okuma graph.db'dendir; generation her gövde okumasında
// artar). Sayaç ölçülür, duvar saati değil: yük altında yanlış kırmızı vermez. Gerçek terminal sayıları
// (boşalma süresi, kare, tuş gecikmesi) tmux ölçümündedir; bkz. SPEC-tui "Değişiklik 2026-11-01".
#include "core/config.hpp"
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"
#include "memory.hpp"
#include "wine.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
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

// NX-B: ham REVIEW'da tek satırlık 20 MiB not. Görünen satır sarılırken satırın tamamı gezilmemeli: tuş
// başına bütçe (SPEC-tui 50 ms) çizimin kendisine uygulanır.
TEST_CASE("nav perf: REVIEW draws a 20 MiB single-line note within the key budget") {
    constexpr std::size_t kLineBytes = std::size_t{20} * 1024 * 1024;
    constexpr double kKeyBudgetMs = 50.0;
    constexpr int kFrames = 5;
    const test::TempDir dir;
    test::upsert_note(dir.path(), "tek.txt", std::string(kLineBytes, 'x'));
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache cache;
    const std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    const tui::Preview& shown = cache.throttled(state, store, now);
    REQUIRE(shown.text.size() == kLineBytes);
    state.set_preview_lines(&shown.lines, cache.generation());
    state.handle(tui::Key::Right);
    const ftxui::Dimensions window{120, 40};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(window.dimx), ftxui::Dimension::Fixed(window.dimy));
    const auto frame = [&] {
        const auto start = std::chrono::steady_clock::now();
        ftxui::Render(screen, tui::render(state, cache.shown(), {"sync off", ""}, window));
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    double worst = frame();
    for (int i = 0; i < kFrames; ++i) {
        state.handle(tui::Key::Down);
        worst = std::max(worst, frame());
    }
    MESSAGE("TUI REVIEW 20 MiB single line: worst frame ", worst, " ms");
    CHECK(worst <= kKeyBudgetMs);
}

namespace {

// GUI önizleme ölçümüyle (preview_perf_test) aynı birim: başlık, sarılan paragraf, wiki bağlantısı, liste, kod.
std::string large_markdown(std::size_t bytes) {
    const std::string unit = "## Bölüm\n\n"
                             "Bir paragraf *vurgu* ve **kalın** metin [[hedef]] ile; satır sarılsın diye biraz "
                             "uzun yazılmış bir cümle daha ekleniyor burada.\n\n"
                             "- madde bir\n- madde iki\n\n"
                             "```\nkod satırı\n```\n\n";
    std::string text;
    text.reserve(bytes + unit.size());
    while (text.size() < bytes)
        text += unit;
    return text;
}

}  // namespace

// NX-D: biçimli REVIEW'da 20 MiB md notu, sınırlar kapalı (view_bytes = 0, view_lines = 0). Görünüm görünen
// pencere kadar üretilir: biçimli görünüme geçiş karesi tuş bütçesinde (50 ms), kaydırma karesi 16 ms'de. Notun
// okunması (ham görünümle ortak, satırlara bölme) ayrı ölçülür; sona atlama (`G`) tüm görünümü üretir. İkisinin
// süresi bilgi için yazılır.
TEST_CASE("nav perf: REVIEW renders a 20 MiB markdown note lazily within the key budget") {
    constexpr std::size_t kNoteBytes = std::size_t{20} * 1024 * 1024;
    constexpr double kFirstBudgetMs = 50.0;
    constexpr double kFrameBudgetMs = 16.0;
    constexpr int kFrames = 10;
    const test::TempDir dir;
    test::upsert_note(dir.path(), "büyük.md", large_markdown(kNoteBytes));
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    const ftxui::Dimensions window{120, 40};
    state.set_window(window.dimx, window.dimy);
    const core::TuiLimits limits =
        core::parse_config("[limits.tui]\nview_bytes = 0\nview_lines = 0\n").config.limits.tui;
    tui::PreviewCache cache(tui::view_limits_of(limits), tui::syntax_limits_of(limits));
    const std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(window.dimx), ftxui::Dimension::Fixed(window.dimy));
    const auto frame = [&] {
        const auto start = std::chrono::steady_clock::now();
        const tui::Preview& shown = cache.throttled(state, store, now);
        state.set_preview_lines(&shown.lines, cache.generation(), shown.partial);
        ftxui::Render(screen, tui::render(state, shown, {"sync off", ""}, window));
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    state.handle(tui::Key::Right);
    state.handle(tui::Key::Char, "R");  // ham: yalnız notun okunması
    const double load = frame();
    REQUIRE_FALSE(cache.shown().rendered);
    state.handle(tui::Key::Char, "R");
    const auto before = test::resident_bytes();
    const double first = frame();
    REQUIRE(cache.shown().rendered);
    const double rss_mib = test::resident_growth_mib(before).value_or(-1.0);  // -1: bu platformda ölçülmez
    double worst = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        state.handle(tui::Key::PageDown);
        worst = std::max(worst, frame());
    }
    CHECK(state.preview_scroll() > 0);
    state.handle(tui::Key::Char, "G");
    const double bottom = frame();
    const double steady_after_bottom = frame();
    MESSAGE("TUI REVIEW 20 MiB md: note read (raw) ", load, " ms; formatted first ", first, " ms, worst scroll frame ",
            worst, " ms, rss +", rss_mib, " MiB; G ", bottom, " ms, then ", steady_after_bottom, " ms");
    CHECK(first <= kFirstBudgetMs);
    CHECK(worst <= kFrameBudgetMs);
    CHECK(steady_after_bottom <= kFrameBudgetMs);

    // Bilgi: tek blok (20 MiB'lık tek satır) parçalara bölünmez; biçimli görünüm satırın tamamını sarar
    // (limits.tui.view_bytes varsayılanı bu yüzden kalır).
    const test::TempDir single_dir;
    test::upsert_note(single_dir.path(), "tek.md", std::string(kNoteBytes, 'x'));
    const core::NoteStore single_store(single_dir.path());
    tui::TuiState single;
    single.set_notes(single_store.list());
    single.set_window(window.dimx, window.dimy);
    tui::PreviewCache single_cache(tui::view_limits_of(limits), tui::syntax_limits_of(limits));
    const auto start = std::chrono::steady_clock::now();
    const tui::Preview& single_shown = single_cache.throttled(single, single_store, now);
    single.set_preview_lines(&single_shown.lines, single_cache.generation(), single_shown.partial);
    ftxui::Render(screen, tui::render(single, single_shown, {"sync off", ""}, window));
    MESSAGE("TUI REVIEW 20 MiB single-line md, formatted: first ",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), " ms");
}

namespace {

// T71: yapıştırma UI iş parçacığında koşar: süre donma süresidir (SPEC-tui "Çoklu seçim ve yank"). Her
// yapıştırma kendi deposunda; yalnız `p` ölçülür, kurulum değil.
double paste_folder_of_2000_notes_ms() {
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
    CHECK(state.summary().empty());
    CHECK(store->list().size() == 4000);
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

}  // namespace

// T93: tek ölçüm başka bir yükün tepesine denk gelince sınırı aşıyordu; eşik, perf paketindeki gibi tek
// sayıda bağımsız yapıştırmanın medyanına uygulanır.
// Sınır SPEC kabulü değildir: T71'in yerli ölçümüne (105 ms) göre konmuş gerileme bekçisidir (not başına
// yazma 28,7 sn). Wine yerli platform değil, Windows ikilisinin test ortamıdır; aynı commit ve boş makinede
// medyan Linux 128 ms, Wine 222 ms (25'er koşu, T93): Wine çarpanı 2. Yerli sınır değişmez.
TEST_CASE("folder copy perf: 2000 notes through ListActions") {
    constexpr int kPastes = 5;
    constexpr double kPasteBudgetMs = 200.0;  // UI iş parçacığında, yerli (Linux, Windows)
    const double budget_ms = test::running_under_wine() ? kPasteBudgetMs * test::kWineFactor : kPasteBudgetMs;
    std::vector<double> samples;
    for (int run = 0; run < kPastes; ++run)
        samples.push_back(paste_folder_of_2000_notes_ms());
    std::ranges::sort(samples);
    const double median = samples[samples.size() / 2];
    MESSAGE("folder_copy_2000_ms=" << median << " (median of " << kPastes << ", worst " << samples.back()
                                   << ", budget " << budget_ms << ")");
    CHECK(median < budget_ms);
}
