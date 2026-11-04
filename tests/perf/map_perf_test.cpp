// T84: not haritasının kare ve yerleşim süresi. T93: paralel `gui` ctest'inden buraya taşındı; perf paketi
// RUN_SERIAL koşar, başka testin yükü ölçüme girmez. Assert'ler T84'tekiyle aynıdır.
#include "core/note_store.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "gui/window_fixture.hpp"
#include "support/note_seed.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <numeric>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::BrowserView;
using noteapp::gui::MapOptions;

namespace core = noteapp::core;

namespace {

constexpr int kPerfNotes = 2001;  // sınırın bir üstü: uyarı görünsün, 2.000 düğüm çizilsin
constexpr double kFrameBudgetMs = 1000.0 / 30.0;

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

std::string perf_path(int index) {
    return "k" + std::to_string(index % 20) + "/not-" + std::to_string(index) + ".md";
}

// Not başına iki bağlantı, her ikinci notta bir üçüncü: 2.000 çizilen notta ~5.000 kenar.
void seed_perf_corpus(const WindowFixture& fixture) {
    std::vector<core::ImportedNote> notes;
    for (int i = 0; i < kPerfNotes; ++i) {
        std::string body =
            "[[" + perf_path((i + 1) % kPerfNotes) + "]] [[" + perf_path((i + 13) % kPerfNotes) + "]]";
        if (i % 2 == 0)
            body += " [[" + perf_path((i + 97) % kPerfNotes) + "]]";
        notes.push_back(core::ImportedNote{np(perf_path(i)), body + "\n"});
    }
    noteapp::test::bulk_seed(fixture.store_dir(), notes);
    core::NoteStore store(fixture.store_dir());
    REQUIRE(store.relink_notes().has_value());
}

}  // namespace

TEST_CASE("map perf: 2,000 nodes and 5,000 edges draw at 30 fps or more, with the cap notice") {
    WindowFixture fixture;
    seed_perf_corpus(fixture);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    state->set_browser_view(BrowserView::Map);
    state->set_map_options(MapOptions{.whole_graph = true});
    view->draw_frame();
    const auto& map = state->note_map();
    CHECK(map.nodes.size() == fixture.config.limits.app.map_nodes.cap());
    CHECK(map.total == static_cast<std::size_t>(kPerfNotes));
    CHECK(map.edges.size() >= 4900);
    CHECK(view->item_rects().contains("map_capped"));

    using Clock = std::chrono::steady_clock;
    constexpr int kFrames = 30;
    const auto measure = [&] {
        std::vector<double> samples;
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto start = Clock::now();
            view->draw_frame();
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        return std::accumulate(samples.begin(), samples.end(), 0.0) / kFrames;
    };
    // Yerleşim sürerken (adım + çizim) ve durduktan sonra (yalnız çizim).
    const double moving = measure();
    REQUIRE_FALSE(state->map_layout().settled());
    constexpr int kMaxFrames = 2000;
    for (int frame = 0; frame < kMaxFrames && !state->map_layout().settled(); ++frame)
        view->draw_frame();
    REQUIRE(state->map_layout().settled());
    const double still = measure();
    MESSAGE("map frame ms: layout running ", moving, ", settled ", still, " (", map.nodes.size(), " nodes, ",
            map.edges.size(), " edges)");
    CHECK(moving <= kFrameBudgetMs);
    CHECK(still <= kFrameBudgetMs);
}

TEST_CASE("map perf: one layout frame on 2,000 nodes and 5,000 edges stays within its budget") {
    WindowFixture fixture;
    seed_perf_corpus(fixture);
    const auto state = fixture.make_state();
    state->set_browser_view(BrowserView::Map);
    state->set_map_options(MapOptions{.whole_graph = true});
    auto& layout = state->map_layout();
    using Clock = std::chrono::steady_clock;
    constexpr int kFrames = 20;
    double worst = 0.0;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto start = Clock::now();
        layout.step(View::kMapIterationsPerFrame);
        worst = std::max(worst, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    MESSAGE("layout ms per frame (", View::kMapIterationsPerFrame, " iterations), worst of ", kFrames, ": ",
            worst);
    CHECK(worst <= View::kMapLayoutBudgetMs);
}
