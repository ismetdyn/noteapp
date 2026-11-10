#include "tui/sixel.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

using noteapp::pdfview::RenderedPage;
using noteapp::tui::CellPixels;
using noteapp::tui::encode_sixel;
using noteapp::tui::sixel_cell;

namespace {

struct Rgb {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

// `color(x, y)` ile boyanmış RGBA sayfa.
template <class Color>
RenderedPage page_of(int width, int height, Color color) {
    RenderedPage page{.width = width, .height = height, .rgba = {}};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const Rgb rgb = color(x, y);
            page.rgba.insert(page.rgba.end(), {rgb.red, rgb.green, rgb.blue, 255});
        }
    return page;
}

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};

}  // namespace

TEST_CASE("a 2x6 two colour image encodes to the exact DCS sixel bytes") {
    const RenderedPage page = page_of(2, 6, [](int x, int) { return x == 0 ? kBlack : kWhite; });
    // Renkler küpün 0 ve 215'i, yüzde RGB; siyah geçişi sondaki boş sixel'i yazmaz, beyaz `$` ile bant başından.
    CHECK(encode_sixel(page, 0, 6, 2) ==
          "\x1BP0;1;0q\"1;1;2;6#0;2;0;0;0#215;2;100;100;100#0~$#215?~\x1B\\");
}

TEST_CASE("encode_sixel crops to the requested rows and columns, and to the page") {
    const RenderedPage page = page_of(3, 12, [](int, int y) { return y < 6 ? kBlack : kWhite; });
    // Satır 4-7: iki siyah, iki beyaz piksel satırı tek bantta; ilk iki sütun.
    CHECK(encode_sixel(page, 4, 4, 2) ==
          "\x1BP0;1;0q\"1;1;2;4#0;2;0;0;0#215;2;100;100;100#0BB$#215KK\x1B\\");
    // Sayfanın altını aşan istek sayfaya kırpılır, tamamen dışarıdaki boştur.
    CHECK(encode_sixel(page, 6, 100, 100) == "\x1BP0;1;0q\"1;1;3;6#215;2;100;100;100#215~~~\x1B\\");
    CHECK(encode_sixel(page, 12, 6, 3).empty());
    CHECK(encode_sixel(page, 0, 6, 0).empty());
}

TEST_CASE("encode_sixel repeats runs with ! and moves to the next band with -") {
    const RenderedPage grey = page_of(5, 1, [](int, int) { return Rgb{128, 128, 128}; });
    // 128 küpte 153'e, gride 133'e düşer: gri yakın (216 + 12).
    CHECK(encode_sixel(grey, 0, 1, 5) == "\x1BP0;1;0q\"1;1;5;1#228;2;52;52;52#228!5@\x1B\\");
    const RenderedPage tall = page_of(1, 7, [](int, int) { return kBlack; });
    CHECK(encode_sixel(tall, 0, 7, 1) == "\x1BP0;1;0q\"1;1;1;7#0;2;0;0;0#0~-#0@\x1B\\");
}

TEST_CASE("pdf_image mode: sixel needs a known cell size, auto also needs DA1 attribute 4") {
    const CellPixels cell{.width = 10, .height = 20};
    const std::vector<int> with_sixel{62, 4, 22};
    const std::vector<int> without{62, 22};
    CHECK(sixel_cell("sixel", without, cell) == cell);
    CHECK(sixel_cell("sixel", with_sixel, std::nullopt) == std::nullopt);
    CHECK(sixel_cell("sixel", with_sixel, CellPixels{0, 20}) == std::nullopt);
    CHECK(sixel_cell("auto", with_sixel, cell) == cell);
    CHECK(sixel_cell("auto", without, cell) == std::nullopt);
    CHECK(sixel_cell("auto", {}, cell) == std::nullopt);
    CHECK(sixel_cell("auto", std::vector<int>{4}, cell) == std::nullopt);  // 4 yalnız sınıf değeri: VT132
    CHECK(sixel_cell("auto", with_sixel, std::nullopt) == std::nullopt);
    CHECK(sixel_cell("blocks", with_sixel, cell) == std::nullopt);
}
