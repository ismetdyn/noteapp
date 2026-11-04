// T85: editör sekmeleri. Model saf GuiState ile; kısayollar, sekme çubuğu ve fare gerçek çizim katmanıyla
// (gerçek ImGui olayları) sınanır.
#include "core/paths.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::CloseChoice;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

// Sekmelerin yolları sırayla; taslak "".
std::vector<std::string> tab_paths(const GuiState& state) {
    std::vector<std::string> paths;
    for (const auto& tab : state.tabs())
        paths.push_back(tab.path);
    return paths;
}

std::uint64_t tab_id(const GuiState& state, const std::string& path) {
    for (const auto& tab : state.tabs()) {
        if (tab.path == path)
            return tab.id;
    }
    FAIL("no tab for " << path);
    return 0;
}

std::string active_path(const GuiState& state) {
    for (const auto& tab : state.tabs()) {
        if (tab.id == state.active_tab())
            return tab.path;
    }
    return "<none>";
}

// c.md en yeni: açılışta o gösterilir. a.md b'ye bağlanır (bağlantı paneli ve harita sekmeyi izler).
struct TabsScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    int save_dialogs = 0;
    int close_questions = 0;
    CloseChoice close_choice = CloseChoice::Cancel;

    TabsScene() {
        fixture.add_note("b.md", "beta\n", 20);  // hedef kaynaktan önce: kenar yazmada kurulur
        fixture.add_note("a.md", "[[b]] alfa\n", 30);
        fixture.add_note("c.md", "gama\n", 10);
        start();
    }
    void start() {
        state = fixture.make_state();
        state->deciders.save_dialog = [this](const noteapp::gui::SaveDialogRequest&) {
            ++save_dialogs;
            return std::optional<noteapp::gui::SaveDetails>{};
        };
        state->deciders.close_unsaved = [this] {
            ++close_questions;
            return close_choice;
        };
    }
    void open(const std::string& path) const { state->open_note(np(path)); }
};

void click_with(View& view, const std::string& name, ImGuiMouseButton button, bool ctrl = false) {
    const auto [x, y] = view.button_center(name);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    view.draw_frame();
    if (ctrl)
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddMouseButtonEvent(button, true);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(button, false);
    view.draw_frame();
    if (ctrl)
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    settle(view);
}

}  // namespace

TEST_CASE("tabs: opening a note replaces the active tab; open in new tab adds one after the others") {
    TabsScene scene;
    REQUIRE(tab_paths(*scene.state) == std::vector<std::string>{"c.md"});
    scene.open("a.md");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md"});
    scene.state->open_in_new_tab(np("b.md"));
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(active_path(*scene.state) == "b.md");
    CHECK(scene.state->buffer() == "beta\n");
    CHECK(scene.state->current_path() == np("b.md"));
}

TEST_CASE("tabs: an edited or pinned tab is kept, the next note opens in a new tab") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->edit_text("alfa 2\n");
    scene.open("b.md");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(scene.fixture.read_note("a.md") == "alfa 2\n");  // ayrılırken kendi notuna yazıldı
    CHECK(scene.fixture.read_note("b.md") == "beta\n");

    scene.state->set_tab_pinned(tab_id(*scene.state, "b.md"), true);
    scene.open("c.md");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md", "c.md"});
    CHECK(scene.state->tabs()[1].pinned);
}

TEST_CASE("tabs: a note already open in a tab is focused, never opened twice") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    scene.open("a.md");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(active_path(*scene.state) == "a.md");
    CHECK(scene.state->buffer() == "[[b]] alfa\n");
    scene.state->open_in_new_tab(np("b.md"));
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(active_path(*scene.state) == "b.md");
    scene.state->select_row(scene.state->model().index_of(np("a.md")));  // ağaç/liste seçimi de aynı yol
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(active_path(*scene.state) == "a.md");
}

