#include "sync/git_runtime.hpp"

#include <doctest/doctest.h>
#include <git2.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// Kullanıcının global git config'inin arama yolu; okunamazsa nullopt.
std::optional<std::string> global_search_path() {
    git_buf path = GIT_BUF_INIT;
    if (git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &path) < 0)
        return std::nullopt;
    std::string text(path.ptr, path.size);
    git_buf_dispose(&path);
    return text;
}

}  // namespace

// Hata (2026-11-08, Wine'da gui_tests çöküşü): GitRuntime her kurulumda libgit2'nin süreç geneli arama yolunu
// yazıyordu ve son yıkım onu serbest bırakıyordu; ikisi de kilitsizdir. GUI'de AutoSync (UI iş parçacığı) ile
// SyncStatusWorker aynı anda kurunca global arabellek iki kez serbest bırakıldı, yığın bozuldu ve sonraki
// testte ImGui'nin köşe noktaları GuiState'in üstüne yazıldı. Kurulum ve yıkım her iş parçacığından aynı anda
// güvenli olmalı, okunan yol hep aynı kalmalı.
TEST_CASE("GitRuntime is created and destroyed on many threads at once; the search path stays stable") {
    constexpr int kThreads = 8;
    constexpr int kRounds = 300;
    std::mutex mutex;
    std::set<std::string> seen;
    int unreadable = 0;
    {
        std::vector<std::jthread> threads;
        for (int thread = 0; thread < kThreads; ++thread)
            threads.emplace_back([&] {
                for (int round = 0; round < kRounds; ++round) {
                    const noteapp::sync::GitRuntime runtime;
                    const std::optional<std::string> path = global_search_path();
                    const std::scoped_lock lock(mutex);
                    if (path)
                        seen.insert(*path);
                    else
                        ++unreadable;
                }
            });
    }
    CHECK(unreadable == 0);
    CHECK(seen.size() == 1);
    CHECK(seen == std::set<std::string>{""});  // yalıtım: kullanıcının global config'i aranmaz
}
