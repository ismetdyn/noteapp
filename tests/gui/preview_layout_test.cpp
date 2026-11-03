// Önizleme yerleşimi (markdown_preview, ImGui'den yalnız font ölçümü): sarma maliyeti ve görünen koşular.
#include "gui/gui_state.hpp"
#include "gui/markdown_preview.hpp"
#include "window_fixture.hpp"

#include <algorithm>
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

TEST_CASE("preview layout: one 256 KiB word wraps in linear time and keeps every byte inside the width") {
    const FontScene scene;
    const std::vector<pdf::Block> blocks{paragraph(std::string(GuiState::kMaxPreviewBytes, 'x'))};
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
    CHECK(bytes == GuiState::kMaxPreviewBytes);
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
    for (const bool dark : {false, true}) {
        CAPTURE(dark);
        if (dark)
            ImGui::StyleColorsDark();
        else
            ImGui::StyleColorsLight();
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec4 shade = style.Colors[noteapp::gui::kInlineCodeBackground];
        REQUIRE(shade.w == 1.0F);  // saydam renk karşıtlığı burada ölçülemezdi
        CHECK(contrast(shade, style.Colors[ImGuiCol_WindowBg]) >= kMinShadeContrast);
        CHECK(contrast(shade, style.Colors[ImGuiCol_Text]) >= kMinTextContrast);
    }
}
