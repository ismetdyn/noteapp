#pragma once

#include <cstddef>
#include <cstdint>
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
    std::size_t rows = 5000;
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

// Girintilenmiş JSON'un bir satırı: `depth` iç içelik, `text` boşluksuz belirteçler (`"ad": "x",`), `offset`
// satırın ilk belirtecinin kaynaktaki (BOM dahil) bayt ofseti.
struct JsonLine {
    std::size_t depth = 0;
    std::string text;
    std::size_t offset = 0;
};

// Katı RFC 8259: geçersiz JSON nullopt. Özyinelemesiz, iç içelik sınırı yok; satır sayısını çağıran sınırlar
// (kaynak zaten limits.tui.view_bytes altındadır).
std::optional<std::vector<JsonLine>> pretty_json(std::string_view source);

// Büyük markdown notunun bağımsız biçimlenen parçası: [begin, end) bayt aralığı, `first_line` başladığı kaynak
// satırı (0 tabanlı).
struct MarkdownChunk {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t first_line = 0;
    bool open_fence = false;  // kapanmamış kuşatılmış kodla biter (yalnız son parça olabilir)

    bool operator==(const MarkdownChunk&) const = default;
};

// İki arayüz büyük notu parça parça biçimler: yalnız görünen parça ayrıştırılıp yerleştirilir. Kapasite sınırı
// değil iş birimidir; bundan küçük not tek parçadır, çıktısı bütün biçimlemeyle aynıdır.
inline constexpr std::size_t kMarkdownChunkBytes = std::size_t{64} * 1024;

// Kesim yalnız bloğu kapatan yerde: boş satırdan sonra sütun 0'da liste imi, alıntı ya da girintiyle
// başlamayan satırın önü, kuşatılmış kodun dışında (liste, alıntı, tablo, paragraf orada biter; md4c
// MD_FLAG_NOHTML ile HTML bloğu yok). Parça en az `target_bytes`tır; kesim yeri yoksa not tek parçadır.
// Bağlantı başvuru tanımları (`[ad]: url`) bütün notta geçerlidir: markdown_link_definitions ve
// markdown_chunk_source ile her parçaya taşınır.
std::vector<MarkdownChunk> markdown_chunks(std::string_view text, std::size_t target_bytes = kMarkdownChunkBytes);

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
MarkdownScan scan_markdown(std::string_view text, std::size_t target_bytes = kMarkdownChunkBytes);

// Parçanın ayrıştırılacak metni: kendisi (tekrar tanımları boşluğa çevrilmiş; bayt ve satır yerleri aynı) ve ardında
// boş satırla parçanın andığı (`[etiket]`) ama başka parçada tanımlanan etiketlerin ilk tanımları (tanım çıktı
// üretmez, satır eşlemesi kaymaz). Yalnız anılanlar: ~10 MB tanımlı notta her parça bütün tabloyu ayrıştırmasın.
// Kuşatılmış kodla biten parçaya tanım eklenmez.
std::string markdown_chunk_source(std::string_view text, const MarkdownChunk& chunk,
                                  const MarkdownDefinitions& definitions);

}  // namespace noteapp::app