TEST_CASE("tabs: switching back and forth writes each pending edit only to its own note") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    const auto a = tab_id(*scene.state, "a.md");
    const auto b = tab_id(*scene.state, "b.md");

    REQUIRE(scene.state->activate_tab(a));
    scene.state->edit_text("A1\n");
    REQUIRE(scene.state->activate_tab(b));
    CHECK(scene.fixture.read_note("a.md") == "A1\n");
    CHECK(scene.fixture.read_note("b.md") == "beta\n");
    CHECK(scene.state->buffer() == "beta\n");
    scene.state->edit_text("B1\n");
    REQUIRE(scene.state->activate_tab(a));
    CHECK(scene.fixture.read_note("a.md") == "A1\n");
    CHECK(scene.fixture.read_note("b.md") == "B1\n");
    CHECK(scene.state->buffer() == "A1\n");
    scene.state->edit_text("A2\n");
    REQUIRE(scene.state->activate_tab(b));
    scene.state->edit_text("B2\n");
    REQUIRE(scene.state->activate_tab(a));
    CHECK(scene.fixture.read_note("a.md") == "A2\n");
    CHECK(scene.fixture.read_note("b.md") == "B2\n");
    CHECK(scene.state->buffer() == "A2\n");
    CHECK_FALSE(scene.state->dirty());
    REQUIRE(scene.state->activate_tab(b));
    CHECK(scene.state->buffer() == "B2\n");
    CHECK(scene.save_dialogs == 0);
}

TEST_CASE("tabs: a draft stays unsaved in its own tab while another tab is shown") {
    TabsScene scene;
    const auto first = scene.state->active_tab();
    scene.state->new_tab();
    REQUIRE(scene.state->tabs().size() == 2);
    REQUIRE(scene.state->is_new_note());
    scene.state->edit_text("taslak\n");
    const auto draft = scene.state->active_tab();
    REQUIRE(scene.state->activate_tab(first));
    CHECK(scene.save_dialogs == 0);
    CHECK(scene.state->buffer() == "gama\n");
    CHECK(scene.state->tabs()[1].unsaved);
    CHECK(scene.fixture.read_note("c.md") == "gama\n");
    REQUIRE(scene.state->activate_tab(draft));
    CHECK(scene.state->is_new_note());
    CHECK(scene.state->buffer() == "taslak\n");
}

TEST_CASE("tabs: closing a tab with unsaved work follows the unsaved-changes flow") {
    TabsScene scene;
    scene.state->new_tab();
    scene.state->edit_text("taslak\n");
    scene.close_choice = CloseChoice::Cancel;
    scene.state->close_active_tab();
    CHECK(scene.close_questions == 1);
    CHECK(scene.state->tabs().size() == 2);
    CHECK(scene.state->buffer() == "taslak\n");

    // Arka plandaki taslak: soru onun metniyle, onu öne getirerek sorulur.
    const auto draft = scene.state->active_tab();
    REQUIRE(scene.state->activate_tab(scene.state->tabs()[0].id));
    scene.state->close_tab(draft);
    CHECK(scene.close_questions == 2);
    CHECK(scene.state->active_tab() == draft);
    scene.close_choice = CloseChoice::Discard;
    scene.state->close_tab(draft);
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"c.md"});
    CHECK(scene.state->buffer() == "gama\n");

    // Kirli kayıtlı not: kapanırken kendi notuna yazılır, soru sorulmaz.
    REQUIRE(scene.close_questions == 3);
    scene.state->edit_text("gama 2\n");
    scene.state->close_active_tab();
    CHECK(scene.close_questions == 3);
    CHECK(scene.fixture.read_note("c.md") == "gama 2\n");
    CHECK(scene.state->tabs().empty());
    CHECK_FALSE(scene.state->current_path());
    scene.state->rescan();  // son sekme kapandı: süzgeç kendiliğinden not açmaz
    CHECK(scene.state->tabs().empty());
}

TEST_CASE("tabs: a background tab follows a sync change and closes when its note is deleted or renamed") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    scene.state->open_in_new_tab(np("c.md"));
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));

    scene.fixture.add_note("b.md", "beta uzak\n");
    scene.state->rescan();
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "b.md")));
    CHECK(scene.state->buffer() == "beta uzak\n");

    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));
    scene.fixture.remove_note("b.md");
    scene.state->rescan();
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md", "c.md"});
    CHECK(scene.state->status().find("b.md") != std::string::npos);
    CHECK(scene.state->buffer() == "[[b]] alfa\n");

    // Senkronda yeniden adlandırma: eski yol gider, yeni yol gelir.
    scene.fixture.remove_note("c.md");
    scene.fixture.add_note("d.md", "gama\n");
    scene.state->rescan();
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md"});
    CHECK(scene.state->status().find("c.md") != std::string::npos);

    // Gösterilen notun kendisi silinirse de sekmesi kapanır, komşu sekme öne gelir.
    scene.state->open_in_new_tab(np("d.md"));
    scene.fixture.remove_note("d.md");
    scene.state->rescan();
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md"});
    CHECK(scene.state->status().find("d.md") != std::string::npos);
    CHECK(scene.state->buffer() == "[[b]] alfa\n");
}

