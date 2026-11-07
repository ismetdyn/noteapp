// GUI araması TUI/CLI ile aynı sorgu argümanlarını alır (search::parse_query): ad modunda argümanlar süzer,
// kalan metin fuzzy sıralar; içerik modunda argümanlar süzer, kalan metin içerikte aranır.
#include "core/i18n.hpp"
#include "core/note_views.hpp"
#include "gui/gui_state.hpp"
#include "search/query.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using gui_test::make_view;
using gui_test::settle;
using gui_test::WindowFixture;

namespace {

std::vector<std::string> rows(const noteapp::gui::GuiState& state) {
    std::vector<std::string> out;
    for (int row = 0; row < state.model().row_count(); ++row)
        out.push_back(state.model().display(row));
    return out;
}

struct ArgsFixture : WindowFixture {
    ArgsFixture() {
        add_note("plan.md", "#proje toplantı\nbütçe\n", 10);
        add_note("alt/plan2.md", "#proje/alt bütçe\n", 20);
        add_note("ev.txt", "#ev bütçe\n", 30);
        add_note("kod.py", "print('bütçe')\n", 40);
    }
};

}  // namespace

TEST_CASE_FIXTURE(ArgsFixture, "gui search: tag: filters by tag and its nested tags") {
    auto state = make_state();
    state->set_query("tag:proje");
    CHECK(rows(*state) == std::vector<std::string>{"plan.md", "alt/plan2.md"});
    state->set_query("tag:proje/alt");
    CHECK(rows(*state) == std::vector<std::string>{"alt/plan2.md"});
    CHECK_FALSE(state->query_error().has_value());
    state->set_query("tag:yok");  // argümanlı sorgu not metni değildir: Enter yeni not açmaz
    CHECK(rows(*state).empty());
    state->search_enter();
    CHECK_FALSE(state->is_new_note());
}

// TUI kuralı (app::search_enter_action): boş sorguda Enter boş not açmaz; eşleşmesiz düz metin taslağın ilk
// satırı olur.
TEST_CASE_FIXTURE(WindowFixture, "gui search: Enter on an empty query with no row opens no note") {
    auto state = make_state();
    REQUIRE(state->model().row_count() == 0);
    state->search_enter();
    CHECK_FALSE(state->is_new_note());
    state->set_query("fikir");
    state->search_enter();
    CHECK(state->is_new_note());
}

// T81: kayıtlı görünüm aynı arama kutusundan; bilinmeyen görünüm argüman hatasıdır.
TEST_CASE_FIXTURE(ArgsFixture, "gui search: view: keeps the notes a saved view returns") {
    noteapp::core::init_i18n("en");
    noteapp::core::NoteStore store(store_dir());
    REQUIRE(noteapp::core::save_view(store, "proje",
                                     "MATCH (n:Note)-[:TAGGED]->(:Tag {name: 'proje'}) RETURN n", false)
                .has_value());
    auto state = make_state();
    state->set_query("view:proje");
    CHECK(rows(*state) == std::vector<std::string>{"plan.md"});
    CHECK_FALSE(state->query_error().has_value());
    state->set_query("view:yok");
    REQUIRE(state->query_error().has_value());
    CHECK(state->query_error()->find("yok") != std::string::npos);
    CHECK(rows(*state) == std::vector<std::string>{"plan.md"});  // son geçerli liste kalır
}

TEST_CASE_FIXTURE(ArgsFixture, "gui search: type: filters and the remaining text ranks by name") {
    auto state = make_state();
    state->set_query("type:md");
    CHECK(rows(*state) == std::vector<std::string>{"plan.md", "alt/plan2.md"});
    state->set_query("plan2 type:md");
    CHECK(rows(*state) == std::vector<std::string>{"alt/plan2.md"});
    state->set_query("type:py kod");
    CHECK(rows(*state) == std::vector<std::string>{"kod.py"});
}

TEST_CASE_FIXTURE(ArgsFixture,
                  "gui search: content mode filters by arguments and searches the remaining text") {
    auto state = make_state();
    state->toggle_search_mode();
    state->set_query("bütçe tag:proje");
    CHECK(rows(*state) == std::vector<std::string>{"plan.md  (1)", "alt/plan2.md  (1)"});
    state->set_query("bütçe type:py");
    CHECK(rows(*state) == std::vector<std::string>{"kod.py  (1)"});
    state->set_query("type:txt");  // metin yok: süzülmüş liste
    CHECK(rows(*state) == std::vector<std::string>{"ev.txt"});
}

TEST_CASE_FIXTURE(ArgsFixture,
                  "gui search: an invalid argument shows the TUI's error text and keeps the list") {
    noteapp::core::init_i18n("en");
    auto state = make_state();
    state->set_query("tag:proje");
    const std::vector<std::string> before = rows(*state);
    state->set_query("tag:proje size:abc");
    const auto parsed = noteapp::search::parse_query("tag:proje size:abc");
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(state->query_error().has_value());
    CHECK(*state->query_error() == noteapp::core::format_error(parsed.error().error));
    CHECK(rows(*state) == before);
    state->toggle_search_mode();  // içerik modunda da çökmez, liste aynı kalır
    CHECK(state->query_error().has_value());
    CHECK(rows(*state) == before);

    auto view = make_view(*state);
    settle(*view);
    CHECK(view->item_rects().contains("search_error"));
    state->set_query("tag:proje");
    settle(*view);
    CHECK_FALSE(state->query_error().has_value());
    CHECK_FALSE(view->item_rects().contains("search_error"));
}
