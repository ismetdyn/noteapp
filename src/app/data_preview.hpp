#pragma once

#include "core/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::app {

// Notun biçimli görünümü (TUI REVIEW, GUI önizleme); uzantıdan belirlenir, None ise yalnız ham metin vardır.
enum class PreviewKind { None, Markdown, Csv, Json };
PreviewKind preview_kind_for(std::string_view extension);  // büyük/küçük harf duyarsız, noktasız

// limits.tui.table_rows ve table_columns: iki arayüzde aynı anahtarlar (varsayılanlar defaults.toml ile
// aynı).
struct TableLimits {
    std::size_t rows = std::numeric_limits<std::size_t>::max();
    std::size_t columns = 128;

    bool operator==(const TableLimits&) const = default;
};

// Ayrıştırılmış CSV: hücreler kaynaktaki baytlardır (görüntü arındırması arayüzün işi), ilk satır başlıktır.
struct CsvTable {
    // En çok limits.rows satır, satırda en çok limits.columns hücre.
    std::vector<std::vector<std::string>> rows;
    std::vector<std::size_t> row_offsets;  // satırın kaynaktaki (BOM dahil) bayt ofseti
    std::size_t column_count = 0;          // kaynaktaki en geniş satır; saklanandan fazla olabilir
    std::size_t omitted_rows = 0;          // limits.rows'u aşan satırlar
};

// Ayırıcı `detect` kuralıyla bulunur; tutarlı tablo değilse nullopt. RFC 4180'e yakın: tırnaklı alanda
// ayırıcı ve satır sonu metindir, "" tek tırnaktır; bozuk tırnak hata değildir, metin görünür kalır. Boş
// satır atlanır.
std::optional<CsvTable> parse_csv(std::string_view source, const TableLimits& limits = {});

// Büyük CSV parça parça: satır dizini tek geçişte (hücre kurulmaz); görünen satırların hücreleri csv_rows ile.
// parse_csv bunun üstündedir (aynı kural). Kontrol noktası kCsvBlockRows satırda ya da kCsvCheckpointBytes baytta bir
// (hangisi önce gelirse; 10 milyon kısa satırda ~2,5 MB, satır başına ofset ~80 MB olurdu): istenen satıra en yakın
// noktadan en çok kCsvBlockRows satır ya da ~kCsvCheckpointBytes bayt (araya giren boş satırlar dahil) taranır, kare
// işi notla değil görünenle orantılıdır. İkisi de kapasite sınırı ya da kullanıcı ayarı değil, dizinin iç tanesidir
// (hiçbir girdiyi kesmez ya da reddetmez).
inline constexpr std::size_t kCsvBlockRows = 64;
inline constexpr std::size_t kCsvCheckpointBytes = std::size_t{64} * 1024;
struct CsvCheckpoint {
    std::size_t row = 0;     // bu noktadan sonraki ilk boş olmayan satırın sırası
    std::size_t offset = 0;  // fiziksel satır başı (BOM dahil kaynakta; boş satır olabilir)
};
struct CsvIndex {
    char delimiter = ',';
    std::size_t row_count = 0;                // boş olmayan satırlar
    std::vector<CsvCheckpoint> checkpoints;   // satıra göre azalmayan; ilki satır 0
    std::size_t column_count = 0;             // en geniş satırın alan sayısı
};
std::optional<CsvIndex> index_csv(std::string_view source);  // tablo değilse nullopt
struct CsvRow {
    std::size_t offset = 0;  // satırın kaynaktaki (BOM dahil) başı
    std::vector<std::string> cells;
};
// [first, first + count) satırları (sona taşan düşer), satır başına en çok `max_columns` hücre.
std::vector<CsvRow> csv_rows(std::string_view source, const CsvIndex& index, std::size_t first, std::size_t count,
                             std::size_t max_columns = std::numeric_limits<std::size_t>::max());

// Girintilenmiş JSON'un bir satırı: `depth` iç içelik, `text` boşluksuz belirteçler (`"ad": "x",`), `offset`
// satırın ilk belirtecinin kaynaktaki (BOM dahil) bayt ofseti.
struct JsonLine {
    std::size_t depth = 0;
    std::string text;
    std::size_t offset = 0;
};

// Katı RFC 8259: geçersiz JSON nullopt. Özyinelemesiz, iç içelik sınırı yok; bütün satırları kurar (küçük not ve
// test). Büyük not index_json + json_lines ile yalnız görünen satırları kurar.
std::optional<std::vector<JsonLine>> pretty_json(std::string_view source);

