#include "core/file_icons.hpp"
#include "gui/icon_font.hpp"
#include "gui/view.hpp"
#include "layout_scenes.hpp"

#include <SDL3/SDL.h>
#include <cstddef>
#include <doctest/doctest.h>
#include <imgui.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using gui_test::LayoutScene;
using noteapp::gui::icon_glyph_ranges;

namespace {

constexpr std::size_t kEmbeddedGlyphs = 50;  // 30 dosya ve klasör ikonu, 20 arayüz eylemi

std::vector<ImWchar> icon_code_points() {
    const std::vector<ImWchar> ranges = icon_glyph_ranges();
    std::vector<ImWchar> points;
    for (std::size_t i = 0; i + 1 < ranges.size(); i += 2)
        points.push_back(ranges[i]);
    return points;
}

}  // namespace

TEST_CASE("the atlas ranges name exactly the table glyphs, file and action icons, as single code point ranges") {
    const std::vector<ImWchar> ranges = icon_glyph_ranges();
    REQUIRE(ranges.size() == kEmbeddedGlyphs * 2 + 1);
    CHECK(ranges.back() == 0);
    for (std::size_t i = 0; i + 1 < ranges.size(); i += 2)
        CHECK(ranges[i] == ranges[i + 1]);
}

TEST_CASE("the embedded icon font is a small subset, not the whole Nerd Font") {
    constexpr std::size_t kMaxSubsetBytes = 64 * 1024;
    const auto font = noteapp::gui::embedded_icon_font("fonts/NoteappIcons.ttf");
    REQUIRE(font.has_value());
    CHECK(font->size() < kMaxSubsetBytes);
}

// View::create ikon yüklenemezse sessizce ikonsuz devam eder; bozuk gömülü PNG ancak burada görünür.
TEST_CASE("the embedded window icons decode to square images of their nominal size") {
    for (const auto& [resource, size] : {std::pair{"icons/hicolor/64x64/apps/noteapp.png", 64},
                                         std::pair{"icons/hicolor/256x256/apps/noteapp.png", 256}}) {
        CAPTURE(resource);
        const auto png = noteapp::gui::embedded_app_icon(resource);
        REQUIRE(png.has_value());
        SDL_Surface* surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(png->data(), png->size()), true);
        REQUIRE_MESSAGE(surface != nullptr, SDL_GetError());
        CHECK(surface->w == size);
        CHECK(surface->h == size);
        SDL_DestroySurface(surface);
    }
}

TEST_CASE("every table glyph is in both selectable UI fonts and has visible pixels in a dummy driver frame") {
    LayoutScene scene(1000, 650, 1.0F);
    scene.view->draw_frame();
    // DejaVu ilk fonttur; Inter Ayarlar → Görünüm'de seçilir, ikonlar ona da birleşir.
    ImFont* inter = nullptr;
    for (ImFont* font : ImGui::GetIO().Fonts->Fonts)
        if (std::string_view(font->GetDebugName()) == "fonts/Inter-Regular.ttf")
            inter = font;
    REQUIRE(inter != nullptr);
    for (ImFont* font : {ImGui::GetIO().Fonts->Fonts[0], inter}) {
        const std::string name = font->GetDebugName();
        CAPTURE(name);
        ImFontBaked* baked = font->GetFontBaked(ImGui::GetFontSize());
        REQUIRE(baked != nullptr);
        for (const ImWchar point : icon_code_points()) {
            CAPTURE(static_cast<unsigned>(point));
            const ImFontGlyph* glyph = baked->FindGlyphNoFallback(point);
            REQUIRE(glyph != nullptr);
            CHECK(glyph->Visible);
            CHECK(glyph->X1 > glyph->X0);
        }
    }
}

TEST_CASE("a Nerd Font glyph outside the table is not in the merged UI font") {
    LayoutScene scene(1000, 650, 1.0F);
    scene.view->draw_frame();
    constexpr ImWchar kFontAwesomeTrash = 0xF1F8;
    ImFontBaked* baked = ImGui::GetIO().Fonts->Fonts[0]->GetFontBaked(ImGui::GetFontSize());
    CHECK(baked->FindGlyphNoFallback(kFontAwesomeTrash) == nullptr);
}

TEST_CASE("row icons are drawn visibly larger than the text and every glyph fits the row's slot") {
    LayoutScene scene(1000, 650, 1.0F);
    scene.view->draw_frame();
    CHECK(noteapp::gui::View::kRowIconScale >= 1.3F);
    const float font_size = ImGui::GetFontSize();
    const float icon_size = font_size * noteapp::gui::View::kRowIconScale;
    const float slot = font_size * noteapp::gui::View::kRowIconSlotEms;
    for (const ImWchar point : icon_code_points()) {
        CAPTURE(static_cast<unsigned>(point));
        ImWchar text[2] = {point, 0};
        char utf8[8] = {};
        ImTextStrToUtf8(utf8, sizeof(utf8), text, text + 1);
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(icon_size, 10000.0F, 0.0F, utf8);
        CHECK(size.x <= slot);
    }
}
