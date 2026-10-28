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

`md`, `txt`, `cpp`, `java`, `py`, `json`, `csv`, `sh`. Diğer uzantılar (ör. `sql`, `yaml`) yalnızca elle seçilir.

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

1. `tests/detect/samples/<tip>/` korpusunda (tip başına ≥ 5 örnek + zor örnekler) ilk tahmin doğruluğu ≥ %90.
2. Zorunlu doğru örnekler: içinde ```` ```cpp ```` bloğu olan Markdown → `md`; `#include` + `class` → `cpp`;
   `public class` + `import java.util.*;` → `java`; iki nokta ile biten Türkçe düz yazı satırları → `py` **değil**;
   virgüllü düz yazı paragrafı → `csv` **değil**; `#!/usr/bin/env bash` → `sh`.
3. Tek satırlık girdi her zaman `default_type`, `detected = false`.
4. 64 KB girdide `suggest` ≤ 5 ms (Release).
5. Aynı girdi her çağrıda aynı sonucu verir; `ranked` puana göre azalan, toplam aday sayısı = 8.

## Testler
Small: korpus tablosu testi (yüzde raporu + eşik), zorunlu örnekler, eşik/varsayılan davranışı, performans testi (Release'te).

## Kapsam dışı
Makine öğrenmesi tabanlı algılama (Magika/guesslang), dil sürümü ayrımı (C vs C++), kodlama (encoding) tespiti.
