#include "core/child_process.hpp"

#include "support/process_probe.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

using noteapp::core::ChildProcess;
using namespace noteapp::test;
using namespace std::chrono_literals;

namespace {

const std::string kHelper = NOTEAPP_FAKE_EDITOR;
using Clock = ChildProcess::Clock;
using Status = ChildProcess::ReadStatus;

// `text` gelene ya da EOF/hata/süre dolana dek okur; okunan her şey döner.
std::string read_until(ChildProcess& child, std::string_view text, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    std::string out;
    while (out.find(text) == std::string::npos && Clock::now() < deadline)
        if (const Status status = child.read_some(out, deadline); status == Status::Eof || status == Status::Failed)
            break;
    return out;
}

// EOF'a kadar okur; EOF gelmeden süre dolarsa nullopt.
std::optional<std::string> read_to_eof(ChildProcess& child, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    std::string out;
    while (Clock::now() < deadline) {
        const Status status = child.read_some(out, deadline);
        if (status == Status::Eof)
            return out;
        if (status == Status::Failed)
            return std::nullopt;
    }
    return std::nullopt;
}

long long wait_for_pid(const fs::path& file) {
    long long pid = 0;
    wait_until(
        [&] {
            pid = fs::exists(file) ? std::atoll(slurp(file).c_str()) : 0;
            return pid > 0;
        },
        10s);
    return pid;
}

}  // namespace

TEST_CASE("child process: stdin and stdout round trip, then EOF and exit code") {
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_ECHO_STDIN=1"});
    REQUIRE(child.has_value());
    REQUIRE(child->write_all("ping\n", Clock::now() + 10s));
    CHECK(read_until(*child, "ping\n", 10s) == "ping\n");
    // Aynı çocukla ikinci tur: hunspell gibi uzun ömürlü konuşma.
    REQUIRE(child->write_all("şöyle pong\n", Clock::now() + 10s));
    CHECK(read_until(*child, "pong\n", 10s) == "şöyle pong\n");
    child->close_stdin();
    CHECK(read_to_eof(*child, 10s) == std::optional<std::string>(""));
    CHECK(child->wait_until(Clock::now() + 10s) == std::optional<int>(0));
}

TEST_CASE("child process: a child that exits at once gives EOF and its exit code") {
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_EXIT=7"});
    REQUIRE(child.has_value());
    CHECK(read_to_eof(*child, 10s) == std::optional<std::string>(""));
    CHECK(child->wait_until(Clock::now() + 10s) == std::optional<int>(7));
}

TEST_CASE("child process: reading from a silent child times out at the deadline") {
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_SLEEP_MS=600000"});
    REQUIRE(child.has_value());
    const auto start = Clock::now();
    std::string out;
    CHECK(child->read_some(out, start + 200ms) == Status::Timeout);
    const auto waited = Clock::now() - start;
    CHECK(waited >= 150ms);
    CHECK(waited < 5s);
    CHECK(out.empty());
    CHECK_FALSE(child->wait_until(Clock::now() + 50ms).has_value());
}

TEST_CASE("child process: a large output is read in full") {
    constexpr std::size_t kBytes = 4 * 1024 * 1024;
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_FLOOD_BYTES=" + std::to_string(kBytes)});
    REQUIRE(child.has_value());
    const auto out = read_to_eof(*child, 30s);
    REQUIRE(out.has_value());
    CHECK(out->size() == kBytes);
    CHECK(out->find_first_not_of('x') == std::string::npos);
    CHECK(child->wait_until(Clock::now() + 10s) == std::optional<int>(0));
}

TEST_CASE("child process: a write to a child that never reads stops at the deadline") {
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_SLEEP_MS=600000"});
    REQUIRE(child.has_value());
    const std::string payload(32 * 1024 * 1024, 'x');  // her boru tamponundan büyük
    const auto start = Clock::now();
    CHECK_FALSE(child->write_all(payload, start + 300ms));
    CHECK(Clock::now() - start >= 250ms);
    CHECK(Clock::now() - start < 5s);
}

TEST_CASE("child process: a write blocked on a full pipe is released by a stop request") {
    auto child = ChildProcess::start({kHelper}, true, {"FAKE_EDITOR_FLOOD_BYTES=1073741824"});
    REQUIRE(child.has_value());
    const std::string payload(32 * 1024 * 1024, 'x');
    std::stop_source stop;
    const auto start = Clock::now();
    const std::jthread requester([&] {
        std::this_thread::sleep_for(200ms);
        stop.request_stop();
    });
    CHECK_FALSE(child->write_all(payload, start + 60s, stop.get_token()));
    CHECK(Clock::now() - start >= 150ms);
    CHECK(Clock::now() - start < 5s);
}

