#include "tui/view.hpp"

#include "core/i18n.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace noteapp::tui {

namespace {

using Range = std::pair<std::size_t, std::size_t>;  // bayt aralığı [başlangıç, bitiş)

// Sol panel genişliğinin pencereye oranı (pay/payda); önizlemeye kalan alan okunabilir olsun.
constexpr int kListWidthNumerator = 2;
constexpr int kListWidthDenominator = 5;
// Kenarlık (2) + sorgu çubuğu (1) + iki ayraç (2) + kısayol çubuğu (1): panellere kalmayan satırlar.
constexpr int kChromeLines = 6;
constexpr std::string_view kEllipsis = "…";  // ContentIndex kesitlerinin kenar işareti
constexpr std::string_view kTabAsSpaces = "    ";

// UTF-8 kod noktasının bayt uzunluğu (baş bayttan).
std::size_t cp_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return 2;
}

// Aralıkları vurgulu, kalanı düz metin olarak dizer. Aralıklar sıralı ve örtüşmesiz varsayılır; taşanlar
// metne kırpılır.
ftxui::Element highlighted(std::string_view s, const std::vector<Range>& ranges) {
    using namespace ftxui;
    Elements parts;
    std::size_t at = 0;
    for (const auto& [begin, end] : ranges) {
        const std::size_t b = std::min(begin, s.size());
        const std::size_t e = std::min(end, s.size());
        if (b < at || b >= e)
            continue;
        if (b > at)
            parts.push_back(text(std::string(s.substr(at, b - at))));
        parts.push_back(text(std::string(s.substr(b, e - b))) | bold | color(Color::Yellow));
        at = e;
    }
    if (at < s.size())
        parts.push_back(text(std::string(s.substr(at))));
    return hbox(std::move(parts));
}

// fuzzy konumları (bayt ofseti) -> her biri bir kod noktası uzunluğunda aralık.
std::vector<Range> ranges_from_positions(std::string_view path, const std::vector<std::size_t>& positions) {
    std::vector<Range> ranges;
    ranges.reserve(positions.size());
    for (const std::size_t p : positions)
        if (p < path.size())
            ranges.emplace_back(p, p + cp_length(static_cast<unsigned char>(path[p])));
    return ranges;
}

// Kesit satırın bir parçasıdır (kenarlarda "…" olabilir); satırdaki yerini bulup aralıkları oraya kaydırır.
// ponytail: yalnız ilk 3 eşleşen satır vurgulanır (ContentIndex'in verdiği kadar); satırda kesit bulunamazsa
// vurgusuz kalır. Tüm eşleşmeler gerekirse terimleri görünüm tarafında yeniden aramak gerekir.
std::vector<Range> hit_ranges_in_line(const search::Hit& hit, std::string_view line) {
    std::string_view core = hit.snippet;
    std::size_t prefix = 0;
    if (core.starts_with(kEllipsis)) {
        core.remove_prefix(kEllipsis.size());
        prefix = kEllipsis.size();
    }
    if (core.ends_with(kEllipsis))
        core.remove_suffix(kEllipsis.size());
    const std::size_t found = line.find(core);
    if (found == std::string_view::npos)
        return {};
    std::vector<Range> ranges;
    for (const auto& [begin, end] : hit.ranges)
        if (begin >= prefix && begin - prefix + found < line.size())
            ranges.emplace_back(begin - prefix + found, end - prefix + found);
    return ranges;
}

ftxui::Element query_bar(const TuiState& state, const StatusLine& status) {
    using namespace ftxui;
    Elements left = {text("> "), text(state.query())};
    left.push_back(text("▏"));
    if (state.query().empty())
        left.push_back(text(core::tr("tui.query_placeholder")) | dim);
    const std::string mode = core::tr(state.mode() == Mode::Name ? "tui.mode_name" : "tui.mode_content");
    const std::string counts = std::to_string(state.rows().size()) + "/" + std::to_string(state.total());
    return hbox({hbox(std::move(left)), filler(), text(mode + " " + counts + "   " + status.sync)});
}

