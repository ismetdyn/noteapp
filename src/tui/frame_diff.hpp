#pragma once

#include "tui/sixel.hpp"

#include <ftxui/screen/cell.hpp>
#include <ftxui/screen/screen.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace noteapp::tui {

// Kareyi önceki kareyle karşılaştırıp terminale yalnız değişen hücreleri yazar (FTXUI her karede tüm ekranı
// yazar: 160x40'ta tuş başına ~3 KiB). Terminalin alternatif ekranda ve satır kaydırması kapalı olduğunu
// varsayar; App::SetFrameWriter'dan çağrılır (cmake/patches/ftxui-7.0.3-frame-writer.patch).
class FrameDiff {
public:
    // `repaint` ya da invalidate() sonrası ekran silinir ve baştan çizilir; boyut değişince de öyle.
    // `images` hücre farkından sonra sixel basılır (SPEC-tui "Sixel ekranla uyumu"): görüntü yeni, yeri ya da
    // kırpımı değişti ya da alanına bu karede hücre yazıldıysa. Altındaki hücreler el değmemiş (Cell{}) olmalı;
    // üstüne bir şey çizilmiş satırı (diyalog, bildirim) basılmaz, son iki ekran satırına inmez. Bu karede
    // olmayan görüntünün hücreleri yeniden yazılır: terminalde pikselleri kalmaz.
    void write(const ftxui::Screen& screen, bool repaint, std::string& output,
               std::span<const SixelImage> images = {});
    // Ctrl+L: terminal içeriğine güvenme, sonraki karede baştan çiz.
    void invalidate() { invalid_ = true; }

private:
    void begin_repaint(int width, int height, std::string& output);
    void write_row(const ftxui::Screen& screen, int y, std::string& output);
    void write_cell(const ftxui::Cell& cell, int x, int y, std::string& output);
    void start_frame(std::string& output);
    void move_to(int x, int y, std::string& output);
    void write_cursor(const ftxui::Screen& screen, std::string& output);
    void write_images(const ftxui::Screen& screen, std::span<const SixelImage> images, std::string& output);
    void write_image(const ftxui::Screen& screen, const SixelImage& image, std::string& output);
    char& written(int x, int y) {
        const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
        return written_[row + static_cast<std::size_t>(x)];
    }

    std::vector<ftxui::Cell> shown_;  // terminalde görünenler
    std::vector<SixelImage> images_;  // terminalde basılı görüntüler
    std::vector<char> written_;       // bu karede yazılan (ya da yazılması zorlanan) hücreler
    int width_ = 0;
    int height_ = 0;
    bool invalid_ = true;
    bool frame_started_ = false;
    int column_ = -1;  // terminal imlecinin sütunu; bilinmiyorsa -1
    int row_ = -1;
    int cursor_x_ = -1;  // son gösterilen imleç
    int cursor_y_ = -1;
    int cursor_shape_ = -1;
    bool cursor_visible_ = true;
    ftxui::Cell style_;  // terminalin geçerli stili; yalnız stil alanları anlamlı
};

}  // namespace noteapp::tui
