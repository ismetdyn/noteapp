// SPEC.md §10/2–3 ve modüllerin perf kabulleri (T35). Yalnız Release'te kaydedilir (tests/CMakeLists.txt):
// Debug süreleri anlamsızdır ve günlük ctest'i yavaşlatırdı. Gürültülü makinede (virüs tarayıcı) tek ölçüm
// yanıltır; eşikler birkaç koşunun medyanına uygulanır.
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "detect/detect.hpp"
#include "search/content_index.hpp"
#include "search/fuzzy.hpp"
#include "support/cli_sandbox.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#ifdef NOTEAPP_PERF_GUI
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/note_list_model.hpp"
#include "gui/view.hpp"

#include <imgui.h>
#endif

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

// Tek sayıda örnekte tam ortadaki değer.
double median(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

std::vector<core::NoteInfo> generated_notes(const std::string& stem) {
    std::vector<core::NoteInfo> notes;
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(
            {core::NotePath::parse("folder" + std::to_string(i % 40) + "/" + stem + std::to_string(i) + ".md")
                 .value(),
             fs::file_time_type{} + std::chrono::hours(100000 - i), 1});
    return notes;
}

std::string render_text(const tui::TuiState& state) {
    constexpr int kWidth = 120;
    constexpr int kHeight = 40;
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, tui::Preview{}, {"", ""}, {kWidth, kHeight}));
    return screen.ToString();
}

}  // namespace

// SPEC.md §10/2: `note "x"` notu ≤ 200 ms'de diske yazar. Kabuksuz başlatılır (cmd.exe açılışı ölçüme
// girmez); süreç açılışı + config + yazma + çıkış dahildir. Senkron kapalı (varsayılan).
TEST_CASE("quick add: note \"x\" writes the note within 200 ms (median of 7)") {
    const test::Sandbox box;
    std::vector<double> samples;
    for (int i = 0; i < 7; ++i) {
        int code = -1;
        samples.push_back(
            time_ms([&] { code = core::run_and_wait({NOTEAPP_NOTE_EXE, "x"}, {}, true).value_or(-1); }));
        CHECK(code == 0);
    }
    MESSAGE("quick add: median " << median(samples) << " ms, first " << samples.front() << " ms");
    CHECK(core::NoteStore(box.data()).list().size() == 7);
    CHECK(median(samples) <= 200.0);
}

// SPEC.md §10/3: `notes` 5.000 notla ≤ 500 ms'de açılır. `notes` terminal ister; vekil (tui_startup_probe)
// her koşuda taze süreçtir: süreç içi önbellek yoktur, dosya sistemi önbelleği ise notlar yeni yazıldığı için
// ılıktır (yeniden başlatma sonrası gerçek soğuk açılış testte üretilemez). Eşik süreç başlatmadan çıkışa
// kadar duvar saatine uygulanır.
TEST_CASE("TUI startup proxy: fresh process with 5000 notes on disk within 500 ms (median of 5)") {
    const test::Sandbox box;
    std::vector<core::ImportedNote> seeded;
    for (int i = 0; i < kNotes; ++i)
        seeded.push_back(core::ImportedNote{
            core::NotePath::parse("folder" + std::to_string(i % 40) + "/note-" + std::to_string(i) + ".md")
                .value(),
            "# note " + std::to_string(i) + "\n"});
    test::bulk_seed(box.data(), seeded);
    const fs::path result = box.notes().parent_path() / "probe.txt";

    std::vector<double> wall, inside;
    for (int run = 0; run < kRuns; ++run) {
        int code = -1;
        wall.push_back(time_ms([&] {
            code = core::run_and_wait({NOTEAPP_TUI_STARTUP_PROBE, test::utf8(result)}, {}, true).value_or(-1);
        }));
        REQUIRE(code == 0);
        double ms = 0;
        std::size_t total = 0;
        std::ifstream(result) >> ms >> total;
        CHECK(total == kNotes);
        inside.push_back(ms);
    }
    MESSAGE("TUI startup proxy: median " << median(wall) << " ms wall (first " << wall.front()
                                         << " ms); in-process config+list+set_notes+preview+render median "
                                         << median(inside) << " ms (first " << inside.front() << " ms)");
    CHECK(median(wall) <= 500.0);
}

