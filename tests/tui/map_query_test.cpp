// F3: TUI haritasında sorgu (SPEC-tui "Harita" → "Sorgu"). Akış GUI ile ortaktır (app::MapQuery); burada TUI'nin
// tuşları gerçek depoyla: `Q` tek satırlık sorgu (geçmiş ↑↓, Tab görünüm adları), sonuç braille haritada vurgu,
// `O` liste panelinde tablo, yazmada onay (varsayılan Hayır), `Esc` sonucu kaldırır.
#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "core/note_query.hpp"
#include "core/note_views.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 120;
constexpr int kHeight = 30;
constexpr int kSettleSteps = 5000;

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

// a → b → c (bağlantı), d ilgisiz; bütün çizge.
struct Scene {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { refresh(); }, [] {}}};
    Intent last = Intent::None;

    Scene() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "c.md", "c\n"));
        REQUIRE(test::seed_note(*store, "b.md", "[[c]]\n"));
        REQUIRE(test::seed_note(*store, "d.md", "d\n"));
        REQUIRE(test::seed_note(*store, "a.md", "[[b]]\n"));
        REQUIRE(core::save_view(*store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
        state.set_window(kWidth, kHeight);
        refresh();
        REQUIRE(state.select_note(np("a.md")));
        for (const tui::Command& command : tui::TuiState::commands())
            if (command.id == "view.map" && (command.contexts & tui::kInList) != 0)
                command.run(state);
        REQUIRE(state.map_view());
        key(Key::Char, "w");  // bütün çizge
        settle();
    }
    void refresh() {
        preview.invalidate();
        state.set_notes(store->list());
        state.set_folders(store->folders());
        state.set_views(app::view_names(*store));
        state.map_edges_changed();
    }
    void update() { state.update_map([this] { return store->edges(); }, np("a.md")); }
    void settle() {
        update();
        for (int i = 0; i < kSettleSteps && state.step_map(); ++i) {
        }
    }
    void key(Key key, std::string_view text = {}) {
        last = state.handle(key, text);
        if (last != Intent::None && last != Intent::OpenMapNode)
            actions.run(last, confirm, rename);
        update();
    }
    void type(std::string_view text) {
        state.handle(Key::Paste, text);
        update();
    }
    void query(std::string_view text) {
        key(Key::Char, "Q");
        REQUIRE(state.map_input_kind() == tui::TuiState::MapInputKind::Query);
        type(text);
        key(Key::Enter);
    }
    // Onay penceresi main'in yolundan: Evet için sola geçip Enter (varsayılan Hayır).
    void answer(bool yes) {
        REQUIRE(confirm.has_value());
        if (yes)
            REQUIRE(confirm->handle(Key::Left) == tui::ConfirmState::Outcome::Pending);
        if (confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed)
            actions.finish_delete(confirm);
        else
            confirm.reset();
        update();
    }
    bool lit(const std::string& key) const {
        const auto& nodes = state.map_model().map().nodes;
        const auto found = std::ranges::find(nodes, key, &app::MapNode::key);
        REQUIRE(found != nodes.end());
        const auto& mask = state.map_model().highlighted_nodes();
        const auto index = static_cast<std::size_t>(found - nodes.begin());
        REQUIRE(index < mask.size());
        return mask[index] != 0;
    }
    std::size_t count(std::string_view cypher) {
        const auto done = core::run_user_query(*store, cypher, core::QueryAction::Read, {});
        REQUIRE(done.has_value());
        return done->result.rows.size();
    }
    std::string screen() const {
        auto shot = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(shot, tui::render(state, {}, status, {kWidth, kHeight}, nullptr,
                                        confirm ? &*confirm : nullptr));
        std::string out;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x)
                out += shot.PixelAt(x, y).character;
            out += "\n";
        }
        return out;
    }
};

}  // namespace

TEST_CASE("map query: Q opens a query line; Enter runs a read that lights its notes; Esc clears the result") {
    Scene s;
    s.query("MATCH (n:Note) WHERE n.name = 'b' RETURN n");
    CHECK_FALSE(s.state.map_input_kind().has_value());
    REQUIRE(s.state.map_query().outcome().has_value());
    CHECK(s.state.map_query().outcome()->highlight.notes == std::set<std::string>{"b.md"});
    CHECK(s.lit("b.md"));
    CHECK_FALSE(s.lit("a.md"));
    CHECK(s.status.message == s.state.map_query().outcome()->summary);
    CHECK(s.screen().find("O table") != std::string::npos);  // kapsam satırı sonucun tuşlarını söyler

    s.key(Key::Escape);
    CHECK_FALSE(s.state.map_query().outcome().has_value());
    CHECK(s.state.map_model().highlighted_nodes().empty());
}

