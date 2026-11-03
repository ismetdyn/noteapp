// Yerleşim sahnelerinin PNG görüntüsünü üretir (ctest'e kayıtlı değildir; gözle inceleme aracı):
//   SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software NOTEAPP_SHOT_DIR=/yol gui_layout_shots
#include "layout_scenes.hpp"

#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <string>
#include <tuple>

namespace {

using gui_test::LayoutScene;
using noteapp::gui::Tab;

SDL_Window* app_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    REQUIRE(windows != nullptr);
    REQUIRE(count == 1);
    SDL_Window* window = windows[0];
    SDL_free(windows);
    return window;
}

// Yazılımsal çizicide sunumdan sonra da arka tampon yerindedir: son kare okunur.
void save_png(const std::filesystem::path& file) {
    SDL_Surface* pixels = SDL_RenderReadPixels(SDL_GetRenderer(app_window()), nullptr);
    REQUIRE(pixels != nullptr);
    const bool saved = SDL_SavePNG(pixels, file.string().c_str());
    SDL_DestroySurface(pixels);
    REQUIRE(saved);
}

// Dummy sürücüde sistem teması yoktur: View::apply_theme'in koyu dalı elle kurulur (saydam olmayan diyalog
// dahil).
void apply_dark_theme() {
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w = 1.0F;
}

// Formun sonu: küçük pencerede Senkron grubu ancak kaydırılınca görünür; altbilgi yerinde kalmalı.
void scroll_settings_form_to_end(LayoutScene& scene) {
    const auto [x, y] = scene.view->button_center("settings_form");
    ImGui::GetIO().AddMousePosEvent(x, y);
    scene.view->draw_frame();
    constexpr float kWheelNotches = -100.0F;
    ImGui::GetIO().AddMouseWheelEvent(0.0F, kWheelNotches);
    gui_test::settle(*scene.view);
}

// Çöpe atınca durum çubuğunda ileti + Geri al görünür.
void settle_after_undo(LayoutScene& scene) {
    scene.state->select_row(0);
    scene.state->trash_note();
    gui_test::settle(*scene.view);
}

}  // namespace

// Gezgin görünümleri tek başına da üretilir (`-tc="renders the browser views*"`): tam set uzun sürer.
TEST_CASE("renders the browser views to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    struct Shot {
        const char* size_name;
        int width;
        int height;
        float scale;
        bool dark;
    };
    for (const Shot& shot :
         {Shot{"1000x650", 1000, 650, 1.0F, false}, Shot{"1000x650", 1000, 650, 1.0F, true},
          Shot{"min", 1, 1, 1.0F, false}, Shot{"min", 1, 1, 2.0F, false}}) {
        LayoutScene scene(shot.width, shot.height, shot.scale);
        if (shot.dark)
            apply_dark_theme();
        // Eski tarihli notlar: Ayrıntılar'da kısa biçimin en geniş hâli ("2025-...") de görünsün.
        constexpr int kMinutesPerDay = 24 * 60;
        for (const auto& [note, age_days] :
             {std::pair{"projeler/plan.md", 30}, std::pair{"projeler/arşiv/eski.md", 400},
              std::pair{"günlük/2026-11-02.md", 0}, std::pair{"eski-not.md", 400}})
            scene.fixture.add_note(note, "x\n", age_days * kMinutesPerDay + 3);
        scene.state->rescan();
        gui_test::settle(*scene.view);
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what,
                                                                  shot.size_name, shot.scale,
                                                                  shot.dark ? "dark" : "light");
        };
        save_png(file("tree"));
        scene.state->select_folder("projeler");
        scene.state->set_expanded("projeler", true);
        gui_test::settle(*scene.view);
        save_png(file("tree-folder"));
        scene.state->set_browser_view(noteapp::gui::BrowserView::Details);
        scene.state->enter_folder("");
        gui_test::settle(*scene.view);
        save_png(file("details"));
        scene.state->enter_folder("projeler");
        gui_test::settle(*scene.view);
        save_png(file("details-folder"));
    }
}

