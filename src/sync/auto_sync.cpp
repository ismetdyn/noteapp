#include "sync/auto_sync.hpp"

#include "core/error.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace noteapp::sync {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Süren senkron bu aralıkla yoklanır: stop() ve zaman aşımı en çok bu gecikmeyle fark edilir.
constexpr std::chrono::milliseconds kPollInterval{50};

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

}  // namespace

fs::path note_executable_beside_self() {
#ifdef _WIN32
    constexpr const char* kName = "note.exe";
#else
    constexpr const char* kName = "note";
#endif
    // Kendi yolu bulunamazsa yalnız ad kalır: PATH'te aranır.
    return core::current_executable().parent_path() / kName;
}

AutoSync::AutoSync(const core::Config& config, fs::path note_exe, StatusCallback on_status)
    : idle_(config.sync.idle_seconds), timeout_(config.tuning.sync_timeout_seconds),
      note_exe_(std::move(note_exe)), on_status_(std::move(on_status)), service_(config) {
    worker_ = std::thread([this] { run_worker(); });
}

AutoSync::~AutoSync() {
    stop(std::chrono::seconds(0));
}

void AutoSync::start() {
    request_now();
}

void AutoSync::notify_changed() {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        changed_ = true;
        // Her bildirim zamanı ileri iter; sırada bekleyen "hemen" isteği ise geciktirilmez.
        const auto now = Clock::now();
        if (!due_ || *due_ > now)
            due_ = now + idle_;
    }
    wake_.notify_all();
}

void AutoSync::request_now() {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        due_ = Clock::now();
    }
    wake_.notify_all();
}

void AutoSync::stop(std::chrono::seconds timeout) {
    if (!worker_.joinable())
        return;  // zaten durdu (stop sonrası yıkıcı)
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        stop_deadline_ = Clock::now() + timeout;
        // Son senkron yalnız kaydedilmemiş değişiklik için; çıkışta "hemen" istekleri düşer.
        if (changed_)
            due_ = Clock::now();
        else
            due_.reset();
    }
    wake_.notify_all();
    worker_.join();
}

void AutoSync::run_worker() {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (stopping_ && (!due_ || Clock::now() >= stop_deadline_))
            return;
        if (!due_) {
            wake_.wait(lock);
            continue;
        }
        if (Clock::now() < *due_) {
            wake_.wait_until(lock, *due_);
            continue;
        }
        due_.reset();
        changed_ = false;  // bundan sonraki bildirimler yeni bir senkron ister
        lock.unlock();
        run_once();
        lock.lock();
    }
}

void AutoSync::run_once() {
    on_status_(service_.status(), true);
    const auto started = Clock::now();
    std::optional<core::Error> failure;
    auto child = core::spawn_detached({utf8(note_exe_), "sync", "--background"});
    if (!child) {
        failure = child.error();
    } else {
        for (;;) {
            const auto ended = core::wait_with_timeout(*child, kPollInterval);
            if (ended && ended->has_value())
                break;  // çıkış kodu önemsiz: sonucu süreç durum dosyasına yazdı
            auto deadline = started + timeout_;
            {
                const std::lock_guard lock(mutex_);
                if (stopping_)
                    deadline = std::min(deadline, stop_deadline_);
            }
            if (!ended || Clock::now() >= deadline) {
                // Asılı kalmış (ör. host anahtarı onayı bekleyen ssh) süreç arayüzü kilitlemesin; bıraktığı
                // kilitleri sonraki senkron temizler (SyncLock bayat → recover_interrupted_sync).
                core::terminate(*child);
                const auto waited = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started);
                failure = ended ? core::Error{core::Error::Code::Io,
                                              "sync.timeout",
                                              {{"seconds", std::to_string(waited.count())}}}
                                : ended.error();
                break;
            }
        }
    }
    if (failure)
        service_.record_failure(*failure);
    on_status_(service_.status(), false);
}

}  // namespace noteapp::sync