TEST_CASE("map query: Up and Down walk the history; Tab completes a saved view's name and Enter runs it") {
    Scene s;
    s.query("MATCH (n:Note) RETURN n");
    s.query("MATCH (n:Note) WHERE n.name = 'd' RETURN n");
    s.key(Key::Char, "Q");
    s.key(Key::Up);
    CHECK(s.state.map_input().text() == "MATCH (n:Note) WHERE n.name = 'd' RETURN n");
    s.key(Key::Up);
    CHECK(s.state.map_input().text() == "MATCH (n:Note) RETURN n");
    s.key(Key::Down);
    CHECK(s.state.map_input().text() == "MATCH (n:Note) WHERE n.name = 'd' RETURN n");
    s.key(Key::Escape);
    CHECK_FALSE(s.state.map_input_kind().has_value());

    s.key(Key::Char, "Q");
    s.type("lin");
    s.key(Key::Tab);  // tek aday: doğrudan yazılır
    CHECK(s.state.map_input().text() == "linked");
    s.key(Key::Enter);
    REQUIRE(s.state.map_query().outcome().has_value());
    CHECK(s.state.map_query().outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md", "c.md"});
    // Yazılan metin görünüm olarak çözüldü: alt satır ve kapsam satırı bunu adıyla söyler.
    CHECK(s.status.message == core::tr("map.query_view", {{"name", "linked"}, {"rows", "2"}, {"notes", "3"}}));
    CHECK(s.screen().find("linked") != std::string::npos);
}

// Güvenlik incelemesi (F3, Sev-2): baştaki boşluklu senkron görünüm adı (` MATCH … SET …`) Tab'la satıra
// gelirdi; metin kırpıldığı için görünüm olarak bulunmaz, kullanıcının yazması olarak onaya giderdi. Ad kuralı
// baştaki boşluğu reddeder: görünüm listelenmez, Tab tamamlamaz, yazma onayı hiç açılmaz.
TEST_CASE("map query: a synced view named with a leading space is not offered by Tab and never runs as a write") {
    Scene s;
    {
        auto raw = graph::Graph::open(s.dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("CREATE (:View {name: \" MATCH (n) SET n.status = 'x'\", query: 'MATCH (n) RETURN n'})",
                           {}, graph::QueryMode::ReadWrite)
                    .has_value());
    }
    s.refresh();
    s.key(Key::Char, "Q");
    s.key(Key::Tab);  // boş satırda Tab bütün görünüm adlarını listeler; ilki (sıralı) seçilir
    if (s.state.map_input().popup() != nullptr)
        s.key(Key::Enter);
    CHECK(s.state.map_input().text() != " MATCH (n) SET n.status = 'x'");
    s.key(Key::Enter);
    CHECK_FALSE(s.confirm.has_value());
    CHECK_FALSE(s.state.map_query().confirming());
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 0);
}

TEST_CASE("map query: a write asks first with No selected; Yes applies it and the map shows the change") {
    Scene s;
    // İlişki ham Cypher'la kurulmaz (`C` ile kurulur); var olanın adı yazılır.
    REQUIRE(s.store->add_relation(np("a.md"), np("c.md"), {"p", false}).has_value());
    s.refresh();
    const std::string relate = "MATCH ()-[r:RELATED_TO]->() SET r.name = 'q'";
    s.query(relate);
    REQUIRE(s.confirm.has_value());
    CHECK(s.confirm->message() == s.state.map_query().confirm_text());
    CHECK_FALSE(s.confirm->yes_selected());
    CHECK(s.screen().find(core::tr("app.query_apply")) != std::string::npos);
    s.answer(false);
    CHECK(s.count("MATCH ()-[r:RELATED_TO]->() WHERE r.name = 'q' RETURN r") == 0);

    // Vazgeçilen metin `Q` ile geri gelir.
    s.key(Key::Char, "Q");
    CHECK(s.state.map_input().text() == relate);
    s.key(Key::Enter);
    s.answer(true);
    CHECK(s.count("MATCH ()-[r:RELATED_TO]->() WHERE r.name = 'q' RETURN r") == 1);
    CHECK(std::ranges::any_of(s.state.map_model().map().edges, [](const app::MapEdge& edge) {
        return edge.kind == app::MapEdgeKind::Related && edge.name == "q";
    }));
    REQUIRE(s.state.map_query().outcome().has_value());
    CHECK(s.state.map_query().outcome()->wrote);

    // Korunan veri onay sormadan reddedilir: satır metinle ve konumlu hatayla yeniden açılır.
    s.query("MATCH (n:Note) SET n.body = 'x'");
    CHECK_FALSE(s.confirm.has_value());
    CHECK(s.state.map_input_kind() == tui::TuiState::MapInputKind::Query);
    CHECK(s.state.map_query().error().find("body") != std::string::npos);
    CHECK(s.screen().find("body") != std::string::npos);
    // Dar panelde kesilebilir: tamamı alt satırda (e2e'de görüldü).
    CHECK(s.status.message == s.state.map_query().error());
    CHECK(s.store->read(np("b.md"))->text == "[[c]]\n");
}