// JSON'un satır dizini: tek geçişte doğrulama ve satır sayımı, metin kurulmadan. Kontrol noktası her
// kJsonCheckpointLines satırda bir (satır, kaynak ofseti, iç içelik): görünen pencere en yakın noktadan üretilir.
// Bellek satır başına değil nokta başınadır; iç içelik yalnız sayıdır (derin not da küçük).
inline constexpr std::size_t kJsonCheckpointLines = 256;
// Satır sayısından önce bu kadar bayt geçerse de nokta konur (belirteçler arası dev boşlukta pencere her karede o
// boşluğu taramasın); nokta sıradaki belirtecin üstündedir. İç tanedir, sınır değil.
inline constexpr std::size_t kJsonCheckpointBytes = std::size_t{64} * 1024;
struct JsonIndex {
    struct Checkpoint {
        std::size_t line = 0;   // bu noktada başlayan satır
        std::size_t pos = 0;    // kaynakta (BOM dahil) satırın ilk belirteci ya da öncesindeki boşluk
        std::size_t depth = 0;  // o andaki iç içelik
    };
    std::size_t line_count = 0;
    std::vector<Checkpoint> checkpoints;  // satıra göre artan; ilki satır 0
};
std::optional<JsonIndex> index_json(std::string_view source);  // geçersizse nullopt (pretty_json ile aynı kural)
// [first, first + count) satırları; pretty_json'un aynı satırlarıyla birebir aynı (sona taşan kısım düşer).
std::vector<JsonLine> json_lines(std::string_view source, const JsonIndex& index, std::size_t first,
                                 std::size_t count);

// Büyük markdown notunun bağımsız biçimlenen parçası: [begin, end) bayt aralığı, `first_line` başladığı kaynak
// satırı (0 tabanlı).
struct MarkdownChunk {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t first_line = 0;
    bool open_fence = false;  // kapanmamış kuşatılmış kodla biter (son parça ya da kodun içinden zorunlu kesim)
    // Zorunlu kesimde parçanın önüne (ardına satır sonu eklenerek) konan sınırlı önek [carry_begin, carry_end) (notun
    // baytları): kodun açılış çiti ve dil sözcüğü ya da küçük tablonun başlık ve ayraç satırı; parça yine kod ya da
    // tablo olarak biçimlenir (tablo başlığı her parçada yinelenir). `carry_lines` bu satırların sayısı (satır
    // eşlemesi kayar).
    std::size_t carry_begin = 0;
    std::size_t carry_end = 0;
    std::size_t carry_lines = 0;

    bool operator==(const MarkdownChunk&) const = default;
};

// İki arayüz büyük notu parça parça biçimler: yalnız görünen parça ayrıştırılıp yerleştirilir. Kapasite sınırı
// değil iş birimidir; bundan küçük not tek parçadır, çıktısı bütün biçimlemeyle aynıdır.
inline constexpr std::size_t kMarkdownChunkBytes = std::size_t{64} * 1024;
// Kesim yeri bulunmayan tek dev blok (uzun paragraf, kod, liste, tablo, tek satır) bu boyda zorla kesilir: önce satır
// başında, satır bundan uzunsa satırın içinde (son boşlukta, yoksa kod noktası sınırında). Bedeli: kesimde paragraf
// ikiye ayrılır (satır içi biçim ve bağlantı kesimi aşmaz), sıralı liste parçada numarasını kaynaktan alır.
inline constexpr std::size_t kMarkdownMaxChunkBytes = 2 * kMarkdownChunkBytes;
// Parçalamanın düşman girdi korumaları; değerler config'ten (limits.tui.preview_*, markdown_chunk_limits_of), üye
// varsayılanları defaults.toml ile aynı (test denetler); sınırsız SIZE_MAX.
//   definition_bytes: parçaya eklenen başka parçadaki tanım satırlarının iş bütçesi (markdown_chunk_source): her
//     parça bütün tanım tablosunu ansa da toplam iş not boyuyla doğrusal kalır; aşan başvuru çözülmez, metin kalır.
//   fence_info_bytes: zorla kesilen koda taşınan açılış satırı önekinin (girinti, im, çit, dil sözcüğü) en çok boyu;
//     sözcükle aşan önekten yalnız çit, çit de aşarsa hiçbir şey taşınmaz.
//   table_header_bytes: zorla kesilen tabloya taşınan başlık ve ayraç satırının en çok boyu (büyüğü taşınmaz).
struct MarkdownChunkLimits {
    std::size_t definition_bytes = std::size_t{128} * 1024;
    std::size_t fence_info_bytes = 64;
    std::size_t table_header_bytes = std::size_t{4} * 1024;

