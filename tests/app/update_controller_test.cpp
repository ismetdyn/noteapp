// app::UpdateController ve app::update_view: TUI ve GUI'nin ortak güncelleme akışı (açılış denetimi, palet,
// onaylı kurulum, iptal, bildirimler). HTTP ve kurulum sahtedir (tests/support/update_fixture.hpp); testler
// ağa çıkmaz, durum dosyası geçici NOTEAPP_HOME'dadır.
#include "app/update_controller.hpp"
#include "core/i18n.hpp"
#include "support/test_support.hpp"
#include "support/update_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using noteapp::app::InstallOutcome;
using noteapp::app::InstallTarget;
using noteapp::app::NoticeKind;
using noteapp::app::NoticeQueue;
using noteapp::app::UpdateAction;
using noteapp::app::UpdateAvailable;
using noteapp::app::UpdateChecking;
using noteapp::app::UpdateController;
using noteapp::app::UpdateDone;
using noteapp::app::UpdateDownloading;
using noteapp::app::UpdateFailed;
using noteapp::app::UpdateInstalling;
using noteapp::app::UpdateView;
using noteapp::app::UpdateWorkerDeps;
using noteapp::app::UpToDate;
using noteapp::core::Error;
using noteapp::core::tr;
using noteapp::test::FakeGitHub;
using noteapp::test::FakeInstaller;
using noteapp::test::installable_target;
using noteapp::test::kLatestApiUrl;
using noteapp::test::update_settings;

namespace {

constexpr auto kWait = std::chrono::seconds(10);
constexpr std::chrono::milliseconds kNoticeLife{4000};
const noteapp::core::Limit kDaily{24ULL * 60 * 60 * 1000, "24h"};

struct TempHome {
    noteapp::test::TempDir dir;
    noteapp::test::ScopedEnv env{"NOTEAPP_HOME", dir.path().u8string()};
    TempHome() { noteapp::core::init_i18n("en"); }
};

UpdateWorkerDeps deps(FakeGitHub& github, FakeInstaller& installer,
                      InstallTarget target = installable_target()) {
    return UpdateWorkerDeps{
        .get = github.get(), .install = installer.installer(), .target = std::move(target), .on_change = {}};
}

// İşçinin işlemi biter, UI iş parçacığı (burada test) changed()'i çağırır; görünür değişiklik varsa true.
bool settle(UpdateController& controller, NoticeQueue& notices) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (controller.busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(controller.busy());
    return controller.changed(notices, NoticeQueue::Clock::now());
}

bool has(const std::vector<UpdateAction>& actions, UpdateAction action) {
    return std::ranges::find(actions, action) != actions.end();
}

std::string joined(const UpdateView& view) {
    std::string text;
    for (const std::string& line : view.lines)
        text += line + "\n";
    return text;
}

const std::string kPage = "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0";

}  // namespace

TEST_CASE("update view: a found release asks before installing and shows its release page") {
    const TempHome home;
    const UpdateView view = noteapp::app::update_view(
        UpdateAvailable{.version = "1.3.0", .release_page = kPage, .installable = true}, "1.2.3");
    CHECK(joined(view).find("1.3.0") != std::string::npos);
    CHECK(joined(view).find("1.2.3") != std::string::npos);  // kurulu sürüm de görünür
    CHECK(view.link == kPage);
    CHECK(view.actions == std::vector{UpdateAction::Install, UpdateAction::Close});
    CHECK_FALSE(view.busy);
}

TEST_CASE("update view: an install that cannot update itself offers the release page instead of Install") {
    const TempHome home;
    const UpdateView view = noteapp::app::update_view(
        UpdateAvailable{.version = "1.3.0", .release_page = kPage, .installable = false}, "1.2.3");
    CHECK(view.link == kPage);
    CHECK_FALSE(has(view.actions, UpdateAction::Install));
    CHECK(view.actions == std::vector{UpdateAction::CopyLink, UpdateAction::Close});
}

TEST_CASE("update view: running steps are busy and can be cancelled; finished ones only close") {
    const TempHome home;
    for (const noteapp::app::UpdateStatus& status : {noteapp::app::UpdateStatus(UpdateChecking{}),
                                                     noteapp::app::UpdateStatus(UpdateDownloading{"1.3.0"}),
                                                     noteapp::app::UpdateStatus(UpdateInstalling{"1.3.0"})}) {
        const UpdateView view = noteapp::app::update_view(status, "1.2.3");
        CHECK(view.busy);
        CHECK(view.actions == std::vector{UpdateAction::Cancel, UpdateAction::Close});
        CHECK_FALSE(view.lines.empty());
    }
    CHECK(joined(noteapp::app::update_view(UpdateDownloading{"1.3.0"}, "1.2.3")) ==
          tr("cli.update_downloading", {{"version", "1.3.0"}}) + "\n");

    const UpdateView latest = noteapp::app::update_view(UpToDate{"1.2.3"}, "1.2.3");
    CHECK(latest.actions == std::vector{UpdateAction::Close});
    CHECK(joined(latest) == tr("cli.update_up_to_date", {{"current", "1.2.3"}, {"latest", "1.2.3"}}) + "\n");

    const Error busy{Error::Code::Io, "update.install_busy", {{"path", "/opt/noteapp"}}};
    const UpdateView failed = noteapp::app::update_view(UpdateFailed{busy}, "1.2.3");
    CHECK(failed.actions == std::vector{UpdateAction::Close});
    CHECK(joined(failed) == noteapp::core::format_error(busy) + "\n");  // yerelleştirilmiş neden

    const UpdateView linux_done = noteapp::app::update_view(UpdateDone{"1.3.0", false}, "1.2.3");
    CHECK(joined(linux_done) == tr("cli.update_installed", {{"version", "1.3.0"}}) + "\n");
    const UpdateView windows_done = noteapp::app::update_view(UpdateDone{"1.3.0", true}, "1.2.3");
    CHECK(joined(windows_done).find(tr("cli.update_restart")) != std::string::npos);
    CHECK(windows_done.actions == std::vector{UpdateAction::Close});
}

