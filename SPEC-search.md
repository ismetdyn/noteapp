# Spec: search

> Modül: `search` · Bağımlılık: core · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

fzf hissiyle notlar arası anında geçiş (ad/yol üzerinde fuzzy) ve hızlı tam metin içerik araması.
v1'de indeks yok: bellekte içerik önbelleği + doğrusal tarama (birkaç bin not için onlarca ms).

## Arayüz (taslak)

```cpp
namespace noteapp::search {
struct FuzzyMatch { double score; std::vector<std::size_t> positions; };   // vurgulama için özgün bayt ofsetleri
struct Ranked { std::size_t index; FuzzyMatch match; };                      // index = notes içindeki sıra
std::optional<FuzzyMatch> fuzzy_match(std::string_view query, std::string_view candidate);
std::vector<Ranked> rank_notes(std::string_view query, std::span<const core::NoteInfo> notes);

struct Hit { std::size_t line; std::string snippet; std::vector<std::pair<std::size_t, std::size_t>> ranges; };
struct ContentResult { core::NotePath path; std::size_t hit_count; std::vector<Hit> first_hits; };

class ContentIndex {                       // bellek içi önbellek; G/Ç yalnız refresh'te
public:
    ContentIndex(const core::NoteStore& store, std::size_t max_note_bytes);
    void refresh();                         // stat; yalnız mtime/boyutu değişenleri yeniden okur
    std::vector<ContentResult> search(std::string_view query, std::size_t limit) const;
    std::size_t read_count() const;         // test kancası: şimdiye kadar (yeniden) okunan dosya sayısı
};
struct Folded { std::u32string original, text; std::vector<std::size_t> byte_offsets; };   // kod noktası başına 1:1\r
Folded fold(std::string_view utf8);          // eşleştirme için katlama (aşağıda); geçersiz bayt -> U+FFFD\r
std::string fold_to_utf8(std::string_view);   // yalnız katlanmış metin
}
```

## Davranış

### Katlama (fuzzy ve içerik aramasında ortak)
UTF-8 kod noktası bazında: büyük/küçük harf duyarsız; Türkçe için `I İ ı i` → `i`, `ç→c ğ→g ö→o ş→s ü→u`
(büyük hâlleri dahil). Böylece `ogrenci` sorgusu `Öğrenci`'yi, `istanbul` `İSTANBUL`'u bulur. Vurgulama
konumları orijinal metnin bayt ofsetlerine geri eşlenir.

### Fuzzy (ad/yol)
- Aday metin: göreli yol + uzantı (`work/meeting-notes.md`), fzf gibi.
- Algoritma: fzy (MIT) yaklaşımı — sorgu alt dizi olarak geçmeli; dinamik programlama ile en iyi hizalama;
  bonus: dizgi başı, `/` sonrası (en yüksek), `-` `_` `.` boşluk sonrası, camelCase sınırı, ardışık eşleşme;
  ceza: boşluk açma/uzatma.
- Sıralama: puan azalan → eşitlikte kısa yol → mtime azalan. Boş sorgu: tüm notlar mtime azalan.

### İçerik araması
- Sorgu boşlukla terimlere bölünür; **tüm terimler aynı notta** geçmeli (AND, satır şartı yok).
  `"tırnaklı ifade"` tam ifade olarak aranır.
- Sonuç: not başına eşleşen satır sayısı + ilk 3 eşleşme (satır no, ~120 karakterlik kesit, vurgu aralıkları).
- Sıralama: eşleşme sayısı azalan → mtime azalan. `limit` uygulanır.
- `binary` notlar ve `max_note_bytes` (defaults.toml, 10 MB) üstü notlar atlanır.

### Eşzamanlılık
`ContentIndex` tek iş parçacığından kullanılır (`refresh` ve `search` aynı anda çağrılmaz); ön yüzler buna uyar.
SQLite FTS5'e geçiş gerekirse dikiş yeri `ContentIndex`'tir (şimdiden soyut arayüz yazılmaz).

## Kabul kriterleri

