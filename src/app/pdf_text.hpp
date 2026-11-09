#pragma once

#include "pdf/markdown_blocks.hpp"
#include "pdfview/structured_text.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// PDF sayfasının yapılı metninden (pdfview::PageText) arayüzlerin ortak kuralları: TUI'nin biçimli görünümü için
// bloklar (markdown'la aynı pdf::Block modeli) ve GUI'nin metin katmanı (isabet, sözcük, seçim metni). MuPDF'siz,
// saf.
namespace noteapp::app {

// Sayfanın görüntü blokları. Başlık sayfanın baskın gövde boyuna (karakter sayısıyla ağırlıklı yazı boyu) göre
// büyüklükten (en çok 3 düzey), paragraf blok içindeki satırların birleşmesinden gelir: satır aralığı açılırsa,
// ilk satır girintiliyse ya da satır liste imiyle (•, -, 1., a)) başlarsa yeni paragraf başlar; sarılmış satırlar
// boşlukla, tireyle bölünmüş sözcük (TextLine::joined) tiresiz birleşir. Kalın, italik ve eş aralıklı parçalar
// korunur. Sekme ve satır sonu boşluk olur.
std::vector<pdf::Block> pdf_page_blocks(const pdfview::PageText& page);

// Sayfa koordinatında (punto) dikdörtgen.
struct PdfRect {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

// Sayfanın karakterleri okuma sırasıyla, tek dizide; seçim [begin, end) karakter aralığıdır (imleç konumları
// 0..size()). GUI sürükleme seçimi, çift tık sözcüğü ve kopyası bunu kullanır.
class PdfTextLayer {
public:
    explicit PdfTextLayer(const pdfview::PageText& page);

    std::size_t size() const { return chars_.size(); }
    float width() const { return width_; }
    float height() const { return height_; }

    // Noktaya en yakın imleç konumu: dikeyde noktayı içeren (yoksa en yakın) satır, satırda karakterin ortasının
    // solu ya da sağı. Karakter yoksa 0.
    std::size_t caret_at(float x, float y) const;
    // Noktadaki sözcüğün [begin, end) aralığı (harf ve rakam koşusu; boşluk ya da noktalamada tek karakter).
    // Karakter yoksa {0, 0}.
    std::pair<std::size_t, std::size_t> word_at(float x, float y) const;
    // Aralığın metni: satırlar blok içinde boşlukla (tireyle bölünmüş sözcük tiresiz), bloklar '\n' ile birleşir.
    std::string text(std::size_t begin, std::size_t end) const;
    // Aralığın vurgu dikdörtgenleri, satır başına bir tane.
    std::vector<PdfRect> boxes(std::size_t begin, std::size_t end) const;

private:
    struct Char {
        float left;
        float right;
        std::uint32_t line;
        std::uint32_t byte;   // text_ içindeki başı
        std::uint8_t length;  // UTF-8 bayt sayısı
    };
    struct Line {
        float top;
        float bottom;
        std::uint32_t block;
        bool joined;
        std::uint32_t first;  // ilk karakteri; satırın karakterleri bir sonraki satırın first'üne dek
    };
    // Noktayı içeren ya da ona en yakın satır (önce dikey, eşitlikte yatay uzaklık). Ön koşul: karakter var.
    std::size_t line_at(float x, float y) const;
    std::pair<std::size_t, std::size_t> line_chars(std::size_t line) const;  // [ilk, son + 1)
    bool is_word_char(std::size_t index) const;

    float width_ = 0.0F;
    float height_ = 0.0F;
    std::string text_;
    std::vector<Char> chars_;
    std::vector<Line> lines_;
};

}  // namespace noteapp::app
