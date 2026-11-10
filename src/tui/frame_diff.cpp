#include "tui/frame_diff.hpp"

#include <algorithm>
#include <cstddef>
#include <ftxui/screen/string.hpp>
#include <string_view>

namespace noteapp::tui {

namespace {

// Desteklemeyen terminal yok sayar; destekleyende kare yırtılmaz.
constexpr std::string_view kSyncBegin = "\x1B[?2026h";
constexpr std::string_view kSyncEnd = "\x1B[?2026l";
constexpr std::string_view kHideCursor = "\x1B[?25l";
constexpr std::string_view kShowCursor = "\x1B[?25h";
constexpr std::string_view kResetStyle = "\x1B[0m";
constexpr std::string_view kClearScreen = "\x1B[2J";
// Sixel bitince imleç görüntünün altına iner, alt kenarı geçerse terminal kaydırır (VT330/VT340 "Sixel
// Scrolling Enabled"). Görüntü son iki satıra inmez: 6 piksellik bant yuvarlaması bir satır daha tutabilir.
constexpr int kImageClearRows = 2;

void append_csi(std::string& output, int value, char final_byte) {
    output += "\x1B[";
    if (value > 1)
        output += std::to_string(value);
    output += final_byte;
}

int cell_width(const ftxui::Cell& cell) {
    if (cell.character.size() <= 1)
        return 1;
    return std::max(ftxui::string_width(cell.character), 1);
}

// "" boş hücredir, terminalde boşlukla aynı görünür.
std::string_view glyph(const ftxui::Cell& cell) {
    return cell.character.empty() ? std::string_view(" ") : std::string_view(cell.character);
}

bool same_style(const ftxui::Cell& a, const ftxui::Cell& b) {
    return a.foreground_color == b.foreground_color && a.background_color == b.background_color &&
           a.bold == b.bold && a.dim == b.dim && a.italic == b.italic && a.inverted == b.inverted &&
           a.underlined == b.underlined && a.underlined_double == b.underlined_double &&
           a.strikethrough == b.strikethrough && a.blink == b.blink;
}

bool same_cell(const ftxui::Cell& a, const ftxui::Cell& b) {
    return same_style(a, b) && glyph(a) == glyph(b);
}

// Hiçbir öğenin dokunmadığı hücre: görüntünün altı. Diyalog (clear_under " " yazar), bildirim ve metin bunu bozar.
bool untouched(const ftxui::Cell& cell) {
    return cell.character.empty() && same_style(cell, ftxui::Cell{});
}

// Terminal bu glifin genişliğini FTXUI gibi sayar mı? ASCII, iki baytlı UTF-8 (Türkçe) ve kutu çizimi kesin;
// Nerd Font, CJK, emoji ve birleşik işaretlerde wcwidth tabloları ayrışabilir, sonraki hücre için imleç
// konumuna güvenilmez.
bool width_is_certain(const ftxui::Cell& cell) {
    constexpr std::size_t kTwoByteUtf8 = 2;
    constexpr std::size_t kThreeByteUtf8 = 3;
    constexpr unsigned char kBoxLead = 0xE2;
    constexpr unsigned char kBoxFirst = 0x94;
    constexpr unsigned char kBoxLast = 0x97;  // U+2500..U+25FF: kutu, blok ve geometrik şekiller
    const std::string& text = cell.character;
    if (text.size() <= kTwoByteUtf8)
        return true;
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    return text.size() == kThreeByteUtf8 && byte(0) == kBoxLead && byte(1) >= kBoxFirst &&
           byte(1) <= kBoxLast;
}

void append_flag(std::string& output, bool from, bool to, std::string_view on, std::string_view off) {
    if (from != to)
        output += to ? on : off;
}

void append_style_change(std::string& output, const ftxui::Cell& from, const ftxui::Cell& to) {
    // 22 kalın ve soluğu birlikte kapatır; açık kalması gereken yeniden açılır.
    if (from.bold != to.bold || from.dim != to.dim) {
        if ((from.bold && !to.bold) || (from.dim && !to.dim))
            output += "\x1B[22m";
        if (to.bold)
            output += "\x1B[1m";
        if (to.dim)
            output += "\x1B[2m";
    }
    if (from.underlined != to.underlined || from.underlined_double != to.underlined_double)
        output += to.underlined ? "\x1B[4m" : to.underlined_double ? "\x1B[21m" : "\x1B[24m";
    append_flag(output, from.blink, to.blink, "\x1B[5m", "\x1B[25m");
    append_flag(output, from.inverted, to.inverted, "\x1B[7m", "\x1B[27m");
    append_flag(output, from.italic, to.italic, "\x1B[3m", "\x1B[23m");
    append_flag(output, from.strikethrough, to.strikethrough, "\x1B[9m", "\x1B[29m");
    if (from.foreground_color != to.foreground_color) {
        output += "\x1B[";
        to.foreground_color.PrintTo(output, false);
        output += 'm';
    }
    if (from.background_color != to.background_color) {
        output += "\x1B[";
        to.background_color.PrintTo(output, true);
        output += 'm';
    }
}

}  // namespace

void FrameDiff::start_frame(std::string& output) {
    if (frame_started_)
        return;
    frame_started_ = true;
    output += kSyncBegin;
    if (cursor_visible_) {
        output += kHideCursor;
        cursor_visible_ = false;
    }
}

void FrameDiff::move_to(int x, int y, std::string& output) {
    if (row_ == y && column_ == x)
        return;
    if (row_ == y && column_ >= 0 && x > column_) {
        append_csi(output, x - column_, 'C');
    } else if (row_ == y) {
        append_csi(output, x + 1, 'G');
    } else if (x == 0 && row_ >= 0 && y == row_ + 1) {
        output += "\r\n";  // y hep ekranda: alt satırda kaydırma olmaz
    } else {
        output += "\x1B[";
        output += std::to_string(y + 1);
        if (x > 0) {
            output += ';';
            output += std::to_string(x + 1);
        }
        output += 'H';
    }
    row_ = y;
    column_ = x;
}

void FrameDiff::write_cursor(const ftxui::Screen& screen, std::string& output) {
    const auto cursor = screen.cursor();
    const bool visible = cursor.shape != ftxui::Screen::Cursor::Hidden;
    const bool moved = visible && (cursor.x != cursor_x_ || cursor.y != cursor_y_);
    const bool reshaped = visible && static_cast<int>(cursor.shape) != cursor_shape_;
    if (!frame_started_ && !moved && !reshaped && visible == cursor_visible_)
        return;
    start_frame(output);  // görünür imleci de gizler
    if (!visible)
        return;
    move_to(cursor.x, cursor.y, output);
    cursor_x_ = cursor.x;
    cursor_y_ = cursor.y;
    if (reshaped) {
        cursor_shape_ = static_cast<int>(cursor.shape);
        output += "\x1B[";
        output += std::to_string(cursor_shape_);
        output += " q";
    }
    output += kShowCursor;
    cursor_visible_ = true;
}

void FrameDiff::begin_repaint(int width, int height, std::string& output) {
    invalid_ = false;
    width_ = width;
    height_ = height;
    shown_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), ftxui::Cell{});
    cursor_visible_ = true;
    cursor_shape_ = -1;
    cursor_x_ = -1;
    images_.clear();  // ekran silindi: görüntüler yeniden basılır
    start_frame(output);
    output += kResetStyle;
    output += kClearScreen;
}

