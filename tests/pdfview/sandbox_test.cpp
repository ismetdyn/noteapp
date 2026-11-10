// PDF-SBX: PDF işçisinin kum havuzu (pdfview::enter_sandbox + core::ChildProcess::start_confined). Prob
// (tests/support/pdf_sandbox_probe.cpp) işçiyle aynı yoldan kum havuzuna girer ve tek bir çağrı dener. Olumsuz problar
// izin listesinin dışını, olumlu problar işçinin gerçekten yaptığı işi sınar: filtre her şeyi öldürseydi olumsuzlar
// yine geçerdi, olumlular düşer (yanlış yeşil koruması).
#include "core/child_process.hpp"
#include "pdfview/built_executable.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>

#if defined(__linux__) || defined(_WIN32)

using namespace noteapp;

namespace {

const std::string kProbe = test::built_executable_utf8(NOTEAPP_PDF_SANDBOX_PROBE);
// Probun alloc_64MiB ve alloc_over'ı bu sınıra göre seçilmiştir.
constexpr std::uint64_t kProbeMemory = std::uint64_t{128} << 20U;
constexpr auto kProbeTimeout = std::chrono::seconds(30);

struct ProbeRun {
    int exit_code = -1;  // -1: süre doldu
    std::string output;
};

ProbeRun run_probe(std::string_view name, std::uint64_t memory = 0, std::string_view input = {}) {
    auto probe = core::ChildProcess::start_confined({kProbe, std::string(name)}, memory);
    REQUIRE(probe);
    const auto deadline = core::ChildProcess::Clock::now() + kProbeTimeout;
    if (!input.empty())
        REQUIRE(probe->write_all(input, deadline));
    probe->close_stdin();
    ProbeRun run;
    while (probe->read_some(run.output, deadline) == core::ChildProcess::ReadStatus::Data) {
    }
    run.exit_code = probe->wait_until(deadline).value_or(-1);
    return run;
}

}  // namespace

#endif

#ifdef __linux__
#include <csignal>

namespace {

constexpr int kKilledBySigsys = 128 + SIGSYS;  // ChildProcess::wait_until: 128 + sinyal
constexpr int kSandboxRefused = 6;             // işçinin kSandbox'ı

}  // namespace

TEST_CASE("every call outside the allowlist kills the sandboxed process with SIGSYS") {
    for (const std::string_view name :
         {"open_read", "open_write", "stat_path", "socket_inet", "socket_unix", "fork", "vfork", "execve",
          "ptrace_ppid", "process_vm_readv_ppid", "kill_ppid", "mmap_exec", "mprotect_exec", "mmap_shared",
          "mmap_growsdown", "prlimit_raise", "i386_entry", "early_thread"}) {
        CAPTURE(name);
        CHECK(run_probe(name, kProbeMemory).exit_code == kKilledBySigsys);
    }
}

TEST_CASE("the worker's own work still runs inside the sandbox") {
    const ProbeRun echoed = run_probe("pipe_io", kProbeMemory, "ping over the pipe");
    CHECK(echoed.exit_code == 0);
    CHECK(echoed.output == "ping over the pipe");
    CHECK(run_probe("thread", kProbeMemory).exit_code == 0);
    CHECK(run_probe("alloc_64MiB", kProbeMemory).exit_code == 0);
}

TEST_CASE("clone3 fails with ENOSYS so that thread creation falls back to the filtered clone") {
    CHECK(run_probe("clone3").exit_code == 0);
}

TEST_CASE("a sandbox that cannot be installed stops the process before any work") {
    CHECK(run_probe("refused").exit_code == kSandboxRefused);
}

// ASan gölge belleği RLIMIT_DATA'ya sayılır: sanitizer derlemesinde sınır her ayırmayı düşürür.
#if !defined(__SANITIZE_ADDRESS__)
TEST_CASE("the memory limit set by the parent stops an allocation over it") {
    CHECK(run_probe("alloc_over", kProbeMemory).exit_code == 0);
    CHECK(run_probe("alloc_64MiB", kProbeMemory).exit_code == 0);  // aynı sınırda sığan ayırma geçer
}
#endif

#elif defined(_WIN32)
#include "core/process.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr const wchar_t* kCanary = L"NOTEAPP_SBX_CANARY";

