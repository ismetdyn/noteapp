// Önizleme yerleşimi (markdown_preview, ImGui'den yalnız font ölçümü): sarma maliyeti ve görünen koşular.
#include "app/data_preview.hpp"
#include "gui/gui_state.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/theme.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;
using noteapp::gui::layout_preview;
using noteapp::gui::PreviewFonts;
using noteapp::gui::PreviewLayout;
using noteapp::gui::PreviewRun;

namespace pdf = noteapp::pdf;

namespace {

constexpr float kFontPixels = 16.0F;

// Gerçek atlas: View kurulunca gömülü UI fontu yüklüdür; yüzlerin hepsi o fonttur (ölçüm için yeter).
struct FontScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state = fixture.make_state();
    std::unique_ptr<gui_test::View> view = make_view(*state);
    PreviewFonts fonts() const {
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
        return {font, font, font, font, font};
    }
};

bool resolve_all(std::string_view /*target*/) {
    return true;
}

pdf::Block paragraph(std::string text) {
    pdf::Block block;
    block.spans.push_back({std::move(text), {}});
    return block;
}

}  // namespace

constexpr std::size_t kWordBytes = std::size_t{256} * 1024;  // limits.tui.view_bytes varsayılanı

TEST_CASE("preview layout: one 256 KiB word wraps in linear time and keeps every byte inside the width") {
    const FontScene scene;
    const std::vector<pdf::Block> blocks{paragraph(std::string(kWordBytes, 'x'))};
    constexpr float kNarrowWidth = 200.0F;
    const auto start = std::chrono::steady_clock::now();
    const PreviewLayout layout = layout_preview(blocks, scene.fonts(), kFontPixels, kNarrowWidth, resolve_all);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::size_t bytes = 0;
    bool inside = true;
    for (const PreviewRun& run : layout.runs) {
        bytes += run.text.size();
        inside = inside && run.pos.x + run.width <= kNarrowWidth + 0.5F;
    }
    CHECK(bytes == kWordBytes);
    CHECK(inside);
    CHECK(layout.runs.size() > 1000);  // gerçekten sarıldı: tek koşu yanlış yeşil olmasın
    // Kare bütçesi değil, karesel davranışı yakalayan sınır: eski kod bu boyutta 4,7 sn sürüyordu (Debug).
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("preview layout: a heading in a list item is drawn when only its top is in view") {
    const FontScene scene;
    pdf::Block heading = paragraph("Büyük");
    heading.kind = pdf::Block::Kind::Heading;
    heading.heading_level = 1;
    heading.list_depth = 1;
    heading.marker = "• ";
    const std::vector<pdf::Block> blocks{heading};
    constexpr float kWidth = 400.0F;
    const PreviewLayout layout = layout_preview(blocks, scene.fonts(), kFontPixels, kWidth, resolve_all);
    const PreviewRun* marker = nullptr;
    const PreviewRun* title = nullptr;
    for (const PreviewRun& run : layout.runs) {
        if (run.text == "•")
            marker = &run;
        if (run.text == "Büyük")
            title = &run;
    }
    REQUIRE(marker != nullptr);
    REQUIRE(title != nullptr);
    // Küçük im büyük başlığın satırında ortalanır: başlık metni imden yukarıda başlar.
    REQUIRE(marker->pos.y > title->pos.y);
    const float bottom = (title->pos.y + marker->pos.y) / 2.0F;  // başlığın üstü görünür, imin üstü değil
    bool drawn = false;
    for (const PreviewRun& run : noteapp::gui::visible_runs(layout, 0.0F, bottom))
        drawn = drawn || run.text == "Büyük";
    CHECK(drawn);
}

TEST_CASE("preview layout: an unresolved [[target]] is dimmed and struck, with nothing to click") {
    const FontScene scene;
    pdf::Block block;
    block.spans.push_back({"b", {}, "b", true});
    block.spans.push_back({" ve ", {}});
    block.spans.push_back({"yok", {}, "yok", true});
    block.spans.push_back({" ve ", {}});
    block.spans.push_back({"site", {}, "https://x.example", false});
    const std::vector<pdf::Block> blocks{block};
    const PreviewLayout layout = layout_preview(blocks, scene.fonts(), kFontPixels, 400.0F,
                                                [](std::string_view target) { return target == "b"; });
    const auto ink_of = [&](std::string_view text) {
        for (const PreviewRun& run : layout.runs) {
            if (run.text == text)
                return run.ink;
        }
        FAIL("no run: ", std::string(text));
        return noteapp::gui::PreviewInk::Text;
    };
    CHECK(ink_of("b") == noteapp::gui::PreviewInk::Link);
    CHECK(ink_of("yok") == noteapp::gui::PreviewInk::Missing);
    CHECK(ink_of("site") == noteapp::gui::PreviewInk::Link);  // dış bağlantı çözülmez, ipucunda kalır
    std::vector<std::string> targets;
    for (const auto& hit : layout.hits)
        targets.emplace_back(hit.target);
    CHECK(targets == std::vector<std::string>{"b", "https://x.example"});
}

namespace {

// Parçalara bölünen not: önce paragraf ağırlıklı, sonra kod ağırlıklı (bayt başına yükseklik değişir, tahmin
// tutmaz).
std::string chunked_markdown() {
    const std::string prose = "## Bölüm\n\nBir paragraf *vurgu* ve **kalın** metin [[hedef]] ile; satır sarılsın diye "
                              "biraz uzun yazılmış bir cümle daha ekleniyor burada.\n\n- madde bir\n- madde iki\n\n";
    const std::string code = "> alıntı\n\n```\nkod\nkod\nkod\nkod\n```\n\n";
    std::string text;
    while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
        text += prose;
    while (text.size() < 4 * noteapp::app::kMarkdownChunkBytes)
        text += code;
    return text;
}


// Kesim yerlerinin her türü: setext başlık, sıralı liste ve öğedeki girintili çit (imden sonra sekmeli de), alıntı,
// tablo, görev listesi, yıldızlı çizgi, başvuru bağlantısı (tanımı sonda; etiketi satır sonlu, `\]` kaçışlı ya da
// Unicode büyük/küçük harf farklı, ß = ss), sonda kapanmamış çit. `crlf`: satır sonları CRLF.
std::string varied_markdown(std::size_t bytes, bool crlf) {
    const std::string unit = "Başlık\n======\n\n1. bir\n2. iki\n   ```\n   kod\n   ```\n\n> alıntı\n> devam\n\n"
                             "| a | b |\n|---|---|\n| 1 | 2 |\n\nAlt başlık\n---\n\n[bağ][r] ve *eğik*\n\n"
                             "- [x] görev\n- [ ] kalan\n\n***\n\n- ```sh\n  echo\n  ```\n\n-\t```\n\tsekme\n```\n\n"
                             "[çok satırlı][my\nlink] ve [kaçış][a\\]b] ve [dış][STRASSE] ve [Çağ][]\n\n";
    std::string text;
    while (text.size() < bytes)
        text += unit;
    text += "[r]: https://r.example\n[my link]: https://ml.example\n[a\\]b]: https://esc.example\n"
            "[straße]: https://ss.example\n[ÇAĞ]: https://cag.example\n\n```\nkapanmamış\n";
    if (!crlf)
        return text;
    std::string out;
    for (const char c : text) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

}  // namespace

// NX-D: parça parça yerleşim tümü yerleşince bütün yerleşimle aynı yerde aynı koşuları ve bağlantıları çizer.
TEST_CASE("preview document: chunks laid out together match the whole layout") {
    const FontScene scene;
    std::string text;
    SUBCASE("prose and code") { text = chunked_markdown(); }
    SUBCASE("every kind of block, LF") { text = varied_markdown(4 * noteapp::app::kMarkdownChunkBytes, false); }
    SUBCASE("every kind of block, CRLF") { text = varied_markdown(4 * noteapp::app::kMarkdownChunkBytes, true); }
    REQUIRE(noteapp::app::markdown_chunks(text).size() >= 4);
    constexpr float kWidth = 500.0F;
    const std::vector<pdf::Block> parsed = pdf::parse_markdown(text, {.links = true});
    const PreviewLayout whole = layout_preview(parsed, scene.fonts(), kFontPixels, kWidth, resolve_all);
    noteapp::gui::PreviewDocument document;
    document.reset(text);
    const float view = whole.size.y * 2.0F;  // her parça görünür
    CHECK(document.update(0.0F, view, scene.fonts(), kFontPixels, kWidth, resolve_all) == 0.0F);
    // Bütün yerleşim 600 bin pikselde float birikimiyle ~200 px kayar; parça yerel koordinatla daha az. Karşılaştırma
    // ardışık koşular arası farkla: kaçan bir boşluk (≥ 2 px) yakalanır, birikim yakalanmaz.
    constexpr float kStepTolerance = 0.5F;
    CHECK(document.size().y == doctest::Approx(whole.size.y).epsilon(1e-3));
    CHECK(document.size().x == doctest::Approx(whole.size.x));
    std::size_t run = 0;
    std::size_t blocks = 0;
    float previous = 0.0F;
    std::vector<std::string_view> targets;
    for (const auto& placed : document.visible(0.0F, view)) {
        REQUIRE(placed.first_block);
        CHECK(*placed.first_block == blocks);
        blocks += placed.layout->block_spans.size();
        for (const auto& hit : placed.layout->hits)
            targets.push_back(hit.target);
        for (const PreviewRun& part : placed.layout->runs) {
            REQUIRE(run < whole.runs.size());
            CAPTURE(run);
            REQUIRE(part.text == whole.runs[run].text);
            REQUIRE(part.pos.x == doctest::Approx(whole.runs[run].pos.x));
            const float step = placed.top + part.pos.y - previous;
            const float whole_step = whole.runs[run].pos.y - (run == 0 ? 0.0F : whole.runs[run - 1].pos.y);
            REQUIRE(std::abs(step - whole_step) < kStepTolerance);
            previous = placed.top + part.pos.y;
            ++run;
        }
    }
    CHECK(run == whole.runs.size());
    CHECK(blocks == parsed.size());
    std::vector<std::string_view> whole_targets;
    for (const auto& hit : whole.hits)
        whole_targets.push_back(hit.target);
    CHECK(targets == whole_targets);
}

// Kısa satırlı notun tahmini yüksekliği gerçeğin çok altındadır: tahmine göre "sonda" sayılmamalı. İlk kare başta,
// geri yüklenen kaydırma yerinde kalır; sona yalnız istenince (End) ya da son parça ölçülmüşken gidilir.
TEST_CASE("preview document: an estimated height does not pull the view to the end") {
    const FontScene scene;
    std::string text;
    while (text.size() < 2 * 1024)
        text += "- a\n";
    constexpr float kWidth = 500.0F;
    constexpr float kView = 600.0F;
    noteapp::gui::PreviewDocument fresh;
    fresh.reset(text);
    CHECK(fresh.update(0.0F, kView, scene.fonts(), kFontPixels, kWidth, resolve_all) == 0.0F);
    REQUIRE(fresh.size().y > 2.0F * kView);

    noteapp::gui::PreviewDocument restored;
    restored.reset(text);
    CHECK(restored.update(300.0F, kView, scene.fonts(), kFontPixels, kWidth, resolve_all) == 300.0F);

    noteapp::gui::PreviewDocument jumped;
    jumped.reset(text);
    const float end = jumped.update(100.0F, kView, scene.fonts(), kFontPixels, kWidth, resolve_all, true);
    CHECK(end == doctest::Approx(jumped.size().y - kView));
}

// NX-E F1: aynı etiketin iki tanımında belge sırasıyla ilki kazanır (CommonMark), sonraki parçadaki tekrar da.
// NX-F: Unicode büyük/küçük harf farkıyla yazılmış tekrar da (ÇAĞ = çağ, md4c katlaması).
TEST_CASE("preview document: the first definition of a label wins across chunks") {
    const FontScene scene;
    std::string text = "[a]: https://first.example\n\n[ÇAĞ]: https://first-cag.example\n\n";
    // Tekrar ve kullanımı aynı parçanın ortasında: parçanın kendi tanımı eklenen tablodan önce gelir.
    while (text.size() < 100 * 1024)
        text += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    text += "[a]: https://second.example\n\n[son][a]\n\n[çağ]: https://second-cag.example\n\n[son][Çağ]\n\n";
    while (text.size() < 200 * 1024)
        text += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    REQUIRE(noteapp::app::markdown_chunks(text).size() > 2);
    noteapp::gui::PreviewDocument document;
    document.reset(text);
    constexpr float kEverything = 1.0e9F;  // her parça görünür
    document.update(0.0F, kEverything, scene.fonts(), kFontPixels, 500.0F, resolve_all);
    std::vector<std::string> targets;
    for (const auto& placed : document.visible(0.0F, kEverything))
        for (const auto& hit : placed.layout->hits)
            targets.emplace_back(hit.target);
    CHECK(targets == std::vector<std::string>{"https://first.example", "https://first-cag.example"});
}

// Bağlantı başvuru tanımı notun her yerinde geçerlidir (CommonMark): sondaki tanım ilk parçadaki kullanımı çözer.
TEST_CASE("preview document: a reference defined at the end of a chunked note resolves in the first chunk") {
    const FontScene scene;
    std::string text = "[başvuru][r] ile başlar.\n\n";
    while (text.size() < 100 * 1024)
        text += "## Bölüm\n\nBir paragraf metni burada.\n\n";
    text += "[r]: https://example.org/r\n";
    REQUIRE(noteapp::app::markdown_chunks(text).size() > 1);
    noteapp::gui::PreviewDocument document;
    document.reset(text);
    constexpr float kView = 600.0F;
    document.update(0.0F, kView, scene.fonts(), kFontPixels, 500.0F, resolve_all);
    const auto placed = document.visible(0.0F, kView);
    REQUIRE_FALSE(placed.empty());
    std::vector<std::string> targets;
    for (const auto& hit : placed.front().layout->hits)
        targets.emplace_back(hit.target);
    CHECK(targets == std::vector<std::string>{"https://example.org/r"});
}

// Yalnız görünen alan yerleşik kalır; yukarıdaki tahmini parça ölçülünce görünen metin ekranda yerinde durur.
TEST_CASE("preview document: only the visible window stays laid out and a measured estimate keeps the view still") {
    const FontScene scene;
    const std::string text = chunked_markdown();
    constexpr float kWidth = 500.0F;
    constexpr float kView = 600.0F;
    noteapp::gui::PreviewDocument document;
    document.reset(text);
    const float end = document.update(FLT_MAX, kView, scene.fonts(), kFontPixels, kWidth, resolve_all);
    const auto at_end = document.visible(0.0F, document.size().y);
    REQUIRE_FALSE(at_end.empty());
    CHECK(at_end.front().chunk > 0);  // baştaki parçalar hiç yerleşmedi
    CHECK(at_end.back().top + at_end.back().layout->size.y == doctest::Approx(document.size().y));
    CHECK(end == doctest::Approx(document.size().y - kView));

    // Görünen ilk parçanın yarım ekran üstü: üstteki parça ölçülür, görünen parça ekranda kıpırdamaz.
    const auto& anchor = at_end.front();
    const float requested = anchor.top - (kView / 2.0F);
    const float on_screen = anchor.top - requested;
    const float scroll = document.update(requested, kView, scene.fonts(), kFontPixels, kWidth, resolve_all);
    bool found = false;
    for (const auto& placed : document.visible(scroll, scroll + kView))
        if (placed.chunk == anchor.chunk) {
            found = true;
            CHECK(placed.top - scroll == doctest::Approx(on_screen));
        }
    CHECK(found);
}

namespace {

// WCAG göreli parlaklığı ve iki rengin karşıtlık oranı (opak renkler).
double luminance(const ImVec4& color) {
    const auto channel = [](float value) {
        const double c = value;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * channel(color.x)) + (0.7152 * channel(color.y)) + (0.0722 * channel(color.z));
}

double contrast(const ImVec4& a, const ImVec4& b) {
    const double light = std::max(luminance(a), luminance(b));
    const double dark = std::min(luminance(a), luminance(b));
    return (light + 0.05) / (dark + 0.05);
}

}  // namespace

TEST_CASE("preview layout: inline code shading stands out in the light and the dark theme") {
    const FontScene scene;
    constexpr double kMinShadeContrast = 1.5;  // zemin farkı gözle seçilir; eski açık tema 1,14'tü
    constexpr double kMinTextContrast = 4.5;   // WCAG AA gövde metni
    // Önizleme tuval zeminindedir; kod zemini temanın code_bg token'ı.
    namespace theme = noteapp::gui::theme;
    for (const theme::Theme* colors : {&theme::kLight, &theme::kDark}) {
        CAPTURE(colors->dark);
        const ImVec4 shade = ImGui::ColorConvertU32ToFloat4(colors->code_bg);
        REQUIRE(shade.w == 1.0F);  // saydam renk karşıtlığı burada ölçülemezdi
        CHECK(contrast(shade, ImGui::ColorConvertU32ToFloat4(colors->canvas)) >= kMinShadeContrast);
        CHECK(contrast(shade, ImGui::ColorConvertU32ToFloat4(colors->text)) >= kMinTextContrast);
    }
}

// NY-P uygunluk (md_conformance): blok modelinin taşıdığı üstü çizili, kap sırası ve tablo hizası çizimde de
// görünür.
namespace {

// Yerleşim blokların metnine işaret eder (layout_preview sözleşmesi): bloklar yerleşimle birlikte yaşar. Vektörün
// taşınması öğelerin yerini değiştirmez.
struct Laid {
    std::vector<pdf::Block> blocks;
    PreviewLayout layout;
};

Laid laid_out(const FontScene& scene, std::string_view markdown, float width) {
    Laid out{pdf::parse_markdown(markdown, {.links = true}), {}};
    out.layout = layout_preview(out.blocks, scene.fonts(), kFontPixels, width, resolve_all);
    return out;
}

const PreviewRun& run_of(const PreviewLayout& layout, std::string_view text) {
    for (const PreviewRun& run : layout.runs)
        if (run.text == text)
            return run;
    FAIL("no run: ", std::string(text));
    return layout.runs.front();
}

}  // namespace

TEST_CASE("preview layout: struck text is drawn struck") {
    const FontScene scene;
    const Laid laid = laid_out(scene, "~~eski~~ yeni", 400.0F);
    const PreviewLayout& layout = laid.layout;
    CHECK(run_of(layout, "eski").strike);
    CHECK_FALSE(run_of(layout, "yeni").strike);
}

TEST_CASE("preview layout: a quote inside a list item keeps its bar at the item's text, not the margin") {
    const FontScene scene;
    const Laid laid = laid_out(scene, "- madde\n\n  > alıntı\n", 400.0F);
    const PreviewLayout& layout = laid.layout;
    const float item_left = run_of(layout, "madde").pos.x;
    std::vector<float> bars;
    for (const auto& box : layout.boxes)
        if (box.kind == noteapp::gui::PreviewBox::Kind::QuoteBar)
            bars.push_back(box.min.x);
    REQUIRE(bars.size() == 1);
    CHECK(bars.front() >= item_left);
    CHECK(run_of(layout, "alıntı").pos.x > bars.front());
}

// SD5a: liste öğesindeki alıntı zinciri ("- > - > …") her blokta her alıntı düzeyine ayrı çubuk verseydi kutular
// derinliğin karesi olurdu. Ardışık blokların aynı çubuğu tek kutudur: alıntı başına bir kutu.
TEST_CASE("preview layout: quotes nested in list items give one bar per quote, not one per block and level") {
    const FontScene scene;
    constexpr std::size_t kDepth = 2000;
    std::string markdown;
    for (std::size_t i = 0; i < kDepth; ++i)
        markdown += "- > ";
    markdown += "x\n";
    const Laid laid = laid_out(scene, markdown, 400.0F);
    std::size_t bars = 0;
    for (const auto& box : laid.layout.boxes)
        bars += box.kind == noteapp::gui::PreviewBox::Kind::QuoteBar ? 1 : 0;
    CHECK(laid.blocks.size() >= kDepth);
    CHECK(bars >= 1);
    CHECK(bars <= kDepth);
}

// SD5a: çubuklar blok başına kutu yerine süren alıntı başına tek kutu oldu; çizilen yer değişmedi. Beklenenler
// 7dab3ce^'in (blok başına kutu, yol karşılaştırması) piksel çıktısıyla farklı karşılaştırmada birebir doğrulandı:
// her çubuk [ilk bloğun üstü, son bloğun altı], x'e göre sıralı. Aynı türde ardışık kaplar (iki ayrı "> a", "> b")
// eskisi gibi aradaki boşlukta birleşir; ayrı öğelerdeki alıntılar ("- > a", "- > b") ayrı kalır.
TEST_CASE("preview layout: quote bars cover exactly what the per-block bars covered") {
    struct Case {
        std::string_view markdown;
        std::vector<std::pair<std::size_t, std::size_t>> bars;  // (ilk blok, son blok)
    };
    const std::vector<Case> cases = {
        {"- > a\n- > b\n", {{1, 1}, {3, 3}}},
        {"> a\n\n---\n\n> b\n", {{0, 0}, {2, 2}}},
        {"> - a\n>\n> text\n", {{0, 1}}},
        {"- a\n\n  > - b\n  >   - c\n  >\n  > d\n\n- e\n", {{1, 3}}},
        {"> > > a\n\nb\n", {{0, 0}, {0, 0}, {0, 0}}},
        {"> - > - a\n\nb\n", {{0, 1}, {1, 1}}},
        {"> a\n\n  > b\n", {{0, 1}}},
        {"- a\n\n  > in item\n\n> top\n", {{2, 2}, {1, 1}}},
        {"> a\n\n> b\n", {{0, 1}}},
        {"> a\n>\n> b\n", {{0, 1}}},
        {"- > - > a\n  >   > b\n  > c\n- > d\n", {{1, 2}, {4, 4}, {2, 2}}},
        {"> 1. a\n>    > q\n> 2. b\n>\n> > deep\n> > > deeper\n> tail\n", {{0, 4}, {3, 4}, {4, 4}, {1, 1}}},
    };
    const FontScene scene;
    for (const Case& test : cases) {
        CAPTURE(test.markdown);
        const Laid laid = laid_out(scene, test.markdown, 400.0F);
        std::vector<noteapp::gui::PreviewBox> bars;
        for (const auto& box : laid.layout.boxes)
            if (box.kind == noteapp::gui::PreviewBox::Kind::QuoteBar)
                bars.push_back(box);
        std::ranges::sort(bars, {}, [](const noteapp::gui::PreviewBox& box) { return std::pair(box.min.x, box.min.y); });
        REQUIRE(bars.size() == test.bars.size());
        const auto& spans = laid.layout.block_spans;
        for (std::size_t at = 0; at < bars.size(); ++at) {
            const auto [first, last] = test.bars[at];
            REQUIRE(last < spans.size());
            CHECK(bars[at].min.y == doctest::Approx(spans[first].first));
            CHECK(bars[at].max.y == doctest::Approx(spans[last].second));
        }
    }
}

// İki ayrı liste HTML'deki gibi paragraf aralığıyla ayrılır; aynı listenin öğeleri ve iç listenin ilk öğesi sık kalır.
TEST_CASE("preview layout: adjacent separate lists keep a block gap, items of one list stay tight") {
    const FontScene scene;
    const auto gap_after_first = [&](std::string_view markdown) {
        const Laid laid = laid_out(scene, markdown, 400.0F);
        const PreviewLayout& layout = laid.layout;
        REQUIRE(layout.block_spans.size() >= 2);
        return layout.block_spans[1].first - layout.block_spans[0].second;
    };
    const float same_list = gap_after_first("- a\n- b\n");
    CHECK(gap_after_first("- a\n\n1. b\n") > same_list);
    CHECK(gap_after_first("- a\n\n* b\n") > same_list);
    CHECK(gap_after_first("- a\n  - b\n") == doctest::Approx(same_list));
}

// Lider kararı (NY-P): GUI kod bloğu TUI gibi dil etiketinden renklenir (ortak sözdizimi ve aile tablosu; tam ad
// `python` da kısaltmaya iner). Etiketsiz kod düz kalır.
TEST_CASE("preview layout: a fenced code block is coloured by its language like the TUI") {
    const FontScene scene;
    const auto token_of = [&](std::string_view markdown, std::string_view word) {
        const Laid laid = laid_out(scene, markdown, 400.0F);
        for (const PreviewRun& run : laid.layout.runs)
            if (run.text.find(word) != std::string_view::npos)
                return run.token;
        FAIL("no run with: ", std::string(word));
        return noteapp::tui::TokenKind::Plain;
    };
    CHECK(token_of("```python\ndef f():\n    return 1\n```\n", "def") == noteapp::tui::TokenKind::Keyword);
    CHECK(token_of("```py\nx = \"dizge\"\n```\n", "dizge") == noteapp::tui::TokenKind::String);
    CHECK(token_of("```\ndef f():\n```\n", "def") == noteapp::tui::TokenKind::Plain);
}

TEST_CASE("preview layout: right and center aligned table columns line up like the source asks") {
    const FontScene scene;
    const std::string table =
        "| sol | sağ | orta |\n|-----|----:|:----:|\n| a | 1 | x |\n| bbbbbb | 22222 | yyyyy |\n";
    const Laid laid = laid_out(scene, table, 600.0F);
    const PreviewLayout& layout = laid.layout;
    const auto right_of = [&](std::string_view text) {
        const PreviewRun& run = run_of(layout, text);
        return run.pos.x + run.width;
    };
    const auto middle_of = [&](std::string_view text) {
        const PreviewRun& run = run_of(layout, text);
        return run.pos.x + (run.width / 2.0F);
    };
    CHECK(run_of(layout, "a").pos.x == doctest::Approx(run_of(layout, "bbbbbb").pos.x));
    CHECK(right_of("1") == doctest::Approx(right_of("22222")));
    CHECK(middle_of("x") == doctest::Approx(middle_of("yyyyy")).epsilon(0.02));
}
