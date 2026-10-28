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
struct NoteInfo { NotePath path; std::filesystem::file_time_type mtime; std::uintmax_t size; bool binary; };
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
std::expected<void, Error> save_config(const Config&);        // [tuning] hariç, atomik
std::string tr(std::string_view key, const Args& args = {});   // i18n
std::expected<int, Error> run_editor(const Config&, const std::filesystem::path& file);
}
```

## Davranış

### Yol güvenliği (`NotePath::parse`)
Reddedilir: boş yol; mutlak yol / sürücü harfi / UNC; `.` veya `..` bileşeni; `.` ile başlayan bileşen
(gizli adlar uygulamaya ayrılmış); `< > : " \ | ? *` ve kontrol karakterleri (ayırıcı yalnız `/`);
Windows ayrılmış adları (`CON PRN AUX NUL COM1-9 LPT1-9`, uzantılı hâlleri dahil, büyük/küçük harf duyarsız);
sonda boşluk/nokta; 255 bayttan uzun bileşen. Ek olarak `NoteStore` her erişimde kanonik yolun
`notes_dir` içinde kaldığını doğrular (sembolik bağla kaçış engellenir).

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
- **Dışarıdan değişiklik:** `write` diskteki mtime `expected_mtime`'dan farklıysa yazmaz → `ModifiedExternally`.
- UTF-8 BOM okunurken korunur, yazılırken olduğu gibi geri yazılır.

### Listeleme
- `notes_dir` altında özyinelemeli; nokta ile başlayan dosya/dizinler atlanır.
- Her normal dosya bir nottur; ilk 8 KB'ta NUL baytı varsa `binary = true` (listede görünür, önizleme/arama yapılmaz).

### Klasörler
- `create_folder` boş klasöre gizli `.keep` dosyası koyar (git boş dizin taşımadığı için senkron korunur).
- `remove_folder`: içinde not varsa `force` olmadan `NotEmpty` (args: not sayısı); `force` ile kalıcı silinir.

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
```
- Varsayılanlar `resources/defaults.toml`'dan (gömülü) gelir; dosya yoksa varsayılanlarla çalışılır, ilk
  kayıtta oluşturulur. Bilinmeyen anahtar → uyarı logu, yok sayılır. Geçersiz değer (yanlış tür, language ∉ {auto,tr,en}, boş/noktalı tip, idle_seconds ≤ 0, boş branch, ≤ 0 tuning) → o anahtar için varsayılan + uyarı; TOML sözdizimi hatası → tüm varsayılanlar + satır/sütunlu uyarı. `save_config` `[tuning]` yazmaz.
- `~` ve ortam değişkenleri `notes_dir`'de genişletilir. Kaydetme atomiktir. Yorumlar korunmaz (bilinen sınır).
- Kimlik doğrulama yöntemi `remote` şemasından türetilir; ayrı anahtar yok.

### i18n
- Kataloglar `resources/i18n/{en,tr}.toml` (düz anahtarlar, `{ad}` yer tutucuları), derlemede gömülür.
- Çözüm: seçili dil → `en` → anahtarın kendisi (+ uyarı logu). Çoğul formu gerektiren cümle kurulmaz.

### Editör
- Çözüm sırası: `config.editor` → `$VISUAL` → `$EDITOR` → Windows: PATH'te `edit` varsa o, yoksa `notepad`;
  Unix: `nano`, yoksa `vi`. Komut tırnak kurallarıyla argümanlara bölünür (`"code --wait"` desteklenir),
  dosya yolu son argüman olarak eklenir. Kabuk kullanılmaz (CreateProcessW / posix_spawnp); süreç beklenir.
- Bulunamazsa `EditorNotFound` (denenen adaylar args'ta); sıfır dışı çıkış kodu `EditorFailed`.

### Platform dizinleri ve log
- Config / durum dizinleri SPEC.md §8'deki gibi; `NOTEAPP_HOME` ayarlıysa ikisi de onun altına.
- Log: `<durum>/log.txt`, 1 MB'ta `log.1.txt`'ye döner; seviye info/warn/error; token asla yazılmaz.
- Windows'ta süreç başında konsol kod sayfası UTF-8'e ayarlanır (cli/tui ortak yardımcı).

## Kabul kriterleri

1. `NotePath::parse` yukarıdaki her ret kuralı için ayrı testte reddeder; `work/2026/plan.md` gibi geçerli yolları kabul eder.
2. notes_dir dışına işaret eden sembolik bağ üzerinden okuma/yazma reddedilir.
3. `slugify("# Toplantı Notları: Q4!") == "toplanti-notlari-q4"`; boş/sembol-only girdi zaman damgalı ad üretir.
4. Aynı adla iki `create` → `ad.md` ve `ad-2.md`; hiçbir dosya ezilmez.
5. Yazma sırasında hata simüle edildiğinde orijinal içerik aynen kalır ve `.tmp` dosyası kalmaz.
6. Dışarıdan değiştirilmiş dosyaya `write` → `ModifiedExternally`, dosya değişmez.
7. `list()` gizli dosyaları/`.git`'i atlar, ikili dosyayı `binary=true` işaretler, mtime azalan sıralar.
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
