# Spec: sync

> Modül: `sync` · Bağımlılık: core · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

`notes_dir`'i private bir GitHub reposuyla, gömülü libgit2 üzerinden, açılıp kapanabilir şekilde eşitlemek.
İlke: **hiçbir koşulda içerik kaybı yok** — force push yok, hard reset yok, çakışmada iki kopya.

## Arayüz (taslak)

```cpp
namespace noteapp::sync {
enum class Auth { Https, Ssh };                         // remote şemasından türetilir
struct SyncResult { int committed; int pulled; bool pushed; std::vector<core::NotePath> conflict_copies; };
struct SyncStatus { std::optional<std::chrono::system_clock::time_point> last_success; std::optional<core::Error> last_error; };
struct Author { std::string name; std::string email; };
Author resolve_author(const core::SyncConfig&);         // config → ~/.gitconfig → XDG git config → kullanıcı@makine
// Döngü 3–5'i "sync.push_rejected" dönerse bir kez daha koşar (testte adım enjekte edilir).
std::expected<bool, core::Error> retry_rejected_push(const std::function<std::expected<bool, core::Error>()>& exchange);

class SyncLock {                                        // <durum>/sync.lock; RAII, yalnız kendi kilidini siler
public:
    static std::expected<SyncLock, core::Error> acquire(const std::filesystem::path&, std::chrono::minutes stale_after);   // canlı+taze → sync.busy
};

class SyncService {
public:
    explicit SyncService(const core::Config&);
    std::expected<void, core::Error> enable(std::string_view remote);   // ilk kurulum (aşağıda)
    std::expected<SyncResult, core::Error> sync_now();                 // tek tam döngü
    SyncStatus status() const;                                          // <durum>/sync-status.toml
};

class AutoSync {                                        // tui/gui için arka plan iş parçacığı
public:
    AutoSync(SyncService&, std::chrono::seconds idle, std::function<void(const SyncStatus&)> on_status);
    void start();                                       // açılışta bir sync
    void notify_changed();                              // idle süre dolunca sync (debounce)
    void request_now();
    void stop(std::chrono::seconds timeout);            // çıkışta son sync, süre aşılırsa bırakır
};

std::expected<void, core::Error> store_token(std::string_view host, std::string_view token);
std::expected<void, core::Error> clear_token(std::string_view host);
bool has_token(std::string_view host);
}
```

## Davranış

### Kimlik doğrulama
- `https://…` → **token**: libgit2 credential callback'i `userpass("x-access-token", token)` döner. Token
  kaynağı: `NOTEAPP_GIT_TOKEN` ortam değişkeni (kalıcı saklanmaz) → OS kasası (keychain; servis `noteapp`,
  hesap = host). Callback en fazla **1** kez kimlik verir; ret olursa `sync.auth_failed` (sonsuz döngü yok).
  Önerilen token: fine-grained PAT, yalnız not reposu, *Contents: read/write*.
- `git@…` / `ssh://…` → **SSH**: libgit2 `USE_SSH=exec` ile sistemdeki `ssh` çağrılır; anahtar, agent ve
  `~/.ssh/config` ssh'ın kendisinden. Uygulama anahtar saklamaz. İlk bağlantıda host anahtarı onayı
  gerekiyorsa (TTY yok) `sync.ssh_host_unknown` hatası + "bir kez `ssh -T git@github.com` çalıştır" yönlendirmesi.
- Linux'ta Secret Service yoksa token düz dosyaya **düşülmez**; `sync.keychain_unavailable` + ortam değişkeni yönlendirmesi.
- Token config'e, loga, `.git/config`'e, URL'ye, hata mesajına asla yazılmaz.

### İlk kurulum (`enable`)
| Durum | Davranış |
|---|---|
| `.git` var, `origin` == remote | Hiçbir şey yapma |
| `.git` var, `origin` farklı/yok | `sync.remote_mismatch` → ön yüz onay alırsa origin güncellenir |
| `.git` yok, `notes_dir` boş | remote'u klonla (remote boşsa: init + origin + ilk commit + push) |
| `.git` yok, `notes_dir` dolu, remote boş | init + `.gitignore` + tümünü commit + push |
| `.git` yok, ikisi de dolu | Reddet: `sync.both_non_empty` + yönlendirme (açık soru SPEC.md §12/3) |

