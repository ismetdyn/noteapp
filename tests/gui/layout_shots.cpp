// Yerleşim sahnelerinin PNG görüntüsünü üretir (ctest'e kayıtlı değildir; gözle inceleme aracı):
//   SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software NOTEAPP_SHOT_DIR=/yol gui_layout_shots
#include "gui/theme.hpp"
#include "layout_scenes.hpp"

#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
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

// Dummy sürücüde sistem teması yoktur: View::apply_theme'in koyu dalı elle kurulur.
void apply_dark_theme() {
    noteapp::gui::theme::apply_colors(ImGui::GetStyle(), noteapp::gui::theme::kDark);
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

// R3 tek başına (`-tc="renders the sort menu*"`): genel arama (en iyi satırla), etiket seçici, sıralama
// menüsü.
TEST_CASE("renders the sort menu, the tag picker and global search to PNG") {
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
          Shot{"min", 1, 1, 1.0F, false}}) {
        LayoutScene scene(shot.width, shot.height, shot.scale);
        if (shot.dark)
            apply_dark_theme();
        scene.fixture.add_note("gündem-planı.md", "#proje #toplantı\nhafta\n", 2);
        scene.fixture.add_note("proje/özet.md", "#proje\nbütçe ve gündem dışı konular\n", 3);
        scene.state->rescan();
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what, shot.size_name,
                                                                  shot.scale, shot.dark ? "dark" : "light");
        };
        scene.state->search_global();
        scene.state->set_query("gündem");
        gui_test::settle(*scene.view);
        save_png(file("global-search"));

        scene.state->open_picker(noteapp::gui::Picker::Tag);
        gui_test::settle(*scene.view);
        save_png(file("tag-picker"));
        scene.state->close_picker();
        scene.state->set_query("");
        gui_test::settle(*scene.view);

        scene.state->open_sort_menu();
        gui_test::settle(*scene.view);
        save_png(file("sort-menu"));
    }
}

// G2/G3 tek başına (`-tc="renders the palette*"`): komut paleti, yardım, ağaçta ve çöpte çoklu seçim, çöp
// önizlemesi, Ayrıntılar'da senkron sütunu, hızlı ekleme satırı.
TEST_CASE("renders the palette, help, marks, columns and quick add to PNG") {
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
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) / std::format("{}-{}-s{:.2f}-{}.png", what, shot.size_name,
                                                                  shot.scale, shot.dark ? "dark" : "light");
        };
        const auto path = [](const char* text) { return *noteapp::core::NotePath::parse(text); };
        // Ağaçta iki not işaretli, imleç üçüncüde.
        scene.state->open_note(path("ayar.toml"));
        scene.state->toggle_mark();
        scene.state->toggle_mark();
        scene.state->request_focus(noteapp::gui::FocusRequest::List);
        gui_test::settle(*scene.view);
        save_png(file("marks-tree"));
        scene.state->clear_marks();

        scene.state->begin_quick_add();
        gui_test::settle(*scene.view);
        gui_test::type_text(*scene.view, "yeni-fikir");
        save_png(file("quick-add"));
        scene.state->cancel_quick_add();

        scene.state->set_browser_view(noteapp::gui::BrowserView::Details);
        scene.state->set_details_column(noteapp::gui::DetailsColumn::Sync);
        gui_test::settle(*scene.view);
        save_png(file("details-sync"));
        scene.state->set_details_column(noteapp::gui::DetailsColumn::SizeAndDate);
        scene.state->set_browser_view(noteapp::gui::BrowserView::Tree);

        scene.with_dialog([&] { scene.state->open_palette(); }, [&] { save_png(file("palette")); });
        scene.with_dialog([&] { scene.state->show_help(); }, [&] { save_png(file("help")); });

        // Çöpte iki not işaretli, seçilinin metni ve bilgisi sağda.
        scene.fixture.add_note(".trash/eski-taslak.md", "çöpteki ikinci not\nikinci satır\n", 2);
        scene.show_trash_view();
        scene.state->mark_all();
        scene.state->set_info_open(true);
        gui_test::settle(*scene.view);
        save_png(file("marks-trash"));
    }
}