// T82 görünümleri tek başına (`-tc="renders the info*"`): bilgi paneli açık, sıra no açık (Ağaç, Ayrıntılar),
// örnek md notunun önizlemesi.
TEST_CASE("renders the info panel, row numbers and the markdown preview to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    constexpr char kSample[] =
        "# Toplantı notları\n\n"
        "Gündem *kısa* tutuldu, **kararlar** aşağıda; ayrıntı için [[kısa]] notuna bakın.\n\n"
        "## Kararlar\n\n"
        "1. Bütçe onaylandı\n2. Takvim `2026-12` olarak kaldı\n   - alt madde: izinler\n\n"
        "- [x] tutanak gönderildi\n- [ ] sunum hazırlanacak\n\n"
        "> Not: bir sonraki toplantıda [[silinmiş-eski-not]] konusu açılacak.\n\n"
        "```python\ndef toplam(liste):\n    return sum(liste)  # çok uzun bir açıklama satırı yatay kayar\n```\n\n"
        "| Şehir | Katılımcı |\n|---|---|\n| İzmir | 12 |\n| Ankara | 7 |\n\n"
        "---\n\n"
        "Kaynak: [site](https://ornek.example/tutanak) ve ![logo](logo.png).\n";
    struct Shot {
        const char* size_name;
        int width;
        int height;
        float scale;
        bool dark;
    };
    for (const Shot& shot :
         {Shot{"1000x650", 1000, 650, 1.0F, false}, Shot{"1000x650", 1000, 650, 1.0F, true},
          Shot{"min", 1, 1, 1.0F, false}, Shot{"min", 1, 1, 2.0F, false}}) {
        LayoutScene scene(shot.width, shot.height, shot.scale);
        if (shot.dark)
            apply_dark_theme();
        scene.fixture.add_note("toplantı/örnek.md", kSample, 1);
        scene.state->rescan();
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what, shot.size_name,
                                                                  shot.scale, shot.dark ? "dark" : "light");
        };
        scene.state->set_info_open(true);
        gui_test::settle(*scene.view);
        save_png(file("info-open"));

        scene.state->set_row_numbers(true);
        scene.state->set_expanded("toplantı", true);
        gui_test::settle(*scene.view);
        save_png(file("row-numbers-tree"));
        scene.state->set_browser_view(noteapp::gui::BrowserView::Details);
        scene.state->enter_folder("");
        gui_test::settle(*scene.view);
        save_png(file("row-numbers-details"));

        scene.state->set_row_numbers(false);
        scene.state->set_info_open(false);
        scene.state->set_browser_view(noteapp::gui::BrowserView::Tree);
        scene.state->open_note(*noteapp::core::NotePath::parse("toplantı/örnek.md"));
        scene.state->toggle_preview();
        gui_test::settle(*scene.view);
        REQUIRE(scene.state->showing_preview());
        save_png(file("preview"));
    }
}

// T84 haritası tek başına (`-tc="renders the map*"`): çevre (derinlik 2), tüm çizge etiketlerle, aramada
// vurgu.
TEST_CASE("renders the map view to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    struct Shot {
        const char* size_name;
        int width;
        int height;
        float scale;
        bool dark;
    };
    for (const Shot& shot :
         {Shot{"1000x650", 1000, 650, 1.0F, false}, Shot{"1000x650", 1000, 650, 1.0F, true},
          Shot{"min", 1, 1, 1.0F, false}, Shot{"min", 1, 1, 2.0F, false}}) {
        LayoutScene scene(shot.width, shot.height, shot.scale);
        if (shot.dark)
            apply_dark_theme();
        // Hedef kaynaktan önce: kenar yazmada kurulur (SPEC-graph "Sonradan oluşan hedef").
        for (const auto& [note, body] :
             {std::pair{"proje/takvim.md", "#proje"}, std::pair{"proje/bütçe.md", "[[proje/takvim]] #proje"},
              std::pair{"proje/plan.md", "[[kısa]] [[proje/bütçe]] [[proje/takvim]] #proje #iş"},
              std::pair{"günlük/2026-11-02.md", "[[proje/plan]] [[kısa]] #günlük"},
              std::pair{"fikirler.txt", "[[proje/plan]] [[olmayan-not]]"}, std::pair{"yalnız.md", "bağsız"}})
            scene.fixture.add_note(note, std::string(body) + "\n", 1);
        scene.state->rescan();
        scene.state->open_note(*noteapp::core::NotePath::parse("proje/plan.md"));
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what,
                                                                  shot.size_name, shot.scale,
                                                                  shot.dark ? "dark" : "light");
        };
        scene.show_map_view();
        save_png(file("map-neighbourhood"));
        scene.state->set_map_options(noteapp::gui::MapOptions{.whole_graph = true, .show_tags = true});
        scene.show_map_view();
        save_png(file("map-whole-tags"));
        scene.state->set_query("tag:proje");
        gui_test::settle(*scene.view);
        save_png(file("map-search"));
    }
}

