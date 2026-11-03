// T82: md notunun salt-okunur Markdown önizlemesi. Ayrıştırma PDF ile aynı md4c yürüyüşüdür (blok çıktısı
// tests/pdf/markdown_blocks_test.cpp'de); burada model kuralları ve gerçek çizim katmanındaki sahne sınanır.
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <doctest/doctest.h>
#include <memory>
#include <string>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::settle;
using gui_test::View;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;

namespace core = noteapp::core;

namespace {

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

constexpr char kSample[] = "# Başlık\n\n"
                           "Giriş *vurgu* ve **kalın** metin.\n\n"
                           "- madde bir\n- madde iki\n\n"
                           "```\nkod satırı\n```\n\n"
                           "> alıntı satırı\n\n"
                           "[[b]] ve [site](https://x.example/y)\n\n"
                           "---\n\n"
                           "| Ad | Değer |\n|---|---|\n| şehir | İzmir |\n";

void select(GuiState& state, const std::string& path) {
    state.select_row(state.model().index_of(np(path)));
}

float height_of(const View& view, const std::string& name) {
    const View::ItemRect& rect = view.item_rects().at(name);
    return rect.max_y - rect.min_y;
}

}  // namespace

TEST_CASE("gui preview: only a saved markdown note previews, the switch lasts the session") {
    WindowFixture fixture;
    fixture.add_note("a.md", kSample, 10);
    fixture.add_note("t.txt", "# düz metin\n", 20);
    const auto state = fixture.make_state();
    select(*state, "t.txt");
    CHECK_FALSE(state->can_preview());
    state->toggle_preview();
    CHECK_FALSE(state->showing_preview());

    select(*state, "a.md");
    REQUIRE(state->can_preview());
    state->toggle_preview();
    CHECK(state->showing_preview());
    select(*state, "t.txt");
    CHECK_FALSE(state->showing_preview());
    select(*state, "a.md");
    CHECK(state->showing_preview());  // TUI'deki `R` gibi oturum boyunca

    state->new_note();
    CHECK_FALSE(state->showing_preview());
}

TEST_CASE("gui preview: a .markdown note previews like .md") {
    WindowFixture fixture;
    fixture.add_note("uzun.markdown", kSample, 10);
    const auto state = fixture.make_state();
    select(*state, "uzun.markdown");
    CHECK(state->previewable_type());
    CHECK(state->can_preview());
}

TEST_CASE("gui preview: a note over the size bound is not previewed") {
    WindowFixture fixture;
    fixture.add_note("büyük.md", std::string(fixture.config.limits.tui.view_bytes.cap() + 1, 'x'), 10);
    fixture.add_note("sınır.md", std::string(fixture.config.limits.tui.view_bytes.cap(), 'x'), 20);
    const auto state = fixture.make_state();
    select(*state, "büyük.md");
    CHECK_FALSE(state->can_preview());
    select(*state, "sınır.md");
    CHECK(state->can_preview());
}

TEST_CASE("gui preview: Ctrl+Shift+V renders headings, lists, code, quotes and links instead of the editor") {
    WindowFixture fixture;
    fixture.add_note("a.md", kSample, 10);
    fixture.add_note("b.md", "b\n", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "a.md");
    settle(*view);
    REQUIRE(view->item_rects().contains("preview_toggle"));
    CHECK_FALSE(view->item_rects().contains("preview"));

    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V);
    settle(*view);
    const RenderedText text;  // editör de metnini günlüğe yazar: yalnız önizleme karelerinin metni
    settle(*view);
    const auto& rects = view->item_rects();
    REQUIRE(rects.contains("preview"));
    CHECK_FALSE(rects.contains("editor"));
    for (const char* shown : {"Başlık", "vurgu", "kalın metin", "madde bir", "madde iki", "kod satırı",
                              "alıntı satırı", "b ve", "site", "şehir", "İzmir"})
        CHECK_MESSAGE(text.contains(shown), shown);
    // Kaynak sözdizimi görünmez: başlık işareti, çit, wiki ayraçları, tablo çizgisi, hedef URL.
    for (const char* hidden : {"# Başlık", "```", "[[b]]", "|---", "x.example"})
        CHECK_MESSAGE(!text.contains(hidden), hidden);
    CHECK(rects.contains("preview_link:b"));
    // Başlık büyük yazıyla: bloğu paragraftan yüksek.
    CHECK(height_of(*view, "preview_block:0") > height_of(*view, "preview_block:1"));
    CHECK(state->buffer() == kSample);  // salt okunur

    press(*view, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V);
    settle(*view);
    CHECK(view->item_rects().contains("editor"));
    CHECK_FALSE(view->item_rects().contains("preview"));
    CHECK(view->editor_active());  // düzenlemeye dönünce imleç editörde
}

