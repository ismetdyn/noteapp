// UPD5: TUI güncelleme akışı (SPEC-tui "Güncelleme"). Karar app::UpdateController'dadır (GUI ile ortak);
// burada palet komutu, pencerenin tuşları, çizim ve bildirimin ipucu, sahte HTTP ve kurulumla uçtan uca akış.
// Ağa çıkılmaz; durum dosyası geçici NOTEAPP_HOME'dadır.
#include "app/update_controller.hpp"
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/version.hpp"
#include "support/test_support.hpp"
#include "support/update_fixture.hpp"
#include "tui/modals.hpp"
#include "tui/settings.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace noteapp;
using app::UpdateAction;
using tui::Intent;
using tui::Key;
using tui::UpdateDialogState;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;
constexpr auto kWait = std::chrono::seconds(10);
const core::Limit kDaily{24ULL * 60 * 60 * 1000, "24h"};
const std::string kPage = "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0";

struct Home {
    test::TempDir dir;
    test::ScopedEnv env{"NOTEAPP_HOME", dir.path().u8string()};
    Home() { core::init_i18n("en"); }
};

core::NoteInfo info(const std::string& path) {
    return core::NoteInfo{core::NotePath::parse(path).value(), std::filesystem::file_time_type{}, 1};
}

tui::TuiState sample_state() {
    tui::TuiState state;
    state.set_notes({info("a.md")});
    state.set_window(kWidth, kHeight);
    return state;
}

std::string screen_text(const tui::TuiState& state, const tui::StatusLine& status,
                        const UpdateDialogState* update) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, {}, status, {kWidth, kHeight}, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr, nullptr, {}, update));
    return screen.ToString();
}

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

app::UpdateView available_view(bool installable) {
    return app::update_view(
        app::UpdateAvailable{.version = "1.3.0", .release_page = kPage, .installable = installable}, "1.2.3");
}

// TuiSession'ın yaptığı: on_change'in UI iş parçacığındaki devamı (burada test bekler).
void settle(app::UpdateController& controller, tui::StatusLine& status) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (controller.busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(controller.busy());
    controller.changed(status.notices, std::chrono::steady_clock::now());
}

// TuiSession::handle_update_key ile aynı yol: içerik işçiden, eylem denetleyiciye; Close pencereyi kapatır.
void press(app::UpdateController& controller, std::optional<UpdateDialogState>& dialog, Key key) {
    REQUIRE(dialog.has_value());
    dialog->show(controller.view());
    const std::optional<UpdateAction> action = dialog->handle(key);
    if (!action || *action == UpdateAction::CopyLink)
        return;
    controller.act(*action);
    if (!controller.is_open())
        dialog.reset();
}

app::UpdateWorkerDeps deps(test::FakeGitHub& github, test::FakeInstaller& installer) {
    return app::UpdateWorkerDeps{.get = github.get(),
                                 .install = installer.installer(),
                                 .target = test::installable_target(),
                                 .on_change = {}};
}

}  // namespace

TEST_CASE("update: the palette command opens the update window in the list, REVIEW and the trash") {
    const Home home;
    for (int view = 0; view < 3; ++view) {
        CAPTURE(view);
        tui::TuiState in = sample_state();
        REQUIRE(in.select_note(core::NotePath::parse("a.md").value()));
        if (view == 1) {
            in.set_preview_line_count(10);
            in.focus_preview();
        } else if (view == 2) {
            in.enter_trash({});
        }
        in.handle(Key::Char, ":");
        for (const char c : std::string_view("update"))
            in.handle(Key::Char, std::string(1, c));
        REQUIRE(in.palette_selected() != nullptr);
        CHECK(in.palette_selected()->id == "update");
        CHECK(in.handle(Key::Enter) == Intent::CheckUpdates);
    }
}

TEST_CASE("update window: the selection starts on Close; only an explicit move to Install installs") {
    const Home home;
    UpdateDialogState dialog;
    dialog.show(available_view(true));
    CHECK(dialog.selected() == UpdateAction::Close);
    CHECK(dialog.handle(Key::Enter) == UpdateAction::Close);  // Enter varsayılanı kurmaz
    CHECK(dialog.handle(Key::Char) == std::nullopt);          // harf kısayolu yok
    CHECK(dialog.handle(Key::Left) == std::nullopt);
    CHECK(dialog.selected() == UpdateAction::Install);
    CHECK(dialog.handle(Key::Enter) == UpdateAction::Install);
    CHECK(dialog.handle(Key::Escape) == UpdateAction::Close);
    CHECK(dialog.handle(Key::Tab) == std::nullopt);
    CHECK(dialog.selected() == UpdateAction::Close);

    // Durum değişince (indirme başladı) seçim yeniden Kapat'a döner: Enter iptal etmez de.
    dialog.handle(Key::Left);
    REQUIRE(dialog.selected() == UpdateAction::Install);
    dialog.show(app::update_view(app::UpdateDownloading{"1.3.0"}, "1.2.3"));
    CHECK(dialog.selected() == UpdateAction::Close);
    dialog.handle(Key::Left);
    CHECK(dialog.handle(Key::Enter) == UpdateAction::Cancel);
}

