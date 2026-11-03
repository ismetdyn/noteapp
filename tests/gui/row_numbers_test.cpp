// T82: Ağaç ve Ayrıntılar'da sıra no sütunu (TUI'deki satır numarası). Numara TUI kuralıyla klasör başına 1'den
// başlar; ayar Ayarlar'dan açılır, pencere yerleşimiyle (gui-state.toml) saklanır.
#include "gui/browser_rows.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <utility>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::gui::BrowserRow;
using noteapp::gui::BrowserView;

namespace {

using Numbers = std::vector<std::pair<std::string, std::size_t>>;

Numbers numbers(const std::vector<BrowserRow>& rows) {
    Numbers out;
    for (const BrowserRow& row : rows)
        out.emplace_back(row.path, row.number);
    return out;
}

}  // namespace

TEST_CASE("gui row numbers: numbers restart in every folder like the TUI, the root has none") {
    WindowFixture fixture;
    fixture.add_note("b.md", "b\n", 10);
    fixture.add_note("a.md", "a\n", 20);
    fixture.add_note("iş/c.md", "c\n", 30);
    fixture.add_note("iş/d.md", "d\n", 40);
    const auto state = fixture.make_state();
    state->set_expanded("iş", true);
    // Varsayılan sıra en yeni başta (karar 6): kökte b, a.
    CHECK(numbers(state->browser_rows()) ==
          Numbers{{"", 0}, {"iş", 1}, {"iş/c.md", 1}, {"iş/d.md", 2}, {"b.md", 2}, {"a.md", 3}});
    state->set_browser_view(BrowserView::Details);
    state->enter_folder("iş");
    CHECK(numbers(state->browser_rows()) == Numbers{{"iş/c.md", 1}, {"iş/d.md", 2}});
}

TEST_CASE("gui row numbers: the setting is off at first and kept in the state file") {
    WindowFixture fixture;
    fixture.add_note("a.md", "a\n");
    {
        const auto state = fixture.make_state();
        CHECK_FALSE(state->row_numbers());
        state->set_row_numbers(true);
        CHECK(state->row_numbers());
    }
    const auto reopened = fixture.make_state();
    CHECK(reopened->row_numbers());
}

TEST_CASE("gui row numbers: the Settings checkbox shows the column in Tree and Details") {
    WindowFixture fixture;
    fixture.add_note("a.md", "a\n", 10);
    fixture.add_note("iş/c.md", "c\n", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("row_number:note:a.md"));

    state->show_settings_tab();
    settle(*view);
    // Görünüm grubu formun sonunda: tekerlekle oraya inilir (gerçek fare yolu).
    const auto [x, y] = view->button_center("settings_form");
    ImGui::GetIO().AddMousePosEvent(x, y);
    view->draw_frame();
    constexpr float kWheelNotches = -100.0F;
    ImGui::GetIO().AddMouseWheelEvent(0.0F, kWheelNotches);
    settle(*view);
    REQUIRE(view->item_rects().contains("setting:row_numbers"));
    click_button(*view, "setting:row_numbers");
    CHECK(state->row_numbers());
    CHECK_FALSE(state->settings().has_changes());  // Kaydet gerekmez: form değişikliği sayılmaz

    state->focus_search();
    settle(*view);
    {
        const auto& rects = view->item_rects();
        REQUIRE(rects.contains("row_number:note:a.md"));
        REQUIRE(rects.contains("row_number:folder:iş"));
        // Numara satırın içinde, adın solunda; kökün numarası yok.
        CHECK(rects.at("row_number:note:a.md").min_x >= rects.at("note:a.md").min_x);
        CHECK_FALSE(rects.contains("row_number:folder:"));
    }

    state->set_browser_view(BrowserView::Details);
    state->enter_folder("");
    settle(*view);
    const auto& rects = view->item_rects();
    REQUIRE(rects.contains("details_number:a.md"));
    REQUIRE(rects.contains("details_header:number"));
    CHECK(rects.at("details_number:a.md").max_x <= rects.at("details_header:0").min_x + 1.0F);
}
