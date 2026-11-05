// NX-B (L1): 20 MiB'lık md notunun GUI önizlemesi. Sınırın üstündeki not artık reddedilmez: ham metin ve
// nedeni çizilir (TUI REVIEW gibi). Karşılaştırma için aynı not limits.tui.view_bytes = 0 (sınırsız) ile tam
// biçimli yerleşimle de ölçülür. max_note_bytes varsayılanı (10 MiB) notu salt okunur yapardı: 0 verilir.
#include "app/data_preview.hpp"
#include "core/limits.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "gui/window_fixture.hpp"
#include "memory.hpp"
#include "wine.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <doctest/doctest.h>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using gui_test::make_view;
using gui_test::WindowFixture;

namespace core = noteapp::core;

namespace {

constexpr std::size_t kNoteBytes = std::size_t{20} * 1024 * 1024;
constexpr double kFrameBudgetMs = 1000.0 / 30.0;
constexpr int kSteadyFrames = 10;
constexpr int kSamples = 3;  // bütçe medyana uygulanır: tek ölçüm başka bir yükün tepesine denk gelebilir

// Wine yerli platform değil, Windows ikilisinin test ortamıdır (wine.hpp): yerli sınır kWineFactor ile genişler,
// Linux ve Windows sınırı değişmez.
double budget(double native_ms) {
    return noteapp::test::running_under_wine() ? native_ms * noteapp::test::kWineFactor : native_ms;
}

// Başlık, sarılan paragraf, wiki bağlantısı, liste ve kod bloğu: yerleşimin her yolu.
std::string large_markdown() {
    const std::string unit = "## Bölüm\n\n"
                             "Bir paragraf *vurgu* ve **kalın** metin [[hedef]] ile; satır sarılsın diye biraz "
                             "uzun yazılmış bir cümle daha ekleniyor burada.\n\n"
                             "- madde bir\n- madde iki\n\n"
                             "```\nkod satırı\n```\n\n";
    std::string text;
    text.reserve(kNoteBytes + unit.size());
    while (text.size() < kNoteBytes)
        text += unit;
    return text;
}

struct Sample {
    double first_ms = 0.0;   // önizlemeye geçilen kare (ayrıştırma + yerleşim ya da ham metin)
    double steady_ms = 0.0;  // ardından gelen karelerin ortalaması
    double worst_ms = 0.0;   // ardından gelen karelerin en yavaşı
    double end_ms = 0.0;     // kaydırırken: sona atlanan kare
    double idle_ms = 0.0;    // kaydırırken: sondaki işsiz karelerin ortalaması (çizim katmanının payı)
    std::optional<double> rss_mib;  // geçişten sonra yerleşik bellek artışı; ölçülemiyorsa (Linux dışı) yok
};

// `scroll`: her kare yarım sayfa iki kez aşağı (PgDn), sonunda sona atlanır (End).
Sample measure(WindowFixture& fixture, core::Limit view_bytes, const std::string& note = "büyük.md",
               bool scroll = false) {
    using Clock = std::chrono::steady_clock;
    fixture.config.limits.tui.view_bytes = std::move(view_bytes);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    state->select_row(state->model().index_of(core::NotePath::parse(note).value()));
    gui_test::settle(*view);
    REQUIRE(state->can_preview());
    const auto before = noteapp::test::resident_bytes();
    Sample sample;
    auto start = Clock::now();
    state->toggle_preview();
    view->draw_frame();
    sample.first_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    REQUIRE(state->showing_preview());
    const auto timed_frame = [&] {
        const auto frame_start = Clock::now();
        view->draw_frame();
        return std::chrono::duration<double, std::milli>(Clock::now() - frame_start).count();
    };
    double total = 0.0;
    for (int frame = 0; frame < kSteadyFrames; ++frame) {
        if (scroll)
            state->scroll_preview_half(2);
        const double ms = timed_frame();
        total += ms;
        sample.worst_ms = std::max(sample.worst_ms, ms);
    }
    sample.steady_ms = total / kSteadyFrames;
    if (scroll) {
        state->scroll_preview(FLT_MAX);
        sample.end_ms = timed_frame();
        double idle = 0.0;
        for (int frame = 0; frame < kSteadyFrames; ++frame)
            idle += timed_frame();
        sample.idle_ms = idle / kSteadyFrames;
    }
    sample.rss_mib = noteapp::test::resident_growth_mib(before);
    return sample;
}

// Her alanın `kSamples` ölçümdeki medyanı (her ölçüm yeni pencere ve durumla).
Sample median_of(const std::function<Sample()>& take) {
    std::vector<Sample> samples;
    for (int run = 0; run < kSamples; ++run)
        samples.push_back(take());
    const auto median = [&](double Sample::*field) {
        std::vector<double> values;
        for (const Sample& sample : samples)
            values.push_back(sample.*field);
        std::ranges::sort(values);
        return values[values.size() / 2];
    };
    Sample out{median(&Sample::first_ms), median(&Sample::steady_ms), median(&Sample::worst_ms),
               median(&Sample::end_ms),   median(&Sample::idle_ms),   {}};
    std::vector<double> rss;
    for (const Sample& sample : samples)
        if (sample.rss_mib)
            rss.push_back(*sample.rss_mib);
    if (!rss.empty()) {
        std::ranges::sort(rss);
        out.rss_mib = rss[rss.size() / 2];
    }
    return out;
}

}  // namespace

