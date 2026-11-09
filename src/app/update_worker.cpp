#include "app/update_worker.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "net/http.hpp"

#include <exception>
#include <string_view>
#include <utility>

namespace noteapp::app {

namespace {

// Kendini güncelleyemeyen kurulum da denetler: kullanıcıya bu platformun paketini ve release sayfasını
// gösterir.
#ifdef _WIN32
constexpr AssetKind kPlatformAsset = AssetKind::WindowsZip;
#else
constexpr AssetKind kPlatformAsset = AssetKind::AppImage;
#endif

// install_update'in iptal hatası (update_install.cpp).
constexpr std::string_view kInstallCancelledKey = "update.install_cancelled";
constexpr std::string_view kInternalErrorKey = "update.internal_error";

bool is_running(const UpdateStatus& status) {
    return std::holds_alternative<UpdateChecking>(status) ||
           std::holds_alternative<UpdateDownloading>(status) || std::holds_alternative<UpdateInstalling>(status);
}

std::int64_t unix_now() {
    using std::chrono::system_clock;
    return std::chrono::floor<std::chrono::seconds>(system_clock::now()).time_since_epoch().count();
}

UpdateAvailable available_status(const ReleaseInfo& release, const InstallTarget& target) {
    return UpdateAvailable{.version = release.version,
                           .release_page = release_page_url(release),
                           .installable = asset_kind(target.kind).has_value()};
}

}  // namespace

UpdateWorkerDeps default_update_deps() {
    return UpdateWorkerDeps{.get = net::http_get,
                            .install = install_update,
                            .target = detect_install_target(),
                            .on_change = {}};
}

UpdateWorker::UpdateWorker(UpdateSettings settings, UpdateWorkerDeps deps)
    : settings_(std::move(settings)), deps_(std::move(deps)) {}

// operation_ son üyedir: yıkıcısı iptal ister ve işlemi bekler; işlem öbür üyeleri kullanırken hepsi yaşar.
UpdateWorker::~UpdateWorker() = default;

bool UpdateWorker::check() {
    std::jthread finished;
    {
        const std::scoped_lock lock(mutex_);
        if (is_running(status_))
            return false;
        UpdateStatus before = status_;
        finished = std::move(operation_);
        start(UpdateChecking{}, [this, before = std::move(before)](std::stop_token stop) {
            run_guarded([&] { run_check(stop, before); });
        });
    }
    notify();
    return true;  // `finished` (bitmiş önceki işlem) kilit dışında beklenir
}

bool UpdateWorker::install() {
    std::jthread finished;
    {
        const std::scoped_lock lock(mutex_);
        if (is_running(status_) || !release_ || !asset_kind(deps_.target.kind))
            return false;
        finished = std::move(operation_);
        start(UpdateDownloading{release_->version}, [this, release = *release_](std::stop_token stop) {
            run_guarded([&] { run_install(stop, release); });
        });
    }
    notify();
    return true;
}

void UpdateWorker::start(UpdateStatus first, std::function<void(std::stop_token)> body) {
    // Önce iş parçacığı: oluşturulamazsa (system_error) durum değişmemiş kalır. Gövde kilidi bekler, durumu
    // bu çağrı bitmeden göremez.
    operation_ = std::jthread(std::move(body));
    status_ = std::move(first);
}

void UpdateWorker::cancel() {
    const std::scoped_lock lock(mutex_);
    if (is_running(status_))
        operation_.request_stop();
}

UpdateStatus UpdateWorker::status() const {
    const std::scoped_lock lock(mutex_);
    return status_;
}

bool UpdateWorker::busy() const {
    const std::scoped_lock lock(mutex_);
    return is_running(status_);
}

bool UpdateWorker::wait_idle_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return !is_running(status_); });
}

void UpdateWorker::set_status(UpdateStatus status) {
    {
        const std::scoped_lock lock(mutex_);
        status_ = std::move(status);
    }
    idle_.notify_all();
    notify();
}

void UpdateWorker::set_status(UpdateStatus status, std::optional<ReleaseInfo> release) {
    {
        const std::scoped_lock lock(mutex_);
        release_ = std::move(release);
        status_ = std::move(status);
    }
    idle_.notify_all();
    notify();
}

