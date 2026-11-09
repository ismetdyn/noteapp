// note update (SPEC-cli "Güncelleme"). Akış süreç içinde sahte GitHub ve sahte kurulumla koşar
// (tests/support/update_fixture.hpp); ağa çıkılmaz. Ayrıştırma ve kullanım hataları gerçek `note` ikilisiyle.
#include "cli/update_flow.hpp"
#include "core/i18n.hpp"
#include "support/cli_sandbox.hpp"
#include "support/update_fixture.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <sstream>
#include <string>

using noteapp::app::InstallKind;
using noteapp::app::InstallOutcome;
using noteapp::app::InstallTarget;
using noteapp::app::UpdateWorker;
using noteapp::app::UpdateWorkerDeps;
using noteapp::cli::UpdateConsole;
using noteapp::cli::UpdateOptions;
using noteapp::test::FakeGitHub;
using noteapp::test::FakeInstaller;
using noteapp::test::kLatestApiUrl;

namespace {

constexpr int kOk = 0;
constexpr int kUser = 1;
constexpr int kUsage = 2;
constexpr int kInternal = 3;
constexpr int kInterrupted = 130;

constexpr std::string_view kReleasePage = "https://github.com/ismetdyn/noteapp/releases/tag/v1.3.0";

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Bir `note update` çalıştırması: sahte GitHub, sahte kurulum, geçici NOTEAPP_HOME (durum dosyası oraya).
struct Run {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env{"NOTEAPP_HOME", home.path().u8string()};
    FakeGitHub github;
    FakeInstaller installer;
    InstallTarget target = noteapp::test::installable_target();
    bool tty = false;
    std::optional<std::string> answer;
    std::function<bool()> interrupted = [] { return false; };
    std::function<void()> reset_interrupt = [] {};
    std::ostringstream out;
    std::ostringstream err;
    int asked = 0;

    Run() { noteapp::core::init_i18n("en"); }

    int operator()(UpdateOptions options) {
        UpdateWorker worker(noteapp::test::update_settings("1.2.3"),
                            UpdateWorkerDeps{.get = github.get(),
                                             .install = installer.installer(),
                                             .target = target,
                                             .on_change = {}});
        UpdateConsole console{.out = out,
                              .err = err,
                              .stdin_tty = tty,
                              .read_line =
                                  [this] {
                                      ++asked;
                                      return answer;
                                  },
                              .interrupted = interrupted,
                              .reset_interrupt = reset_interrupt};
        return noteapp::cli::run_update_flow(options, worker, console);
    }
};

}  // namespace

TEST_CASE("update --check: up to date prints the installed and the latest version, exit 0") {
    Run run;
    run.github.publish("1.2.3");
    CHECK(run({.check_only = true}) == kOk);
    CHECK(contains(run.out.str(), "1.2.3"));
    CHECK(contains(run.out.str(), "up to date"));
    CHECK(run.err.str().empty());
}

TEST_CASE("update --check: a newer release prints its version and installs nothing, exit 0") {
    Run run;
    run.github.publish("1.3.0");
    CHECK(run({.check_only = true}) == kOk);
    CHECK(contains(run.out.str(), "1.3.0"));
    CHECK(contains(run.out.str(), "available"));
    CHECK(run.installer.calls() == 0);
    CHECK(run.asked == 0);
}

TEST_CASE("update --check: a failed check prints the reason on stderr with exit 3") {
    Run run;
    run.github.respond(std::string(kLatestApiUrl), noteapp::test::http_reply(500));
    CHECK(run({.check_only = true}) == kInternal);
    CHECK(run.out.str().empty());
    CHECK(contains(run.err.str(), "HTTP 500"));
}

TEST_CASE("update: up to date installs nothing and asks nothing") {
    Run run;
    run.github.publish("1.2.3");
    run.tty = true;
    CHECK(run({}) == kOk);
    CHECK(contains(run.out.str(), "up to date"));
    CHECK(run.asked == 0);
    CHECK(run.installer.calls() == 0);
}

TEST_CASE("update: without a terminal and without --yes it refuses to install, exit 1") {
    Run run;
    run.github.publish("1.3.0");
    run.tty = false;
    CHECK(run({}) == kUser);
    CHECK(contains(run.out.str(), "1.3.0"));
    CHECK(contains(run.err.str(), "--yes"));
    CHECK(run.asked == 0);
    CHECK(run.installer.calls() == 0);
}

TEST_CASE("update --yes installs the verified package without asking") {
    Run run;
    run.github.publish("1.3.0");
    CHECK(run({.yes = true}) == kOk);
    CHECK(run.asked == 0);
    REQUIRE(run.installer.calls() == 1);
    CHECK(run.installer.installed.front() == noteapp::test::kUpdatePayload);
    CHECK(contains(run.out.str(), "1.3.0 is installed"));
    CHECK_FALSE(contains(run.out.str(), "Restart"));
}