Init sırasında notes reposuna `.gitignore` yazılır: `.noteapp/` ve `.*.tmp`. Dal: `config.sync.branch` (varsayılan `main`).

### Senkron döngüsü (`sync_now`)
1. **Kilit:** `<durum>/sync.lock` (PID + zaman). Canlı başka süreç tutuyorsa `sync.busy` (çağıran sonra dener);
   süreci ölmüş ya da `tuning.sync_stale_lock_minutes`'dan (10 dk) eski kilit bayat sayılır.
2. **Commit:** tüm değişiklikler (ekleme/silme dahil) sahnelenir; değişiklik varsa commit atılır.
   Mesaj: i18n `sync.commit_message` (`{count}` dosya, `{host}`, zaman). Yazar: config → git global config →
   `OS_kullanıcı@host`.
3. **Fetch** `origin/<branch>`.
4. **Birleştirme analizi:**
   - güncel → 5'e geç;
   - fast-forward → `GIT_CHECKOUT_SAFE` ile checkout + ref ilerletme;
   - ortak ata yok (ilgisiz geçmiş) → `sync.unrelated_histories`, merge'e hiç başlanmaz (ilk kurulum `enable`'ın işi);
   - normal → merge. **Çakışan her dosya için:** yerel sürüm yerinde kalır; uzak sürüm
     `<ad> (conflict YYYY-MM-DD HHMM).<uzantı>` olarak yanına yazılır; silme/değiştirme çakışmasında
     **değiştirilmiş sürüm** korunur. Hepsi sahnelenir, iki ebeveynli merge commit atılır.
     - Ad: `<ad>` = uzantısız dosya adı, damga **yerel saat**, aynı klasörde. Uzantısız dosyada nokta yok
       (`yapilacaklar (conflict 2026-10-28 1430)`). Ad doluysa uzantıdan önce ` 2`, ` 3` …:
       `not (conflict 2026-10-28 1430) 2.md`. Kopya yalnız yoksa oluşturulur; var olan dosyanın üzerine yazılmaz.
     - İki tarafta aynı içerik çakışma değildir (kopya yok). İkili dosyalar aynı kurallarla, bayt bayt.
     - Kopyalar `SyncResult.conflict_copies`'te döner (gizli dosya kopyası `NotePath` olmadığından listede
       yok, diskte ve commit'te var) ve her biri loglanır. Merge mesajı: i18n `sync.merge_message`.
   - Döngü başında depo yarım bir git işlemindeyse (merge/rebase/cherry-pick; `git_repository_state` ≠ NONE)
     `sync.repo_busy_state`: commit dahil hiçbir şeye dokunulmaz, kullanıcı git ile bitirir ya da iptal eder.
5. **Push** (yerel öndeyse). Non-fast-forward ret → 3'ten bir kez daha; yine olmazsa `sync.push_rejected`.
6. **Durum** `<durum>/sync-status.toml`'a yazılır, loglanır; kilit bırakılır.

Asla: force push, `reset --hard`, çalışma ağacında commit'lenmemiş değişikliği ezen checkout.

### Aç/kapa
`sync.enabled = false` iken hiçbir otomatik senkron çalışmaz; repo ve geçmiş olduğu gibi kalır. Tekrar
açmak `enable`'ı çağırır (repo varsa no-op).

## Kabul kriterleri (yerel bare repo ile, ağsız)

1. Fast-forward: A'da yeni not → sync; B sync → not B'de, B'de yerel değişiklik kaybı yok.
2. Çakışmasız birleşme: A ve B farklı notları değiştirir → ikisi de sync → her iki değişiklik her iki tarafta.
3. Çakışma: aynı not iki tarafta değişir → ikinci sync'te yerel sürüm yerinde, uzak sürüm `(conflict …)`
   kopyasında; içerik kaybı yok; merge commit iki ebeveynli.
