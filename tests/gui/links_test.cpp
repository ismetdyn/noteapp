// T78: editörün altındaki bağlantı paneli. Model saf; panel gerçek çizim katmanıyla, fare ve kısayol gerçek
// ImGui olaylarıyla sınanır.
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

// a.md: giden b.md, gelen c.md, eksik [[yok]].
struct LinksScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    LinksScene() {
        fixture.add_note("b.md", "b\n", 30);
        fixture.add_note("a.md", "[[b]] [[yok]]\n", 20);
        fixture.add_note("c.md", "[[a]]\n", 10);
        fixture.add_note("hiç.md", "bağlantısız\n", 5);
    }
    void open() {
        state = fixture.make_state();
        view = make_view(*state);
        state->select_row(state->model().index_of(np("a.md")));
        settle(*view);
    }
    bool shows(const std::string& name) const { return view->item_rects().contains(name); }
};

}  // namespace

TEST_CASE("gui links: the model follows the shown note") {
    LinksScene scene;
    scene.state = scene.fixture.make_state();
    scene.state->select_row(scene.state->model().index_of(np("a.md")));
    CHECK(scene.state->links().outgoing.at(0).string() == "b.md");
    CHECK(scene.state->links().incoming.at(0).string() == "c.md");
    CHECK(scene.state->links().missing.at(0) == "yok");
    CHECK(scene.state->link_lines().size() == 6);
    scene.state->select_row(scene.state->model().index_of(np("hiç.md")));
    CHECK(scene.state->links().empty());
    scene.state->new_note();
    CHECK(scene.state->links().empty());
}

TEST_CASE("gui links: the header names the shown note's tags (T80)") {
    WindowFixture fixture;
    fixture.add_note("t.md", "#Proje #iş metin\n", 10);
    fixture.add_note("u.md", "etiketsiz\n", 5);
    const auto state = fixture.make_state();
    state->select_row(state->model().index_of(np("t.md")));
    CHECK(state->links().tags == std::vector<std::string>{"iş", "Proje"});
    CHECK(state->header().find("#iş #Proje") != std::string::npos);
    CHECK(state->header().find("t.md") != std::string::npos);
    state->select_row(state->model().index_of(np("u.md")));
    CHECK(state->header().find('#') == std::string::npos);
}

TEST_CASE("gui links: a saved edit and an outside change refresh the lists") {
    LinksScene scene;
    scene.state = scene.fixture.make_state();
    scene.state->select_row(scene.state->model().index_of(np("hiç.md")));
    scene.state->edit_text("[[b]]\n");
    scene.state->save_now();
    REQUIRE(scene.state->links().outgoing.size() == 1);
    scene.fixture.add_note("d.md", "[[hiç]]\n");
    scene.state->rescan();
    CHECK(scene.state->links().incoming.at(0).string() == "d.md");
}

TEST_CASE("gui links: a note without links has no panel and a full-height editor") {
    LinksScene scene;
    scene.open();
    scene.state->edit_selected();  // seçilen not incelemede açılır; editörün boyu ölçülür
    settle(*scene.view);
    REQUIRE(scene.shows("links_header"));
    REQUIRE(scene.shows("links_list"));
    const float short_editor =
        scene.view->item_rects().at("editor").max_y - scene.view->item_rects().at("editor").min_y;
    CHECK(scene.view->item_rects().at("links_list").min_y >= scene.view->item_rects().at("editor").max_y);

    scene.state->select_row(scene.state->model().index_of(np("hiç.md")));
    scene.state->edit_selected();
    settle(*scene.view);
    CHECK_FALSE(scene.shows("links_header"));
    const float full_editor =
        scene.view->item_rects().at("editor").max_y - scene.view->item_rects().at("editor").min_y;
    CHECK(full_editor > short_editor);
}

TEST_CASE("gui links: Ctrl+L collapses and opens the panel, also from the editor") {
    LinksScene scene;
    scene.open();
    REQUIRE(scene.shows("links_list"));
    scene.state->edit_selected();  // seçilen not incelemede açılır
    scene.state->request_focus(noteapp::gui::FocusRequest::Editor);
    settle(*scene.view);
    REQUIRE(scene.view->editor_active());
    const std::string before = scene.state->buffer();

    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_L);
    settle(*scene.view);
    CHECK_FALSE(scene.state->links_open());
    CHECK(scene.shows("links_header"));
    CHECK_FALSE(scene.shows("links_list"));
    CHECK(scene.state->buffer() == before);

    press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_L);
    settle(*scene.view);
    CHECK(scene.state->links_open());
    CHECK(scene.shows("links_list"));
}

TEST_CASE("gui links: clicking the header collapses the panel") {
    LinksScene scene;
    scene.open();
    click_button(*scene.view, "links_header");
    settle(*scene.view);
    CHECK_FALSE(scene.state->links_open());
    CHECK_FALSE(scene.shows("links_list"));
}

TEST_CASE("gui links: clicking a row opens that note") {
    LinksScene scene;
    scene.open();
    REQUIRE(scene.shows("link:b.md"));
    click_button(*scene.view, "link:b.md");
    settle(*scene.view);
    REQUIRE(scene.state->current_path().has_value());
    CHECK(scene.state->current_path()->string() == "b.md");
    CHECK(scene.state->links().incoming.at(0).string() == "a.md");
}

