#pragma once

#include "app/pdf_text.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <utility>

namespace noteapp::gui {

// PDF önizlemesinin metin katmanı (view_pdf.cpp): görünen sayfaların yapılı metni ve tek sayfalık fare seçimi.
// Sayfalar limits.pdf.cache_pages'le sınırlı, görünenler sayılmaz (dokularla aynı kural); oturum değişince atılır.
struct PdfSelection {
    struct Page {
        std::optional<app::PdfTextLayer> layer;  // yoksa sayfanın metni okunamadı: seçilemez, yeniden istenmez
        std::size_t used = 0;                    // son çizildiği kare
    };
    std::map<int, Page> pages;
    // Seçim [min(anchor, caret), max) karakter aralığı, `page` sayfasında; page < 0 seçim yok.
    int page = -1;
    std::size_t anchor = 0;
    std::size_t caret = 0;

    bool active() const { return page >= 0 && anchor != caret; }
    std::pair<std::size_t, std::size_t> range() const { return std::minmax(anchor, caret); }
    void clear_selection() {
        page = -1;
        anchor = caret = 0;
    }
    void reset() {
        pages.clear();
        clear_selection();
    }
};

}  // namespace noteapp::gui