// SPEC.md §10/3 + SPEC-tui kabul 5: 5.000 notla tuş başına süzme ≤ 50 ms. Ölçüme yeniden çizim de girer
// (kullanıcının gördüğü gecikme); koşu başına en kötü tuş alınır.
TEST_CASE("TUI: per-keystroke filter with 5000 notes within 50 ms") {
    core::init_i18n("en");  // sayaç metni katalogdan gelir
    std::vector<double> worst_per_run;
    for (int run = 0; run < kRuns; ++run) {
        tui::TuiState state;
        state.set_notes(generated_notes("note-"));
        CHECK(render_text(state).find("5000 notes") != std::string::npos);
        state.handle(tui::Key::Char, "/");  // yazı yalnız Arama odağında süzer
        double worst = 0;
        for (const char c : std::string("note-42"))
            worst = std::max(worst, time_ms([&] {
                                 state.handle(tui::Key::Char, std::string(1, c));
                                 (void)render_text(state);
                             }));
        CHECK(state.query() == "note-42");  // ölçüm gerçekten süzdü
        CHECK_FALSE(state.rows().empty());
        worst_per_run.push_back(worst);
    }
    MESSAGE("TUI keystroke (filter + render): median of worst " << median(worst_per_run) << " ms");
    CHECK(median(worst_per_run) <= 50.0);
}

#ifdef NOTEAPP_PERF_GUI
// SPEC.md §10/3 + SPEC-gui kabul 7: arama kutusunda tuş başına liste güncellemesi ≤ 50 ms; görünür satırların
// verisi (liste çizimi) dahil.
TEST_CASE("GUI: per-keystroke model update with 5000 notes within 50 ms") {
    std::vector<double> worst_per_run;
    for (int run = 0; run < kRuns; ++run) {
        gui::NoteListModel model;
        model.set_notes(generated_notes("meeting-notes-"));
        std::string query;
        double worst = 0;
        for (const char c : std::string("meetingnotes12")) {
            query.push_back(c);
            worst = std::max(worst, time_ms([&] {
                                 model.set_query(query);
                                 for (int row = 0; row < std::min(model.row_count(), 40); ++row) {
                                     (void)model.display(row);
                                     (void)model.highlights(row);
                                 }
                             }));
        }
        CHECK(model.row_count() > 0);
        worst_per_run.push_back(worst);
    }
    MESSAGE("GUI keystroke (model + visible rows): median of worst " << median(worst_per_run) << " ms");
    CHECK(median(worst_per_run) <= 50.0);
}

// Plan riski: InputTextMultiline tamponu her karede işler. Tuş gecikmesi = karakter + kare (yazılımsal çizim,
// en kötü durum). Sınır SPEC §10/3'ün tuş başına 50 ms'i; aşılırsa kullanıcıya sorulur, gizli çözüm eklenmez.
TEST_CASE("GUI: typing in a 1 MB note stays within 50 ms per keystroke") {
    const test::TempDir home;
    const test::ScopedEnv home_env("NOTEAPP_HOME", test::utf8(home.path()));
    core::init_i18n("en");
    core::Config config = core::load_config().config;
    config.notes_dir = home.path() / "notes";
    fs::create_directories(config.notes_dir);
    std::string note;
    constexpr std::size_t kNoteBytes = 1024 * 1024;
    while (note.size() < kNoteBytes)
        note += "satır çğıİöşü lorem ipsum dolor sit amet consectetur\n";
    test::upsert_note(core::data_dir(), "big.txt", note);

    gui::GuiState state(config);
    auto view = gui::View::create(state);
    REQUIRE(view.has_value());
    state.request_focus(gui::FocusRequest::Editor);
    for (int frame = 0; frame < 3; ++frame)
        (*view)->draw_frame();
    REQUIRE((*view)->editor_active());

    std::vector<double> samples;
    for (int key = 0; key < 50; ++key) {
        samples.push_back(time_ms([&] {
            ImGui::GetIO().AddInputCharactersUTF8("x");
            (*view)->draw_frame();
        }));
    }
    CHECK(state.dirty());
    const double idle_frame = time_ms([&] { (*view)->draw_frame(); });
    MESSAGE("GUI 1 MB note: keystroke median " << median(samples) << " ms, worst "
                                               << *std::ranges::max_element(samples) << " ms, plain frame "
                                               << idle_frame << " ms");
    CHECK(median(samples) <= 50.0);
}
#endif

