# Spec: detect

> Modül: `detect` · Bağımlılık: — · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Bir metnin içeriğine bakıp hangi dosya tipiyle (uzantı) saklanması gerektiğini **önermek**. Karar her zaman
kullanıcınındır; öneri yalnızca kaydet diyaloğunu/CLI varsayılanını doldurur. Saf, deterministik, G/Ç yok.

## Arayüz (taslak)

```cpp
namespace noteapp::detect {
struct Candidate  { std::string type; double score; };          // type: "md", "java", ...
struct Suggestion { std::string type; double confidence; std::vector<Candidate> ranked; bool detected; };
struct Options    { std::string default_type; double min_confidence; std::size_t max_scan_bytes; };

Suggestion suggest(std::string_view content, const Options& options);
}
```
`min_confidence` (0.5) ve `max_scan_bytes` (64 KB) `resources/defaults.toml`'dan gelir.

## Otomatik algılanan tipler (v1)

`md`, `txt`, `cpp`, `java`, `py`, `json`, `csv`, `sh`; 2026-11-01'den beri `env`, `properties`, `yaml`, `toml`, `ini`, `xml`, `html`, `css`, `js`, `ts`, `sql`, `c`, `go`, `rs`, `dockerfile`, `makefile` (bkz. sondaki Değişiklik). Diğer uzantılar yalnızca elle seçilir.

## Karar sırası

1. **Boş veya tek satır** → `default_type`, `detected = false` (hızlı notlar algılamaya girmez).
2. **Shebang** (`#!`) → `python` içeriyorsa `py`, değilse `sh`; güven 1.0.
3. **JSON:** kırpılmış metin `{…}` veya `[…]` ve yapısal denetimden (dengeli parantez/tırnak, anahtar
   `"…":` deseni) geçiyorsa `json`.
4. **CSV:** ≥ 2 satır; aynı ayırıcı (`,` `;` `\t`) satırların ≥ %90'ında aynı sayıda (≥ 1) geçiyor ve kod
   sinyali yok → `csv`.
