# Spec: search

> Modül: `search` · Bağımlılık: core · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

fzf hissiyle notlar arası anında geçiş (ad/yol üzerinde fuzzy) ve hızlı tam metin içerik araması.
v1'de indeks yok: bellekte içerik önbelleği + doğrusal tarama (birkaç bin not için onlarca ms).

## Arayüz (taslak)

```cpp
namespace noteapp::search {
struct FuzzyMatch { double score; std::vector<std::size_t> positions; };   // vurgulama için bayt ofsetleri
std::optional<FuzzyMatch> fuzzy_match(std::string_view query, std::string_view candidate);
std::vector<Ranked> rank_notes(std::string_view query, std::span<const core::NoteInfo> notes);

struct Hit { std::size_t line; std::string snippet; std::vector<std::pair<std::size_t, std::size_t>> ranges; };
struct ContentResult { core::NotePath path; std::size_t hit_count; std::vector<Hit> first_hits; };

class ContentIndex {                       // bellek içi önbellek; G/Ç yalnız refresh'te
public:
    explicit ContentIndex(const core::NoteStore& store);
    void refresh();                         // stat; yalnız mtime/boyutu değişenleri yeniden okur
    std::vector<ContentResult> search(std::string_view query, std::size_t limit) const;
};
std::string fold(std::string_view text);   // eşleştirme için katlama (aşağıda)
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

1. `fuzzy_match("wmn", "work/meeting-notes.md")` eşleşir; `"mtg"` sorgusunda `meeting.md`,
   `my-long-text-generator.md`'den yüksek puan alır (kelime başı bonusu).
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
