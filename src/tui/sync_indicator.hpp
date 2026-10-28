#pragma once

#include "core/error.hpp"
#include "sync/sync_service.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace noteapp::tui {

// Üst çubuktaki senkron parçası (SPEC-tui): kapalı / ⟳ / ✓ saat / ⚠ kısa neden. Saf modeldir: AutoSync
// durumu UI iş parçacığında buna yazılır, çizim text()'i okur. FTXUI'ye bağlı değildir.
class SyncIndicator {
public:
    enum class State { Disabled, Running, Ok, Failed };

    // Senkron kapalıyken (varsayılan durum).
    void disable();
    // `running`: bir senkron sürüyor (⟳). Aksi halde `status` son sonuçtur: hata varsa ⚠, yoksa başarı
    // zamanı varsa ✓; ikisi de yoksa ilk senkron henüz yapılmadı (başlamak üzere): ⟳.
    void set_status(const sync::SyncStatus& status, bool running);

    State state() const { return state_; }
    std::string text() const;
    // Yalnız Failed'da dolu: durum satırında gösterilecek tam, çevrilmiş hata metni.
    std::string error_text() const;

private:
    State state_ = State::Disabled;
    std::optional<std::chrono::system_clock::time_point> last_success_;
    std::optional<core::Error> last_error_;
};

}  // namespace noteapp::tui
