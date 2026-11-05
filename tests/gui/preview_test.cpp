// T82: md notunun salt-okunur Markdown önizlemesi. Ayrıştırma PDF ile aynı md4c yürüyüşüdür (blok çıktısı
// tests/pdf/markdown_blocks_test.cpp'de); burada model kuralları ve gerçek çizim katmanındaki sahne sınanır.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <cfloat>
#include <doctest/doctest.h>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <string_view>

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

// Önizlemenin çocuk penceresi (adı "…/##preview<sekme>_…"): kaydırması yalnız ImGui'de durur.
ImGuiWindow* preview_window() {
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
        if (window->Active && std::string_view(window->Name).find("/##preview") != std::string_view::npos)
            return window;
    return nullptr;
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

// NX-B (L1): sınırın üstündeki not reddedilmez; TUI REVIEW gibi ham metin ve nedeni (anahtarıyla) görünür.
TEST_CASE("gui preview: a note over limits.tui.view_bytes previews as raw text with the TUI's notice") {
    WindowFixture fixture;
    fixture.config.limits.tui.view_bytes = {16, "16"};
    fixture.add_note("büyük.md", "# Başlık\n\nuzun bir paragraf satırı\n", 10);
    fixture.add_note("sınır.md", "# Kısa\n", 20);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "büyük.md");
    REQUIRE(state->can_preview());
    CHECK(state->preview_too_large());
    state->toggle_preview();
    settle(*view);
    const RenderedText text;
    settle(*view);
    REQUIRE(view->item_rects().contains("preview"));
    CHECK(text.contains(core::tr("gui.preview_too_large", {{"limit", core::human_size(16)}})));
    CHECK(text.contains("# Başlık"));  // biçimlenmemiş: başlık işareti görünür
    CHECK(text.contains("uzun bir paragraf satırı"));
    select(*state, "sınır.md");
    CHECK_FALSE(state->preview_too_large());
}

// Ham önizlemede uzun satır parçalara bölünür: her bayt çizilir, çok baytlı karakter bölünmez.
TEST_CASE("gui preview: a long raw line is drawn in 4 KiB rows without losing or splitting a character") {
    WindowFixture fixture;
    fixture.config.limits.tui.view_bytes = {16, "16"};
    fixture.add_note("uzun.md", std::string(4095, 'a') + "ş" + std::string(10, 'b') + "SON\nikinci\n", 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "uzun.md");
    state->toggle_preview();
    settle(*view);
    const RenderedText text;
    settle(*view);
    REQUIRE(view->item_rects().contains("preview"));
    // ImGui günlüğü satırları IM_NEWLINE ile ayırır (Windows'ta "\r\n"): parçanın sonu platformun satır sonudur.
    CHECK(text.contains(std::string(4095, 'a') + IM_NEWLINE));  // ilk parça ş'den önce biter
    CHECK(text.contains("şbbbbbbbbbbSON"));
    CHECK(text.contains("ikinci"));
}

// Kaydırılmış ham önizleme: ImGuiListClipper görünen aralığa satırları atlayarak gelir; bölünmüş uzun satır iki
// görüntü satırı sayılmazsa sondaki satırlar kayardı. ImGui günlüğü açıkken kırpıcı kırpmaz (RenderedText bu yolu
// görmez): çizilen satırlar `preview_raw:` kancalarından okunur.
TEST_CASE("gui preview: a scrolled raw preview skips to the visible rows, counting split long lines") {
    constexpr int kLines = 2000;
    constexpr int kLongLine = 1990;
    WindowFixture fixture;
    fixture.config.limits.tui.view_bytes = {16, "16"};
    std::string note;
    for (int line = 0; line < kLines; ++line) {
        note += line == kLongLine ? std::string(4096, 'x') + "UÇ" : std::format("satır {:04}", line);
        note += '\n';
    }
    note.pop_back();
    fixture.add_note("uzun.md", note, 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "uzun.md");
    state->toggle_preview();
    settle(*view);
    state->scroll_preview(FLT_MAX);
    settle(*view);
    const auto& rects = view->item_rects();
    const auto top_of = [&](const std::string& row) {
        const auto found = rects.find("preview_raw:" + row);
        REQUIRE_MESSAGE(found != rects.end(), row);
        return found->second.min_y;
    };
    CHECK_FALSE(rects.contains("preview_raw:satır 1000"));
    // Bölünen satır iki görüntü satırıdır ve ardışık: x'ler, "UÇ", sonraki satır.
    CHECK(top_of("satır 1989") < top_of(std::string(32, 'x')));
    CHECK(top_of(std::string(32, 'x')) < top_of("UÇ"));
    CHECK(top_of("UÇ") < top_of("satır 1991"));
    // Son satır en altta çizilir: atlama bir satır kaysaydı ya çizilmez ya da yerinde başka satır olurdu.
    float lowest = 0.0F;
    for (const auto& [name, rect] : rects)
        if (name.starts_with("preview_raw:"))
            lowest = std::max(lowest, rect.min_y);
    CHECK(top_of("satır 1999") == lowest);
    // Sona kaydırılmış: son satır pencerenin içinde biter (atlama bir satır az olsaydı bir satır aşağıda kalırdı).
    REQUIRE(rects.contains("preview"));
    CHECK(rects.at("preview_raw:satır 1999").max_y <= rects.at("preview").max_y);
}

