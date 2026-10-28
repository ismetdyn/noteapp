#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "sync/git_runtime.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::sync {

struct SyncResult {
    int committed = 0;  // yerel commit'e giren dosya sayısı (commit yoksa 0)
    int pulled = 0;     // uzaktan gelen değişikliklerin değiştirdiği dosya sayısı
    bool pushed = false;
    std::vector<core::NotePath> conflict_copies;  // T25'e kadar hep boş
};

struct SyncStatus {
    std::optional<std::chrono::system_clock::time_point> last_success;
    std::optional<core::Error> last_error;  // yalnız gösterim için: kod saklanmaz, Code::Io döner
};

struct Author {
    std::string name;
    std::string email;
};

// Commit yazarı, alan alan: config.sync.author_* → ~/.gitconfig ($HOME, yoksa ev dizini) →
// $XDG_CONFIG_HOME/git/config (yoksa ~/.config/git/config) → ad = OS kullanıcısı, e-posta =
// <kullanıcı>@<makine>. Git dosyaları açıkça okunur; libgit2'nin (izole edilmiş) arama yollarına bağlı değil.
Author resolve_author(const core::SyncConfig& sync);

// Döngünün 3–5. adımları (`exchange`: fetch → birleştir → push; push yapıldıysa true) "sync.push_rejected"
// ile dönerse bir kez daha koşar. Ayrı fonksiyon: uzak deponun fetch ile push arasında ilerlemesi testte
// deterministik kurulamıyor, yeniden deneme kuralı enjekte adımla sınanır.
std::expected<bool, core::Error>
retry_rejected_push(const std::function<std::expected<bool, core::Error>()>& exchange);

// notes_dir'i `origin` uzak deposuyla eşitler (SPEC-sync "Senkron döngüsü"). notes_dir, origin'i tanımlı
// bir git deposu olmalı (kurulum `enable`'ın işi, T26); değilse "sync.not_initialized".
class SyncService {
public:
    explicit SyncService(const core::Config& config);

    // Tek tam döngü: kilit → commit → fetch → fast-forward → push → durum dosyası. Asla force push,
    // hard reset ya da commit'lenmemiş değişikliği ezen checkout yapmaz. Ayrışmış geçmişte (T25'e kadar)
    // hiçbir şeye dokunmadan "sync.diverged_not_supported".
    std::expected<SyncResult, core::Error> sync_now();

    // state_dir()/sync-status.toml; dosya yoksa ya da bozuksa boş durum.
    SyncStatus status() const;

private:
    std::expected<SyncResult, core::Error> run_cycle();
    void record(const std::expected<SyncResult, core::Error>& outcome) const;

    GitRuntime runtime_;
    core::Config config_;
};

}  // namespace noteapp::sync