// G4 tek başına (`-tc="renders the G4*"`): CSV tablosu, JSON görünümü, arama kutusunda gölge öneri, renkli
// argümanlar ve Tab listesi.
TEST_CASE("renders the G4 data previews and the search line to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    for (const bool dark : {false, true}) {
        LayoutScene scene(1000, 650, 1.0F);
        if (dark)
            apply_dark_theme();
        scene.fixture.add_note(
            "veri/şehirler.csv",
            "şehir;nüfus;bölge;not\nİstanbul;15655924;Marmara;\"en kalabalık; liman\"\n"
            "Ankara;5803482;İç Anadolu;başkent\nİzmir;4479525;Ege;\"\"\"Körfez\"\" kenti\"\n",
            1);
        scene.fixture.add_note(
            "veri/ayar.json",
            "{\"ad\":\"noteapp\",\"sürüm\":3,\"etkin\":true,\"etiketler\":[\"proje\",\"plan\"],"
            "\"sınırlar\":{\"satır\":5000,\"sütun\":128,\"boş\":null},\"oran\":-1.5e3}",
            2);
        scene.fixture.add_note("planlama.md", "#proje taslak\n", 3);
        scene.state->rescan();
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) /
                   std::format("{}-1000x650-s1.00-{}.png", what, dark ? "dark" : "light");
        };
        scene.state->open_note(*noteapp::core::NotePath::parse("veri/şehirler.csv"));
        gui_test::settle(*scene.view);
        REQUIRE(scene.state->showing_preview());
        save_png(file("g4-csv"));
        scene.state->open_note(*noteapp::core::NotePath::parse("veri/ayar.json"));
        gui_test::settle(*scene.view);
        REQUIRE(scene.state->showing_preview());
        save_png(file("g4-json"));

        const auto type_in_search = [&](const char* text) {
            scene.state->set_query({});
            scene.state->focus_search();
            gui_test::settle(*scene.view);
            gui_test::type_text(*scene.view, text);
            gui_test::settle(*scene.view);
        };
        type_in_search("planl");
        REQUIRE_FALSE(scene.state->search_ghost().empty());
        save_png(file("g4-search-ghost"));
        type_in_search("tag:proje size:kocaman plan");
        save_png(file("g4-search-args"));
        type_in_search("type:");
        gui_test::press(*scene.view, ImGuiKey_Tab);
        gui_test::settle(*scene.view);
        REQUIRE(scene.state->search_popup() != nullptr);
        save_png(file("g4-search-popup"));
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

// G5: EditorBuffer üstündeki editör: satır numaraları, geçerli satır, durum çubuğu, gizli karakter, not içi
// bul.
TEST_CASE("renders the G5 editor to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    for (const bool dark : {false, true}) {
        LayoutScene scene(1000, 650, 1.0F);
        if (dark)
            apply_dark_theme();
        scene.fixture.add_note("kod/örnek.md",
                               "\xEF\xBB\xBF# Başlık\r\n\r\nİstanbul'da *toplantı* var.\r\nGizli: a\xE2\x80\xAE"
                               "b\r\n\r\n```\r\nkod satırı\r\n```\r\nistanbul ISTANBUL İstanbul\r\n",
                               1);
        scene.state->rescan();
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) /
                   std::format("{}-1000x650-s1.00-{}.png", what, dark ? "dark" : "light");
        };
        scene.state->open_note(*noteapp::core::NotePath::parse("kod/örnek.md"));
        scene.state->edit_selected();  // seçilen not incelemede açılır
        scene.state->request_focus(noteapp::gui::FocusRequest::Editor);
        gui_test::settle(*scene.view);
        gui_test::press(*scene.view, ImGuiKey_DownArrow);
        gui_test::press(*scene.view, ImGuiKey_DownArrow);
        gui_test::press(*scene.view, ImGuiKey_End);
        gui_test::settle(*scene.view);
        save_png(file("g5-editor"));
        gui_test::press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_F);
        gui_test::type_text(*scene.view, "istanbul");
        gui_test::press(*scene.view, ImGuiKey_F3);
        gui_test::settle(*scene.view);
        save_png(file("g5-find"));
        gui_test::press(*scene.view, ImGuiKey_Escape);
    }
}

