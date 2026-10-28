#pragma once

#include "core/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>

namespace noteapp::sync {

// Bu yaştan eski kilit, sahibi yaşasa bile bayattır (SPEC-sync döngü 1): asılı kalmış bir senkron
// sonsuza dek engellemesin.
inline constexpr std::chrono::minutes kStaleLockAge{10};

// Süreçler arası senkron kilidi: dosya (PID + ISO zaman + rastgele belirteç) özel olarak oluşturulur,
// yıkıcıda — hâlâ bizimse — silinir.
class SyncLock {
public:
    // Canlı bir süreç tutuyor ve kilit kStaleLockAge'den gençse Error{"sync.busy", {pid}}. Bayat kilit
    // (sahibi ölmüş ya da eski) devralınır ve loglanır. Dizin yoksa oluşturulur.
    static std::expected<SyncLock, core::Error> acquire(const std::filesystem::path& file);

    SyncLock(SyncLock&& other) noexcept;
    SyncLock& operator=(SyncLock&&) = delete;
    SyncLock(const SyncLock&) = delete;
    SyncLock& operator=(const SyncLock&) = delete;
    ~SyncLock();

private:
    SyncLock(std::filesystem::path file, std::string content)
        : file_(std::move(file)), content_(std::move(content)) {}
    std::filesystem::path file_;  // boş: taşınmış, bırakılacak kilit yok
    std::string content_;         // dosya hâlâ bunu içeriyorsa kilit bizimdir
};

}  // namespace noteapp::sync
