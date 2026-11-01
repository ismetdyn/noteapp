#include "layout_scenes.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <doctest/doctest.h>
#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using gui_test::ItemRects;
using gui_test::LayoutScene;
using gui_test::View;
using noteapp::gui::Tab;

const char* const kNotesItems[] = {"search",
                                   "mode",
                                   "folder_tree",
                                   "toolbar_new_note",
                                   "toolbar_new_folder",
                                   "toolbar_delete",
                                   "toolbar_trash",
                                   "toolbar_move",
                                   "toolbar_export",
                                   "note_list",
                                   "header",
                                   "type",
                                   "editor",
                                   "count",
                                   "status",
                                   "sync"};
const char* const kBarItems[] = {"count", "status", "sync"};

struct ViewSize {
    int width;
    int height;
};

std::string describe(const std::string& name, const View::ItemRect& rect) {
    return std::format("{} [{:.0f},{:.0f} - {:.0f},{:.0f}]", name, rect.min_x, rect.min_y, rect.max_x,
                       rect.max_y);
}

bool overlaps(const View::ItemRect& a, const View::ItemRect& b) {
    return a.min_x < b.max_x && b.min_x < a.max_x && a.min_y < b.max_y && b.min_y < a.max_y;
}

// Sınırda paylaşılan kesirli piksel çakışma sayılmaz; ImGui konumları yuvarlar.
constexpr float kTolerance = 1.0F;

void check_inside_x(const ItemRects& rects, const std::string& name, const View::ItemRect& frame) {
    const auto& rect = rects.at(name);
    CHECK_MESSAGE((rect.min_x >= frame.min_x - kTolerance && rect.max_x <= frame.max_x + kTolerance),
                  describe(name, rect), " is outside horizontally ", describe("frame", frame));
}

void check_inside_y(const ItemRects& rects, const std::string& name, const View::ItemRect& frame) {
    const auto& rect = rects.at(name);
    CHECK_MESSAGE((rect.min_y >= frame.min_y - kTolerance && rect.max_y <= frame.max_y + kTolerance),
                  describe(name, rect), " is outside vertically ", describe("frame", frame));
}

void check_no_pair_overlaps(const ItemRects& rects, const std::vector<std::string>& names) {
    for (std::size_t first = 0; first < names.size(); ++first) {
        for (std::size_t second = first + 1; second < names.size(); ++second) {
            const auto& a = rects.at(names[first]);
            const auto& b = rects.at(names[second]);
            CHECK_MESSAGE(!overlaps(a, b), describe(names[first], a), " overlaps ",
                          describe(names[second], b));
        }
    }
}

View::ItemRect window_frame() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    return {0.0F, 0.0F, size.x, size.y};
}

// Ayarlar içeriği kaydırılır: y ekseninde taşma meşrudur, x ekseninde yatay kaydırma olmadığından taşan
// görünmez.
std::vector<std::string> settings_form_item_names(const ItemRects& rects) {
    std::vector<std::string> names;
    for (const auto& [name, rect] : rects) {
        if ((name.starts_with("setting") && !name.starts_with("settings_form") &&
             !name.starts_with("settings_footer") && name != "settings_unsaved" &&
             name != "settings_error") ||
            name.starts_with("token") || name == "browse" || name == "test" || name == "test_result")
            names.push_back(name);
    }
    return names;
}

// Altbilgi kaydırılmaz: en küçük pencerede de form bölgesinin altında, tam görünür.
std::vector<std::string> settings_footer_item_names(const ItemRects& rects) {
    std::vector<std::string> names = {"settings_form", "revert", "save"};
    for (const char* optional : {"settings_unsaved", "settings_error"}) {
        if (rects.contains(optional))
            names.emplace_back(optional);
    }
    return names;
}

std::vector<std::string> dialog_item_names(const ItemRects& rects) {
    std::vector<std::string> names;
    for (const auto& [name, rect] : rects) {
        if (name.starts_with("choice:") || name.starts_with("dialog_") || name.starts_with("save_"))
            names.push_back(name);
    }
    return names;
}

