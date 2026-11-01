# Spec: cli

> Modül: `cli` (`note` çalıştırılabiri) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Terminalden saniyeler içinde not eklemek ve notları betiklenebilir biçimde yönetmek.

## Komutlar

| Komut | Davranış |
|---|---|
| `note [-t TİP] [-f KLASÖR] [-n AD] [--no-sync] METİN…` | Argümanlar tek boşlukla birleşip not olur |
| `… \| note [bayraklar]` | stdin TTY değilse ve metin argümanı yoksa içerik stdin'den |
| `note [bayraklar]` | Metin de stdin de yoksa editör açılır (aşağıda) |
| `note -e [bayraklar] [METİN…]` | Editörü her durumda açar; METİN varsa ön içerik olur (ör. başlık yazıp devam etmek) |
| `note ls [SORGU] [-f KLASÖR]` | Notları listeler: sorgu varsa fuzzy sıra, yoksa mtime azalan |
| `note find TERİM…` | İçerik araması; `yol:satır: kesit` (TTY'de vurgulu) |
| `note show SORGU` | En iyi eşleşmenin içeriğini yazdırır |
| `note edit SORGU` | En iyi eşleşmeyi editörde açar |
| `note rm YOL [-y]` | **Yalnız tam yol**; onay ister (TTY değilse `-y` zorunlu) |
| `note mv KAYNAK HEDEF` | Taşı/yeniden adlandır; uzantı değişimi = tip değişimi; klasörler oluşur |
| `note export --pdf YOL [-o ÇIKTI]` | PDF dışa aktar (aşağıda) |
| `note sync` · `sync status` · `sync on\|off` · `sync init REMOTE [--replace]` | Senkron yönetimi (aşağıda) |
| `note sync login [HOST]` · `sync logout [HOST]` | Token'ı ekosuz stdin'den okuyup kasaya yazar / siler |
| `note config path\|list\|get ANAHTAR\|set ANAHTAR DEĞER` | Config yönetimi |
| `note query [--write] [--json] [--param K=D]… CYPHER` | Graph sorgusu (aşağıda) |
| `note graph migrate [--skip-failed]` | Eski düz dosyaları içe aktar ([SPEC-graph.md](SPEC-graph.md)) |
| `note graph export --format jsonl [-o DOSYA]`, `note graph import [--overwrite] DOSYA` | Tüm graph'ı JSON satırları olarak çıkar ya da yükle |
| `note export --all DİZİN` | Tüm notları bayt bayt düz dosya olarak çıkar |
| `note --help` · `note help KOMUT` · `note --version` | Yardım (i18n) |

- Alt komut adıyla aynı metinde not eklemek için: `note -- ls`.
- "En iyi eşleşme" = tam yol varsa o, yoksa `search::rank_notes` ilk sonucu; seçilen yol stderr'e yazılır.
  Eşleşme yoksa çıkış kodu 1.
- **Terminal temizliği (T36):** stdout TTY ise `find` (yol + kesit) ve `show` not baytlarını
  `sanitize_for_terminal`'dan geçirir: C0 denetim karakterleri (`\t`, `\n` hariç), DEL, C1 (U+0080–U+009F) ve geçersiz
  UTF-8 → `U+FFFD`, `\r` atılır. Boruya/dosyaya çıktı bayt bayt aynıdır. Neden: VT işleme açıkken senkronla gelen not
  OSC 52 (pano) ya da başlık dizisi yollayabilirdi.

## Hızlı ekleme kuralları

- **Tip:** `-n`'de yazılan uzantı (`-n deneme.cpp`, `a.tar.gz`, `notlar.v2`: değiştirilmez, üstüne eklenmez; SPEC-tui "Uzantı kuralı"); yoksa `-t`; yoksa `detect::suggest` (tek satır → `default_type`).
- **Ad:** `-n` (NotePath kurallarıyla doğrulanır, slug uygulanmaz) yoksa içerikten slug. **Klasör:** `-f`, yoksa `default_folder`.
- **Editör akışı:** OS geçici dizininde `noteapp-<rastgele>.<default_type>` oluşturulur → editör açılır →
  kapanınca okunur. Boş/yalnız boşluk → "iptal edildi", çıkış 1, dosya oluşmaz. Kayıt başarısız olursa geçici
  dosya **silinmez** ve yolu yazdırılır (veri kaybı yok); başarıdaysa silinir. `-e` ile metin argümanları geçici
  dosyaya ön içerik olarak yazılır; kullanıcı içeriği değiştirmeden kapatırsa ön içerik not olur.
- Çıktı tek satır: `Kaydedildi: work/plan.md (md, önerilen)` / `(java, seçilen)`.
- **Senkron:** `sync.enabled` ve `--no-sync` yoksa, yazmadan sonra ayrık arka plan süreci
  `note sync --background` başlatılır (çıktısı yok, sonucu log + sync-status'a). Komut ağ beklemez.
  `rm`, `mv` ve `edit` (dosya değiştiyse) de aynısını yapar; `--no-sync` bunlarda her konumda kabul edilir.

## Senkron komutları (T27; ayrıntı SPEC-sync karar 9)

- `note sync`: ön planda, süreç içinde tek döngü; özet satırı + her çakışma kopyasının yolu. Hata (`sync.busy`
  dahil) → çıkış 4.
- `note sync status`: açık/kapalı, `sync.remote` (ssh dışında kullanıcı bilgisi çıkarılmış), son başarı (yerel
  saat) ve son hata (`sync-status.toml`).
- `note sync on|off`: `sync.enabled`'ı yazar (bozuk config'te `config set` gibi reddeder, çıkış 1). `on`, remote
  yoksa `sync init` yönlendirmesiyle çıkış 1; varsa önce `enable(remote)` (kurulu depoda no-op).
- `note sync init REMOTE [--replace]`: `enable`; başarıda `sync.remote` + `sync.enabled = true` kaydedilir.
  Farklı origin (`sync.remote_mismatch`): TTY'de yerelleştirilmiş evet harfiyle onay, değilse çıkış 1 +
  `--replace` yönlendirmesi; `--replace` sormadan değiştirir.
- `note sync login [HOST]` / `logout [HOST]`: HOST yoksa `sync.remote`'un host'u, o da yoksa çıkış 2. Token
  TTY'de ekosuz (Windows `ENABLE_ECHO_INPUT`, POSIX `ECHO` kapalı; Ctrl-C dahil her çıkışta geri yüklenir), boruda
  ilk satırdan okunur; hiçbir yere yazdırılmaz.
- `note sync --background`: iç kullanım (yukarıdaki ayrık süreç ve TUI/GUI `AutoSync`).

## PDF dışa aktarma

`note` PDF'i süreç içinde `noteapp_pdf` ile üretir (`pdf::export_pdf`); `notes-gui` ve ekran gerekmez, GUI'siz
derlemede de çalışır. Girdi `NoteStore::read` ile okunur;
PDF yalnız bu baytlardan üretilir, dosya yeniden açılmaz. Okuma/yol hatasında hata türüne göre çıkılır (T36).
Binary (ikili) not reddedilir, çıkış 1. `pdf::export_pdf` hatasında çıkış kodu hata türünden gelir (ör. yazılamayan
çıktı: 3, yarım PDF bırakılmaz). Başarıda `cli.exported` satırı stdout'a yazılır.
Varsayılan çıktı: geçerli dizinde `<ad>.pdf`; `-o` geçerli dizine göredir. `--pdf` zorunludur, yoksa kullanım hatası (2).
Kapsam: A4, 15 mm; Markdown (`.md`) için başlık, kalın/italik, liste, kod, alıntı, bağlantı metni, GFM tablo; diğer
tipler düz metin (eş aralıklı). Yalnız soldan sağa yazı: Arapça/İbranice ve karmaşık shaping yok, Türkçe tam desteklenir.
Gömülü DejaVu'da olmayan glifler (CJK vb.) için PDF çıktısı doğrulanmadı; BMP dışı kod noktaları (emoji) `U+FFFD` olur. Görseller alt
metne (yoksa `pdf.image_placeholder`) iner; hiçbir görsel/bağlantı hedefine istek yapılmaz.

## Graph sorgusu

`note query` uygulamanın deposu `<veri dizini>/graph.db` üzerinde çalışır. Dosya yoksa çıkış 1; yalnız
`--write` oluşturur. `--write` olmadan yazan sorgu çıkış 2. Söz dizimi, desteklenmeyen öğe, eksik parametre, geçersiz
`--param`: 2. Çalışma anı hatası ve aşılan sınır: 1. Bozuk veya yeni sürüm dosya, G/Ç: 3. Dil: [SPEC-graph](SPEC-graph.md).
`--param k=v`: `null`, `true`, `false`, tam sayı, sonlu ondalık otomatik; `k="12"`, `inf`, `nan` ve aralık dışı sayı (`1e999`) metindir. Değer sorgu metnine girmez.
Tablo her zaman terminal için temizlenir (hücrede `\n` ve `\t` görünür kaçış olur, `bytes` `<bytes:N>`). `--json`:
`{"columns":[…],"rows":[[…]]}`; denetim karakterleri `\uXXXX`, `bytes` `{"$bytes":"<base64>"}`, zaman
`{"$timestamp":ms}`, düğüm ve kenar `{"$node":"<hex>"}`, `{"$edge":"<hex>"}`.

## Çıkış kodları
`0` başarı · `1` kullanıcı kaynaklı (iptal, bulunamadı, onay verilmedi) · `2` kullanım hatası ·
`3` G/Ç/iç hata · `4` senkron hatası. Hatalar stderr'e, i18n mesajıyla.

**Config uyarıları (T36):** `load_config()` uyarıları (bozuk `config.toml` -> tüm varsayılanlar, ayıklanan remote kimlik
bilgisi, geçersiz değer) her komutta stderr'e yazılır; stdout ve çıkış kodu değişmez. Tek istisna ayrık süreç
`sync --background`: tamamen sessiz kalır.

Argüman ayrıştırma elle yazılır (ek kütüphane yok). Windows'ta UTF-8 argümanlar `wmain`/`GetCommandLineW` ile alınır.

## Kabul kriterleri (alt süreç testleri, `NOTEAPP_HOME` geçici dizinde)

1. `note "merhaba dünya"` → `merhaba-dunya.md` oluşur, içerik birebir; ≤ 200 ms.
2. `printf 'public class A {}\nimport java.util.*;' | note` → `.java` dosyası; `-t txt` ile `.txt`.
3. `-f work/2026 -n plan "x"` → `work/2026/plan.md`; `-n ../kacis` → çıkış 2, dosya yok.
4. Editör akışı `fake-editor` ile: içerik yazılırsa not oluşur; boş bırakılırsa çıkış 1, dosya yok.
5. `note rm` fuzzy sorguyla silmez; TTY yokken `-y`'siz reddeder; `-y` ile siler.
6. `note ls mtg` ve `note find "proje bütçe"` SPEC-search sıralamasını izler; Türkçe katlama çalışır.
7. `note mv a.md a.java` tipi değiştirir; hedef varsa ezmez, çıkış 1.
8. `note sync login` token'ı ekrana yansıtmaz, kasaya yazar; config/logda token yok.
9. `note config set language en` sonrası mesajlar İngilizce.
10. ~~`notes-gui` yokken `note export --pdf` anlaşılır hata ve çıkış 3 verir.~~ **EMEKLİ (2026-10-31):** `note` artık
    `notes-gui` çağırmaz. Karşılığı: `note export --pdf` eşleşmesiz sorguda çıkış 1, `--pdf`'siz kullanım hatası,
    yazılamayan çıktıda çıkış 3 ve artık dosya yok (`tests/cli/cli_export_test.cpp`).

**Elle:** Windows Terminal ve Fedora'da `edit`/`micro`/`nano`/`vim` ile editör akışı; Türkçe karakterli argümanlar.

## Kapsam dışı
Kabuk tamamlama (bash/zsh/PowerShell), etkileşimli seçici (fzf benzeri seçim TUI'nin işi), `mkdir/rmdir` komutları
(klasör `-f`/`mv` ile örtük oluşur; klasör silme TUI/GUI'de).

## Değişiklik 2026-10-31
- **MODIFIED** PDF dışa aktarma: `note`, `noteapp_pdf`'e doğrudan link olur ve PDF'i süreç içinde üretir. Neden: GUI'siz
  derlemede ve ekransız sunucuda çalışsın (`tasks/plan-imgui.md` davranış 5).
- **REMOVED** `notes-gui --export-pdf` çağrısı, `QT_QPA_PLATFORM` ortamı, `NOTEAPP_GUI_EXE` test dikişi ve `cli.gui_not_installed` hatası.
- **REMOVED** Kabul 10 (`notes-gui` yokken çıkış 3): emekli, numara korundu; karşılığı kabul 10 satırında.
- **MODIFIED** PDF içeriği: yalnız soldan sağa basit metin; Markdown kapsamı yukarıda (davranış 6). Çıktı artık Qt yerine libharu + md4c ile üretilir.