// G5b: sözdizimi renkleri, yazım alt çizgisi (sahte hunspell: `kotu` ile başlayan sözcük yanlış), gölge öneri
// ve Ctrl+Space listesi.
TEST_CASE("renders the G5 editor features to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    const noteapp::test::TempDir tools;
    const noteapp::test::ScopedEnv path("PATH", noteapp::test::utf8(tools.path()));
    {
        std::ofstream out(tools.path() / "hunspell");
        out << "#!/bin/sh\necho '@(#) International Ispell Version 3.2.06 (but really Hunspell 1.7.2)'\n"
               "while IFS= read -r line; do\n  for word in $line; do\n    word=\"${word#^}\"\n"
               "    case \"$word\" in kotu*) echo \"& $word 1 0: iyi\" ;; *) echo \"*\" ;; esac\n  done\n"
               "  echo\ndone\n";
    }
    std::filesystem::permissions(tools.path() / "hunspell", std::filesystem::perms::owner_all);
    for (const bool dark : {false, true}) {
        LayoutScene scene(1000, 650, 1.0F);
        if (dark)
            apply_dark_theme();
        scene.fixture.add_note("kod/örnek.py",
                               "# Örnek betik\nimport os\n\n\ndef topla(a: int, b: int) -> int:\n"
                               "    \"\"\"İki sayıyı toplar.\"\"\"\n    return a + b  # sonuç\n\n\n"
                               "class Kayıt:\n    sayı = 42\n    ad = 'kayıt'\n",
                               1);
        scene.fixture.add_note("yazı.md", "# Günlük\n\nBugün kotu bir hava var, kotuluk yok.\nalpine alpaca\n", 2);
        scene.state->rescan();
        const auto file = [&](const std::string& what) {
            return std::filesystem::path(directory) /
                   std::format("{}-1000x650-s1.00-{}.png", what, dark ? "dark" : "light");
        };
        scene.state->open_note(*noteapp::core::NotePath::parse("kod/örnek.py"));
        gui_test::settle(*scene.view);
        save_png(file("g5-syntax"));

        scene.state->open_note(*noteapp::core::NotePath::parse("yazı.md"));
        scene.state->edit_selected();  // seçilen not incelemede açılır
        scene.state->request_focus(noteapp::gui::FocusRequest::Editor);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!scene.view->item_rects().contains("editor_spell:3:7") && std::chrono::steady_clock::now() < deadline) {
            scene.state->tick(std::chrono::steady_clock::now());
            scene.view->draw_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(scene.view->item_rects().contains("editor_spell:3:7"));
        save_png(file("g5-spell"));

        gui_test::press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_End);
        gui_test::press(*scene.view, ImGuiKey_Enter);
        gui_test::type_text(*scene.view, "al");
        gui_test::settle(*scene.view);
        save_png(file("g5-ghost"));
        gui_test::press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_Space);
        gui_test::settle(*scene.view);
        REQUIRE(scene.view->item_rects().contains("editor_completion"));
        save_png(file("g5-completion"));
        gui_test::press(*scene.view, ImGuiKey_Escape);
        gui_test::press(*scene.view, ImGuiMod_Ctrl | ImGuiKey_Z);
    }
}

// G5c: dil sunucusu tanısı (alt çizgi ve imleçteki ileti); sunucu tests/tui/fake_lsp_server.
TEST_CASE("renders the G5 language server diagnostic to PNG") {
    const char* directory = std::getenv("NOTEAPP_SHOT_DIR");
    REQUIRE(directory != nullptr);
    std::filesystem::create_directories(directory);
    for (const bool dark : {false, true}) {
        LayoutScene scene(1000, 650, 1.0F);
        scene.view.reset();
        scene.state.reset();
        scene.fixture.config.lsp.enabled = true;
        scene.fixture.config.lsp.servers.clear();
        scene.fixture.config.lsp.servers["python"] = {
            {std::filesystem::absolute(NOTEAPP_FAKE_LSP_SERVER).string(), "normal"}};
        scene.fixture.add_note("kod/hata.py", "import os, sys\n\n\ndef topla(a, b):\n    return a + b\n", 1);
        scene.state = scene.fixture.make_state();
        scene.state->layout().width = 1000;
        scene.state->layout().height = 650;
        scene.view = gui_test::make_view(*scene.state);
        scene.view->install_deciders();
        if (dark)
            apply_dark_theme();
        scene.state->open_note(*noteapp::core::NotePath::parse("kod/hata.py"));
        scene.state->edit_selected();  // seçilen not incelemede açılır
        scene.state->request_focus(noteapp::gui::FocusRequest::Editor);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!scene.view->item_rects().contains("editor_diagnostic:1:0") &&
               std::chrono::steady_clock::now() < deadline) {
            scene.state->tick(std::chrono::steady_clock::now());
            scene.view->draw_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(scene.view->item_rects().contains("editor_diagnostic:1:0"));
        gui_test::settle(*scene.view);
        save_png(std::filesystem::path(directory) /
                 std::format("g5-lsp-diagnostic-1000x650-s1.00-{}.png", dark ? "dark" : "light"));
    }
}