// Bölmeden taşan satır kırpılır, altındaki araç çubuğuyla çakışmaz: yalnız görünen kısmı denetlenir. Satırlar
// ağacın içindedir; ağacın kendisiyle eşleştirilmez.
std::vector<std::string> clip_folder_rows(ItemRects& rects) {
    const View::ItemRect tree = rects.at("folder_tree");
    std::vector<std::string> rows;
    for (auto& [name, rect] : rects) {
        if (!name.starts_with("folder:"))
            continue;
        rect.min_y = std::max(rect.min_y, tree.min_y);
        rect.max_y = std::min(rect.max_y, tree.max_y);
        if (rect.min_y < rect.max_y)
            rows.push_back(name);
    }
    return rows;
}

void check_notes_tab(const LayoutScene& scene) {
    ItemRects rects = scene.view->item_rects();
    const View::ItemRect frame = window_frame();
    std::vector<std::string> names(std::begin(kNotesItems), std::end(kNotesItems));
    for (const std::string& name : names) {
        REQUIRE_MESSAGE(rects.contains(name), name);
        check_inside_x(rects, name, frame);
        check_inside_y(rects, name, frame);
    }
    const std::vector<std::string> rows = clip_folder_rows(rects);
    REQUIRE(rows.size() >= 2);  // kök ve en az bir klasör: boş liste bu denetimi yanlış yeşil yapmasın
    for (const std::string& row : rows)
        check_inside_x(rects, row, rects.at("folder_tree"));
    std::erase(names, "folder_tree");
    names.insert(names.end(), rows.begin(), rows.end());
    check_no_pair_overlaps(rects, names);
}

void check_settings_rects(const ItemRects& rects) {
    const View::ItemRect frame = window_frame();
    REQUIRE(rects.contains("settings_form"));
    // Form kaydırılır: y ekseninde taşma meşrudur, görünür alanı form bölgesidir. Öğelerin birbirine göre
    // konumu kaydırmayla değişmez, bu yüzden kırpmadan karşılaştırılır.
    const auto form = settings_form_item_names(rects);
    REQUIRE(form.size() > 10);
    for (const std::string& name : form)
        check_inside_x(rects, name, rects.at("settings_form"));
    check_no_pair_overlaps(rects, form);

    auto fixed = settings_footer_item_names(rects);
    fixed.insert(fixed.end(), std::begin(kBarItems), std::end(kBarItems));
    for (const std::string& name : fixed) {
        REQUIRE_MESSAGE(rects.contains(name), name);
        check_inside_x(rects, name, frame);
        check_inside_y(rects, name, frame);
    }
    check_no_pair_overlaps(rects, fixed);
}

void check_settings_tab(LayoutScene& scene) {
    scene.show_tab(Tab::Settings);
    check_settings_rects(scene.view->item_rects());

    scene.fill_settings_with_longest_rows();
    const ItemRects rects = scene.view->item_rects();
    REQUIRE(rects.contains("settings_unsaved"));
    REQUIRE(rects.contains("setting_error:sync.idle_seconds"));
    REQUIRE(rects.contains("test_result"));
    REQUIRE(rects.contains("token_state"));
    check_settings_rects(rects);

    scene.state->settings().revert();
    scene.show_tab(Tab::Notes);
}

void check_dialogs(LayoutScene& scene) {
    const View::ItemRect frame = window_frame();
    for (const auto& dialog : gui_test::dialog_scenes()) {
        CAPTURE(dialog.name);
        ItemRects rects;
        scene.with_dialog([&] { dialog.open(scene); }, [&] { rects = scene.view->item_rects(); });
        REQUIRE(rects.contains("modal"));
        check_inside_x(rects, "modal", frame);
        check_inside_y(rects, "modal", frame);
        const auto names = dialog_item_names(rects);
        REQUIRE(!names.empty());
        if (!rects.contains(
                "choice:0")) {  // adlar önek süzgecinden geçer: kayıtsız düğme sessizce atlanmasın
            REQUIRE(rects.contains("dialog_ok"));
            REQUIRE(rects.contains("dialog_cancel"));
        }
        for (const std::string& name : names)
            check_inside_x(rects, name, rects.at("modal"));
        check_no_pair_overlaps(rects, names);
    }
}

SDL_Window* app_window() {
    int count = 0;
    const std::unique_ptr<SDL_Window*, decltype(&SDL_free)> windows(SDL_GetWindows(&count), &SDL_free);
    REQUIRE(count == 1);
    return windows.get()[0];
}