void FrameDiff::write_cell(const ftxui::Cell& cell, int x, int y, std::string& output) {
    start_frame(output);
    move_to(x, y, output);
    if (!same_style(style_, cell)) {
        append_style_change(output, style_, cell);
        style_ = cell;
    }
    output += glyph(cell);
    written(x, y) = 1;
    const int end = x + cell_width(cell);
    column_ = (end < width_ && width_is_certain(cell)) ? end : -1;
}

void FrameDiff::write_row(const ftxui::Screen& screen, int y, std::string& output) {
    auto shown = shown_.begin() + static_cast<std::ptrdiff_t>(y) * width_;
    auto forced = written_.begin() + static_cast<std::ptrdiff_t>(y) * width_;  // kalkan görüntünün altı
    int skip = 0;         // geniş glifin üstüne bindiği hücre yazılmaz
    int invalidated = 0;  // eski geniş glifin yarısı silindi, hücre eşit görünse de yeniden yazılır
    for (int x = 0; x < width_; ++x, ++shown, ++forced) {
        const ftxui::Cell& cell = screen.CellAt(x, y);
        const bool changed = !same_cell(*shown, cell);
        const int next_width = cell_width(cell);
        if (skip == 0 && (changed || invalidated > 0 || *forced != 0))
            write_cell(cell, x, y, output);
        skip = skip > 0 ? 0 : next_width - 1;
        invalidated = std::max({invalidated, cell_width(*shown), next_width}) - 1;
        if (changed)
            *shown = cell;
    }
}

