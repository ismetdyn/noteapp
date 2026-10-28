#include "tui/view.hpp"

#include "core/i18n.hpp"
#include "tui/modals.hpp"

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

// Kaydet diyaloğunun iç genişliği; en küçük pencerede (60) kenarlara sığar.
constexpr int kDialogWidth = 52;
constexpr int kLabelWidth = 9;  // "Klasör:" + boşluk

ftxui::Element dialog_field(std::string label, const std::string& value, bool focused,
                            ftxui::Element extra = ftxui::emptyElement()) {
    using namespace ftxui;
    Element name = text(std::move(label) + ":") | size(WIDTH, EQUAL, kLabelWidth);
    if (focused)
        name = name | bold;
    Elements cells = {std::move(name), text(value)};
    if (focused)
        cells.push_back(text("▏"));
    cells.push_back(text("  "));
    cells.push_back(std::move(extra));
    return hbox(std::move(cells));
}

ftxui::Element discard_choice(const std::string& label, bool selected) {
    using namespace ftxui;
    Element button = text("[ " + label + " ]");
    return selected ? button | inverted : button;
}

// Onay penceresinin gövdesi: soru + [Vazgeç] [Evet] düğmeleri. Silme ve not atma onayı aynı görünümü
// paylaşır; "Vazgeç" (tui.discard_no) her ikisinde de varsayılan.
ftxui::Elements confirm_body(const std::string& question, const std::string& yes_label, bool yes_selected) {
    using namespace ftxui;
    return {paragraph(question) | bold, text(""),
            hbox({discard_choice(core::tr("tui.discard_no"), !yes_selected), text("   "),
                  discard_choice(yes_label, yes_selected)}) |
                center};
}

