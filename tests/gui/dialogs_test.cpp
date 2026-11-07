// G11 ve G12: diyalog ve palet görünümü (.engineering/design/ui-redesign.md §4.9). Davranış (varsayılan odak,
// Esc, klavye gezintisi) kendi testlerinde; burada düğme rolü, hizası ve yerleşim. Renk koyu temada son
// kareden okunur.
#include "core/i18n.hpp"
#include "gui/theme.hpp"
#include "layout_scenes.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>

using gui_test::LayoutScene;
using gui_test::View;
using ItemRects = gui_test::ItemRects;

namespace {

// Koyu tema ve fare dışarıda: üzerine gelme rengi karışmasın.
void dark(LayoutScene& scene) {
    noteapp::gui::theme::apply_colors(ImGui::GetStyle(), noteapp::gui::theme::kDark);
    ImGui::GetIO().AddMousePosEvent(-1.0F, -1.0F);
    gui_test::settle(*scene.view);
}

// Düğmenin sol kenarına yakın, metnin dışında kalan zemin.
std::uint32_t fill_of(const View::ItemRect& rect) {
    constexpr float kInset = 3.0F;
    return gui_test::pixel_at(rect.min_x + kInset, (rect.min_y + rect.max_y) / 2.0F);
}

bool near(std::uint32_t got, std::uint32_t want) {
    constexpr int kChannelTolerance = 2;
    for (const int shift : {16, 8, 0}) {
        const int a = static_cast<int>((got >> shift) & 0xFFU);
        const int b = static_cast<int>((want >> shift) & 0xFFU);
        if (std::abs(a - b) > kChannelTolerance)
            return false;
    }
    return true;
}

constexpr std::uint32_t kDangerFill = 0xB83A3F;
constexpr std::uint32_t kAccentFill = 0x3B6FD1;
constexpr std::uint32_t kRaised = 0x24272E;
constexpr float kModalPad = 16.0F;

}  // namespace

TEST_CASE("the delete confirmation puts its buttons on the right, Delete filled red and Cancel as a ghost") {
    LayoutScene scene(1000, 650, 1.0F);
    dark(scene);
    ItemRects rects;
    std::uint32_t delete_fill = 0;
    std::uint32_t cancel_fill = 0;
    scene.with_dialog(
        [&] { scene.state->deciders.confirm_delete(noteapp::core::tr("gui.trash_empty_hint"), {}); },
        [&] {
            rects = scene.view->item_rects();
            delete_fill = fill_of(rects.at("choice:0"));
            cancel_fill = fill_of(rects.at("choice:1"));
        });
    REQUIRE(rects.contains("modal"));
    CHECK(near(delete_fill, kDangerFill));
    CHECK(near(cancel_fill, kRaised));  // hayalet: diyaloğun zemini
    CHECK(rects.at("modal").max_x - rects.at("choice:1").max_x == doctest::Approx(kModalPad).epsilon(0.1));
    CHECK(rects.at("choice:0").max_x < rects.at("choice:1").min_x);  // sıra aynı: Sil, Vazgeç
}

TEST_CASE("the name dialog ends with a primary OK and a ghost Cancel on the right") {
    LayoutScene scene(1000, 650, 1.0F);
    dark(scene);
    ItemRects rects;
    std::uint32_t ok_fill = 0;
    scene.with_dialog(
        [&] { scene.state->deciders.folder_name(noteapp::core::tr("gui.new_folder_title"), ""); },
        [&] {
            rects = scene.view->item_rects();
            ok_fill = fill_of(rects.at("dialog_ok"));
        });
    REQUIRE(rects.contains("dialog_cancel"));
    CHECK(near(ok_fill, kAccentFill));
    CHECK(rects.at("modal").max_x - rects.at("dialog_cancel").max_x ==
          doctest::Approx(kModalPad).epsilon(0.1));
}

TEST_CASE("the palette is 36 em wide, sits 18 % from the top and shows shortcuts as right-aligned chips") {
    LayoutScene scene(1000, 650, 1.0F);
    ItemRects rects;
    scene.with_dialog([&] { scene.state->open_palette(); }, [&] { rects = scene.view->item_rects(); });
    REQUIRE(rects.contains("palette_input"));
    const View::ItemRect& input = rects.at("palette_input");
    CHECK(input.max_x - input.min_x == doctest::Approx(36.0F * 16.0F));  // NY-G: gövde 16 px
    CHECK(input.max_y - input.min_y == doctest::Approx(32.0F));
    CHECK(rects.at("modal").min_y == doctest::Approx(0.18F * ImGui::GetIO().DisplaySize.y).epsilon(0.02));
    REQUIRE(rects.contains("palette:new"));
    REQUIRE(rects.contains("palette_keys:new"));  // Ctrl+N
    const View::ItemRect& row = rects.at("palette:new");
    const View::ItemRect& chip = rects.at("palette_keys:new");
    CHECK(row.max_y - row.min_y == doctest::Approx(26.0F));
    CHECK(chip.min_x > (row.min_x + row.max_x) / 2.0F);  // sağa yaslı
    CHECK(chip.max_x <= row.max_x);
    int rows = 0;
    int chips = 0;
    for (const auto& [name, rect] : rects) {
        rows += name.starts_with("palette:") ? 1 : 0;
        chips += name.starts_with("palette_keys:") ? 1 : 0;
    }
    CHECK(chips > 0);
    CHECK(chips < rows);  // kısayolsuz satırın sağı boş
}
