// app::UpdateWorker: denetim ve kurulum işçi iş parçacığında, tek işlem, iptal edilebilir. HTTP ve kurulum
// sahtedir (tests/support/update_fixture.hpp); testler ağa çıkmaz, durum dosyası geçici NOTEAPP_HOME'dadır.
#include "app/update_worker.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"
#include "support/update_fixture.hpp"

#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

using noteapp::app::InstallKind;
using noteapp::app::InstallOutcome;
using noteapp::app::InstallTarget;
using noteapp::app::UpdateAvailable;
using noteapp::app::UpdateChecking;
using noteapp::app::UpdateDone;
using noteapp::app::UpdateDownloading;
using noteapp::app::UpdateFailed;
using noteapp::app::UpdateIdle;
using noteapp::app::UpdateStatus;
using noteapp::app::UpdateWorker;
using noteapp::app::UpdateWorkerDeps;
using noteapp::app::UpToDate;
using noteapp::core::Error;
using noteapp::test::FakeGitHub;
using noteapp::test::FakeInstaller;
using noteapp::test::installable_target;
using noteapp::test::kLatestApiUrl;
using noteapp::test::kUpdatePayload;
using noteapp::test::update_settings;
namespace fs = std::filesystem;

namespace {

constexpr auto kWait = std::chrono::seconds(10);

// Durum dosyası (update-state.toml) geçici dizine yazılır, kullanıcının gerçek dizinine değil.
struct TempHome {
    noteapp::test::TempDir dir;
    noteapp::test::ScopedEnv env{"NOTEAPP_HOME", dir.path().u8string()};
};

UpdateWorkerDeps deps(FakeGitHub& github, FakeInstaller& installer,
                      InstallTarget target = installable_target()) {
    return UpdateWorkerDeps{
        .get = github.get(), .install = installer.installer(), .target = std::move(target), .on_change = {}};
}

void wait_idle(UpdateWorker& worker) {
    REQUIRE(worker.wait_idle_for(kWait));
}

// Kopya döner: status() bir anlık görüntüdür (geçici değer).
template <class T>
T as(const UpdateStatus& status) {
    REQUIRE(std::holds_alternative<T>(status));
    return std::get<T>(status);
}

}  // namespace

TEST_CASE("update worker: starts idle; a check finds a newer release that this install can update to") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    CHECK(std::holds_alternative<UpdateIdle>(worker.status()));
    CHECK_FALSE(worker.busy());

    REQUIRE(worker.check());
    wait_idle(worker);
    const auto available = as<UpdateAvailable>(worker.status());
    CHECK(available.version == "1.3.0");
    CHECK(available.release_page == "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0");
    CHECK(available.installable);
    CHECK(installer.calls() == 0);  // denetim kurmaz
}

TEST_CASE("update worker: an equal release is up to date") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.2.3");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    wait_idle(worker);
    CHECK(as<UpToDate>(worker.status()).latest_version == "1.2.3");
    CHECK_FALSE(worker.install());  // kurulacak sürüm yok
}

TEST_CASE("update worker: install downloads, verifies and installs the found release, then is done") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    installer.result = InstallOutcome{.restart_required = true};
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    CHECK_FALSE(worker.install());  // denetimden önce kurulacak sürüm bilinmez
    REQUIRE(worker.check());
    wait_idle(worker);

    REQUIRE(worker.install());
    wait_idle(worker);
    const auto done = as<UpdateDone>(worker.status());
    CHECK(done.version == "1.3.0");
    CHECK(done.restart_required);
    REQUIRE(installer.calls() == 1);
    CHECK(installer.installed.front() == kUpdatePayload);
    CHECK_FALSE(worker.install());  // aynı sürüm ikinci kez kurulmaz
}

TEST_CASE("update worker: a failed check, download or install surfaces as its Error") {
    const TempHome home;
    FakeGitHub github;
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));

    github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
    REQUIRE(worker.check());
    wait_idle(worker);
    CHECK(as<UpdateFailed>(worker.status()).error.message_key == "update.http_status");

    github.publish("1.3.0");
    github.respond(noteapp::test::release_download_url("v1.3.0", "noteapp-1.3.0-x86_64.AppImage"),
                   noteapp::test::http_reply(200, "tampered"));
    REQUIRE(worker.check());
    wait_idle(worker);
    REQUIRE(worker.install());
    wait_idle(worker);
    CHECK(as<UpdateFailed>(worker.status()).error.message_key == "update.checksum_mismatch");
    CHECK(installer.calls() == 0);  // doğrulanmayan paket kurulmaz

    github.publish("1.3.0");
    installer.result = std::unexpected(Error{Error::Code::Io, "update.old_in_use", {{"path", "x"}}});
    REQUIRE(worker.install());  // başarısız kurulum yeniden denenebilir
    wait_idle(worker);
    CHECK(as<UpdateFailed>(worker.status()).error.message_key == "update.old_in_use");
}

