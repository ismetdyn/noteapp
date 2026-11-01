#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "sync/git_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::sync {

// Bir cihazın günlüğü: log_seq dosyadaki son işlem, applied_seq bu cihazın uyguladığı. log_seq < applied_seq:
// o cihaz geri alınmış olabilir. Sessiz kalmaz, durum dosyasına ve arayüze çıkar (SPEC-graph "Görünürlük").
struct DeviceLogHead {
    std::string device;  // 32 küçük hex
    std::uint64_t log_seq = 0;
    std::uint64_t applied_seq = 0;
};

struct SyncResult {
    int committed = 0;  // bu cihazın gönderilmemiş yazması olan not sayısı; çöpe atma 1, kalıcı silme 0
    int pulled = 0;     // uzaktan gelip canlı ya da çöp listesinde değişen not sayısı
    bool pushed = false;
    std::vector<core::NotePath> conflict_copies;  // birleştirmede oluşan "(conflict …)" kopyaları
    std::uint64_t exported_seq = 0;               // günlük dosyasına yazılan son işlem
    std::uint64_t pushed_seq = 0;                 // uzağa ulaşan son işlem numarası
    std::string pushed_log;                       // bu numaranın ait olduğu günlük
    std::vector<core::Error> log_issues;  // stalled, bad_frames, partial_tails, omitted, kenara alınan dosya
    std::vector<DeviceLogHead> heads = {};
};

struct SyncStatus {
    std::optional<std::chrono::system_clock::time_point> last_success;
    std::optional<core::Error> last_error;  // yalnız gösterim için: kod saklanmaz, Code::Io döner
    std::uint64_t pushed_seq = 0;           // note_status: bundan büyük yazma henüz uzakta değil
    // pushed_seq'in ait olduğu günlük (`<cihaz>.oplog`). graph.db kaybolursa cihaz yeni kimlik alır: eski
    // numara geçmez.
    std::string pushed_log = {};
    std::vector<core::Error> log_issues = {};  // son başarılı senkronun bildirdikleri
    std::vector<DeviceLogHead> heads = {};
};

struct Author {
    std::string name;
    std::string email;
};

// Commit yazarı, alan alan: config.sync.author_* → ~/.gitconfig ($HOME, yoksa ev dizini) →
// $XDG_CONFIG_HOME/git/config (yoksa ~/.config/git/config) → ad = OS kullanıcısı, e-posta =
// <kullanıcı>@<makine>. Git dosyaları açıkça okunur; libgit2'nin (izole edilmiş) arama yollarına bağlı değil.
Author resolve_author(const core::SyncConfig& sync);

// Yalnız test dikiş noktaları; üretim kodu vermez.
struct SyncTestHooks {
    // Her turda birleştirme ve günlük almadan sonra, push'tan hemen önce: uzak deponun fetch ile push
    // arasında ilerlediği gerçek yarışı kurar.
    std::function<void()> before_push;
    // Her çakışma kopyası diske yazıldıktan hemen sonra (depo-göreli yol). Hata dönerse birleştirme o hatayla
    // düşer: yarıda kalan merge'ün geri alınması sınanır.
    std::function<std::expected<void, core::Error>(const std::string& copy)> after_conflict_copy;
};

// Döngünün 3–5. adımları (`exchange`: fetch → birleştir → push; push yapıldıysa true) "sync.push_rejected"
// ile dönerse bir kez daha koşar. Ayrı fonksiyon: yeniden deneme kuralı enjekte adımla da sınanır.
std::expected<bool, core::Error>
retry_rejected_push(const std::function<std::expected<bool, core::Error>()>& exchange);

// graph.db'yi `origin` üzerinden diğer cihazlarla eşitler (SPEC-sync "Senkron döngüsü"): git yalnız
// taşıyıcıdır,
// `<notes_dir>/graph/*.oplog` taşınır. notes_dir, origin'i tanımlı bir git deposu olmalı (kurulum `enable`'ın
// işi, T26); değilse "sync.not_initialized".
class SyncService {
public:
    explicit SyncService(const core::Config& config);

    // İlk kurulum (SPEC-sync "İlk kurulum" tablosu), senkron kilidi altında:
    //  - .git var, origin == remote → no-op; origin farklı/yok → "sync.remote_mismatch" {current, remote},
    //    `replace_origin` (ön yüz onayı) ile origin değiştirilir;
    //  - .git yok, uzak boş → init + .gitignore + .gitattributes + bu cihazın günlüğü commit + push;
    //  - .git yok, notes_dir boş (gizli girdiler sayılmaz), uzakta `sync.branch` var → klon (init + origin +
    //    fetch + checkout); dal yoksa "sync.remote_branch_missing";
    //  - .git yok, ikisi de dolu → "sync.both_non_empty".
    // Ağa çıkmadan adres doğrulanır ve SSH ön kontrolü / HTTPS token'ı hazırlanır (bkz. auth.hpp). Hata
    // olursa oluşturulan .git ve bu çağrının yazdığı .gitignore silinir; notlara dokunulmaz.
    std::expected<void, core::Error> enable(std::string_view remote, bool replace_origin = false);

    // Tek tam döngü: kilit → kendi günlüğünü al → ver → commit → fetch → fast-forward ya da merge →
    // günlükleri al → ver → push → durum dosyası. Asla force push, hard reset ya da commit'lenmemiş
    // değişikliği ezen checkout yapmaz. Gövde çakışması motorun head'lerinden çözülür: kazanan yerinde,
    // kaybeden kopya not olur. Yarım kalmış elle git işleminde "sync.repo_busy_state", ortak geçmiş yoksa
    // "sync.unrelated_histories" — ikisinde de birleştirmeye dokunulmaz.
    std::expected<SyncResult, core::Error> sync_now(const SyncTestHooks& hooks = {});

    // state_dir()/sync-status.toml; dosya yoksa ya da bozuksa boş durum.
    SyncStatus status() const;

    // Senkron sürecinin kendisi yazamadığı hatayı durum dosyasına (ve loga) yazar: AutoSync'in sonlandırdığı
    // süreç ("sync.timeout") ya da hiç başlatılamayan `note`. Önceki başarı zamanı korunur.
    void record_failure(const core::Error& error) const;
    // Bu cihaza ait son gönderilen işlem: durum dosyası başka kimliğe aitse 0.
    std::uint64_t pushed_seq_of(std::string_view own_log) const;

private:
    std::expected<SyncResult, core::Error> run_cycle(const SyncTestHooks& hooks);
    void record(const std::expected<SyncResult, core::Error>& outcome) const;

    GitRuntime runtime_;
    core::Config config_;
};

}  // namespace noteapp::sync
