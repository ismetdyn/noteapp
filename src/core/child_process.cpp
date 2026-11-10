#include "core/child_process.hpp"

#include "core/process.hpp"
#include "core/unique_handle.hpp"

#include <algorithm>
#include <array>
#include <utility>

#ifdef _WIN32
#include "core/process_win.hpp"

#include <atomic>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
#endif

namespace noteapp::core {

namespace {

// AppRun bunları paketin kendi CA'sı için ayarlar; çocuğun bağlı olduğu host kitaplıklarını şaşırtmasın.
// Windows'ta da düşer: ortam iki platformda aynı süzülür.
constexpr std::array kDroppedEnv = {"SSL_CERT_FILE=",
                                    "SSL_CERT_DIR=",
                                    "LD_LIBRARY_PATH=",
                                    "LD_PRELOAD=",
                                    "APPDIR=",
                                    "APPIMAGE=",
                                    "ARGV0=",
                                    "OWD=",
                                    "NOTEAPP_GIT_TOKEN=",
                                    "NOTEAPP_GIT_TOKEN_HOST="};
constexpr std::size_t kReadChunk = 64 * 1024;

// Yazma beklerken durma isteği bu aralıkla yoklanır: takılan çocuk çıkışı geciktirmesin.
constexpr int kWriteStopPollMs = 50;

int remaining_ms(ChildProcess::Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - ChildProcess::Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(left, 0, 60'000));
}

}  // namespace

#ifdef _WIN32

namespace {

core::EnvOverrides child_env_overrides(const std::vector<std::string>& extra) {
    core::EnvOverrides overrides = core::host_environment_restores();
    for (const std::string_view prefix : kDroppedEnv)
        overrides.emplace_back(std::string(prefix.substr(0, prefix.size() - 1)), std::nullopt);
    for (const std::string& assignment : extra) {
        const std::size_t equals = assignment.find('=');
        overrides.emplace_back(assignment.substr(0, equals), assignment.substr(equals + 1));
    }
    return overrides;
}

struct PipeEnds {
    core::UniqueHandle parent;  // bindirmeli (overlapped)
    core::UniqueHandle child;   // devredilebilir, eşzamanlı: çocuk sıradan stdin/stdout görür
};

// Anonim boru (CreatePipe) bindirmeli G/Ç desteklemez; süreli okuma/yazma ve iptal için ebeveyn ucu bindirmeli adlı
// borudur. Tek örnek + FILE_FLAG_FIRST_PIPE_INSTANCE: adı önceden alan ya da araya giren başka süreç ucu bize
// veremez, başlatma başarısız olur. İstemci CreateFileW ile bağlandığından ConnectNamedPipe gerekmez.
// ponytail: çocuk ucu CreateProcessW'e dek devredilebilir; o arada başka iş parçacığının liste kullanmayan
// CreateProcessW'i (core::run_and_wait) ucu da devralır ve EOF o süreç bitene dek gecikir. Sorun olursa
// başlatmalar tek kilitle sıralanır.
std::optional<PipeEnds> open_pipe(bool child_reads) {
    static std::atomic<unsigned long> serial{0};
    const std::wstring name = L"\\\\.\\pipe\\noteapp-child-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(++serial);
    const DWORD direction = child_reads ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    auto parent = core::own_win_handle<core::UniqueHandle>(
        CreateNamedPipeW(name.c_str(), direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                         kReadChunk, kReadChunk, 0, nullptr));
    if (!parent)
        return std::nullopt;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto child = core::own_win_handle<core::UniqueHandle>(CreateFileW(
        name.c_str(), child_reads ? GENERIC_READ : GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, nullptr));
    if (!child)
        return std::nullopt;
    return PipeEnds{std::move(parent), std::move(child)};
}

struct IoResult {
    DWORD bytes = 0;
    DWORD error = ERROR_SUCCESS;  // süre dolumu ya da durma: ERROR_OPERATION_ABORTED
};

// `issue` tek bindirmeli okuma/yazmayı başlatır. Tamamlanana, `deadline`a ya da durma isteğine dek beklenir; sonra
// iptal edilir ve iptalin bitmesi beklenir: tampon ancak o zaman serbesttir. İptalle yarışan tamamlanma kaybolmaz,
// aktarılan bayt yine döner. POSIX dalındaki poll + durma yoklamasının karşılığı.
template <class Issue>
IoResult overlapped_io(HANDLE pipe, Issue issue, ChildProcess::Clock::time_point deadline,
                       const std::stop_token& stop) {
    const core::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return {0, GetLastError()};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (!issue(&overlapped)) {
        if (const DWORD error = GetLastError(); error != ERROR_IO_PENDING)
            return {0, error};
    }
    while (true) {
        const DWORD waited =
            WaitForSingleObject(event.get(), static_cast<DWORD>(std::min(remaining_ms(deadline), kWriteStopPollMs)));
        if (waited == WAIT_OBJECT_0)
            break;
        if (waited != WAIT_TIMEOUT || stop.stop_requested() || ChildProcess::Clock::now() >= deadline) {
            CancelIoEx(pipe, &overlapped);
            break;
        }
    }
    IoResult result;
    if (!GetOverlappedResult(pipe, &overlapped, &result.bytes, TRUE))
        result.error = GetLastError();
    return result;
}

void close_if_open(void*& handle) {
    if (handle != nullptr)
        CloseHandle(handle);
    handle = nullptr;
}

}  // namespace

std::optional<ChildProcess> ChildProcess::start(const std::vector<std::string>& argv, bool capture_stdout,
                                                const std::vector<std::string>& extra_env,
                                                const std::filesystem::path& working_directory) {
    return start_windows(argv, capture_stdout, extra_env, working_directory, nullptr);
}

std::optional<ChildProcess> ChildProcess::start_confined(const std::vector<std::string>& argv,
                                                         std::uint64_t memory_bytes) {
    // Ortam, konsol ve sınırlar core::spawn_confined'da (process_win.hpp).
    const core::SpawnConfinement confinement{memory_bytes};
    return start_windows(argv, true, {}, {}, &confinement);
}

std::optional<ChildProcess> ChildProcess::start_windows(const std::vector<std::string>& argv, bool capture_stdout,
                                                        const std::vector<std::string>& extra_env,
                                                        const std::filesystem::path& working_directory,
                                                        const SpawnConfinement* confinement) {
    if (argv.empty())
        return std::nullopt;
    std::optional<PipeEnds> input = open_pipe(true);
    if (!input)
        return std::nullopt;
    std::optional<PipeEnds> output;
    if (capture_stdout) {
        output = open_pipe(false);
        if (!output)
            return std::nullopt;
    }
    // stderr boş aygıta: okunmayan bir boru dolup çocuğu kilitlemesin.
    const core::UniqueHandle null_device = core::open_null_device();
    if (!null_device)
        return std::nullopt;
    HANDLE child_stdout = output ? output->child.get() : null_device.get();
    auto spawned = confinement != nullptr
                       ? core::spawn_confined(argv, input->child.get(), child_stdout, null_device.get(), *confinement)
                       : core::spawn_with_stdio(argv, child_env_overrides(extra_env), input->child.get(),
                                                child_stdout, null_device.get(), CREATE_NO_WINDOW, working_directory);
    if (!spawned)
        return std::nullopt;
    // Çocuğun uçları burada kapanır: yalnız çocukta kalmalı, yoksa çocuk çıkınca okuyan EOF görmez.
    ChildProcess child;
    child.process_ = spawned->process.release();
    child.job_ = spawned->job.release();
    child.stdin_pipe_ = input->parent.release();
    child.stdout_pipe_ = output ? output->parent.release() : nullptr;
    return child;
}

bool ChildProcess::write_all(std::string_view data, Clock::time_point deadline, const std::stop_token& stop) {
    if (stdin_pipe_ == nullptr)
        return false;
    while (!data.empty()) {
        if (stop.stop_requested())
            return false;
        const auto size = static_cast<DWORD>(std::min(data.size(), kReadChunk));
        const IoResult io = overlapped_io(
            stdin_pipe_,
            [&](OVERLAPPED* overlapped) { return WriteFile(stdin_pipe_, data.data(), size, nullptr, overlapped); },
            deadline, stop);
        if (io.error != ERROR_SUCCESS || io.bytes == 0)
            return false;  // süre, durma ya da ölü çocuk (ERROR_NO_DATA)
        data.remove_prefix(io.bytes);
    }
    return true;
}

void ChildProcess::close_stdin() {
    close_if_open(stdin_pipe_);
}

ChildProcess::ReadStatus ChildProcess::read_some(std::string& out, Clock::time_point deadline) {
    if (stdout_pipe_ == nullptr)
        return ReadStatus::Failed;
    std::array<char, kReadChunk> chunk{};
    const IoResult io = overlapped_io(
        stdout_pipe_,
        [&](OVERLAPPED* overlapped) {
            return ReadFile(stdout_pipe_, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, overlapped);
        },
        deadline, {});
    out.append(chunk.data(), io.bytes);
    if (io.bytes > 0 || io.error == ERROR_SUCCESS)
        return ReadStatus::Data;  // sıfır baytlık okuma boş yazmadır, EOF değil
    if (io.error == ERROR_OPERATION_ABORTED)
        return ReadStatus::Timeout;
    return io.error == ERROR_BROKEN_PIPE ? ReadStatus::Eof : ReadStatus::Failed;
}

std::optional<int> ChildProcess::wait_until(Clock::time_point deadline) {
    while (process_ != nullptr) {
        const DWORD waited = WaitForSingleObject(process_, static_cast<DWORD>(remaining_ms(deadline)));
        if (waited == WAIT_TIMEOUT) {
            if (Clock::now() >= deadline)
                return std::nullopt;
            continue;
        }
        DWORD code = 0;
        const bool exited = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process_, &code);
        close_if_open(process_);
        return exited ? std::optional<int>(static_cast<int>(code)) : std::nullopt;
    }
    return std::nullopt;
}

void ChildProcess::kill() {
    // İş nesnesi torunları da öldürür; çocuk atanamadıysa (iç içe iş desteklenmiyorsa) yalnız doğrudan çocuk.
    if ((job_ == nullptr || !TerminateJobObject(job_, 1)) && process_ != nullptr)
        TerminateProcess(process_, 1);
    if (process_ != nullptr)
        WaitForSingleObject(process_, INFINITE);
    close_if_open(process_);
    close_if_open(job_);
}

void ChildProcess::close_fds() {
    close_if_open(stdin_pipe_);
    close_if_open(stdout_pipe_);
}

void ChildProcess::take(ChildProcess& other) noexcept {
    process_ = std::exchange(other.process_, nullptr);
    job_ = std::exchange(other.job_, nullptr);
    stdin_pipe_ = std::exchange(other.stdin_pipe_, nullptr);
    stdout_pipe_ = std::exchange(other.stdout_pipe_, nullptr);
}

#else

namespace {

constexpr auto kReapPoll = std::chrono::milliseconds(2);

std::vector<std::string> child_environment(const std::vector<std::string>& extra) {
    std::vector<std::string> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text(*entry);
        const bool dropped =
            std::ranges::any_of(kDroppedEnv, [&](const char* prefix) { return text.starts_with(prefix); });
        if (!dropped)
            entries.emplace_back(text);
    }
    for (const auto& [key, value] : core::host_environment_restores()) {
        const std::string prefix = key + "=";
        std::erase_if(entries, [&](const std::string& entry) { return entry.starts_with(prefix); });
        if (value)
            entries.push_back(prefix + *value);
    }
    for (const std::string& assignment : extra) {
        const std::string prefix = assignment.substr(0, assignment.find('=') + 1);
        std::erase_if(entries, [&](const std::string& entry) { return entry.starts_with(prefix); });
        entries.push_back(assignment);
    }
    return entries;
}

std::vector<char*> c_strings(const std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (const std::string& text : strings)
        out.push_back(const_cast<char*>(text.c_str()));
    out.push_back(nullptr);
    return out;
}

struct Pipe {
    core::UniqueFd read_end;
    core::UniqueFd write_end;
};

// CLOEXEC şart: başka bir iş parçacığının başlattığı çocuk bu borunun yazma ucunu devralırsa araç EOF görmez
// ve zaman aşımına düşer.
std::optional<Pipe> open_pipe() {
    std::array<int, 2> ends{-1, -1};
#ifdef __linux__
    if (pipe2(ends.data(), O_CLOEXEC) != 0)
        return std::nullopt;
#else
    if (pipe(ends.data()) != 0)
        return std::nullopt;
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
#endif
    return Pipe{core::UniqueFd(ends[0]), core::UniqueFd(ends[1])};
}

void close_if_open(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

// Yazma sırasında ölü çocuk SIGPIPE üretir, varsayılanı süreci öldürür. İşaret yalnız bu iş parçacığında ve
// bu kapsamda engellenir; süreç geneli SIG_IGN çocuklara da miras kalırdı.
class SigpipeGuard {
public:
    SigpipeGuard() {
        sigemptyset(&pipe_only_);
        sigaddset(&pipe_only_, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        was_pending_ = sigismember(&pending, SIGPIPE) == 1;
        pthread_sigmask(SIG_BLOCK, &pipe_only_, &previous_);
    }
    ~SigpipeGuard() {
        sigset_t pending;
        sigpending(&pending);
        if (!was_pending_ && sigismember(&pending, SIGPIPE) == 1) {
            int taken = 0;
            sigwait(&pipe_only_, &taken);
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
    SigpipeGuard(const SigpipeGuard&) = delete;
    SigpipeGuard& operator=(const SigpipeGuard&) = delete;

private:
    sigset_t pipe_only_{};
    sigset_t previous_{};
    bool was_pending_ = false;
};

}  // namespace

std::optional<ChildProcess> ChildProcess::start(const std::vector<std::string>& argv, bool capture_stdout,
                                                const std::vector<std::string>& extra_env,
                                                const std::filesystem::path& working_directory) {
    return start_with_env(argv, capture_stdout, child_environment(extra_env), working_directory);
}

std::optional<ChildProcess> ChildProcess::start_confined(const std::vector<std::string>& argv,
                                                         std::uint64_t memory_bytes) {
    // Ortam ebeveynde boş kurulur: token, AppImage değişkenleri ve host_environment_restores'un geri verdikleri de
    // dahil hiçbir şey geçmez.
    std::optional<ChildProcess> child = start_with_env(argv, true, {}, {});
#if defined(__SANITIZE_ADDRESS__)
    // GEÇİCİ: ASan'ın gölge belleği (TiB'larca yazılabilir eşleme) RLIMIT_DATA'ya sayılır; sınırla çocuk ilk ayırmada
    // ölürdü.
    // BEDEL: sanitizer derlemesinde PDF işçisinin OS bellek sınırı yok (yalnız context_memory bütçesi).
    // NE ZAMAN KALDIRILIR: sanitizer derlemesi RLIMIT_DATA'yı saymayan bir sınıra (cgroup) geçince; sahip PDF-SBX.
    memory_bytes = 0;
#endif
    if (!child)
        return child;
#ifdef __linux__
    // İlk bayt bu çağrılardan sonra yazılır: çocuk güvenilmez girdiyi sınırlar konmadan okuyamaz. Sert sınır da aynı
    // değerdir; çocuk kendi sınırını yükseltemez. Çekirdek dökümü hep kapalı: filtreye takılıp SIGSYS'le ölen çocuk
    // saldırganın doldurduğu belleği (PDF baytları dahil) ebeveynin çalışma dizinine dökmesin.
    const auto pid = static_cast<pid_t>(child->pid_);
    const rlimit no_core{0, 0};
    const auto bytes = static_cast<rlim_t>(memory_bytes);
    const rlimit data{bytes, bytes};
    if (prlimit(pid, RLIMIT_CORE, &no_core, nullptr) == 0 &&
        (memory_bytes == 0 || prlimit(pid, RLIMIT_DATA, &data, nullptr) == 0))
        return child;
#endif
    // Sınır konamadı (başka POSIX'te başka sürecinki hiç konamaz): sınırsız çalıştırmak yerine başlatma reddedilir.
    child->kill();
    return std::nullopt;
}

std::optional<ChildProcess> ChildProcess::start_with_env(const std::vector<std::string>& argv, bool capture_stdout,
                                                         const std::vector<std::string>& env,
                                                         const std::filesystem::path& working_directory) {
    if (argv.empty())
        return std::nullopt;
    auto program = core::find_program(argv.front());
    if (!program)
        return std::nullopt;
    // Çalışma dizini değişince göreli program yolu başka yeri gösterirdi.
    std::error_code absolute_error;
    if (!working_directory.empty())
        program = std::filesystem::absolute(*program, absolute_error);
    std::optional<Pipe> input = open_pipe();
    if (!input)
        return std::nullopt;
    std::optional<Pipe> output;
    if (capture_stdout) {
        output = open_pipe();
        if (!output)
            return std::nullopt;
    }
    // Ayrılan bellek init/destroy çiftinin dışında: bad_alloc eylem ve öznitelik nesnesini sızdırmasın.
    std::vector<char*> args = c_strings(argv);
    std::vector<char*> envp = c_strings(env);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input->read_end.get(), STDIN_FILENO);
    if (output)
        posix_spawn_file_actions_adddup2(&actions, output->write_end.get(), STDOUT_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!working_directory.empty())
        posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());  // glibc 2.29, macOS 10.15
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // torunlar birlikte öldürülebilsin
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, program->c_str(), &actions, &attributes, args.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return std::nullopt;
    ChildProcess child;
    child.pid_ = pid;
    child.stdin_fd_ = input->write_end.release();
    child.stdout_fd_ = output ? output->read_end.release() : -1;
    return child;
}

bool ChildProcess::write_all(std::string_view data, Clock::time_point deadline, const std::stop_token& stop) {
    if (stdin_fd_ < 0)
        return false;
    const int flags = fcntl(stdin_fd_, F_GETFL);
    fcntl(stdin_fd_, F_SETFL, flags | O_NONBLOCK);
    const SigpipeGuard guard;
    while (!data.empty()) {
        if (stop.stop_requested())
            return false;
        pollfd target{stdin_fd_, POLLOUT, 0};
        const int ready = poll(&target, 1, std::min(remaining_ms(deadline), kWriteStopPollMs));
        if (ready < 0 && errno != EINTR)
            return false;
        if (ready == 0 && Clock::now() >= deadline)
            return false;  // zaman aşımı: çocuk okumuyor
        if (ready <= 0)
            continue;
        const ssize_t written = write(stdin_fd_, data.data(), data.size());
        if (written < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

void ChildProcess::close_stdin() {
    close_if_open(stdin_fd_);
}

ChildProcess::ReadStatus ChildProcess::read_some(std::string& out, Clock::time_point deadline) {
    if (stdout_fd_ < 0)
        return ReadStatus::Failed;
    pollfd source{stdout_fd_, POLLIN, 0};
    const int ready = poll(&source, 1, remaining_ms(deadline));
    if (ready < 0)
        return errno == EINTR ? ReadStatus::Timeout : ReadStatus::Failed;
    if (ready == 0)
        return ReadStatus::Timeout;
    std::array<char, kReadChunk> chunk{};
    const ssize_t count = read(stdout_fd_, chunk.data(), chunk.size());
    if (count < 0)
        return errno == EINTR || errno == EAGAIN ? ReadStatus::Timeout : ReadStatus::Failed;
    if (count == 0)
        return ReadStatus::Eof;
    out.append(chunk.data(), static_cast<std::size_t>(count));
    return ReadStatus::Data;
}

std::optional<int> ChildProcess::wait_until(Clock::time_point deadline) {
    if (pid_ < 0)
        return std::nullopt;
    while (true) {
        int status = 0;
        const pid_t done = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (done == static_cast<pid_t>(pid_)) {
            pid_ = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        if (done < 0 && errno != EINTR) {
            pid_ = -1;
            return std::nullopt;
        }
        if (Clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(kReapPoll);
    }
}

void ChildProcess::kill() {
    if (pid_ < 0)
        return;
    // Spawn grubu kurdu: -pid torunları da hedefler; arka plana çatallanmış pano aracı ayrı gruptaysa
    // etkilenmez.
    ::kill(-static_cast<pid_t>(pid_), SIGKILL);
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    while (waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
}

void ChildProcess::close_fds() {
    close_if_open(stdin_fd_);
    close_if_open(stdout_fd_);
}

void ChildProcess::take(ChildProcess& other) noexcept {
    pid_ = std::exchange(other.pid_, -1);
    stdin_fd_ = std::exchange(other.stdin_fd_, -1);
    stdout_fd_ = std::exchange(other.stdout_fd_, -1);
}

#endif

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
    take(other);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        kill();
        close_fds();
        take(other);
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    kill();
    close_fds();
}

}  // namespace noteapp::core
