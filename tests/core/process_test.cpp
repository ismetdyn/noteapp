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
#else
#include <unistd.h>
#endif

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