TEST_CASE(
    "update controller: the startup check runs once the interval has passed and posts a found release") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    NoticeQueue notices(kNoticeLife);
    REQUIRE(controller.check_on_startup(std::chrono::system_clock::now(), kDaily));
    CHECK_FALSE(controller.is_open());  // sessiz: pencere açılmaz
    CHECK(settle(controller, notices));
    REQUIRE(notices.items().size() == 1);
    const auto& notice = notices.items().front();
    CHECK(notice.kind == NoticeKind::Info);
    CHECK(notice.update_version == "1.3.0");
    CHECK(notice.text == tr("cli.update_available", {{"version", "1.3.0"}, {"current", "1.2.3"}}));
    CHECK(installer.calls() == 0);  // kurulum hep onayla

    CHECK_FALSE(controller.changed(notices, NoticeQueue::Clock::now()));  // aynı sonuç ikinci kez değil
    CHECK(notices.items().front().repeats == 1);

    // Denetim zamanı kaydedildi: aralık dolmadan bir sonraki açılış denetlemez.
    UpdateController next(update_settings("1.2.3"), deps(github, installer));
    CHECK_FALSE(next.check_on_startup(std::chrono::system_clock::now(), kDaily));
    CHECK(github.requests() == 1);
}

TEST_CASE("update controller: an up-to-date or failed startup check stays silent") {
    const TempHome home;
    FakeGitHub github;
    FakeInstaller installer;
    NoticeQueue notices(kNoticeLife);
    SUBCASE("up to date") {
        github.publish("1.2.3");
    }
    SUBCASE("failed") {
        github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
    }
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(controller.check_on_startup(std::chrono::system_clock::now(), kDaily));
    CHECK_FALSE(settle(controller, notices));  // görünür değişiklik yok: kare istenmez
    CHECK(notices.items().empty());
    CHECK(github.requests() == 1);
}

TEST_CASE("update controller: a check_interval of 0 never checks at startup") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    CHECK_FALSE(controller.check_on_startup(std::chrono::system_clock::now(), noteapp::core::Limit{0, "0"}));
    CHECK_FALSE(controller.busy());
    CHECK(github.requests() == 0);
}

TEST_CASE("update controller: the palette command opens the window and checks; the window shows the result") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    NoticeQueue notices(kNoticeLife);
    controller.open(notices);
    CHECK(controller.is_open());
    CHECK(github.requests() <= 1);
    settle(controller, notices);
    CHECK(github.requests() == 1);
    CHECK(notices.items().empty());  // pencere açık: sonuç pencerede
    CHECK(controller.view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
}

TEST_CASE("update controller: a manual check finished behind a closed window becomes a notice") {
    const TempHome home;
    FakeGitHub github;
    FakeInstaller installer;
    github.hold();
    NoticeQueue notices(kNoticeLife);
    std::string expected;
    NoticeKind kind = NoticeKind::Info;
    SUBCASE("up to date") {
        github.publish("1.2.3");
        expected = tr("cli.update_up_to_date", {{"current", "1.2.3"}, {"latest", "1.2.3"}});
    }
    SUBCASE("failed") {
        github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
        expected = "";
        kind = NoticeKind::Error;
    }
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    controller.open(notices);
    REQUIRE(github.wait_for_waiting());
    controller.act(UpdateAction::Close);  // pencere kapanır, denetim sürer
    CHECK_FALSE(controller.is_open());
    CHECK(controller.busy());
    github.release();
    settle(controller, notices);
    REQUIRE(notices.items().size() == 1);
    CHECK(notices.items().front().kind == kind);
    CHECK_FALSE(notices.items().front().update_version.has_value());
    if (!expected.empty())
        CHECK(notices.items().front().text == expected);
}

