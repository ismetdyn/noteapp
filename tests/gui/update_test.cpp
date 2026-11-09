// UPD5: GUI güncelleme akışı (SPEC-gui "Güncelleme"). Karar app::UpdateController'dadır (TUI ile ortak);
// burada palet komutu, bildirimin düğmesi, bekletmeyen pencere (gerçek çizim, fare), ayar satırı ve kapanış.
// HTTP ve kurulum sahtedir (tests/support/update_fixture.hpp); testler ağa çıkmaz.
#include "app/update_controller.hpp"
#include "core/version.hpp"
#include "gui/commands.hpp"
#include "gui/gui_state.hpp"
#include "gui/settings_page.hpp"
#include "support/update_fixture.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <functional>
#include <imgui.h>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using gui_test::click_button;
using gui_test::make_view;
using gui_test::WindowFixture;
using noteapp::app::UpdateAction;
using noteapp::core::tr;
using noteapp::gui::GuiState;

namespace {

constexpr auto kWait = std::chrono::seconds(10);

void run_command(GuiState& state, std::string_view id) {
    const auto found = std::ranges::find(noteapp::gui::kGuiCommands, id, &noteapp::gui::GuiCommand::id);
    REQUIRE_MESSAGE(found != std::end(noteapp::gui::kGuiCommands), id);
    noteapp::gui::run_gui_command(state, *found);
}

// İşçinin on_change'i post eder; UI döngüsü tick'te çalıştırır. Koşul tutana dek döngünün yerine geçer.
bool pump_until(GuiState& state, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
        state.tick(std::chrono::steady_clock::now());
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// İşlem biter ve son duyuru UI iş parçacığında işlenir.
void finish(GuiState& state) {
    REQUIRE(pump_until(state, [&] { return !state.update_view().busy; }));
    for (int i = 0; i < 5; ++i) {  // son post (durum yazıldıktan sonra gelir) kaçmasın
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        state.tick(std::chrono::steady_clock::now());
    }
}

struct UpdateScene : WindowFixture {
    noteapp::test::FakeGitHub github;
    noteapp::test::FakeInstaller installer;

    UpdateScene() { github.publish("1.3.0"); }

    std::unique_ptr<GuiState> make_updating_state() {
        auto state = make_state();
        state->update_deps = [this] {
            return noteapp::app::UpdateWorkerDeps{.get = github.get(),
                                                  .install = installer.installer(),
                                                  .target = noteapp::test::installable_target(),
                                                  .on_change = {}};
        };
        return state;
    }
};

std::string button_name(UpdateAction action) {
    return std::string("update:") + noteapp::app::update_action_label(action);
}

bool drawn(const gui_test::View& view, const std::string& name) {
    return view.item_rects().contains(name);
}

}  // namespace

TEST_CASE_FIXTURE(UpdateScene,
                  "update: the startup check posts a notice with an Install button for a newer release") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::milliseconds(0);  // bu test yuva yarışını değil akışı sınar
    state->check_updates_on_startup();
    REQUIRE(pump_until(*state, [&] { return !state->notices().items().empty(); }));
    const noteapp::app::Notice notice = state->notices().items().back();
    CHECK(notice.update_version == "1.3.0");
    CHECK(notice.text == tr("cli.update_available",
                            {{"version", "1.3.0"}, {"current", std::string(noteapp::core::version())}}));
    CHECK_FALSE(state->update_open());  // açılış denetimi pencere açmaz
    CHECK(installer.calls() == 0);

    const std::string button = "notice_update:" + std::to_string(notice.id);
    gui_test::settle(*view);  // kendiliğinden boyutlanan kutu ikinci karede yerine oturur
    REQUIRE(drawn(*view, button));
    click_button(*view, button);
    CHECK(state->update_open());
    CHECK(std::ranges::none_of(state->notices().items(),
                               [](const auto& n) { return n.update_version.has_value(); }));
    view->draw_frame();
    CHECK(drawn(*view, "update_dialog"));
    CHECK(drawn(*view, button_name(UpdateAction::Install)));
    CHECK(installer.calls() == 0);  // pencere açıldı, kurulum onay bekliyor
}

