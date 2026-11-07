// G3: komut paleti (app::rank_palette, son kullanılanlar limits.app.recent_commands), kayıttan üretilen
// yardım, ilişki `L`, gezinme kimlikleri, bölme ve uygulama komutları; TUI ile aynı komut kimlikleri.
#include "app/commands.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::settle;
using gui_test::tap_key;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::PaletteRow;

namespace {

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

std::vector<std::string> ids(const std::vector<PaletteRow>& rows) {
    std::vector<std::string> out;
    for (const PaletteRow& row : rows)
        out.emplace_back(row.id);
    return out;
}

const PaletteRow* row_of(const std::vector<PaletteRow>& rows, std::string_view id) {
    const auto found = std::ranges::find(rows, id, &PaletteRow::id);
    return found == rows.end() ? nullptr : &*found;
}

struct PaletteScene : WindowFixture {
    PaletteScene() {
        add_note("a.md", "a\n", 20);
        add_note("b.md", "b\n", 10);
        add_note("c.md", "c\n", 30);
    }
    std::string state_text() const {
        std::ifstream in(home.path() / "state" / "gui-state.toml", std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

// Diyalog karesi betiği: önce açan kısayol bırakılır, sonra yazı ve yanıt.
struct Script {
    int frame = 0;
    const char* text = nullptr;
    ImGuiKey answer = ImGuiKey_Enter;
    std::function<void()> at_answer;
    void operator()() {
        constexpr int kTypeFrame = 5;
        constexpr int kGiveUpFrame = 200;
        ++frame;
        if (frame == 1) {
            ImGuiIO& io = ImGui::GetIO();
            for (const ImGuiKey key : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_P, ImGuiKey_F1, ImGuiKey_R})
                io.AddKeyEvent(key, false);
        }
        if (frame == kTypeFrame && text != nullptr)
            ImGui::GetIO().AddInputCharactersUTF8(text);
        if (frame == kTypeFrame + 2) {
            if (at_answer)
                at_answer();
            tap_key(answer);
        }
        if (frame == kGiveUpFrame)
            tap_key(ImGuiKey_Escape);
    }
};

}  // namespace

TEST_CASE_FIXTURE(PaletteScene, "palette rows follow the registry; blocked rows sink, recent rows rise") {
    auto state = make_state();
    const auto all = state->palette_rows("");
    // Kayıttaki her GUI komutu bir kez; çalışamayan (yank yokken paste) aşağıda ama görünür.
    for (const noteapp::app::CommandSpec& spec : noteapp::app::kCommandRegistry) {
        const bool bound = std::ranges::any_of(noteapp::gui::kGuiCommands,
                                               [&](const auto& command) { return command.id == spec.id; });
        CAPTURE(spec.id);
        CHECK((row_of(all, spec.id) != nullptr) == bound);
    }
    const PaletteRow* paste = row_of(all, "paste");
    REQUIRE(paste != nullptr);
    CHECK(paste->blocked != nullptr);
    CHECK(ids(all).back() != "new");

    state->run_palette_command("paste");  // çalışmaz: nedeni durum çubuğunda
    CHECK(state->status() == noteapp::core::tr(paste->blocked));

    state->run_palette_command("details");
    CHECK(state->info_open());
    state->run_palette_command("go.root");
    CHECK(ids(state->palette_rows("")).front() == "go.root");  // en yeni başta
    CHECK(state->recent_commands() == std::vector<std::string>{"go.root", "details"});
    CHECK(ids(state->palette_rows("trash")).front().starts_with("trash"));

    auto reopened = make_state();
    CHECK(reopened->recent_commands() == std::vector<std::string>{"go.root", "details"});
}

TEST_CASE_FIXTURE(PaletteScene, "recent commands are capped by limits.app.recent_commands") {
    config.limits.app.recent_commands = noteapp::core::Limit{2, "2"};
    auto state = make_state();
    for (const char* id : {"details", "go.root", "pane.zoom", "pane.zoom"})
        state->run_palette_command(id);
    CHECK(state->recent_commands() == std::vector<std::string>{"pane.zoom", "go.root"});
}

TEST_CASE_FIXTURE(PaletteScene, "Ctrl+Shift+P opens the palette; typing filters and Enter runs the top row") {
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    Script script;
    script.text = "go root";
    bool drawn = false;
    script.at_answer = [&] { drawn = view->item_rects().contains("palette:go.root"); };
    view->on_modal_frame = std::ref(script);
    state->enter_folder("");
    state->set_query("b");
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P);
    settle(*view);
    CHECK(drawn);
    CHECK(state->query().empty());  // go.root çalıştı
    CHECK(state->recent_commands() == std::vector<std::string>{"go.root"});
}

TEST_CASE_FIXTURE(PaletteScene, "F1 shows every registry command with its GUI binding") {
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    std::vector<std::string> missing;
    bool shown = false;
    RenderedText text;
    Script script;
    script.answer = ImGuiKey_Escape;
    script.at_answer = [&] {
        shown = view->item_rects().contains("modal");
        for (const noteapp::app::CommandSpec& spec : noteapp::app::kCommandRegistry)
            if (!view->item_rects().contains("help:" + std::string(spec.id)))
                missing.emplace_back(spec.id);
    };
    view->on_modal_frame = std::ref(script);
    press(*view, ImGuiKey_F1);
    settle(*view);
    REQUIRE(shown);
    CHECK(missing.empty());
    CHECK(text.contains("Ctrl+Alt+Z"));                 // pane.zoom: yalnız yardımda yazılır
    CHECK(text.contains("Ctrl+Shift+P"));               // paletin kendisi
    CHECK_FALSE(view->item_rects().contains("modal"));  // Esc kapattı
}

TEST_CASE_FIXTURE(PaletteScene, "Ctrl+R relates the shown note to another; the related row can be unlinked") {
    auto state = make_state();
    auto view = make_view(*state);
    state->open_note(*noteapp::core::NotePath::parse("a.md"));
    std::string asked_title;
    state->deciders.link_target = [&](const std::string& title) {
        asked_title = title;
        return std::optional<std::string>("b");
    };
    state->deciders.confirm_delete = [](const std::string&, const std::string&) { return true; };
    // F1: hedeften sonra ad penceresi (TUI `L` ile aynı); ad ve yön ilişkiye gider.
    noteapp::gui::RelationRequest asked;
    state->deciders.relation = [&](const noteapp::gui::RelationRequest& request) {
        asked = request;
        return std::optional(noteapp::core::RelationSpec{"uses", true});
    };
    press(*view, ImGuiMod_Ctrl | ImGuiKey_R);
    CHECK_FALSE(asked_title.empty());
    CHECK(asked.from == "a.md");
    CHECK(asked.to == "b.md");
    REQUIRE(state->links().related.size() == 1);
    CHECK(state->links().related.front().path.string() == "b.md");
    CHECK(state->links().related.front().name == "uses");
    CHECK(state->links().related.front().direction == noteapp::core::RelationDirection::Outgoing);

    state->set_links_open(false);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L);
    settle(*view);
    CHECK(state->links_open());
    CHECK(view->item_rects().contains("link:b.md"));

