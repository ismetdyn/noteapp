#pragma once

#include "core/i18n.hpp"
#include "gui/save_dialog.hpp"
#include "sync/sync_service.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Çakışma testi ve ekran görüntüsü aracı aynı sahneleri kullanır: görüntüde görülen, testte denetlenendir.
namespace gui_test {

using ItemRects = std::map<std::string, View::ItemRect>;

inline const std::string kLongNotePath =
    "çok-uzun-bir-klasör-adı/alt-klasör-içinde-başka-bir-alt-klasör/üçüncü-düzey-klasör-şimdi-bitti/"
    "İstanbul-toplantısının-ayrıntılı-notları-ve-yapılacak-işler.md";

// Her ölçekte en küçük pencereden uzun bir ileti.
constexpr int kTallMessageLines = 40;

inline noteapp::core::Error long_error(const char* key) {
    return noteapp::core::Error{
        noteapp::core::Error::Code::Io,
        key,
        {{"path", "/home/kullanıcı/Belgeler/notlarımın-bulunduğu-çok-uzun-klasör-adı"},
         {"branch", "ana-geliştirme-dalı"},
         {"detail", "bağlantı karşı taraf tarafından\nkapatıldı"}}};
}

// Türkçe arayüz, uzun not yolu, uzun senkron hatası ve uzun durum iletisi. Pencere boyutu View kurulmadan
// verilir; dummy sürücüde ölçek hep 1 olduğundan ekran ölçeği de elle uygulanır.
struct LayoutScene {
    WindowFixture fixture;
    std::unique_ptr<GuiState> state;
    std::unique_ptr<View> view;

    LayoutScene(int width, int height, float scale) {
        noteapp::core::init_i18n("tr");
        fixture.add_note(kLongNotePath, "# Toplantı\n\nGündem maddeleri burada.\n");
        fixture.add_note("kısa.md", "kısa not\n", 5);
        state = fixture.make_state();
        state->layout().width = width;
        state->layout().height = height;
        state->select_row(state->model().index_of(*noteapp::core::NotePath::parse(kLongNotePath)));
        state->show_sync_status(
            {.last_success = std::nullopt, .last_error = long_error("sync.both_non_empty")}, false);
        state->show_config_warnings({long_error("sync.repo_busy_state")});
        view = make_view(*state);
        view->install_deciders();
        view->set_ui_scale(scale);
        settle(*view);
    }
    ~LayoutScene() { noteapp::core::init_i18n("en"); }
    LayoutScene(const LayoutScene&) = delete;
    LayoutScene& operator=(const LayoutScene&) = delete;

    void show_tab(noteapp::gui::Tab tab) {
        if (tab == noteapp::gui::Tab::Settings)
            state->show_settings_tab();
        else
            state->focus_search();
        settle(*view);
    }

    // Her satırın en uzun hâli: alan hatası, bağlantı sonucu, https token durumu, kaydedilmemiş ipucu.
    void fill_settings_with_longest_rows() {
        auto& page = state->settings();
        page.set_field("sync.enabled", "true");
        page.set_field("sync.remote", kLongNotePath + "/olmayan-uzak-depo.git");
        page.test_connection();
        constexpr int kMaxWaitSteps = 3000;  // işçi sonucu kuyruktan gelir; asılı kalırsa aşağıda düşer
        for (int wait = 0; wait < kMaxWaitSteps && (page.testing() || page.test_result().empty()); ++wait) {
            SDL_Delay(10);
            state->tick(std::chrono::steady_clock::now());
        }
        REQUIRE_FALSE(page.test_result().empty());
        page.set_field("sync.remote", "https://git.ornek.com/kullanıcı/notlarımın-uzak-deposu.git");
        page.set_field("sync.idle_seconds", "x");
        page.save();
        REQUIRE_FALSE(page.field_error("sync.idle_seconds").empty());
        settle(*view);
    }

    // Çöp görünümü en uzun hâliyle: en uzun yol çöpte, durum çubuğunda geri al.
    void show_trash_view() {
        fixture.add_note("işler/çöpe-atılacak-uzun-adlı-bir-not.md", "çöpteki not\n", 1);
        state->rescan();
        state->select_row(state->model().index_of(
            *noteapp::core::NotePath::parse("işler/çöpe-atılacak-uzun-adlı-bir-not.md")));
        state->trash_note();
        state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("kısa.md")));
        settle(*view);
        state->open_trash();
        settle(*view);
    }

    // Diyaloğu açar, yerleşmesini bekler, `at_settled` çağırır ve Esc ile kapatır.
    void with_dialog(const std::function<void()>& open, const std::function<void()>& at_settled) {
        static constexpr int kSettledFrame = 6;
        int frame = 0;
        view->on_modal_frame = [&] {
            ++frame;
            if (frame == kSettledFrame) {
                at_settled();
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
            }
        };
        open();
        view->on_modal_frame = nullptr;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        settle(*view);
    }
};

struct DialogScene {
    std::string name;
    std::function<void(LayoutScene&)> open;
};

// Üretim diyalogları: Deciders doğrudan çağrılır, böylece her biri uzun metinle görülür.
inline std::vector<DialogScene> dialog_scenes() {
    using noteapp::core::tr;
    return {
        {"save",
         [](LayoutScene& scene) {
             noteapp::gui::SaveDialogRequest request;
             request.initial = {"istanbul-toplantisi", "çok-uzun-bir-klasör-adı", "md"};
             request.folders = scene.state->folders();
             request.types = {"md", "txt"};
             request.hint = tr("gui.suggestion", {{"type", "md"}, {"percent", "87"}});
             scene.state->deciders.save_dialog(request);
         }},
        {"confirm",
         [](LayoutScene& scene) {
             scene.state->deciders.confirm_delete(tr("gui.confirm_delete_note", {{"path", kLongNotePath}}));
         }},
        {"confirm_tall",
         [](LayoutScene& scene) {
             std::string message;
             for (int line = 0; line < kTallMessageLines; ++line)
                 message += kLongNotePath + "\n";
             scene.state->deciders.confirm_delete(message);
         }},
        {"conflict", [](LayoutScene& scene) { scene.state->deciders.conflict(kLongNotePath); }},
        {"close_settings", [](LayoutScene& scene) { scene.state->deciders.close_unsaved_settings(); }},
        {"folder_name",
         [](LayoutScene& scene) { scene.state->deciders.folder_name(tr("gui.new_folder_title"), ""); }},
        {"settings_browse",
         [](LayoutScene& scene) {
             scene.state->deciders.browse_dir(
                 "/home/kullanıcı/Belgeler/notlarımın-bulunduğu-çok-uzun-klasör-adı");
         }},
        {"settings_origin",
         [](LayoutScene& scene) {
             scene.state->deciders.confirm_replace_origin(
                 tr("sync.remote_mismatch",
                    {{"remote", "https://git.ornek.com/kullanıcı/notlarımın-uzak-deposu.git"},
                     {"current", "https://eski.ornek.com/kullanıcı/eski-notlarımın-deposu.git"}}));
         }},
    };
}

}  // namespace gui_test
