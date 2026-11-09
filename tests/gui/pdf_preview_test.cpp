// PDF önizlemesi (SPEC-gui "PDF önizleme"): incelemedeki PDF notu sayfa dokularıyla ve sayfa göstergesiyle
// çizilir; bozuk PDF yerinde yerelleştirilmiş hatayı gösterir. İşçi gerçek `note __pdf-worker`.
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/text_pdf.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <functional>
#include <string>
#include <thread>

using gui_test::make_view;
using gui_test::RenderedText;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

void select(GuiState& state, const std::string& path) {
    state.select_row(state.model().index_of(core::NotePath::parse(path).value()));
}

// İşçinin sonuçları post ile gelir: koşul doğru olana dek kare çizilir (en çok 20 s; sanitizer'lı derleme yavaş).
bool draw_until(View& view, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        view.draw_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace

TEST_CASE("gui pdf preview: a PDF note shows its pages as textures and a page indicator") {
    WindowFixture fixture;
    fixture.add_note("doc.pdf", noteapp::test::text_pdf({"One", "Two"}));
    fixture.add_note("plain.md", "# x\n", 10);
    const auto state = fixture.make_state();
    state->set_pdf_worker(NOTEAPP_NOTE_EXE);
    select(*state, "doc.pdf");
    CHECK(state->showing_pdf());
    CHECK(state->showing_preview());
    const auto view = make_view(*state);

    REQUIRE(draw_until(*view, [&] { return view->pdf_texture_count() == 2; }));
    const RenderedText text;
    view->draw_frame();
    CHECK(text.contains("1 / 2"));
    CHECK(view->item_rects().contains("pdf_page:1"));
    CHECK(view->item_rects().contains("pdf_page:2"));
    CHECK(view->item_rects().contains("pdf_indicator"));

    select(*state, "plain.md");  // başka not gösterilince oturum kapanır
    view->draw_frame();
    CHECK(state->pdf_preview() == nullptr);
}

TEST_CASE("gui pdf preview: a damaged PDF shows the localized reason in place of the pages") {
    WindowFixture fixture;
    fixture.add_note("bad.pdf", "%PDF-1.7\nnot really\n");
    const auto state = fixture.make_state();
    state->set_pdf_worker(NOTEAPP_NOTE_EXE);
    select(*state, "bad.pdf");
    const auto view = make_view(*state);
    REQUIRE(draw_until(*view, [&] {
        return state->pdf_preview() != nullptr &&
               state->pdf_preview()->state() == noteapp::app::PdfPreview::State::Failed;
    }));
    const RenderedText text;
    view->draw_frame();
    CHECK(text.contains(core::tr("error.pdf_invalid")));
    CHECK(view->pdf_texture_count() == 0);
}
