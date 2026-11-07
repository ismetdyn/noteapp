#include "app/sync_status_worker.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "sync/note_status.hpp"

#include <utility>

namespace noteapp::app {

SyncStatusWorker::SyncStatusWorker(Deliver deliver)
    : deliver_(std::move(deliver)), worker_([this](const std::stop_token& stop) { run(stop); }) {}

SyncStatusWorker::~SyncStatusWorker() = default;

void SyncStatusWorker::request(const core::Config& config, std::span<const core::NoteMeta> notes) {
    {
        const std::scoped_lock lock(mutex_);
        job_ = Job{config, {notes.begin(), notes.end()}, generation_.load()};
    }
    wake_.notify_all();
}

void SyncStatusWorker::cancel() {
    const std::scoped_lock lock(mutex_);
    job_.reset();
    ++generation_;
}

void SyncStatusWorker::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock, stop, [this] { return job_.has_value(); });
        if (stop.stop_requested() || !job_)
            return;
        Job job = std::move(*job_);
        job_.reset();
        lock.unlock();
        if (const auto filled = sync::fill_sync_status(job.config, job.notes); filled) {
            SyncStates states;
            for (const core::NoteMeta& note : job.notes)
                states.emplace(note.path, note.sync_status);
            if (current(job.generation))  // UI yine bakar: teslimle uygulama arasında da iptal olabilir
                deliver_(std::move(states), job.generation);
        } else {
            core::log_warn("event=sync_status_failed detail=" + core::format_error(filled.error()));
        }
        lock.lock();
    }
}

}  // namespace noteapp::app