TEST_CASE("preview perf: a 20 MiB note over the view limit opens as raw text within the frame budget") {
    WindowFixture fixture;
    fixture.config.tuning.max_note_bytes = {0, "0"};
    fixture.add_note("büyük.md", large_markdown(), 10);
    core::Limit limit{256U * 1024U, "256KiB"};  // eski varsayılan: notu ham metne düşürür
    const Sample raw = median_of([&] { return measure(fixture, limit); });
    MESSAGE("20 MiB preview, raw over the limit (median of ", kSamples, "): first ", raw.first_ms, " ms, steady ",
            raw.steady_ms, " ms/frame, rss +", raw.rss_mib.value_or(-1.0), " MiB");
    CHECK(raw.first_ms <= budget(kFrameBudgetMs));
    CHECK(raw.steady_ms <= budget(kFrameBudgetMs));
}

// NX-D: aynı not biçimli (view_bytes = 0). Yerleşim parça parça: yalnız görünen parçalar ayrıştırılıp
// yerleştirilir. Kaydırma kareleri yarım sayfa iki kez ilerler (parça sınırlarını geçer), sonra sona atlanır.
// Bütçe: ilk kare ve sona atlama 50 ms, kaydırmanın kareye eklediği iş 16 ms; bellek notun iki katı (önizleme
// metnin kopyasını tutar). Başsız testin yazılım çizicisi dolu ekran metinde kare başına ~20 ms harcar (ham
// önizleme de öyle): kaydırma karesi, aynı ekranın işsiz karesiyle karşılaştırılır.
TEST_CASE("preview perf: a 20 MiB note previews formatted in chunks within the frame budget") {
    constexpr double kFirstBudgetMs = 50.0;
    constexpr double kScrollWorkBudgetMs = 16.0;
    constexpr double kNoteMib = static_cast<double>(kNoteBytes) / (1024.0 * 1024.0);
    WindowFixture fixture;
    fixture.config.tuning.max_note_bytes = {0, "0"};
    fixture.add_note("büyük.md", large_markdown(), 10);
    const Sample formatted = median_of([&] { return measure(fixture, {0, "0"}, "büyük.md", true); });
    MESSAGE("20 MiB preview, formatted (view_bytes = 0, median of ", kSamples, "): first ", formatted.first_ms,
            " ms, scroll avg ",
            formatted.steady_ms, " / worst ", formatted.worst_ms, " ms/frame, End ", formatted.end_ms,
            " ms, idle ", formatted.idle_ms, " ms/frame, rss +", formatted.rss_mib.value_or(-1.0), " MiB");
    CHECK(formatted.first_ms <= budget(kFirstBudgetMs));
    CHECK(formatted.worst_ms - formatted.idle_ms <= budget(kScrollWorkBudgetMs));
    CHECK(formatted.worst_ms <= budget(kFrameBudgetMs));
    CHECK(formatted.end_ms <= budget(kFirstBudgetMs));
    if (formatted.rss_mib)
        CHECK(*formatted.rss_mib <= 2.0 * kNoteMib);
    else
        MESSAGE("resident memory is not measured on this platform: memory budget skipped");
}