// Probun ortamındaki adlar, sıralı.
std::vector<std::string> probe_environment_names() {
    const ProbeRun run = run_probe("env_dump", kProbeMemory);
    REQUIRE(run.exit_code == 0);
    std::vector<std::string> names;
    std::istringstream lines(run.output);
    for (std::string line; std::getline(lines, line);)
        names.push_back(line.substr(0, line.find('=', 1)));  // "=C:=..." gibi gizli girdiler de sayılır
    std::ranges::sort(names);
    return names;
}

}  // namespace

// Wine her sürece kendi değişkenlerini (WINE*, SystemDrive, PROCESSOR_ARCHITECTURE) ekler: tam küme yalnız gerçek
// Windows'ta sınanır (aşağıda manual-win11). Burada ebeveynden gelen hiçbir şeyin geçmediği sınanır.
TEST_CASE("the sandboxed process inherits none of the parent's environment") {
    REQUIRE(SetEnvironmentVariableW(kCanary, L"leaked"));
    const std::vector<std::string> names = probe_environment_names();
    SetEnvironmentVariableW(kCanary, nullptr);
    for (const std::string inherited : {"NOTEAPP_SBX_CANARY", "PATH", "Path", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
                                  "TEMP", "TMP", "USERNAME", "NOTEAPP_HOME"}) {
        CAPTURE(inherited);
        CHECK(std::ranges::find(names, inherited) == names.end());
    }
    CHECK(std::ranges::find(names, "SystemRoot") != names.end());
    CHECK(std::ranges::find(names, "NoDefaultCurrentDirectoryInExePath") != names.end());
}

TEST_CASE("the worker's own work still runs inside the sandbox") {
    const ProbeRun echoed = run_probe("pipe_io", kProbeMemory, "ping over the pipe");
    CHECK(echoed.exit_code == 0);
    CHECK(echoed.output == "ping over the pipe");
    CHECK(run_probe("alloc_64MiB", kProbeMemory).exit_code == 0);
}

// Wine'ın belirteci yöneticidir (grup etkin): orada da ayırt eder. Windows'ta yalnız yönetici olarak çalıştırılınca
// ayırt eder; prob grubun durumunu yazar ("absent": yönetici olmayan kullanıcı).
TEST_CASE("an elevated parent's Administrators group is deny-only in the sandboxed process") {
    const ProbeRun run = run_probe("admin_sid", kProbeMemory);
    MESSAGE(run.output);
    CHECK(run.exit_code == 0);
}

// Wine bu denetimleri uygulamaz (iş nesnesinin süreç ve bellek sınırı, bütünlük düzeyi, PROHIBIT_DYNAMIC_CODE): varsayılan
// koşuda atlanır; ctest `pdfview_win11` (etiket manual-win11) gerçek Windows'ta --no-skip ile koşar (SPEC-core.md "PDF
// yardımcı süreci").
TEST_SUITE("manual-win11" * doctest::skip()) {
    TEST_CASE("the sandboxed process gets only SystemRoot and the current-directory guard in its environment") {
        CHECK(probe_environment_names() ==
              std::vector<std::string>{"NoDefaultCurrentDirectoryInExePath", "SystemRoot"});
    }

    TEST_CASE("the sandboxed process cannot start another process") {
        const ProbeRun run = run_probe("spawn_child", kProbeMemory);
        INFO(run.output);
        CHECK(run.exit_code == 0);
    }

    TEST_CASE("the memory limit set by the parent stops an allocation over it") {
        CHECK(run_probe("alloc_over", kProbeMemory).exit_code == 0);
        CHECK(run_probe("alloc_64MiB", kProbeMemory).exit_code == 0);  // aynı sınırda sığan ayırma geçer
    }

    TEST_CASE("the sandboxed process runs at low integrity") {
        CHECK(run_probe("integrity", kProbeMemory).exit_code == 0);
    }

    TEST_CASE("the sandboxed process cannot write into the user's local app data") {
        const std::optional<std::string> local_app_data = core::get_env_utf8("LOCALAPPDATA");
        REQUIRE(local_app_data);
        CHECK(run_probe("write_localappdata", kProbeMemory, *local_app_data).exit_code == 0);
    }

    TEST_CASE("the sandboxed process cannot allocate executable memory") {
        CHECK(run_probe("dynamic_code", kProbeMemory).exit_code == 0);
    }
}

#endif