    state->unlink_note(state->links().related.front());
    CHECK(state->links().related.empty());
}

TEST_CASE_FIXTURE(PaletteScene, "Home, End and page keys move the list selection; E focuses the editor") {
    auto state = make_state();
    auto view = make_view(*state);
    state->set_query("type:md");  // düz liste: b, a, c (en yeni başta)
    state->select_row(1);
    state->request_focus(FocusRequest::List);
    settle(*view);
    press(*view, ImGuiKey_End);
    CHECK(state->current_row() == 2);
    press(*view, ImGuiKey_Home);
    CHECK(state->current_row() == 0);
    press(*view, ImGuiKey_PageDown);
    CHECK(state->current_row() == 2);  // görünen satır sayfadan az
    press(*view, ImGuiKey_E);
    settle(*view);
    CHECK(view->editor_active());
}

TEST_CASE_FIXTURE(PaletteScene, "pane keys resize and zoom the list pane; Ctrl+Q asks to quit") {
    auto state = make_state();
    auto view = make_view(*state);
    const float before = state->layout().left_pane_width;
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_W);
    CHECK(state->layout().left_pane_width > before);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_N);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_N);
    CHECK(state->layout().left_pane_width < before);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Z);
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("left_pane"));
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Z);
    settle(*view);
    CHECK(view->item_rects().contains("left_pane"));

    CHECK_FALSE(view->close_requested());
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Q);
    CHECK(view->close_requested());
}

