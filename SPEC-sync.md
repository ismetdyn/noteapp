# Spec: sync

> Modül: `sync` · Bağımlılık: core · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

`notes_dir`'i private bir GitHub reposuyla, gömülü libgit2 üzerinden, açılıp kapanabilir şekilde eşitlemek.
İlke: **hiçbir koşulda içerik kaybı yok** — force push yok, hard reset yok, çakışmada iki kopya.

## Arayüz (taslak)

```cpp
namespace noteapp::sync {
enum class Auth { None, Https, Ssh };                   // remote şemasından türetilir (auth.hpp)
struct SyncResult { int committed; int pulled; bool pushed; std::vector<core::NotePath> conflict_copies; };
struct SyncStatus { std::optional<std::chrono::system_clock::time_point> last_success; std::optional<core::Error> last_error; };
struct Author { std::string name; std::string email; };
Author resolve_author(const core::SyncConfig&);         // config → ~/.gitconfig → XDG git config → kullanıcı@makine
// Döngü 3–5'i "sync.push_rejected" dönerse bir kez daha koşar (testte adım enjekte edilir).
std::expected<bool, core::Error> retry_rejected_push(const std::function<std::expected<bool, core::Error>()>& exchange);

class SyncLock {                                        // <durum>/sync.lock; RAII, yalnız kendi kilidini siler
public:
    static std::expected<SyncLock, core::Error> acquire(const std::filesystem::path&, std::chrono::minutes stale_after);   // canlı sahip → sync.busy
};

class SyncService {
public:
    explicit SyncService(const core::Config&);
    std::expected<void, core::Error> enable(std::string_view remote, bool replace_origin = false);  // ilk kurulum
    std::expected<SyncResult, core::Error> sync_now();                 // tek tam döngü
    SyncStatus status() const;                                          // <durum>/sync-status.toml
};

std::filesystem::path note_executable_beside_self();    // ön yüzün yanındaki `note` (auto_sync.hpp)
class AutoSync {                                        // tui/gui için arka plan iş parçacığı (T27, karar 9)
public:
    using StatusCallback = std::function<void(const SyncStatus&, bool running)>;  // işçi iş parçacığından
    AutoSync(const core::Config&, std::filesystem::path note_exe, StatusCallback on_status);
    void start();                                       // açılışta bir sync
    void notify_changed();                              // idle süre dolunca sync (debounce)
    void request_now();
    void stop(std::chrono::seconds timeout);            // çıkışta son sync, süre aşılırsa sonlandırır
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
  hesap = host). Ortamdaki token yalnız `NOTEAPP_GIT_TOKEN_HOST`'taki host'a (varsayılan `github.com`,
  büyük/küçük harf duyarsız) verilir; başka host'lar kasadaki kendi kaydını kullanır — her https remote'a
  sunulsaydı yanlış ya da kötü niyetli bir adres onu alırdı. Callback en fazla **1** kez kimlik verir; ret olursa `sync.auth_failed` (sonsuz döngü yok).
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
1. **Kilit:** `<durum>/sync.lock` (PID + zaman). Canlı başka süreç tutuyorsa, kilit ne kadar eski olursa olsun
   `sync.busy` (çağıran sonra dener; devralmak canlı sürecin altındaki `.git/*.lock`'ları silerdi). Sahibi ölmüşse
   bayattır; PID okunamıyorsa (yarım/bozuk dosya) `tuning.sync_stale_lock_minutes`'dan (10 dk) eskiyse bayattır.
   Bilinen sınır: PID yeniden kullanılırsa ilgisiz canlı süreç çıkana dek kilit meşgul görünür.
2. **Commit:** tüm değişiklikler (ekleme/silme dahil) sahnelenir; değişiklik varsa commit atılır.
   Mesaj: i18n `sync.commit_message` (`{count}` dosya, `{host}`, zaman). Yazar: config → git global config →
   `OS_kullanıcı@host`.
3. **Fetch** `origin/<branch>`.
4. **Birleştirme analizi:**
   - güncel → 5'e geç;
   - fast-forward → `GIT_CHECKOUT_SAFE` ile checkout + ref ilerletme;
   - uzak ağaç bu sisteme yazılamıyorsa → `sync.remote_invalid_paths` {path}, yerel hiçbir şey değişmez: libgit2'nin
     reddettiği ad (Windows'ta `aux.md`) ya da `core.ignorecase` açıkken yalnız harf büyüklüğüyle ayrılan iki yol
     (checkout hata vermeden birini kaybederdi; Windows'ta Unicode, macOS'ta yalnız ASCII denetlenir);
   - ortak ata yok (ilgisiz geçmiş) → `sync.unrelated_histories`, merge'e hiç başlanmaz (ilk kurulum `enable`'ın işi);
   - normal → merge. **Çakışan her dosya için:** yerel sürüm yerinde kalır; uzak sürüm
     `<ad> (conflict YYYY-MM-DD HHMM).<uzantı>` olarak yanına yazılır; silme/değiştirme çakışmasında
     **değiştirilmiş sürüm** korunur. Hepsi sahnelenir, iki ebeveynli merge commit atılır.
     - Ad: `<ad>` = uzantısız dosya adı, damga **yerel saat**, aynı klasörde. Uzantısız dosyada nokta yok
       (`yapilacaklar (conflict 2026-10-28 1430)`). Ad doluysa uzantıdan önce ` 2`, ` 3` …:
       `not (conflict 2026-10-28 1430) 2.md`. Kopya yalnız yoksa oluşturulur; var olan dosyanın üzerine yazılmaz.
       Tam ad 255 bayta sığmıyorsa `<ad>` kısaltılır (UTF-8 karakteri bölünmez); 32 bayttan uzun "uzantı", adın parçası sayılır.
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
   (fazlalık, kayıp değil). `git_merge` kendi hatasında durumunu temizler; ondan sonraki bir hata (kopya
   yazılamadı, index'e eklenemedi …) merge'ü **geri alır** (karar 9, `abort_merge`), depo MERGE durumunda kalmaz.
   Push yarışı: ret → fetch → yeniden merge → push (bir kez); testte `sync_now(SyncTestHooks)` dikiş noktasıyla
   (`before_push`; kopya yazımına hata enjeksiyonu `after_conflict_copy`).
8. **Kurulum ve kimlik (T26):**
   - `enable` senkron kilidi altında koşar (arka plan senkronu yarım depoda commit atmasın). `enable` kodu
     `sync_service.cpp`'de (plandaki `enable.cpp` değil): commit/fetch/integrate/push yardımcılarını paylaşır.
   - Adres sınırda bir kez doğrulanır (`validate_remote`), `enable` ve her `sync_now` ağa çıkmadan önce:
     ssh dışında URL'de kullanıcı bilgisi → `sync.remote_has_credentials` (adres hata/log'a hiç girmez);
     boş adres, boş ya da `-` ile başlayan host (ssh seçenek enjeksiyonu) ve **var olmayan yerel klasör** →
     `sync.remote_invalid`. Sonuncusu gerekli: Windows'ta libgit2 var olmayan `C:/…` yolunu scp biçimi sayıp
     ön kontrolsüz sistem `ssh`'ını başlatıyor (host `C`).
   - `http://` token almaz (Auth::None). HTTPS token'ı ağa çıkmadan çözülür (yoksa ağsız `sync.token_missing`).
     Callback kimliği yalnız remote'un kendi host'una, https ile ve userpass olarak verir; ikinci çağrı,
     başka host ya da http → `sync.auth_failed`. Tek deneme kuralı ağ işlemi (ls, fetch, push) başınadır.
   - Kasa kaydı: package `noteapp`, servis `noteapp`, hesap = küçük harfli host (Windows Credential Manager'da
     `noteapp.noteapp/<host>`). `NotFound` dışındaki her kasa hatası `sync.keychain_unavailable`; düz dosya yok.
     Windows'ta kütüphane kullanılmaz: `CRED_PERSIST_ENTERPRISE` yazar, etki alanı profilinde token başka
     makinelere dolaşır (T36). `CredWriteW/ReadW/DeleteW`, `CRED_TYPE_GENERIC`, aynı hedef adı,
     `CRED_PERSIST_LOCAL_MACHINE`; yerel olmayan eski kayıt ilk okumada yerele çevrilir (olmazsa uyarı, token yine
     kullanılır). Linux/macOS'ta kütüphane kalır.
     `store_token` baştaki/sondaki boşluğu atar; boş token `sync.token_missing`.
   - SSH ön kontrolü `ssh-keygen -F <host>` (port 22 dışıysa `[host]:port`); `~/.ssh/config` takma adları
     (Host … HostName) görülmez. ssh-keygen bulduğu satırı (açık host anahtarı) stdout'a yazar: arka plan
     senkron süreci (T27) stdout/stderr'i devralmamalı.
   - "Uzak boş" = `git_remote_ls`'te `refs/` ile başlayan ref yok. Uzakta geçmiş var ama `sync.branch` dalı
     yoksa `sync.remote_branch_missing` (both_non_empty'den önce). Klon = init + origin + fetch + doğmamış dala
     hızlı ileri (döngü 4'ün `integrate`'i); gizli girdili ya da var olmayan klasörde de çalışır.
   - Hata olursa yalnız bu çağrının **oluşturduğu** `.git` (salt-okunur bitleri temizlenerek) ve yazdığı
     `.gitignore` silinir; notlara dokunulmaz, yarıda kalan bir klonun yazdığı dosyalar da silinmez.
   - `replace_origin`: origin silinip yeniden oluşturulur; eski `origin/*` takip dalları da gider (yoksa boş yeni
     remote'a push "güncel" sanılıp yapılmazdı). `remote_mismatch`'teki `current` adresinden kimlik ayıklanır.
   - Var olan `.gitignore`'a yalnız eksik satırlar eklenir (CRLF korunur); okunamıyorsa üzerine yazılmaz.
   - Klon, izlenmeyen bir `.gitignore`'a takılmaz (T36): dosya `.gitignore.noteapp.tmp`'ye alınır (silinmez,
     süreç öldürülse de satırlar kaybolmaz), checkout sonrası satırları uzaktan gelene eklenir; checkout
     başarısızsa bayt bayt geri gelir. Çakışan diğer gizli dosyalar klonu temizce durdurur, üzerlerine yazılmaz.
   - **Bağlar düz dosyadır (T36):** libgit2 Linux/macOS'ta init'te `core.symlinks`'i açık bırakır; uzaktaki
     `.gitignore -> ../.ssh/id_ed25519` gerçek bağ olarak çıkar, `.gitignore` tamamlanırken hedef okunup commit'e
     girerdi. Notlar deposunun yerel config'ine init'ten hemen sonra ve depo kurulumda/her senkronda açılırken
     `core.symlinks=false` yazılır (zaten kapalıysa dokunulmaz): bağ, içinde hedef yolu olan düz dosya olarak
     çıkar (SPEC-core "notlar bağın ardında yaşamaz"). Ayrıca `.gitignore` bağ ya da düz olmayan girdiyse hiç
     okunmaz → `sync.gitignore_not_regular` (POSIX'te `O_NOFOLLOW`).
9. **Arka plan senkronu ve AutoSync (T27):**
   - `note sync --background` = ayrık süreç (`core::spawn_detached`: stdio boş aygıt, yalnız NUL tutamacı
     devredilir; Windows `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW`, POSIX `setsid`).
     Hiç çıktı yazmaz; sonuç log + `sync-status.toml`, çıkış 0/4. `sync.enabled` kapalıysa hiçbir şey yapmaz.
     `sync.busy`'de 500 ms aralıkla `tuning.sync_timeout_seconds / 3`'e kadar yeniden dener: ayrık sürecin "sonra
     deneyen" çağıranı yok, yoksa arka arkaya iki hızlı eklemenin ikincisi senkronlanmadan kalırdı. Üçte bir
     sınırı: AutoSync aynı bütçe sonunda süreci öldürür; kilit tam o anda alınırsa senkron yarıda kesilirdi.
   - CLI'de notu değiştiren her komut (hızlı ekleme/editör akışı, `rm`, `mv`, `edit` — mtime değiştiyse)
     `sync.enabled` ve `--no-sync` yokken `note sync --background`'u başlatır, beklemez. Kendi yolu
     `core::current_executable()` (Windows `GetModuleFileNameW`, Linux `/proc/self/exe`; diğerlerinde PATH'teki
     `note`). Ön plandaki `note sync` ise süreç içinde koşar: kullanıcı başında, asılı ssh'ı Ctrl-C ile keser.
   - `AutoSync(config, note_exe, on_status)`: tek işçi iş parçacığı; her senkron `note_exe sync --background`
     ayrık sürecidir, 50 ms yoklamayla beklenir; `tuning.sync_timeout_seconds` (120) aşılırsa sonlandırılır ve
     durum dosyasına `sync.timeout` {seconds} yazılır (`SyncService::record_failure`); `note` başlatılamazsa o
     hata yazılır. Callback `(SyncStatus, running)`: başlarken `true` (önceki durum, ⟳), bitince `false` (yeni
     durum); **işçi iş parçacığından** çağrılır, ön yüz kendi UI iş parçacığına aktarır. Debounce: her
     `notify_changed` zamanı "son bildirim + idle"a iter, sıradaki "hemen" isteğini geciktirmez. `stop(t)`: yalnız
     son senkron başladıktan sonra değişiklik varsa son bir senkron; toplam `t`'yi aşarsa süren süreç
     sonlandırılır; yıkıcı = `stop(0)`. Sonlandırma tüm süreç ağacını öldürür (`core::terminate`: Windows'ta
     iş nesnesi, POSIX'te süreç grubu), böylece asılı kalan ssh torunu yetim kalmaz (T27b).
   - **Bayat kilit kurtarma:** `sync_now`/`enable` bayat `sync.lock`'u devraldıysa (`SyncLock::took_over`)
     `.git/*.lock` ve `.git/refs/**/*.lock` **yaşına bakılmadan** silinir. Yalnız "bayat yaştan eski" libgit2
     kilitleri silinseydi zaman aşımıyla öldürülen sürecin saniyelik `index.lock`'u hiç temizlenmezdi: sonraki
     senkron artık kilit devralmaz, her senkron "index is locked" ile düşerdi. Kilit bizde, noteapp'ten yazan
     başka süreç yok; o an elle çalışan bir git komutunun kilidi silinebilir (milisaniyelik pencere, bilinen
     sınır). Depo MERGE durumundaysa — index'te çakışma olsun olmasın — merge geri alınır, döngü onu yeniden
     yapar. Kilit devralınmadıysa hiçbir libgit2 kilidine ya da merge durumuna dokunulmaz (elle başlatılmış
     merge'ler korunur → `sync.repo_busy_state`).
   - **Merge'ü geri alma (T36, `abort_merge`):** merge'e başlamadan önce tüm yerel değişiklikler commit'lendi;
     HEAD + uzak dal her baytı tutar, geri almak hiçbir şey kaybettirmez. Eski kural ("çakışma varsa bırak")
     depoyu kalıcı `sync.repo_busy_state`'te bırakıyordu. Adımlar, asla HARD reset ya da FORCE checkout olmadan:
     diskteki merge index'i okunur ve çakışma kayıtları çıkarılır; HEAD'e `GIT_CHECKOUT_SAFE |
     ALLOW_CONFLICTS` checkout, taban = o index: içeriği hâlâ merge'ün yazdığı gibi olan dosyalar HEAD'e döner,
     kullanıcının arada düzenlediği dosya olduğu gibi kalır (sonraki senkron commit'ler); index `git_reset`
     MIXED ile HEAD'e (merge durum dosyalarını da siler); bu denemenin yazdığı kopyalar, içerikleri hâlâ
     yazıldığı gibiyse silinir. Merge commit'i atıldıktan sonraki bir hatada kopyalar HEAD'dedir, silinmez.
     Bilinen sınır: öldürülmüş denemenin kopyaları izlenemez; kalır ve fazlalık olarak commit'lenir.
   - `ssh-keygen -F` ön kontrolü çıktısı atılarak çalışır (`run_and_wait(…, discard_output = true)`).

## Kapsam dışı
Rebase, seçmeli (dosya bazlı) senkron, GitHub dışı sağlayıcılara özel destek (genel git remote zaten çalışır),
LFS, şifreli repo, Git Credential Manager entegrasyonu.

## Değişiklik 2026-11-02 (G5)
Bu bölüm "Senkron döngüsü" adım 2 ve 4'ü, kabul 3-4'ü ve aşağıdaki "Değişiklik 2026-11-01"in dosya ayrıntılarını geçersiz kılar. Birleştirilen şey dosya değil işlemdir ([SPEC-graph.md](SPEC-graph.md)). Kilit, kimlik doğrulama, `enable`, geri alma, `core.symlinks=false` ve uzak yol denetimi aynen geçerlidir.
- **Döngü:** kilit → geçiş kapısı (`sync.migration_required`, git'e dokunulmaz) → `.gitattributes` ve `.gitignore` → **kendi günlüğünü al** → ver → commit → fetch → birleştir → tüm günlükleri al ve çöz → ver → commit → push → durum. Ret: fetch'ten bir kez daha. Doğmamış dalda commit'ten önce fetch: yoksa ilgisiz kök olurdu.
- **Commit** yalnız `.gitignore`, `.gitattributes` (`*.oplog -text -merge`), `graph/<32 hex>.oplog`. Depodaki diğer dosyalara, eski notlar dahil, dokunulmaz. `graph/*.aside-*` yok sayılır.
- **Gövde çakışması:** `ingest_note_logs` her ingest'te tüm notlara bakar, yalnız `MergeReport.conflicts`'e değil: rapor sınıra takılsa da süreç iki adım arasında ölse de kaybeden sürüm kaybolmaz. Kaybeden head `derive_id("conflict", not, kaybeden dot)` kimlikli kopya nota çıkar, gövde kazananda birleşir. Ad UTC damgalıdır (`<ad> (conflict YYYY-MM-DD HHMM).<uzantı>`), iki cihaz aynı düğüme varır. Yerel yazma da önce bunu yapar.
- **Satır birleştirme:** Eşzamanlı gövdeler `git_merge_file` (libgit2 1.9) ile üç yönlü birleştirilir. Taban ortak nedensel atadır: her gövde yazması gördüğü head'leri `body_parents`'ta tutar, geçmiş `ReadTxn::properties_at` ile `ops`'tan okunur (motor nottan habersizdir). Girdi sırası (cihaz, seq) sabittir: iki cihaz aynı anda çözerse aynı baytı yazar, aynı değerli head çakışma değildir. CRLF ve BOM aynen kalır. Örtüşme, taban yok (4096 yazma ya da 256 MiB bütçe), ikili ya da geçersiz UTF-8: kopya kuralı. Yerel kayıt, önceki senkronun çözemediği head görürse birleştirmez, kopyaya çıkarır (libgit2'siz yol, nadir).
- **Silme:** Gövdeyi görmemiş silme etkisizdir, düzenleme kazanır. Görülmeyen head aynı baytlarsa düzenleme sayılmaz.
- **Görünürlük:** `sync-status.toml`: `pushed_seq`, `log_issues` (`stalled`, `bad_frame`, `partial_tail`, `omitted`, `set_aside`), `heads`. TUI göstergesi, GUI etiketi, `note sync` ve `note sync status` bunları gösterir.
- **`note_status`:** Git status geçişi kalktı. Ölçüt: not başına bu cihazın yazma numarası ve `pushed_seq`. Çakışma kopyası adı → `conflict_copy`; bu cihazda oluşup gönderilmemiş → `new`; gönderilmemiş yazma → `modified`; aksi → `synced`; sync kapalı ya da `.git` yok → `sync_off`. 5.050 notta 0,45 ms.
- **Sınır:** Günlük sıkıştırma yok. Her kayıt `ops`'a ve git'e gövdeyle girer; silinen içerik eski günlük satırlarında ve git geçmişinde kalır (SPEC-graph).
- **`graph` dosya olursa:** Uzak commit `graph` klasörünü dosyaya çevirdiyse `sync.log_dir_not_regular`: notlar veritabanında güvende, kullanıcı düzeltir.
- **Terminal:** Uzak sunucunun ret metni ve durum dosyası `format_error`'dan sonra temizlenir (C0, C1, geçersiz UTF-8).
- **`committed` / `pulled`:** Gönderilmemiş yazması olan not sayısı / uzaktan gelip canlı ya da çöp listesinde değişen not sayısı. Çöpe atma 1, kalıcı silme 0.
- **`enable`:** `graph.db`'deki notlar `notes_dir`'de durmadığından, boş `notes_dir`'li cihaz notları varken de klonlar: notlar birleşir. `sync.both_non_empty` yalnız görünür dosya varsa geçerli. Geçiş bekliyorsa kurulur ama düz dosya commit'lenmez, senkron geçişe kadar reddeder.
- Test eşlemesi: [tasks/g5-test-map.md](tasks/g5-test-map.md).

## Değişiklik 2026-11-01
- **ADDED** Çöp kutusu cihazlar arası (T55a): `.trash/` sıradan bir yol olarak commit'lenir. Yönetilen `.gitignore` (`.noteapp/`, `.*.tmp`) ve
  `sync.remote_invalid_paths` denetimi onu dışlamaz; kod değişikliği gerekmedi, gerçek `enable` + iki klonla sınandı (`tests/sync/trash_sync_test.cpp`):
  A'daki atma B'nin çöpünde bayt bayt görünür ve canlı listesinden düşer; B'deki geri yükleme, A'daki `purge` ve `empty_trash` karşıya yayılır.
  - Atma = silme + `.trash`'e ekleme (yeniden adlandırma tespiti kapalı). A atarken B düzenlerse silme/değiştirme çakışmasıdır, karar 7 geçerli:
    **düzenleme yerinde kalır**, hangi taraf önce senkronlarsa; A'nın eski sürümü de çöpte durur.
  - İki taraf aynı notun farklı sürümünü atarsa `.trash/<yol>` ekleme/eklemedir: yerel sürüm yerinde, uzak sürüm
    `.trash/<ad> (conflict …).<uzantı>`. Bu kopya `SyncResult.conflict_copies`'te yoktur (gizli yol `NotePath` değil), `list_trash`'te vardır. Aynı sürüm çakışma değildir.
- **ADDED** Not başına durum (T55a), `sync/note_status.hpp`: `fill_sync_status(config, span<NoteMeta>)`, `is_conflict_copy_name(path)`.
  - Çağrı başına **tek** libgit2 status listesi; taban (`baseline`) `refs/remotes/origin/<dal>` ağacıdır, böylece commit'lenmiş ama gönderilmemiş
    değişiklik de ikinci bir karşılaştırma olmadan görünür. Ağa çıkmaz, kilit almaz, index'i yazmaz; tazelik son fetch kadardır.
    UI iş parçacığından çağrılmaz (5.050 notta 18 ms, ilk çağrı 24 ms; Release, konteyner, `tests/perf/query_perf_test.cpp`, sınır 500 ms).
  - Kural sırası: sync kapalı ya da `notes_dir` depo değil → `sync_off`; ad çakışma kopyası (`<ad> (conflict YYYY-MM-DD HHMM)[ N][.uzantı]`) → `conflict_copy`,
    gönderilmiş olsa da, kullanıcı çözene dek; uzak dal hiç alınmamış → `new`; `.gitignore`'a takılan → `sync_off`; uzakta yok → `new`; uzaktakinden farklı → `modified`; aynı → `synced`.
  - Sınır: fetch edilip henüz birleştirilmemiş uzak değişiklik de `modified` görünür (not uzakla aynı değil). Kopya adını elle veren not da `conflict_copy` sayılır.
