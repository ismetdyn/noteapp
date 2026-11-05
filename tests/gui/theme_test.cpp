#include "gui/theme.hpp"
#include "layout_scenes.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace theme = noteapp::gui::theme;
using gui_test::LayoutScene;

namespace {

constexpr double kTextContrast = 4.5;       // WCAG 1.4.3 AA gövde metni
constexpr double kNonTextContrast = 3.0;    // WCAG 1.4.11 UI sınırı ve odak
constexpr double kCodeShadeContrast = 1.5;  // SPEC-gui: satır içi kod zemini gözle seçilir

// WCAG 2.x göreli parlaklık (opak renk).
double luminance(ImU32 color) {
    const ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    const auto channel = [](float value) {
        const double c = value;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * channel(rgba.x)) + (0.7152 * channel(rgba.y)) + (0.0722 * channel(rgba.z));
}

double contrast(ImU32 a, ImU32 b) {
    const double light = std::max(luminance(a), luminance(b));
    const double dark = std::min(luminance(a), luminance(b));
    return (light + 0.05) / (dark + 0.05);
}

using Named = std::pair<const char*, ImU32>;

void check_all_reach(const std::vector<Named>& colors, ImU32 background, double minimum) {
    for (const auto& [name, color] : colors) {
        const std::string token = name;
        CAPTURE(token);
        CHECK(contrast(color, background) >= minimum);
    }
}

}  // namespace

TEST_CASE("theme: body, muted, status and syntax text reach 4.5:1 on the canvas in both themes") {
    for (const theme::Theme* t : {&theme::kDark, &theme::kLight}) {
        CAPTURE(t->dark);
        check_all_reach({{"text", t->text},
                         {"muted", t->muted},
                         {"danger", t->danger},
                         {"warning", t->warning},
                         {"success", t->success},
                         {"comment", t->comment},
                         {"string", t->string},
                         {"number", t->number},
                         {"type", t->type},
                         {"keyword", t->keyword},
                         {"heading", t->heading}},
                        t->canvas, kTextContrast);
        // Krom: durum çubuğu, kenar çubuğu ve araç çubuğu metni.
        check_all_reach({{"text", t->text}, {"muted", t->muted}, {"danger", t->danger}}, t->chrome,
                        kTextContrast);
        check_all_reach({{"text", t->text}, {"muted", t->muted}}, t->selected, kTextContrast);
        check_all_reach({{"text", t->text}}, t->code_bg, kTextContrast);
        check_all_reach({{"on accent_fill", t->on_fill}}, t->accent_fill, kTextContrast);
        check_all_reach({{"on danger_fill", t->on_fill}}, t->danger_fill, kTextContrast);
    }
}

TEST_CASE("theme: the boundary and the accent reach 3:1 against the surfaces they sit on") {
    for (const theme::Theme* t : {&theme::kDark, &theme::kLight}) {
        CAPTURE(t->dark);
        // hover: girdinin üzerine gelince (FrameBgHovered/Active) sınır kaybolmasın.
        for (const ImU32 surface : {t->input, t->hover, t->canvas, t->chrome}) {
            CAPTURE(surface);
            check_all_reach({{"boundary", t->boundary}, {"accent", t->accent}}, surface, kNonTextContrast);
        }
        CHECK(contrast(t->code_bg, t->canvas) >= kCodeShadeContrast);
    }
}

TEST_CASE("theme: apply_colors maps the tokens onto the ImGui slots and becomes the current theme") {
    ImGuiStyle style;
    for (const theme::Theme* t : {&theme::kDark, &theme::kLight}) {
        CAPTURE(t->dark);
        theme::apply_colors(style, *t);
        CHECK(&theme::current() == t);
        const auto slot = [&style](ImGuiCol col) {
            return ImGui::ColorConvertFloat4ToU32(style.Colors[col]);
        };
        CHECK(slot(ImGuiCol_WindowBg) == t->chrome);
        CHECK(slot(ImGuiCol_PopupBg) == t->raised);
        CHECK(style.Colors[ImGuiCol_PopupBg].w == 1.0F);  // arkadaki yazı diyaloğun içinden görünmez
        CHECK(slot(ImGuiCol_Text) == t->text);
        CHECK(slot(ImGuiCol_Border) == t->boundary);
        CHECK(slot(ImGuiCol_Header) == t->selected);
        CHECK(slot(ImGuiCol_TextLink) == t->accent);
        CHECK(slot(ImGuiCol_NavCursor) == t->accent);
        CHECK(slot(ImGuiCol_TabSelected) == t->canvas);
        // ImGui TextDisabled bilgi taşıyan metinde de kullanılır (ipucu, kesit, kısayol, boş durum):
        // devre dışı değil ikincil metin olarak okunmalı.
        const std::string token = "TextDisabled";
        check_all_reach({{token.c_str(), slot(ImGuiCol_TextDisabled)}}, t->canvas, kTextContrast);
        check_all_reach({{token.c_str(), slot(ImGuiCol_TextDisabled)}}, t->chrome, kTextContrast);
    }
}

TEST_CASE("theme: the view applies the design sizes at every ui scale") {
    constexpr float kFrameRounding = 5.0F;
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        ImGuiStyle previous;
        theme::apply_colors(previous, theme::kDark);  // önceki durum koyu: görünüm temayı gerçekten kurmalı
        LayoutScene scene(1000, 650, scale);
        const ImGuiStyle& style = ImGui::GetStyle();
        CHECK(style.ChildBorderSize == 0.0F);
        CHECK(style.FrameRounding == std::trunc(kFrameRounding * scale));  // ScaleAllSizes ImTrunc ile keser
        CHECK(style.FrameBorderSize >= 1.0F);
        // Dummy sürücüde sistem teması yoktur: açık tema, stil yuvaları da onun.
        CHECK(&theme::current() == &theme::kLight);
        CHECK(ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_WindowBg]) == theme::kLight.chrome);
    }
}
