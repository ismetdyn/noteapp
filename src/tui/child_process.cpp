#include "tui/child_process.hpp"

#include "core/process.hpp"
#include "core/unique_handle.hpp"

#include <array>
#include <utility>

#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
#endif

namespace noteapp::tui {

#ifdef _WIN32

std::optional<ChildProcess> ChildProcess::start(const std::vector<std::string>&, bool,
                                                const std::vector<std::string>&) {
    return std::nullopt;
}

bool ChildProcess::write_all(std::string_view, Clock::time_point, const std::stop_token&) {
    return false;
}

void ChildProcess::close_stdin() {}

ChildProcess::ReadStatus ChildProcess::read_some(std::string&, Clock::time_point) {
    return ReadStatus::Failed;
}

std::optional<int> ChildProcess::wait_until(Clock::time_point) {
    return std::nullopt;
}

void ChildProcess::kill() {}

void ChildProcess::close_fds() {}

#else

namespace {

// AppRun bunları paketin kendi CA'sı için ayarlar; çocuğun bağlı olduğu host kitaplıklarını şaşırtmasın.
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
constexpr auto kReapPoll = std::chrono::milliseconds(2);
constexpr std::size_t kReadChunk = 64 * 1024;

std::vector<std::string> child_environment(const std::vector<std::string>& extra) {
    std::vector<std::string> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text(*entry);
        const bool dropped =
            std::ranges::any_of(kDroppedEnv, [&](const char* prefix) { return text.starts_with(prefix); });
        if (!dropped)
            entries.emplace_back(text);
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

// Yazma beklerken durma isteği bu aralıkla yoklanır: takılan çocuk çıkışı geciktirmesin.
constexpr int kWriteStopPollMs = 50;

int remaining_ms(ChildProcess::Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - ChildProcess::Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(left, 0, 60'000));
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
                                                const std::vector<std::string>& extra_env) {
    if (argv.empty())
        return std::nullopt;
    const auto program = core::find_program(argv.front());
    if (!program)
        return std::nullopt;
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
    const std::vector<std::string> env = child_environment(extra_env);
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

#endif

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)), stdin_fd_(std::exchange(other.stdin_fd_, -1)),
      stdout_fd_(std::exchange(other.stdout_fd_, -1)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        kill();
        close_fds();
        pid_ = std::exchange(other.pid_, -1);
        stdin_fd_ = std::exchange(other.stdin_fd_, -1);
        stdout_fd_ = std::exchange(other.stdout_fd_, -1);
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    kill();
    close_fds();
}

}  // namespace noteapp::tui
