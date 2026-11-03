// T82: sağ paneldeki not bilgisi. Alanlar TUI'nin ayrıntısıyla aynı (boyut, güncelleme, oluşturma, senkron,
// kaynak) ve tip ile etiketler; model saf, panel gerçek çizim katmanıyla sınanır.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

std::string value_of(const GuiState& state, const char* label_key) {
    for (const auto& field : state.note_info()) {
        if (field.label == core::tr(label_key))
            return field.value;
    }
    return "<missing>";
}

}  // namespace

TEST_CASE("gui info: the shown note has the TUI's detail fields plus type and tags") {
    WindowFixture fixture;
    fixture.add_note("iş/plan.md", "#Proje metin\n", 10);
    const auto state = fixture.make_state();
    const int row = state->model().index_of(np("iş/plan.md"));
    state->select_row(row);
    const core::NoteInfo& info = state->model().note_at(row);
    REQUIRE(info.created_at.has_value());

    std::vector<std::string> labels;
    for (const auto& field : state->note_info())
        labels.push_back(field.label);
    CHECK(labels == std::vector<std::string>{core::tr("gui.info_size"), core::tr("gui.info_updated"),
                                             core::tr("gui.info_created"), core::tr("gui.info_sync"),
                                             core::tr("gui.info_origin"), core::tr("gui.info_type"),
                                             core::tr("gui.info_tags")});
    CHECK(value_of(*state, "gui.info_size") == core::human_size(info.size));
    CHECK(value_of(*state, "gui.info_updated") == core::local_time_text(info.mtime));
    CHECK(value_of(*state, "gui.info_created") == core::local_time_text(*info.created_at));
    CHECK(value_of(*state, "gui.info_sync") == core::tr("gui.sync_off"));  // senkron kapalı depo
    CHECK(value_of(*state, "gui.info_origin") == (info.origin ? *info.origin : core::tr("gui.info_unknown")));
    CHECK(value_of(*state, "gui.info_type") == "md");
    CHECK(value_of(*state, "gui.info_tags") == "#Proje");
}

TEST_CASE("gui info: a save refreshes the size, a draft and a folder have no fields") {
    WindowFixture fixture;
    fixture.add_note("a.md", "kısa\n", 10);
    const auto state = fixture.make_state();
    state->select_row(state->model().index_of(np("a.md")));
    const std::string before = value_of(*state, "gui.info_size");
    state->edit_text(std::string(4096, 'x'));
    state->save_now();
    CHECK(value_of(*state, "gui.info_size") == core::human_size(4096));
    CHECK(value_of(*state, "gui.info_size") != before);
    CHECK(value_of(*state, "gui.info_tags") == core::tr("gui.info_none"));

    state->select_folder("");
    CHECK(state->note_info().empty());
    state->new_note();
    CHECK(state->note_info().empty());
}

TEST_CASE("gui info: the panel is closed at first, Ctrl+I and its header open and close it") {
    WindowFixture fixture;
    fixture.add_note("a.md", "#etiket gövde\n", 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    state->select_row(state->model().index_of(np("a.md")));
    settle(*view);
    REQUIRE(view->item_rects().contains("info_header"));
    CHECK_FALSE(state->info_open());
    CHECK_FALSE(view->item_rects().contains("info_list"));

    {
        const RenderedText text;
        press(*view, ImGuiMod_Ctrl | ImGuiKey_I);
        settle(*view);
        CHECK(state->info_open());
        REQUIRE(view->item_rects().contains("info_list"));
        CHECK(text.contains(core::tr("gui.info_created")));
        CHECK(text.contains("#etiket"));
    }
    // Panel editörün altında, editörü örtmez.
    CHECK(view->item_rects().at("info_header").min_y >= view->item_rects().at("editor").max_y);

    click_button(*view, "info_header");
    settle(*view);
    CHECK_FALSE(state->info_open());
    CHECK_FALSE(view->item_rects().contains("info_list"));

    state->select_folder("");
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("info_header"));
}