void resize_window(View& view, int width, int height) {
    REQUIRE(SDL_SetWindowSize(app_window(), width, height));
    gui_test::settle(view);
    gui_test::settle(view);
    REQUIRE(ImGui::GetIO().DisplaySize.x == static_cast<float>(width));
    REQUIRE(ImGui::GetIO().DisplaySize.y == static_cast<float>(height));
}

// Gerçek bileşikçi en küçük boyutu uygulamayabilir (1x'ten 2x'e taşıma): dummy sürücüde sınır kaldırılarak
// aynı durum kurulur. View ölçek değişmedikçe sınırı yeniden koymaz.
void force_window_size(View& view, int width, int height) {
    REQUIRE(SDL_SetWindowMinimumSize(app_window(), 0, 0));
    resize_window(view, width, height);
}

// Bölme kenarı ImGui'nin kendi kulpuyla sürüklenir: kullanıcı boyutlandırmasının tek gerçek yolu.
void drag(View& view, float from_x, float from_y, float delta_x, float delta_y) {
    ImGui::GetIO().AddMousePosEvent(from_x, from_y);
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    view.draw_frame();
    ImGui::GetIO().AddMousePosEvent(from_x + delta_x, from_y + delta_y);
    view.draw_frame();
    view.draw_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    gui_test::settle(view);
}

void drag_left_pane(View& view, float delta_x) {
    const View::ItemRect pane = view.item_rects().at("left_pane");
    drag(view, pane.max_x - 1.0F, (pane.min_y + pane.max_y) / 2.0F, delta_x, 0.0F);
}

void drag_folder_pane(View& view, float delta_y) {
    const View::ItemRect pane = view.item_rects().at("folder_tree");
    drag(view, (pane.min_x + pane.max_x) / 2.0F, pane.max_y - 1.0F, 0.0F, delta_y);
}

float rect_width(const View::ItemRect& rect) {
    return rect.max_x - rect.min_x;
}

float rect_height(const View::ItemRect& rect) {
    return rect.max_y - rect.min_y;
}

void check_sizes_not_negative(const ItemRects& rects) {
    for (const auto& [name, rect] : rects) {
        CAPTURE(name);
        CHECK(rect_width(rect) >= 0.0F);
        CHECK(rect_height(rect) >= 0.0F);
    }
}

// ImGui::GetFontSize kare dışında ölçeği görmez.
float font_pixels() {
    constexpr float kBaseFontPixels = 16.0F;
    return kBaseFontPixels * ImGui::GetStyle().FontScaleDpi;
}

// Negatif genişlik ImGui'de "sağdan pay" demektir: alan sessizce 1 piksele iner, görünmez olurdu.
void check_field_usable(const ItemRects& rects, const std::string& name) {
    CAPTURE(name);
    if (rects.contains(name)) {
        CHECK(rect_width(rects.at(name)) >= font_pixels());
    }
}

void check_dialogs_below_minimum(LayoutScene& scene) {
    const View::ItemRect frame = window_frame();
    for (const auto& dialog : gui_test::dialog_scenes()) {
        CAPTURE(dialog.name);
        ItemRects rects;
        scene.with_dialog([&] { dialog.open(scene); }, [&] { rects = scene.view->item_rects(); });
        REQUIRE(rects.contains("modal"));
        check_sizes_not_negative(rects);
        check_inside_x(rects, "modal", frame);
        for (const char* field : {"dialog_folder_name", "dialog_path", "save_name"})
            check_field_usable(rects, field);
    }
}

}  // namespace

// {1, 1}: en küçük pencere; istek SDL'in alt sınırına takılmalı, yerleşim yine de bozulmamalı.
TEST_CASE("no named item overlaps or leaves the window for Turkish long text at every scale") {
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        for (const ViewSize size : {ViewSize{1000, 650}, ViewSize{1, 1}}) {
            CAPTURE(scale);
            CAPTURE(size.width);
            LayoutScene scene(size.width, size.height, scale);
            check_notes_tab(scene);
            check_settings_tab(scene);
            check_dialogs(scene);
        }
    }
}

