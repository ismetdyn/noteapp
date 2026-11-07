// R3 (karar 6): ağaç, Ayrıntılar ve düz liste kullanıcının sıralamasıyla; sıralama menüsü TUI'nin `o`
// menüsüyle aynı komut kimlikleri. Sıra ve açık klasörler gui-state.toml'da TUI'nin anahtar adlarıyla kalır.
#include "gui/browser_rows.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::gui::BrowserRow;
using noteapp::gui::BrowserRowKind;
using noteapp::gui::GuiState;
using noteapp::search::SortField;

namespace {

using Paths = std::vector<std::string>;

Paths tree(GuiState& state) {
    Paths paths;
    for (const BrowserRow& row : state.browser_rows())
        paths.push_back((row.kind == BrowserRowKind::Folder ? "d:" : "n:") + row.path);
    return paths;
}

Paths flat(const GuiState& state) {
    Paths paths;
    for (int row = 0; row < state.model().row_count(); ++row)
        paths.push_back(state.model().note_at(row).path.string());
    return paths;
}

// Komut tablosundaki eylem: kimliğin GUI'de bağı olduğunu da kanıtlar.
void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

struct PrefsFixture : WindowFixture {
    PrefsFixture() {
        add_note("b.md", "b", 30);
        add_note("a.md", "a", 10);
        add_note("c.md", "c", 20);
        add_note("work/x.md", "x", 40);
        add_note("home/y.md", "y", 5);
    }
    std::filesystem::path state_file() const { return home.path() / "state" / "gui-state.toml"; }
    std::string state_text() const {
        std::ifstream in(state_file(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

}  // namespace

TEST_CASE_FIXTURE(PrefsFixture, "the tree and the flat list follow the chosen sort; reverse flips it") {
    auto state = make_state();
    // Varsayılan TUI ile aynı: en yeni başta, klasörler kendi grubunda aynı sırayla.
    CHECK(state->sort().field == SortField::Updated);
    CHECK(state->sort().descending);
    CHECK(tree(*state) == Paths{"d:", "d:home", "d:work", "n:a.md", "n:c.md", "n:b.md"});

    run_command(*state, "sort.name");
    CHECK(state->sort().field == SortField::Name);
    CHECK_FALSE(state->sort().descending);
    CHECK(tree(*state) == Paths{"d:", "d:home", "d:work", "n:a.md", "n:b.md", "n:c.md"});

    run_command(*state, "sort.reverse");
    CHECK(state->sort().descending);
    CHECK(tree(*state) == Paths{"d:", "d:work", "d:home", "n:c.md", "n:b.md", "n:a.md"});
    // Metinsiz düz liste de aynı sırayla (yalnız argüman: kapsamı daraltmayan `type:`).
    state->set_query("type:md");
    CHECK(flat(*state) == Paths{"home/y.md", "work/x.md", "c.md", "b.md", "a.md"});  // dosya adına göre

    run_command(*state, "sort.size");
    CHECK(state->sort().field == SortField::Size);
    CHECK(state->sort().descending);  // boyut ve tarih: en büyük ve en yeni başta
    run_command(*state, "sort.created");
    CHECK(state->sort().field == SortField::Created);
    run_command(*state, "sort.updated");
    CHECK(state->sort().field == SortField::Updated);
    CHECK(flat(*state) == Paths{"home/y.md", "a.md", "c.md", "b.md", "work/x.md"});
}

TEST_CASE_FIXTURE(PrefsFixture, "the chosen sort is saved at once and restored with the TUI's key names") {
    {
        auto state = make_state();
        run_command(*state, "sort.size");
        run_command(*state, "sort.reverse");
        // Kapanış beklenmez: TUI gibi seçimde yazılır.
        const std::string saved = state_text();
        // TOML dizgesi iki tırnak biçiminde de yazılabilir; anahtar ve değer TUI'ninkiyle aynı.
        CHECK((saved.find("sort = \"size\"") != std::string::npos ||
               saved.find("sort = 'size'") != std::string::npos));
        CHECK(saved.find("sort_descending = false") != std::string::npos);
#ifndef _WIN32
        // Arama geçmişi taşır: TUI'nin tui-state.toml'u gibi yalnız sahibi okur (app::save_ui_prefs).
        using std::filesystem::perms;
        CHECK((std::filesystem::status(state_file()).permissions() & (perms::group_all | perms::others_all)) ==
              perms::none);
#endif
    }
    auto reopened = make_state();
    CHECK(reopened->sort().field == SortField::Size);
    CHECK_FALSE(reopened->sort().descending);

    std::ofstream(state_file(), std::ios::binary) << "sort = \"bogus\"\nsort_descending = \"no\"\n";
    auto fallback = make_state();
    CHECK(fallback->sort().field == SortField::Updated);
    CHECK(fallback->sort().descending);
}

TEST_CASE_FIXTURE(PrefsFixture, "the Sort button opens the sort menu and its items run the sort commands") {
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    REQUIRE(view->item_rects().contains("sort"));
    CHECK_FALSE(view->item_rects().contains("sort:sort.name"));
    click_button(*view, "sort");
    settle(*view);
    REQUIRE(view->item_rects().contains("sort:sort.name"));
    click_button(*view, "sort:sort.name");
    settle(*view);
    CHECK(state->sort().field == SortField::Name);

    // Komut da menüyü açar (palet ve kısayol yolu).
    run_command(*state, "sort");
    settle(*view);
    REQUIRE(view->item_rects().contains("sort:sort.reverse"));
    click_button(*view, "sort:sort.reverse");
    settle(*view);
    CHECK(state->sort().descending);
}

TEST_CASE_FIXTURE(PrefsFixture, "open folders survive a restart; folders that are gone are not saved") {
    {
        auto state = make_state();
        state->set_expanded("work", true);
        state->set_expanded("gone", true);
        CHECK(state->request_close());
    }
    const std::string saved = state_text();
    CHECK(saved.find("open_folders") != std::string::npos);
    CHECK(saved.find("gone") == std::string::npos);

    auto reopened = make_state();
    CHECK(reopened->is_expanded("work"));
    CHECK_FALSE(reopened->is_expanded("home"));
    CHECK(tree(*reopened) == Paths{"d:", "d:home", "d:work", "n:work/x.md", "n:a.md", "n:c.md", "n:b.md"});
}

TEST_CASE_FIXTURE(PrefsFixture, "expanding or collapsing a folder saves the open folders at once") {
    auto state = make_state();
    const auto open_line = [this] {
        const std::string text = state_text();
        const std::size_t start = text.find("open_folders");
        return start == std::string::npos ? std::string()
                                          : text.substr(start, text.find('\n', start) - start);
    };
    state->set_expanded("work", true);
    // Kapanış beklenmez: çökme ya da kill açık klasörleri kaybettirmesin (TUI her değişimde yazar).
    CHECK(open_line().find("work") != std::string::npos);
    state->toggle_expanded("work");
    CHECK(open_line() == "open_folders = []");
}

TEST_CASE_FIXTURE(PrefsFixture, "Esc closes the sort menu and the tag picker") {
    add_note("t.md", "#etiket\n", 1);
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    click_button(*view, "sort");
    settle(*view);
    REQUIRE(view->item_rects().contains("sort:sort.name"));
    gui_test::press(*view, ImGuiKey_Escape);
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("sort:sort.name"));

    gui_test::press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T);
    settle(*view);
    REQUIRE(view->item_rects().contains("picker:etiket"));
    gui_test::press(*view, ImGuiKey_Escape);
    settle(*view);
    CHECK_FALSE(view->item_rects().contains("picker:etiket"));
    CHECK(state->picker() == noteapp::gui::Picker::None);
}