TEST_CASE("update worker: a failed check forgets the release an earlier check found") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    wait_idle(worker);
    REQUIRE(std::holds_alternative<UpdateAvailable>(worker.status()));

    github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
    REQUIRE(worker.check());
    wait_idle(worker);
    REQUIRE(std::holds_alternative<UpdateFailed>(worker.status()));
    CHECK_FALSE(worker.install());  // Failed durumunda kurulacak sürüm yok
    CHECK(installer.calls() == 0);
}

TEST_CASE("update worker: a cancelled check keeps the release an earlier check found") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    wait_idle(worker);
    github.hold();
    REQUIRE(worker.check());
    REQUIRE(github.wait_for_waiting());
    worker.cancel();
    wait_idle(worker);
    CHECK(std::holds_alternative<UpdateAvailable>(worker.status()));
    github.release();
    CHECK(worker.install());
    wait_idle(worker);
}

TEST_CASE("update worker: an exception in the installer is a failure, not a crash") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    UpdateWorkerDeps d{.get = github.get(),
                       .install = [](const InstallTarget&, const noteapp::app::ReleaseInfo&, std::string_view,
                                     const noteapp::core::UpdateLimits&,
                                     std::stop_token) -> noteapp::app::InstallResult {
                           throw std::runtime_error("boom");
                       },
                       .target = installable_target(),
                       .on_change = {}};
    UpdateWorker worker(update_settings("1.2.3"), std::move(d));
    REQUIRE(worker.check());
    wait_idle(worker);
    REQUIRE(worker.install());
    wait_idle(worker);
    CHECK(as<UpdateFailed>(worker.status()).error.message_key == "update.internal_error");
}

TEST_CASE("update worker: two threads starting at once start one operation") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    github.hold();
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    std::atomic<int> started{0};
    {
        std::jthread first([&] { started += worker.check() ? 1 : 0; });
        std::jthread second([&] { started += worker.check() ? 1 : 0; });
    }
    CHECK(started.load() == 1);
    REQUIRE(github.wait_for_waiting());
    github.release();
    wait_idle(worker);
    CHECK(github.requests() == 1);
}

TEST_CASE("update worker: one operation at a time") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    github.hold();
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    REQUIRE(github.wait_for_waiting());
    CHECK(worker.busy());
    CHECK(std::holds_alternative<UpdateChecking>(worker.status()));
    CHECK_FALSE(worker.check());
    CHECK_FALSE(worker.install());
    CHECK_FALSE(worker.wait_idle_for(std::chrono::milliseconds(50)));

    github.release();
    wait_idle(worker);
    CHECK(github.requests() == 1);  // ikinci denetim hiç başlamadı
    CHECK(std::holds_alternative<UpdateAvailable>(worker.status()));
}

TEST_CASE("update worker: cancel stops a check and brings back the previous state") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    github.hold();
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    REQUIRE(github.wait_for_waiting());
    worker.cancel();
    wait_idle(worker);
    CHECK(std::holds_alternative<UpdateIdle>(worker.status()));
    CHECK_FALSE(noteapp::app::load_update_state().last_check_unix.has_value());  // iptal denetim sayılmaz
}

TEST_CASE("update worker: cancel during the download installs nothing and the release stays installable") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
    REQUIRE(worker.check());
    wait_idle(worker);

    github.hold();
    REQUIRE(worker.install());
    REQUIRE(github.wait_for_waiting());
    CHECK(as<UpdateDownloading>(worker.status()).version == "1.3.0");
    worker.cancel();
    wait_idle(worker);
    CHECK(as<UpdateAvailable>(worker.status()).version == "1.3.0");
    CHECK(installer.calls() == 0);

    github.release();
    REQUIRE(worker.install());
    wait_idle(worker);
    CHECK(std::holds_alternative<UpdateDone>(worker.status()));
}

