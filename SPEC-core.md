# Spec: core

> Modül: `core` · Bağımlılık: — · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Tüm ön yüzlerin (cli/tui/gui) ve `search`/`sync`'in üzerine oturduğu ortak çekirdek: not deposu, yol
güvenliği, atomik yazma, config, i18n, editör başlatma, platform dizinleri, log. Hiçbir GUI araç takımına bağımlı değildir.

## Arayüz (taslak)

```cpp
namespace noteapp::core {
struct Error { enum class Code { InvalidPath, NotFound, AlreadyExists, ModifiedExternally, NotEmpty,
                                 Io, ConfigInvalid, EditorNotFound, EditorFailed };
               Code code; std::string message_key; std::vector<std::pair<std::string, std::string>> args; };

class NotePath;                       // doğrulanmış göreli yol (SPEC.md §6)
struct NoteInfo { NotePath path; std::filesystem::file_time_type mtime; std::uintmax_t size; };
bool looks_binary(std::string_view bytes);   // ilk 8 KB'ta NUL; dosyayı zaten okuyan yer çağırır
struct NewNote  { std::string folder; std::string name; std::string type; std::string content; };
struct NoteContent { std::string text; std::filesystem::file_time_type mtime; };   // mtime okuma anındaki

class NoteStore {                     // kök: config.notes_dir
    std::vector<NoteInfo> list() const;                       // tüm notlar, mtime azalan
    std::vector<std::string> folders() const;                 // tüm klasörler (göreli)
    std::expected<NoteContent, Error> read(const NotePath&) const;
    std::expected<path, Error> absolute_path(const NotePath&) const;  // read() ile aynı kapı; dosyayı kendisi açanlar için
    std::expected<NotePath, Error> create(const NewNote&, system_clock::time_point now = now());  // ad boşsa içerikten slug; tip boş, `.`/`/`/`\` içeren ya da ad kuralının reddettiği tip → InvalidPath `invalid_type`
    std::expected<file_time_type, Error> write(const NotePath&, std::string_view content, file_time_type expected_mtime);  // yeni mtime döner
    std::expected<NotePath, Error> move(const NotePath& from, const NotePath& to);  // uzantı değişimi = tip değişimi
    std::expected<void, Error> remove(const NotePath&);
    std::expected<void, Error> create_folder(std::string_view folder);
    std::expected<void, Error> rename_folder(std::string_view from, std::string_view to);
    std::expected<void, Error> remove_folder(std::string_view folder, bool force); // dolu + !force → NotEmpty
};

std::string slugify(std::string_view first_line);             // saf fonksiyon
struct LoadedConfig { Config config; std::vector<std::string> warnings; };
LoadedConfig load_config();                                   // varsayılan + config_file(); çökmez, uyarı döner
LoadedConfig parse_config(std::string_view user_toml);        // saf çekirdek (dosya erişimi yok)
std::expected<void, Error> save_config(const Config&);        // [tuning]'de yalnız varsayılandan farklılar, atomik
std::string tr(std::string_view key, const Args& args = {});   // i18n
std::expected<int, Error> run_editor(const Config&, const std::filesystem::path& file);
}
```

## Davranış

### Yol güvenliği (`NotePath::parse`)
Reddedilir: boş yol; mutlak yol / sürücü harfi / UNC; `.` veya `..` bileşeni; `.` ile başlayan bileşen
(gizli adlar uygulamaya ayrılmış); `< > : " \ | ? *` ve kontrol karakterleri (ayırıcı yalnız `/`);
Windows ayrılmış adları (`CON PRN AUX NUL COM0-9 LPT0-9` ve üst simgeli `COM¹ COM² COM³ LPT¹ LPT² LPT³`,
uzantılı hâlleri dahil, büyük/küçük harf duyarsız);
sonda boşluk/nokta; 255 bayttan uzun bileşen. Ek olarak `NoteStore` her erişimde (okuma, yazma, oluşturma,
taşıma, silme, klasör işlemleri) iki kural uygular:
- **Not yolu hiçbir bileşeninde bağ içeremez; Windows junction'ları dahil.** Kökün altındaki var olan her
  bileşen (ara klasörler ve son öğe) `core::stat_file` ile denetlenir; bağ → `InvalidPath` (`reason = link`).
  Kural her platformda aynıdır; hedefi kök içinde olan bağ da reddedilir. Kökün (`notes_dir`) kendisi bağ
  olabilir. Bileşen denetlenemezse (yok/üstü dosya dışındaki hata) güvenli tarafta kalınır: `Io`.
  Neden: MinGW libstdc++ junction'ı bağ saymaz (`is_symlink` false, `weakly_canonical` çözmez,
  `recursive_directory_iterator`/`remove_all` içine iner) ve junction yetki istemeden kurulur
  (`mklink /J`); `link/x.md` kök dışına okuma/yazma/taşıma, klasör silme kök dışını silme demekti (T16d).
- Kanonik yol `notes_dir` içinde kalmalı (`reason = outside_root`).
- **Windows 8.3 kısa adları (T36):** `GIT~1/config`, `GITIGN~1`, `NOTEAP~1` NotePath'ten geçer ama `.git`, `.gitignore`,
  `.noteapp`'e çözülür. Var olan ve `~` içeren her bileşenin diskteki uzun adı (`FindFirstFileExW`'nin `cFileName`'i)
  gizli ad kuralından yeniden geçer → `InvalidPath` (`reason = hidden`); denetlenemezse `Io`. Otomatik 8.3 adları hep
  `~` taşır; yöneticinin `SetFileShortNameW` ile verdiği tildesiz kısa ad kapsam dışıdır.

### Not oluşturma ve adlandırma
- **Slug:** ilk dolu satır → baştaki `#`, `-`, `*`, `>` ve boşluklar atılır → Türkçe harfler ASCII'ye
  (ç→c ğ→g ı→i İ→i ö→o ş→s ü→u) → küçük harf → harf/rakam dışı her dizi `-` → baş/son `-` kırpılır →
  en çok `slug_max_length` (defaults.toml, 60) karakter, tire sınırında kesilir. Boş kalırsa `note-YYYYMMDD-HHMMSS`.
- **Çakışma:** ad varsa `-2`, `-3`… eklenir; mevcut dosyanın üzerine asla yazılmaz.
- **Tip = uzantı.** `type` başında nokta olmadan verilir (`md`, `java`); geçerlilik `NotePath` kurallarıyla.
- Klasör yoksa oluşturulur. Yeni not içeriği LF satır sonuyla yazılır; mevcut notların satır sonlarına dokunulmaz.

