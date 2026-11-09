#pragma once

#include "app/notices.hpp"
#include "app/update_worker.hpp"
#include "core/limits.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Güncelleme penceresinin düğmeleri (TUI modalı, GUI açılır penceresi); UpdateView::actions çizim sırasıdır.
// CopyLink'i arayüz yapar (pano arayüzündedir); gerisi UpdateController::act.
enum class UpdateAction { Install, Cancel, CopyLink, Close };

// Düğmenin etiketi (i18n anahtarı).
const char* update_action_label(UpdateAction action);

// Pencerenin içeriği, işçinin durumundan. Metin çevrilmiştir; sürüm ve adres sunucudan gelir (güvenilmez):
// arayüz terminal_safe / printable ile çizer. Enter ve Esc her durumda Close'tur: kurulum yalnız açıkça
// Install seçilince başlar (CLI'nin [e/H] varsayılanı gibi).
struct UpdateView {
    std::vector<std::string> lines;
    std::string link;  // release sayfası; boşsa yok
    std::vector<UpdateAction> actions;
    bool busy = false;  // denetim, indirme ya da kurulum sürüyor

    bool operator==(const UpdateView&) const = default;
};

UpdateView update_view(const UpdateStatus& status, std::string_view current_version);

// TUI ve GUI'nin güncelleme akışı (SPEC-tui / SPEC-gui "Güncelleme"): açılış denetimi, palet komutu, pencere
// ve bildirimler. Yalnız UI iş parçacığından çağrılır; işçinin on_change'i (işçi iş parçacığı) UI iş
// parçacığına changed() çağrısını gönderir (TUI Post, GUI post). Arayüz yalnız çizer ve girdiyi act()'e
// eşler.
class UpdateController {
public:
    // Çıkışta işlemin iptali gözlemlesi için verilen süre: denetim ve indirme bu sürede durur.
    static constexpr std::chrono::milliseconds kExitGrace{500};

    UpdateController(UpdateSettings settings, UpdateWorkerDeps deps);

    // Açılış denetimi (arayüz açıldıktan sonra): `interval` (config.update.check_interval; 0 = kapalı)
    // dolduysa (should_check_on_startup) sessiz denetim başlar ve beklenmez. Sonucu yalnız yeni sürümse
    // bildirim olur; güncel ve hata yalnız loglanır. Başladıysa true.
    bool check_on_startup(std::chrono::system_clock::time_point now, const core::Limit& interval);
    // Palet komutu ve bildirimin eylemi: pencere açılır, güncelleme bildirimi kalkar (kullanıcı ona
    // davrandı). İşlem sürmüyorsa ve kurulacak ya da kurulmuş sürüm yoksa yeni denetim başlar.
    void open(NoticeQueue& notices);
    // Install: onaylanan kurulum başlar; Cancel: süren işlem iptal edilir; Close: pencere kapanır, süren
    // işlem arka planda biter ve sonucu bildirim olur. CopyLink etkisizdir.
    void act(UpdateAction action);
    bool is_open() const { return open_; }
    UpdateView view() const;
    bool busy() const { return worker_.busy(); }
    // on_change'in UI iş parçacığındaki devamı. Biten işlemin sonucu pencere kapalıysa bildirim olur; bildirim
    // eklendiyse true (pencere kapalıyken başka görünür değişiklik yoktur: boşta kare istenmez).
    bool changed(NoticeQueue& notices, NoticeQueue::Clock::time_point now);
    // Çıkış: süren işlemi iptal eder, en çok `grace` bekler. Hâlâ sürüyorsa true: kurulum değiştirme
    // evresinde iptali yok sayar, çıkış onu bekler (arayüz bunu söyler).
    bool stop_for_exit(std::chrono::milliseconds grace = kExitGrace);

private:
    // Sonucu kimin beklediği: açılış denetimi sessizdir; elle denetim ve kurulum sonucu söyler.
    enum class Origin { Startup, Manual, Install };

    bool start_check(Origin origin);

    UpdateWorker worker_;
    std::optional<Origin> pending_;  // biten ama henüz bildirilmemiş işlemin kaynağı
    bool open_ = false;
};

}  // namespace noteapp::app
