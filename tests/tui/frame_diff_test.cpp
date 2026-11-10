#include "tui/frame_diff.hpp"
#include "tui/sixel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

// xterm davranışını taklit eden küçük terminal: satır kaydırması kapalı, geniş glifin yarısı ezilince öbür
// yarısı silinir. Fark çıktısı ile tam kare aynı son ekranı vermeli.
class VirtualTerminal {
public:
    VirtualTerminal(int width, int height)
        : width_(width), height_(height), cells_(static_cast<std::size_t>(width) * height) {}

    void feed(std::string_view bytes) {
        std::string text;
        const auto flush = [&] {
            for (const std::string& glyph : ftxui::Utf8ToGlyphs(text))
                if (!glyph.empty())
                    put(glyph);
            text.clear();
        };
        for (std::size_t i = 0; i < bytes.size();) {
            const char byte = bytes[i];
            if (byte == '\x1B') {
                flush();
                i = escape(bytes, i);
            } else if (byte == '\r' || byte == '\n' || byte == '\0') {
                flush();
                if (byte == '\r')
                    x_ = 0;
                if (byte == '\n')
                    ++y_;
                REQUIRE(y_ < height_);  // alt satırda kaydırma ekranı bozardı
                ++i;
            } else {
                text += byte;
                ++i;
            }
        }
        flush();
    }

    // Satır başına "glif|stil" dökümü: fark olduğunda doctest çıktısı okunur.
    std::string dump() const {
        std::string out;
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                const Cell& cell = at(x, y);
                out += cell.continuation ? "<>" : cell.text;
                out += '|';
                out += cell.continuation ? "" : cell.style;
                out += ' ';
            }
            out += '\n';
        }
        return out;
    }

    bool cursor_visible() const { return cursor_visible_; }
    int cursor_x() const { return x_; }
    int cursor_y() const { return y_; }

