// K1: arayüz ve editör yazı ailesi Ayarlar → Görünüm'den seçilir (DejaVu varsayılan; Inter, JetBrains Mono
// gömülü). Seçim config değildir: sıra no gibi hemen uygulanır ve gui-state.toml'da kalır.
#include "core/paths.hpp"
#include "gui/gui_state.hpp"
#include "gui/icon_font.hpp"
#include "gui/view.hpp"
#include "layout_scenes.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using gui_test::click_button;
using gui_test::LayoutScene;
using gui_test::make_view;
using gui_test::settle;
using gui_test::WindowFixture;

namespace {

// Görünüm grubu formun sonunda: tekerlekle oraya inilir (gerçek fare yolu).
void scroll_settings_to_end(gui_test::View& view) {
    const auto [x, y] = view.button_center("settings_form");
    ImGui::GetIO().AddMousePosEvent(x, y);
    view.draw_frame();
    constexpr float kWheelNotches = -100.0F;
    ImGui::GetIO().AddMouseWheelEvent(0.0F, kWheelNotches);
    settle(view);
}

void choose(gui_test::View& view, const std::string& setting, const std::string& option) {
    REQUIRE(view.item_rects().contains("setting:" + setting));
    click_button(view, "setting:" + setting);
    REQUIRE(view.item_rects().contains(setting + ":" + option));
    click_button(view, setting + ":" + option);
    // Yeni yüzle etiket sütunu daralır, form bir karede yerleşir: sonraki tık eski konuma düşmesin.
    settle(view);
}

float width_of(const gui_test::View& view, const std::string& name) {
    const auto& rect = view.item_rects().at(name);
    return rect.max_x - rect.min_x;
}

}  // namespace

TEST_CASE("the interface and editor fonts default to DejaVu and are kept in the state file") {
    WindowFixture fixture;
    {
        const auto state = fixture.make_state();
        CHECK(state->layout().ui_font == "dejavu");
        CHECK(state->layout().editor_font == "dejavu-mono");
        state->set_ui_font("inter");
        state->set_editor_font("jetbrains-mono");
        state->set_ui_font("comic-sans");  // listede olmayan ad yok sayılır
    }
    const auto reopened = fixture.make_state();
    CHECK(reopened->layout().ui_font == "inter");
    CHECK(reopened->layout().editor_font == "jetbrains-mono");
}

TEST_CASE("an unknown font name in the state file falls back to DejaVu") {
    WindowFixture fixture;
    std::filesystem::create_directories(noteapp::core::state_dir());
    std::ofstream(noteapp::core::state_dir() / "gui-state.toml")
        << "ui_font = \"wingdings\"\neditor_font = 7\nrow_numbers = true\n";
    const auto state = fixture.make_state();
    CHECK(state->row_numbers());  // dosya okundu: düşüş bozuk dosyadan değil, bilinmeyen addan
    CHECK(state->layout().ui_font == "dejavu");
    CHECK(state->layout().editor_font == "dejavu-mono");
}

TEST_CASE("choosing Inter and JetBrains Mono in Settings redraws the window and the editor with them") {
    WindowFixture fixture;
    fixture.add_note("a.md", "alpha body\n");
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    state->edit_selected();  // editör hücresi ölçülür
    settle(*view);
    const float dejavu_count = width_of(*view, "count");
    const float dejavu_cell = width_of(*view, "editor_cursor");
    CHECK(std::string_view(ImGui::GetIO().FontDefault->GetDebugName()).find("DejaVuSans") !=
          std::string_view::npos);

    state->show_settings_tab();
    settle(*view);
    scroll_settings_to_end(*view);
    choose(*view, "ui_font", "inter");
    choose(*view, "editor_font", "jetbrains-mono");
    CHECK(state->layout().ui_font == "inter");
    CHECK(state->layout().editor_font == "jetbrains-mono");
    CHECK_FALSE(state->settings().has_changes());  // Kaydet gerekmez: config değil

    state->focus_search();
    settle(*view);
    CHECK(std::string_view(ImGui::GetIO().FontDefault->GetDebugName()).find("Inter") !=
          std::string_view::npos);
    CHECK(width_of(*view, "count") < dejavu_count);  // aynı metin Inter'de daha dar
    CHECK(width_of(*view, "editor_cursor") != doctest::Approx(dejavu_cell));
}

// İnceleme 4: gömülü font baytları statiktir; atlas kopyalamaz ve sahiplenmez (kapanışta serbest bırakmaz).
TEST_CASE("the atlas reads every embedded font in place instead of copying it") {
    LayoutScene scene(1000, 650, 1.0F);
    const ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
    REQUIRE(atlas.Sources.Size >= 16);  // DejaVu, Inter, JetBrains Mono, ikonlar ve yedek birleşimleri
    for (const ImFontConfig& source : atlas.Sources) {
        CAPTURE(source.Name);
        CHECK_FALSE(source.FontDataOwnedByAtlas);
    }
    const auto inter = noteapp::gui::embedded_ui_font("fonts/Inter-Regular.ttf");
    REQUIRE(inter.has_value());
    CHECK(std::ranges::any_of(atlas.Sources, [&](const ImFontConfig& source) {
        return source.FontData == static_cast<const void*>(inter->data());
    }));
}