TEST_CASE("update window: shows the versions, the release page and the buttons; untrusted text is safe") {
    const Home home;
    UpdateDialogState dialog;
    dialog.show(available_view(true));
    const std::string text = screen_text(sample_state(), {"sync off", ""}, &dialog);
    CHECK(has(text, core::tr("update.ui_title")));
    CHECK(has(text, "1.3.0"));
    CHECK(has(text, "1.2.3"));
    // Uzun adres kutunun iç genişliğinde (50 hücre) bölünür, kaybolmaz.
    CHECK(has(text, kPage.substr(0, 50)));
    CHECK(has(text, kPage.substr(50)));
    CHECK(has(text, "[ " + core::tr("update.action_install") + " ]"));
    CHECK(has(text, "[ " + core::tr("update.action_close") + " ]"));

    UpdateDialogState unsupported;
    unsupported.show(available_view(false));
    const std::string page = screen_text(sample_state(), {"sync off", ""}, &unsupported);
    CHECK_FALSE(has(page, "[ " + core::tr("update.action_install") + " ]"));
    CHECK(has(page, "[ " + core::tr("update.action_copy_link") + " ]"));

    UpdateDialogState hostile;
    hostile.show(
        app::update_view(app::UpdateFailed{core::Error{
                             core::Error::Code::Io, "update.install_busy", {{"path", "/tmp/\x1b[31mevil"}}}},
                         "1.2.3"));
    CHECK_FALSE(has(screen_text(sample_state(), {"sync off", ""}, &hostile), "\x1b[31m"));
    // FTXUI kaçış baytını kendisi de süzebilir: metin çizimden önce de temiz olmalı.
    app::UpdateView raw = hostile.view();
    raw.link = "https://github.com/ismetdyn/noteapp/releases/tag/v1\x1b]8;;evil\x07";
    const app::UpdateView safe = tui::safe_update_view(raw);
    for (const std::string& line : safe.lines)
        CHECK(line.find('\x1b') == std::string::npos);
    CHECK(safe.link.find('\x1b') == std::string::npos);
    CHECK(safe.link.find('\x07') == std::string::npos);
}

TEST_CASE("update notice: the toast says how to open the update window") {
    const Home home;
    tui::StatusLine status{"sync off", ""};
    status.notices.post_update(core::tr("cli.update_available", {{"version", "1.3.0"}, {"current", "1.2.3"}}),
                               "1.3.0", std::chrono::steady_clock::now());
    const std::string text = screen_text(sample_state(), status, nullptr);
    CHECK(has(text, core::tr("notice.info") + "  noteapp 1.3.0"));
    CHECK(has(text, core::tr("tui.notice_update_hint")));
}

TEST_CASE("update flow: the startup check posts the notice; the palette window installs only on confirm") {
    const Home home;
    test::FakeGitHub github;
    github.publish("1.3.0");
    test::FakeInstaller installer;
    app::UpdateController controller(test::update_settings("1.2.3"), deps(github, installer));
    tui::StatusLine status{"sync off", ""};
    REQUIRE(controller.check_on_startup(std::chrono::system_clock::now(), kDaily));
    settle(controller, status);
    REQUIRE(status.notices.items().size() == 1);
    CHECK(status.notices.items().front().update_version == "1.3.0");

    // Palet: pencere açılır, bildirim kalkar; Enter (Kapat) hiçbir şey kurmaz.
    std::optional<UpdateDialogState> dialog;
    controller.open(status.notices);
    dialog.emplace();
    CHECK(status.notices.items().empty());
    press(controller, dialog, Key::Enter);
    CHECK_FALSE(dialog.has_value());
    CHECK(installer.calls() == 0);

    controller.open(status.notices);
    dialog.emplace();
    press(controller, dialog, Key::Left);  // Kur
    press(controller, dialog, Key::Enter);
    REQUIRE(dialog.has_value());
    settle(controller, status);
    CHECK(installer.calls() == 1);
    dialog->show(controller.view());
    CHECK(dialog->view().lines.front() == core::tr("cli.update_installed", {{"version", "1.3.0"}}));
    CHECK(status.notices.items().empty());  // pencere açık: sonuç pencerede
}