TEST_CASE_FIXTURE(PaletteScene, "icons.toggle flips the icon setting and writes config.toml") {
    auto state = make_state();
    const auto before = state->icon_style();
    run_command(*state, "icons.toggle");
    CHECK(state->icon_style() != before);
    std::ifstream in(noteapp::core::config_file(), std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>(in), {}};
    CHECK(written.find("icons") != std::string::npos);
}

// G4 (G2+G3 review): simge anahtarı config.toml'u yeniden yazarken elle yazılmış [lsp] ve [limits]
// tablolarını, birimli değeri ("64MiB") birimiyle korur.
TEST_CASE_FIXTURE(PaletteScene, "icons.toggle keeps hand-written [lsp] and [limits] in config.toml") {
    {
        std::filesystem::create_directories(noteapp::core::config_file().parent_path());
        std::ofstream out(noteapp::core::config_file(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << "icons = \"nerd\"\n\n"
               "[lsp]\nenabled = false\n\n"
               "[lsp.servers]\npython = [\"my-pyls\", \"--stdio\"]\n\n"
               "[limits.tui]\nundo_bytes = \"64MiB\"\n";
    }
    config = noteapp::core::load_config().config;
    REQUIRE_FALSE(config.lsp.enabled);  // dosya gerçekten okundu
    config.notes_dir = home.path() / "notes";
    auto state = make_state();
    REQUIRE(state->icon_style() == noteapp::core::IconStyle::Nerd);
    run_command(*state, "icons.toggle");
    CHECK(state->icon_style() == noteapp::core::IconStyle::Off);
    std::ifstream in(noteapp::core::config_file(), std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>(in), {}};
    INFO(written);
    CHECK(written.find("undo_bytes = \"64MiB\"") != std::string::npos);
    const noteapp::core::Config reloaded = noteapp::core::load_config().config;
    CHECK(reloaded.icons == "off");
    CHECK_FALSE(reloaded.lsp.enabled);
    CHECK(reloaded.lsp.servers.at("python") == std::vector<std::vector<std::string>>{{"my-pyls", "--stdio"}});
    CHECK(reloaded.limits.tui.undo_bytes.value == std::uint64_t{64} * 1024 * 1024);
}

TEST_CASE_FIXTURE(PaletteScene, "preview keys scroll the markdown preview") {
    std::string body = "# Başlık\n\n";
    for (int line = 0; line < 200; ++line)
        body += "satır " + std::to_string(line) + "\n\n";
    add_note("long.md", body, 1);
    auto state = make_state();
    auto view = make_view(*state);
    state->open_note(*noteapp::core::NotePath::parse("long.md"));
    settle(*view);
    REQUIRE(state->showing_preview());
    const auto [x, y] = view->button_center("preview");
    ImGui::GetIO().AddMousePosEvent(x, y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    settle(*view);
    REQUIRE(view->item_rects().contains("preview_block:0"));
    press(*view, ImGuiMod_Ctrl | ImGuiKey_D);
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("preview_block:0"));  // ilk blok kaydırıldı
    press(*view, ImGuiKey_Home);
    settle(*view);
    REQUIRE(state->showing_preview());  // Home listeye gitmedi: odak önizlemede kaldı
    CHECK(view->item_rects().contains("preview_block:0"));
}