TEST_CASE("tabs: renaming a folder updates the path of a background tab") {
    TabsScene scene;
    scene.fixture.add_note("proje/plan.md", "plan\n", 40);
    scene.state->rescan();
    scene.open("proje/plan.md");
    scene.state->open_in_new_tab(np("a.md"));
    scene.state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("iş");
    };
    scene.state->rename_folder("proje");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"iş/plan.md", "a.md"});
    REQUIRE(scene.state->activate_tab(scene.state->tabs()[0].id));
    CHECK(scene.state->buffer() == "plan\n");
}

TEST_CASE("tabs: trashing the shown note closes its tab") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    REQUIRE(scene.state->current_path() == np("b.md"));
    scene.state->trash_note();
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"a.md"});
    CHECK(scene.state->buffer() == "[[b]] alfa\n");
    CHECK(scene.state->can_undo_trash());  // çöp iletisi ezilmedi
}

TEST_CASE(
    "tabs: open tabs, their order, pins and the active tab survive a restart; missing notes are skipped") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    scene.state->open_in_new_tab(np("c.md"));
    scene.state->set_tab_pinned(tab_id(*scene.state, "b.md"), true);
    scene.state->reorder_tabs(
        {tab_id(*scene.state, "c.md"), tab_id(*scene.state, "b.md"), tab_id(*scene.state, "a.md")});
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "b.md")));
    REQUIRE(scene.state->request_close());
    const auto state_file = core::state_dir() / "gui-state.toml";
    REQUIRE(std::filesystem::exists(state_file));
    CHECK(state_file.string().starts_with(scene.fixture.home.path().string()));  // geçici ev dizini

    scene.fixture.remove_note("c.md");
    scene.state.reset();
    scene.start();
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"b.md", "a.md"});
    CHECK(active_path(*scene.state) == "b.md");
    CHECK(scene.state->buffer() == "beta\n");
    CHECK(scene.state->current_path() == np("b.md"));
    CHECK(scene.state->tabs()[0].pinned);
    CHECK_FALSE(scene.state->tabs()[1].pinned);
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));
    CHECK(scene.state->buffer() == "[[b]] alfa\n");
}

TEST_CASE("tabs: the number of open tabs is capped by limits.app.tabs with a status message") {
    TabsScene scene;
    constexpr std::size_t kCap = 5;
    constexpr std::size_t kExtra = 4;
    scene.fixture.config.limits.app.tabs = noteapp::core::Limit{kCap, std::to_string(kCap)};
    scene.start();
    for (std::size_t i = 0; i < kCap + kExtra; ++i)
        scene.fixture.add_note(std::format("n{:02}.md", i), "x\n", 50);
    scene.state->rescan();
    for (std::size_t i = 0; i < kCap + kExtra; ++i)
        scene.state->open_in_new_tab(np(std::format("n{:02}.md", i)));
    CHECK(scene.state->tabs().size() == kCap);
    CHECK(scene.state->status().find(std::to_string(kCap)) != std::string::npos);
    scene.state->new_tab();
    CHECK(scene.state->tabs().size() == kCap);
    CHECK_FALSE(scene.state->is_new_note());
}

// Sabit tavan yok (kullanıcı kuralı): varsayılan 0, eski 32 sekme sınırı kalktı.
TEST_CASE("tabs: with the default limit more tabs open than the old fixed cap") {
    TabsScene scene;
    constexpr std::size_t kMany = 40;
    for (std::size_t i = 0; i < kMany; ++i)
        scene.fixture.add_note(std::format("n{:02}.md", i), "x\n", 50);
    scene.state->rescan();
    for (std::size_t i = 0; i < kMany; ++i)
        scene.state->open_in_new_tab(np(std::format("n{:02}.md", i)));
    CHECK(scene.state->tabs().size() >= kMany);  // açılışta gösterilen notun sekmesi de durur
}

TEST_CASE("tabs: each tab keeps its own preview toggle; links, info and the map follow the active tab") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->toggle_preview();
    REQUIRE(scene.state->showing_preview());
    scene.state->open_in_new_tab(np("c.md"));
    CHECK_FALSE(scene.state->showing_preview());
    CHECK(scene.state->links().outgoing.empty());
    CHECK(scene.state->note_map().nodes.size() == 1);
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));
    CHECK(scene.state->showing_preview());
    REQUIRE(scene.state->links().outgoing.size() == 1);
    CHECK(scene.state->links().outgoing[0] == np("b.md"));
    CHECK(scene.state->note_map().nodes.size() == 2);
    CHECK_FALSE(scene.state->note_info().empty());
}

