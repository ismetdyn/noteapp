// `notes-gui --help`: pencere açmadan kısa kullanım; tanınmayan argüman çıkış 2. Argümansız açılış değişmez
// (pencere testleri onu sınar).
#include "support/run_process.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <string>
#include <vector>

#ifndef _WIN32
namespace {

// `timeout`: argümanı tanımayan eski ikili pencereyi açıp beklerdi; test asılmasın.
noteapp::test::RunResult run_gui(const std::vector<std::string>& args) {
    std::vector<std::string> command{"20", NOTEAPP_GUI_EXE};
    command.insert(command.end(), args.begin(), args.end());
    return noteapp::test::run_process("/usr/bin/timeout", command);
}

}  // namespace

TEST_CASE("notes-gui --help prints the short usage without a window; an unknown argument exits 2") {
    const auto help = run_gui({"--help"});
    CHECK(help.exit_code == 0);
    CHECK(help.out.find("notes-gui") != std::string::npos);
    const auto bad = run_gui({"import", "x.md"});
    CHECK(bad.exit_code == 2);
    CHECK_FALSE(bad.err.empty());
}
#endif
