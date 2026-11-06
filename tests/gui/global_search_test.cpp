// R3 (P-G1): genel arama (ad ve içerik birlikte, en iyi satırla), etiket ve görünüm seçici ile `sync:`
// argümanı GUI'de; kurallar app::Listing'dedir, TUI ile aynı.
#include "core/note_views.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::settle;
using gui_test::WindowFixture;
using noteapp::app::SearchMode;
using noteapp::gui::FocusRequest;
using noteapp::gui::GuiState;
using noteapp::gui::Picker;
using noteapp::gui::PickerItem;

namespace {

using Strings = std::vector<std::string>;

Strings rows(const GuiState& state) {
    Strings out;
    for (int row = 0; row < state.model().row_count(); ++row)
        out.push_back(state.model().note_at(row).path.string());
    return out;
}

Strings values(const std::vector<PickerItem>& items) {
    Strings out;
    for (const PickerItem& item : items)
        out.push_back(item.value);
    return out;
}

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    found->run(state);
}

struct SearchFixture : WindowFixture {
    SearchFixture() {
        add_note("plan.md", "#proje toplantı\nbütçe\n", 10);
        add_note("alt/plan2.md", "#proje/alt bütçe\n", 20);
        add_note("notlar.md", "#ev alışveriş\nplan yapıldı\n", 30);
        add_note("kod.py", "print('x')\n", 40);
    }
};

}  // namespace

TEST_CASE_FIXTURE(SearchFixture, "global search finds names and contents; content rows carry the best line") {
    auto state = make_state();
    run_command(*state, "search.global");
    CHECK(state->search_mode() == SearchMode::Global);
    CHECK(state->take_focus_request() == FocusRequest::Search);

    state->set_query("plan");
    const Strings found = rows(*state);
    CHECK(std::ranges::find(found, "plan.md") != found.end());
    CHECK(std::ranges::find(found, "alt/plan2.md") != found.end());
    REQUIRE(std::ranges::find(found, "notlar.md") != found.end());  // yalnız içerikte geçiyor
    const int content_row = state->model().index_of(*noteapp::core::NotePath::parse("notlar.md"));
    const noteapp::app::ListRow& row = state->model().row(content_row);
    CHECK(row.show_best_line);
    REQUIRE_FALSE(row.hits.empty());
    CHECK(row.hits.front().snippet.find("plan yapıldı") != std::string::npos);
    CHECK(state->model().display(content_row) == "notlar.md  (1)");

    // TUI: eşleşmesiz genel aramada Enter not açmaz; Tab genelden ada döner.
    state->set_query("zzzz");
    CHECK(rows(*state).empty());
    state->search_enter();
    CHECK_FALSE(state->is_new_note());
    state->toggle_search_mode();
    CHECK(state->search_mode() == SearchMode::Name);
}

TEST_CASE_FIXTURE(SearchFixture,
                  "global rows draw the best line under the path; Ctrl+Shift+F switches mode") {
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    settle(*view);
    REQUIRE(state->search_mode() == SearchMode::Global);
    state->set_query("plan");
    RenderedText text;
    settle(*view);
    CHECK(text.contains("notlar.md  (1)"));
    CHECK(text.contains("plan yapıldı"));
}

// İlişki araması (SPEC-gui): ad ve içerikte bulunmayan not ilişkisiyle gelir, nedeni satırda ikincil metin.
TEST_CASE_FIXTURE(SearchFixture,
                  "a note found only through a tag or a relation draws its reason on the row") {
    {
        noteapp::core::NoteStore store(store_dir());
        REQUIRE(store
                    .add_relation(*noteapp::core::NotePath::parse("kod.py"),
                                  *noteapp::core::NotePath::parse("plan.md"), {"uses", true})
                    .has_value());
    }
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    state->set_query("ev");  // ad modu: hiçbir yolda yok, notlar.md'nin #ev etiketi
    REQUIRE(rows(*state) == Strings{"notlar.md"});
    REQUIRE(state->model().row(0).reason.has_value());
    {
        RenderedText text;
        settle(*view);
        CHECK(text.contains("tag: #ev"));
    }
    state->set_search_mode(SearchMode::Global);
    state->set_query("uses");  // ad ve içerikte yok: ilişkinin iki ucu
    const Strings related = rows(*state);
    CHECK(std::ranges::is_permutation(related, Strings{"kod.py", "plan.md"}));
    RenderedText text;
    settle(*view);
    CHECK(text.contains("relation: uses → plan.md"));
    CHECK(text.contains("relation: uses ← kod.py"));
}

TEST_CASE_FIXTURE(SearchFixture, "the tag picker lists tags; picking one filters by tag: in name mode") {
    auto state = make_state();
    state->set_search_mode(SearchMode::Content);
    run_command(*state, "search.tag");
    REQUIRE(state->picker() == Picker::Tag);
    CHECK(values(state->picker_items()) == Strings{"ev", "proje", "proje/alt"});

    state->pick("proje");
    CHECK(state->picker() == Picker::None);
    CHECK(state->query() == "tag:proje");
    CHECK(state->search_mode() == SearchMode::Name);
    CHECK(rows(*state) == Strings{"plan.md", "alt/plan2.md"});
    CHECK_FALSE(state->query_error().has_value());
}

