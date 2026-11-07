#include "gui/note_editor.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace noteapp::gui {

namespace {

using tui::Key;

struct CodeRange {
    char32_t first;
    char32_t last;
};

// Birleşen işaretler (genişlik 0): Latin/Yunan/Kiril aksanları, Arap ve İbrani harekeleri, Hint ve Tay
// ünlüleri, değişken seçiciler.
// ponytail: Unicode Mn/Me sınıfının sık kullanılan alt kümesi; eksik yazı sisteminde işaret ayrı hücreye
// düşer (veri bozulmaz, imleç işaretin önünde durabilir). Yükseltme: tam UCD tablosu üretmek.
constexpr std::array kZeroWidth = {
    CodeRange{0x0300, 0x036F}, CodeRange{0x0483, 0x0489}, CodeRange{0x0591, 0x05BD}, CodeRange{0x05BF, 0x05BF},
    CodeRange{0x05C1, 0x05C2}, CodeRange{0x05C4, 0x05C5}, CodeRange{0x05C7, 0x05C7}, CodeRange{0x0610, 0x061A},
    CodeRange{0x064B, 0x065F}, CodeRange{0x0670, 0x0670}, CodeRange{0x06D6, 0x06DC}, CodeRange{0x06DF, 0x06E4},
    CodeRange{0x06E7, 0x06E8}, CodeRange{0x06EA, 0x06ED}, CodeRange{0x0900, 0x0902}, CodeRange{0x093A, 0x093A},
    CodeRange{0x093C, 0x093C}, CodeRange{0x0941, 0x0948}, CodeRange{0x094D, 0x094D}, CodeRange{0x0951, 0x0957},
    CodeRange{0x0962, 0x0963}, CodeRange{0x0E31, 0x0E31}, CodeRange{0x0E34, 0x0E3A}, CodeRange{0x0E47, 0x0E4E},
    CodeRange{0x1160, 0x11FF}, CodeRange{0x1AB0, 0x1AFF}, CodeRange{0x1DC0, 0x1DFF}, CodeRange{0x20D0, 0x20FF},
    CodeRange{0x302A, 0x302D}, CodeRange{0x3099, 0x309A}, CodeRange{0xFE00, 0xFE0F}, CodeRange{0xFE20, 0xFE2F},
    CodeRange{0xE0100, 0xE01EF},
};

// Doğu Asya geniş ve tam genişlikli karakterler, emoji blokları (genişlik 2).
constexpr std::array kWide = {
    CodeRange{0x1100, 0x115F},   CodeRange{0x231A, 0x231B},   CodeRange{0x2329, 0x232A},
    CodeRange{0x23E9, 0x23EC},   CodeRange{0x23F0, 0x23F0},   CodeRange{0x23F3, 0x23F3},
    CodeRange{0x25FD, 0x25FE},   CodeRange{0x2614, 0x2615},   CodeRange{0x2648, 0x2653},
    CodeRange{0x267F, 0x267F},   CodeRange{0x2693, 0x2693},   CodeRange{0x26A1, 0x26A1},
    CodeRange{0x26AA, 0x26AB},   CodeRange{0x26BD, 0x26BE},   CodeRange{0x26C4, 0x26C5},
    CodeRange{0x26CE, 0x26CE},   CodeRange{0x26D4, 0x26D4},   CodeRange{0x26EA, 0x26EA},
    CodeRange{0x26F2, 0x26F3},   CodeRange{0x26F5, 0x26F5},   CodeRange{0x26FA, 0x26FA},
    CodeRange{0x26FD, 0x26FD},   CodeRange{0x2705, 0x2705},   CodeRange{0x270A, 0x270B},
    CodeRange{0x2728, 0x2728},   CodeRange{0x274C, 0x274C},   CodeRange{0x274E, 0x274E},
    CodeRange{0x2753, 0x2755},   CodeRange{0x2757, 0x2757},   CodeRange{0x2795, 0x2797},
    CodeRange{0x27B0, 0x27B0},   CodeRange{0x27BF, 0x27BF},   CodeRange{0x2B1B, 0x2B1C},
    CodeRange{0x2B50, 0x2B50},   CodeRange{0x2B55, 0x2B55},   CodeRange{0x2E80, 0x3029},
    CodeRange{0x302E, 0x303E},   CodeRange{0x3041, 0x3098},   CodeRange{0x309B, 0x33FF},
    CodeRange{0x3400, 0x4DBF},   CodeRange{0x4E00, 0x9FFF},   CodeRange{0xA000, 0xA4CF},
    CodeRange{0xA960, 0xA97F},   CodeRange{0xAC00, 0xD7A3},   CodeRange{0xF900, 0xFAFF},
    CodeRange{0xFE10, 0xFE19},   CodeRange{0xFE30, 0xFE6F},   CodeRange{0xFF00, 0xFF60},
    CodeRange{0xFFE0, 0xFFE6},   CodeRange{0x16FE0, 0x16FE4}, CodeRange{0x17000, 0x18AFF},
    CodeRange{0x1B000, 0x1B2FF}, CodeRange{0x1F004, 0x1F004}, CodeRange{0x1F0CF, 0x1F0CF},
    CodeRange{0x1F18E, 0x1F18E}, CodeRange{0x1F191, 0x1F19A}, CodeRange{0x1F200, 0x1F2FF},
    CodeRange{0x1F300, 0x1F64F}, CodeRange{0x1F680, 0x1F6FF}, CodeRange{0x1F7E0, 0x1F7EB},
    CodeRange{0x1F900, 0x1F9FF}, CodeRange{0x1FA70, 0x1FAFF}, CodeRange{0x20000, 0x2FFFD},
    CodeRange{0x30000, 0x3FFFD},
};

template <std::size_t N> bool in_ranges(const std::array<CodeRange, N>& ranges, char32_t code_point) {
    const auto found = std::ranges::upper_bound(ranges, code_point, {}, &CodeRange::first);
    return found != ranges.begin() && code_point <= std::prev(found)->last;
}

constexpr int kWheelRows = 3;  // TUI tekerlek adımı

}  // namespace