TEST_CASE("tabs: the hidden-character warning follows the active tab (T91)") {
    TabsScene scene;
    scene.fixture.add_note("gizli.md",
                           "a\xE2\x80\xAE"
                           "b\n",
                           40);  // U+202E
    scene.state->rescan();
    scene.open("gizli.md");
    REQUIRE(scene.state->hidden_controls() == 1);
    scene.state->open_in_new_tab(np("c.md"));
    CHECK(scene.state->hidden_controls() == 0);
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "gizli.md")));
    CHECK(scene.state->hidden_controls() == 1);
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "c.md")));
    CHECK(scene.state->hidden_controls() == 0);
}

// İmleç ve seçim sekmenin EditorBuffer'ındadır: sekme dönünce aynen gelir.
TEST_CASE("tabs: returning to a tab puts the editor cursor back where it was") {
    TabsScene scene;
    scene.open("a.md");
    for (int step = 0; step < 3; ++step)
        scene.state->editor_key(noteapp::tui::Key::Right);
    scene.state->editor_key(noteapp::tui::Key::ShiftRight);
    scene.state->editor_key(noteapp::tui::Key::ShiftRight);
    scene.state->open_in_new_tab(np("c.md"));
    scene.state->editor_key(noteapp::tui::Key::Right);
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));
    REQUIRE(scene.state->editor_selection());
    CHECK(scene.state->editor_selection()->start == 3);
    CHECK(scene.state->editor_selection()->end == 5);
}

TEST_CASE("tabs: Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+PageUp/PageDown") {
    TabsScene scene;
    auto view = make_view(*scene.state);
    scene.state->request_focus(noteapp::gui::FocusRequest::Editor);
    settle(*view);
    REQUIRE(view->editor_active());
    const auto first = scene.state->active_tab();

    press(*view, ImGuiMod_Ctrl | ImGuiKey_T);
    REQUIRE(scene.state->tabs().size() == 2);
    CHECK(scene.state->is_new_note());
    const auto draft = scene.state->active_tab();

    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab);
    CHECK(scene.state->active_tab() == first);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Tab);
    CHECK(scene.state->active_tab() == draft);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_PageUp);
    CHECK(scene.state->active_tab() == first);
    CHECK(scene.state->buffer() == "gama\n");  // Ctrl+Tab editöre sekme karakteri yazmadı
    press(*view, ImGuiMod_Ctrl | ImGuiKey_PageDown);
    CHECK(scene.state->active_tab() == draft);

    press(*view, ImGuiMod_Ctrl | ImGuiKey_W);
    CHECK(scene.state->tabs().size() == 1);
    CHECK(scene.state->active_tab() == first);
    settle(*view);
    CHECK(view->item_rects().contains(std::format("note_tab:{}", first)));
}

TEST_CASE("tabs: middle-click and Ctrl+click on a tree row open a new tab; clicking a tab shows it") {
    TabsScene scene;
    auto view = make_view(*scene.state);
    settle(*view);
    REQUIRE(view->item_rects().contains("note:a.md"));
    click_with(*view, "note:a.md", ImGuiMouseButton_Middle);
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"c.md", "a.md"});
    CHECK(active_path(*scene.state) == "a.md");
    click_with(*view, "note:b.md", ImGuiMouseButton_Left, true);
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"c.md", "a.md", "b.md"});

    const std::string first = std::format("note_tab:{}", tab_id(*scene.state, "c.md"));
    REQUIRE(view->item_rects().contains(first));
    click_with(*view, first, ImGuiMouseButton_Left);
    CHECK(active_path(*scene.state) == "c.md");
    CHECK(scene.state->buffer() == "gama\n");
}

namespace {

// ImGui'nin sekme çubuğunda seçili gördüğü editör sekmesinin kimliği ("###note_tab<id>" sonu); yoksa 0.
std::uint64_t imgui_selected_note_tab() {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    constexpr std::string_view kMarker = "###note_tab";
    for (int index = 0; index < context.TabBars.GetMapSize(); ++index) {
        ImGuiTabBar* bar = context.TabBars.TryGetMapData(index);
        if (bar == nullptr)
            continue;
        ImGuiTabItem* tab = ImGui::TabBarFindTabByID(bar, bar->SelectedTabId);
        if (tab == nullptr)
            continue;
        const std::string_view name = ImGui::TabBarGetTabName(bar, tab);
        if (const auto at = name.find(kMarker); at != std::string_view::npos)
            return std::stoull(std::string(name.substr(at + kMarker.size())));
    }
    return 0;
}

}  // namespace