// NX-D: sondan yukarı her karede küçük bir klavye isteği (görünen alandan kısa). Son parça kısa paragraf, üstündeki
// ölçülmemiş kod parçası (tahmini ilk paragraf parçasından, tutmaz). Sınır geçilirken görünen son parça çapadır,
// üstteki ölçülür ve kaydırma düzeltilir; o karenin isteği düzeltmeye eklenir, kaybolmaz. Sondan uzaklık bu
// düzeltmeyle değişmez: N istekten sonra N adım. Sona atlama, ölçüm toplamı değiştirse de sonda kalır.
TEST_CASE("gui preview: a keyboard scroll in the frame of an anchor correction is kept") {
    constexpr float kLines = 10.0F;
    constexpr int kSteps = 40;
    constexpr std::size_t kChunk = 64 * 1024;
    const std::string prose = "## Bölüm\n\nBir paragraf *vurgu* ve **kalın** metin; satır sarılsın diye biraz uzun "
                              "yazılmış bir cümle daha.\n\n";
    const std::string code = "```\nkod\nkod\nkod\nkod\n```\n\n";
    std::string note;
    while (note.size() < kChunk)
        note += prose;
    while (note.size() < 3 * kChunk)
        note += code;
    while (note.size() < (3 * kChunk) + 3000)
        note += prose;
    WindowFixture fixture;
    fixture.config.limits.tui.view_bytes = {0, "0"};
    fixture.add_note("büyük.md", note, 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "büyük.md");
    state->toggle_preview();
    settle(*view);
    state->scroll_preview(FLT_MAX);
    settle(*view);
    const auto from_bottom = [] {
        const ImGuiWindow* preview = preview_window();
        REQUIRE(preview != nullptr);
        return preview->ScrollMax.y - preview->Scroll.y;
    };
    CHECK(from_bottom() == 0.0F);
    state->scroll_preview(-kLines);
    settle(*view);
    const float step = from_bottom();
    REQUIRE(step > 0.0F);
    for (int i = 1; i < kSteps; ++i) {
        state->scroll_preview(-kLines);
        view->draw_frame();
    }
    settle(*view);
    CHECK(from_bottom() == doctest::Approx(step * kSteps).epsilon(0.005));
}

// NX-E H2: geniş kod bloğu yatay kaydırma çubuğu açar, görünen yükseklik onun kadar kısalır: End yine en alta iner.
TEST_CASE("gui preview: End reaches the bottom when a wide code block adds a horizontal scrollbar") {
    WindowFixture fixture;
    std::string note = "```\n" + std::string(2000, 'x') + "\n```\n\n";
    for (int i = 0; i < 200; ++i)
        note += "Paragraf " + std::to_string(i) + "\n\n";
    fixture.add_note("geniş.md", note, 10);
    const auto state = fixture.make_state();
    const auto view = make_view(*state);
    select(*state, "geniş.md");
    state->toggle_preview();
    settle(*view);
    state->scroll_preview(FLT_MAX);
    settle(*view);
    const ImGuiWindow* preview = preview_window();
    REQUIRE(preview != nullptr);
    REQUIRE(preview->ScrollbarX);
    CHECK(preview->ScrollMax.y > 0.0F);
    CHECK(preview->Scroll.y == preview->ScrollMax.y);
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
    CHECK(state->can_preview());  // reddedilmez: ham metin ve neden (önceki testte)
    CHECK(state->preview_too_large());
    select(*state, "sınır.json");
    CHECK(state->can_preview());
    CHECK_FALSE(state->preview_too_large());
}
