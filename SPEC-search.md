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
- Sonuç: not başına eşleşen satır sayısı + ilk `limits.search.content_hits` eşleşme (varsayılan 3, `0` hepsi; satır no,
  ~120 karakterlik kesit, vurgu aralıkları). `ContentIndex(store, max_note_bytes, max_hits)`.
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
  | `view:` | kayıtlı görünümün döndürdüğü notlar (T81, SPEC-graph "Görünümler"): `view:iş`, boşluklu ad `view:"ev notları"`. Ad katlanmadan core'a gider, eşleşme `tag_key` katlamalıdır (`view:İŞ` = `iş`). Önce ad kuralı (bozuk ad depoya sorulmaz), sonra görünüm salt okunur çalışır. Bilinmeyen görünüm, yazan görünüm, not döndürmeyen sonuç ve sorgu hatası diğer bozuk değerler gibi değerin yerinde `QueryError`dır (ileti core'un). `parse_query` görünüm araması (`ViewLookup`) verilmeden çağrılırsa (girdi boyama) yalnız ad kuralı denetlenir, süzgeç hiçbir notu geçirmez |
  | `tag:` | etiket ve altındakiler (T80): `tag:proje` `proje`, `proje/alt` eşleşir, `projeler` eşleşmez; `tag:proje/alt` yalnız o dal. `#` öneki ve sondaki `/` yok sayılır (`tag:#proje/` = `tag:proje`); katlanmış (`tag:calisma` `#çalışma`yı bulur). Kaynak `NoteMeta::tags` (SPEC-graph "Etiketler"); çöpteki not girmez |

  - Kalan sözcükler `text`'tir, tırnaklarıyla: `parse_terms` aynı metni görür. Değer tırnaklanabilir (`in:"iş notları/"`); tamamı tırnaklı
    sözcük anahtar değildir. Anahtar ve değerler katlama kurallarıyla karşılaştırılır (`SORT:-SİZE`).
  - Bilinmeyen anahtar (`foo:bar`, `http://x`) düz metindir. Bilinen anahtarın geçersiz değeri metne düşmez: `QueryError {begin, end, error}`,
    `[begin, end)` değerin bayt aralığı (boş değerde tüm sözcük), `error` `format_error` ile gösterilir.
  - Filtreler AND'dir, aynı anahtar da (`size:>1kb size:<2kb` aralıktır, `type:md type:txt` boş döner). Bilinmeyen `created`/`origin` filtreye takılır;
    `sort:created`'da bilinmeyenler iki yönde de sondadır.
  - `apply` argümanlarına göre saftır; tek ortam girdisi saat dilimidir (`mktime`, gün sınırı kullanıcının günü olsun diye).
- **MODIFIED (L3)** Sonuç sayısı ve ad eşleşmesinin aday tavanı config'tedir: `search_all(query, notes, content_index,
  core::SearchLimits, now, views)` `limits.search.global_results` (200, `0` hepsi) ve `fuzzy_candidate_chars` (1024,
  `0` sınırsız) okur. Sınırsız sonuçta her içerik eşleşmesi için en iyi satır üretilir (sonuç başına bir kesit).
  `fuzzy_match`/`rank_notes` aday tavanını parametre alır; uzun aday eşleşir, konumsuz ve düşük puanlı döner.
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

## Değişiklik 2026-11-06 — İlişki araması
- **ADDED** Arama yalnız ad ve içerik değildir: çizgedeki ilişkiler de eşleşir (`search/relation_search.hpp`,
  `relation_hits(text, notes, allowed, GraphInput{edges, depth})`). Metnin katlanmış terimlerinin **hepsi** alt dizgi olarak aynı yerde geçmeli:
  | Neden | Eşleşen | Gösterim (`app::reason_text`) |
  |---|---|---|
  | etiket | notun bir etiketi (`NoteMeta::tags`) | `etiket: #proje/alt` |
  | ilişki | notun bir RELATED_TO kenarının adı; iki uç da gelir, yön notun gözünden | `ilişki: uses → b.md` (`←` gelen, `—` yönsüz) |
  | bağlı | `depth` adım içindeki komşunun yolu (LINKS_TO ve RELATED_TO, iki yön) | `bağlı: b.md (uses)`; uzakta `bağlı: c.md, 2 adım` |
  - Not başına en güçlü neden: etiket → ilişki → bağlı (az adım önce). Adı eşleşen notun kendisi (0 adım) ad eşleşmesidir, "bağlı" olmaz.
  - **Doğrudan eşleşme önce:** ad ya da içerikte bulunan not ilişkiyle yinelenmez, nedeni gösterilmez. İlişki satırları her modda (ad, içerik,
    genel; `note find`) doğrudan satırların altındadır; `sort:` iki grubu ayrı sıralar. Genel aramada sınırı (`global_results`) önce onlar bırakır.
    İlişki satırlarının sırası neden → adım → en yeni.
  - Eşleşen komşu süzgeç ve kapsam dışında da olabilir; sonuç süzgeçten geçen notlardır.
  - Kenarlar arama başına bir kez gelir: `NoteStore::shared_edges()` görüntü başına bir kez okunur ve paylaşılır (kendi yazma ve başka
    bağlantının yazması görüntüyü düşürür); satır başına sorgu yok. Komşu gezintisi adı eşleşen tüm notlardan aynı anda genişlik önceliklidir,
    iş not + kenar sayısıyla doğrusaldır.