TEST_CASE("child process: destruction kills the child and its grandchild, even one blocked writing") {
    const TempDir dir;
    const fs::path child_pid = dir.path() / "child.pid";
    const fs::path grandchild_pid = dir.path() / "grandchild.pid";
    long long child_id = 0;
    long long grandchild_id = 0;
    const auto start = Clock::now();
    {
        // Çıktısı okunmayan çocuk dolu boruda yazarken takılır; torun 10 dakika uyur.
        auto child = ChildProcess::start({kHelper}, true,
                                         {"FAKE_EDITOR_PID_OUT=" + utf8(child_pid),
                                          "FAKE_EDITOR_GRANDCHILD_PID_OUT=" + utf8(grandchild_pid),
                                          "FAKE_EDITOR_FLOOD_BYTES=1073741824"});
        REQUIRE(child.has_value());
        child_id = wait_for_pid(child_pid);
        grandchild_id = wait_for_pid(grandchild_pid);
        REQUIRE(child_id > 0);
        REQUIRE(grandchild_id > 0);
        REQUIRE(process_alive(grandchild_id));
    }
    CHECK(Clock::now() - start < 20s);
    CHECK(wait_until([&] { return !process_alive(child_id); }, 2s));
    const bool grandchild_gone = wait_until([&] { return !process_alive(grandchild_id); }, 2s);
    CHECK(grandchild_gone);
    if (!grandchild_gone)
        kill_process(grandchild_id);
}

TEST_CASE("child process: the sync token and AppImage variables do not reach the child; extra_env does") {
    const TempDir dir;
    const fs::path env_file = dir.path() / "env.txt";
    const ScopedEnv token("NOTEAPP_GIT_TOKEN", "token-must-not-leak");
    const ScopedEnv token_host("NOTEAPP_GIT_TOKEN_HOST", "host-must-not-leak");
    const ScopedEnv appimage("APPIMAGE", "appimage-must-not-leak");
    auto child = ChildProcess::start({kHelper}, false,
                                     {"FAKE_EDITOR_ENV_OUT=" + utf8(env_file), "NOTEAPP_CHILD_PROBE=visible"});
    REQUIRE(child.has_value());
    REQUIRE(child->wait_until(Clock::now() + 10s) == std::optional<int>(0));
    const std::string env = slurp(env_file);
    CHECK(env.find("NOTEAPP_CHILD_PROBE=visible") != std::string::npos);
    CHECK(env.find("token-must-not-leak") == std::string::npos);
    CHECK(env.find("host-must-not-leak") == std::string::npos);
    CHECK(env.find("appimage-must-not-leak") == std::string::npos);
}

TEST_CASE("child process: the host GIO_MODULE_DIR saved by AppRun replaces the bundled one") {
    const TempDir dir;
    const fs::path env_file = dir.path() / "env.txt";
    const ScopedEnv bundled("GIO_MODULE_DIR", "/package/.no-gio-modules");
    const ScopedEnv saved("NOTEAPP_HOST_GIO_MODULE_DIR", "/host/gio/modules");
    auto child = ChildProcess::start({kHelper}, false, {"FAKE_EDITOR_ENV_OUT=" + utf8(env_file)});
    REQUIRE(child.has_value());
    REQUIRE(child->wait_until(Clock::now() + 10s) == std::optional<int>(0));
    const std::string env = slurp(env_file);
    CHECK(("\n" + env).find("\nGIO_MODULE_DIR=/host/gio/modules\n") != std::string::npos);
    CHECK(env.find("no-gio-modules") == std::string::npos);
    CHECK(env.find("NOTEAPP_HOST_GIO") == std::string::npos);
}

TEST_CASE("child process: a missing program or an empty command does not start") {
    CHECK_FALSE(ChildProcess::start({"noteapp-no-such-program-t59d"}, true).has_value());
    CHECK_FALSE(ChildProcess::start({}, true).has_value());
}

#ifdef _WIN32
// .cmd yalnız cmd.exe üzerinden çalışır; cmd.exe argümanı yeniden ayrıştırdığından metakarakter reddedilir.
TEST_CASE("child process: batch scripts run through cmd.exe and reject metacharacters") {
    const TempDir dir;
    const fs::path script = dir.path() / "tool.cmd";
    std::ofstream(script) << "@exit /b 3\r\n";
    auto plain = ChildProcess::start({utf8(script), "plain arg"}, false);
    REQUIRE(plain.has_value());
    CHECK(plain->wait_until(Clock::now() + 10s) == std::optional<int>(3));
    for (const char* bad : {"a\"b", "50%", "x!y", "a^b", "a&b", "a|b", "a<b", "a>b"})
        CHECK_FALSE(ChildProcess::start({utf8(script), bad}, false).has_value());
}
#endif
