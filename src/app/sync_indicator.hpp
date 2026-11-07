#pragma once

#include "app/notices.hpp"
#include "core/error.hpp"
#include "sync/sync_service.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace noteapp::app {

// Senkron göstergesi (SPEC-tui üst çubuk, SPEC-gui durum çubuğu): kapalı / ⟳ / ✓ saat / ⚠ neden. Saf
// modeldir: AutoSync durumu UI iş parçacığında buna yazılır; metnin genişliği ve rengi arayüzündür.
class SyncIndicator {
public:
    enum class State { Disabled, Running, Ok, Failed };

    // Durumdan doğan bildirim: arayüz kendi kuyruğuna koyar.
    struct Notice {
        NoticeKind kind = NoticeKind::Info;
        std::string text;
    };

    // Senkron kapalıyken (varsayılan durum).
    void disable();
    // Açılış: son kayıtlı sonuç (SyncService::status()) gösterilir; bildirim üretmez.
    void restore(const sync::SyncStatus& status);
    // AutoSync bildirimi. `running`: bir senkron sürüyor (⟳). Aksi halde `status` son sonuçtur: hata varsa ⚠,
    // yoksa başarı zamanı varsa ✓; ikisi de yoksa ilk senkron başlamak üzere: ⟳. Bildirim yalnız süren
    // senkronun sonucunda (Running'den çıkışta): başarıda "bitti"; hata bir kez, aynı hata araya başarı
    // girmedikçe yinelenmez (AutoSync her turu Running ile açar).
    [[nodiscard]] std::optional<Notice> update(const sync::SyncStatus& status, bool running);

    State state() const { return state_; }
    // Yalnız Ok'ta dolu: son başarının yerel "HH:MM"i.
    std::string success_time() const;
    // Yalnız Failed'da dolu: tam, çevrilmiş hata metni.
    std::string error_text() const;

private:
    void apply(const sync::SyncStatus& status, bool running);

    State state_ = State::Disabled;
    std::optional<std::chrono::system_clock::time_point> last_success_;
    std::optional<core::Error> last_error_;
    std::optional<std::string> reported_error_;  // son bildirilen hata metni
};

}  // namespace noteapp::app
