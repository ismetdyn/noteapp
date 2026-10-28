// AutoSync (SPEC-sync kabul 9, karar 3): her senkron ayrı bir `note sync --background` sürecidir. Çoğu testte
// `note` yerine fake_editor koşar (FAKE_EDITOR_SLEEP_MS ile asılı kalan senkronu, PID_OUT ile çocuğu taklit
// eder); sondaki test gerçek `note` ikilisiyle uçtan uca.
#include "core/config.hpp"
#include "core/paths.hpp"
#include "sync/auto_sync.hpp"
#include "sync/sync_service.hpp"
#include "sync_fixture.hpp"

#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#endif

namespace fs = std::filesystem;
using noteapp::sync::AutoSync;
using noteapp::sync::SyncService;
using noteapp::sync::SyncStatus;
using namespace noteapp::sync_test;
using namespace std::chrono_literals;

namespace {

const fs::path kFakeNote = NOTEAPP_FAKE_EDITOR;
const fs::path kRealNote = NOTEAPP_NOTE_EXE;

// AutoSync'in çağrılarını (işçi iş parçacığından) sayar; testin iş parçacığı koşulu süreyle bekler.
class Recorder {
public:
    AutoSync::StatusCallback callback() {
        return [this](const SyncStatus& status, bool running) {
            const std::lock_guard lock(mutex_);
            if (running) {
                ++started_;
            } else {
                ++finished_;
                last_ = status;
            }
            changed_.notify_all();
        };
    }
    bool wait_started(int count, std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return started_ >= count; });
    }
    bool wait_finished(int count, std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return finished_ >= count; });
    }
    int started() {
        const std::lock_guard lock(mutex_);
        return started_;
    }
    int finished() {
        const std::lock_guard lock(mutex_);
        return finished_;
    }
    std::optional<SyncStatus> last() {
        const std::lock_guard lock(mutex_);
        return last_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int started_ = 0;
    int finished_ = 0;
    std::optional<SyncStatus> last_;
};

noteapp::core::Config config_with(int idle_seconds, int timeout_seconds) {
    noteapp::core::Config config = noteapp::core::parse_config("").config;
    config.sync.idle_seconds = idle_seconds;
    config.tuning.sync_timeout_seconds = timeout_seconds;
    return config;
}

bool process_alive(long long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

// Çocuğun PID_OUT ile yazdığı PID; dosya belirene kadar (en çok `limit`) bekler.
long long wait_for_pid(const fs::path& file, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const std::string text = read_file(file); !text.empty())
            return std::stoll(text);
        std::this_thread::sleep_for(20ms);
    }
    return 0;
}

}  // namespace

TEST_CASE("acceptance 9: a burst of notify_changed runs one sync, after the idle time") {
    const SyncFixture fx;
    Recorder recorder;
    AutoSync auto_sync(config_with(1, 30), kFakeNote, recorder.callback());
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        auto_sync.notify_changed();
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(recorder.wait_finished(1, 10s));
    CHECK(std::chrono::steady_clock::now() - start >= 1s);  // son bildirimden sonra idle süre doldu
    std::this_thread::sleep_for(1500ms);
    CHECK(recorder.finished() == 1);
    CHECK(recorder.started() == 1);
    auto_sync.stop(5s);
    CHECK(recorder.finished() == 1);  // bekleyen değişiklik yok: çıkışta yeni senkron yok
}

TEST_CASE("start syncs once at once; request_now syncs again without waiting for the idle time") {
    const SyncFixture fx;
    Recorder recorder;
    AutoSync auto_sync(config_with(30, 30), kFakeNote, recorder.callback());
    auto_sync.start();
    REQUIRE(recorder.wait_finished(1, 10s));
    auto_sync.request_now();
    REQUIRE(recorder.wait_finished(2, 10s));
    CHECK(recorder.started() == 2);
}