TEST_CASE_FIXTURE(UpdateScene, "update: the palette command opens the window and checks for a new release") {
    auto state = make_updating_state();
    run_command(*state, "update");
    CHECK(state->update_open());
    finish(*state);
    CHECK(github.requests() == 1);
    CHECK(state->update_view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
    CHECK(state->notices().items().empty());  // sonuç pencerede
}

TEST_CASE_FIXTURE(UpdateScene, "update: the window installs only when Install is clicked") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::milliseconds(0);  // bu test yuva yarışını değil akışı sınar
    run_command(*state, "update");
    finish(*state);
    view->draw_frame();
    REQUIRE(drawn(*view, "update_dialog"));
    CHECK(drawn(*view, "update_link"));  // release sayfası görünür

    gui_test::tap_key(ImGuiKey_Enter);  // imleç Kapat'ta: Enter kurmaz, pencereyi kapatır
    view->draw_frame();
    view->draw_frame();
    view->draw_frame();
    CHECK_FALSE(state->update_open());
    view->draw_frame();
    CHECK_FALSE(drawn(*view, "update_dialog"));
    CHECK(installer.calls() == 0);

    run_command(*state, "update");
    view->draw_frame();
    view->draw_frame();
    click_button(*view, button_name(UpdateAction::Close));
    CHECK_FALSE(state->update_open());
    CHECK(installer.calls() == 0);

    run_command(*state, "update");  // bulunan sürüm: yeniden denetlenmez
    CHECK(github.requests() == 1);
    view->draw_frame();
    view->draw_frame();
    click_button(*view, button_name(UpdateAction::Install));
    finish(*state);
    CHECK(installer.calls() == 1);
    CHECK(state->update_view().lines.front() == tr("cli.update_installed", {{"version", "1.3.0"}}));
}

TEST_CASE_FIXTURE(UpdateScene, "update: Cancel in the window stops the download and installs nothing") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::milliseconds(0);  // bu test yuva yarışını değil akışı sınar
    run_command(*state, "update");
    finish(*state);
    view->draw_frame();
    view->draw_frame();
    github.hold();
    click_button(*view, button_name(UpdateAction::Install));
    REQUIRE(github.wait_for_waiting());
    state->tick(std::chrono::steady_clock::now());
    view->draw_frame();
    view->draw_frame();
    REQUIRE(drawn(*view, button_name(UpdateAction::Cancel)));
    click_button(*view, button_name(UpdateAction::Cancel));
    finish(*state);
    github.release();
    CHECK(installer.calls() == 0);
    CHECK(state->update_view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
}

TEST_CASE_FIXTURE(UpdateScene,
                  "update: an install that cannot update itself offers the release page to copy") {
    auto state = make_state();
    state->update_deps = [this] {
        return noteapp::app::UpdateWorkerDeps{.get = github.get(),
                                              .install = installer.installer(),
                                              .target = noteapp::app::InstallTarget{},  // Unsupported
                                              .on_change = {}};
    };
    std::string copied;
    state->deciders.copy_text = [&copied](const std::string& text) {
        copied = text;
        return true;
    };
    run_command(*state, "update");
    finish(*state);
    CHECK(state->update_view().actions == std::vector{UpdateAction::CopyLink, UpdateAction::Close});
    CHECK_FALSE(state->update_installable());
    state->update_action(UpdateAction::CopyLink);
    CHECK(copied == "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0");
    state->update_action(UpdateAction::Install);  // düğmesi yok; doğrudan istense de kurmaz
    finish(*state);
    CHECK(installer.calls() == 0);
}