ftxui::Element save_dialog_box(const SaveDialogState& dialog) {
    using namespace ftxui;
    Elements body;
    if (dialog.confirming_discard()) {
        body = confirm_body(core::tr("tui.confirm_discard"), core::tr("tui.discard_yes"),
                            dialog.discard_selected());
    } else {
        using Field = SaveDialogState::Field;
        body.push_back(
            dialog_field(core::tr("tui.field_name"), dialog.name(), dialog.focus() == Field::Name));
        body.push_back(
            dialog_field(core::tr("tui.field_folder"), dialog.folder(), dialog.focus() == Field::Folder));
        if (dialog.focus() == Field::Folder) {
            const auto& suggestions = dialog.suggestions();
            for (std::size_t i = 0; i < suggestions.size(); ++i) {
                const bool picked = static_cast<int>(i) == dialog.selected_suggestion();
                Element line = hbox(
                    {text(std::string(kLabelWidth, ' ') + (picked ? "▸ " : "  ")), text(suggestions[i])});
                body.push_back(picked ? line | inverted : line | dim);
            }
        }
        body.push_back(dialog_field(core::tr("tui.field_type"), dialog.type(), dialog.focus() == Field::Type,
                                    text(dialog.type_label()) | dim));
        if (!dialog.error().empty())
            body.push_back(paragraph(dialog.error()) |
                           color(Color::Red));  // temp yolu uzun olabilir: sarılır
        body.push_back(text(""));
        body.push_back(text(core::tr("tui.save_hints")) | dim);
    }
    return window(text(" " + dialog.title() + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

ftxui::Element confirm_box(const ConfirmState& confirm) {
    using namespace ftxui;
    return window(text(" " + core::tr("tui.delete_title") + " ") | bold,
                  vbox(confirm_body(confirm.message(), core::tr("tui.delete_yes"), confirm.yes_selected()))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Yardım: katalogdaki satırlar kaydırılır; görünür satır sayısı pencere yüksekliğinden (kenarlık 2 + boşluk
// ve ipucu 2) çıkar, böylece 60x15'te de ipucu satırı görünür kalır.
ftxui::Element help_box(const HelpState& help, int window_height) {
    using namespace ftxui;
    const std::vector<std::string> lines = help_lines();
    const auto visible = static_cast<std::size_t>(std::max(window_height - 4, 1));
    const std::size_t first = std::min(help.scroll(), lines.size() > visible ? lines.size() - visible : 0);
    Elements body;
    for (std::size_t i = first; i < lines.size() && i < first + visible; ++i)
        body.push_back(text(lines[i]));
    body.push_back(text(""));
    body.push_back(text(core::tr("tui.help_hints")) | dim);
    return window(text(" " + core::tr("tui.help_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Ayarlar formu: etiket / değer iki sütun. Kaydet düğmesi, hata ve ipucu satırı sabit kalır; yalnız 12 alan
// satırı kayar (60x15'te 10 satır görünür), odaklı satır her zaman görünür.
constexpr int kSettingsWidth = 58;
constexpr int kSettingsLabelWidth = 22;
constexpr std::size_t kSettingsValueCells = 30;  // uzun yol/URL'de sondaki kısım görünür (imleç orada)
constexpr int kSettingsFixedLines = 5;           // kenarlık 2 + düğme + hata + ipucu

// Değerin son `limit` kod noktası; kesilen başa "…" konur.
std::string tail_code_points(const std::string& value, std::size_t limit) {
    std::vector<std::size_t> starts;  // her kod noktasının ilk baytı
    for (std::size_t i = 0; i < value.size(); ++i)
        if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    if (starts.size() <= limit)
        return value;
    return "…" + value.substr(starts[starts.size() - limit + 1]);
}

ftxui::Element settings_row(const SettingsState::Row& row, bool focused) {
    using namespace ftxui;
    std::string shown;
    switch (row.kind) {
    case SettingsState::Kind::Toggle:
        shown = row.value == "true" ? "[x]" : "[ ]";
        break;
    case SettingsState::Kind::Choice:
        shown = "‹ " + tail_code_points(row.value, kSettingsValueCells) + " ›";
        break;
    case SettingsState::Kind::Text:
    case SettingsState::Kind::Token:  // değer modelde zaten maskelenmiş noktalardır
        shown = tail_code_points(row.value, kSettingsValueCells);
        break;
    }
    if (focused && (row.kind == SettingsState::Kind::Text || row.kind == SettingsState::Kind::Token))
        shown += "▏";
    Element value = text(shown);
    if (!row.error.empty())
        value = value | color(Color::Red);
    if (!row.note.empty())
        value = hbox({std::move(value), text("  " + row.note) | dim});
    Element line =
        hbox({text(settings_label(row)) | size(WIDTH, EQUAL, kSettingsLabelWidth), std::move(value)});
    return focused ? line | inverted : line;
}

ftxui::Element settings_box(const SettingsState& settings, int window_height) {
    using namespace ftxui;
    Elements body;
    if (const ConfirmState* confirm = settings.discard_confirm()) {
        body = confirm_body(confirm->message(), core::tr("tui.discard_yes"), confirm->yes_selected());
    } else if (const ConfirmState* replace = settings.replace_confirm()) {
        body = confirm_body(replace->message(), core::tr("tui.replace_yes"), replace->yes_selected());
    } else {
        // Token satırı (varsa) 12 satırın ardındadır; odak/kaydırma aynı sıra numarasıyla yürür.
        std::vector<SettingsState::Row> rows = settings.rows();
        if (settings.token_visible())
            rows.push_back(settings.token_row());
        const auto visible = static_cast<std::size_t>(std::max(window_height - kSettingsFixedLines, 1));
        const std::size_t focus_row = std::min(settings.focus(), rows.size() - 1);
        const std::size_t first = focus_row < visible ? 0 : focus_row + 1 - visible;
        std::string error = settings.error();
        for (const auto& row : rows)
            if (error.empty())
                error = row.error;
        for (std::size_t i = first; i < rows.size() && i < first + visible; ++i)
            body.push_back(settings_row(rows[i], settings.focus() == i));
        Element save = text("[ " + core::tr("tui.settings_save") + " ]");
        body.push_back((settings.on_button() ? save | inverted : save) | center);
        body.push_back(text(error) | color(Color::Red));
        body.push_back(
            text(core::tr(settings.on_token() ? "tui.settings_token_hints" : "tui.settings_hints")) | dim);
    }
    return window(text(" " + core::tr("tui.settings_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kSettingsWidth) | clear_under;
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
    const auto content = store.read(row->path);
    if (!content) {
        preview_.kind = Preview::Kind::Error;
        preview_.error = core::format_error(content.error());
        return preview_;
    }
    if (core::looks_binary(content->text)) {
        preview_.kind = Preview::Kind::Binary;
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
                      ftxui::Dimensions window, const SaveDialogState* dialog, const ConfirmState* confirm,
                      const HelpState* help, const SettingsState* settings) {
    using namespace ftxui;
    if (window.dimx < kMinWidth || window.dimy < kMinHeight)
        return text(core::tr("tui.too_small")) | center | flex;

    const int panel_height = window.dimy - kChromeLines;
    const int list_width = window.dimx * kListWidthNumerator / kListWidthDenominator;
    const Element bottom =
        status.message.empty() ? text(core::tr("tui.key_hints")) | dim : text(status.message) | bold;
    const Element main = vbox({
                             query_bar(state, status),
                             separator(),
                             hbox({note_list(state, panel_height) | size(WIDTH, EQUAL, list_width),
                                   separator(), preview_pane(state, preview, panel_height) | flex}) |
                                 flex,
                             separator(),
                             bottom,
                         }) |
                         border;
    Element modal;
    if (help)
        modal = help_box(*help, window.dimy);
    else if (confirm)
        modal = confirm_box(*confirm);
    else if (dialog)
        modal = save_dialog_box(*dialog);
    else if (settings)
        modal = settings_box(*settings, window.dimy);
    else
        return main;
    return dbox({main, modal | center});
}

}  // namespace noteapp::tui