    bool operator==(const MarkdownChunkLimits&) const = default;
};
MarkdownChunkLimits markdown_chunk_limits_of(const core::TuiLimits& limits);

// Kesim yalnız bloğu kapatan yerde: boş satırdan sonra sütun 0'da liste imi, alıntı ya da girintiyle
// başlamayan satırın önü, kuşatılmış kodun dışında (liste, alıntı, tablo, paragraf orada biter; md4c
// MD_FLAG_NOHTML ile HTML bloğu yok). Parça en az `target_bytes`tır; kesim yeri yoksa `max_bytes`ta zorla kesilir
// (kMarkdownMaxChunkBytes).
// Bağlantı başvuru tanımları (`[ad]: url`) bütün notta geçerlidir: markdown_link_definitions ve
// markdown_chunk_source ile her parçaya taşınır.
std::vector<MarkdownChunk> markdown_chunks(std::string_view text, std::size_t target_bytes = kMarkdownChunkBytes,
                                           std::size_t max_bytes = kMarkdownMaxChunkBytes);

// Notun kuşatılmış kod dışındaki tek satırlık bağlantı başvuru tanımları. md4c'nin bütün belgeden tuttuğu tek
// bağlam budur (dipnot bayrağı yok, liste numarası parçaya bölünmez). Satır yalnız boş satırdan ya da başka bir
// tanımdan sonra tanımdır (paragraf devamı değil). Alıntı ya da liste imli satırdaki ve çok satırlı tanım alınmaz:
// yalnız kendi parçasında çözülür.
struct MarkdownDefinition {
    std::uint64_t hash = 0;  // normalleşmiş etiketin özeti
    std::size_t begin = 0;   // tanım satırı [begin, end), notun baytları
    std::size_t end = 0;

    bool operator==(const MarkdownDefinition&) const = default;
};
struct MarkdownDefinitions {
    // Her etiketin ilk tanımı; özete göre sıralı tek dizi (aranır, düğüm ve anahtar ayrılmaz: 190 bin tanımda
    // eşlem tablosu ilk kareye ~70 ms ekliyordu).
    std::vector<MarkdownDefinition> first;
    // Daha önce tanımlanmış etiketin sonraki tanım satırları [begin, end), sıralı: parçada boşaltılır ki belge
    // sırasıyla ilk tanım kazansın (CommonMark).
    std::vector<std::pair<std::size_t, std::size_t>> repeats;

    bool empty() const { return first.empty(); }
};
MarkdownDefinitions markdown_link_definitions(std::string_view text);

// scan_markdown'ın son adımı; özet çakışması testte elle kurulabilsin diye açık. Girdi: `first`te belge sırasıyla
// toplanmış tanım satırları (`text`in baytları). Çıktı: her etiketin ilk tanımı (özet, normalleşmiş etiket) sırasıyla
// `first`te, sonraki tanımları sıralı `repeats`te.
void keep_first_definitions(std::string_view text, MarkdownDefinitions& definitions);

// Parçalar ve tanımlar tek satır geçişinde (20 MiB'ta iki ayrı geçiş ilk kareye ~7 ms ekliyordu).
struct MarkdownScan {
    std::vector<MarkdownChunk> chunks;
    MarkdownDefinitions definitions;
};
MarkdownScan scan_markdown(std::string_view text, std::size_t target_bytes = kMarkdownChunkBytes,
                           std::size_t max_bytes = kMarkdownMaxChunkBytes, const MarkdownChunkLimits& limits = {});

// Parçanın ayrıştırılacak metni: kendisi (tekrar tanımları boşluğa çevrilmiş; bayt ve satır yerleri aynı) ve ardında
// boş satırla parçanın andığı (`[etiket]`) ama başka parçada tanımlanan etiketlerin ilk tanımları (tanım çıktı
// üretmez, satır eşlemesi kaymaz). Yalnız anılanlar: ~10 MB tanımlı notta her parça bütün tabloyu ayrıştırmasın.
// Kuşatılmış kodla biten parçaya tanım eklenmez.
std::string markdown_chunk_source(std::string_view text, const MarkdownChunk& chunk,
                                  const MarkdownDefinitions& definitions, const MarkdownChunkLimits& limits = {});

}  // namespace noteapp::app
