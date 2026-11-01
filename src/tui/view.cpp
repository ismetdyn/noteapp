#include "tui/view.hpp"

#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "tui/display_text.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/modals.hpp"

#include <algorithm>
#include <chrono>
#include <ftxui/screen/string.hpp>
#include <string_view>
#include <utility>

namespace noteapp::tui {

namespace {

using Range = std::pair<std::size_t, std::size_t>;  // bayt aralığı [başlangıç, bitiş)

constexpr std::string_view kHintSeparator = "  ";
constexpr int kFrameCells = 2;
constexpr int kMinPathCells = 16;  // sağ sütun yolu bundan fazla sıkıştırmaz
constexpr std::string_view kColumnGap = "  ";
constexpr std::string_view kEllipsis = "…";  // ContentIndex kesitlerinin kenar işareti
constexpr int kFrameLabelOffset = 1;         // etiket köşeyi örtmesin
constexpr int kPriorityHints = 5;            // Listede süzgeç yüzünden düşmeyen ilk ipuçları

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
            parts.push_back(text(terminal_safe(s.substr(at, b - at))));
        parts.push_back(text(terminal_safe(s.substr(b, e - b))) | bold | color(Color::Yellow));
        at = e;
    }
    if (at < s.size())
        parts.push_back(text(terminal_safe(s.substr(at))));
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

std::string mode_label(const TuiState& state) {
    if (state.trash_view())
        return core::tr("tui.mode_trash");
    switch (state.mode()) {
    case Mode::Content:
        return core::tr("tui.mode_content");
    case Mode::Global:
        return core::tr("tui.mode_global");
    case Mode::Name:
        break;
    }
    return core::tr("tui.mode_name");
}

std::string sort_label(const TuiState& state) {
    const search::SortSpec& sort = state.list_sort();
    const char* key = "tui.sort_updated";
    if (sort.field == search::SortField::Name)
        key = "tui.sort_name";
    else if (sort.field == search::SortField::Size)
        key = "tui.sort_size";
    else if (sort.field == search::SortField::Created)
        key = "tui.sort_created";
    return core::tr(key) + (sort.descending ? "↓" : "↑");
}

ftxui::Element top_bar(const TuiState& state, const StatusLine& status) {
    using namespace ftxui;
    const std::string counts = std::to_string(state.rows().size()) + "/" + std::to_string(state.total());
    const std::string left = state.trash_view() ? core::tr("tui.trash_title") : trash_indicator(state);
    return hbox({text(left) | bold, filler(),
                 text(sort_label(state) + "  " + mode_label(state) + " " + counts + "   " + status.sync)});
}

// `n` görünümünün üst satırı: liste ve sayaçlar gizli, kaydedildiyse notun yolu.
ftxui::Element new_note_header(const EditorSession& editor) {
    using namespace ftxui;
    Elements cells = {text(core::tr("tui.new_note_header")) | bold};
    if (editor.path())
        cells.push_back(text(std::string(kHintSeparator) + editor.path()->string()));
    return hbox(std::move(cells));
}

// Odaklı panelin kipi: 16 ANSI rengi (açık ve koyu terminalde okunur), rengi görmeyen için metin etiketi.
struct PaneMode {
    ftxui::Color color;
    std::string label;
};

PaneMode list_mode() {
    return {ftxui::Color::Cyan, core::tr("tui.frame_list")};
}

PaneMode review_mode() {
    return {ftxui::Color::Green, core::tr("tui.frame_review")};
}

// Salt-okunur notta yazılamaz: etiket EDIT dese kullanıcı tuşların neden etkisiz olduğunu anlamazdı.
PaneMode editor_mode(const EditorSession& editor) {
    if (editor.read_only() != EditorSession::ReadOnly::No)
        return review_mode();
    return {ftxui::Color::Magenta,
            core::tr("tui.frame_edit") + (editor.dirty() ? std::string(kDirtyMark) : "")};
}

// Odaklı panel kalın ve renkli çerçeve alır; renksiz terminalde de kalınlık farkı ve etiket görünür. Etiket
// çerçevenin üzerine ayrı katmandır: `color` süsleyicisi içeriğe uygulanırsa sözdizimi renklerini ezerdi.
ftxui::Element framed_pane(ftxui::Element content, const std::optional<PaneMode>& mode) {
    using namespace ftxui;
    if (!mode)
        return std::move(content) | borderStyled(LIGHT);
    // Dolgu şart: katman tüm panel boyunu alırsa `color` kutusu etiketin altındaki sütunları da boyar.
    return dbox({std::move(content) | borderStyled(HEAVY, mode->color),
                 vbox({hbox({emptyElement() | size(WIDTH, EQUAL, kFrameLabelOffset),
                             text(" " + mode->label + " ") | bold | color(mode->color)}),
                       filler()})});
}

std::string sync_state_text(core::SyncState state) {
    switch (state) {
    case core::SyncState::Synced:
        return core::tr("tui.sync_state_synced");
    case core::SyncState::Modified:
        return core::tr("tui.sync_state_modified");
    case core::SyncState::New:
        return core::tr("tui.sync_state_new");
    case core::SyncState::ConflictCopy:
        return core::tr("tui.sync_state_conflict");
    case core::SyncState::SyncOff:
        break;
    }
    return core::tr("tui.sync_off");
}

// Eski depodan gelen notta created ya da origin olmayabilir: "bilinmiyor" dürüst cevaptır, boş sütun değil.
std::string created_text(const core::NoteMeta& meta) {
    return meta.created_at ? local_time_text(*meta.created_at) : core::tr("tui.meta_unknown");
}

std::string origin_text(const core::NoteMeta& meta) {
    return meta.origin ? terminal_safe(*meta.origin) : core::tr("tui.meta_unknown");
}

// Sütun tüm çizim için aynı: saat satır başına okunmaz.
struct ListCells {
    int digits = 1;  // en büyük satır numarasının hane sayısı
    int inner_width = 0;
    std::chrono::system_clock::time_point now;
    bool name_row_open = false;  // odak ad satırında: seçili satır işaretlenmez
};

int digit_count(std::size_t value) {
    return static_cast<int>(std::to_string(value).size());
}

// Satır başı: seçim, numara, boşluk, işaret, boşluk.
int row_prefix_cells(int digits) {
    constexpr int kFixedPrefixCells = 4;
    return kFixedPrefixCells + digits;
}

// Sığmayan değer yarım kesilmez: önce tarih, sonra boyut düşer. `free` yolu kesmeden kalan yer; yol uzunsa
// boyut `squeezed` yerde de gösterilir (yol en az kMinPathCells kalır), tarih yol kesmeye değmez.
std::string fitted_size_and_date(const core::NoteMeta& meta, const ListCells& cells, int free, int squeezed) {
    const std::string size = human_size(meta.size);
    const std::string both = size + std::string(kColumnGap) + short_time_text(meta.updated_at, cells.now);
    if (ftxui::string_width(both) <= free)
        return both;
    return ftxui::string_width(size) <= std::max(free, squeezed) ? size : std::string{};
}

std::string column_text(const core::NoteMeta& meta, ListColumn column) {
    switch (column) {
    case ListColumn::Size:
        return human_size(meta.size);
    case ListColumn::Updated:
        return local_time_text(meta.updated_at);
    case ListColumn::Created:
        return created_text(meta);
    case ListColumn::Sync:
        return sync_state_text(meta.sync_status);
    case ListColumn::Origin:
        return origin_text(meta);
    case ListColumn::SizeAndDate:
    case ListColumn::None:
        break;
    }
    return {};
}

// Boş dönerse sütun çizilmez; yol en az kMinPathCells hücre tutar.
std::string fitted_column(const TuiState& state, const Row& row, const ListCells& cells) {
    const ListColumn column = state.list_column();
    const core::NoteMeta* meta = state.meta_of(row.path);
    if (!meta || column == ListColumn::None)
        return {};
    const int after_prefix = cells.inner_width - row_prefix_cells(cells.digits) - 1;
    const int free = after_prefix - ftxui::string_width(row.path.string());
    const int squeezed = after_prefix - kMinPathCells;
    if (column == ListColumn::SizeAndDate)
        return fitted_size_and_date(*meta, cells, free, squeezed);
    std::string shown = column_text(*meta, column);
    return ftxui::string_width(shown) <= std::max(free, squeezed) ? shown : std::string{};
}

ftxui::Element list_row(const TuiState& state, const Row& row, std::size_t index, const ListCells& list) {
    using namespace ftxui;
    const bool is_selected = index == state.selected() && !list.name_row_open;
    const std::string path = row.path.string();
    std::string number = std::to_string(index + 1);
    number.insert(0, static_cast<std::size_t>(list.digits) - number.size(), ' ');
    // `>` seçim (sağ panel odaklıyken ters renk yok), `▸` `▾` ayrıntının kapalı ya da açık olduğu.
    Elements cells = {text(is_selected ? ">" : " "), text(number) | dim, text(" "),
                      text(state.is_expanded(row.path) ? "▾ " : "▸ "),
                      highlighted(path, ranges_from_positions(path, row.positions)) | flex_shrink};
    if (state.mode() != Mode::Name && row.hit_count > 0)
        cells.push_back(text("  (" + std::to_string(row.hit_count) + ")") | dim);
    cells.push_back(filler());  // seçili satırın tersi tüm genişliği kaplasın
    if (const std::string column = fitted_column(state, row, list); !column.empty())
        cells.push_back(text(" " + column) | dim);
    Element line = hbox(std::move(cells));
    return is_selected && !right_pane_focused(state.focus()) ? line | inverted : line;
}

// Satırın ek görsel satırları; adedi TuiState::row_height ile aynı olmalı (fare ve kaydırma ona güvenir).
void push_row_details(ftxui::Elements& lines, const TuiState& state, const Row& row) {
    using namespace ftxui;
    constexpr std::string_view kDetailIndent = "    ";
    if (row.show_best_line && !row.hits.empty()) {
        const search::Hit& hit = row.hits.front();
        std::vector<Range> ranges(hit.ranges.begin(), hit.ranges.end());
        lines.push_back(hbox({text(std::string(kDetailIndent)), highlighted(hit.snippet, ranges) | dim}));
    }
    if (!state.is_expanded(row.path))
        return;
    const core::NoteMeta* meta = state.meta_of(row.path);
    const std::string unknown = core::tr("tui.meta_unknown");
    const std::string fields[TuiState::kMetaLines] = {
        core::tr("tui.meta_size", {{"value", meta ? human_size(meta->size) : unknown}}),
        core::tr("tui.meta_updated", {{"value", meta ? local_time_text(meta->updated_at) : unknown}}),
        core::tr("tui.meta_created", {{"value", meta ? created_text(*meta) : unknown}}),
        core::tr("tui.meta_sync", {{"value", meta ? sync_state_text(meta->sync_status) : unknown}}),
        core::tr("tui.meta_origin", {{"value", meta ? origin_text(*meta) : unknown}}),
    };
    for (const std::string& field : fields)
        lines.push_back(text(std::string(kDetailIndent) + field) | dim);
}

// `a`: listenin ilk satırı. Numara sütunu boş kalır, yol diğer satırlarla aynı hücrede başlar.
ftxui::Element name_row(const NewRowState& row, const ListCells& cells) {
    using namespace ftxui;
    constexpr int kCursorCells = 1;
    const int room = std::max(cells.inner_width - row_prefix_cells(cells.digits) - kCursorCells, 1);
    return hbox({text(">"), text(std::string(static_cast<std::size_t>(cells.digits), ' ')), text(" "),
                 text("+ "), text(tail_code_points(row.text(), static_cast<std::size_t>(room))), text("▏"),
                 filler()}) |
           inverted;
}

// Ad satırının kapladığı satır: kendisi ve panele sarılan hatası.
int name_row_lines(const NewRowState& row, int inner_width) {
    if (row.error().empty())
        return 1;
    return 1 + (ftxui::string_width(row.error()) + inner_width - 1) / std::max(inner_width, 1);
}

ftxui::Element note_list(const TuiState& state, int height, int width, const NewRowState* new_row) {
    using namespace ftxui;
    const auto& rows = state.rows();
    const int inner_width = width - kFrameCells;
    const int reserved = new_row ? name_row_lines(*new_row, inner_width) : 0;
    const auto visible = static_cast<std::size_t>(std::max(height - reserved, 1));
    const ListCells cells{digit_count(rows.size()), inner_width, std::chrono::system_clock::now(),
                          new_row != nullptr};
    Elements lines;
    if (new_row) {
        lines.push_back(name_row(*new_row, cells));
        if (!new_row->error().empty())
            lines.push_back(paragraph(terminal_safe(new_row->error())) | color(Color::Red));
    }
    const std::size_t room = visible + lines.size();
    for (std::size_t i = state.list_top(visible); i < rows.size() && lines.size() < room; ++i) {
        lines.push_back(list_row(state, rows[i], i, cells));
        push_row_details(lines, state, rows[i]);
    }
    return vbox(std::move(lines));
}

// Tüm eşleşmeler (içerik araması + not içi arama) tek vurgu listesi.
std::vector<ByteRange> line_highlights(const TuiState& state, const Row* row, std::size_t index,
                                       std::string_view line) {
    std::vector<ByteRange> ranges = state.preview_find().ranges_on_line(index);
    if (row)
        for (const auto& hit : row->hits)
            if (hit.line == index + 1)
                for (const auto& [begin, end] : hit_ranges_in_line(hit, line))
                    ranges.push_back({begin, end});
    return ranges;
}

// `cat` gibi: uzun satır kırpılmaz, panele sarılır. Kaydırma birimi mantıksal satırdır; sarma, satır kaçış
// baytları ve sekmeler editörle aynı yoldan geçer.
ftxui::Element preview_pane(const TuiState& state, const Preview& preview, int height, int columns,
                            SpellChecker* spell) {
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
    const auto wanted = static_cast<std::size_t>(std::max(height, 1));
    Elements lines;
    for (std::size_t i = state.preview_scroll(); i < preview.lines.size() && lines.size() < wanted; ++i) {
        const EditorBuffer buffer(preview.lines[i], terminal_cell_width);
        const std::vector<ByteRange> ranges = line_highlights(state, row, i, preview.lines[i]);
        std::vector<SyntaxSpan> spans;
        if (preview.language != SyntaxLanguage::Plain) {
            const SyntaxState entry = preview.syntax.entry_state(preview.language, i, [&](std::size_t index) {
                return std::string_view(preview.lines[index]);
            });
            highlight_line(preview.language, preview.lines[i], entry, &spans);
        }
        const std::optional<ByteRange> selected = state.preview_selection_on_line(i, preview.lines[i].size());
        const std::vector<ByteRange> misspelled =
            spell && preview.spell_checked ? spell->misspelled(preview.lines[i]) : std::vector<ByteRange>{};
        const RowPaint paint{std::nullopt,
                             selected ? selected : state.preview_find().current_range_on_line(i),
                             ranges,
                             spans,
                             0,
                             misspelled};
        for (const VisualRow& visual : buffer.rows_of_line(0, columns))
            if (lines.size() < wanted)
                lines.push_back(paint_row(buffer, visual, paint));
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
    const std::string title = confirm.title().empty() ? core::tr("tui.delete_title") : confirm.title();
    return window(text(" " + title + " ") | bold,
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

const char* hints_key(const TuiState& state) {
    if (state.trash_view())
        return "tui.key_hints_trash";
    return state.focus() == Focus::Preview ? "tui.key_hints_preview" : "tui.key_hints_list";
}

// Toplamlar: süzgeç yokken eşleşme sayısı gürültüdür. Sayfa yalnız konum bilgisidir, kaydırma sanaldır.
std::string totals_text(const TuiState& state, int panel_height) {
    const auto page_rows = static_cast<std::size_t>(std::max(panel_height, 1));
    const std::size_t shown = state.rows().size();
    std::string totals = core::tr("tui.totals", {{"total", std::to_string(state.total())}});
    if (!state.query().empty())
        totals += core::tr("tui.totals_matches", {{"count", std::to_string(shown)}});
    return totals + core::tr("tui.totals_page", {{"page", std::to_string(state.selected() / page_rows + 1)},
                                                 {"pages", std::to_string(std::max<std::size_t>(
                                                               (shown + page_rows - 1) / page_rows, 1))}});
}

// Yol girdisi: imleç metnin içindedir; uzun yolda imleç çevresi görünür kalır.
ftxui::Element rename_bar(const RenameState& rename, int width) {
    using namespace ftxui;
    constexpr int kCursorCells = 1;
    const std::string prefix = core::tr("tui.rename_label") + " ";
    const std::string hints = core::tr("tui.rename_hints");
    const int room = std::max(width - string_width(prefix) - kCursorCells, 2);
    const std::string before = rename.text().substr(0, rename.cursor());
    const std::string after = rename.text().substr(rename.cursor());
    const std::string shown_before = tail_code_points(before, static_cast<std::size_t>(room));
    const int after_room = room - string_width(shown_before);
    Elements cells = {text(prefix) | bold, text(shown_before), text("▏"),
                      text(string_width(after) <= after_room ? after : "") | flex_shrink};
    const int left = after_room - string_width(after) - static_cast<int>(kHintSeparator.size());
    if (const std::string fitted = fit_hints(hints, left); !fitted.empty()) {
        cells.push_back(filler());
        cells.push_back(text(fitted) | dim);
    }
    return hbox(std::move(cells));
}

// Yeri belli hata: "6: sync değeri geçersiz: nope ...". Metin sayılmadığını kullanıcı burada görür.
std::string query_error_text(const TuiState& state) {
    if (!state.query_error())
        return {};
    return core::tr("tui.query_error",
                    {{"column", std::to_string(state.query_error()->begin + 1)},
                     {"detail", terminal_safe(core::format_error(state.query_error()->error))}});
}

ftxui::Element search_bar(const TuiState& state, int width) {
    const bool global = state.mode() == Mode::Global;
    return input_bar(global ? core::tr("tui.global_label") : ">", state.query(),
                     core::tr(global ? "tui.key_hints_global" : "tui.key_hints_search"), width, {},
                     query_error_text(state));
}

ftxui::Element preview_find_bar(const TuiState& state, int width) {
    const PreviewFind& find = state.preview_find();
    return input_bar(core::tr("tui.find_label"), find.query(), core::tr("tui.note_find_hints"), width,
                     match_label(find.current(), find.count()));
}

constexpr std::string_view kFilterPrompt = "> ";

// Süzgeç ya da not içi arama etkin kaldıysa üst çubukta yeri yok: kısayolların başında görünür. Uzun sorgunun
// sonu `room` hücreye kısalır, öncelikli ipuçları çubuktan düşmesin.
std::string active_filter(const TuiState& state, int room) {
    if (state.focus() == Focus::Preview)
        return match_label(state.preview_find().current(), state.preview_find().count());
    if (state.query().empty())
        return {};
    const int query_room = std::max(room - static_cast<int>(kFilterPrompt.size()), 1);
    return std::string(kFilterPrompt) + tail_code_points(state.query(), static_cast<std::size_t>(query_room));
}

// Katalogdaki ilk kPriorityHints ipucunun kapladığı hücre.
int priority_hints_cells(std::string_view hints) {
    std::size_t end = 0;
    for (int i = 0; i < kPriorityHints && end != std::string_view::npos; ++i)
        end = hints.find(kHintSeparator, end == 0 ? 0 : end + kHintSeparator.size());
    return ftxui::string_width(std::string(hints.substr(0, end)));
}

ftxui::Element bottom_bar(const TuiState& state, const StatusLine& status, const NewRowState* new_row,
                          const RenameState* rename, int width) {
    using namespace ftxui;
    if (rename) {
        if (rename->error().empty())
            return rename_bar(*rename, width);
        return vbox({text(rename->error()) | color(Color::Red), rename_bar(*rename, width)});
    }
    if (new_row)
        return text(fit_hints(core::tr("tui.new_row_hints"), width)) |
               dim;  // girdi listede, burada yalnız tuşlar
    if (state.focus() == Focus::Search)
        return search_bar(state, width);
    if (state.focus() == Focus::PreviewFind)
        return preview_find_bar(state, width);
    if (state.focus() == Focus::SortMenu)
        return text(fit_hints(core::tr("tui.sort_menu"), width)) | bold;
    if (!status.message.empty())
        return text(status.message) | bold;
    const std::string hints = core::tr(hints_key(state));
    const std::string filter =
        active_filter(state, width - priority_hints_cells(hints) - static_cast<int>(kHintSeparator.size()));
    if (filter.empty())
        return text(fit_hints(hints, width)) | dim;
    const std::string shown = filter + std::string(kHintSeparator);
    return hbox({text(shown) | bold, text(fit_hints(hints, width - string_width(shown))) | dim});
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

// Birden çoğu açıksa öncelik: yardım > onay > diyalog > ayarlar.
ftxui::Element open_modal(const HelpState* help, const ConfirmState* confirm, const SaveDialogState* dialog,
                          const SettingsState* settings, int window_height) {
    if (help)
        return help_box(*help, window_height);
    if (confirm)
        return confirm_box(*confirm);
    if (dialog)
        return save_dialog_box(*dialog);
    return settings ? settings_box(*settings, window_height) : nullptr;
}

const EditorSession* active_editor(const TuiState& state, const EditorSession* editor) {
    return state.focus() == Focus::Editor ? editor : nullptr;
}

ftxui::Element right_pane(const TuiState& state, const Preview& preview, const EditorSession* editing,
                          int height, int columns, bool show_cursor, SpellChecker* spell) {
    return editing && editing->is_open() ? editor_pane(*editing, show_cursor, spell)
                                         : preview_pane(state, preview, height, columns, spell);
}

std::optional<PaneMode> right_mode(const TuiState& state, const EditorSession* editing) {
    if (editing && editing->is_open())
        return editor_mode(*editing);
    if (right_pane_focused(state.focus()))
        return review_mode();
    return std::nullopt;
}

// Yakınlaştırmada yalnız odaklı panel çizilir.
ftxui::Element panes(const TuiState& state, const Preview& preview, const EditorSession* editing,
                     const NewRowState* new_row, ftxui::Dimensions window, int height, bool show_cursor,
                     SpellChecker* spell) {
    using namespace ftxui;
    const PaneGeometry geometry = state.layout().geometry(window.dimx, right_pane_focused(state.focus()));
    Elements cells;
    if (geometry.list_visible)
        cells.push_back(
            framed_pane(note_list(state, height, geometry.list_width, new_row) | flex,
                        state.focus() == Focus::List ? std::optional(list_mode()) : std::nullopt) |
            size(WIDTH, EQUAL, geometry.list_width));
    if (geometry.right_visible)
        cells.push_back(
            framed_pane(right_pane(state, preview, editing, height,
                                   editor_viewport(window, state.layout()).columns, show_cursor, spell) |
                            flex,
                        right_mode(state, editing)) |
            flex);
    return hbox(std::move(cells));
}

}  // namespace

ftxui::Element input_bar(const std::string& label, const std::string& value, const std::string& hints,
                         int width, const std::string& counter, const std::string& error) {
    using namespace ftxui;
    constexpr int kCursorCells = 1;
    const std::string prefix = label + " ";
    const std::string note = !error.empty() ? error : counter;
    const int note_cells = note.empty() ? 0 : string_width(note) + static_cast<int>(kHintSeparator.size());
    const int room = std::max(width - string_width(prefix) - kCursorCells - note_cells, 1);
    const std::string shown = tail_code_points(value, static_cast<std::size_t>(room));
    Elements cells = {text(prefix) | bold, text(shown), text("▏")};
    if (!note.empty()) {
        Element shown_note = text(std::string(kHintSeparator) + note) | bold;
        cells.push_back(error.empty() ? shown_note : shown_note | color(Color::Red));
    }
    const int left = room - string_width(shown) - static_cast<int>(kHintSeparator.size());
    if (const std::string fitted = fit_hints(hints, left); !fitted.empty()) {
        cells.push_back(filler());
        cells.push_back(text(fitted) | dim);
    }
    return hbox(std::move(cells));
}

std::string trash_indicator(const TuiState& state) {
    if (state.trash_view() || state.trash_count() == 0)
        return {};
    return core::tr("tui.trash_indicator", {{"count", std::to_string(state.trash_count())}});
}

const Preview& PreviewCache::get(const core::NoteStore& store, const Row* row) {
    return load(row, false, [&] { return store.read(row->path); });
}

const Preview& PreviewCache::get_trashed(const core::NoteStore& store, const Row* row) {
    return load(row, true, [&] { return store.read_trash(row->path); });
}

const Preview& PreviewCache::current(const TuiState& state, const core::NoteStore& store) {
    return state.trash_view() ? get_trashed(store, state.selected_row()) : get(store, state.selected_row());
}

const Preview& PreviewCache::throttled(const TuiState& state, const core::NoteStore& store,
                                       std::chrono::steady_clock::time_point now) {
    deferred_ = false;
    const Row* row = state.selected_row();
    if (row && (!requested_ || *requested_ != row->path)) {
        const bool idle = now - last_move_ >= std::chrono::milliseconds(kCoalesceMs);
        requested_ = row->path;
        last_move_ = now;
        waiting_ = !idle;
    }
    if (row && waiting_ && now - last_move_ < std::chrono::milliseconds(kCoalesceMs)) {
        deferred_ = true;
        return preview_;
    }
    waiting_ = false;
    return current(state, store);
}

int row_marker_column(const TuiState& state) {
    constexpr int kCellsBeforeNumber = 2;  // kenar, seçim
    constexpr int kCellsAfterNumber = 1;   // boşluk
    return kCellsBeforeNumber + digit_count(state.rows().size()) + kCellsAfterNumber;
}

const Preview& PreviewCache::load(const Row* row, bool from_trash, const Reader& read) {
    if (!row) {
        path_.reset();
        preview_ = {};
        return preview_;
    }
    if (path_ && *path_ == row->path && from_trash_ == from_trash)
        return preview_;
    path_ = row->path;
    from_trash_ = from_trash;
    preview_ = {};
    ++generation_;
    auto content = read();
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
    const std::u8string extension = row->path.value().extension().u8string();
    const std::string type =
        extension.empty() ? std::string{} : std::string(extension.begin() + 1, extension.end());
    preview_.spell_checked = spellcheck_applies_to(type);
    if (content->text.size() <= kMaxHighlightBytes)
        preview_.language = type.empty() ? SyntaxLanguage::Plain : syntax_language_for(type);
    // Sarma geçerli UTF-8 ister; ikili sayılmayan ama bozuk bayt içeren notta gösterim için arındırılır.
    if (!core::is_valid_utf8(content->text) || content->text.find('\0') != std::string::npos) {
        content->text = sanitized_utf8(content->text);
        preview_.display_only = true;
    }
    preview_.text = std::move(content->text);
    std::string_view rest = preview_.text;
    while (true) {
        const std::size_t eol = rest.find('\n');
        std::string_view line = rest.substr(0, eol);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        preview_.lines.emplace_back(line);
        preview_.line_starts.push_back(static_cast<std::size_t>(rest.data() - preview_.text.data()));
        if (eol == std::string_view::npos)
            break;
        rest.remove_prefix(eol + 1);
    }
    return preview_;
}

std::string selected_text(const Preview& preview, const PreviewRange& range) {
    if (range.end_line >= preview.line_starts.size())
        return {};
    const std::size_t begin = preview.line_starts[range.begin_line] + range.begin_offset;
    const std::size_t end = preview.line_starts[range.end_line] + range.end_offset;
    // Uçlar çizildiği andaki metne aittir: bu arada küçülen not substr'ı out_of_range'e götürürdü.
    return end > begin && begin < preview.text.size() ? preview.text.substr(begin, end - begin)
                                                      : std::string{};
}

std::string tail_code_points(const std::string& value, std::size_t limit) {
    std::vector<std::size_t> starts;  // her kod noktasının ilk baytı
    for (std::size_t i = 0; i < value.size(); ++i)
        if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    if (starts.size() <= limit)
        return value;
    if (limit <= 1)
        return "…";  // uzun hata satırı girdiye tek hücre bırakabilir
    return "…" + value.substr(starts[starts.size() - limit + 1]);
}

std::string fit_hints(std::string_view hints, int width) {
    std::string fitted;
    std::size_t start = 0;
    while (start < hints.size()) {
        const std::size_t end = hints.find(kHintSeparator, start);
        std::string candidate = fitted;
        if (!candidate.empty())
            candidate += kHintSeparator;
        candidate += hints.substr(start, end - start);
        if (ftxui::string_width(candidate) > width)
            break;
        fitted = std::move(candidate);
        if (end == std::string_view::npos)
            break;
        start = end + kHintSeparator.size();
    }
    return fitted;
}

EditorViewport editor_viewport(ftxui::Dimensions window, const PaneLayout& layout) {
    const PaneGeometry geometry = layout.geometry(window.dimx, true);
    return {std::max(geometry.right_width - kFrameCells, 1),
            std::max(window.dimy - TuiState::kChromeLines, 1)};
}

ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window, const SaveDialogState* dialog, const ConfirmState* confirm,
                      const HelpState* help, const SettingsState* settings, const NewRowState* new_row,
                      const EditorSession* editor, SpellChecker* spell, const RenameState* rename) {
    using namespace ftxui;
    if (window.dimx < kMinWidth || window.dimy < kMinHeight)
        return text(core::tr("tui.too_small")) | center | flex;

    // Yol girdisi hatası alt çubuğa bir satır ekler: panel kısalır ki seçili satır kırpılmasın.
    const bool input_error = rename && !rename->error().empty();
    const EditorSession* editing = active_editor(state, editor);
    const bool editor_open = editing && editing->is_open();
    const int panel_height = window.dimy - TuiState::kChromeLines - (input_error ? 1 : 0);
    Element modal = open_modal(help, confirm, dialog, settings, window.dimy);
    const bool modal_open = modal != nullptr;
    Elements lines = {editing && state.new_note_view() ? new_note_header(*editing) : top_bar(state, status),
                      panes(state, preview, editing, new_row, window, panel_height, !modal_open, spell) |
                          flex};
    lines.push_back(editor_open ? editor_info_bar(*editing, window.dimx)
                                : text(totals_text(state, panel_height)) | dim);
    lines.push_back(editor_open ? editor_status_bar(*editing, window.dimx)
                                : bottom_bar(state, status, new_row, rename, window.dimx));
    const Element main = vbox(std::move(lines));
    if (!modal && editing)
        modal = editor_modal(*editing);
    if (!modal)
        return main;
    return dbox({main, modal | center});
}

}  // namespace noteapp::tui