ftxui::Element list_row(const Row& row, bool is_selected, Mode mode) {
    using namespace ftxui;
    const std::string path = row.path.string();
    Elements cells = {text(is_selected ? "▸ " : "  "),
                      highlighted(path, ranges_from_positions(path, row.positions))};
    if (mode == Mode::Content && row.hit_count > 0)
        cells.push_back(text("  (" + std::to_string(row.hit_count) + ")") | dim);
    cells.push_back(filler());  // seçili satırın tersi tüm genişliği kaplasın
    Element line = hbox(std::move(cells));
    return is_selected ? line | inverted : line;
}

ftxui::Element note_list(const TuiState& state, int height) {
    using namespace ftxui;
    const auto& rows = state.rows();
    const std::size_t visible = static_cast<std::size_t>(std::max(height, 1));
    // Seçili satır her zaman görünsün: pencere seçimle birlikte aşağı kayar.
    const std::size_t top = state.selected() >= visible ? state.selected() + 1 - visible : 0;
    Elements lines;
    for (std::size_t i = top; i < rows.size() && i < top + visible; ++i)
        lines.push_back(list_row(rows[i], i == state.selected(), state.mode()));
    return vbox(std::move(lines));
}

ftxui::Element preview_pane(const TuiState& state, const Preview& preview, int height) {
    using namespace ftxui;
    switch (preview.kind) {
    case Preview::Kind::Empty:
        return emptyElement();
    case Preview::Kind::Binary:
        return text(core::tr("tui.binary_preview")) | dim;
    case Preview::Kind::Error:
        return text(preview.error) | dim;
    case Preview::Kind::Text:
        break;
    }
    const Row* row = state.selected_row();
    const std::size_t first = state.preview_scroll();
    const std::size_t last =
        std::min(preview.lines.size(), first + static_cast<std::size_t>(std::max(height, 1)));
    Elements lines;
    for (std::size_t i = first; i < last; ++i) {
        std::string line = preview.lines[i];
        for (std::size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', tab))
            line.replace(tab, 1, kTabAsSpaces);  // sekme FTXUI'de sütun hesabını bozar
        std::vector<Range> ranges;
        if (row)
            for (const auto& hit : row->hits)
                if (hit.line == i + 1)
                    ranges = hit_ranges_in_line(hit, line);
        lines.push_back(highlighted(line, ranges));
    }
    return vbox(std::move(lines));
}

}  // namespace

const Preview& PreviewCache::get(const core::NoteStore& store, const Row* row) {
    if (!row) {
        path_.reset();
        preview_ = {};
        return preview_;
    }
    if (path_ && *path_ == row->path)
        return preview_;
    path_ = row->path;
    preview_ = {};
    if (row->binary) {
        preview_.kind = Preview::Kind::Binary;
        return preview_;
    }
    const auto content = store.read(row->path);
    if (!content) {
        preview_.kind = Preview::Kind::Error;
        preview_.error = core::format_error(content.error());
        return preview_;
    }
    preview_.kind = Preview::Kind::Text;
    std::string_view rest = content->text;
    while (true) {
        const std::size_t eol = rest.find('\n');
        std::string_view line = rest.substr(0, eol);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        preview_.lines.emplace_back(line);
        if (eol == std::string_view::npos)
            break;
        rest.remove_prefix(eol + 1);
    }
    return preview_;
}

ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window) {
    using namespace ftxui;
    if (window.dimx < kMinWidth || window.dimy < kMinHeight)
        return text(core::tr("tui.too_small")) | center | flex;

    const int panel_height = window.dimy - kChromeLines;
    const int list_width = window.dimx * kListWidthNumerator / kListWidthDenominator;
    const Element bottom =
        status.message.empty() ? text(core::tr("tui.key_hints")) | dim : text(status.message) | bold;
    return vbox({
               query_bar(state, status),
               separator(),
               hbox({note_list(state, panel_height) | size(WIDTH, EQUAL, list_width), separator(),
                     preview_pane(state, preview, panel_height) | flex}) |
                   flex,
               separator(),
               bottom,
           }) |
           border;
}

}  // namespace noteapp::tui