TEST_CASE("update controller: Install starts only when confirmed; closing the window installs nothing") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    NoticeQueue notices(kNoticeLife);
    REQUIRE(controller.check_on_startup(std::chrono::system_clock::now(), kDaily));
    settle(controller, notices);
    REQUIRE(notices.items().size() == 1);

    controller.open(notices);        // bildirimin "Kur" eylemi: onay penceresi
    CHECK(notices.items().empty());  // kullanıcı bildirime davrandı
    CHECK(github.requests() == 1);   // bulunan sürüm yeniden denetlenmez
    CHECK(controller.view().link == kPage);
    controller.act(UpdateAction::Close);
    CHECK(installer.calls() == 0);

    controller.open(notices);
    controller.act(UpdateAction::Install);
    CHECK(controller.view().busy);
    settle(controller, notices);
    CHECK(installer.calls() == 1);
    CHECK(controller.view().lines.front() == tr("cli.update_installed", {{"version", "1.3.0"}}));
    CHECK(notices.items().empty());  // pencere açık: sonuç pencerede

    controller.act(UpdateAction::Close);
    controller.open(notices);  // kurulduktan sonra yeniden denetlemez, sonucu gösterir
    CHECK_FALSE(controller.busy());
    CHECK(github.requests() == 3);  // denetim + SHA256SUMS + paket
    CHECK(controller.view().lines.front() == tr("cli.update_installed", {{"version", "1.3.0"}}));
}

TEST_CASE(
    "update controller: Cancel stops the download; nothing is installed and the release stays offered") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    NoticeQueue notices(kNoticeLife);
    controller.open(notices);
    settle(controller, notices);
    github.hold();
    controller.act(UpdateAction::Install);
    REQUIRE(github.wait_for_waiting());
    CHECK(controller.view().actions == std::vector{UpdateAction::Cancel, UpdateAction::Close});
    controller.act(UpdateAction::Cancel);
    settle(controller, notices);
    github.release();
    CHECK(installer.calls() == 0);
    CHECK(controller.view().actions == std::vector{UpdateAction::Install, UpdateAction::Close});
}

TEST_CASE("update controller: an install finished behind a closed window becomes a notice") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    NoticeQueue notices(kNoticeLife);
    SUBCASE("Linux: done") {
        installer.result = InstallOutcome{.restart_required = false};
    }
    SUBCASE("Windows: done, restart") {
        installer.result = InstallOutcome{.restart_required = true};
    }
    SUBCASE("failed") {
        installer.result = std::unexpected(Error{Error::Code::Io,
                                                 "update.rollback_pending",
                                                 {{"path", "C:/noteapp"}, {"url", kPage}, {"marker", ".x"}}});
    }
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    controller.open(notices);
    settle(controller, notices);
    github.hold_only(noteapp::test::release_download_url("v1.3.0", "SHA256SUMS"));
    controller.act(UpdateAction::Install);
    REQUIRE(github.wait_for_waiting());
    controller.act(UpdateAction::Close);
    github.release();
    settle(controller, notices);
    REQUIRE(notices.items().size() == 1);
    const auto& notice = notices.items().front();
    if (installer.result) {
        CHECK(notice.kind == NoticeKind::Success);
        CHECK(notice.text.find(tr("cli.update_installed", {{"version", "1.3.0"}})) != std::string::npos);
        CHECK((notice.text.find(tr("cli.update_restart")) != std::string::npos) ==
              installer.result->restart_required);
    } else {
        CHECK(notice.kind == NoticeKind::Error);
        CHECK(notice.text == noteapp::core::format_error(installer.result.error()));
    }
}

TEST_CASE("update controller: closing the app stops a running check without waiting for the network") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    github.hold();
    FakeInstaller installer;
    UpdateController controller(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(controller.check_on_startup(std::chrono::system_clock::now(), kDaily));
    REQUIRE(github.wait_for_waiting());
    CHECK_FALSE(controller.stop_for_exit(std::chrono::seconds(5)));  // iptal edildi: çıkış beklemez
    CHECK_FALSE(controller.busy());
}

TEST_CASE("update controller: closing the app during an install that ignores cancel says the exit waits") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    std::mutex mutex;
    std::condition_variable_any finish;
    bool may_finish = false;
    bool installing = false;
    UpdateWorkerDeps d{.get = github.get(),
                       .install = [&](const InstallTarget&, const noteapp::app::ReleaseInfo&,
                                      std::string_view, const noteapp::core::UpdateLimits&,
                                      std::stop_token) -> noteapp::app::InstallResult {
                           std::unique_lock lock(mutex);
                           installing = true;
                           finish.notify_all();
                           finish.wait(lock, [&] { return may_finish; });  // değiştirme: iptali yok sayar
                           return InstallOutcome{};
                       },
                       .target = installable_target(),
                       .on_change = {}};
    UpdateController controller(update_settings("1.2.3"), std::move(d));
    NoticeQueue notices(kNoticeLife);
    controller.open(notices);
    settle(controller, notices);
    controller.act(UpdateAction::Install);
    {
        std::unique_lock lock(mutex);
        REQUIRE(finish.wait_for(lock, kWait, [&] { return installing; }));
    }
    CHECK(controller.stop_for_exit(std::chrono::milliseconds(20)));  // çıkış kurulumu bekleyecek
    {
        const std::scoped_lock lock(mutex);
        may_finish = true;
    }
    finish.notify_all();
    settle(controller, notices);
    CHECK(controller.view().lines.front() == tr("cli.update_installed", {{"version", "1.3.0"}}));
}