private:
    struct Cell {
        std::string text = " ";
        std::string style = "00000000,,";  // varsayılan stilin metni: style_text() ile aynı biçim
        bool continuation = false;
    };

    Cell& at(int x, int y) { return cells_[static_cast<std::size_t>(y) * width_ + x]; }
    const Cell& at(int x, int y) const { return cells_[static_cast<std::size_t>(y) * width_ + x]; }

    void break_wide(int x) {
        if (at(x, y_).continuation && x > 0)
            at(x - 1, y_) = Cell{" ", at(x - 1, y_).style, false};
        if (x + 1 < width_ && at(x + 1, y_).continuation)
            at(x + 1, y_) = Cell{" ", at(x, y_).style, false};
    }

    void put(const std::string& glyph) {
        REQUIRE(x_ < width_);
        const int width = ftxui::string_width(glyph);
        break_wide(x_);
        if (width == 2 && x_ + 1 < width_)
            break_wide(x_ + 1);
        at(x_, y_) = Cell{glyph, style_text(), false};
        if (width == 2 && x_ + 1 < width_)
            at(x_ + 1, y_) = Cell{"", "", true};
        x_ = std::min(x_ + width, width_ - 1);
    }

    std::string style_text() const {
        std::string out;
        for (const bool flag : flags_)
            out += flag ? '1' : '0';
        return out + "," + foreground_ + "," + background_;
    }

    std::size_t escape(std::string_view bytes, std::size_t start) {
        REQUIRE(start + 1 < bytes.size());
        REQUIRE(bytes[start + 1] == '[');
        std::size_t end = start + 2;
        while (end < bytes.size() && (bytes[end] < 0x40 || bytes[end] > 0x7E))
            ++end;
        REQUIRE(end < bytes.size());
        const std::string_view params = bytes.substr(start + 2, end - start - 2);
        csi(params, bytes[end]);
        return end + 1;
    }

    static std::vector<int> numbers(std::string_view params) {
        std::vector<int> out;
        std::size_t pos = 0;
        while (pos <= params.size()) {
            const std::size_t next = std::min(params.find(';', pos), params.size());
            int value = 0;
            std::from_chars(params.data() + pos, params.data() + next, value);
            out.push_back(value);
            pos = next + 1;
        }
        return out;
    }

    void csi(std::string_view params, char final_byte) {
        if (params == "?2026")
            return;
        if (params == "?25") {
            cursor_visible_ = final_byte == 'h';
            return;
        }
        if (final_byte == 'q')
            return;
        const std::vector<int> args = numbers(params);
        const int first = args.front();
        switch (final_byte) {
        case 'H':
            y_ = std::max(first, 1) - 1;
            x_ = args.size() > 1 ? std::max(args[1], 1) - 1 : 0;
            break;
        case 'C':
            x_ = std::min(x_ + std::max(first, 1), width_ - 1);
            break;
        case 'G':
            x_ = std::max(first, 1) - 1;
            break;
        case 'J':
            REQUIRE(first == 2);
            for (Cell& cell : cells_)
                cell = Cell{};
            break;
        case 'm':
            sgr(args);
            break;
        default:
            FAIL("unexpected CSI final byte: ", final_byte);
        }
    }

    static std::string join(const std::vector<int>& args, std::size_t from, std::size_t count) {
        std::string out;
        for (std::size_t i = from; i < from + count; ++i)
            out += std::to_string(args[i]) + ";";
        return out;
    }

    struct FlagCode {
        int code;
        std::size_t flag;
        bool value;
    };

    void sgr(const std::vector<int>& args) {
        constexpr std::size_t kBold = 0, kDim = 1, kUnderlined = 2, kDouble = 3, kBlink = 4, kInverted = 5,
                              kItalic = 6, kStrike = 7;
        static constexpr std::array<FlagCode, 17> kFlagCodes = {{
            {1, kBold, true},
            {2, kDim, true},
            {22, kBold, false},
            {22, kDim, false},
            {3, kItalic, true},
            {23, kItalic, false},
            {4, kUnderlined, true},
            {4, kDouble, false},
            {21, kDouble, true},
            {21, kUnderlined, false},
            {24, kUnderlined, false},
            {24, kDouble, false},
            {5, kBlink, true},
            {25, kBlink, false},
            {7, kInverted, true},
            {27, kInverted, false},
            {9, kStrike, true},
        }};
        for (std::size_t i = 0; i < args.size(); ++i) {
            const int code = args[i];
            if (code == 38 || code == 48) {
                const std::size_t count = args[i + 1] == 5 ? 2 : 4;
                (code == 38 ? foreground_ : background_) = join(args, i + 1, count);
                i += count;
                continue;
            }
            bool known = sgr_color(code);
            for (const FlagCode& entry : kFlagCodes) {
                if (entry.code == code) {
                    flags_[entry.flag] = entry.value;
                    known = true;
                }
            }
            if (code == 29)
                flags_[kStrike] = false;
            else if (!known)
                FAIL("unexpected SGR: ", code);
        }
    }

    static bool in_range(int code, int first, int last) { return code >= first && code <= last; }

    // Sıfırlama ve tek parametreli renkler; tanındıysa true.
    bool sgr_color(int code) {
        if (code == 0) {
            flags_ = {};
            foreground_.clear();
            background_.clear();
        } else if (code == 39) {
            foreground_.clear();
        } else if (code == 49) {
            background_.clear();
        } else if (in_range(code, 30, 37) || in_range(code, 90, 97)) {
            foreground_ = std::to_string(code);
        } else if (in_range(code, 40, 47) || in_range(code, 100, 107)) {
            background_ = std::to_string(code);
        } else {
            return false;
        }
        return true;
    }

    int width_;
    int height_;
    int x_ = 0;
    int y_ = 0;
    bool cursor_visible_ = true;
    std::array<bool, 8> flags_{};
    std::string foreground_;
    std::string background_;
    std::vector<Cell> cells_;
};

std::string full_frame(const ftxui::Screen& screen) {
    VirtualTerminal fresh(screen.dimx(), screen.dimy());
    fresh.feed(screen.ToString());
    return fresh.dump();
}

constexpr std::array<std::string_view, 12> kNarrow = {
    "a", "Z", " ", "ş", "İ", "ğ", "─", "│", "\xEF\x81\xBB" /* U+F07B Nerd */, "e\xCC\x81" /* e + birleşik */,
    "█", "ö"};
constexpr std::array<std::string_view, 3> kWide = {"漢", "字", "語"};

ftxui::Color random_color(std::mt19937& random) {
    switch (random() % 4) {
    case 0:
        return ftxui::Color::Default;
    case 1:
        return ftxui::Color(static_cast<ftxui::Color::Palette16>(random() % 16));
    case 2:
        return ftxui::Color(static_cast<ftxui::Color::Palette256>(random() % 256));
    default:
        return ftxui::Color::RGB(random() % 256, random() % 256, random() % 256);
    }
}

