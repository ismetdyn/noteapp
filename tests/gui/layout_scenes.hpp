#pragma once

#include "core/i18n.hpp"
#include "gui/save_dialog.hpp"
#include "sync/sync_service.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <format>
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
        fixture.add_note("kısa.md", "kısa not\n", 5);
        fixture.add_note(kLongNotePath,
                         "# Toplantı\n\nGündem maddeleri burada. [[kısa]] [[silinmiş-eski-not]]\n");
        fixture.add_note("gelen.md", "[[İstanbul-toplantısının-ayrıntılı-notları-ve-yapılacak-işler]]\n", 6);
        for (const char* typed : {"betik.sh", "ayar.toml", "veri.json", "kod.py", "tablo.csv"})
            fixture.add_note(typed, "x\n", 6);  // liste ikonlarının türleri görüntüde seçilsin
        {  // bağlantı paneli her sahnede açık: çakışma testi onu da denetler
            noteapp::core::NoteStore store(fixture.store_dir());
            REQUIRE(store
                        .add_relation(*noteapp::core::NotePath::parse(kLongNotePath),
                                      *noteapp::core::NotePath::parse("tablo.csv"))
                        .has_value());
        }
        state = fixture.make_state();
        state->layout().width = width;
        state->layout().height = height;
        state->select_row(state->model().index_of(*noteapp::core::NotePath::parse(kLongNotePath)));
        // Sahneler düzenleme kipinin yerleşimini ölçer (editör, alanlar, bul satırı); seçilen not incelemede
        // açılır, önizleme sahneleri toggle_preview ile incelemeye döner.
        state->begin_edit();
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

    // Ayrıntılar görünümü en uzun yol çubuğuyla: seçili not üç düzey derindeki klasörde, konum oraya gelir.
    void show_details_view() {
        state->set_browser_view(noteapp::gui::BrowserView::Details);
        settle(*view);
    }

    // Harita görünümü, gösterilen notun çevresi; yerleşim durana dek kare üretilir (sınırlı).
    void show_map_view() {
        state->set_browser_view(noteapp::gui::BrowserView::Map);
        constexpr int kMaxFrames = 600;
        for (int frame = 0; frame < kMaxFrames && !state->map_layout().settled(); ++frame)
            view->draw_frame();
        REQUIRE(state->map_layout().settled());
        settle(*view);
    }

    // T85: `count` not ayrı sekmede (uzun Türkçe adlar), sonuncusu etkin; ilki iğneli, biri taslak.
    void open_tabs(int count) {
        for (int index = 0; index < count; ++index)
            fixture.add_note(std::format("sekme-{:02}-toplantı-ayrıntıları.md", index), "x\n", 1);
        state->rescan();
        state->set_tab_pinned(state->tabs().front().id, true);
        state->new_tab();
        state->edit_text("kaydedilmemiş taslak\n");
        for (int index = 0; index < count; ++index)
            state->open_in_new_tab(
                *noteapp::core::NotePath::parse(std::format("sekme-{:02}-toplantı-ayrıntıları.md", index)));
        state->begin_edit();  // sahneler düzenleme kipini ölçer
        settle_tab_scroll();
    }

    // ImGui sekme çubuğunun kaydırması gerçek zamanla canlanır (DeltaTime): etkin sekme görünene dek kare
    // üretilir (sınırlı; görünmezse testin denetimi düşer).
    void settle_tab_scroll() {
        settle(*view);
        constexpr int kMaxFrames = 600;
        const auto active_visible = [this] {
            const auto& rects = view->item_rects();
            const auto tab = rects.find(std::format("note_tab:{}", state->active_tab()));
            const auto bar = rects.find("note_tabs");
            return tab != rects.end() && bar != rects.end() && tab->second.min_x >= bar->second.min_x &&
                   tab->second.max_x <= bar->second.max_x;
        };
        for (int frame = 0; frame < kMaxFrames && !active_visible(); ++frame)
            view->draw_frame();
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