int cell_width(char32_t code_point) {
    if (in_ranges(kZeroWidth, code_point))
        return 0;
    return in_ranges(kWide, code_point) ? 2 : 1;
}

NoteEditor::NoteEditor(std::string text, bool writable, std::string type, EditorLimits limits)
    : buffer_(std::move(text), &cell_width, limits.undo_bytes), writable_(writable), limits_(limits) {
    set_type(std::move(type));
}

void NoteEditor::set_writable(bool writable) {
    writable_ = writable;
    if (writable)
        return;
    modes_.clear_ghost();
    if (mode_ == Mode::Complete)
        mode_ = Mode::Editing;
}

void NoteEditor::set_type(std::string type) {
    if (type == type_)
        return;
    language_ = tui::syntax_language_for(type);
    type_ = std::move(type);
    syntax_cache_.clear();
}

std::vector<tui::SyntaxSpan> NoteEditor::syntax_spans(std::size_t line) const {
    if (language_ == tui::SyntaxLanguage::Plain || buffer_.text().size() > limits_.syntax.note_bytes)
        return {};
    syntax_cache_.invalidate_from(buffer_.take_changed_line());
    const tui::SyntaxState entry = syntax_cache_.entry_state(
        language_, line, [this](std::size_t index) { return buffer_.line_text(index); },
        limits_.syntax.line_bytes);
    std::vector<tui::SyntaxSpan> spans;
    tui::highlight_line(language_, buffer_.line_text(line), entry, &spans, limits_.syntax.line_bytes);
    return spans;
}

void NoteEditor::layout(int columns, int rows) {
    columns_ = std::max(columns, tui::EditorBuffer::kMinColumns);
    rows_ = std::max(rows, 1);
    buffer_.scroll_to_cursor(columns_, rows_);
}

