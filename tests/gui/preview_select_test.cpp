// V14 C: önizlemede fareyle metin seçimi (TUI önizleme seçimi gibi görüntülenen metin) ve kopyası: TUI gibi
// fare bırakılınca kendiliğinden, ayrıca Ctrl+C ile.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/text_selection.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <imgui.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

void select(GuiState& state, const std::string& path) {
    state.select_row(state.model().index_of(core::NotePath::parse(path).value()));
}

ImVec2 locate(const View& view, std::string_view needle, bool end = false) {
    const std::optional<ImVec2> point = view.preview_selection().locate(needle, end);
    REQUIRE_MESSAGE(point.has_value(), "not drawn in the preview: ", std::string(needle));
    return *point;
}

// Gerçek fare: bas, iki adımda sürükle, bırak; her olay ayrı karede.
void drag(View& view, ImVec2 from, ImVec2 to) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(from.x, from.y);
    view.draw_frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view.draw_frame();
    io.AddMousePosEvent((from.x + to.x) / 2.0F, (from.y + to.y) / 2.0F);
    view.draw_frame();
    io.AddMousePosEvent(to.x, to.y);
    view.draw_frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    view.draw_frame();
    view.draw_frame();
}

void double_click(View& view, ImVec2 at) {
    ImGuiIO& io = ImGui::GetIO();
    constexpr float kUnboundedDoubleClickSeconds = 3600.0F;  // gerçek zaman: ASan altında iki tık 0.3 sn'yi aşar
    io.MouseDoubleClickTime = kUnboundedDoubleClickSeconds;
    io.MouseSingleClickDelay = kUnboundedDoubleClickSeconds + 1.0F;
    io.AddMousePosEvent(at.x, at.y);
    view.draw_frame();
    for (int click = 0; click < 2; ++click) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        view.draw_frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        view.draw_frame();
    }
    view.draw_frame();
}

struct Scene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;
    std::vector<std::string> copied;
    std::vector<std::string> primary;

    Scene(const std::string& path, const std::string& content) {
        fixture.add_note(path, content, 10);
        fixture.add_note("b.md", "b\n", 20);
        state = fixture.make_state();
        state->deciders.copy_text = [this](const std::string& text) {
            copied.push_back(text);
            return true;
        };
        state->deciders.primary_selection = [this](const std::string& text) { primary.push_back(text); };
        view = make_view(*state);
        select(*state, path);
        settle(*view);
        REQUIRE(state->showing_preview());
    }

    // Ctrl+C önizleme odaktayken (fare tıkı önizlemeyi odaklar).
    void copy() {
        press(*view, ImGuiMod_Ctrl | ImGuiKey_C);
        settle(*view);
    }
};

}  // namespace

TEST_CASE("gui preview select: a drag across two markdown lines copies the displayed text on release") {
    Scene scene("a.md", "alfa **beta**\n\ngama `delta`\n");
    drag(*scene.view, locate(*scene.view, "beta"), locate(*scene.view, "delta", true));
    CHECK(scene.view->preview_selection().selected_text() == "beta\ngama delta");
    // Bırakınca bir kez (sürüklemenin her karesinde değil); görüntülenen metin (işaretsiz), notun baytları
    // değil; TUI gibi kopya iletisi; orta tık için birincil seçim de.
    REQUIRE(scene.copied == std::vector<std::string>{"beta\ngama delta"});
    CHECK(scene.state->status() == core::tr("gui.copied", {{"size", core::human_size(15)}}));
    CHECK(scene.primary == std::vector<std::string>{"beta\ngama delta"});
    // Ctrl+C seçimi yine kopyalar.
    scene.copy();
    CHECK(scene.copied == std::vector<std::string>{"beta\ngama delta", "beta\ngama delta"});
}

TEST_CASE("gui preview select: Ctrl+C without a selection still copies the note bytes") {
    Scene scene("a.md", "alfa **beta**\n");
    const ImVec2 at = locate(*scene.view, "beta");
    drag(*scene.view, at, at);  // tık: önizleme odaklanır, seçim boş kalır
    CHECK(scene.view->preview_selection().empty());
    CHECK(scene.copied.empty());  // seçimsiz tık kopyalamaz
    scene.copy();
    CHECK(scene.copied == std::vector<std::string>{"alfa **beta**\n"});
}

