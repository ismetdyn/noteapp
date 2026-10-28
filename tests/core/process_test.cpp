#include "core/process.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <functional>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif
#include <thread>

using namespace noteapp::core;
using namespace noteapp::test;
using namespace std::chrono_literals;

namespace {

const std::string kFakeEditor = NOTEAPP_FAKE_EDITOR;

// Bu sürecin stdout/stderr'ini `action` süresince dosyaya çevirip yakalananı döner: devralan çocuk
// oraya yazar, devralmayan yazamaz.
std::string captured_output(const std::function<void()>& action) {
    const TempDir dir;
    const fs::path file = dir.path() / "captured.txt";
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    const int fd =
        _wopen(file.wstring().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    const int saved_out = _dup(1), saved_err = _dup(2);
    _dup2(fd, 1);
    _dup2(fd, 2);
    action();
    _dup2(saved_out, 1);
    _dup2(saved_err, 2);
    _close(saved_out);
    _close(saved_err);
    _close(fd);
#else
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int saved_out = dup(1), saved_err = dup(2);
    dup2(fd, 1);
    dup2(fd, 2);
    action();
    dup2(saved_out, 1);
    dup2(saved_err, 2);
    close(saved_out);
    close(saved_err);
    close(fd);
#endif
    return slurp(file);
}

#ifdef __linux__
// Ölmüş ama henüz biçilmemiş (zombi) süreç: ebeveyni ölen torun PID 1'e devredilir; Docker'da PID 1 bir kabuk
// olduğundan onu hemen biçmeyebilir. Zombi çalışmaz, kill(pid, 0) yine de başarılı döner.
bool is_zombie(long long pid) {
    std::FILE* stat = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
    if (stat == nullptr)
        return false;
    char buffer[512] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, stat);
    std::fclose(stat);
    // Biçim: "pid (komut) DURUM ..."; komut adı parantez içerebileceğinden son ')' sonrası okunur.
    const std::string line(buffer, read);
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
}
#endif

// PID hâlâ çalışan bir süreç mi? (Windows: çıkış kodu STILL_ACTIVE; POSIX: kill(pid, 0), zombi ölü sayılır.)
bool process_alive(long long pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    const bool exists = kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#ifdef __linux__
    return exists && !is_zombie(pid);
#else
    return exists;
#endif
#endif
}

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

}  // namespace

TEST_CASE("spawn_detached does not wait; wait_with_timeout returns the exit code once it ends") {
    auto child = spawn_detached({kFakeEditor}, {{"FAKE_EDITOR_EXIT", "7"}});
    REQUIRE(child.has_value());
    CHECK(child->pid() > 0);
    const auto code = wait_with_timeout(*child, 10s);
    REQUIRE(code.has_value());
    CHECK(*code == std::optional<int>(7));
}

TEST_CASE("wait_with_timeout reports a still running child; terminate ends it at once") {
    auto child = spawn_detached({kFakeEditor}, {{"FAKE_EDITOR_SLEEP_MS", "600000"}});
    REQUIRE(child.has_value());
    const auto start = std::chrono::steady_clock::now();
    const auto running = wait_with_timeout(*child, 200ms);
    REQUIRE(running.has_value());
    CHECK_FALSE(running->has_value());
    CHECK(std::chrono::steady_clock::now() - start >= 150ms);  // gerçekten bekledi

    terminate(*child);
    const auto ended = wait_with_timeout(*child, 0ms);  // terminate sonlanmayı da bekler
    REQUIRE(ended.has_value());
    CHECK(ended->has_value());
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("terminate kills the whole process tree, grandchild included") {
    const TempDir dir;
    const fs::path pid_file = dir.path() / "grandchild.pid";
    auto child = spawn_detached({kFakeEditor}, {{"FAKE_EDITOR_SLEEP_MS", "600000"},
                                                {"FAKE_EDITOR_GRANDCHILD_PID_OUT", utf8(pid_file)}});
    REQUIRE(child.has_value());
    long long grandchild = 0;
    REQUIRE(wait_until(
        [&] {
            if (!fs::exists(pid_file))
                return false;
            grandchild = std::atoll(slurp(pid_file).c_str());
            return grandchild > 0;
        },
        10s));
    REQUIRE(process_alive(grandchild));

    terminate(*child);
    CHECK(wait_until([&] { return !process_alive(child->pid()); }, 2s));
    const bool grandchild_gone = wait_until([&] { return !process_alive(grandchild); }, 2s);
    CHECK(grandchild_gone);
    // Başarısızlıkta torun 10 dakika uyuyup kalmasın.
    if (!grandchild_gone) {
#ifdef _WIN32
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(grandchild))) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
#else
        kill(static_cast<pid_t>(grandchild), SIGKILL);
#endif
    }
}