TEST_CASE("update flow: Cancel in the window stops the download and installs nothing") {
    const Home home;
    test::FakeGitHub github;
    github.publish("1.3.0");
    test::FakeInstaller installer;
    app::UpdateController controller(test::update_settings("1.2.3"), deps(github, installer));
    tui::StatusLine status{"sync off", ""};
    std::optional<UpdateDialogState> dialog;
    controller.open(status.notices);  // palet: denetim başlar
    dialog.emplace();
    settle(controller, status);
    github.hold();
    press(controller, dialog, Key::Left);
    press(controller, dialog, Key::Enter);  // Kur
    REQUIRE(github.wait_for_waiting());
    dialog->show(controller.view());
    CHECK(dialog->view().busy);
    press(controller, dialog, Key::Left);  // İptal
    press(controller, dialog, Key::Enter);
    settle(controller, status);
    github.release();
    CHECK(installer.calls() == 0);
    dialog->show(controller.view());
    CHECK(dialog->view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
}

TEST_CASE("update button: Settings shows Check for updates with the installed version") {
    const Home home;
    const tui::SettingsState settings(core::parse_config("").config);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    const tui::StatusLine status{"sync off", ""};
    ftxui::Render(screen, tui::render(sample_state(), {}, status, {kWidth, kHeight}, nullptr, nullptr,
                                      nullptr, &settings));
    const std::string text = screen.ToString();
    CHECK(has(text, "[ " + core::tr("command.update") + " ]"));
    CHECK(has(text, "noteapp " + std::string(core::version())));
    CHECK(app::version_label() == "noteapp " + std::string(core::version()));
}

namespace {

std::string top_line(int width, const tui::StatusLine& status) {
    tui::TuiState state = sample_state();
    state.set_window(width, kHeight);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, {}, status, {width, kHeight}));
    const std::string text = screen.ToString();
    return text.substr(0, text.find('\n'));
}

}  // namespace

TEST_CASE("version: the TUI top bar ends with the installed version when there is room") {
    const Home home;
    const std::string line = top_line(kWidth, {"sync off", ""});
    CHECK(has(line, "sync off · noteapp " + std::string(core::version())));
}

TEST_CASE("version: a narrow top bar drops the version before it shortens the sync text") {
    const Home home;
    const std::string sync = "✓ synced at 09:05";
    const std::string version = app::version_label();
    bool seen_both = false;
    bool seen_sync_only = false;
    for (int width = kWidth; width >= 40; --width) {
        CAPTURE(width);
        const std::string line = top_line(width, {sync, ""});
        const bool full_sync = has(line, sync);
        const bool shows_version = has(line, version);
        CHECK_FALSE((shows_version && !full_sync));  // senkron kısaldıysa sürüm çoktan düşmüştür
        seen_both = seen_both || (full_sync && shows_version);
        seen_sync_only = seen_sync_only || (full_sync && !shows_version);
    }
    CHECK(seen_both);
    CHECK(seen_sync_only);  // sürüm düştüğü genişlikte senkron hâlâ tam
}

TEST_CASE("update button: Enter on it in Settings runs the palette's check; the window opens over the form") {
    const Home home;
    test::FakeGitHub github;
    github.publish("1.3.0");
    test::FakeInstaller installer;
    app::UpdateController controller(test::update_settings("1.2.3"), deps(github, installer));
    tui::StatusLine status{"sync off", ""};
    tui::SettingsState settings(core::parse_config("").config);
    for (int i = 0; i < 3; ++i)
        settings.handle(Key::ShiftTab);  // ilk satırdan geri: Kaydet, bağlantı testi, güncelleme
    REQUIRE(settings.on_update());
    REQUIRE(settings.handle(Key::Enter) == tui::SettingsState::Outcome::CheckUpdates);

    // TuiSession'ın CheckUpdates'te yaptığı: palet `update` ile aynı yol.
    std::optional<UpdateDialogState> dialog;
    controller.open(status.notices);
    dialog.emplace();
    settle(controller, status);
    CHECK(github.requests() == 1);
    dialog->show(controller.view());
    CHECK(dialog->view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(sample_state(), {}, status, {kWidth, kHeight}, nullptr, nullptr,
                                      nullptr, &settings, nullptr, nullptr, nullptr, nullptr, {}, &*dialog));
    const std::string text = screen.ToString();
    CHECK(has(text, dialog->view().lines.front()));
    CHECK_FALSE(has(text, core::tr("tui.settings_title")));  // form altta kalır, kapanınca geri gelir
    press(controller, dialog, Key::Enter);  // Kapat
    CHECK_FALSE(dialog.has_value());
    CHECK(installer.calls() == 0);
}