TEST_CASE_FIXTURE(UpdateScene,
                  "update: closing during a replace waits, tells why, then asks about unsaved work once") {
    std::mutex mutex;
    std::condition_variable_any finish_install;
    bool may_finish = false;
    bool installing = false;
    auto state = make_state();
    state->update_deps = [&] {
        return noteapp::app::UpdateWorkerDeps{
            .get = github.get(),
            .install = [&](const noteapp::app::InstallTarget&, const noteapp::app::ReleaseInfo&,
                           std::string_view, const noteapp::core::UpdateLimits&,
                           std::stop_token) -> noteapp::app::InstallResult {
                std::unique_lock lock(mutex);
                installing = true;
                finish_install.notify_all();
                finish_install.wait(lock, [&] { return may_finish; });  // değiştirme: iptali yok sayar
                return noteapp::app::InstallOutcome{};
            },
            .target = noteapp::test::installable_target(),
            .on_change = {}};
    };
    int asked = 0;  // kaydedilmemiş taslak: kapanışta sorulur
    state->deciders.close_unsaved = [&asked] {
        ++asked;
        return noteapp::gui::CloseChoice::Discard;
    };
    state->new_note();
    state->edit_text("taslak");
    run_command(*state, "update");
    finish(*state);
    state->update_action(UpdateAction::Install);
    {
        std::unique_lock lock(mutex);
        REQUIRE(finish_install.wait_for(lock, kWait, [&] { return installing; }));
    }
    CHECK_FALSE(state->request_close());  // pencere açık kalır
    CHECK(asked == 0);                    // bekleme sorulardan önce: soru kapanış gerçekten olunca
    REQUIRE_FALSE(state->notices().items().empty());
    CHECK(state->notices().items().back().text == tr("gui.update_exit_wait"));
    CHECK_FALSE(state->take_quit_request());
    {
        const std::scoped_lock lock(mutex);
        may_finish = true;
    }
    finish_install.notify_all();
    REQUIRE(
        pump_until(*state, [&] { return state->take_quit_request(); }));  // kurulum bitti: kapanış istenir
    CHECK(state->request_close());
    CHECK(asked == 1);
}

TEST_CASE_FIXTURE(UpdateScene,
                  "update: the settings row refuses an interval under an hour and saves 0 as off") {
    auto state = make_state();
    noteapp::gui::SettingsPage& page = state->settings();
    CHECK(page.field("update.check_interval") == "24h");
    page.set_field("update.check_interval", "30min");
    page.save();
    CHECK(page.field_error("update.check_interval").find(tr("reason.must_be_update_interval")) !=
          std::string::npos);
    CHECK(noteapp::core::load_config().config.update.check_interval.text == "24h");

    page.set_field("update.check_interval", "0");
    page.save();
    CHECK(page.field_error("update.check_interval").empty());
    CHECK(noteapp::core::load_config().config.update.check_interval == noteapp::core::Limit{0, "0"});

    auto view = make_view(*state);
    state->show_settings_tab();
    gui_test::settle(*view);
    CHECK(drawn(*view, "setting_label:update.check_interval"));
    CHECK(drawn(*view, "setting_hint:update.check_interval"));
}

TEST_CASE_FIXTURE(UpdateScene, "update: Settings' Check for updates button runs the palette's check") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    state->show_settings_tab();
    gui_test::settle(*view);
    REQUIRE(drawn(*view, "update_check"));
    CHECK_FALSE(state->update_open());
    click_button(*view, "update_check");
    CHECK(state->update_open());
    finish(*state);
    CHECK(github.requests() == 1);
    CHECK(state->update_view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
    view->draw_frame();
    CHECK(drawn(*view, "update_dialog"));
    CHECK(installer.calls() == 0);
}

TEST_CASE_FIXTURE(UpdateScene, "update: the window title always shows the installed version") {
    auto state = make_state();
    auto view = make_view(*state);
    int count = 0;
    const std::unique_ptr<SDL_Window*, decltype(&SDL_free)> windows(SDL_GetWindows(&count), &SDL_free);
    REQUIRE(count == 1);
    CHECK(std::string(SDL_GetWindowTitle(windows.get()[0])) ==
          "noteapp " + std::string(noteapp::core::version()));
}

TEST_CASE_FIXTURE(UpdateScene, "update: Settings shows the installed version beside the button") {
    auto state = make_state();
    auto view = make_view(*state);
    const gui_test::RenderedText rendered;
    state->show_settings_tab();
    gui_test::settle(*view);
    CHECK(drawn(*view, "version"));
    CHECK(rendered.contains("noteapp " + std::string(noteapp::core::version())));
    CHECK(rendered.contains(tr("command.update")));
}