- **ADDED** Config: `limits.search.relation_depth` (varsayılan `1`, `0` sınırsız: bağlı bileşenin tamamı) ve `[tuning] search_relations`
  (`true`; `false` ilişki satırlarını kapatır, `rel:`/`linked:` süzmeye devam eder). Ön yüzler `app::relation_depth(config)` ile okur.
- **ADDED** Sorgu anahtarları (`query_key_names`, tamamlama ve girdi boyama aynı listeyi görür):
  | Argüman | Biçim |
  |---|---|
  | `rel:` | adı tam bu olan (katlanmış) ilişkinin iki ucu: `rel:uses`, `rel:"depends on"` |
  | `linked:` | notun komşuları (bağlantı ya da ilişki, iki yön); not yol, uzantısız yol, dosya adı ya da uzantısız adla verilir: `linked:work/b.md` = `linked:b` |
  - Notları `bind_graph_filters(filters, edges)` kurar (`view:` gibi `Filter::notes`); bağlanmamış süzgeç hiçbir notu geçirmez.
- **MODIFIED** `search_all(query, notes, content_index, limits, views = {}, graph = {})`: `GraphInput` verilir; `now` parametresi kalktı
  (her çağıran şimdiyi veriyordu; tarih süzgeçleri şimdiye göredir). `GlobalHit::reason` yalnız
  ilişkiyle gelen sonuçta doludur. `app::GlobalSearcher` üçüncü parametre olarak `GraphInput` alır (`Listing` kenarları ve derinliği geçirir).
- **ADDED** Kabul: aynı bütçe (5.000 × ~5 KB, sıcak `search_all` ≤ 100 ms) not başına bir bağlantı + bir adlı ilişki (10.000 kenar) ve
  etiketlerle; TUI tuş başına süzme + çizim ≤ 50 ms aynı kenarlarla (`tests/perf/query_perf_test.cpp`, `perf_test.cpp`).
  Ölçüm (konteyner, Release): `search_all` 19–31 ms (ilişki kapalı `proje bütçe` 21,6 ms, açık 24,3 ms; sınırsız derinlik 21,6 ms;
  en kötü `e` 30,5 ms); TUI tuş başına en kötü 15,0 ms; GUI model 6,1 ms.
- **ADDED** Kenar dizini (`search::RelationGraph`: düğüm yolları ve katlanmışları, komşuluk, ad -> kenarlar) kenar listesi başına bir kez
  kurulur; `app::Listing` görüntü değişene dek tuş başına yeniden kullanır (`GraphInput::index`, notların düğümleri `GraphInput::nodes`).
  Komşu gezintisi izinli hedeflerin hepsine ulaşınca durur. Ölçüm (Release, 10^6 kenar, tuş başına ad araması): 5.000 notta
  1,4–13,7 ms (önbelleksiz 159–205 ms); 50.000 notta 30–66 ms, her notu listeleyen sorguda ilişkisiz taban 20 ms (bütçe 5.000 not içindir).

## Değişiklik 2026-11-06 — `prop:` (F4)
- **ADDED** `prop:anahtar` (özelliği taşıyan notlar) ve `prop:anahtar=değer` (değer metni katlanarak tam eşit: `prop:durum=bitti` "Bitti"yi bulur, "bitti mi"yi bulmaz). Anahtar tam yazılır (graf anahtarı büyük/küçük harf ayırır); boşluklu değer tırnakla: `prop:"durum=devam ediyor"`. Boş anahtar `must_be_property`. Değer metni `core::property_text`'tir (`true`, `3`, `2.5`).
- Eşleme **metinseldir**: değer `core::property_text` biçimiyle karşılaştırılır, sayı olarak değil (`prop:fiyat=2.5` saklı `2.5`'i bulur, `2.50` yazmak bulmaz; `prop:n=1` tamsayı `1`'i bulur, ondalık `1.0`'ı bulmaz). Büyük değer (motorun ayrı sakladığı) yüklenmez, metni `<N bytes>`'tır: `prop:k` eşleşir, `prop:k=…` eşleşmez.
- Önbellek: görüntü başına en son kullanılan `limits.search.property_keys` (32; `0` sınırsız) anahtar; yazdıkça her kısmi anahtar bir girdidir, en eskisi düşer.
- Notları `bind_graph_filters` kurar, kenar istemez (`GraphInput::properties`, ön yüzde `app::search_properties` → `core::notes_with_property`; görüntü başına anahtar başına tek okuma). Bağlanmamış süzgeç hiçbir notu geçirmez (`view:` kuralı). Uygulamanın anahtarları (`type`, `created_at`…) aranmaz: kendi süzgeçleri var.
- **Kapsam dışı (şimdilik):** ilişki aramasında kenar özelliklerinin değerleri; `NoteEdge` özellik taşımaz, her aramada tüm kenar özelliklerini okumak bütçeyi aşardı. Cypher (`MATCH ()-[r:RELATED_TO]->() WHERE r.weight > 1`) bunu karşılar.
