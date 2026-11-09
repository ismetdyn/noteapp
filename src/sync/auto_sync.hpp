#pragma once

#include "core/config.hpp"
#include "sync/sync_service.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace noteapp::sync {

// TUI/GUI arka plan senkronu (SPEC-sync karar 3): tek işçi iş parçacığı; her senkron ayrı bir
// `note sync --background` sürecidir. Süreç `tuning.sync_timeout_seconds` içinde bitmezse sonlandırılır ve
// durum "sync.timeout" olur (asılı ssh arayüzü kilitlemez; sonraki senkron artıkları temizler).
class AutoSync {
public:
    // `running` true: bir senkron başladı (`status` önceki sonuç: ⟳ göstergesi); false: bitti (`status`
    // durum dosyasından yeni sonuç). İŞÇİ İŞ PARÇACIĞINDAN çağrılır: ön yüz kendi UI iş parçacığına
    // aktarmalı (FTXUI: ScreenInteractive::Post; ImGui: GuiState::post + SDL_PushEvent).
    // Callback içinden notify_changed/request_now çağrılabilir; stop() ve yıkıcı çağrılamaz (kilitlenir).
    using StatusCallback = std::function<void(const SyncStatus& status, bool running)>;

    // `note_exe`: senkronu koşacak `note` ikilisi (ön yüzler core::note_executable_beside_self()). Süreler
    // config'ten: sync.idle_seconds (debounce), tuning.sync_timeout_seconds (bir senkronun üst sınırı).
    // Yalnız `config.sync.enabled` iken kurulmalı; `note sync --background` de kapalıyken hiçbir şey yapmaz.
    AutoSync(const core::Config& config, std::filesystem::path note_exe, StatusCallback on_status);
    ~AutoSync();  // stop() çağrılmadıysa stop(0): süren senkron sonlandırılır, son senkron yok

    AutoSync(const AutoSync&) = delete;
    AutoSync& operator=(const AutoSync&) = delete;

    void start();           // açılışta bir senkron (= request_now)
    void notify_changed();  // her kayıtta; son bildirimden idle süre sonra tek senkron (debounce)
    void request_now();     // hemen senkron (Ctrl-S / F5); sürmekte olan varsa o bitince
    // Çıkışta: bekleyen değişiklik varsa son bir senkron; toplam en çok `timeout` (aşılırsa süren senkron
    // sonlandırılır). Döndükten sonra callback çağrılmaz.
    void stop(std::chrono::seconds timeout);

private:
    void run_worker();
    void run_once();

    const std::chrono::seconds idle_;
    const std::chrono::seconds timeout_;
    const std::filesystem::path note_exe_;
    const StatusCallback on_status_;
    const SyncService service_;  // yalnız durum dosyası: okuma ve zaman aşımı kaydı

    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::chrono::steady_clock::time_point> due_;  // sıradaki senkron zamanı
    bool changed_ = false;                                      // son senkron başladıktan sonra değişiklik
    bool stopping_ = false;
    std::chrono::steady_clock::time_point stop_deadline_;
    std::thread worker_;  // en son: diğer üyeler hazırken başlar
};

}  // namespace noteapp::sync