// NX-E H1: yarısı bağlantı başvuru tanımı (~10 MB) olan 20 MiB not; tanım kullanımının yanında. Tanım tablosu her
// parçaya eklenince her parça ~10 MB ayrıştırırdı (kaydırma karesi +40 ms): parçaya yalnız andığı tanımlar eklenir.
// Ön geçişlerin (parçalara bölme ve tanım toplama tek geçiş, tanım tablosu özete göre sıralı tek dizi) süresi ilk
// karenin payı olarak yazılır. İlk kare için adı konmuş ayrı bütçe: ilk parça ayrıştırılmadan önce 190 bin tanımın
// hepsi okunup özetlenmeli (başka parçadaki tanımı bilmek için; 20 MiB'ta ~25 ms, düz notta ~7 ms) ve başsız testin
// yazılım çizicisi kareye ~20 ms ekler. Ölçüm (Release, 3 koşunun her biri 3 örnek medyanı): 54–56 ms; bütçe
// ölçümün ~%25 üstü. Kaydırma ve sona atlama öbür testlerin bütçesindedir.
TEST_CASE("preview perf: a 20 MiB note with 10 MB of link definitions previews within the frame budget") {
    constexpr double kDefinitionHeavyFirstBudgetMs = 70.0;
    constexpr double kEndBudgetMs = 50.0;
    constexpr double kScrollWorkBudgetMs = 16.0;
    std::string note;
    note.reserve(kNoteBytes + 256);
    for (int i = 0; note.size() < kNoteBytes; ++i) {
        const std::string label = "e" + std::to_string(i);
        note += "## Bölüm\n\nMetin [bağ][" + label + "] ve devamı.\n\n[" + label +
                "]: https://example.org/uzun/bir/yol/" + std::to_string(i) + " \"başlık\"\n\n";
    }
    using Clock = std::chrono::steady_clock;
    const auto timed = [](const auto& work) {
        const auto start = Clock::now();
        work();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const std::string plain = large_markdown();
    MESSAGE("pre-passes, 20 MiB without definitions: chunks ",
            timed([&] { (void)noteapp::app::markdown_chunks(plain); }), " ms, definitions ",
            timed([&] { (void)noteapp::app::markdown_link_definitions(plain); }), " ms");
    MESSAGE("pre-passes, 20 MiB with 10 MB of definitions: chunks ",
            timed([&] { (void)noteapp::app::markdown_chunks(note); }), " ms, definitions ",
            timed([&] { (void)noteapp::app::markdown_link_definitions(note); }), " ms");
    WindowFixture fixture;
    fixture.config.tuning.max_note_bytes = {0, "0"};
    fixture.add_note("tanım.md", note, 10);
    const Sample formatted = median_of([&] { return measure(fixture, {0, "0"}, "tanım.md", true); });
    MESSAGE("20 MiB preview with 10 MB of definitions (median of ", kSamples, "): first ", formatted.first_ms, " ms, scroll worst ",
            formatted.worst_ms, " ms/frame, End ", formatted.end_ms, " ms, idle ", formatted.idle_ms, " ms/frame");
    CHECK(formatted.first_ms <= budget(kDefinitionHeavyFirstBudgetMs));
    CHECK(formatted.worst_ms - formatted.idle_ms <= budget(kScrollWorkBudgetMs));
    CHECK(formatted.end_ms <= budget(kEndBudgetMs));
}

// Uç durum: tek satır (base64, küçültülmüş JSON). Görünen satır her karede baştan sona ölçülmemeli.
TEST_CASE("preview perf: a 20 MiB single-line note opens as raw text within the frame budget") {
    WindowFixture fixture;
    fixture.config.tuning.max_note_bytes = {0, "0"};
    fixture.add_note("tek.md", std::string(kNoteBytes, 'x'), 10);
    const Sample raw = median_of([&] { return measure(fixture, {256U * 1024U, "256KiB"}, "tek.md"); });
    MESSAGE("20 MiB single-line preview, raw (median of ", kSamples, "): first ", raw.first_ms, " ms, steady ",
            raw.steady_ms, " ms/frame, rss +", raw.rss_mib.value_or(-1.0), " MiB");
    CHECK(raw.first_ms <= budget(kFrameBudgetMs));
    CHECK(raw.steady_ms <= budget(kFrameBudgetMs));
    // NX-D bilgi: tek blok parçalara bölünmez; biçimli yerleşim tüm satırı sarar (varsayılan sınır bu yüzden kalır).
    const Sample formatted = measure(fixture, {0, "0"}, "tek.md");
    MESSAGE("20 MiB single-line preview, formatted (view_bytes = 0): first ", formatted.first_ms, " ms, steady ",
            formatted.steady_ms, " ms/frame, rss +", formatted.rss_mib.value_or(-1.0), " MiB");
}