TEST_CASE("gui links: a jump flushes the buffer and clears the filter hiding the target") {
    LinksScene scene;
    // İlişki araması açıkken "a.md" araması bağlı b.md'yi de listeler; süzgeç hedefi gizlesin diye kapalı.
    scene.fixture.config.tuning.search_relations = false;
    scene.open();
    scene.state->set_query("a.md");
    settle(*scene.view);
    REQUIRE(scene.state->model().index_of(np("b.md")) < 0);
    scene.state->edit_text("[[b]] [[yok]] değişti\n");
    scene.state->open_link(np("b.md"));
    CHECK(scene.state->current_path()->string() == "b.md");
    CHECK(scene.fixture.read_note("a.md") == "[[b]] [[yok]] değişti\n");
}

TEST_CASE("gui links: a missing target has a row but nothing to click") {
    LinksScene scene;
    scene.open();
    CHECK(scene.shows("link_missing:yok"));
    click_button(*scene.view, "link_missing:yok");
    settle(*scene.view);
    CHECK(scene.state->current_path()->string() == "a.md");
}

TEST_CASE("gui links: hostile names draw as text, thousands of rows draw only the window") {
    LinksScene scene;
    std::string body = "[[%s%n##x\x1b]]\n";
    for (int i = 0; i < 3000; ++i)
        body += "[[kayıp-" + std::to_string(i) + "]] ";
    scene.fixture.add_note("a.md", body);
    scene.open();
    CHECK(scene.state->links().missing.size() == 3001);  // not başına tavan yok (L3): hepsi tutulur
    int drawn = 0;
    for (const auto& [name, rect] : scene.view->item_rects())
        drawn += name.starts_with("link_missing:") ? 1 : 0;
    CHECK(drawn > 0);
    CHECK(drawn < 40);
}

// F1: ilişkili satır yön oku ve adla; sağ tık menüsü kimlikle yeniden adlandırır, yönü çevirir, yönsüz yapar.
TEST_CASE("gui links: a related row shows its arrow and name; its menu flips, undirects and renames it") {
    LinksScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("a.md"), np("hiç.md"), {"uses", true}).has_value());
    }
    scene.open();
    REQUIRE(scene.shows("link:hiç.md"));
    {
        const gui_test::RenderedText rendered;
        settle(*scene.view);
        CHECK(rendered.contains("↗ hiç.md — uses"));
    }
    const auto menu = [&] {
        // Önceki işlemin bildirimi panelin üstünde durur: sağ tık satıra değil ona giderdi.
        std::vector<std::uint64_t> shown;
        for (const auto& notice : scene.state->notices().items())
            shown.push_back(notice.id);
        for (const std::uint64_t id : shown)
            scene.state->dismiss_notice(id);
        settle(*scene.view);
        const auto [x, y] = scene.view->button_center("link:hiç.md");
        ImGui::GetIO().AddMousePosEvent(x, y);
        scene.view->draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        scene.view->draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        settle(*scene.view);
    };
    const auto relation = [&] { return scene.state->links().related.at(0); };
    menu();
    REQUIRE(scene.shows("link_flip"));
    click_button(*scene.view, "link_flip");
    CHECK(relation().direction == core::RelationDirection::Incoming);
    CHECK(scene.state->status() == "Relation directed: hiç.md → a.md");
    menu();
    click_button(*scene.view, "link_undirect");
    CHECK(relation().direction == core::RelationDirection::Undirected);
    CHECK(scene.state->status() == "Relation made undirected: a.md and hiç.md");

    scene.view->install_deciders();
    std::size_t frame = 0;
    scene.view->on_modal_frame = [&] {
        ++frame;
        if (frame == 1)
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        if (frame == 5)
            gui_test::tap_key(ImGuiKey_Backspace);  // klavye odağıyla tüm metin seçilir ya da son harf
        if (frame >= 6 && frame <= 9)
            gui_test::tap_key(ImGuiKey_Backspace);
        if (frame == 12)
            ImGui::GetIO().AddInputCharactersUTF8("cites");
        if (frame == 15)
            gui_test::tap_key(ImGuiKey_Enter);
        if (frame == 300)
            gui_test::tap_key(ImGuiKey_Escape);
    };
    menu();
    click_button(*scene.view, "link_rename");
    CHECK(relation().name == "cites");
    CHECK(scene.state->status() == "Relation renamed to 'cites': a.md and hiç.md");
    REQUIRE_FALSE(scene.state->notices().items().empty());
    CHECK(scene.state->notices().items().back().text == "Relation renamed to 'cites': a.md and hiç.md");
}

// İnceleme: açık menü satır sırasına bağlıydı; panel yeniden okunup satırlar kayarsa başka ilişkiyi hedeflerdi.
TEST_CASE("gui links: a related row's menu stays with its relation when the rows move") {
    LinksScene scene;
    {
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("a.md"), np("hiç.md"), {"uses", true}).has_value());
    }
    scene.open();
    const auto [x, y] = scene.view->button_center("link:hiç.md");
    ImGui::GetIO().AddMousePosEvent(x, y);
    scene.view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    scene.view->draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    settle(*scene.view);
    REQUIRE(scene.shows("link_flip"));
    {  // başka cihaz (senkron): hiç.md'nin önüne yeni ilişki satırı girer
        core::NoteStore store(scene.fixture.store_dir());
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
    }
    scene.state->rescan();
    settle(*scene.view);
    REQUIRE(scene.state->links().related.size() == 2);
    if (scene.shows("link_flip"))
        click_button(*scene.view, "link_flip");
    const auto rows = core::NoteStore(scene.fixture.store_dir()).links(np("a.md")).value().related;
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].path == np("b.md"));
    CHECK(rows[0].direction == core::RelationDirection::Undirected);  // menü ona geçmedi
}