// SPEC.md §10/3 + SPEC-search kabul 5: 5.000 not × ~5 KB; sıcak arama ≤ 100 ms, değişiklik yokken refresh
// (list() dahil) ≤ 100 ms; list() ≤ 80 ms (T16b).
TEST_CASE("content search: 5000 notes of ~5 KB, warm search within 100 ms") {
    const test::TempDir dir;
    const core::NoteStore store{dir.path()};
    std::mt19937 rng{42};
    std::vector<std::string> words;
    for (int i = 0; i < 2000; ++i) {
        std::string w;
        for (int j = 0, n = 3 + static_cast<int>(rng() % 8); j < n; ++j)
            w += static_cast<char>('a' + rng() % 26);
        words.push_back(w);
    }
    words[0] = "proje";
    words[1] = "bütçe";
    std::vector<core::ImportedNote> seeded;
    for (int n = 0; n < kNotes; ++n) {
        std::string text;
        while (text.size() < 5000) {
            text += words[rng() % words.size()];
            text += (rng() % 12 == 0) ? '\n' : ' ';
        }
        seeded.push_back(core::ImportedNote{
            core::NotePath::parse("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md").value(),
            std::move(text)});
    }
    test::bulk_seed(dir.path(), seeded);

    search::ContentIndex index{store, 10 * 1024 * 1024};
    const double cold = time_ms([&] { index.refresh(); });
    std::vector<search::ContentResult> results;
    const double first = time_ms([&] { results = index.search("proje bütçe", 50); });
    std::vector<double> warm, refresh, list;
    for (int run = 0; run < kRuns; ++run) {
        warm.push_back(time_ms([&] { results = index.search("proje bütçe", 50); }));
        refresh.push_back(time_ms([&] { index.refresh(); }));
        std::size_t listed = 0;
        list.push_back(time_ms([&] { listed = store.list().size(); }));
        CHECK(listed == kNotes);
    }
    MESSAGE("content: cold refresh " << cold << " ms, first search " << first << " ms; medians: warm search "
                                     << median(warm) << " ms, no-change refresh " << median(refresh)
                                     << " ms, list() " << median(list) << " ms");
    CHECK(index.read_count() == kNotes);
    CHECK_FALSE(results.empty());
    CHECK(median(warm) <= 100.0);
    CHECK(median(refresh) <= 100.0);
    CHECK(median(list) <= 80.0);
}

// SPEC-search kabul 5: 5.000 gerçekçi yolda rank_notes ≤ 20 ms.
TEST_CASE("rank_notes: 5000 realistic paths within 20 ms") {
    static const char* const kFolders[] = {"work",    "personal",       "ideas",
                                           "journal", "projects/alpha", "projects/beta"};
    static const char* const kWords[] = {"meeting", "notes",  "shopping", "list", "design",
                                         "review",  "budget", "travel",   "plan", "retro",
                                         "idea",    "draft",  "summary",  "todo"};
    std::vector<core::NoteInfo> notes;
    unsigned state = 12345;  // deterministik LCG; testte rastgelelik istemiyoruz
    auto next = [&state](unsigned mod) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % mod;
    };
    for (int i = 0; i < kNotes; ++i) {
        const std::string p = std::string(kFolders[next(6)]) + "/" + kWords[next(14)] + "-" +
                              kWords[next(14)] + "-" + std::to_string(i) + ".md";
        notes.push_back({core::NotePath::parse(p).value(),
                         fs::file_time_type{std::chrono::seconds{static_cast<int>(next(100000))}}, 0});
    }

    std::vector<double> samples;
    std::size_t matches = 0;
    for (int run = 0; run < kRuns; ++run)
        samples.push_back(time_ms([&] { matches = search::rank_notes("wmn", notes).size(); }));
    MESSAGE("rank_notes(5000 paths): median " << median(samples) << " ms, " << matches << " matches");
    CHECK(matches > 0);
    CHECK(median(samples) <= 20.0);
}

// SPEC-detect kabul 4: 64 KB girdide suggest ≤ 5 ms (5 denemenin en iyisi).
TEST_CASE("detect: 64 KB input is classified within 5 ms") {
    std::ifstream in(fs::path(NOTEAPP_DETECT_SAMPLES_DIR) / "cpp" / "02_class.txt", std::ios::binary);
    const std::string unit{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(unit.empty());
    std::string big;
    while (big.size() < 64 * 1024)
        big += unit;
    const detect::Options options{"md", 0.5, 64 * 1024};
    double best = 1e9;
    for (int run = 0; run < kRuns; ++run) {
        std::string type;
        best = std::min(best, time_ms([&] { type = detect::suggest(big, options).type; }));
        CHECK(type == "cpp");
    }
    MESSAGE("detect(64 KB): best of 5 " << best << " ms");
    CHECK(best <= 5.0);
}