// T85 sekmeleri tek başına (`-tc="renders the editor tabs*"`): üç sekme, kayan çok sekme; ilki iğneli, biri
// taslak, yeni sekmede açılanlar korunur (düz yazı), sonradan değişecek olan eğik.
TEST_CASE("renders the editor tabs to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    struct Shot {
        const char* size_name;
        int width;
        int height;
        float scale;
        bool dark;
    };
    constexpr int kFewTabs = 3;
    constexpr int kManyTabs = 20;
    for (const Shot& shot :
         {Shot{"1000x650", 1000, 650, 1.0F, false}, Shot{"1000x650", 1000, 650, 1.0F, true},
          Shot{"min", 1, 1, 1.0F, false}, Shot{"min", 1, 1, 2.0F, false}}) {
        for (const int count : {kFewTabs, kManyTabs}) {
            LayoutScene scene(shot.width, shot.height, shot.scale);
            if (shot.dark)
                apply_dark_theme();
            scene.open_tabs(count);
            // Sonuncuyu sekme çubuğundan değil ağaçtan aç: korunmayan sekme eğik görünsün.
            scene.state->open_note(*noteapp::core::NotePath::parse("kısa.md"));
            scene.settle_tab_scroll();
            save_png(std::filesystem::path(directory) / std::format("tabs-{}-{}-s{:.2f}-{}.png", count,
                                                                    shot.size_name, shot.scale,
                                                                    shot.dark ? "dark" : "light"));
        }
    }
}

TEST_CASE("renders every layout scene to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);

    for (const bool dark : {false, true}) {
        for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
            for (const auto& [size_name, width, height] :
                 {std::tuple{"1000x650", 1000, 650}, std::tuple{"min", 1, 1}}) {
                LayoutScene scene(width, height, scale);
                if (dark)
                    apply_dark_theme();
                int actual_width = 0;
                int actual_height = 0;
                SDL_GetWindowSize(app_window(), &actual_width, &actual_height);
                const auto file = [&](const std::string& what) {
                    return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what,
                                                                          size_name, scale,
                                                                          dark ? "dark" : "light");
                };
                gui_test::settle(*scene.view);
                save_png(file("notes"));
                scene.show_tab(Tab::Settings);
                save_png(file("settings"));
                scene.fill_settings_with_longest_rows();
                save_png(file("settings-filled"));
                scroll_settings_form_to_end(scene);
                save_png(file("settings-filled-end"));
                scene.state->settings().revert();
                scene.show_tab(Tab::Notes);
                for (const auto& dialog : gui_test::dialog_scenes()) {
                    scene.with_dialog([&] { dialog.open(scene); }, [&] { save_png(file(dialog.name)); });
                }
                scene.show_trash_view();
                save_png(file("trash"));
                scene.with_dialog([&] { scene.state->empty_trash(); },
                                  [&] { save_png(file("trash-empty-confirm")); });
                scene.state->close_trash();
                settle_after_undo(scene);
                save_png(file("undo"));
                MESSAGE(std::string(size_name), " s", scale, " window ", actual_width, "x", actual_height);
            }
        }
    }
}
