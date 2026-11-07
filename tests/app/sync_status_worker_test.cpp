// app::SyncStatusWorker: senkron kapatılınca (cancel) yoldaki eski sonuç uygulanmaz (SD7 review, yarış).
#include "app/sync_status_worker.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <doctest/doctest.h>
#include <mutex>
#include <vector>

using noteapp::app::SyncStates;
using noteapp::app::SyncStatusWorker;

namespace {

struct Deliveries {
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<std::uint64_t> generations;

    std::uint64_t wait_for(std::size_t count) {
        std::unique_lock lock(mutex);
        REQUIRE(arrived.wait_for(lock, std::chrono::seconds(10), [&] { return generations.size() >= count; }));
        return generations[count - 1];
    }
};

}  // namespace

TEST_CASE("sync status worker: a result requested before cancel is no longer current") {
    noteapp::core::Config config;
    config.sync.enabled = false;
    const std::vector<noteapp::core::NoteMeta> metas = {
        noteapp::core::NoteMeta{noteapp::core::NotePath::parse("a.md").value()}};
    Deliveries got;
    SyncStatusWorker worker([&](SyncStates, std::uint64_t generation) {
        const std::scoped_lock lock(got.mutex);
        got.generations.push_back(generation);
        got.arrived.notify_all();
    });
    worker.request(config, metas);
    const std::uint64_t before = got.wait_for(1);
    CHECK(worker.current(before));
    worker.cancel();  // senkron kapandı: UI bu nesli artık uygulamaz
    CHECK_FALSE(worker.current(before));
    worker.request(config, metas);
    const std::uint64_t after = got.wait_for(2);
    CHECK(after != before);
    CHECK(worker.current(after));
}
