#pragma once

#include "core/config.hpp"
#include "core/note_store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace noteapp::app {

using SyncStates = std::map<core::NotePath, core::SyncState>;

// sync::fill_sync_status 5.000 notta onlarca ms sürer: işçi iş parçacığında çalışır, tuşu asla bekletmez. Üst
// üste gelen istekten yalnız sonuncusu çalışır. `deliver` işçiden çağrılır; arayüz sonucu UI iş parçacığına
// aktarır (TUI Post, GUI post) ve uygularken `current(generation)` ile bakar: cancel()den önceki istek (senkron
// kapandı) yolda olsa da eski durumu geri getirmez.
class SyncStatusWorker {
public:
    using Deliver = std::function<void(SyncStates, std::uint64_t generation)>;

    explicit SyncStatusWorker(Deliver deliver);
    ~SyncStatusWorker();
    SyncStatusWorker(const SyncStatusWorker&) = delete;
    SyncStatusWorker& operator=(const SyncStatusWorker&) = delete;

    void request(const core::Config& config, std::span<const core::NoteMeta> notes);
    // Bekleyen işi düşürür; o ana dek verilmiş isteklerin sonuçları eskir.
    void cancel();
    // Sonuç, son cancel()den sonra istenmiş mi (UI iş parçacığında, uygulamadan hemen önce).
    bool current(std::uint64_t generation) const { return generation == generation_.load(); }

private:
    struct Job {
        core::Config config{};
        std::vector<core::NoteMeta> notes;
        std::uint64_t generation = 0;
    };
    void run(const std::stop_token& stop);

    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Job> job_;
    std::atomic<std::uint64_t> generation_{0};
    std::jthread worker_;  // son üye: önce o durur
};

}  // namespace noteapp::app