TEST_CASE("gui preview: the header button switches between preview and edit") {
    WindowFixture fixture;
    fixture.add_note("a.md", kSample, 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "a.md");
    settle(*view);
    click_button(*view, "preview_toggle");
    settle(*view);
    CHECK(state->showing_preview());
    CHECK(view->item_rects().contains("preview"));
    click_button(*view, "preview_toggle");
    settle(*view);
    CHECK_FALSE(state->showing_preview());
}

TEST_CASE("gui preview: clicking a [[link]] saves the pending edit to its own note, then opens the target") {
    WindowFixture fixture;
    fixture.add_note("a.md", "[[b]]\n", 10);
    fixture.add_note("b.md", "b\n", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "a.md");
    state->edit_text("[[b]] değişti\n");  // autosave bekliyor
    REQUIRE(state->dirty());
    state->toggle_preview();
    settle(*view);
    REQUIRE(view->item_rects().contains("preview_link:b"));

    click_button(*view, "preview_link:b");
    settle(*view);
    REQUIRE(state->current_path().has_value());
    CHECK(state->current_path()->string() == "b.md");
    CHECK(fixture.read_note("a.md") == "[[b]] değişti\n");
    CHECK(fixture.read_note("b.md") == "b\n");
    CHECK(state->buffer() == "b\n");
    CHECK_FALSE(state->dirty());
}

TEST_CASE("gui preview: a target that resolves to no note reports it and stays") {
    WindowFixture fixture;
    fixture.add_note("a.md", "[[yok]]\n", 10);
    const auto state = fixture.make_state();
    state->open_wiki_link("yok");
    CHECK(state->status().find("yok") != std::string::npos);
    select(*state, "a.md");
    state->open_wiki_link("yok");
    CHECK(state->current_path()->string() == "a.md");
    CHECK(state->status().find("yok") != std::string::npos);
}

TEST_CASE("gui preview: a missing [[target]] cannot be clicked until the note exists") {
    WindowFixture fixture;
    fixture.add_note("a.md", "[[b]] ve [[yok]]\n", 10);
    fixture.add_note("b.md", "b\n", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "a.md");
    state->toggle_preview();
    settle(*view);
    CHECK(view->item_rects().contains("preview_link:b"));
    CHECK_FALSE(view->item_rects().contains("preview_link:yok"));

    fixture.add_note("yok.md", "artık var\n", 1);  // senkron ya da başka pencere oluşturdu
    state->rescan();
    settle(*view);
    CHECK(view->item_rects().contains("preview_link:yok"));
}