TEST_CASE("gui preview select: a double-click selects and copies a word and does not start editing") {
    Scene scene("a.md", "alfa beta-gama\n");
    const ImVec2 begin = locate(*scene.view, "gama");
    const ImVec2 end = locate(*scene.view, "gama", true);
    double_click(*scene.view, ImVec2((begin.x + end.x) / 2.0F, begin.y));
    CHECK(scene.view->preview_selection().selected_text() == "gama");
    CHECK(scene.copied == std::vector<std::string>{"gama"});
    CHECK(scene.state->showing_preview());
}

TEST_CASE("gui preview select: json lines copy as displayed") {
    Scene scene("d.json", R"({"ad":"değer","sayı":42})");
    drag(*scene.view, locate(*scene.view, R"("ad")"), locate(*scene.view, "42", true));
    CHECK(scene.copied == std::vector<std::string>{"\"ad\": \"değer\",\n\"sayı\": 42"});
}

TEST_CASE("gui preview select: csv cells copy row by row") {
    Scene scene("t.csv", "ad,şehir\nAli,İzmir\nVeli,Bursa\n");
    drag(*scene.view, locate(*scene.view, "Ali"), locate(*scene.view, "Bursa", true));
    CHECK(scene.copied == std::vector<std::string>{"Ali İzmir\nVeli Bursa"});
}

TEST_CASE("gui preview select: a link click opens it, a drag from the link selects instead") {
    Scene scene("a.md", "[[b]] ve metin\n");
    drag(*scene.view, locate(*scene.view, "b"), locate(*scene.view, "metin", true));
    CHECK(scene.view->preview_selection().selected_text() == "b ve metin");
    REQUIRE(scene.state->current_path().has_value());
    CHECK(scene.state->current_path()->string() == "a.md");  // sürükleme bağlantıyı açmadı
    // Bağlantının içinde biten sürükleme de açmaz (düğme bırakılınca "tıklandı" der).
    drag(*scene.view, locate(*scene.view, "b"), locate(*scene.view, "b", true));
    CHECK(scene.view->preview_selection().selected_text() == "b");
    CHECK(scene.state->current_path()->string() == "a.md");

    gui_test::click_button(*scene.view, "preview_link:b");
    settle(*scene.view);
    REQUIRE(scene.state->current_path().has_value());
    CHECK(scene.state->current_path()->string() == "b.md");
}

TEST_CASE("gui preview select: Ctrl+C in the frame the drag is released copies once") {
    Scene scene("a.md", "alfa **beta**\n\ngama `delta`\n");
    const ImVec2 from = locate(*scene.view, "beta");
    const ImVec2 to = locate(*scene.view, "delta", true);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = false;  // tuş ve bırakma aynı karede işlensin
    io.AddMousePosEvent(from.x, from.y);
    scene.view->draw_frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    scene.view->draw_frame();
    io.AddMousePosEvent(to.x, to.y);
    scene.view->draw_frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    scene.view->draw_frame();
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    settle(*scene.view);
    io.ConfigInputTrickleEventQueue = true;
    CHECK(scene.copied == std::vector<std::string>{"beta\ngama delta"});
}

// Önizlemede başlayan tık bırakılmadan editöre geçilirse (çift tık, kısayol) dönüşte kopyalamaz: bırakma
// önizlemede olmadı.
TEST_CASE("gui preview select: a press released while the editor is shown does not copy on return") {
    Scene scene("a.md", "alfa beta gama\n");
    drag(*scene.view, locate(*scene.view, "alfa"), locate(*scene.view, "beta", true));
    REQUIRE(scene.copied == std::vector<std::string>{"alfa beta"});
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 at = locate(*scene.view, "gama", true);
    io.AddKeyEvent(ImGuiMod_Shift, true);  // Shift+tık seçimi basınca genişletir
    io.AddMousePosEvent(at.x, at.y);
    scene.view->draw_frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    scene.view->draw_frame();
    REQUIRE_FALSE(scene.view->preview_selection().empty());
    scene.state->begin_edit();
    scene.view->draw_frame();
    REQUIRE_FALSE(scene.state->showing_preview());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    scene.view->draw_frame();
    io.AddKeyEvent(ImGuiMod_Shift, false);
    scene.view->draw_frame();
    scene.state->end_edit();
    settle(*scene.view);
    REQUIRE(scene.state->showing_preview());
    CHECK(scene.copied == std::vector<std::string>{"alfa beta"});
}
