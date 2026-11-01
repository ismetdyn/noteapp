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
