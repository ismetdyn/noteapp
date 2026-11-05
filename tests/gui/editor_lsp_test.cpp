// G5c: GUI editöründe dil sunucusu (TUI ile aynı tui::LanguageServers): tanı alt çizgisi ve imleçteki ileti,
// önerilerin Ctrl+Space listesine katılması, durum iletileri. Gerçek sunucu yerine tests/tui/fake_lsp_server.
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/note_editor.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <imgui.h>
#include <string>
#include <thread>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::NoteEditor;
using noteapp::gui::View;
namespace fs = std::filesystem;

namespace {

constexpr auto kArrival = std::chrono::seconds(10);

struct LspFixture : WindowFixture {
    explicit LspFixture(const char* mode = "normal") {
        config.lsp.enabled = true;
        config.lsp.servers.clear();
        config.lsp.servers["python"] = {{fs::absolute(NOTEAPP_FAKE_LSP_SERVER).string(), mode}};
    }
};

struct CrashFixture : LspFixture {
    CrashFixture() : LspFixture("crash") {}
};

// Kareler sunucu cevabı gelene dek çizilir (cevap işçiden post edilir, tick UI iş parçacığına alır).
template <class Predicate> bool pump_until(GuiState& state, View& view, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + kArrival;
    while (std::chrono::steady_clock::now() < deadline) {
        state.tick(std::chrono::steady_clock::now());
        view.draw_frame();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE_FIXTURE(LspFixture, "editor: language server diagnostics are underlined, the message shows at the cursor") {
    add_note("pkg/a.py", "import os\nx = 1\n");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    REQUIRE(pump_until(*state, *view, [&] { return view->item_rects().contains("editor_diagnostic:1:0"); }));
    CHECK_FALSE(view->item_rects().contains("editor_diagnostic:2:0"));
    {
        const gui_test::RenderedText rendered;
        settle(*view);
        CHECK(rendered.contains("count=1 text=import os"));  // imleç tanının üstünde
    }
    press(*view, ImGuiKey_DownArrow);
    const gui_test::RenderedText rendered;
    settle(*view);
    CHECK_FALSE(rendered.contains("count=1 text=import os"));
}

// Editörün imleci yanıp sönmez, boşta kare yoktur: değişiklik yine süresi dolunca kendi karesiyle gider.
TEST_CASE_FIXTURE(LspFixture, "editor: an edit reaches the language server while the window only idles") {
    add_note("pkg/e.py", "x = 1\n");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    REQUIRE(pump_until(*state, *view, [&] { return !state->editor().diagnostics().empty(); }));
    type_text(*view, "y");
    REQUIRE(state->buffer() == "yx = 1\n");
    const auto frames = view->frames();
    const auto sent = [&] {
        const auto marks = state->editor().diagnostics();
        return !marks.empty() && marks.front().message.find("text=yx = 1") != std::string::npos;
    };
    const auto deadline = std::chrono::steady_clock::now() + kArrival;
    while (!sent() && std::chrono::steady_clock::now() < deadline)
        view->idle_step(1);  // yalnız olay döngüsü: draw_frame doğrudan çağrılmaz
    CHECK(sent());
    // Gönderim karesi + sunucu cevabının kareleri; boşta sık uyanma ya da imleç karesi yok.
    constexpr std::size_t kMaxFrames = 10;
    MESSAGE("frames until the diagnostic of the edit: ", view->frames() - frames);
    CHECK(view->frames() - frames <= kMaxFrames);
}

TEST_CASE_FIXTURE(LspFixture, "editor: server completion items join the Ctrl+Space list and are inserted") {
    add_note("pkg/b.py", "al\n");
    auto state = make_state();
    auto view = make_view(*state);
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    press(*view, ImGuiKey_End);
    // Belge sunucuda açık olsun (tanı geldi), sonra liste istenir.
    REQUIRE(pump_until(*state, *view, [&] { return !state->editor().diagnostics().empty(); }));
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Space);
    REQUIRE(state->editor().mode() == NoteEditor::Mode::Complete);
    REQUIRE(pump_until(*state, *view, [&] {
        const auto& items = state->editor().completion_items();
        return std::ranges::find(items, "alpha") != items.end();
    }));
    while (state->editor().completion_items()[state->editor().completion_selected()] != "alpha")
        press(*view, ImGuiKey_DownArrow);
    press(*view, ImGuiKey_Enter);
    CHECK(state->buffer() == "alpha()\n");  // sunucunun tek satırlık ekleme metni (TUI kuralı)
}

TEST_CASE_FIXTURE(LspFixture, "editor: a draft never reaches the language server") {
    add_note("pkg/c.py", "import os\n");
    auto state = make_state();
    auto view = make_view(*state);
    REQUIRE(pump_until(*state, *view, [&] { return !state->editor().diagnostics().empty(); }));
    press(*view, ImGuiMod_Ctrl | ImGuiKey_N);
    REQUIRE(state->is_new_note());
    type_text(*view, "import os");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < until) {
        state->tick(std::chrono::steady_clock::now());
        view->draw_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(state->editor().diagnostics().empty());
}

TEST_CASE_FIXTURE(CrashFixture, "editor: a crashed language server is reported once in the editor status") {
    add_note("pkg/d.py", "import os\n");
    auto state = make_state();
    auto view = make_view(*state);
    REQUIRE(pump_until(*state, *view, [&] { return !state->editor_notice().empty(); }));
    CHECK(state->editor_notice().find(noteapp::core::tr("tui.lsp_exited")) != std::string::npos);
    {
        const gui_test::RenderedText rendered;
        settle(*view);
        CHECK(rendered.contains(state->editor_notice()));
    }
    state->request_focus(FocusRequest::Editor);
    settle(*view);
    press(*view, ImGuiKey_RightArrow);  // sonraki tuş iletiyi siler (TUI)
    CHECK(state->editor_notice().empty());
}
