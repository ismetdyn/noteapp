#pragma once

#include <ftxui/screen/cell.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

namespace noteapp::tui {

// Kareyi önceki kareyle karşılaştırıp terminale yalnız değişen hücreleri yazar (FTXUI her karede tüm ekranı
// yazar: 160x40'ta tuş başına ~3 KiB). Terminalin alternatif ekranda ve satır kaydırması kapalı olduğunu
// varsayar; App::SetFrameWriter'dan çağrılır (cmake/patches/ftxui-7.0.3-frame-writer.patch).
class FrameDiff {
public:
    // `repaint` ya da invalidate() sonrası ekran silinir ve baştan çizilir; boyut değişince de öyle.
    void write(const ftxui::Screen& screen, bool repaint, std::string& output);
    // Ctrl+L: terminal içeriğine güvenme, sonraki karede baştan çiz.
    void invalidate() { invalid_ = true; }

private:
    void begin_repaint(int width, int height, std::string& output);
    void write_row(const ftxui::Screen& screen, int y, std::string& output);
    void write_cell(const ftxui::Cell& cell, int x, int y, std::string& output);
    void start_frame(std::string& output);
    void move_to(int x, int y, std::string& output);
    void write_cursor(const ftxui::Screen& screen, std::string& output);

    std::vector<ftxui::Cell> shown_;  // terminalde görünenler
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