TEST_CASE("update on a terminal installs only after a yes") {
    SUBCASE("yes") {
        Run run;
        run.github.publish("1.3.0");
        run.tty = true;
        run.answer = "y";
        CHECK(run({}) == kOk);
        CHECK(run.asked == 1);
        CHECK(contains(run.err.str(), "Install noteapp 1.3.0?"));
        CHECK(run.installer.calls() == 1);
    }
    SUBCASE("no, empty or end of input") {
        for (const std::optional<std::string>& answer :
             {std::optional<std::string>("n"), std::optional<std::string>(""),
              std::optional<std::string>("yes"), std::optional<std::string>()}) {
            Run run;
            run.github.publish("1.3.0");
            run.tty = true;
            run.answer = answer;
            CHECK(run({}) == kUser);
            CHECK(contains(run.err.str(), "nothing was installed"));
            CHECK(run.installer.calls() == 0);
        }
    }
}

TEST_CASE("update: a Windows install asks to restart") {
    Run run;
    run.github.publish("1.3.0");
    run.installer.result = InstallOutcome{.restart_required = true};
    CHECK(run({.yes = true}) == kOk);
    CHECK(contains(run.out.str(), "Restart"));
}

TEST_CASE("update: a failed download or install is reported with exit 3") {
    Run run;
    run.github.publish("1.3.0");
    run.github.respond(noteapp::test::release_download_url("v1.3.0", "noteapp-1.3.0-x86_64.AppImage"),
                       noteapp::test::http_reply(200, "tampered"));
    CHECK(run({.yes = true}) == kInternal);
    CHECK(contains(run.err.str(), "SHA-256"));
    CHECK(run.installer.calls() == 0);
}

TEST_CASE("update: an install that cannot update itself names the release page") {
    for (const bool check_only : {false, true}) {
        CAPTURE(check_only);
        Run run;
        run.github.publish("1.3.0");
        run.target = InstallTarget{.kind = InstallKind::Unsupported, .path = {}};
        run.tty = true;
        run.answer = "y";
        CHECK(run({.check_only = check_only, .yes = true}) == (check_only ? kOk : kUser));
        const std::string all = run.out.str() + run.err.str();
        CHECK(contains(all, "cannot update itself"));
        CHECK(contains(all, kReleasePage));
        CHECK(run.asked == 0);
        CHECK(run.installer.calls() == 0);
    }
}

TEST_CASE("update: Ctrl-C during the download cancels it, installs nothing, exit 130") {
    Run run;
    run.github.publish("1.3.0");
    // Denetim geçer; indirme SHA256SUMS isteğinde bekler. Bekleyen istek görülünce Ctrl-C gelmiş sayılır.
    run.github.hold_only(noteapp::test::release_download_url("v1.3.0", "SHA256SUMS"));
    run.interrupted = [&run] { return run.github.waiting() > 0; };
    CHECK(run({.yes = true}) == kInterrupted);
    CHECK(run.installer.calls() == 0);
    CHECK(contains(run.err.str(), "cancelled"));
    CHECK_FALSE(contains(run.out.str(), "is installed"));
}

TEST_CASE("update: a Ctrl-C left over from the check is cleared before asking and installing") {
    Run run;
    run.github.publish("1.3.0");
    run.tty = true;
    run.answer = "y";
    int resets = 0;
    int asked_at_reset = -1;
    std::size_t installs_at_reset = 99;
    run.reset_interrupt = [&] {
        ++resets;
        asked_at_reset = run.asked;
        installs_at_reset = run.installer.calls();
    };
    CHECK(run({}) == kOk);
    CHECK(resets == 1);
    CHECK(asked_at_reset == 0);
    CHECK(installs_at_reset == 0);
    CHECK(run.installer.calls() == 1);
}

TEST_CASE("update on a terminal: an install that cannot update itself asks nothing, exit 1") {
    Run run;
    run.github.publish("1.3.0");
    run.target = InstallTarget{.kind = InstallKind::Unsupported, .path = {}};
    run.tty = true;
    run.answer = "y";
    CHECK(run({}) == kUser);
    CHECK(run.asked == 0);
    CHECK(contains(run.err.str(), "cannot update itself"));
    CHECK(contains(run.err.str(), kReleasePage));
    CHECK(run.installer.calls() == 0);
}

TEST_CASE("update: an installer error is reported on stderr with exit 3") {
    Run run;
    run.github.publish("1.3.0");
    run.installer.result = std::unexpected(noteapp::core::Error{
        noteapp::core::Error::Code::Io, "update.install_busy", {{"path", "/opt/noteapp"}}});
    CHECK(run({.yes = true}) == kInternal);
    CHECK(contains(run.err.str(), "Another noteapp update is running in /opt/noteapp"));
    CHECK_FALSE(contains(run.out.str(), "is installed"));
}

TEST_CASE("note update rejects unknown options and arguments with exit 2") {
    const noteapp::test::Sandbox box("en");
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"update", "--bogus"}, std::vector<std::string>{"update", "now"}}) {
        const auto result = box.run(args);
        CHECK(result.exit_code == kUsage);
        CHECK(contains(result.err, "note help update"));
        CHECK(result.out.empty());
    }
}