// Glif ve stil: geniş glifin arkasındaki hücre "" olur, FTXUI metin düğümü de böyle yazar.
void paint_cell(ftxui::Screen& screen, int x, int y, std::mt19937& random) {
    constexpr unsigned kWideOdds = 6;
    ftxui::Cell style;
    style.foreground_color = random_color(random);
    style.background_color = random_color(random);
    style.bold = random() % 4 == 0;
    style.dim = random() % 4 == 0;
    style.italic = random() % 8 == 0;
    style.inverted = random() % 4 == 0;
    style.underlined = random() % 8 == 0;
    style.underlined_double = !style.underlined && random() % 16 == 0;
    style.strikethrough = random() % 16 == 0;
    style.blink = random() % 16 == 0;
    const bool left_is_wide = x > 0 && ftxui::string_width(screen.CellAt(x - 1, y).character) == 2;
    const bool wide = !left_is_wide && x + 1 < screen.dimx() && random() % kWideOdds == 0;
    style.character =
        wide ? std::string(kWide[random() % kWide.size()]) : std::string(kNarrow[random() % kNarrow.size()]);
    if (wide) {
        ftxui::Cell continuation = style;
        continuation.character = "";
        // Önceki geniş glifin arkası ezilmesin diye bu hücre sonra yazılan glifle tutarlı kalır.
        screen.CellAt(x + 1, y) = continuation;
    }
    screen.CellAt(x, y) = style;
}

// Üst üste binen kutu: açılır pencere ve satır vurgusu gibi.
void mutate(ftxui::Screen& screen, std::mt19937& random) {
    const int left = static_cast<int>(random() % screen.dimx());
    const int top = static_cast<int>(random() % screen.dimy());
    const int columns = 1 + static_cast<int>(random() % (screen.dimx() - left));
    const int rows = 1 + static_cast<int>(random() % (screen.dimy() - top));
    for (int y = top; y < top + rows; ++y)
        for (int x = left; x < left + columns; ++x)
            paint_cell(screen, x, y, random);
}

void paint_all(ftxui::Screen& screen, std::mt19937& random) {
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x < screen.dimx(); ++x)
            paint_cell(screen, x, y, random);
}

ftxui::Screen blank_screen(int width, int height) {
    return ftxui::Screen(width, height);
}

}  // namespace

TEST_CASE("frame diff replayed on a terminal equals the full frame across random screens") {
    std::mt19937 random(75);  // NOLINT(cert-msc51-cpp): tekrar üretilebilir olmalı
    constexpr int kSessions = 40;
    constexpr int kFrames = 60;
    for (int session = 0; session < kSessions; ++session) {
        int width = 1 + static_cast<int>(random() % 30);
        int height = 1 + static_cast<int>(random() % 12);
        noteapp::tui::FrameDiff diff;
        auto screen = blank_screen(width, height);
        VirtualTerminal terminal(width, height);
        paint_all(screen, random);
        std::string output;
        diff.write(screen, true, output);
        terminal.feed(output);
        REQUIRE(terminal.dump() == full_frame(screen));
        for (int frame = 0; frame < kFrames; ++frame) {
            const unsigned action = random() % 20;
            bool repaint = false;
            if (action == 0) {  // boyut değişti: terminal içeriği yok sayılır
                width = 1 + static_cast<int>(random() % 30);
                height = 1 + static_cast<int>(random() % 12);
                screen = blank_screen(width, height);
                paint_all(screen, random);
                terminal = VirtualTerminal(width, height);
            } else if (action == 1) {  // harici editörden dönüş
                repaint = true;
                terminal = VirtualTerminal(width, height);
                mutate(screen, random);
            } else if (action == 2) {  // Ctrl+L: terminal bozulmuş olabilir
                diff.invalidate();
                terminal.feed("\x1B[1;1Hgarbage");
            } else {
                mutate(screen, random);
            }
            if (random() % 4 == 0)
                screen.SetCursor({static_cast<int>(random() % width), static_cast<int>(random() % height),
                                  ftxui::Screen::Cursor::Bar});
            else
                screen.SetCursor({0, 0, ftxui::Screen::Cursor::Hidden});
            output.clear();
            diff.write(screen, repaint, output);
            terminal.feed(output);
            REQUIRE(terminal.dump() == full_frame(screen));
            const auto cursor = screen.cursor();
            const bool visible = cursor.shape != ftxui::Screen::Cursor::Hidden;
            REQUIRE(terminal.cursor_visible() == visible);
            if (visible) {
                CHECK(terminal.cursor_x() == cursor.x);
                CHECK(terminal.cursor_y() == cursor.y);
            }
        }
    }
}

