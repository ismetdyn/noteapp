// PDF önizlemesi (SPEC-gui "PDF önizleme"): incelemedeki PDF notu sayfa dokularıyla ve sayfa göstergesiyle
// çizilir; bozuk PDF yerinde yerelleştirilmiş hatayı gösterir. İşçi gerçek `note __pdf-worker`.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/text_pdf.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <functional>
#include <imgui.h>
#include <string>
#include <thread>
#include <vector>

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

TEST_CASE("gui pdf preview: dragging over a page selects its text and Ctrl+C copies the selection") {
    WindowFixture fixture;
    // İkili yorum satırı aynı uzunlukta ASCII olur (xref kaymaz): not geçerli UTF-8'dir, notu kopyalayan komut
    // çalışsaydı panoya PDF'in kendisini yazardı.
    std::string pdf = noteapp::test::text_pdf({"One", "Two"});
    pdf.replace(pdf.find("%\xE2\xE3\xCF\xD3"), 5, "%abcd");
    fixture.add_note("doc.pdf", pdf);
    const auto state = fixture.make_state();
    state->set_pdf_worker(NOTEAPP_NOTE_EXE);
    select(*state, "doc.pdf");
    const auto view = make_view(*state);
    std::vector<std::string> copied;
    state->deciders.copy_text = [&](const std::string& text) {
        copied.push_back(text);
        return true;
    };
    REQUIRE(draw_until(*view, [&] { return view->item_rects().contains("pdf_page:1"); }));
    // Sayfa 200x100 pt; "One" 24 pt Helvetica, taban çizgisi alttan 40 pt, x = 20 pt'den başlar.
    const auto at = [&](float x, float y) {
        const View::ItemRect& page = view->item_rects().at("pdf_page:1");
        return ImVec2(page.min_x + (x * (page.max_x - page.min_x) / 200.0F),
                      page.min_y + (y * (page.max_y - page.min_y) / 100.0F));
    };
    ImGuiIO& io = ImGui::GetIO();
    // Metin katmanı dokudan sonra gelir: seçim tutana dek sürükleme yinelenir.
    REQUIRE(draw_until(*view, [&] {
        io.AddMousePosEvent(at(5.0F, 52.0F).x, at(5.0F, 52.0F).y);
        view->draw_frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        view->draw_frame();
        io.AddMousePosEvent(at(180.0F, 52.0F).x, at(180.0F, 52.0F).y);
        view->draw_frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        view->draw_frame();
        return view->item_rects().contains("pdf_selection");
    }));
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    view->draw_frame();
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    view->draw_frame();
    // Yalnız seçim kopyalanır: notu kopyalayan önizleme komutu aynı basışta çalışmaz (panoya tek yazma).
    CHECK(copied == std::vector<std::string>{"One"});
    CHECK(state->status() == core::tr("gui.copied", {{"size", core::human_size(3)}}));
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
