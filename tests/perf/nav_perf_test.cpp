// T64: gezinme depo okuması yapmaz (G4'ten beri okuma graph.db'dendir; generation her gövde okumasında
// artar). Sayaç ölçülür, duvar saati değil: yük altında yanlış kırmızı vermez. Gerçek terminal sayıları
// (boşalma süresi, kare, tuş gecikmesi) tmux ölçümündedir; bkz. SPEC-tui "Değişiklik 2026-11-01".
#include "app/data_preview.hpp"
#include "core/config.hpp"
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
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

// Biçimli REVIEW ölçümü (NX-D, NY-P): sınırlar kapalı (view_bytes, view_lines, table_rows = 0). Görünüm görünen
// pencere kadar üretilir: biçimli görünüme geçiş karesi tuş bütçesinde (50 ms), kaydırma karesi 16 ms'de, sona
// atlamadan sonraki kare 16 ms'de. Notun okunması (ham görünümle ortak, satırlara bölme) ayrı ölçülür; sona atlama
// (`G`) tüm görünümü üretir: süresi bilgi için yazılır. Wine çarpanı wine.hpp kuralıyla.
struct ReviewSample {
    double load = 0.0;
    double first = 0.0;
    double worst = 0.0;
    double bottom = 0.0;
    double after_bottom = 0.0;
    double rss_mib = -1.0;  // -1: bu platformda ölçülmez
};

ReviewSample review_formatted(const std::string& name, const std::string& note) {
    constexpr int kFrames = 10;
    const test::TempDir dir;
    test::upsert_note(dir.path(), name, note);
    const core::NoteStore store(dir.path());
    tui::TuiState state;
    state.set_notes(store.list());
    const ftxui::Dimensions window{120, 40};
    state.set_window(window.dimx, window.dimy);
    const core::TuiLimits limits =
        core::parse_config("[limits.tui]\nview_bytes = 0\nview_lines = 0\ntable_rows = 0\n").config.limits.tui;
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
    ReviewSample sample;
    state.handle(tui::Key::Right);
    state.handle(tui::Key::Char, "R");  // ham: yalnız notun okunması
    sample.load = frame();
    REQUIRE_FALSE(cache.shown().rendered);
    state.handle(tui::Key::Char, "R");
    const auto before = test::resident_bytes();
    sample.first = frame();
    REQUIRE(cache.shown().rendered);
    sample.rss_mib = test::resident_growth_mib(before).value_or(-1.0);
    for (int i = 0; i < kFrames; ++i) {
        state.handle(tui::Key::PageDown);
        sample.worst = std::max(sample.worst, frame());
    }
    CHECK(state.preview_scroll() > 0);
    state.handle(tui::Key::Char, "G");
    sample.bottom = frame();
    sample.after_bottom = frame();
    return sample;
}

void check_review(const char* label, const std::string& name, const std::string& note) {
    const double factor = test::running_under_wine() ? test::kWineFactor : 1.0;
    constexpr double kFirstBudgetMs = 50.0;
    constexpr double kFrameBudgetMs = 16.0;
    const ReviewSample sample = review_formatted(name, note);
    MESSAGE("TUI REVIEW ", std::string(label), ": note read (raw) ", sample.load, " ms; formatted first ", sample.first,
            " ms, worst scroll frame ", sample.worst, " ms, rss +", sample.rss_mib, " MiB; G ", sample.bottom,
            " ms, then ", sample.after_bottom, " ms");
    CHECK(sample.first <= kFirstBudgetMs * factor);
    CHECK(sample.worst <= kFrameBudgetMs * factor);
    CHECK(sample.after_bottom <= kFrameBudgetMs * factor);
}

constexpr std::size_t kLargeNoteBytes = std::size_t{20} * 1024 * 1024;

}  // namespace

TEST_CASE("nav perf: REVIEW renders a 20 MiB markdown note lazily within the key budget") {
    check_review("20 MiB md", "büyük.md", large_markdown(kLargeNoteBytes));
}

// NY-P: kesim yeri olmayan tek blok zorla parçalanır (kMarkdownMaxChunkBytes); csv ve json dizinden satır öbekleriyle.
TEST_CASE("nav perf: REVIEW renders a 20 MiB single-line markdown note lazily within the key budget") {
    check_review("20 MiB single-line md", "tek.md", std::string(kLargeNoteBytes, 'x'));
}

TEST_CASE("nav perf: REVIEW renders a 20 MiB JSON note lazily within the key budget") {
    std::string text = "[";
    for (int i = 0; text.size() < kLargeNoteBytes; ++i)
        text += (i > 0 ? "," : "") + std::string("{\"id\":") + std::to_string(i) +
                ",\"ad\":\"Çağrı\",\"etiketler\":[\"a\",\"b\",[]],\"iç\":{\"x\":1.5,\"y\":null,\"z\":{}}}";
    check_review("20 MiB minified JSON", "büyük.json", text + "]");
}

TEST_CASE("nav perf: REVIEW renders a 20 MiB CSV note lazily within the key budget") {
    std::string text = "id,ad,şehir,not,a,b,c,d\n";
    for (int i = 0; text.size() < kLargeNoteBytes; ++i)
        text += std::to_string(i) + ",Çağrı,\"İstanbul, merkez\",\"uzun \"\"alıntılı\"\" not\",1,2.5,x,y\n";
    check_review("20 MiB CSV", "büyük.csv", text);
}