// G4 (matris 55–57): CSV ve JSON önizlemesi TUI REVIEW'un ayrıştırmasını kullanır (app::parse_csv,
// app::pretty_json); sınırlar aynı anahtarlardır (limits.tui.view_bytes, table_rows, table_columns,
// view_lines).
TEST_CASE("gui preview: a csv note previews as a table with the detected delimiter") {
    WindowFixture fixture;
    fixture.add_note("veri.csv", "ad;yaş;şehir\nÇağrı;30;\"İstanbul; merkez\"\nAli;4;Van\n", 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "veri.csv");
    CHECK(state->previewable_type());
    REQUIRE(state->can_preview());
    state->toggle_preview();
    settle(*view);
    REQUIRE(state->showing_preview());
    const RenderedText text;
    settle(*view);
    CHECK(view->item_rects().contains("preview_table"));
    CHECK_FALSE(view->item_rects().contains("editor"));
    for (const char* shown : {"ad", "yaş", "şehir", "Çağrı", "İstanbul; merkez", "Ali", "Van"})
        CHECK_MESSAGE(text.contains(shown), shown);
    CHECK_FALSE(text.contains("\"İstanbul"));      // tırnak açıldı
    CHECK(state->buffer().starts_with("ad;yaş"));  // salt okunur
}

TEST_CASE("gui preview: table rows and columns follow limits.tui and say what they cut") {
    WindowFixture fixture;
    fixture.config.limits.tui.table_rows = {2, "2"};
    fixture.config.limits.tui.table_columns = {2, "2"};
    fixture.add_note("büyük.csv", "kol_a,kol_b,kol_c\nv11,v12,v13\nv21,v22,v23\nv31,v32,v33\n", 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "büyük.csv");
    state->toggle_preview();
    settle(*view);
    const RenderedText text;
    settle(*view);
    CHECK(text.contains("v12"));
    CHECK_FALSE(text.contains("v21"));
    CHECK_FALSE(text.contains("kol_c"));
    CHECK(text.contains(noteapp::core::tr("gui.preview_rows_cut", {{"count", "2"}})));
    CHECK(text.contains(noteapp::core::tr("gui.preview_columns_cut", {{"count", "1"}})));
}

TEST_CASE("gui preview: a csv that is no table shows its raw text and why") {
    WindowFixture fixture;
    fixture.add_note("liste.csv", "tek sütun\nbaşka satır\n", 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "liste.csv");
    state->toggle_preview();
    settle(*view);
    const RenderedText text;
    settle(*view);
    CHECK(text.contains(noteapp::core::tr("gui.preview_not_table")));
    CHECK(text.contains("başka satır"));
}

TEST_CASE("gui preview: a json note is pretty printed by depth; invalid json shows raw text and why") {
    WindowFixture fixture;
    fixture.add_note("veri.json", "{\"ad\":\"Çağrı\",\"liste\":[1,{}]}", 10);
    fixture.add_note("bozuk.json", "{\"a\":}", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "veri.json");
    REQUIRE(state->can_preview());
    state->toggle_preview();
    settle(*view);
    {
        const RenderedText text;
        settle(*view);
        for (const char* shown : {"\"ad\"", "\"Çağrı\"", "\"liste\""})
            CHECK_MESSAGE(text.contains(shown), shown);
    }
    const auto& rects = view->item_rects();
    REQUIRE(rects.contains("preview_json:1"));  // "ad": derinlik 1
    REQUIRE(rects.contains("preview_json:3"));  // 1, : derinlik 2
    CHECK(rects.at("preview_json:3").min_x > rects.at("preview_json:1").min_x);
    CHECK(rects.at("preview_json:1").min_x > rects.at("preview_json:0").min_x);

    select(*state, "bozuk.json");
    settle(*view);
    REQUIRE(state->showing_preview());
    const RenderedText text;
    settle(*view);
    CHECK(text.contains(noteapp::core::tr("gui.preview_invalid_json")));
    CHECK(text.contains("{\"a\":}"));
}

TEST_CASE("gui preview: limits.tui.view_bytes bounds every previewed type") {
    WindowFixture fixture;
    fixture.config.limits.tui.view_bytes = {16, "16"};
    fixture.add_note("sınır.json", std::string("[") + std::string(14, '1') + "]", 10);
    fixture.add_note("büyük.json", std::string("[") + std::string(15, '1') + "]", 20);
    const auto state = fixture.make_state();
    select(*state, "büyük.json");
    CHECK(state->previewable_type());
    CHECK_FALSE(state->can_preview());
    select(*state, "sınır.json");
    CHECK(state->can_preview());
}
