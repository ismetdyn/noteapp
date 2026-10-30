#pragma once

#include "core/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>

namespace noteapp::sync {

// Süreçler arası senkron kilidi: dosya (PID + ISO zaman + rastgele belirteç) özel olarak oluşturulur,
// yıkıcıda — hâlâ bizimse — silinir.
class SyncLock {
public:
    // Sahibi canlıysa yaşı ne olursa olsun Error{"sync.busy", {pid}}. Bayat kilit (sahibi ölmüş; ya da sahibi
    // okunamıyor ve `stale_after`'dan eski, tuning.sync_stale_lock_minutes) devralınır ve loglanır (SPEC-sync
    // döngü 1). Sahibi okunamayan genç kilit de meşguldür. Dizin yoksa oluşturulur.
    static std::expected<SyncLock, core::Error> acquire(const std::filesystem::path& file,
                                                        std::chrono::minutes stale_after);

    SyncLock(SyncLock&& other) noexcept;
    SyncLock& operator=(SyncLock&&) = delete;
    SyncLock(const SyncLock&) = delete;
    SyncLock& operator=(const SyncLock&) = delete;
    ~SyncLock();

    // Bayat bir kilit devralındı: önceki sahip yarıda kalmış (öldürülmüş/çökmüş) olabilir; çağıran
    // onun artıklarını (libgit2 kilitleri, yarım merge) temizler.
    bool took_over() const { return took_over_; }

private:
    SyncLock(std::filesystem::path file, std::string content, bool took_over)
        : file_(std::move(file)), content_(std::move(content)), took_over_(took_over) {}
    std::filesystem::path file_;  // boş: taşınmış, bırakılacak kilit yok
    std::string content_;         // dosya hâlâ bunu içeriyorsa kilit bizimdir
    bool took_over_ = false;
};

}  // namespace noteapp::sync