void NoteEditor::click(std::size_t visible_row, int cell, bool extend) {
    if (mode_ != Mode::Editing)
        return;
    const std::vector<tui::VisualRow> rows = buffer_.visible_rows(columns_, rows_);
    if (rows.empty())
        return;
    modes_.clear_ghost();
    const tui::VisualRow& row = rows[std::min(visible_row, rows.size() - 1)];
    if (extend)
        buffer_.extend_to_cell(row, std::max(cell, 0));
    else
        buffer_.move_to_cell(row, std::max(cell, 0));
}

void NoteEditor::wheel(int rows) {
    if (rows == 0 || mode_ == Mode::Complete)
        return;
    modes_.clear_ghost();
    buffer_.move_rows(rows * kWheelRows, columns_, false);
}

void NoteEditor::handle(Key key, std::string_view text, const tui::WordTable* words) {
    switch (mode_) {
    case Mode::Editing:
        handle_editing(key, text, words);
        break;
    case Mode::Find:
        handle_find(key, text);
        break;
    case Mode::Complete:
        handle_complete(key, text);
        break;
    }
}

void NoteEditor::handle_editing(Key key, std::string_view text, const tui::WordTable* words) {
    if (modes_.accept_ghost(buffer_, key, writable_))  // her tuş öneriyi düşürür; yazım yenisini üretir
        return;
    if (tui::apply_motion_key(buffer_, key, columns_, rows_))
        return;
    switch (key) {
    case Key::Escape:
        buffer_.clear_selection();  // TUI gibi: Esc hiçbir şey atmaz
        return;
    case Key::CtrlF:
        open_find(words);
        return;
    case Key::F3:
        step_find(1);
        return;
    default:
        break;
    }
    if (!writable_)
        return;
    if (key == Key::CtrlSpace) {
        modes_.open_completion(buffer_, type_, words);
        mode_ = Mode::Complete;
        return;
    }
    tui::apply_edit_key(buffer_, key, text);
    modes_.clear_find_matches();
    if (key == Key::Char)
        modes_.update_ghost(buffer_, writable_, type_, words);
}

std::string NoteEditor::cut() {
    if (!writable_)
        return copy();
    modes_.clear_ghost();
    modes_.clear_find_matches();
    return buffer_.cut();
}

void NoteEditor::paste(std::string_view text) {
    if (!writable_ || text.empty())
        return;
    modes_.clear_ghost();
    buffer_.paste(text);
    modes_.clear_find_matches();
}

// Baytlar aynen girer (yapıştırmadaki satır sonu çevrimi yok): programatik metin olduğu gibi yazılır.
void NoteEditor::replace_all(std::string_view text) {
    if (!writable_ || !core::is_valid_utf8(text))
        return;
    buffer_.select_range({0, buffer_.text().size()});
    if (text.empty())
        buffer_.backspace();
    else
        buffer_.type_text(text);
    modes_.clear_ghost();
    modes_.clear_find_matches();
}

void NoteEditor::add_completion_items(std::uint64_t generation,
                                      std::span<const core::lsp::CompletionItem> items) {
    if (mode_ == Mode::Complete)
        modes_.add_completion_items(generation, items);
}

void NoteEditor::handle_complete(Key key, std::string_view text) {
    if (modes_.handle_complete(buffer_, key, text) != tui::EditorModes::CompleteResult::Open)
        mode_ = Mode::Editing;
}

void NoteEditor::open_find(const tui::WordTable* words) {
    modes_.open_find(buffer_, type_, words);
    mode_ = Mode::Find;
}

void NoteEditor::step_find(int delta) {
    modes_.step_find(buffer_, delta);
}

void NoteEditor::close_find() {
    modes_.close_find();
    mode_ = Mode::Editing;
}

void NoteEditor::handle_find(Key key, std::string_view text) {
    if (modes_.handle_find(buffer_, key, text) == tui::EditorModes::FindResult::Closed)
        mode_ = Mode::Editing;
}

}  // namespace noteapp::gui