TEST_CASE("a sync that outlives tuning.sync_timeout_seconds is killed and reported as sync.timeout") {
    const SyncFixture fx;
    const fs::path pid_file = fx.root() / "pid.txt";
    const EnvGuard sleep("FAKE_EDITOR_SLEEP_MS", fs::path("600000"));
    const EnvGuard pid_out("FAKE_EDITOR_PID_OUT", pid_file);
    Recorder recorder;
    AutoSync auto_sync(config_with(30, 1), kFakeNote, recorder.callback());

    const auto start = std::chrono::steady_clock::now();
    auto_sync.request_now();
    REQUIRE(recorder.wait_finished(1, 10s));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 1s);
    CHECK(elapsed < 5s);
    REQUIRE(recorder.last()->last_error.has_value());
    CHECK(recorder.last()->last_error->message_key == "sync.timeout");
    // Öldürülen süreç durumu yazamadı; AutoSync yazar (`note sync status` da görsün).
    const SyncStatus persisted = SyncService(noteapp::core::parse_config("").config).status();
    REQUIRE(persisted.last_error.has_value());
    CHECK(persisted.last_error->message_key == "sync.timeout");
    const long long pid = wait_for_pid(pid_file, 1s);
    REQUIRE(pid > 0);
    CHECK_FALSE(process_alive(pid));
}

TEST_CASE("stop() never waits longer than its timeout and kills the running sync") {
    const SyncFixture fx;
    const fs::path pid_file = fx.root() / "pid.txt";
    const EnvGuard sleep("FAKE_EDITOR_SLEEP_MS", fs::path("600000"));
    const EnvGuard pid_out("FAKE_EDITOR_PID_OUT", pid_file);
    Recorder recorder;
    AutoSync auto_sync(config_with(30, 600), kFakeNote, recorder.callback());
    auto_sync.request_now();
    const long long pid = wait_for_pid(pid_file, 10s);
    REQUIRE(pid > 0);

    const auto start = std::chrono::steady_clock::now();
    auto_sync.stop(1s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 900ms);
    CHECK(elapsed < 3s);
    CHECK_FALSE(process_alive(pid));
    CHECK(recorder.finished() == 1);
}

TEST_CASE("stop() runs a last sync for pending changes, and none without them") {
    const SyncFixture fx;
    {
        Recorder recorder;
        AutoSync auto_sync(config_with(30, 30), kFakeNote, recorder.callback());
        auto_sync.notify_changed();  // idle 30 sn: kendiliğinden koşmazdı
        auto_sync.stop(10s);
        CHECK(recorder.finished() == 1);
    }
    {
        Recorder recorder;
        AutoSync auto_sync(config_with(30, 30), kFakeNote, recorder.callback());
        auto_sync.stop(10s);
        CHECK(recorder.started() == 0);
    }
}

TEST_CASE("a missing note executable is reported in the status, not thrown") {
    const SyncFixture fx;
    Recorder recorder;
    AutoSync auto_sync(config_with(30, 30), fx.root() / "yok" / "note", recorder.callback());
    auto_sync.request_now();
    REQUIRE(recorder.wait_finished(1, 10s));
    REQUIRE(recorder.last()->last_error.has_value());
    CHECK(recorder.last()->last_error->message_key == "error.process_not_found");
}

TEST_CASE("end to end: AutoSync runs the real `note sync --background` and the note reaches the remote") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    write_file(a / "not.md", "arka plan\n");
    std::string notes = utf8(a);
    write_file(noteapp::core::config_file(), "language = \"en\"\nnotes_dir = \"" + notes +
                                                 "\"\n[sync]\nenabled = true\nauthor_name = \"Test\"\n"
                                                 "author_email = \"test@noteapp.invalid\"\n");
    Recorder recorder;
    AutoSync auto_sync(noteapp::core::load_config().config, kRealNote, recorder.callback());
    auto_sync.request_now();
    REQUIRE(recorder.wait_finished(1, 60s));
    const auto status = recorder.last();
    CHECK_MESSAGE(!status->last_error.has_value(),
                  (status->last_error ? status->last_error->message_key : std::string()));
    CHECK(status->last_success.has_value());
    CHECK(commit_count(fx.remote()) == 1);
    auto_sync.stop(5s);
}
