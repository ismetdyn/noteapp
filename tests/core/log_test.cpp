#include "core/log.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp::core;
using namespace noteapp::test;

namespace {

constexpr std::size_t kBig = 1 << 20;

}  // namespace

TEST_CASE("log line format: ISO timestamp, level, pid, message") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    init_log(kBig);
    log_info("merhaba dünya çğıİöşü");
    log_warn("w");
    log_error("e");

    const std::regex re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2} (info|warn|error) \d+ )");
    std::istringstream lines(slurp(state_dir() / "log.txt"));
    std::string line;
    std::vector<std::string> all;
    while (std::getline(lines, line))
        all.push_back(line);
    REQUIRE(all.size() == 3);
    for (const auto& l : all)
        CHECK(std::regex_search(l, re));
    CHECK(all[0].find(" info ") != std::string::npos);
    CHECK(all[0].ends_with("merhaba dünya çğıİöşü"));
    CHECK(all[1].find(" warn ") != std::string::npos);
    CHECK(all[2].find(" error ") != std::string::npos);
}

TEST_CASE("log rotates to log.1.txt when the max size is exceeded and replaces the old rotation") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    init_log(200);
    for (int i = 0; i < 40; ++i)
        log_info("satır numarası " + std::to_string(i));

    const fs::path current = state_dir() / "log.txt";
    const fs::path rotated = state_dir() / "log.1.txt";
    REQUIRE(fs::exists(rotated));
    CHECK(fs::file_size(current) <= 200 + 100);  // en çok bir satır taşabilir
    CHECK(slurp(current).find("satır numarası 39") != std::string::npos);
    CHECK_FALSE(fs::exists(state_dir() / "log.2.txt"));
}

TEST_CASE("logging into an unusable location neither throws nor crashes") {
    const TempDir tmp(kTurkishUncreated);
    fs::create_directories(tmp.path());
    // state dizini yerine aynı adlı bir dosya: create_directories başarısız olur.
    {
        std::ofstream(tmp.path() / "state") << "not a dir";
    }
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    init_log(kBig);
    CHECK_NOTHROW(log_error("hiçbir yere yazılamaz"));
    CHECK_NOTHROW(log_info(""));
}

TEST_CASE("concurrent logging from several threads keeps every line intact") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    init_log(kBig);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 25; ++i)
                log_info("thread " + std::to_string(t) + " line " + std::to_string(i));
        });
    for (auto& th : threads)
        th.join();
    std::istringstream lines(slurp(state_dir() / "log.txt"));
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        CHECK(line.find(" line ") != std::string::npos);
        ++count;
    }
    CHECK(count == 100);
}

TEST_CASE("control bytes in a message never reach log.txt raw") {
    const TempDir dir;
    const ScopedEnv home("NOTEAPP_HOME", dir.path().u8string());
    log_warn("ad \x1B[31mkirmizi\x07 \xC2\x9B c1 \xFF son\nikinci");
    const std::string text = slurp(state_dir() / "log.txt");
    CHECK(text.find('\x1B') == std::string::npos);
    CHECK(text.find('\x07') == std::string::npos);
    CHECK(text.find("\xC2\x9B") == std::string::npos);
    CHECK(text.find('\xFF') == std::string::npos);
    CHECK(text.find("\\x1b[31mkirmizi") != std::string::npos);
    CHECK(std::ranges::count(text, '\n') == 1);
    log_warn("türkçe ığ düz");
    CHECK(slurp(state_dir() / "log.txt").find("türkçe ığ düz") != std::string::npos);
}