4. Silme/değiştirme çakışması → değiştirilmiş sürüm diskte kalır.
5. Push yarışı (iki sync arasında remote ilerler) → bir yeniden deneme ile başarılı.
6. Kilit tutuluyken ikinci süreç `sync.busy` alır; bayat kilit devralınır.
7. Credential callback ikinci çağrıda hata döner (sayaç testi); token config/log/`.git/config`/notes_dir'de
   geçmez (grep testi).
8. `enable` tablosundaki 5 durumun her biri için test.
9. `AutoSync`: `notify_changed()` art arda çağrılınca idle süre içinde tek sync; `stop(timeout)` süreyi aşmaz.

**Elle (Large):** gerçek private GitHub reposu ile Windows ↔ Fedora; HTTPS+token ve SSH ikisi de.

## Uygulama kararları (T03 spike bulguları, 2026-10-28)

1. **Kullanıcının git config'i izole edilir.** Bu makinede global/system `core.autocrlf=true` ve
   `url.https://github.com/.insteadOf git@github.com:` var; libgit2 bunları uygular. Notlar reposunda global/system/XDG
   arama yolları boşaltılır (`git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, …, "")`): satır sonları dönüştürülmez, remote
   adresi gizlice değişmez. Yazar adı/e-postası için global config izolasyondan **önce** bir kez okunur.
2. **Kimlik doğrulama türü**, izole config ile okunan `remote` URL şemasından türetilir.
3. **SSH asılı kalma riski:** Win32-OpenSSH, host anahtarı bilinmiyorsa onay bekleyip asılı kalıyor; libgit2 exec
   backend'ine Windows'ta argüman (`BatchMode`) geçirilemiyor (`GIT_SSH_COMMAND`/argümanlı `core.sshCommand` çalışmıyor).
   Önlemler: (a) SSH senkronundan önce `ssh-keygen -F <host>` ön kontrolü; kayıt yoksa `sync.ssh_host_unknown`;
   (b) **her senkron ayrı bir `note sync --background` sürecinde koşar**: `AutoSync` (TUI/GUI) ve CLI bu süreci
   başlatıp bekler, `tuning.sync_timeout_seconds` (defaults.toml, 120) aşılırsa sonlandırır. Sonlandırma sonrası ilk
   senkron, `sync.lock` bayatsa `.git/index.lock` gibi bayat libgit2 kilitlerini temizler.
4. Remote her zaman bir repoya bağlı oluşturulur (bağımsız/"detached" remote, exec SSH ile çöküyor).
5. libgit2'ye yollar UTF-8 verilir (`generic_u8string`). Testlerde Windows'ta salt-okunur git nesneleri silinmeden
   önce öznitelikleri temizlenir.
6. Doğrulandı: WinHTTP üzerinden HTTPS (GitHub'dan ref listesi), keychain gidiş-dönüşü (Windows Credential Manager).
   SSH-exec gerçek bağlantısı Kontrol noktası 4'e kaldı.
7. **Merge (T25):** `git_merge`, dosya tercihi NORMAL, checkout `SAFE | USE_OURS | DONT_OVERWRITE_IGNORED`:
   çakışan dosyaya `<<<<<<<` işaretli metin hiç yazılmaz, yolda doğrudan yerel sürüm durur (silme/değiştirmede
   checkout tek kalan sürümü yazar). Uygulama yalnız yeni kopya dosyası oluşturur, hiçbir dosyanın üzerine yazmaz.
   **Yeniden adlandırma tespiti kapalı:** açıkken libgit2 farklı yollardaki girdileri tek çakışmada birleştiriyor
   ve checkout bir tarafı yazmıyor; kapalıyken bir tarafta adlandırma + diğerinde düzenleme iki dosya bırakır
   (fazlalık, kayıp değil). Merge başladıktan sonraki bir hata (ör. dosya/klasör ad çakışması) depoyu MERGE
   durumunda bırakır → sonraki senkron `sync.repo_busy_state`; iki tarafın içeriği HEAD'de ve uzak dalda durur.
   Push yarışı: ret → fetch → yeniden merge → push (bir kez); testte `sync_now(before_push)` dikiş noktasıyla.

## Kapsam dışı
Rebase, seçmeli (dosya bazlı) senkron, GitHub dışı sağlayıcılara özel destek (genel git remote zaten çalışır),
LFS, şifreli repo, Git Credential Manager entegrasyonu.