TEST_CASE("the trash view and the undo button fit at every scale, also in the smallest window") {
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        for (const ViewSize size : {ViewSize{1000, 650}, ViewSize{1, 1}}) {
            CAPTURE(scale);
            CAPTURE(size.width);
            LayoutScene scene(size.width, size.height, scale);
            scene.show_trash_view();
            const ItemRects rects = scene.view->item_rects();
            const View::ItemRect frame = window_frame();
            const View::ItemRect& left = rects.at("left_pane");
            std::vector<std::string> names = {"toolbar_trash", "trash_title", "trash_restore", "trash_purge",
                                              "trash_empty",   "trash_back",  "note_list"};
            for (const std::string& name : names) {
                REQUIRE_MESSAGE(rects.contains(name), name);
                check_inside_x(rects, name, left);
                check_inside_y(rects, name, frame);
            }
            check_no_pair_overlaps(rects, names);
            CHECK(rects.at("note_list").max_y - rects.at("note_list").min_y >= ImGui::GetFrameHeight());
            std::vector<std::string> bar = {"count", "status", "status_undo", "sync"};
            for (const std::string& name : bar) {
                REQUIRE_MESSAGE(rects.contains(name), name);
                check_inside_x(rects, name, frame);
            }
            check_no_pair_overlaps(rects, bar);
        }
    }
}

TEST_CASE("dialogs are opaque so text behind them cannot show through") {
    LayoutScene scene(1000, 650, 1.0F);
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w == 1.0F);
}

TEST_CASE("ui scale divides out the pixel density the framebuffer already covers") {
    CHECK(View::ui_scale_for(2.0F, 2.0F) == 1.0F);  // Wayland 2x: 4 kat çizilmemeli
    CHECK(View::ui_scale_for(2.0F, 1.0F) == 2.0F);  // X11/Windows 2x
    CHECK(View::ui_scale_for(1.5F, 1.0F) == 1.5F);
    CHECK(View::ui_scale_for(1.25F, 1.2502F) == 1.0F);  // kesirli yoğunluk yuvarlaması stili oynatmaz
    CHECK(View::ui_scale_for(0.0F, 1.0F) == 1.0F);      // SDL hatası
}

TEST_CASE("display scale change event reapplies the window's scale") {
    LayoutScene scene(1000, 650, 1.5F);
    REQUIRE(scene.view->ui_scale() == 1.5F);
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
    REQUIRE(SDL_PushEvent(&event));
    scene.view->draw_frame();
    // Dummy sürücüde pencerenin gerçek ölçeği 1: olay işlenmezse 1.5 kalırdı.
    CHECK(scene.view->ui_scale() == 1.0F);
    CHECK(ImGui::GetStyle().FontScaleDpi == 1.0F);
}

TEST_CASE("window cannot shrink below the scaled minimum") {
    LayoutScene scene(1, 1, 1.0F);
    CHECK(ImGui::GetIO().DisplaySize.x >= 640.0F);
    CHECK(ImGui::GetIO().DisplaySize.y >= 420.0F);
    scene.view->set_ui_scale(2.0F);
    gui_test::settle(*scene.view);
    CHECK(ImGui::GetIO().DisplaySize.x >= 1280.0F);
    CHECK(ImGui::GetIO().DisplaySize.y >= 840.0F);
    CHECK(scene.state->layout().width >= 1280);  // pencere boyutu durum dosyasına da yansır
}

TEST_CASE("pane sizes follow the scale and do not drift") {
    LayoutScene scene(2000, 1000, 1.0F);
    const float stored = scene.state->layout().left_pane_width;
    const float width_at_one = scene.view->item_rects().at("editor").min_x;
    scene.view->set_ui_scale(1.25F);
    gui_test::settle(*scene.view);
    CHECK(scene.view->item_rects().at("editor").min_x > width_at_one * 1.2F);
    for (int frame = 0; frame < 50; ++frame)
        scene.view->draw_frame();
    CHECK(scene.state->layout().left_pane_width == doctest::Approx(stored).epsilon(0.01));
}

