#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Sayfanın yapılı metni: bloklar -> satırlar -> aynı biçimli parçalar, her karakterin yatay kutusuyla. İşçi MuPDF'in
// stext'inden kurar (engine), istemci telden doğrulayarak okur (protocol::decode_structured); biçimleme (başlık,
// paragraf) ve seçim app'tedir. MuPDF görmez.
namespace noteapp::pdfview {

// TextSpan::style bitleri.
inline constexpr std::uint8_t kStyleBold = 1;
inline constexpr std::uint8_t kStyleItalic = 2;
inline constexpr std::uint8_t kStyleMono = 4;
inline constexpr std::uint8_t kStyleMask = kStyleBold | kStyleItalic | kStyleMono;

// Aynı biçim ve boydaki ardışık karakterler. `text` geçerli UTF-8'dir; her kod noktası bir karakterdir ve
// `edges`'te iki değeri vardır: karakter kutusunun sol ve sağ x'i (punto, sayfanın sol kenarından). Boş değildir.
struct TextSpan {
    std::string text;
    std::uint8_t style = 0;
    float size = 0.0F;  // yazı boyu, punto
    std::vector<float> edges;
    bool operator==(const TextSpan&) const = default;
};

// Ortak taban çizgisindeki karakterler; dikey kutu satırındır (y aşağı doğru artar). `joined`: satır tireyle
// bölünmüş bir sözcükle biter (MuPDF FZ_STEXT_DEHYPHENATE); birleştirirken sondaki tire düşer.
struct TextLine {
    float top = 0.0F;
    float bottom = 0.0F;
    bool joined = false;
    std::vector<TextSpan> spans;
    bool operator==(const TextLine&) const = default;
};

struct TextBlock {
    std::vector<TextLine> lines;
    bool operator==(const TextBlock&) const = default;
};

// Bloklar okuma sırasıyla. Koordinatlar sayfa kutusunun sol üstünden punto; çizilmiş sayfanın pikseline ölçekle
// (render_page'in `scale`'i) çevrilir.
struct PageText {
    float width = 0.0F;
    float height = 0.0F;
    std::vector<TextBlock> blocks;
    bool operator==(const PageText&) const = default;
};

}  // namespace noteapp::pdfview