### Yazma
- **Atomik:** aynı dizinde `.<ad>.<rastgele>.tmp` → fsync (`FlushFileBuffers` / `fsync`) → `std::filesystem::rename`
  (MinGW libstdc++'ta da var olan hedefi değiştirdiği doğrulandı, 2026-10-28). Yeni not `create`'te geçici dosyasız,
  dışlayıcı oluşturmayla (`CREATE_NEW` / `O_EXCL`) yazılır; yarış durumunda bile var olan dosya ezilmez.
  Hata olursa geçici dosya silinir, orijinal dosya bozulmamış kalır.
- **İzinler korunur (T36):** var olan dosya değiştirilirken POSIX'te hedefin mod bitleri (`fchmod`, `& 07777`) ve
  yetki varsa sahibi (`fchown`, en iyi çaba) geçici dosyaya kopyalanır; Windows'ta değiştirme `ReplaceFileW`
  (`REPLACEFILE_IGNORE_MERGE_ERRORS`) ile yapılır, hedefin ACL'si ve öznitelikleri (gizli vb.) kalır. `ReplaceFileW`
  başarısızsa (desteklenmeyen birim, paylaşım hatası) `rename`'e düşülür; hedefi kaldırıp yenisini koyamadıysa
  geçici dosya silinmez (yeni içeriğin tek kopyası). Neden: düz `rename` geçici dosyanın `0666 & ~umask`'ını
  getiriyordu; `0600` not tek kayıtta `0644` olur, senkron da her kayıtta mod değişikliği commit'lerdi.
- **Dışarıdan değişiklik:** `write` diskteki mtime `expected_mtime`'dan farklıysa yazmaz → `ModifiedExternally`.
- **mtime kaynağı:** karşılaştırılan her mtime (`list`, `read`, `write`) tek kaynaktan, `core::stat_file`'dan gelir
  (bağı izlemez): Windows'ta FILETIME (100 ns), POSIX'te `st_mtim` / `st_mtimespec` (ns). Neden: MinGW libstdc++'ta
  `std::filesystem::last_write_time` `_wstat64` üzerinden saniyeye keser; okumayla aynı saniyedeki aynı boyutlu dış
  düzenleme yakalanmaz, üzerine yazılırdı (T16c, 2026-10-28). Kalan pencere dosya sisteminin saat tıkıdır (~1–16 ms).
- UTF-8 BOM okunurken korunur, yazılırken olduğu gibi geri yazılır.

### Listeleme
- `notes_dir` altında özyinelemeli; nokta ile başlayan dosya/dizinler atlanır.
- Sembolik bağlar listelenmez, içlerine inilmez. Windows'ta bağ = ad vekili reparse point (sembolik bağ, junction);
  OneDrive yer tutucusu gibi diğer reparse point'ler normal dosya/dizindir.
- Tarama `core::scan_files`: Windows'ta `FindFirstFileExW` (`FindExInfoBasic`, `FIND_FIRST_EX_LARGE_FETCH`) — ad,
  boyut ve mtime dizin girdisinden gelir, dosya başına stat yok; mtime `stat_file` ile aynı FILETIME dönüşümünden
  geçer (Yazma › mtime kaynağı). POSIX'te `recursive_directory_iterator` + `stat_file`. Neden: kurumsal Windows'ta
  dosya başına stat 5.000 notta ~170–290 ms tutuyordu; tarama ~15–45 ms (Release, T16c).
- Her normal dosya bir nottur; ikili dosya da listede görünür. `list()` içerik okumaz: ikili tespiti (`looks_binary`,
  ilk 8 KB'ta NUL baytı) dosyayı zaten okuyan yerde yapılır — TUI önizlemesi, `ContentIndex::refresh`, GUI editörü,
  CLI `show`/`export`; ikili notta önizleme/arama/düzenleme/dışa aktarma yapılmaz. Neden: 5.000 dosyayı yalnız bu
  bayrak için açmak soğuk `list()`'i 295–917 ms'ye çıkarıyordu (T35).

### Klasörler
- `create_folder` boş klasöre gizli `.keep` dosyası koyar (git boş dizin taşımadığı için senkron korunur).
- `remove_folder`: içinde not varsa `force` olmadan `NotEmpty` (args: not sayısı); `force` ile kalıcı silinir.
  Silmeden önce alt ağaç bağ izlenmeden taranır; içinde bağ varsa (`force` olsa da) hiçbir şey silinmez →
  `InvalidPath` (`reason = contains_link`). Silme `fs::remove_all` değil, bağ izlemeyen alttan üste gezintidir.
- `folders()` bağları listelemez, içlerine inmez.

### Silme
`remove` dosyayı kalıcı siler (senkron açıksa geçmişte kalır; çöp kutusu için `trash`, bkz. Değişiklik 2026-11-01). Onay sorumluluğu ön yüzdedir.

### Config (`config.toml`)
```toml
language = "auto"        # auto | tr | en   (auto: Windows UI dili / LC_ALL→LC_MESSAGES→LANG)
notes_dir = "~/notes"
editor = ""              # boş: $VISUAL → $EDITOR → platform varsayılanı
default_type = "md"
default_folder = ""      # hızlı notların klasörü ("" = kök)
types = ["md", "txt", "csv", "cpp", "java", "py", "json", "sh"]   # seçim listesi; elle başka uzantı da yazılabilir
icons = "nerd"           # nerd | off; `auto` nerd okunur; davranış SPEC-tui "Dosya ikonları"

[sync]
enabled = false
remote = ""              # https://github.com/<u>/notes.git → token | git@github.com:<u>/notes.git → SSH
branch = "main"
idle_seconds = 30
author_name = ""         # boş: git global config → OS kullanıcı adı
author_email = ""

[update]                 # TUI ve GUI ayar ekranında da; SPEC-tui / SPEC-gui "Güncelleme"
check_interval = "24h"   # açılış denetiminin en kısa aralığı; 0 = kapalı, 0 < x < 1h reddedilir

[tui]
pdf_image = "auto"       # auto | sixel | blocks; PDF sayfa görünümünün çizimi, SPEC-tui "PDF sayfa görüntüsü (TPI)"

[lsp]                    # yalnız TUI; davranış ve varsayılan sunucular SPEC-tui "Dil sunucusu (LSP)"
enabled = true
[lsp.servers]            # LSP languageId → kabuksuz argv ya da argv listesi; kullanıcının girdileri tek tek üstüne biner
python = ["pyright-langserver", "--stdio"]   # tam liste: resources/defaults.toml
html = [["vscode-html-language-server", "--stdio"], ["htmx-lsp"]]   # dil başına en çok 4 sunucu
[lsp.settings.rust]      # yalnız kullanıcı dosyasında; JSON'a çevrilir, sunucunun gömülü ayarına anahtar anahtar biner
rust-analyzer.cargo.buildScripts.enable = true

[tuning]                # arayüzde görünmeyen ayarlar; yalnız defaults.toml'da (kullanıcı isterse geçersiz kılar)
slug_max_length = 60
min_confidence = 0.5     # 0..1
max_scan_bytes = 65536
max_note_bytes = 10485760   # 0 = sınırsız; "64MiB" gibi birimli yazılabilir (core::Limit)
rescan_seconds = 5
autosave_ms = 1000
exit_timeout_seconds = 10
log_max_bytes = 1048576   # log.txt bu boyutu aşınca log.1.txt'ye döner
sync_stale_lock_minutes = 10   # okunamayan sync.lock bundan eskiyse bayat; sahibi canlı kilit devralınmaz (SPEC-sync)
sync_timeout_seconds = 120     # AutoSync bir `note sync --background` sürecini en çok bu kadar bekler (SPEC-sync karar 3)
graph_batch_ops = 30000        # bir graf yazma işleminin işlem bütçesi; aşan iş bölünür, reddedilmez
completion_min_prefix_bytes = 2   # TUI gölge önerisinin beklediği en kısa önek (alt sınır, `0` geçersiz)
notice_ms = 4000              # işlem bildiriminin süresi ("4s" yazılabilir); hata kalıcı; 0 = kendiliğinden kapanmaz
tui_list_share = 0.33   # TUI ilk açılışta sol listenin pencereye oranı (0..1); seçilen genişlik geçer
search_relations = true # aramalar etiket, ilişki adı ve bağlı notu da tarar (SPEC-search "İlişki araması"); false kapatır

[limits.search]          # kapasite sınırları; aşağıdaki "Sınırlar"
content_results = 1000   # içerik aramasında (TUI, GUI) listelenen en çok not; 0 = hepsi
```
- Varsayılanlar `resources/defaults.toml`'dan (gömülü) gelir; dosya yoksa varsayılanlarla çalışılır, ilk
  kayıtta oluşturulur. Bilinmeyen anahtar → uyarı logu, yok sayılır. Geçersiz değer (yanlış tür, language ∉ {auto,tr,en}, boş/noktalı tip, idle_seconds ≤ 0, boş branch, ≤ 0 tuning — `max_note_bytes` hariç, o bir sınırdır) → o anahtar için varsayılan + uyarı; TOML sözdizimi hatası → tüm varsayılanlar + satır/sütunlu uyarı. `save_config` `[tuning]`'e yalnız
  gömülü varsayılandan farklı anahtarları yazar: elle verilmiş ince ayar uygulamanın kaydında (ör. `note config
  set`) kaybolmaz, dokunulmamış varsayılan dosyaya gömülmez (T36).
- `[lsp.servers]` girdisi tek tek doğrulanır: kimlik `a-z0-9_-`, en çok 32 bayt; argv en çok 64 boş olmayan, NUL'suz,
  en çok 4096 baytlık dizge; değer tek argv ya da en çok 4 boş olmayan argv'nin listesi (`kMaxServersPerLanguage`);
  dosya başına en çok 32 dil. Girdi varsayılanın üstüne biner; boş dizi (`[]`) o dili kapatır. Bozuk girdi `config.invalid_value` (`must_be_language_id`,
  `must_be_server_command`, `too_many_servers`) ve atlanır, geçerliler kalır.
- `[lsp.settings.<dil>]` tablosu `core::Json` nesnesine çevrilir (`LspConfig::settings`); dil başına tablonun tamamı
  önceki katmanın yerine geçer. Tablo olmayan değer, 16 düzeyden derin iç içelik, 4096'dan çok değer, tarih/saat ve
  nan/inf `must_be_settings_table` olur ve o dil atlanır. Birleştirme ve gömülü sertleştirme TUI'dedir
  (SPEC-tui "Dil sunucusu").
- `save_config` `[lsp]`'yi bellekten değil, dosyada o an nasılsa öyle (bozuk girdiler dahil) geri yazar; kökteki satır içi
  tablo (`lsp = { enabled = false }`) başlıklı tabloya çevrilir (yoksa `[sync]`'ten sonra `sync.lsp` okunurdu), tablo
  olmayan `lsp` değeri yazılmaz; dosyada yoksa yazmaz. Ayar kaydı sunucuları silmez, uygulama açıkken
  dosyadan kapatılan LSP'yi de geri açmaz. `note config list/set` bu bölümü göstermez, dosya elle düzenlenir.
- `~` ve ortam değişkenleri `notes_dir`'de genişletilir. Kaydetme atomiktir. Yorumlar korunmaz (bilinen sınır).
- Kimlik doğrulama yöntemi `remote` şemasından türetilir; ayrı anahtar yok.
- `sync.remote` http(s) URL'sinde kullanıcı bilgisi (`https://kullanıcı:token@host/…`, `://`dan sonraki ilk `/ ? #`'ten
  önce `@`; birden çok `@`'ta sonuncusuna kadar) kabul edilmez (T36):
  - `set_config_value` reddeder → `config.invalid_value`, `reason = remote_has_credentials`.
  - Dosyadan yüklemede kimlik soyulur, remote korunur, uyarı `config.remote_credentials_stripped` üretilir; sır bellekte
    kalmaz, sonraki `save_config` dosyadan da siler. Neden: elle yazılmış remote sessizce kaybolmasın; URL'deki kimlik
    zaten hiç kullanılmaz (token ortamdan ya da kasadan gelir, yoksa `sync.token_missing`).
  - URL'nin ham hâli hata ve uyarılara asla yazılmaz. SSH kullanıcı adı (`git@host:…`, `ssh://git@host/…`) etkilenmez.

### Sınırlar (`[limits]`, L0)
Kural (tasks/plan-limits.md): kullanıcının işlemi kaynak koddaki bir tavanla reddedilmez; kalan her kapasite sayısı
config'tedir.
- **Anahtar adı:** `limits.<modül>.<ad>`; dosyada `[limits.<modül>]` tablosunda `<ad> = …`. `<modül>` olguyu sahiplenen
  kaynak modülüdür (`app`, `core`, `search`, `graph`, `sync`, `tui`, `gui`, `lsp`, `query`); `<ad>` snake_case ve ne
  saydığını söyler (`max_tabs`, `content_results`, `view_max_duration`). Aynı olgu tek anahtardadır; iki arayüz aynı
  değeri kullanıyorsa ikisi de o anahtarı okur (ör. `limits.search.content_results`: TUI ve GUI).
- **Değer:** `0 = sınırsız`. Negatif olmayan TOML tam sayısı ya da dizge: adet `"1000"`; boyut `"512KiB"`, `"64MiB"`,
  `"10MB"` (B, KB, MB, GB, TB = 10³; KiB, MiB, GiB, TiB = 2¹⁰), düz sayı bayt; süre `"250ms"`, `"5s"`, `"2min"`,
  `"1h"`, düz sayı ms. Ek büyük/küçük harf duyarlı; işaret, boşluk, kesir yok; üst değer int64. Birim anahtarın
  türündedir (`core::LimitUnit`): adet anahtarı birimli değeri reddeder.
- **Bellekte:** `Config::limits` (`core::Limits`, modül başına bir struct), her değer `core::Limit{value, text}`:
  `value` taban birimde, `text` dosyadaki yazım. Modüller 0 ile karşılaştırmaz: `allows(n)` (sınırsızda hep true,
  sınır dahil), `cap()` (sınırsızda `SIZE_MAX`; kesit/sayaç üst değeri), `unlimited()`. Config bir kez yüklenir ve
  modüle değer olarak geçer; global yok.
- **Doğrulama:** geçersiz değer (negatif, kesir, bool, bozuk ya da yanlış birimli dizge) → `config.invalid_value`,
  `reason` = `must_be_limit_count` | `must_be_limit_size` | `must_be_limit_duration`, varsayılan kalır. Bilinmeyen
  modül/anahtar → `config.unknown_key`. `set_config_value` aynı kuralı kullanır (rakamlar tam sayı, birimli metin
  dizge olarak denenir).
- **CLI ve arayüz:** `note config list/get/set` `limits.*` anahtarlarını gösterir ve değiştirir (`core::limit_entries`;
  değer dosyadaki yazımdır). Ayar ekranları `config_entries`'i listeler, `[limits]` orada yoktur: TUI ve GUI'de eşit
  biçimde yalnız dosya/CLI.
- **Kayıt:** `save_config` `[limits]`'i dosyada o an nasılsa öyle (birimli yazım, bilinmeyen ve bozuk girdiler dahil)
  yazar; yalnız bellekteki değeri dosyanın verdiğinden farklı anahtar üzerine yazılır (rakamsa tam sayı, değilse
  dizge). Dokunulmamış varsayılan dosyaya yazılmaz. Yorumlar korunmaz. Bilinen sınır (`[tuning]` ile aynı): uygulama
  açıkken dosyada değiştirilen sınır, aynı oturumdaki bir ayar ekranı kaydında açılıştaki değere döner.
- **Anahtar ekleme (modül görevi):** (1) `resources/defaults.toml`'da `[limits.<modül>]` altına bir satır (varsayılan ve
  ne saydığı); (2) `core/limits.hpp`'de modül struct'ına bir `Limit` alanı (modül yoksa struct ve `Limits` alanı);
  (3) `config.cpp` `limit_keys()`'e bir satır (anahtar, birim, alan; aynı modülün satırları art arda; tablonun boyu
  `std::to_array` ile satırlardan çıkar, elle sayı yoktur); (4) bir test
  (varsayılan, `0`, geçersiz → uyarı). Okuyan kod `config.limits.<modül>.<ad>`'ı kullanır, kopya sabit tutmaz.
- **Graf ve sorgu (L4):** `[limits.graph]` motorun sınırları (çerçeve, cihaz, ingest, rapor, kenara alınan günlük,
  `note graph import` satırının baytı ve JSON değeri; ayrıca not deposunun ilişki adı baytı `relation_name_bytes`, SPEC-graph "Adlı ilişkiler"); `[limits.query]` kullanıcının kendi sorgusu (`note query`), `[limits.view]` görünüm
  (`view:`, `note view run`, kayıt denetimi) — iki profil aynı 12 anahtarı taşır, varsayılanları aynıdır. Motor
  config okumaz: `core::graph_limits_of` / `core::query_limits_of` (`core/graph_limits.hpp`) çevirir, `0` →
  `SIZE_MAX` (süre `milliseconds::max()`); `NoteStore::for_config` ve CLI motoru bu değerlerle açar. Bir test
  `defaults.toml` ile motorun kendi varsayılanlarının aynı olduğunu denetler (tek olgu). Hangi değerin her cihazda
  aynı olması gerektiği ve protokolde sabit kalanlar: SPEC-graph "Sınırlar". `[tuning] graph_batch_ops`'un sabit
  tavanı (32.768) kalktı; L4b'den beri çerçeve sınırına da kırpılmaz (işlem günlükte kayıtlara bölünür). İşlem başına bayt payı
  etkin kayıt sınırının yarısıdır (`Graph::max_frame_bytes`: `frame_bytes` ile SQLite'ın tek değer sınırının küçüğü).

- **Çekirdek, arama, senkron (L3):** `limits.core.folder_path_bytes` (4096 bayt; klasör yolu, derinlik sınırı yok;
  güvenilmez girdi koruması, SPEC-graph "Sınırlar"), `limits.search.global_results` (200; genel arama sonucu),
  `limits.search.content_hits` (3; içerik sonucunda not başına kesitli satır), `limits.search.fuzzy_candidate_chars`
  (1024 kod noktası; vurgulu fuzzy eşleşmenin en uzun adayı, bellek sorgu × aday × 16 bayt),
  `limits.search.relation_depth` (1; aramada adı eşleşen notun kaç adım uzağındaki bağlı notlar da bulunur, `0` bağlı bileşenin
  tamamı; iş not + kenar sayısıyla doğrusal; kapatmak `[tuning] search_relations = false`),
  `limits.search.property_keys` (32; `prop:` süzgecinin depo görüntüsü başına önbellekte tuttuğu anahtar, en son
  kullanılanlar; `0` sınırsız, SPEC-search),
  `limits.sync.merge_input_bytes` (4 MiB; üç yönlü birleştirmenin en büyük girdisi, aşan çakışma kopya not olur).
  Yapılandırmasız depo (`NoteStore(dizin)`) `kDefaultFolderPathBytes` kullanır; bir test defaults.toml ile
  aynı olduğunu denetler. OS dosya adı sınırının tek sahibi `core::kMaxFileNameBytes` (255, `note_path.hpp`).
  Not başına bağlantı/etiket tavanı, klasör derinliği, kopya sayacı ve ad arama sigortası kaldırıldı (sonlu
  kümede arama doğal olarak biter). Kalan tek ad deneme sigortası `create_private_temp_file`'ındır (rastgele ad,
  dosya sistemi arızasına karşı; kapasite sınırı değildir).
- **TUI ve dil sunucusu (L2, L5):** `[limits.tui]` biçimli görünüm (`view_bytes` 0 = sınırsız (NY-P), `view_lines`, `table_rows`,
  `table_columns`), büyük md parçalarının düşman girdi korumaları (`preview_definition_bytes` 128 KiB,
  `preview_fence_info_bytes` 64, `preview_table_header_bytes` 4 KiB; `app::markdown_chunk_limits_of`, iki arayüz; 0
  sınırsız, riski defaults.toml'da), renklendirme (`highlight_bytes` 2 MiB, `highlight_line_bytes` 64 KiB), yazım satırı, tamamlama
  (`completion_*`), yapıştırma (`paste_bytes` 32 MiB, terminal yapıştırmasında da; bitiş işareti gelmeyen yapıştırmanın sessizlik süresi `paste_timeout` 1 sn, 0 = hiç), arama geçmişi, geri alma (`undo_bytes` 64 MiB), taşıma
  diyaloğu önerileri.
  `[limits.lsp]` sunucu çıktısı korumaları (çerçeve, JSON iç içeliği/değer sayısı, tanı, öneri, bekleyen cevap),
  yansıtılan not boyu ve üç süre (`initialize_timeout`, `write_timeout`, `shutdown_timeout`). Değerler ve neyi
  saydıkları defaults.toml'da, davranışları SPEC-tui'de. Alt sınır olan en kısa tamamlama öneki `[tuning]
  completion_min_prefix_bytes`'tadır (`0 = sınırsız` anlamsız). TUI modülleri değeri kendi struct'ıyla alır
  (`tui::view_limits_of`, `syntax_limits_of`, `completion_limits_of`, `lsp_limits_of`); bir test bu struct'ların
  kod varsayılanlarının defaults.toml ile aynı olduğunu denetler. OSC 52'nin 75.000 baytı terminal protokolü
  sınırıdır, config'te değildir. `view_bytes` GUI önizlemesinin de sınırıdır: kullanıcı bir sınır verirse üstündeki not iki arayüzde de
  reddedilmez, biçimlenmeden ham metinle ve nedeniyle (anahtar adı dahil) gösterilir. Varsayılan sınırsızdır: md, csv ve
  json biçimli görünümü parça parça üretilir (SPEC-tui "REVIEW biçimli görünüm", SPEC-gui "Önizleme").
- **PDF okuma (PDF1):** `[limits.pdf]` güvenilmez PDF'in (senkronla gelir) sınırlarıdır, okuyan `pdfview::PdfDocument`:
  `pages` (10.000; fazlası olan belge açılmaz), `render_pixels` (16.777.216; sayfa başına çizilen piksel, aşan
  yakınlaştırma çizilmez), `text_bytes` (1 MiB; sayfa başına çıkarılan metin), `context_memory` (512 MiB; belge
  başına MuPDF bellek bütçesi), `worker_memory` (1 GiB; PDF-SBX, işçi sürecinin OS bellek sınırı, Linux'ta
  `RLIMIT_DATA`; `context_memory`den büyük olmalı, aşılınca sayfa açılmaz), `duration` (10 sn; açılış ve sayfa başına metin çıkarma/çizim süresi, dolunca iş kesilir). Aşım
  kesilmiş sonuç değil `LimitExceeded` hatasıdır; 0 sınırsız, riski defaults.toml'da. `cache_pages` (32; PDF3)
  önizlemenin bellekte tuttuğu çizilmiş sayfa sayısıdır (`app::PdfPreview`, GUI dokuları; görünen sayfalar sayılmaz).
- **PDF önizleme (PDF3, `app/pdf_preview.*`):** üç arayüzün ortak katmanı. `app::is_pdf_type` (uzantı `pdf`, büyük/küçük
  harf duyarsız) önizlemeyi seçer; `app::pdf_page_separator` ayırıcı satırı; `app::read_pdf_text` bütün metni çağıranın
  iş parçacığında sayfa sayfa verir (CLI; sayfa hatası okumayı durdurmaz). `app::PdfPreview` belgeyi tek işçi iş
  parçacığında açar ve metin/çizim isteklerini FIFO, yinelemesiz yürütür; `drop_pending` yalnız bekleyenleri siler,
  yürüyen biter; çizilmiş sayfalar `cache_pages` ile LRU, metinler düşmez (sayfa sayısı x `text_bytes`); hatalı sayfa
  (süre dolması dahil) hatırlanır. Her sonuçta `on_change` (veri taşımaz, UpdateWorker gibi); yıkıcı yürüyen çağrıyı
  keser (işçi ~50 ms'de öldürülür) ve bekler. Senkronla gelen PDF otomatik açılmaz: önizleme yalnız kullanıcının
  gösterdiği nottadır; arama dizinine PDF metni henüz girmez.
- **PDF yardımcı süreci (PDF1b, kullanıcı kararı 2026-11-09):** MuPDF'in güvenilmez girdide kesilemeyen döngüleri var
  (xref onarımı, bağlantı hedefinin yavaş sayfa araması, Form XObject çoğalması); döngü döngü yamalamak yerine
  ayrıştırma ayrı süreçtedir. `pdfview::PdfDocument` (MuPDF'siz `noteapp_pdfview`) belge başına bir uzun ömürlü
  `note __pdf-worker` başlatır (`core::ChildProcess`, stdin/stdout boruları; gizli alt komut, config/log yok, stdin
  terminalse 2 ile çıkar, stdin EOF'unda 0). Tel protokolü `pdfview/protocol.hpp`: küçük-sonlu 16 baytlık başlık
  `{u32 tür, u32 0, u64 uzunluk}`, sıkı istek/cevap; Open `{sürüm (2), ebeveyn pid, dört sınır}` + baytlar, Text,
  Structured, Render; cevaplar Opened (`page_count ≤ pages`), Text (`≤ text_bytes`), StructuredResult (aşağıda),
  Rendered (uzunluk tam `8 + w·h·4`, `w·h ≤ render_pixels`), Failed `{neden, a, b}` + en çok 256 bayt ayrıntı. Uymayan
  çerçeve bozuk sayılır. İşçi i18n
  anahtarı ya da kullanıcı metni göndermez: ebeveyn `core::Error`'u kendi değerleriyle kurar, ayrıntı yalnız loga
  (`terminal_safe`), metne `clean_text` yeniden uygulanır. Süre ve durdurma ebeveyndedir (50 ms'lik bekleme
  dilimleri): süre dolunca, durdurmada, çöküşte ya da bozuk çerçevede işçi öldürülüp toplanır, çağrı
  `pdf_too_slow`/`Cancelled`/`pdf_invalid`·`pdf_page_invalid` döner ve belge geçerli kalır; sonraki çağrı işçiyi
  yeniden başlatır, baytları yeniden açtırır ve sayfa sayısının aynı olduğunu denetler. Öksüz işçi: Linux'ta işçi
  saniyede bir `getppid()`'i Open'daki ebeveyn pid'iyle karşılaştırır (PR_SET_PDEATHSIG değil), Windows'ta
  KILL_ON_JOB_CLOSE iş nesnesi. MuPDF'i yalnız `note` bağlar (`noteapp_pdfengine`); `notes` ve `notes-gui`
  bağlamaz (ctest `pdfview_link_check`). İşçinin yolu API'de açık parametredir; ön yüzler
  `core::note_executable_beside_self()` verir. Geliştirme ağacında `notes`/`notes-gui` `note`'u kendi dizininde
  bulamazsa PDF açılamaz (`error.process_not_found`); otomatik senkronla aynı durum, kurulu pakette yoktur.
  **Kum havuzu (PDF-SBX, karar ve kalan riskler: `.engineering/adr/0003-pdf-worker-sandbox.md`).**
  **Linux x86_64:** `core::ChildProcess::start_confined` işçiyi boş ortamla başlatır (ebeveynde
  kurulur; hiçbir değişken geçmez) ve ilk bayt yazılmadan `RLIMIT_CORE = 0` (hep; SIGSYS'le ölen işçi PDF'li
  belleğini dökmesin) ile `RLIMIT_DATA = worker_memory` koyar (yumuşak = sert; 0 sınırsız; biri konamazsa çocuk
  öldürülür, başlatma başarısız). İşçi (`pdfview/engine/sandbox.hpp`) tek bayt okumadan 3
  ve üstü fd'leri kapatır, stdin/stdout'a sabit tampon verir (glibc yol alan `newfstatat` çağırmasın), NO_NEW_PRIVS
  ve seccomp-bpf izin listesini TSYNC ile (her iş parçacığına) kurar. Mimari x86_64 değilse, x32 ise ya da çağrı
  listede yoksa süreç SIGSYS'le ölür
  (ebeveyn `exit_code=159` loglar, kullanıcı `pdf_invalid` görür). Liste strace ile ölçüldü, gerekçeleri
  `sandbox.cpp`'de; dosya, ağ, süreç oluşturma, sinyal gönderme ve ptrace çağrısı içermez; `mmap`/`mprotect` yalnız
  PROT_EXEC'siz, `mmap` ayrıca MAP_SHARED ve MAP_GROWSDOWN'suz (o eşlemeler `RLIMIT_DATA`'ya sayılmaz),
  `clone` yalnız CLONE_THREAD ile (öksüz denetimi iş parçacığı), `clone3` ENOSYS döner (glibc `clone`a düşer).
  Kurulamazsa işçi 6 ile çıkar, PDF açılmaz (fail-closed, kullanıcı kararı 2026-11-10); başka Linux mimarisinde de 6.
  ASan derlemesinde bellek sınırı konmaz (gölge bellek `RLIMIT_DATA`'ya sayılır) ve işçi `_exit` ile çıkar (LSan
  taraması filtreye takılmasın).
  **Windows:** `start_confined` işçiyi `CreateProcessAsUserW` ile kendi belirtecimizin
  `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` kopyasıyla (BUILTIN\Administrators yalnız deny-only: yönetici olarak
  çalışan ebeveynin işçisi yöneticinin okuyabildiği dosyaları okuyamaz), düşük bütünlükte (S-1-16-4096), konsolsuz
  (`DETACHED_PROCESS`), askıda ve yalnız stdin/stdout/NUL tutamaçlarını devrederek başlatır; azaltımlar
  `PROHIBIT_DYNAMIC_CODE`, `IMAGE_LOAD_NO_REMOTE`, `IMAGE_LOAD_NO_LOW_LABEL`, `EXTENSION_POINT_DISABLE`. Ortam yalnız
  `SystemRoot` ve `NoDefaultCurrentDirectoryInExePath=1`. Sürdürmeden önce iş nesnesine atanır: KILL_ON_JOB_CLOSE,
  tek etkin süreç (torun yok), `worker_memory` kadar ayrılmış bellek (0 sınırsız), bütün `JOB_OBJECT_UILIMIT_*`.
  Adımlardan biri (sürdürme dahil) başarısızsa askıdaki çocuk öldürülür, başlatma başarısız olur (fail-closed; logda
  `event=sandboxed_spawn_failed step=<adım> error=<Windows hata kodu>`). UI sınırlı iş iç içe olamaz: uygulama zaten
  bir iş nesnesinin içinde başlatıldıysa atama başarısız olur, PDF açılmaz (`step=job`). win32k kapatma yok: `note.exe` user32/gdi32'yi doğrudan içe aktarmasa da shell32 ve ole32
  üzerinden yükler (ayrı işçi exe'si backlog'da). Düşük bütünlük yalnız **yazmayı** keser: işçi kullanıcının
  dosyalarını okuyabilir ve ağa çıkabilir (AppContainer geniş dağıtımdan önce, kullanıcı kararı 2026-11-10).
  Win11'de ölçülmedi: işçinin Kimlik Bilgisi Yöneticisi'ndeki senkron token'ını `CredReadW` ile okuyup okuyamadığı ve
  çökme dökümünün (WER) PDF baytlarını taşıyıp taşımadığı (ADR 0003 "Kalan riskler").
  Wine bu denetimlerden ortamın süzülmesini ve Administrators'ın deny-only olmasını sınar; iş nesnesinin süreç ve
  bellek sınırını, bütünlük düzeyini, yazma reddini ve `PROHIBIT_DYNAMIC_CODE`'u uygulamaz. Onlar doctest
  `manual-win11` takımında, ctest `pdfview_win11` (etiket `manual-win11`; çapraz derlemede Disabled) olarak gerçek
  Windows 11'de elle koşulur, derleme ortamı gerekmez: `scripts/build-windows.sh --test` derleme biriminde
  `test/tests/pdfview/{pdfview_tests,pdf_sandbox_probe,fake_pdf_worker}.exe` ve `test/src/cli/note.exe` üretir (yalnız
  Windows sistem DLL'lerini içe aktarırlar); dördü aynı klasöre kopyalanır (testler derlemenin gömdüğü yol yoksa
  yardımcıları kendi yanlarında arar, `tests/pdfview/built_executable.hpp`). O klasörde cmd'de:
  `set NOTEAPP_HOME=%TEMP%\noteapp-sbx`, sonra `pdfview_tests.exe` (gerçek işçi bütün sınırlar altında PDF açar; son
  satır `Status: SUCCESS!`) ve `pdfview_tests.exe --test-suite=manual-win11 --no-skip` (kısıtların kendisi;
  `6 passed | 0 failed`). İkisi bir kez de yönetici cmd'sinde koşulur (Administrators deny-only sınaması).
- **Yapılı sayfa metni (V14):** `PdfDocument::page_structure` sayfanın metnini bloklar → satırlar → aynı biçimli
  parçalar olarak verir (`pdfview/structured_text.hpp`): parça `{metin, biçim (kalın/italik/eş aralıklı), punto}` ve
  her kod noktası için yatay kutu `{x0, x1}`; satır dikey kutu `{top, bottom}` ve tireyle bölünmüş sözcük işareti
  (`joined`, MuPDF `FZ_STEXT_DEHYPHENATE`); sayfa boyu punto. Koordinatlar sayfa kutusunun sol üstünden; işçi sayfa
  dışındaki karakterleri atar (`FZ_STEXT_CLIP`), kutuları sayfaya kırpar, metni `clean_text` kuralıyla geçerli UTF-8
  yapar (kod noktası başına bir kutu kalır). StructuredResult yükü `{f32 w, f32 h, u32 blok}`, blok `{u32 satır}`,
  satır `{u8 bayrak, f32 top, f32 bottom, u32 parça}`, parça `{u8 biçim, f32 punto, u32 bayt, metin, kod noktası
  başına f32 x0, f32 x1}`. Ebeveyn katı okur (`protocol::decode_structured`) ve her ihlali bozuk çerçeve sayar (işçi
  öldürülür, `pdf_page_invalid`): kısa ya da artık bayt; boş blok/satır/parça; bilinmeyen biçim ya da bayrak biti;
  geçersiz UTF-8; sonlu olmayan sayı; 1 pt'den küçük sayfa boyu; sayfa kutusunun ±1 pt dışındaki ya da ters kutu;
  toplam metin `> text_bytes`; çerçeve `> 12 + 35·text_bytes` (her öğe boş olmadığından metin baytı başına en çok yük). Sayılar yalnız döngü
  sınırıdır, bayt geldikçe büyünür: yalan sayı bellek ayırtmaz. Metne `clean_text` yeniden uygulanır. Yeni sınır yok.
  `FZ_STEXT_SEGMENT` ve `FZ_STEXT_PARAGRAPH_BREAK` ölçüldü ve alındı (2026-11-09): tek sütunlu markdown dışa
  aktarımında (başlık, paragraflar, liste) blok yapısı birebir aynı; iki sütunlu sayfada bunlarsız aynı taban
  çizgisindeki sol ve sağ satırlar tek blokta sırayla karışıyordu, bunlarla sütunlar ayrı bloklarda okuma
  sırasındadır. MuPDF ikisini deneysel sayar; yavaşlık süre sınırıyla kesilir. İç içe yapı blokları özyinelemesiz
  yürünür.
  Biçimleme (başlık, paragraf) ve seçim kuralları MuPDF'siz `app/pdf_text.hpp`'dedir (SPEC-tui, SPEC-gui "PDF
  önizleme"): başlık satırın baskın puntosunun sayfanın baskın gövde puntosuna oranından (≥ 1,6 h1, ≥ 1,3 h2,
  ≥ 1,12 h3), paragraf blok içindeki satırların birleşmesinden (aralık açılırsa, ilk satır girintiliyse ya da satır
  liste imiyle başlarsa yeni paragraf; tireyle bölünmüş sözcük tiresiz birleşir). CLI `note show` düz metni (Text)
  kullanmayı sürdürür.
- **Uygulama (L1):** `[limits.app]` `recent_commands` (20), `trash_undo_steps` (0), `tabs` (0), `map_nodes` (2.000) ve
  `map_depth` (6; harita çevre kapsamının en çok adımı, `MapOptions::kMaxDepth` sabiti yerine; TUI `1`–`9` tuşları ve GUI
  adımlayıcısı `app::MapModel::max_depth`'ten okur, sınır küçülürse geçerli derinlik ona iner) ve `query_cell_bytes`
  (4 KiB; harita sorgu tablosunun hücresi, F3: `app::query_outcome` hücreyi bir kez temizler ve bu baytta kod noktası
  sınırında `…` ile keser; 0 sınırsız, gövde döndüren büyük sonuçta bellek ve tablo kurulumu büyür; değer açılışta
  okunur). İki arayüz aynı değeri
  kullanır (SPEC-gui/SPEC-tui "Harita", "Sekmeler"). İki arayüzde Ayarlar'da Kaydet `map_nodes` ve `map_depth`'i dosyadan
  okuyup çalışan haritaya hemen uygular (`app::adopt_live_limits`; uygulama açıkken `note config set` ile değişmiş olabilir);
  öteki sınırlar açılışta kurulan modüllere verilir, yeniden başlatınca geçerli olur.

### i18n
- Kataloglar `resources/i18n/{en,tr}.toml` (düz anahtarlar, `{ad}` yer tutucuları), derlemede gömülür.
- Çözüm: seçili dil → `en` → anahtarın kendisi (+ uyarı logu).  `format_error`: `reason` argümanı `reason.<değer>` kataloğuyla çevrilir. `LoadedConfig::warnings` `Error` (ConfigInvalid) listesidir. Çoğul formu gerektiren cümle kurulmaz.

### Editör
- Çözüm sırası: `config.editor` → `$VISUAL` → `$EDITOR` → Windows: PATH'te `edit` varsa o, yoksa `notepad`;
  Unix: `nano`, yoksa `vi`. Komut tırnak kurallarıyla argümanlara bölünür (`"code --wait"` desteklenir),
  dosya yolu son argüman olarak eklenir. Kabuk kullanılmaz (CreateProcessW / posix_spawn); süreç beklenir.
- **Süreç sertleştirme (T36):** program `find_program`'la bir kez çözülür ve bulunan yol başlatılır (POSIX'te
  `posix_spawnp`'in ikinci PATH araması yok; PATH'in yalnız mutlak girdilerine bakılır — `.`, boş girdi,
  `node_modules/.bin` atlanır —, PATH yoksa varsayılan arama da yok; Windows'ta her çocuk
  `NoDefaultCurrentDirectoryInExePath=1` ile başlar). `.cmd`/`.bat` için `cmd.exe` `GetSystemDirectoryW`'den gelir, PATH'ten değil. `EnvOverrides` değeri `nullopt`
  ise değişken çocuktan silinir; editör ve `notes-gui` `without_secrets()` ile `NOTEAPP_GIT_TOKEN` /
  `NOTEAPP_GIT_TOKEN_HOST` görmeden çalışır.
- Bulunamazsa `EditorNotFound` (denenen adaylar args'ta); sıfır dışı çıkış kodu `EditorFailed`.

### Platform dizinleri ve log
- Config / durum dizinleri SPEC.md §8'deki gibi; `NOTEAPP_HOME` ayarlıysa ikisi de onun altına.
- Log: `<durum>/log.txt`, `tuning.log_max_bytes` (1 MiB) aşılınca `log.1.txt`'ye döner; satır: `YYYY-MM-DDTHH:MM:SS seviye pid mesaj` (yerel saat); G/Ç hatası uygulamayı düşürmez; seviye info/warn/error; token asla yazılmaz.
- Windows'ta süreç başında konsol kod sayfası UTF-8'e ayarlanır (cli/tui ortak yardımcı).

## Kabul kriterleri

1. `NotePath::parse` yukarıdaki her ret kuralı için ayrı testte reddeder; `work/2026/plan.md` gibi geçerli yolları kabul eder.
2. notes_dir dışına işaret eden sembolik bağ ya da Windows junction'ı üzerinden okuma/yazma/oluşturma/taşıma/silme
   reddedilir; içinde bağ olan klasör `force` ile de silinmez ve bağın hedefine dokunulmaz.
3. `slugify("# Toplantı Notları: Q4!") == "toplanti-notlari-q4"`; boş/sembol-only girdi zaman damgalı ad üretir.
4. Aynı adla iki `create` → `ad.md` ve `ad-2.md`; hiçbir dosya ezilmez.
5. Yazma sırasında hata simüle edildiğinde orijinal içerik aynen kalır ve `.tmp` dosyası kalmaz.
6. Dışarıdan değiştirilmiş dosyaya `write` → `ModifiedExternally`, dosya değişmez.
7. `list()` gizli dosyaları/`.git`'i atlar, ikili dosyayı da listeler, mtime azalan sıralar; `looks_binary` yalnız
   ilk 8 KB'taki NUL'u ikili sayar.
8. Dolu klasörde `remove_folder(force=false)` → `NotEmpty` ve hiçbir şey silinmez.
9. Bozuk `config.toml` uygulamayı çökertmez: varsayılanlar + log uyarısı.
10. `en.toml` ve `tr.toml` anahtar kümeleri eşit (test); eksik anahtar en'e düşer.
11. Editör komutu tırnaklı argümanlarla doğru bölünür; editör bulunamazsa denenen adaylar mesajda listelenir.

## Testler
Small: NotePath, slugify, config ayrıştırma, katalog eşliği, editör komut bölme.
Medium: geçici dizinde NoteStore (oluştur/yaz/taşı/sil/klasör, sembolik bağ, dış değişiklik, atomiklik);
`run_editor` testi, test için derlenen `fake-editor` yardımcı programıyla.

## Kapsam dışı
Dosya izleme (ön yüzlerin işi), etiket/front-matter, şifreleme.

## Değişiklik 2026-10-31
- **MODIFIED** Amaç: "Qt'ye bağımlı değildir" cümlesi GUI araç takımından bağımsızlık olarak genelleştirildi. Davranış değişmedi;
  GUI'nin Qt'den SDL3 + Dear ImGui'ye geçişi için bkz. [SPEC.md](SPEC.md) ve [SPEC-gui.md](SPEC-gui.md).

## Değişiklik 2026-11-02 (G4)
Bu bölüm yukarıdaki "Yazma", "Listeleme", "Klasörler", "Yol güvenliği" ve "Çöp kutusu"nun dosya sistemi ayrıntılarını geçersiz kılar. `SPEC-graph.md` bağlayıcıdır.
- **MODIFIED** `NoteStore(dizin, slug)` `<dizin>/graph.db` üzerinde çalışır; uygulama `NoteStore::for_config(config)` ile `data_dir()`'i kullanır. Kopyalar tek bağlantıyı paylaşır, thread-safe'tir. Okuma veritabanını kurmaz.
- **ADDED** `core::data_dir()`: `NOTEAPP_HOME/data`, `$XDG_DATA_HOME/noteapp`, macOS Application Support, Windows `%LOCALAPPDATA%\noteapp`.
- **Uygulama şeması:** `Note {name, type, body, created_at, updated_at, origin, deleted_at, trashed_from}`, `Folder {name}`, `IN_FOLDER`, `CHILD_OF`, `LINKS_TO`. `Tag` bu turda yok, sonraya bırakıldı. Motor nottan habersizdir: bunların hepsi core'dadır (`note_snapshot.cpp`, `note_write.cpp`).
- **Yol okuma kuralı (saf fonksiyon, her cihazda aynı):**
  - Klasör kimliği `derive_id(üst, ad, tuz)`; aynı klasörü iki cihaz aynı düğüme getirir. Yeniden adlandırma tek yazmadır, altındakilere dokunmaz. Eski adla yeni klasör sonraki tuzu alır.
  - Birden çok `IN_FOLDER` ya da `CHILD_OF` (eşzamanlı taşıma): en büyük kenar kimliği geçerlidir. Kenar kimliği UUIDv7'dir, yani en son taşıma. Sonraki taşıma hepsini siler.
  - `CHILD_OF` döngüsü: döngüdeki en küçük kimlikli klasörün üst kenarı yok sayılır, kök altına düşer.
  - Aynı yolda iki not: eski not (`created_at`, eşitlikte kimlik) yolu alır, diğeri `-2`, `-3`. Ek saklanmaz.
  - Ad ve yol `NotePath` kapısından geçer, ad `/` içeremez. Geçemeyen düğüm listelenmez, loglanır, veride kalır (`note graph export`).
- **MODIFIED** `ModifiedExternally`: mtime yerine `Revision` (gövdeyi yazan dot). `NoteInfo.mtime` artık `updated_at`'tır, yalnız sıralama ve gösterim içindir. `write` yeni `Revision` döner. Başka süreç aynı notu yazdıysa çakışma akışı çalışır.
- **MODIFIED** Aynı baytları `write` işlem üretmez, revizyon değişmez.
- **ADDED** Dış değişiklik tespiti `Graph::data_version()` (`PRAGMA data_version`): değişiklik yokken `list()` tüm notları okumaz (5.000 notta 0,3 ms), varsa 25 ms.
- **MODIFIED** `.trash` yerine `deleted_at`, API aynı. Silme gövdenin her head'ini görmüş olmalı, görmediği farklı baytlı düzenleme kazanır (saf okuma kuralı, `ReadTxn::saw`). `trashed_from` atıldığı yolu tutar: klasör silinmiş olsa da geri yükleme oraya döner.
- **MODIFIED** `absolute_path` kalktı. Dosya isteyen araç `edit_in_temp_file` kullanır: özel dosya (`create_private_temp_file`, 0600, 0700 dizin), bağ okunmaz, çakışma denetimiyle geri yazma, sonra silme. Kayıt başarısızsa dosya kalır, hata yolunu söyler (`error.edit_kept`). Bilinen sınır: süreç öldürülürse dosya `<durum>/tmp` altında kalır.
- **MODIFIED** `.keep` emekli: boş klasör `Folder` düğümüdür. `remove_folder` yalnız canlı notları sayar.
- **MODIFIED** Not boyutu (L4b, 2026-11-03): çerçeve (`limits.graph.frame_bytes`) not boyutunu sınırlamaz; büyük not günlükte parçalara bölünür ve senkronlanır (SPEC-graph "Parçalı çerçeve ve çok çerçeveli işlem"). Tek sınır depolamanındır: not, işlemiyle birlikte SQLite'ın tek değer sınırına (1 GB) sığmalıdır, aşan kayıt `LimitExceeded`; geçişte bu sınırı aşan dosya `too_large` ile atlanır ve raporlanır.
- **ADDED** `LINKS_TO`: `[[hedef]]`, `[[hedef|takma]]`, `[[hedef#başlık]]`, kayıtla aynı işlemde. Çözüm sırası yol, uzantısız yol, dosya adı, yalın ad. Hedef yazma anında çözülür: sonradan oluşan hedef, kaynak yeniden kaydedilene kadar bağlanmaz. Not başına en çok 1024 bağlantı.
- **ADDED** `created_at`, `origin` gerçek değerdir, `origin` derleme platformu sabitidir (`windows`, `linux`, `macos`, `android`; geçişte `unknown`). `origin:` ve `created:` araması çalışır.
- **MODIFIED** `tuning.autosave_ms` 1000'den 3000'e. Ölçüm (30 dakikalık benzetim, `tests/perf/note_store_perf_test.cpp`): 2 KiB notta 1 sn'de 119 kayıt 462 KiB, 3 sn'de 57 kayıt 217 KiB; 20 KiB notta 2,6 MiB'den 1,26 MiB'ye. Nedenselliğe dokunmaz. Bedel: kapanmadan öldürülen GUI son 3 sn'yi kaybeder (kapanış ve not geçişi yine kaydeder).
- **ADDED** `health()` depo sorununu ön yüze söyler.
- **Performans (Release, 5.000 not, 2 KB):** `note "x"` 36 ms (sınır 200), açılış 44 ms (300), tuş başına liste 2 ms (50).

## Değişiklik 2026-11-01
- **ADDED** Çöp kutusu (T55a, kullanıcı kararı: senkronlanır, `<notes_dir>/.trash/` altında):
  `NoteStore::trash(path) -> TrashItem`, `list_trash()`, `restore(item) -> NotePath`, `purge(item)`, `empty_trash()`.
  `TrashItem {NotePath path; file_time_type trashed_at; uintmax_t size}`; `path` `.trash`'e göre göreli, notun yoludur.
  - **Taşıma:** bayt değişmez, `.trash/<yol>`'a ezmeyen atomik rename (`move_no_replace`). Çöpte ad doluysa `create()`'in
    biçimiyle `-2`, `-3`… (`a.tar.gz` → `a.tar-2.gz`); `restore` canlı ağaçta aynı kuralla ad seçer, asla ezmez.
    Klasörler yeniden oluşur; çöpte boş kalan klasörler budanır. Yan dosya olmadığından `x-2.md` geri yüklenince
    `x.md` değil `x-2.md` olur (bilinen sınır).
  - **Atılma anı = dosyanın mtime'ı (seçim):** `trash` rename'den sonra mtime'ı o ana çeker. Addaki ek yerine bu seçildi:
    yol `.trash/<yol>` olarak kalır, iki cihazın aynı notu atması çakışma üretmez, geri yükleme ad çözmek zorunda kalmaz.
    Bedeli: git mtime taşımaz, diğer cihazda `trashed_at` notun oraya indiği andır; geri yüklenen notun mtime'ı atılma
    anıdır. Mtime yazılamazsa uyarı loglanır, not yine çöptedir.
  - **Görünürlük:** `.trash` noktalı olduğundan `list()`, `folders()`, arama ve klasör silme sayımı onu görmez; `NotePath::parse`
    reddeder, bu beş işlev dışında ona yol yoktur. `remove` kalıcı silmeye devam eder.
  - **Bağ kuralı:** `.trash`'in kendisi dahil her bileşen `resolve` kuralından geçer (`reason = link`); `list_trash` ve
    `empty_trash` `.trash` bağsa reddeder; içinde bağ varsa `empty_trash` hiçbir şey silmez (`reason = contains_link`).
  - Aynı adlı bir dosyanın (`.trash/a`) altına not atılamaz, canlı ağaçta dosya olan bir yolun altına geri yüklenemez: `Io`,
    hiçbir şey değişmez (`move` ile aynı sınır).
- **ADDED** `NoteMeta {path, size, updated_at, created_at?, origin?, sync_status}`, `SyncState {Synced, Modified, New, ConflictCopy,
  SyncOff}`, `NoteStore::list_meta()`. `created_at` ve `origin` depolama kararına kadar boştur; dosya sisteminin doğum
  zamanından üretilmez. `sync_status`'u `sync::fill_sync_status` doldurur (SPEC-sync).
- **ADDED** `error.query_invalid`, `reason.must_be_{sort_field,size_bound,age_or_date,sync_state,folder}` katalog anahtarları (SPEC-search).
- **MODIFIED** Kapsam dışı: "Çöp kutusu" çıkarıldı.

## Değişiklik 2026-11-06 (ilişki araması)
- **ADDED** `limits.search.relation_depth` (1, `0` sınırsız) ve `[tuning] search_relations` (true). Derinlik bir kapasite sınırıdır
  (0 = sınırsız kuralı); kapatmak ayrı anahtardır, `0` "kapalı" anlamına gelmez.
- **MODIFIED** Senkronla ya da Cypher'la gelen Tag düğümünün `name`/`display`'i ilişki adı gibi okunurken temizlenir
  (`shown_name`: kırpılır, C0/DEL/C1/görünmez biçim U+FFFD, `limits.graph.relation_name_bytes`'ta kesilir); `tags()`, `list()`'in
  etiketleri, bağlantı panelinin etiketleri ve `tag:` aynı temiz biçimi görür. Graf değişmez.
- **ADDED** `NoteStore::shared_edges()`: `edges()` ile aynı liste, görüntü başına bir kez okunur ve paylaşılır (arama her tuşta sorar);
  `edges()` artık bunun kopyasıdır.

## Değişiklik 2026-11-03 (T59e)
- **ADDED** `[lsp.settings.<dil>]` → `LspConfig::settings` (`core::Json`), `reason.must_be_settings_table`; `LspConfig::kMaxServersPerLanguage`, `kMaxSettingsDepth`, `kMaxSettingsValues`.
- **MODIFIED** `[lsp.servers]` değeri argv listesi de olabilir (`LspConfig::servers` dil → argv listesi).
- **MODIFIED** `save_config` kökteki satır içi `lsp` tablosunu başlıklı yazar, tablo olmayan `lsp` değerini yazmaz (satır içi kapatma kayıtta kayboluyordu).
- **ADDED** `core::find_program_on_absolute_path`: yalnız mutlak PATH girdileri; dil sunucusu araması bunu kullanır. `find_program` değişmedi.
- **ADDED** `core::json_text`/`append_json` (Json yazımı), `Json::operator==`.

## Değişiklik 2026-11-03 (L0 sınır sözleşmesi)
- **ADDED** `[limits.<modül>]` bölümü, `core::Limit`/`Limits`/`LimitUnit`, `core::parse_limit`, `core::limit_entries`,
  `reason.must_be_limit_{count,size,duration}`; ilk anahtar `limits.search.content_results` (TUI `main.cpp` ve GUI
  `gui_state.cpp`'deki iki `kContentResultLimit = 1000` kopyasının yerine). Bkz. "Sınırlar".
- **MODIFIED** `tuning.max_note_bytes` `core::Limit` oldu: `0 = sınırsız`, birimli yazım kabul edilir (A33).
- **MODIFIED** `set_config_value` ve `note config list/get/set` `limits.*` anahtarlarını kabul eder.

## Değişiklik 2026-11-09 (V14 yapılı PDF metni)
- **ADDED** PDF işçi protokolü sürüm 2: Structured isteği ve StructuredResult cevabı (sayfanın bloklar → satırlar → biçimli parçalar modeli, karakter kutuları, sayfa boyu), ebeveynde katı doğrulama (bkz. "Yapılı sayfa metni"). Text değişmedi (CLI). Neden: plan V14, TUI biçimli PDF ve GUI metin seçimi.
