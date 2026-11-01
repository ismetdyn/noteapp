#pragma once

#include "core/config.hpp"
#include "core/note_store.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace noteapp::tui {

using SyncStates = std::map<core::NotePath, core::SyncState>;

// sync::fill_sync_status 5.000 notta onlarca ms sürer: işçi iş parçacığında çalışır, tuşu asla bekletmez. Üst
// üste gelen istekten yalnız sonuncusu çalışır. `deliver` işçiden çağrılır; main UI iş parçacığına Post eder.
class SyncStatusWorker {
public:
    using Deliver = std::function<void(SyncStates)>;

    explicit SyncStatusWorker(Deliver deliver);
    ~SyncStatusWorker();
    SyncStatusWorker(const SyncStatusWorker&) = delete;
    SyncStatusWorker& operator=(const SyncStatusWorker&) = delete;

    void request(const core::Config& config, std::span<const core::NoteMeta> notes);

private:
    struct Job {
        core::Config config;
        std::vector<core::NoteMeta> notes;
    };
    void run(const std::stop_token& stop);

    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Job> job_;
    std::jthread worker_;  // son üye: önce o durur
};

}  // namespace noteapp::tui