void UpdateWorker::run_guarded(const std::function<void()>& body) {
    // İş parçacığı sınırı: kaçan istisna std::terminate olurdu (değiştirmenin ortasında da). İşlem hata olarak
    // biter, ayrıntı logdadır; kurulacak sürüm kalır (yarım kalan Windows değiştirmesini işaret korur).
    try {
        body();
    } catch (const std::exception& error) {
        core::log_error(std::string("event=update_operation_exception detail=\"") + error.what() + "\"");
        set_status(UpdateFailed{core::Error{core::Error::Code::Io, std::string(kInternalErrorKey), {}}});
    } catch (...) {
        core::log_error("event=update_operation_exception detail=\"unknown\"");
        set_status(UpdateFailed{core::Error{core::Error::Code::Io, std::string(kInternalErrorKey), {}}});
    }
}

void UpdateWorker::notify() const {
    if (deps_.on_change)
        deps_.on_change();
}

void UpdateWorker::run_check(const std::stop_token& stop, const UpdateStatus& before) {
    const AssetKind kind = asset_kind(deps_.target.kind).value_or(kPlatformAsset);
    UpdateCheck result = check_latest(deps_.get, settings_, kind, stop);
    if (!result) {
        // İptalin hatası (update.cancelled, net.cancelled) gösterilmez: denetim hiç başlamamış sayılır, önceki
        // denetimin bulduğu sürüm kalır. Başarısız denetim onu unutur: Failed durumunda kurulum olmaz.
        if (stop.stop_requested())
            set_status(before);
        else
            set_status(UpdateFailed{std::move(result.error())}, std::nullopt);
        return;
    }
    std::optional<ReleaseInfo> release;
    UpdateStatus next;
    std::string latest;
    if (const auto* up_to_date = std::get_if<UpToDate>(&*result)) {
        latest = up_to_date->latest_version;
        next = *up_to_date;
    } else {
        release = std::get<ReleaseInfo>(std::move(*result));
        latest = release->version;
        next = available_status(*release, deps_.target);
    }
    // Kaydedilemeyen zaman yalnız bir sonraki açılış denetimini erkene alır: sonuç yine gösterilir.
    const UpdateState state{.last_check_unix = unix_now(), .last_seen_version = latest};
    if (auto saved = save_update_state(state); !saved)
        core::log_warn("event=update_state_save_failed detail=" + core::format_error(saved.error()));
    set_status(std::move(next), std::move(release));
}

void UpdateWorker::run_install(const std::stop_token& stop, const ReleaseInfo& release) {
    const AssetKind kind = *asset_kind(deps_.target.kind);  // install() denetledi
    // İptal edilen işlem hata değildir: sürüm yine kurulabilir.
    const auto stopped_or_failed = [&](core::Error error, bool cancelled) {
        set_status(cancelled ? UpdateStatus(available_status(release, deps_.target))
                             : UpdateStatus(UpdateFailed{std::move(error)}));
    };
    auto bytes = download_verified(deps_.get, release, kind, settings_, stop);
    if (!bytes) {
        // İndirme hiçbir şeyi değiştirmez: iptalden sonraki her hata iptaldir.
        stopped_or_failed(std::move(bytes.error()), stop.stop_requested());
        return;
    }
    set_status(UpdateInstalling{release.version});
    const InstallResult outcome = deps_.install(deps_.target, release, *bytes, settings_.limits, stop);
    if (!outcome) {
        // Değiştirme başladıktan sonra kurulum iptali yok sayar: o evrenin hatası
        // (ör. update.rollback_failed) iptal istense de gösterilir. Yalnız kurulumun kendi iptal hatası
        // iptaldir.
        const bool cancelled = stop.stop_requested() && outcome.error().message_key == kInstallCancelledKey;
        stopped_or_failed(outcome.error(), cancelled);
        return;
    }
    core::log_info("event=update_installed version=" + release.version);
    set_status(UpdateDone{.version = release.version, .restart_required = outcome->restart_required},
               std::nullopt);
}

void cleanup_previous_update() {
    // Açılış sınırı: artık temizliği uygulamayı durdurmaz (kalan dosya bir sonraki açılışta yeniden denenir).
    try {
        cleanup_after_update(detect_install_target());
    } catch (const std::exception& error) {
        core::log_warn(std::string("event=update_cleanup_failed detail=") + error.what());
    }
}

}  // namespace noteapp::app
