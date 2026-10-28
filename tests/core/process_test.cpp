#include "core/process.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>

using namespace noteapp::core;
using namespace noteapp::test;

namespace {

const std::string kFakeEditor = NOTEAPP_FAKE_EDITOR;

}  // namespace

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
