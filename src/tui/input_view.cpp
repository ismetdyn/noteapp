#include "tui/input_view.hpp"

#include "core/i18n.hpp"
#include "tui/display_text.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <ftxui/screen/string.hpp>
#include <string_view>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr int kCursorCells = 1;
constexpr int kPopupWidth = 52;
constexpr std::string_view kCursor = "▏";
constexpr std::string_view kEllipsis = "…";

ftxui::Color span_color(InputSpan::Kind kind) {
    switch (kind) {
    case InputSpan::Kind::Key:
        return ftxui::Color::Cyan;
    case InputSpan::Kind::Value:
        return ftxui::Color::Green;
    case InputSpan::Kind::Invalid:
        break;
    }
    return ftxui::Color::Red;
}

// [begin, end) aralığını renk sınırlarından böler. Sınırlar sözcük ve `:` kıyısında ya da kod noktası başında
// olduğundan çok baytlı karakter bölünmez. Metin güvenilmezdir: `F2` hostile adla dolu açılır, yazım bidi
// karakteri taşıyabilir.
void append_runs(ftxui::Elements& out, std::string_view text, std::size_t begin, std::size_t end,
                 std::span<const InputSpan> spans) {
    const auto plain = [&](std::size_t from, std::size_t to) {
        if (from < to)
            out.push_back(ftxui::text(terminal_safe(text.substr(from, to - from))));
    };
    std::size_t at = begin;
    for (const InputSpan& span : spans) {
        const std::size_t from = std::max(span.begin, at);
        const std::size_t to = std::min(span.end, end);
        if (from >= to)
            continue;
        plain(at, from);
        out.push_back(ftxui::text(terminal_safe(text.substr(from, to - from))) |
                      ftxui::color(span_color(span.kind)));
        at = to;
    }
    plain(at, end);
}

// Baştan `cells` hücreye sığan kod noktaları.
std::string head_cells(std::string_view value, int cells) {
    std::string shown;
    std::size_t at = 0;
    while (at < value.size()) {
        std::size_t next = at + 1;
        while (next < value.size() && (static_cast<unsigned char>(value[next]) & 0xC0) == 0x80)
            ++next;
        std::string candidate = shown + std::string(value.substr(at, next - at));
        if (ftxui::string_width(candidate) > cells)
            break;
        shown = std::move(candidate);
        at = next;
    }
    return shown;
}

}  // namespace

InputCells input_text_cells(const InputLine& line, std::span<const InputSpan> spans, const std::string& ghost,
                            int room) {
    const std::string_view value = line.text();
    const std::string before(value.substr(0, line.cursor()));
    const std::string after(value.substr(line.cursor()));
    // tail_cells kestiyse başa "…" koyar: gösterilen kuyruğun baytı metindeki yerine geri çevrilir.
    const std::string shown_before = tail_cells(before, static_cast<std::size_t>(std::max(room, 1)));
    const bool cut = shown_before != before;
    const std::size_t tail = shown_before.size() - (cut ? kEllipsis.size() : 0);
    InputCells result;
    if (cut)
        result.cells.push_back(ftxui::text(std::string(kEllipsis)));
    append_runs(result.cells, value, before.size() - tail, before.size(), spans);
    result.cells.push_back(ftxui::text(std::string(kCursor)));
    // Çizilen terminal_safe metin ölçülür: ham ESC ya da bozuk bayt 0 hücre sayılır, U+FFFD olarak 1 hücre
    // çizilir.
    int used = ftxui::string_width(terminal_safe(shown_before));
    if (const int after_cells = ftxui::string_width(terminal_safe(after)); after_cells <= room - used) {
        append_runs(result.cells, value, before.size(), value.size(), spans);
        used += after_cells;
    }
    if (const std::string shown_ghost = head_cells(terminal_safe(ghost), room - used); !shown_ghost.empty()) {
        result.cells.push_back(ftxui::text(shown_ghost) | ftxui::dim);
        used += ftxui::string_width(shown_ghost);
    }
    result.width = used;
    return result;
}

ftxui::Element input_line_bar(const std::string& label, const InputLine& line,
                              std::span<const InputSpan> spans, const std::string& ghost,
                              const std::string& hints, int width, const std::string& counter,
                              const std::string& error) {
    using namespace ftxui;
    // Etiket klasör adını, hata ve sayaç dosya adını taşıyabilir: girdi metni gibi çizimde arınır.
    const std::string prefix = terminal_safe(label) + " ";
    const int separator = static_cast<int>(kHintSeparator.size());
    // Uzun hata girdiyi yemesin: kullanıcı neyi düzelteceğini görmeli. Metin satırın yarısına dek
    // önceliklidir.
    const int kept_cells = std::min(string_width(line.text()), width / 2);
    const int note_room = width - string_width(prefix) - kCursorCells - separator - kept_cells;
    const std::string note =
        head_cells(terminal_safe(!error.empty() ? error : counter), std::max(note_room, 0));
    const int note_cells = note.empty() ? 0 : string_width(note) + separator;
    const int room = std::max(width - string_width(prefix) - kCursorCells - note_cells, 1);
    InputCells content = input_text_cells(line, spans, ghost, room);
    Elements cells = {text(prefix) | bold};
    std::ranges::move(content.cells, std::back_inserter(cells));
    if (!note.empty()) {
        Element shown_note = text(std::string(kHintSeparator) + note) | bold;
        cells.push_back(error.empty() ? shown_note : shown_note | color(Color::Red));
    }
    if (const std::string fitted = fit_hints(hints, room - content.width - separator); !fitted.empty()) {
        cells.push_back(filler());
        cells.push_back(text(fitted) | dim);
    }
    return hbox(std::move(cells));
}

ftxui::Element completion_popup(const std::string& query, const std::vector<std::string>& items,
                                std::size_t selected) {
    using namespace ftxui;
    Elements body = {hbox({text("> ") | bold, text(terminal_safe(query)), text(std::string(kCursor))})};
    for (std::size_t i = 0; i < items.size(); ++i) {
        Element row = text(terminal_safe(items[i]));
        body.push_back(i == selected ? row | inverted : row);
    }
    if (items.empty())
        body.push_back(text(core::tr("tui.editor_not_found")) | dim);
    body.push_back(text(core::tr("tui.completion_hints")) | dim);
    return window(text(" " + core::tr("tui.completion_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kPopupWidth) | clear_under;
}

ftxui::Element input_popup_box(const InputLine::Popup& popup) {
    return completion_popup(popup.query, popup.items, popup.selected);
}

}  // namespace noteapp::tui
