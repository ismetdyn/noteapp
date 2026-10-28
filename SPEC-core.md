# Spec: core

> Modül: `core` · Bağımlılık: — · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Tüm ön yüzlerin (cli/tui/gui) ve `search`/`sync`'in üzerine oturduğu ortak çekirdek: not deposu, yol
güvenliği, atomik yazma, config, i18n, editör başlatma, platform dizinleri, log. Qt'ye bağımlı değildir.

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
    std::expected<NotePath, Error> create(const NewNote&, system_clock::time_point now = now());  // ad boşsa içerikten slug
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
`remove` dosyayı kalıcı siler (çöp kutusu yok; senkron açıksa geçmişte kalır). Onay sorumluluğu ön yüzdedir.

### Config (`config.toml`)
```toml
language = "auto"        # auto | tr | en   (auto: Windows UI dili / LC_ALL→LC_MESSAGES→LANG)
notes_dir = "~/notes"
editor = ""              # boş: $VISUAL → $EDITOR → platform varsayılanı
default_type = "md"
default_folder = ""      # hızlı notların klasörü ("" = kök)
types = ["md", "txt", "csv", "cpp", "java", "py", "json", "sh"]   # seçim listesi; elle başka uzantı da yazılabilir

[sync]
enabled = false
remote = ""              # https://github.com/<u>/notes.git → token | git@github.com:<u>/notes.git → SSH
branch = "main"
idle_seconds = 30
author_name = ""         # boş: git global config → OS kullanıcı adı
author_email = ""

[tuning]                # arayüzde görünmeyen ayarlar; yalnız defaults.toml'da (kullanıcı isterse geçersiz kılar)
slug_max_length = 60
min_confidence = 0.5     # 0..1
max_scan_bytes = 65536
max_note_bytes = 10485760
rescan_seconds = 5
autosave_ms = 1000
exit_timeout_seconds = 10
log_max_bytes = 1048576   # log.txt bu boyutu aşınca log.1.txt'ye döner
sync_stale_lock_minutes = 10   # bundan eski sync.lock, sahibi yaşasa bile bayat sayılır (SPEC-sync döngü 1)
sync_timeout_seconds = 120     # AutoSync bir `note sync --background` sürecini en çok bu kadar bekler (SPEC-sync karar 3)
```
- Varsayılanlar `resources/defaults.toml`'dan (gömülü) gelir; dosya yoksa varsayılanlarla çalışılır, ilk
  kayıtta oluşturulur. Bilinmeyen anahtar → uyarı logu, yok sayılır. Geçersiz değer (yanlış tür, language ∉ {auto,tr,en}, boş/noktalı tip, idle_seconds ≤ 0, boş branch, ≤ 0 tuning) → o anahtar için varsayılan + uyarı; TOML sözdizimi hatası → tüm varsayılanlar + satır/sütunlu uyarı. `save_config` `[tuning]`'e yalnız
  gömülü varsayılandan farklı anahtarları yazar: elle verilmiş ince ayar uygulamanın kaydında (ör. `note config
  set`) kaybolmaz, dokunulmamış varsayılan dosyaya gömülmez (T36).
- `~` ve ortam değişkenleri `notes_dir`'de genişletilir. Kaydetme atomiktir. Yorumlar korunmaz (bilinen sınır).
- Kimlik doğrulama yöntemi `remote` şemasından türetilir; ayrı anahtar yok.

### i18n
- Kataloglar `resources/i18n/{en,tr}.toml` (düz anahtarlar, `{ad}` yer tutucuları), derlemede gömülür.
- Çözüm: seçili dil → `en` → anahtarın kendisi (+ uyarı logu).  `format_error`: `reason` argümanı `reason.<değer>` kataloğuyla çevrilir. `LoadedConfig::warnings` `Error` (ConfigInvalid) listesidir. Çoğul formu gerektiren cümle kurulmaz.

### Editör
- Çözüm sırası: `config.editor` → `$VISUAL` → `$EDITOR` → Windows: PATH'te `edit` varsa o, yoksa `notepad`;
  Unix: `nano`, yoksa `vi`. Komut tırnak kurallarıyla argümanlara bölünür (`"code --wait"` desteklenir),
  dosya yolu son argüman olarak eklenir. Kabuk kullanılmaz (CreateProcessW / posix_spawnp); süreç beklenir.
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
Çöp kutusu, dosya izleme (ön yüzlerin işi), etiket/front-matter, şifreleme.
