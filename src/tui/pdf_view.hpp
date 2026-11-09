#pragma once

#include "pdf/markdown_blocks.hpp"
#include "tui/styled_view.hpp"

#include <cstddef>
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

}  // namespace noteapp::tui