TEST_CASE("update worker: an install that ignores cancel after its point of no return is done") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    UpdateWorker* self = nullptr;
    UpdateWorkerDeps d{.get = github.get(),
                       .install =
                           [&self](const InstallTarget&, const noteapp::app::ReleaseInfo&, std::string_view,
                                   const noteapp::core::UpdateLimits&,
                                   std::stop_token) -> noteapp::app::InstallResult {
                           self->cancel();  // değiştirme başladı: kurulum iptali yok sayar
                           return InstallOutcome{};
                       },
                       .target = installable_target(),
                       .on_change = {}};
    UpdateWorker worker(update_settings("1.2.3"), std::move(d));
    self = &worker;
    REQUIRE(worker.check());
    wait_idle(worker);
    REQUIRE(worker.install());
    wait_idle(worker);
    CHECK(as<UpdateDone>(worker.status()).version == "1.3.0");
}

TEST_CASE("update worker: an install kind that cannot update itself is offered the release page only") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"),
                        deps(github, installer, InstallTarget{.kind = InstallKind::Unsupported, .path = {}}));
    REQUIRE(worker.check());
    wait_idle(worker);
    const auto available = as<UpdateAvailable>(worker.status());
    CHECK_FALSE(available.installable);
    CHECK(available.release_page == "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0");
    CHECK_FALSE(worker.install());
    CHECK(installer.calls() == 0);
}

TEST_CASE("update worker: a completed check saves its time and the latest version; a failed one does not") {
    const TempHome home;
    FakeGitHub github;
    FakeInstaller installer;
    UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));

    github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
    REQUIRE(worker.check());
    wait_idle(worker);
    CHECK_FALSE(fs::exists(noteapp::core::state_dir() / "update-state.toml"));

    github.publish("1.3.0");
    const auto unix_now = [] {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
            .time_since_epoch()
            .count();
    };
    const std::int64_t before = unix_now();
    REQUIRE(worker.check());
    wait_idle(worker);
    const std::int64_t after = unix_now();
    const auto state = noteapp::app::load_update_state();
    REQUIRE(state.last_check_unix.has_value());
    CHECK(*state.last_check_unix >= before);
    CHECK(*state.last_check_unix <= after);
    CHECK(state.last_seen_version == "1.3.0");
}

TEST_CASE("update worker: every state change is announced") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    FakeInstaller installer;
    std::atomic<int> changes{0};
    UpdateWorkerDeps d = deps(github, installer);
    d.on_change = [&changes] { ++changes; };
    UpdateWorker worker(update_settings("1.2.3"), std::move(d));
    // Son duyuru durum yazıldıktan sonra gelir: bekleyen uyanmış olabilir, sayı kısa bir süre geride kalır.
    const auto announced = [&changes](int count) {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (changes.load() < count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return changes.load();
    };
    REQUIRE(worker.check());
    wait_idle(worker);
    CHECK(announced(2) == 2);  // Checking, Available
    REQUIRE(worker.install());
    wait_idle(worker);
    CHECK(announced(5) == 5);  // Downloading, Installing, Done
}

TEST_CASE("update worker: destroying a busy worker cancels and waits for it") {
    const TempHome home;
    FakeGitHub github;
    github.publish("1.3.0");
    github.hold();
    FakeInstaller installer;
    {
        UpdateWorker worker(update_settings("1.2.3"), deps(github, installer));
        REQUIRE(worker.check());
        REQUIRE(github.wait_for_waiting());
    }  // askıda kalırsa test zaman aşımına düşer
    CHECK(github.requests() == 1);
}

TEST_CASE("update worker: an install error after cancel is reported unless the installer says cancelled") {
    for (const auto& [key, cancelled] : {std::pair{"update.rollback_failed", false},
                                         std::pair{"update.install_cancelled", true}}) {
        CAPTURE(key);
        const TempHome home;
        FakeGitHub github;
        github.publish("1.3.0");
        UpdateWorker* self = nullptr;
        UpdateWorkerDeps d{.get = github.get(),
                           .install =
                               [&self, key](const InstallTarget&, const noteapp::app::ReleaseInfo&,
                                            std::string_view, const noteapp::core::UpdateLimits&,
                                            std::stop_token) -> noteapp::app::InstallResult {
                               self->cancel();  // Ctrl-C kurulum sürerken
                               return std::unexpected(Error{Error::Code::Io, key, {}});
                           },
                           .target = installable_target(),
                           .on_change = {}};
        UpdateWorker worker(update_settings("1.2.3"), std::move(d));
        self = &worker;
        REQUIRE(worker.check());
        wait_idle(worker);
        REQUIRE(worker.install());
        wait_idle(worker);
        if (cancelled)
            CHECK(as<UpdateAvailable>(worker.status()).version == "1.3.0");
        else
            CHECK(as<UpdateFailed>(worker.status()).error.message_key == key);  // yarım kurulum gizlenmez
    }
}