TEST_CASE("spawn_detached reports a missing program as NotFound") {
    const auto child = spawn_detached({"noteapp-no-such-program-xyz"});
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().code == Error::Code::NotFound);
}

TEST_CASE("a detached child and a quiet run_and_wait write nothing to our stdout/stderr") {
    // Kontrol: devralan çocuğun çıktısı yakalanıyor (test gerçekten bir şey ölçüyor).
    CHECK(captured_output([] {
              (void)run_and_wait({kFakeEditor}, {{"FAKE_EDITOR_STDOUT", "görünür"}});
          }).find("görünür") != std::string::npos);
    CHECK(captured_output([] {
              (void)run_and_wait({kFakeEditor}, {{"FAKE_EDITOR_STDOUT", "gizli"}}, true);
          }).empty());
    CHECK(captured_output([] {
              auto child = spawn_detached({kFakeEditor}, {{"FAKE_EDITOR_STDOUT", "ayrık"}});
              if (child)
                  (void)wait_with_timeout(*child, 10s);
          }).empty());
}

TEST_CASE("current_executable is the running binary") {
    const fs::path self = current_executable();
    REQUIRE_FALSE(self.empty());
    CHECK(self.is_absolute());
    CHECK(self.stem() == "core_tests");
}

TEST_CASE("run_and_wait propagates the exit code") {
    CHECK(run_and_wait({kFakeEditor}, {}).value() == 0);
    CHECK(run_and_wait({kFakeEditor}, {{"FAKE_EDITOR_EXIT", "7"}}).value() == 7);
}

TEST_CASE("run_and_wait makes env overrides visible to the child") {
    TempDir dir;
    const auto file = dir.path() / "out.txt";
    const auto result = run_and_wait({kFakeEditor, utf8(file)}, {{"FAKE_EDITOR_CONTENT", "içerik ğüşıöç"}});
    REQUIRE(result.has_value());
    CHECK(slurp(file) == "içerik ğüşıöç");
}

TEST_CASE("run_and_wait delivers awkward arguments intact") {
    TempDir dir;
    const auto out = dir.path() / "args.txt";
    const std::vector<std::string> args = {"a b",      "q\"uote", "trailing\\", "dir with space\\",
                                           "ğüşıöç İ", "",        "x\\\"y"};
    std::vector<std::string> argv = {kFakeEditor};
    argv.insert(argv.end(), args.begin(), args.end());
    const auto result = run_and_wait(argv, {{"FAKE_EDITOR_ARGS_OUT", utf8(out)}});
    REQUIRE(result.has_value());
    std::string expected;
    for (const auto& a : args)
        expected += a + "\n";
    CHECK(slurp(out) == expected);
}

TEST_CASE("run_and_wait reports a missing program as NotFound with its name") {
    const auto result = run_and_wait({"noteapp-no-such-program-xyz"});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(result.error().args.at(0) ==
          std::pair<std::string, std::string>{"program", "noteapp-no-such-program-xyz"});
}

TEST_CASE("find_program locates a program on PATH and ignores the working directory") {
    TempDir dir;
    ScopedEnv path("PATH", utf8(dir.path()));
    CHECK_FALSE(find_program("fake_editor").has_value());
    CHECK(find_program(kFakeEditor).has_value());  // dizin içeren ad doğrudan denenir
}

#ifdef _WIN32
TEST_CASE("run_and_wait runs .cmd scripts through cmd.exe and rejects metacharacters") {
    TempDir dir;
    const auto script = dir.path() / "tool with space.cmd";
    std::ofstream(script) << "@exit /b 3\r\n";

    CHECK(run_and_wait({utf8(script), "plain arg"}).value() == 3);
    for (const char* bad : {"a\"b", "50%", "x!y", "a^b", "a&b", "a|b", "a<b", "a>b"}) {
        const auto result = run_and_wait({utf8(script), bad});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Error::Code::InvalidPath);
    }
}
#endif
