#pragma once

#include "pdf/markdown_blocks.hpp"
#include "pdfview/rendered_page.hpp"
#include "tui/sixel.hpp"
#include "tui/styled_view.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// PDF sayfasının REVIEW görünümü (PreviewCache): app::pdf_page_blocks'un blokları markdown'ın biçimli görünümüyle
// aynı görünüşte. Önce soluk sayfa ayırıcısı (app::pdf_page_separator), sonra başlıklar (soluk "# " imi ve başlık
// rengi), kalın/italik/eş aralıklı parçalar ve genişliğe sarılan paragraflar; bloklar boş satırla ayrılır. `error`
// boş değilse bloklar yerine soluk hata satırı. Satırlar genişliğe sığar, metin terminal_safe'ten geçer.
RenderedView render_pdf_page(std::string_view separator, const std::vector<pdf::Block>& blocks,
                             std::string_view error, int width, CellWidthFn cell_width,
                             std::size_t max_lines = ViewLimits{}.lines);

// Tek soluk satır (yükleniyor, kesildi, açılış hatası): genişliğe sarılır.
RenderedView render_pdf_note(std::string_view note, int width, CellWidthFn cell_width);

// Sayfa görünümünün (`R`) `▀` blok çizimi: hücre iki dikey pikseldir, üstü ön plan, altı arka plan rengi.
struct Rgb {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    bool operator==(const Rgb&) const = default;
};
struct BlockCell {
    std::optional<Rgb> top;     // yoksa hücre sayfanın altında: boş kalır
    std::optional<Rgb> bottom;  // yoksa sayfa bu hücrenin ortasında biter
};

// Blok çiziminde hücre bir piksel genişlik, iki piksel yüksekliktir (terminal hücresi ~1:2, piksel kareye yakın).
inline constexpr CellPixels kBlockCell{.width = 1, .height = 2};

// `width` x `height` punto sayfanın `columns` hücre genişlikte kapladığı hücre satırı (en az 1); sayfa
// `columns` x `cell.width` piksel genişliğe çizilir (sixel: terminalin hücre boyu).
int page_block_rows(float width, float height, int columns, CellPixels cell = kBlockCell);

// Çizilmiş sayfanın `row`. hücre satırı: sayfa iki eksende aynı oranla `columns` piksel genişliğe kutu örneklenir
// (her hedef piksel kapladığı kaynak piksellerin ortalaması; alfa hep 255, yok sayılır).
std::vector<BlockCell> page_block_row(const pdfview::RenderedPage& page, int row, int columns);

}  // namespace noteapp::tui