TEST_CASE("an unchanged frame writes nothing") {
    std::mt19937 random(1);  // NOLINT(cert-msc51-cpp)
    auto screen = blank_screen(40, 10);
    paint_all(screen, random);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output);
    CHECK_FALSE(output.empty());
    output.clear();
    diff.write(screen, false, output);
    CHECK(output.empty());
}

TEST_CASE("one changed cell costs a move, the glyph and the sync brackets") {
    auto screen = blank_screen(160, 40);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output);
    output.clear();
    screen.CellAt(100, 20).character = "x";
    diff.write(screen, false, output);
    CHECK(output == "\x1B[?2026h\x1B[21;101Hx\x1B[?2026l");
}

TEST_CASE("repaint and invalidate clear the terminal first") {
    auto screen = blank_screen(10, 3);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, false, output);  // ilk kare: terminalde ne olduğu bilinmez
    CHECK(output.find("\x1B[2J") != std::string::npos);
    output.clear();
    diff.write(screen, true, output);
    CHECK(output.find("\x1B[2J") != std::string::npos);
    output.clear();
    diff.invalidate();
    diff.write(screen, false, output);
    CHECK(output.find("\x1B[2J") != std::string::npos);
    output.clear();
    diff.write(screen, false, output);
    CHECK(output.empty());
}

TEST_CASE("a style that ends the frame non-default is reset so nothing leaks to the shell") {
    auto screen = blank_screen(5, 1);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output);
    output.clear();
    screen.CellAt(4, 0).character = "x";
    screen.CellAt(4, 0).inverted = true;
    diff.write(screen, false, output);
    CHECK(output.find("\x1B[0m") != std::string::npos);
}

TEST_CASE("the cursor column is not trusted after a glyph terminals may measure differently") {
    auto screen = blank_screen(20, 1);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output);
    output.clear();
    screen.CellAt(0, 0).character = "\xEF\x81\xBB";  // U+F07B: Nerd Font klasörü
    screen.CellAt(1, 0).character = "a";
    screen.CellAt(2, 0).character = "ş";
    screen.CellAt(3, 0).character = "b";
    diff.write(screen, false, output);
    CHECK(output == "\x1B[?2026h\x1B[1H\xEF\x81\xBB\x1B[2Ga"
                    "şb\x1B[?2026l");
}

TEST_CASE("replacing a wide glyph with narrow ones redraws its right half") {
    auto screen = blank_screen(6, 1);
    screen.CellAt(1, 0).character = "漢";
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output);
    output.clear();
    screen.CellAt(1, 0).character = "a";  // arka hücre "" kalır, terminalde o yarım silinmiştir
    diff.write(screen, false, output);
    VirtualTerminal terminal(6, 1);
    terminal.feed("\x1B[1;2H漢");
    terminal.feed(output);
    CHECK(terminal.dump() == full_frame(screen));
}

namespace {

// 4x3 hücrelik siyah sayfa, hücre 1x2 piksel: sixel baytları küçük kalır.
constexpr noteapp::tui::CellPixels kTinyCell{.width = 1, .height = 2};

noteapp::tui::SixelImage black_image(int x, int y, int rows) {
    auto page = std::make_shared<noteapp::pdfview::RenderedPage>();
    page->width = 4;
    page->height = 6;
    page->rgba.assign(static_cast<std::size_t>(4 * 6 * 4), 0);
    for (std::size_t i = 3; i < page->rgba.size(); i += 4)
        page->rgba[i] = 255;
    return {.pixels = std::move(page), .cell = kTinyCell, .x = x, .y = y, .columns = 4, .rows = rows,
            .first_row = 0};
}

// Görüntünün `first`. hücre satırından `rows` satırın sixel'i.
std::string sixel_rows(const noteapp::tui::SixelImage& image, int first, int rows) {
    return noteapp::tui::encode_sixel(*image.pixels, first * kTinyCell.height, rows * kTinyCell.height,
                                      image.columns * kTinyCell.width);
}

std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

}  // namespace

TEST_CASE("a sixel image is drawn at its cell after the cells, once; an unchanged frame writes nothing") {
    auto screen = blank_screen(10, 8);
    const std::vector images{black_image(2, 1, 3)};
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, images);
    CHECK(output.find("\x1B[2;3H" + sixel_rows(images[0], 0, 3)) != std::string::npos);
    CHECK(count_of(output, "\x1BP") == 1);
    output.clear();
    diff.write(screen, false, output, images);
    CHECK(output.empty());
}