1. `fuzzy_match("wmn", "work/meeting-notes.md")` eşleşir (konumlar 0, 5, 13); fzf/fzy gibi, `"mtg"` sorgusunda
   kelime başlarına düşen `my-long-text-generator.md`, kelime içi eşleşen `meeting.md`'den yüksek puan alır
   (puanlama sabitleri fzy'den sapmasız; 2026-10-28 düzeltmesi — önceki örnek fzf davranışıyla çelişiyordu).
2. `/` sonrası eşleşme ortadaki eşleşmeden yüksek puanlıdır; alt dizi olmayan sorgu `nullopt` döner.
3. Katlama: `ogrenci`→`Öğrenci.md`, `ISIK`→`ışık.txt`, `şehir`→`SEHIR.md` eşleşir; vurgu ofsetleri doğru karakteri gösterir.
4. İçerik: `"proje bütçe"` iki kelimeyi farklı satırlarda içeren notu bulur; yalnız birini içereni bulmaz;
   tırnaklı ifade yalnız bitişik geçişi bulur.
5. 5.000 not × 5 KB: `rank_notes` ≤ 20 ms, sıcak `search` ≤ 100 ms, değişiklik yokken `refresh` ≤ 100 ms (Release).
6. `refresh` yalnız değişen dosyaları yeniden okur (test: okuma sayacı); silinen not sonuçlardan düşer.

## Testler
Small: katlama tablosu, fuzzy puan sıralama örnekleri, terim ayrıştırma, vurgu ofsetleri.
Medium: geçici dizinde ContentIndex (refresh/silme/ikili dosya); performans testi Release'te, üretilmiş 5.000 notla.

## Kapsam dışı
Kalıcı indeks, regex araması, fzf genişletilmiş sözdizimi (`^`, `$`, `!`, `'`), bulanık içerik araması.

## Değişiklik 2026-11-01
- **ADDED** Sorgu argümanları (T55a), `search/query.hpp`: saf `parse_query(string) -> expected<ParsedQuery{text, filters, sort}, QueryError>`,
  `apply(filters, sort, notes, now = now()) -> vector<size_t>` (`notes` sıraları, kararlı), `sort_notes`.

  | Argüman | Biçim |
  |---|---|
  | `sort:` | `name` (dosya adı, katlanmış) `size` `updated` `created`; `-` öneki azalan; birden çoksa sonuncusu |
  | `type:` | son uzantı, noktasız (`type:md`) |
  | `in:` | klasör öneki, tam bileşen (`in:work/` = `in:work`; `workshop/` eşleşmez); değer `NotePath` kuralından geçer, `.trash` reddedilir |
  | `size:` | `>` ya da `<` zorunlu, kesin sınır; tam sayı + `b kb mb gb` (1024 tabanlı), birimsiz = bayt |
  | `updated:` `created:` | `7d` son 7 gün (1–36500); `>YYYY-MM-DD` o günden sonra, `<YYYY-MM-DD` o günden önce (gün yerel saattir, 1970–2200) |
  | `origin:` | tam eşleşme, katlanmış |
  | `sync:` | `synced modified new conflict_copy sync_off` |

  - Kalan sözcükler `text`'tir, tırnaklarıyla: `parse_terms` aynı metni görür. Değer tırnaklanabilir (`in:"iş notları/"`); tamamı tırnaklı
    sözcük anahtar değildir. Anahtar ve değerler katlama kurallarıyla karşılaştırılır (`SORT:-SİZE`).
  - Bilinmeyen anahtar (`foo:bar`, `http://x`) düz metindir. Bilinen anahtarın geçersiz değeri metne düşmez: `QueryError {begin, end, error}`,
    `[begin, end)` değerin bayt aralığı (boş değerde tüm sözcük), `error` `format_error` ile gösterilir.
  - Filtreler AND'dir, aynı anahtar da (`size:>1kb size:<2kb` aralıktır, `type:md type:txt` boş döner). Bilinmeyen `created`/`origin` filtreye takılır;
    `sort:created`'da bilinmeyenler iki yönde de sondadır.
  - `apply` argümanlarına göre saftır; tek ortam girdisi saat dilimidir (`mktime`, gün sınırı kullanıcının günü olsun diye).
- **ADDED** Genel arama (T55a), `search/global_search.hpp`: `search_all(query, notes, content_index, limit = 200, now) ->
  expected<vector<GlobalHit{index, path, name_matched, content_matched, name_positions, content_hits, best_line}>, QueryError>`.
  - Not başına tek sonuç. Argümanlar önce süzer; metnin her terimi yolda fuzzy (puanlar toplanır, konumlar birleşir) **ya da** tümü notta tam
    metin (mevcut AND kuralı) geçmelidir: terimin biri adda biri içerikte olan not eşleşmez.
  - Sıra: güçlü ad (karakter başına fzy puanı ≥ 0,5; puan → kısa yol → mtime) → içerik (eşleşen satır → mtime) → zayıf ad. `sort:` verilirse o sıra,
    eşitlikte bu sıra. Metin yoksa süzülmüş liste, eşleşme bilgisi yok.
  - Limit güçlü adlarla dolacaksa `limit/4` içerik ve zayıf sonuçlara ayrılır, kullanılmayan pay adlara kalır. Kesit yalnız dönen sonuçlar için üretilir.
  - `best_line`: en çok farklı terim → en çok geçiş → en erken satır. `ContentIndex`'e `count_matches` ve `best_hit` eklendi; `search` davranışı aynı.
- **ADDED** Kabul: 5.000 notta `apply` ≤ 20 ms; 5.000 × ~5 KB notta sıcak `search_all` ve değişiklik yokken `refresh` + `list_meta` ayrı ayrı ≤ 100 ms
  (`tests/perf/query_perf_test.cpp`, Release). Ölçüm (T55a, konteyner): `apply` 1,3 ms; `search_all` 18–31 ms (en kötü tek harf `e`, 200 kesit); `refresh` + `list_meta` 47 ms.