TEST_CASE("an oversized saved left pane cannot push the editor out of the window") {
    gui_test::WindowFixture fixture;
    fixture.add_note("a.md", "a\n");
    auto state = fixture.make_state();
    state->layout().left_pane_width = 5000.0F;
    state->layout().folder_pane_height = 5000.0F;
    auto view = gui_test::make_view(*state);
    gui_test::settle(*view);
    const ItemRects rects = view->item_rects();
    check_inside_x(rects, "editor", window_frame());
    check_inside_y(rects, "note_list", window_frame());
    CHECK(rects.at("editor").max_x - rects.at("editor").min_x >= 200.0F);
    CHECK(rects.at("note_list").max_y - rects.at("note_list").min_y >= 50.0F);
}

TEST_CASE("shrinking the window keeps the pane sizes the user set, on screen and in the state file") {
    gui_test::WindowFixture fixture;
    fixture.add_note("a.md", "a\n");
    float left_width = 0.0F;
    float folder_height = 0.0F;
    {
        auto state = fixture.make_state();
        state->layout().width = 1400;
        state->layout().height = 900;
        auto view = gui_test::make_view(*state);
        drag_left_pane(*view, 300.0F);
        drag_folder_pane(*view, 200.0F);
        left_width = state->layout().left_pane_width;
        folder_height = state->layout().folder_pane_height;
        REQUIRE(left_width > 600.0F);
        REQUIRE(folder_height > 340.0F);

        resize_window(*view, 640, 420);
        CHECK(rect_width(view->item_rects().at("left_pane")) < left_width);  // kısıt gerçekten kırptı
        CHECK(rect_height(view->item_rects().at("folder_tree")) < folder_height);
        check_inside_x(view->item_rects(), "editor", window_frame());
        CHECK(state->layout().left_pane_width == left_width);
        CHECK(state->layout().folder_pane_height == folder_height);

        resize_window(*view, 1400, 900);
        CHECK(state->layout().left_pane_width == left_width);
        CHECK(state->layout().folder_pane_height == folder_height);
        CHECK(rect_width(view->item_rects().at("left_pane")) == doctest::Approx(left_width).epsilon(0.01));
        CHECK(rect_height(view->item_rects().at("folder_tree")) ==
              doctest::Approx(folder_height).epsilon(0.01));
        REQUIRE(state->request_close());
    }
    const auto reopened = fixture.make_state();
    CHECK(reopened->layout().left_pane_width == doctest::Approx(left_width));
    CHECK(reopened->layout().folder_pane_height == doctest::Approx(folder_height));
}

TEST_CASE("a toolbar that wraps and unwraps keeps the folder pane height") {
    LayoutScene scene(1400, 420, 1.0F);
    View& view = *scene.view;
    drag_left_pane(view, 700.0F);
    REQUIRE(view.item_rects().at("toolbar_export").min_y == view.item_rects().at("toolbar_new_note").min_y);
    scene.state->layout().folder_pane_height = 5000.0F;  // tek satırlı araç çubuğundaki üst sınırı ölçer
    gui_test::settle(view);
    const float wanted = rect_height(view.item_rects().at("folder_tree")) - 2.0F;
    scene.state->layout().folder_pane_height = wanted;
    gui_test::settle(view);

    drag_left_pane(view, -1400.0F);
    REQUIRE(view.item_rects().at("toolbar_export").min_y > view.item_rects().at("toolbar_new_note").min_y);
    CHECK(rect_height(view.item_rects().at("folder_tree")) < wanted);  // sarılan satır bölmeyi kırptı
    CHECK(scene.state->layout().folder_pane_height == wanted);

    drag_left_pane(view, 700.0F);
    REQUIRE(view.item_rects().at("toolbar_export").min_y == view.item_rects().at("toolbar_new_note").min_y);
    CHECK(rect_height(view.item_rects().at("folder_tree")) == doctest::Approx(wanted).epsilon(0.01));
    CHECK(scene.state->layout().folder_pane_height == wanted);
}