// İnceleme 1: reddedilen geçiş her karede yeniden istenmez (çakışma sorusu bir kez), çubuk etkin sekmeye
// döner.
TEST_CASE("tabs: a refused switch is asked once and the bar returns to the active tab") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->open_in_new_tab(np("b.md"));
    const auto a = tab_id(*scene.state, "a.md");
    const auto b = tab_id(*scene.state, "b.md");
    REQUIRE(scene.state->activate_tab(a));
    int conflicts = 0;
    scene.state->deciders.conflict = [&conflicts](const std::string&) {
        ++conflicts;
        return noteapp::gui::ConflictChoice::Cancel;
    };
    scene.state->edit_text("A1\n");
    scene.fixture.add_note("a.md", "dışarıda\n");  // kayıt çakışır
    auto view = make_view(*scene.state);
    settle(*view);
    click_with(*view, std::format("note_tab:{}", b), ImGuiMouseButton_Left);
    for (int frame = 0; frame < 10; ++frame)
        view->draw_frame();
    CHECK(conflicts == 1);
    CHECK(scene.state->active_tab() == a);
    CHECK(scene.state->buffer() == "A1\n");
    CHECK(imgui_selected_note_tab() == a);
}

// İnceleme 2-3: tarama eski listeyle sekme kapatıp komşuyu öne getirince, yolu yeni olan komşu "yok" sanılıp
// başka bir sekme düşürülüyordu. Gösterilen not dışarıda silinmiş, kullanıcı arka plandaki notun klasörünü
// yeniden adlandırıyor (aynı tarama).
TEST_CASE("tabs: closing a missing shown note in the same scan as a folder rename keeps the renamed tab") {
    TabsScene scene;
    scene.fixture.add_note("proje/plan.md", "plan\n", 40);
    scene.fixture.add_note("gone.md", "gidecek\n", 50);
    scene.state->rescan();
    scene.open("proje/plan.md");
    scene.state->open_in_new_tab(np("gone.md"));
    REQUIRE(tab_paths(*scene.state) == std::vector<std::string>{"proje/plan.md", "gone.md"});
    scene.fixture.remove_note("gone.md");  // henüz taranmadı
    scene.state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("iş");
    };
    scene.state->rename_folder("proje");
    CHECK(tab_paths(*scene.state) == std::vector<std::string>{"iş/plan.md"});
    CHECK(active_path(*scene.state) == "iş/plan.md");
    CHECK(scene.state->buffer() == "plan\n");
    CHECK(scene.state->status().find("gone.md") != std::string::npos);
}

// İnceleme 4: dışarıda değişen notta saklı imleç kod noktasının ortasına düşmez (yazılan bayt UTF-8'i
// bozardı). Yeniden okuma imleci aynı satır/sütunda tutar (TUI EditorBuffer::reload): sütun 2, "aç"ın sonu.
TEST_CASE("tabs: a remembered cursor is moved back to a code point boundary after a reload") {
    TabsScene scene;
    scene.open("a.md");
    scene.state->editor_key(noteapp::tui::Key::Right);
    scene.state->editor_key(noteapp::tui::Key::Right);
    scene.state->open_in_new_tab(np("c.md"));
    scene.fixture.add_note("a.md", "aç\n");
    scene.state->rescan();
    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "a.md")));
    REQUIRE(scene.state->buffer() == "aç\n");
    CHECK_FALSE(scene.state->editor_selection());
    CHECK(scene.state->editor().buffer().cursor() == 3);
}

// A5b: klasör yeniden adlandırılınca arka plandaki sekmenin belgesi de yeni yolu izler; yoksa düzenleme eski
// yola yazılır (Gone -> kaydet diyaloğu ya da o yolda sonradan açılan başka notla çakışma).
TEST_CASE("tabs: a background tab in a renamed folder saves to the new path") {
    TabsScene scene;
    scene.fixture.add_note("f/n.md", "eski\n", 5);
    scene.state->rescan();
    scene.open("f/n.md");
    scene.state->open_in_new_tab(np("b.md"));
    REQUIRE(active_path(*scene.state) == "b.md");
    scene.state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("g");
    };
    scene.state->rename_folder("f");
    REQUIRE(scene.fixture.note_exists("g/n.md"));

    REQUIRE(scene.state->activate_tab(tab_id(*scene.state, "g/n.md")));
    scene.state->edit_text("yeni\n");
    scene.state->save_now();
    CHECK(scene.save_dialogs == 0);
    CHECK(scene.fixture.read_note("g/n.md") == "yeni\n");
    CHECK_FALSE(scene.fixture.note_exists("f/n.md"));
}
