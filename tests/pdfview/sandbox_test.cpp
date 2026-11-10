// PDF-SBX: PDF işçisinin kum havuzu (pdfview::enter_sandbox + core::ChildProcess::start_confined). Prob
// (tests/support/pdf_sandbox_probe.cpp) işçiyle aynı yoldan kum havuzuna girer ve tek bir çağrı dener. Olumsuz problar
// izin listesinin dışını, olumlu problar işçinin gerçekten yaptığı işi sınar: filtre her şeyi öldürseydi olumsuzlar
// yine geçerdi, olumlular düşer (yanlış yeşil koruması).
#include "core/child_process.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>

#ifdef __linux__
#include <csignal>

using namespace noteapp;

namespace {

const std::string kProbe = NOTEAPP_PDF_SANDBOX_PROBE;
constexpr int kKilledBySigsys = 128 + SIGSYS;  // ChildProcess::wait_until: 128 + sinyal
constexpr int kSandboxRefused = 6;             // işçinin kSandbox'ı
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

#endif
