#pragma once

#include "app/update_install.hpp"
#include "app/updater.hpp"
#include "core/error.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace noteapp::app {

// Kurulumu yapan fonksiyon: üretimde install_update, testlerde sahte.
using UpdateInstaller = std::function<InstallResult(const InstallTarget&, const ReleaseInfo&,
                                                    std::string_view bytes, const core::UpdateLimits&,
                                                    std::stop_token)>;

// İşçinin durumu (arayüz bunu çizer). Denetim sonucu UpToDate (updater.hpp) ya da UpdateAvailable'dır.
struct UpdateIdle {
    bool operator==(const UpdateIdle&) const = default;
};
struct UpdateChecking {
    bool operator==(const UpdateChecking&) const = default;
};
struct UpdateAvailable {
    std::string version;
    std::string release_page;  // release_page_url: kendini güncelleyemeyen kurulum paketi buradan indirir
    bool installable = false;  // kurulum türü Unsupported değil: install() çalışır
    bool operator==(const UpdateAvailable&) const = default;
};
struct UpdateDownloading {
    std::string version;
    bool operator==(const UpdateDownloading&) const = default;
};
struct UpdateInstalling {
    std::string version;
    bool operator==(const UpdateInstalling&) const = default;
};
struct UpdateDone {
    std::string version;
    bool restart_required = false;  // Windows: çalışan exe'ler yeniden başlayınca yenilenir
    bool operator==(const UpdateDone&) const = default;
};
struct UpdateFailed {
    core::Error error;
};

using UpdateStatus = std::variant<UpdateIdle, UpdateChecking, UpToDate, UpdateAvailable, UpdateDownloading,
                                  UpdateInstalling, UpdateDone, UpdateFailed>;

struct UpdateWorkerDeps {
    HttpGet get;
    UpdateInstaller install;
    InstallTarget target;
    // Her durum değişikliğinden sonra işçi iş parçacığından (ya da check/install'ı çağırandan) çağrılır; veri
    // taşımaz, arayüz status()'u okur (TUI Post, GUI post). İçinden yalnız status()/busy() çağrılabilir:
    // check()/install() bitmekte olan iş parçacığını beklerdi. Boş olabilir.
    std::function<void()> on_change;
};

// Üretim: net::http_get, install_update, detect_install_target().
UpdateWorkerDeps default_update_deps();

// Güncellemenin denetimi ve kurulumu ağa çıkar, dakikalar sürebilir: işçi iş parçacığında çalışır, arayüzü
// bekletmez (SyncStatusWorker gibi). Aynı anda tek işlem: meşgulken check()/install() false döner. İptal
// edilen işlem önceki durumu geri getirir (denetim: önceki durum; kurulum: UpdateAvailable); değiştirme
// başladıktan sonra kurulum iptali yok sayar ve UpdateDone (ya da o evrenin hatası) olur. Tamamlanan denetim (UpToDate ya da UpdateAvailable) zamanı ve
// görülen sürümü save_update_state ile yazar; hata ve iptal yazmaz.
class UpdateWorker {
public:
    UpdateWorker(UpdateSettings settings, UpdateWorkerDeps deps);
    // Çalışan işlemi iptal eder ve bitmesini bekler.
    ~UpdateWorker();
    UpdateWorker(const UpdateWorker&) = delete;
    UpdateWorker& operator=(const UpdateWorker&) = delete;

    // Son sürümü sorar (UpdateChecking). Meşgulse false.
    bool check();
    // Son denetimin bulduğu sürümü indirir, doğrular ve kurar (UpdateDownloading -> UpdateInstalling ->
    // UpdateDone). Meşgulse, kurulacak sürüm yoksa (son denetim başarısızsa da) ya da kurulum türü Unsupported
    // ise false. Başarısız kurulumdan sonra yeniden denenebilir.
    bool install();
    // Çalışan işlemi iptal eder; boştayken etkisiz.
    void cancel();

    UpdateStatus status() const;
    bool busy() const;
    // Çalışan işlem bitene dek en çok `timeout` bekler; boştaysa true.
    bool wait_idle_for(std::chrono::milliseconds timeout);
    const std::string& current_version() const { return settings_.current_version; }

private:
    // Kilit altında çağrılır: işlemi başlatır, durumu `first` yapar.
    void start(UpdateStatus first, std::function<void(std::stop_token)> body);
    void run_check(const std::stop_token& stop, const UpdateStatus& before);
    void run_install(const std::stop_token& stop, const ReleaseInfo& release);
    // İşlem gövdesinden kaçan istisna UpdateFailed{update.internal_error} olur (loglanır).
    void run_guarded(const std::function<void()>& body);
    // Durumu (ve kurulacak sürümü) birlikte değiştirir, duyurur; meşgul olmayan durum işlemi bitirir
    // (bekleyenler uyanır).
    void set_status(UpdateStatus status);
    void set_status(UpdateStatus status, std::optional<ReleaseInfo> release);
    void notify() const;

    const UpdateSettings settings_;
    const UpdateWorkerDeps deps_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    UpdateStatus status_;
    std::optional<ReleaseInfo> release_;  // son denetimin bulduğu, henüz kurulmamış sürüm
    std::jthread operation_;  // son üye: önce o durur
};

// Açılışta bir kez (üç arayüzün main'i): önceki güncellemenin artıklarını siler
// (cleanup_after_update(detect_install_target())). Hata açılışı durdurmaz, loglanır.
void cleanup_previous_update();

}  // namespace noteapp::app
