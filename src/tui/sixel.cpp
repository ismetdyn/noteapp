#include "tui/sixel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace noteapp::tui {

namespace {

// DA1'in sixel özniteliği (xterm ctlseqs "Sixel Graphics": CSI ? ... ; 4 ; ... c).
constexpr int kSixelAttribute = 4;

constexpr int kBandPixels = 6;  // bir sixel karakteri altı dikey piksel
constexpr char kSixelZero = '?';  // 0x3F: boş sixel; değer bu kayma ile yazılır
constexpr int kMinRepeat = 4;    // "!4x" dört karakterden kısa: daha kısa koşu düz yazılır

// Palet: 6x6x6 küp (0..215) ve küpün siyah-beyazı arasında 24 gri (216..239). PDF sayfası çoğunlukla gri tonlu:
// gri basamakları kenar yumuşatmayı küpten iyi taşır.
constexpr int kCubeLevels = 6;
constexpr int kCubeStep = 51;  // 255 / 5
constexpr int kGreyFirst = kCubeLevels * kCubeLevels * kCubeLevels;
constexpr int kGreys = 24;
constexpr int kGreyDivisions = kGreys + 1;  // gri i = i * 255 / 25, i 1..24
constexpr int kColors = kGreyFirst + kGreys;
constexpr int kChannels = 4;  // RGBA

struct Color {
    int red;
    int green;
    int blue;
};

Color palette_color(int index) {
    if (index >= kGreyFirst) {
        const int grey = ((index - kGreyFirst + 1) * 255 + (kGreyDivisions / 2)) / kGreyDivisions;
        return {grey, grey, grey};
    }
    return {(index / (kCubeLevels * kCubeLevels)) * kCubeStep, ((index / kCubeLevels) % kCubeLevels) * kCubeStep,
            (index % kCubeLevels) * kCubeStep};
}

int distance(Color a, Color b) {
    const int red = a.red - b.red;
    const int green = a.green - b.green;
    const int blue = a.blue - b.blue;
    return (red * red) + (green * green) + (blue * blue);
}

// En yakın küp rengi ile en yakın gri arasından yakın olanı.
std::uint8_t nearest(Color color) {
    const auto level = [](int value) { return (value + (kCubeStep / 2)) / kCubeStep; };
    const int cube = (level(color.red) * kCubeLevels * kCubeLevels) + (level(color.green) * kCubeLevels) +
                     level(color.blue);
    const int mean = (color.red + color.green + color.blue) / 3;
    const int grey = kGreyFirst + std::clamp(((mean * kGreyDivisions) + 127) / 255, 1, kGreys) - 1;
    return static_cast<std::uint8_t>(distance(color, palette_color(grey)) < distance(color, palette_color(cube))
                                         ? grey
                                         : cube);
}

int percent(int value) {
    return ((value * 100) + 127) / 255;
}

void append_number(std::string& out, int value) {
    out += std::to_string(value);
}

// Bir renk geçişinin sixel karakterleri: tekrarlar `!n`, sondaki boş sixel'ler yazılmaz (arka plan seçeneği 1).
void append_runs(std::string& out, const std::uint8_t* bits, int width) {
    while (width > 0 && bits[width - 1] == 0)
        --width;
    for (int x = 0; x < width;) {
        int end = x + 1;
        while (end < width && bits[end] == bits[x])
            ++end;
        const int count = end - x;
        const char sixel = static_cast<char>(kSixelZero + bits[x]);
        if (count >= kMinRepeat) {
            out += '!';
            append_number(out, count);
            out += sixel;
        }
        else {
            out.append(static_cast<std::size_t>(count), sixel);
        }
        x = end;
    }
}

}  // namespace

std::optional<CellPixels> sixel_cell(std::string_view mode, std::span<const int> da1,
                                     std::optional<CellPixels> cell) {
    if (!cell || cell->width <= 0 || cell->height <= 0 || mode == "blocks")
        return std::nullopt;
    if (mode == "sixel")
        return cell;
    if (da1.empty())
        return std::nullopt;
    const std::span<const int> attributes = da1.subspan(1);
    return std::ranges::find(attributes, kSixelAttribute) != attributes.end() ? cell : std::nullopt;
}

std::string encode_sixel(const pdfview::RenderedPage& page, int top, int height, int width) {
    top = std::max(top, 0);
    width = std::min(width, page.width);
    height = std::min(height, page.height - top);
    if (width <= 0 || height <= 0)
        return {};
    const auto columns = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> indices(columns * static_cast<std::size_t>(height));
    std::array<bool, kColors> used{};
    for (int y = 0; y < height; ++y) {
        const std::size_t row_start =
            static_cast<std::size_t>(top + y) * static_cast<std::size_t>(page.width) * kChannels;
        const std::uint8_t* row = page.rgba.data() + row_start;
        for (std::size_t x = 0; x < columns; ++x) {
            const std::uint8_t* pixel = row + (x * kChannels);
            const std::uint8_t index = nearest({pixel[0], pixel[1], pixel[2]});
            indices[(static_cast<std::size_t>(y) * columns) + x] = index;
            used[index] = true;
        }
    }

    std::string out = "\x1BP0;1;0q\"1;1;";
    append_number(out, width);
    out += ';';
    append_number(out, height);
    for (int index = 0; index < kColors; ++index) {
        if (!used[static_cast<std::size_t>(index)])
            continue;
        const Color color = palette_color(index);
        out += '#';
        append_number(out, index);
        out += ";2;";
        append_number(out, percent(color.red));
        out += ';';
        append_number(out, percent(color.green));
        out += ';';
        append_number(out, percent(color.blue));
    }

    // Bant başına bir geçiş: her renge sütun başına altı bitlik maske, sonra renk sırasıyla yazılır.
    std::array<int, kColors> slot{};
    std::vector<int> band_colors;
    std::vector<std::uint8_t> bits;
    for (int band = 0; band < height; band += kBandPixels) {
        slot.fill(-1);
        band_colors.clear();
        bits.clear();
        const int rows = std::min(kBandPixels, height - band);
        for (int dy = 0; dy < rows; ++dy)
            for (std::size_t x = 0; x < columns; ++x) {
                const std::uint8_t index = indices[(static_cast<std::size_t>(band + dy) * columns) + x];
                if (slot[index] < 0) {
                    slot[index] = static_cast<int>(band_colors.size());
                    band_colors.push_back(index);
                    bits.resize(bits.size() + columns);
                }
                const auto mask = static_cast<std::uint8_t>(1U << dy);
                bits[(static_cast<std::size_t>(slot[index]) * columns) + x] |= mask;
            }
        std::ranges::sort(band_colors);  // slot renge bağlı: sıralama maskeleri kaydırmaz
        for (std::size_t i = 0; i < band_colors.size(); ++i) {
            const int index = band_colors[i];
            if (i > 0)
                out += '$';  // aynı bandın başına dön, sonraki renk
            out += '#';
            append_number(out, index);
            const auto slot_of = static_cast<std::size_t>(slot[static_cast<std::size_t>(index)]);
            append_runs(out, bits.data() + (slot_of * columns), width);
        }
        if (band + kBandPixels < height)
            out += '-';  // sonraki bant
    }
    out += "\x1B\\";
    return out;
}

}  // namespace noteapp::tui