// NY-P inceleme: CSV satır dizininin belleği. 20 MiB'lık 2 baytlık satırlar (~10 milyon satır): satır başına 8 baytlık
// ofset ~80 MB olurdu (notun 4 katı). Dizin notun dörtte birinden küçük kalmalı.
TEST_CASE("nav perf: the row index of a 20 MiB CSV of 2-byte lines stays small") {
    std::string text = "a,b\n";
    text.reserve(kLargeNoteBytes + 8);
    while (text.size() < kLargeNoteBytes)
        text += ",\n";
    const auto before = test::resident_bytes();
    const auto start = std::chrono::steady_clock::now();
    const std::optional<app::CsvIndex> index = app::index_csv(text);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(index);
    const std::optional<double> rss = test::resident_growth_mib(before);
    MESSAGE("CSV index, 20 MiB of 2-byte rows: ", ms, " ms, rss +", rss.value_or(-1.0), " MiB");
    if (rss)
        CHECK(*rss <= static_cast<double>(kLargeNoteBytes) / (4.0 * 1024.0 * 1024.0));
}

// NY-P güvenlik (Sev-3): kontrol noktası satır sayısının yanında bayt mesafesiyle de konur. Satırları arasında 10 MB
// boş satır (csv) ya da belirteçleri arasında 10 MB boşluk (json) olan notta görünen pencere her karede notun başından
// ya da bloğun başından taranmaz: 200 karelik pencere okuması 50 ms'de biter (önce kare başına ~10 MB tarama).
TEST_CASE("nav perf: reading a window after 10 MB of padding does not rescan the padding every frame") {
    constexpr std::size_t kPadding = std::size_t{10} * 1024 * 1024;
    constexpr int kFrames = 200;
    constexpr double kBudgetMs = 50.0;
    const auto timed = [](const auto& frame) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i)
            frame();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };

    std::string csv = "n,x\n";
    for (int i = 1; i < 100; ++i) {
        csv += std::to_string(i) + ",x\n";
        if (i == 30)
            csv += std::string(kPadding, '\n');
    }
    const std::optional<app::CsvIndex> rows = app::index_csv(csv);
    REQUIRE(rows);
    const double csv_ms = timed([&] { CHECK(app::csv_rows(csv, *rows, 40, 30).front().cells.front() == "40"); });

    std::string json = "[";
    for (int i = 0; i < 100; ++i) {
        json += std::to_string(i) + (i + 1 < 100 ? "," : "");
        if (i == 50)
            json += std::string(kPadding, ' ');
    }
    json += "]";
    const std::optional<app::JsonIndex> lines = app::index_json(json);
    REQUIRE(lines);
    const double json_ms = timed([&] { CHECK(app::json_lines(json, *lines, 60, 30).front().text == "59,"); });
    MESSAGE("200 frames after 10 MB of padding: csv ", csv_ms, " ms, json ", json_ms, " ms");
    CHECK(csv_ms <= kBudgetMs);
    CHECK(json_ms <= kBudgetMs);
}

// NY-P güvenlik (Sev-2): düşman not; 10 MB tanım ve her parçası tanım tablosunun büyük bölümünü anan yalnız-başvuru
// satırları. Parça başına eklenen tanım iş bütçesiyle sınırlı (limits.tui.preview_definition_bytes): bütün parçaların
// üretimi (TUI'de `G` ya da not içi arama) tanımsız aynı notun en çok 4 katı sürer. Bütçesiz her parça ~1,3 MB tanım
// ayrıştırırdı (parça sayısı × tanım baytı).
TEST_CASE("nav perf: rendering every chunk of a note referencing 10 MB of definitions stays linear") {
    constexpr std::size_t kDefinitionBytes = std::size_t{10} * 1024 * 1024;
    constexpr double kMaxRatio = 4.0;
    std::string definitions;
    int labels = 0;
    for (; definitions.size() < kDefinitionBytes; ++labels)
        definitions += "[e" + std::to_string(labels) + "]: https://example.org/uzun/bir/yol/" +
                       std::to_string(labels) + "/" + std::string(40, 'p') + "\n";
    std::string references;
    for (int i = 0; references.size() < kDefinitionBytes; ++i) {
        references += "[e" + std::to_string(i % labels) + "] ";
        if (i % 20 == 19)
            references += i % 400 == 399 ? "\n\nBölüm\n\n" : "\n";
    }
    const auto render_all = [](const std::string& note) {
        const auto start = std::chrono::steady_clock::now();
        const app::MarkdownScan scan = app::scan_markdown(note);
        std::size_t lines = 0;
        for (const app::MarkdownChunk& chunk : scan.chunks)
            lines += tui::render_markdown(app::markdown_chunk_source(note, chunk, scan.definitions), 120,
                                          tui::terminal_cell_width)
                         .lines.size();
        CHECK(lines > 0);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const double crafted = render_all(references + "\n\n" + definitions);
    const double plain = render_all(references + "\n\n" + std::string(definitions.size(), 'x') + "\n");
    MESSAGE("all chunks, 10 MB references + 10 MB definitions: ", crafted, " ms; same size without definitions: ",
            plain, " ms");
    CHECK(crafted <= kMaxRatio * plain);
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