// İnceleme (güvenlik, Sev-3): denetim biterken Kur İptal'in yerine (ilk düğme) geçer. Pencere yeni satırlarla
// büyüdüğünden İptal'in eski konumu çoğu kez metne düşer; yarış, değişiklikten hemen sonra ilk düğmeye inen
// tıklamadır (kullanıcı bir önceki karede İptal'i gördü).
TEST_CASE_FIXTURE(UpdateScene, "update: a click aimed at Cancel that lands after the check does not install") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::seconds(60);  // makine yükünden bağımsız
    github.hold();
    run_command(*state, "update");
    REQUIRE(github.wait_for_waiting());
    gui_test::settle(*view);
    REQUIRE(drawn(*view, button_name(UpdateAction::Cancel)));
    github.release();
    finish(*state);  // yeni sürüm: Kur, İptal'in yerinde
    REQUIRE(state->update_view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
    view->draw_frame();  // değişikliğin karesi
    click_button(*view, button_name(UpdateAction::Install));
    finish(*state);
    CHECK(installer.calls() == 0);
    CHECK(github.requests() == 1);  // indirme de başlamadı
    CHECK(state->update_open());
    CHECK(view->item_rects().at(button_name(UpdateAction::Install)).disabled);
}

TEST_CASE_FIXTURE(UpdateScene, "update: a second click right after Cancel does not install; Install opens later") {
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::milliseconds(0);
    run_command(*state, "update");
    finish(*state);
    gui_test::settle(*view);
    github.hold();
    click_button(*view, button_name(UpdateAction::Install));
    REQUIRE(github.wait_for_waiting());
    state->tick(std::chrono::steady_clock::now());
    view->update_arm_delay = std::chrono::seconds(60);
    gui_test::settle(*view);
    click_button(*view, button_name(UpdateAction::Cancel));
    finish(*state);
    github.release();
    REQUIRE(state->update_view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
    view->draw_frame();
    click_button(*view, button_name(UpdateAction::Install));  // sabırsız ikinci tıklama: ilk düğme artık Kur
    finish(*state);
    CHECK(installer.calls() == 0);
    CHECK(github.requests() == 2);  // denetim ve iptal edilen SHA256SUMS

    // Süre dolunca Kur açılır; döngü o an kendiliğinden bir kare çizer. Kare süresinden bağımsız: gecikme
    // uzun, "süre doldu" anını kanca kurar. Önceki 60 sn'lik kilit de doldurulur: aşağıdaki kilit yalnız yeniden
    // açılıştan gelebilir.
    click_button(*view, button_name(UpdateAction::Close));
    view->expire_update_arm();
    view->idle_step(0);
    view->update_arm_delay = std::chrono::seconds(10);
    run_command(*state, "update");  // yeniden açılış da değişikliktir
    finish(*state);
    view->draw_frame();
    view->draw_frame();
    CHECK(view->item_rects().at(button_name(UpdateAction::Install)).disabled);
    view->expire_update_arm();
    view->idle_step(0);
    CHECK_FALSE(view->item_rects().at(button_name(UpdateAction::Install)).disabled);
    click_button(*view, button_name(UpdateAction::Install));
    finish(*state);
    CHECK(installer.calls() == 1);
}

// Klavye: denetim sürerken imleç İptal'de; denetim bitince Enter ya da ok + Enter kurmamalı.
TEST_CASE_FIXTURE(UpdateScene, "update: keys aimed at Cancel during a check do not install when it finishes") {
    std::vector<ImGuiKey> keys;
    SUBCASE("Enter") {
        keys = {ImGuiKey_Enter};
    }
    SUBCASE("Left, Enter") {
        keys = {ImGuiKey_LeftArrow, ImGuiKey_Enter};
    }
    SUBCASE("Right, Enter") {
        keys = {ImGuiKey_RightArrow, ImGuiKey_Enter};
    }
    auto state = make_updating_state();
    auto view = make_view(*state);
    view->update_arm_delay = std::chrono::seconds(60);
    github.hold();
    run_command(*state, "update");
    REQUIRE(github.wait_for_waiting());
    gui_test::settle(*view);
    gui_test::press(*view, ImGuiKey_LeftArrow);  // Kapat → İptal
    github.release();
    finish(*state);
    gui_test::settle(*view);
    for (const ImGuiKey key : keys)
        gui_test::press(*view, key);
    finish(*state);
    CHECK(installer.calls() == 0);
    CHECK(github.requests() == 1);
}