TEST_CASE("a drag at the clamped limit and a double-click auto-fit still store the pane size") {
    LayoutScene scene(1400, 900, 1.0F);
    View& view = *scene.view;
    drag_left_pane(view, 300.0F);
    const float dragged = scene.state->layout().left_pane_width;
    resize_window(view, 640, 420);
    drag_left_pane(view, -50.0F);  // kırpılmış bölmeyi daraltmak yeni kullanıcı kararıdır
    const float narrowed = rect_width(view.item_rects().at("left_pane"));
    CHECK(narrowed < dragged);
    CHECK(scene.state->layout().left_pane_width == doctest::Approx(narrowed));
    resize_window(view, 1400, 900);
    CHECK(rect_width(view.item_rects().at("left_pane")) == doctest::Approx(narrowed));

    const float before = scene.state->layout().folder_pane_height;
    const View::ItemRect pane = view.item_rects().at("folder_tree");
    // Çift tıklama penceresi gerçek zamandır: yük altında iki tıklama arası varsayılan süreyi aşar.
    constexpr float kUnboundedDoubleClickSeconds = 3600.0F;
    ImGui::GetIO().MouseDoubleClickTime = kUnboundedDoubleClickSeconds;
    ImGui::GetIO().MouseSingleClickDelay = kUnboundedDoubleClickSeconds + 1.0F;  // ImGui: tek > çift olmalı
    ImGui::GetIO().AddMousePosEvent((pane.min_x + pane.max_x) / 2.0F, pane.max_y - 1.0F);
    view.draw_frame();
    for (int click = 0; click < 2; ++click) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        view.draw_frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        view.draw_frame();
    }
    gui_test::settle(view);
    CHECK(scene.state->layout().folder_pane_height != before);
    CHECK(rect_height(view.item_rects().at("folder_tree")) ==
          doctest::Approx(scene.state->layout().folder_pane_height));
}

TEST_CASE("a window forced below the minimum keeps every width positive") {
    for (const ViewSize size : {ViewSize{640, 420}, ViewSize{320, 210}, ViewSize{80, 60}, ViewSize{20, 20}}) {
        CAPTURE(size.width);
        LayoutScene scene(1000, 650, 2.0F);
        force_window_size(*scene.view, size.width, size.height);
        check_sizes_not_negative(scene.view->item_rects());
        check_dialogs_below_minimum(scene);
        scene.show_tab(Tab::Settings);
        check_sizes_not_negative(scene.view->item_rects());
        // Daha küçükte Ayarlar penceresinin görünür alanı kalmaz: ImGui öğeleri atlar, dikdörtgen eskidir.
        if (size.width >= 320)
            check_field_usable(scene.view->item_rects(), "setting:notes_dir");
        scene.show_tab(Tab::Notes);
    }
}

TEST_CASE("dialog text keeps a readable wrap width in a window below the minimum") {
    LayoutScene scene(1000, 650, 2.0F);
    force_window_size(*scene.view, 80, 60);
    constexpr int kMessageCharacters = 40;
    const std::string message(kMessageCharacters, 'a');
    float content_height = 0.0F;
    scene.with_dialog([&] { scene.state->deciders.info(message); },
                      [&] {
                          // Düğme varsayılan odakta olduğundan pencere ona kayar: konumu metin yüksekliğini
                          // göstermez, içerik yüksekliği gösterir.
                          const ImGuiWindow* modal = ImGui::FindWindowByName("noteapp###modal");
                          REQUIRE(modal != nullptr);
                          content_height = modal->ContentSize.y;
                      });
    // Negatif genişlikte ImGui her karakteri ayrı satıra sarar: metin 40 satır olurdu.
    CHECK(content_height > 0.0F);
    CHECK(content_height < font_pixels() * (kMessageCharacters / 2));
}

TEST_CASE("shortened text shows its full value in a tooltip") {
    LayoutScene scene(1000, 650, 1.0F);
    const auto [x, y] = scene.view->button_center("status");
    ImGui::GetIO().AddMousePosEvent(x, y);
    gui_test::RenderedText rendered;
    // İpucu ImGui'de bekleme süresinden sonra açılır (HoverFlagsForTooltipMouse).
    const std::string tail = "yeniden çalıştırın";
    for (int frame = 0; frame < 60 && !rendered.contains(tail); ++frame) {
        SDL_Delay(10);
        scene.view->draw_frame();
    }
    CHECK(rendered.contains(tail));
}

TEST_CASE("settings labels come from the catalog") {
    LayoutScene scene(1000, 650, 1.0F);
    gui_test::RenderedText rendered;
    scene.show_tab(Tab::Settings);
    CHECK(rendered.contains("Senkron ve geçiş klasörü"));
    CHECK_FALSE(rendered.contains("gui_setting"));
}