5. **Puanlama:** her tip için ağırlıklı sinyaller toplanır, normalize edilip sıralanır:
   - `cpp`: `#include <`, `std::`, `::`, `template<`, `->`, `nullptr`, `int main(`
   - `java`: `public class`, `package x.y;`, `import java.`, `System.out.`, `@Override`, `public static void main`
   - `py`: `def x(`, `import x` / `from x import`, satır sonunda `:` + girinti bloğu, `self.`, `print(`
   - `sh`: `$VAR` / `${…}`, `fi`, `esac`, `then`, `echo`, `export X=`
   - `md`: `#`–`######` başlık, `- ` / `* ` / `1. ` listeler, ```` ``` ```` çitleri, `[x](y)`, `**x**`, `|` tabloları, `- [ ]`
   - Ayrım kuralları: `#include` → Java puanı sıfırlanır; `import java.` / `package …;` → C++ puanı sıfırlanır;
     çit (```` ``` ````) içindeki kod, kod tiplerine **sayılmaz** (Markdown içinde kod bloğu olan belge `md`'dir).
6. En yüksek puan `min_confidence` altındaysa: Markdown sinyali varsa `md`, yoksa `txt` (`detected = true`).
   `default_type` yalnız boş / tek dolu satırlı girdide döner (kural 1).
7. **Sınırlamalar (uygulamada netleşti):** Python `:` blok sinyali yalnız Python anahtar kelimesiyle başlayıp
   sonraki dolu satırı girintili satırlarda sayılır (Türkçe düz yazı korunur); tek `# ` başlığı en çok 1,5 puan
   (py/sh yorumları Markdown sayılmasın); CSV için ayrıca Markdown puanı < 3 ve ortalama alan uzunluğu ≤ 40.

Yalnızca ilk `max_scan_bytes` taranır; satır bazlı, regex motoru zorunlu değil (performans).

## Kabul kriterleri

1. **(değişti 2026-11-01)** `tests/detect/samples/<tip>/` korpusunda (tip başına ≥ 5 örnek + zor örnekler) ilk tahmin doğruluğu ≥ %90, tek tek her tipte ≥ %80.
2. Zorunlu doğru örnekler: içinde ```` ```cpp ```` bloğu olan Markdown → `md`; `#include` + `class` → `cpp`;
   `public class` + `import java.util.*;` → `java`; iki nokta ile biten Türkçe düz yazı satırları → `py` **değil**;
   virgüllü düz yazı paragrafı → `csv` **değil**; `#!/usr/bin/env bash` → `sh`.
3. Tek satırlık girdi her zaman `default_type`, `detected = false`.
4. 64 KB girdide `suggest` ≤ 5 ms (Release).
5. **(değişti 2026-11-01)** Aynı girdi her çağrıda aynı sonucu verir; `ranked` puana göre azalan, toplam aday sayısı = tüm tipler (24).

## Testler
Small: korpus tablosu testi (yüzde raporu + eşik), zorunlu örnekler, eşik/varsayılan davranışı, performans testi (Release'te).

## Kapsam dışı
Makine öğrenmesi tabanlı algılama (Magika/guesslang), dil sürümü ayrımı (C++17 vs C++23), kodlama (encoding) tespiti.

## Değişiklik 2026-11-01

Yeni notta (`n`) ad önerisi ve canlı vurgu için algılanan tip sayısı arttı (T58). Tip adları uzantıdır: `js`, `ts`, `rs`.

- **ADDED** Tipler: `env`, `properties`, `yaml`, `toml`, `ini`, `xml`, `html`, `css`, `js`, `ts`, `sql`, `c`, `go`, `rs`, `dockerfile`, `makefile`. `resources/defaults.toml` `types` listesine de eklendi. `ranked` artık 24 adayın tamamıdır.
- **ADDED** Karar sırasına 3b (`src/detect/structure.cpp`): JSON'dan sonra, CSV'den önce satır biçimi kesin karar verir, puanlama beklenmez: tek bir komut satırı kararı bozmalıdır (`A=1` + `echo` → `sh`), ortalama puan bunu göremezdi.
  - **işaretleme:** ilk etiket; `<?xml` → `xml`, `<!doctype html` ve bilinen HTML etiketi → `html`, diğer etiket → `xml`; son satır `>` ile bitmeli.
  - **dockerfile:** devam satırı ve yorum dışındaki her satır bir Dockerfile komutuyla başlar ve `FROM` geçer.
  - **env:** tüm satırlar `BÜYÜK_AD=değer` ya da `export AD=değer`, bölüm yok, değerde `$(...)` yok.
  - **toml:** bütün değerler TOML değişmezi (tırnak, sayı, boole, dizi); çok satırlı dizge ve dizi atlanır.
  - **ini:** `[bölüm]` ya da `;` yorumu, en az bir anahtar.
  - **properties:** bölüm yok; noktalı anahtar (`a.b.c=`, `a.b: x`) ya da en az iki düz `a=b`. Küçük harfli anahtarda `$x` kabuk betiğidir, properties değil.
  - **yaml:** satırların ≥ %80'i `anahtar:` ya da `- öğe`; `=`, bölüm ve sekme girintisi yok. Düz yazı koruması: anahtar küçük harfli olmalı (`Not: x` değil), cümle uzunluğunda değer sayılmaz, `## başlık` belge başlığıdır; `---`, iç içe anahtar, liste, sayı/boole değer ya da `a_b` yoksa en az 3 anahtar gerekir.
- **ADDED** Puanlama: `js`, `ts`, `c`, `go`, `rs`, `sql` (büyük/küçük harf ayırmaz), `css`, `makefile` sinyalleri. Aile kuralları (`resolve_families`): yalnız C++'ta olan sözdizimi (`std::`, şablon, uzantısız `#include <x>`) ya da `class` C'yi sıfırlar, yoksa C sinyali varsa C++ puanı C'ye geçer; `ts` sinyali `js` puanını devralır; `func`/`package x` Go, `fn`/`impl`/`use std::` Rust, Java işaretleri komşu aileleri sıfırlar.
- **MODIFIED** Kabul 1: tip başına ≥ %80 ve toplam ≥ %90. Kabul 5: aday sayısı = tüm tipler (24). Korpus: 24 tip, tip başına ≥ 5 örnek, zor negatifler (iki noktalı düz yazı, `=` içeren düz yazı, `SELECT` diye başlayan İngilizce, env/properties/ini/kabuk).
- **MODIFIED** Eski `config.toml`'daki kısa `types` listesi öneriyi etkilemez: detect listeye bakmaz; TUI kaydet diyaloğu ve GUI tip alanı listede olmayan öneriyi tutar, `←/→` listeye girer.
- **REMOVED** Kapsam dışından "C vs C++ ayrımı": artık yapılır (dil sürümü ayrımı hâlâ yok).
- **Bilinen sınır:** dotenv `.env` dosya adı nokta ile başladığından yol kurallarınca reddedilir; `env` içerik `<ad>.env` önerilir.
