#pragma once

#include "pdfview/rendered_page.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace noteapp::tui {

// Terminal hücresinin piksel boyu.
struct CellPixels {
    int width = 0;
    int height = 0;
    bool operator==(const CellPixels&) const = default;
};

// `[tui] pdf_image` (SPEC-tui "PDF sayfa görüntüsü"): sixel çizilecekse hücre boyu, `▀` blok çizilecekse nullopt.
// `sixel` hücre boyu bilinince, `auto` ayrıca DA1 yanıtı sixel (öznitelik 4) bildirince sixel olur; `da1`
// FTXUI'nin sakladığı yanıttır (App::TerminalCapabilities, ilk değer terminal sınıfı).
std::optional<CellPixels> sixel_cell(std::string_view mode, std::span<const int> da1,
                                     std::optional<CellPixels> cell);

// Sayfanın `[top, top + height)` piksel satırlarının ilk `width` sütunu sixel DCS dizisi olarak (sayfaya
// kırpılır; boş bölge boş dizi). Arka plan seçeneği 1: renk geçişleri birbirini silmez. Palet sabit 6x6x6
// küp + 24 gri, yalnız kullanılan renkler tanımlanır; tekrarlar `!n` ile kısalır. Terminale yazmaz.
std::string encode_sixel(const pdfview::RenderedPage& page, int top, int height, int width);

// Kare sonunda sixel basılacak görüntü (FrameDiff): `pixels`in `first_row`. hücre satırından başlayan `rows`
// satırı ekranın (`x`, `y`) hücresine, `columns` sütun genişlikte.
struct SixelImage {
    std::shared_ptr<const pdfview::RenderedPage> pixels;
    CellPixels cell;
    int x = 0;
    int y = 0;
    int columns = 0;
    int rows = 0;
    int first_row = 0;
    bool operator==(const SixelImage&) const = default;
};

}  // namespace noteapp::tui