TEST_CASE_FIXTURE(SearchFixture, "the view picker runs a saved view; a name with a space is quoted") {
    noteapp::core::NoteStore store(store_dir());
    REQUIRE(noteapp::core::save_view(store, "proje",
                                     "MATCH (n:Note)-[:TAGGED]->(:Tag {name: 'proje'}) RETURN n", false)
                .has_value());
    REQUIRE(noteapp::core::save_view(store, "ev işleri",
                                     "MATCH (n:Note)-[:TAGGED]->(:Tag {name: 'ev'}) RETURN n", false)
                .has_value());
    auto state = make_state();
    run_command(*state, "search.view");
    REQUIRE(state->picker() == Picker::View);
    CHECK(values(state->picker_items()) == Strings{"ev işleri", "proje"});

    state->pick("ev işleri");
    CHECK(state->query() == "view:\"ev işleri\"");
    CHECK_FALSE(state->query_error().has_value());
    CHECK(rows(*state) == Strings{"notlar.md"});
}

TEST_CASE_FIXTURE(SearchFixture, "Ctrl+Shift+T opens the tag picker; clicking a tag sets the query") {
    auto state = make_state();
    auto view = make_view(*state);
    settle(*view);
    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T);
    settle(*view);
    REQUIRE(view->item_rects().contains("picker:proje"));
    click_button(*view, "picker:proje");
    settle(*view);
    CHECK(state->query() == "tag:proje");
    CHECK(state->picker() == Picker::None);
    CHECK_FALSE(view->item_rects().contains("picker:ev"));  // seçici kapandı, satırı yeniden çizilmedi
}

TEST_CASE_FIXTURE(SearchFixture, "sync: sees per-note sync states filled off the UI thread") {
    // Senkron açık ve depo git deposu: henüz gönderilmemiş notlar `new`dir (sync::fill_sync_status).
    config.sync.enabled = true;
    std::filesystem::create_directories(config.notes_dir / ".git");
    auto state = make_state();
    state->set_query("sync:new");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state->model().row_count() < 4 && std::chrono::steady_clock::now() < deadline)
        state->tick(std::chrono::steady_clock::now());
    CHECK(state->model().row_count() == 4);
    state->set_query("sync:sync_off");
    CHECK_FALSE(state->query_error().has_value());
    CHECK(rows(*state).empty());
}

TEST_CASE_FIXTURE(SearchFixture, "sync states that did not change draw no frames; changed ones still do") {
    config.sync.enabled = true;
    std::filesystem::create_directories(config.notes_dir / ".git");
    auto state = make_state();
    auto view = make_view(*state);
    state->set_query("sync:new");
    const auto idle_until = [&](const auto& done, std::chrono::milliseconds limit) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < until)
            view->idle_step(1);
    };
    idle_until([&] { return state->model().row_count() == 4; }, std::chrono::seconds(10));
    REQUIRE(state->model().row_count() == 4);
    // Açılıştaki senkron iletileri dinsin: 500 ms kare yoksa sakin.
    for (auto quiet = view->frames(), seen = quiet + 1; seen != quiet;) {
        seen = quiet;
        idle_until([] { return false; }, std::chrono::milliseconds(500));
        quiet = view->frames();
    }
    const std::size_t frames = view->frames();

    // Başka bağlantı yazar: liste değişir, notun durumu aynı (new). Tarama doğrudan: kareyi yalnız işçinin
    // sonucu üretebilir.
    add_note("plan.md", "#proje toplantı\nbütçe\nek satır\n", 5);
    state->rescan();
    idle_until([] { return false; }, std::chrono::milliseconds(1500));
    CHECK(view->frames() == frames);

    // Yeni not yeni durum getirir: sonuç uygulanır ve çizilir.
    add_note("yeni.md", "x\n");
    state->rescan();
    idle_until([&] { return state->model().row_count() == 5 && view->frames() > frames; },
               std::chrono::seconds(10));
    CHECK(state->model().row_count() == 5);
    CHECK(view->frames() > frames);
}

TEST_CASE_FIXTURE(SearchFixture, "a name-only global row is one line tall; a content row has its best line") {
    add_note("zeta.md", "x\n", 50);
    add_note("other.md", "zeta burada\n", 60);
    auto state = make_state();
    auto view = make_view(*state);
    state->set_search_mode(SearchMode::Global);
    state->set_query("zeta");
    settle(*view);
    const auto& rects = view->item_rects();
    REQUIRE(rects.contains("row:zeta.md"));
    REQUIRE(rects.contains("row:other.md"));
    const auto height = [&](const char* name) { return rects.at(name).max_y - rects.at(name).min_y; };
    // Yalnız adı eşleşen satırın altında boş ikinci satır kalmasın.
    CHECK(height("row:zeta.md") < height("row:other.md") * 0.75F);
}

// NY-G inceleme: genel aramada Enter de bulunan notu incelemede bırakır (odak önizlemeye).
TEST_CASE_FIXTURE(SearchFixture, "Enter in a global search keeps the found note in review") {
    auto state = make_state();
    auto view = make_view(*state);
    state->set_search_mode(SearchMode::Global);
    state->set_query("plan");
    REQUIRE(state->current_path().has_value());
    state->search_enter();
    settle(*view);
    CHECK(state->reviewing());
    CHECK_FALSE(view->editor_active());
}