TEST_CASE("map query: O shows a table in the list pane; Down and Enter open a row's note; hostile text is safe") {
    Scene s;
    REQUIRE(core::run_user_query(*s.store, "MATCH (n:Note {name: 'b'}) SET n.title = 'ab\\u202Ecd\\nef'",
                                 core::QueryAction::Write, {}));
    s.query("MATCH (n:Note) RETURN n, n.title AS title ORDER BY n.name");
    s.key(Key::Char, "O");
    REQUIRE(s.state.map_query_table());
    const std::string shown = s.screen();
    CHECK(shown.find("title") != std::string::npos);
    CHECK(shown.find("a.md") != std::string::npos);
    CHECK(shown.find("\xE2\x80\xAE") == std::string::npos);
    CHECK(shown.find("ab\xEF\xBF\xBD" "cd") != std::string::npos);

    s.key(Key::Down);
    CHECK(s.state.map_query_row() == 1);
    s.key(Key::Enter);
    CHECK(s.last == Intent::OpenMapNode);
    CHECK(s.state.map_selected_key() == "b.md");

    s.key(Key::Char, "O");
    CHECK_FALSE(s.state.map_query_table());
}

namespace {

void type_into(std::optional<tui::RenameState>& rename, std::string_view text) {
    REQUIRE(rename.has_value());
    for (const char c : text)
        rename->handle(Key::Char, std::string(1, c));
}

}  // namespace

// SD11b: kayıtlı görünüm TUI'de de (palet view.save / view.remove, haritada K); kural GUI ve CLI ile aynı.
TEST_CASE("map query: K saves the shown read as a view; view.remove deletes one after a second Enter") {
    Scene s;
    s.key(Key::Char, "K");  // sonuç yok: ileti, girdi açılmaz
    CHECK_FALSE(s.rename.has_value());
    CHECK(s.status.message == core::tr("error.view_needs_query"));

    const std::string text = "MATCH (n:Note) WHERE n.name = 'b' RETURN n";
    s.query(text);
    s.key(Key::Char, "K");
    REQUIRE(s.rename.has_value());
    CHECK(s.rename->is_view());
    type_into(s.rename, " mine");
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    REQUIRE(s.rename.has_value());  // baş boşluk core kuralıyla reddedilir, kırpılmaz; girdi açık kalır
    CHECK_FALSE(s.rename->error().empty());
    CHECK_FALSE(core::find_view(*s.store, " mine"));
    s.actions.handle_rename_key(s.rename, Key::Escape, {});

    s.key(Key::Char, "K");
    type_into(s.rename, "mine");
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    CHECK_FALSE(s.rename.has_value());
    REQUIRE(core::find_view(*s.store, "mine"));
    CHECK(core::find_view(*s.store, "mine")->query == text);
    CHECK(std::ranges::contains(s.state.views(), "mine"));  // Tab ve `view:` hemen görür

    for (const tui::Command& command : tui::TuiState::commands())
        if (command.id == "view.remove")
            s.last = command.run(s.state);
    REQUIRE(s.last == Intent::RemoveView);
    s.actions.run(s.last, s.confirm, s.rename);
    type_into(s.rename, "mi");
    s.rename->handle(Key::Tab);  // kayıtlı adlar
    CHECK(s.rename->text() == "mine");
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    REQUIRE(s.rename.has_value());  // ilk Enter uyarır, silmez
    CHECK(core::find_view(*s.store, "mine"));
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    CHECK_FALSE(s.rename.has_value());
    CHECK_FALSE(core::find_view(*s.store, "mine"));
    CHECK_FALSE(std::ranges::contains(s.state.views(), "mine"));

    // SD11b review: uyarıdan sonra senkron aynı adı başka sorguyla yeniden kurarsa ikinci Enter onu silmez.
    s.actions.run(Intent::RemoveView, s.confirm, s.rename);
    type_into(s.rename, "linked");
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    REQUIRE(core::save_view(*s.store, "linked", text, true));
    s.actions.handle_rename_key(s.rename, Key::Enter, {});
    REQUIRE(s.rename.has_value());
    CHECK_FALSE(s.rename->error().empty());
    REQUIRE(core::find_view(*s.store, "linked"));
    CHECK(core::find_view(*s.store, "linked")->query == text);
}