void FrameDiff::write_images(const ftxui::Screen& screen, std::span<const SixelImage> images,
                             std::string& output) {
    for (const SixelImage& image : images) {
        bool touched = std::ranges::find(images_, image) == images_.end();
        for (int y = std::max(image.y, 0); !touched && y < std::min(image.y + image.rows, height_); ++y)
            for (int x = std::max(image.x, 0); !touched && x < std::min(image.x + image.columns, width_); ++x)
                touched = written(x, y) != 0;
        if (touched)
            write_image(screen, image, output);
    }
    images_.assign(images.begin(), images.end());
}

// Altı el değmemiş ardışık satırlar tek sixel: üstüne çizilmiş satır atlanır, diyalog görüntünün önünde kalır.
void FrameDiff::write_image(const ftxui::Screen& screen, const SixelImage& image, std::string& output) {
    const int columns = std::min(image.columns, width_ - image.x);
    if (!image.pixels || image.x < 0 || image.y < 0 || columns <= 0)
        return;
    const int bottom = std::min(image.y + image.rows, height_ - kImageClearRows);
    const auto clear_row = [&](int y) {
        for (int x = image.x; x < image.x + columns; ++x)
            if (!untouched(screen.CellAt(x, y)))
                return false;
        return true;
    };
    for (int y = image.y; y < bottom;) {
        if (!clear_row(y)) {
            ++y;
            continue;
        }
        int end = y + 1;
        while (end < bottom && clear_row(end))
            ++end;
        start_frame(output);
        move_to(image.x, y, output);
        output += encode_sixel(*image.pixels, (image.first_row + y - image.y) * image.cell.height,
                               (end - y) * image.cell.height, columns * image.cell.width);
        // İmleç görüntünün altında, terminale göre değişir: sonraki hareket mutlak.
        row_ = -1;
        column_ = -1;
        y = end;
    }
}

void FrameDiff::write(const ftxui::Screen& screen, bool repaint, std::string& output,
                      std::span<const SixelImage> images) {
    const int width = screen.dimx();
    const int height = screen.dimy();
    frame_started_ = false;
    // Karenin ilk hareketi mutlak: geçen kareden kalan konum varsayımı bozulduysa hata taşınmaz.
    column_ = -1;
    row_ = -1;
    if (repaint || invalid_ || width != width_ || height != height_)
        begin_repaint(width, height, output);

    written_.assign(shown_.size(), 0);
    for (const SixelImage& image : images_)
        if (std::ranges::find(images, image) == images.end())
            for (int y = std::max(image.y, 0); y < std::min(image.y + image.rows, height_); ++y)
                for (int x = std::max(image.x, 0); x < std::min(image.x + image.columns, width_); ++x)
                    written(x, y) = 1;

    style_ = ftxui::Cell{};
    for (int y = 0; y < height; ++y)
        write_row(screen, y, output);
    if (!same_style(style_, ftxui::Cell{}))
        output += kResetStyle;
    write_images(screen, images, output);
    write_cursor(screen, output);
    if (frame_started_)
        output += kSyncEnd;
}

}  // namespace noteapp::tui