TEST_CASE("after a sixel image the cursor is placed with an absolute move") {
    auto screen = blank_screen(10, 8);
    screen.SetCursor({.x = 5, .y = 7, .shape = ftxui::Screen::Cursor::Bar});
    const std::vector images{black_image(2, 1, 3)};
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, images);
    const std::string sixel = sixel_rows(images[0], 0, 3);
    const std::size_t end = output.find(sixel);
    REQUIRE(end != std::string::npos);
    CHECK(output.substr(end + sixel.size()).starts_with("\x1B[8;6H"));
}

TEST_CASE("a cell written over the image redraws it without the rows the overlay covers") {
    auto screen = blank_screen(10, 8);
    const std::vector images{black_image(2, 1, 3)};
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, images);

    output.clear();
    screen.CellAt(3, 2).character = "x";  // diyalog görüntünün ikinci satırında
    diff.write(screen, false, output, images);
    CHECK(output.find('x') != std::string::npos);
    CHECK(count_of(output, "\x1BP") == 2);  // üst ve alt satır ayrı, ortadaki diyalogun
    CHECK(output.find("\x1B[2;3H" + sixel_rows(images[0], 0, 1)) != std::string::npos);
    CHECK(output.find("\x1B[4;3H" + sixel_rows(images[0], 2, 1)) != std::string::npos);
    CHECK(output.find('x') < output.find("\x1BP"));  // hücreler önce: görüntü onların üstüne değil yanına

    output.clear();
    diff.write(screen, false, output, images);  // diyalog yerinde: basılmaz
    CHECK(output.empty());

    output.clear();
    screen.CellAt(3, 2) = ftxui::Cell{};  // diyalog kapandı
    diff.write(screen, false, output, images);
    CHECK(count_of(output, "\x1BP") == 1);
    CHECK(output.find("\x1B[2;3H" + sixel_rows(images[0], 0, 3)) != std::string::npos);
}

// Yeniden basım (bildirim alanın üstünden kalktı) aynı görüntünün aynı kırpımını yeniden kodlamaz. Görüntü
// üretimde değişmez (shared_ptr<const>); burada pikseller yalnız eski kodlamanın kullanıldığını görmek için değişir.
TEST_CASE("a redrawn sixel image reuses its encoding while it stays in place") {
    auto screen = blank_screen(10, 8);
    auto page = std::make_shared<noteapp::pdfview::RenderedPage>(*black_image(2, 1, 3).pixels);
    const std::vector images{noteapp::tui::SixelImage{
        .pixels = page, .cell = kTinyCell, .x = 2, .y = 1, .columns = 4, .rows = 3, .first_row = 0}};
    const std::string black = sixel_rows(images[0], 0, 3);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, images);
    REQUIRE(output.find(black) != std::string::npos);

    screen.CellAt(3, 2).character = "x";
    diff.write(screen, false, output, images);
    std::ranges::fill(page->rgba, std::uint8_t{255});
    REQUIRE(sixel_rows(images[0], 0, 3) != black);
    output.clear();
    screen.CellAt(3, 2) = ftxui::Cell{};
    diff.write(screen, false, output, images);
    CHECK(output.find("\x1B[2;3H" + black) != std::string::npos);
}

TEST_CASE("a moved image is drawn at its new place and the cells it left are rewritten") {
    auto screen = blank_screen(10, 8);
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, std::vector{black_image(2, 1, 3)});

    output.clear();
    const std::vector moved{black_image(2, 2, 3)};
    diff.write(screen, false, output, moved);
    CHECK(output.find("\x1B[2;3H    ") != std::string::npos);  // eski alanın pikselleri boşlukla silinir
    CHECK(output.find("\x1B[3;3H" + sixel_rows(moved[0], 0, 3)) != std::string::npos);

    output.clear();
    diff.write(screen, false, output);  // görüntü kalktı (metin görünümü)
    CHECK(output.find("\x1BP") == std::string::npos);
    CHECK(output.find("\x1B[3;3H    \x1B[4;3H    \x1B[5;3H    ") != std::string::npos);
}

TEST_CASE("a sixel image stays off the last two rows so the terminal never scrolls") {
    auto screen = blank_screen(10, 8);
    const std::vector images{black_image(2, 5, 3)};
    noteapp::tui::FrameDiff diff;
    std::string output;
    diff.write(screen, true, output, images);
    CHECK(count_of(output, "\x1BP") == 1);
    CHECK(output.find("\x1B[6;3H" + sixel_rows(images[0], 0, 1)) != std::string::npos);
}
