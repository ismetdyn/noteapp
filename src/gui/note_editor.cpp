#include "gui/note_editor.hpp"

#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "edit/note_find.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <set>
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

constexpr char kDelete = 0x7F;
constexpr std::size_t kCompletionRows = 8;  // TUI ile aynı liste boyu
constexpr int kWheelRows = 3;               // TUI tekerlek adımı

bool is_typable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20 && text.front() != kDelete &&
           core::is_valid_utf8(text);
}

void pop_code_point(std::string& text) {
    while (text.size() > 1 && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U)
        text.pop_back();
    if (!text.empty())
        text.pop_back();
}

struct Motion {
    Key key;
    void (*apply)(tui::EditorBuffer&, int columns, int rows);
};

// TUI EditorSession'daki tabloyla aynı: hareketler salt-okunurda da çalışır.
constexpr Motion kMotions[] = {
    {Key::Left, [](tui::EditorBuffer& b, int, int) { b.move_left(false); }},
    {Key::Right, [](tui::EditorBuffer& b, int, int) { b.move_right(false); }},
    {Key::Up, [](tui::EditorBuffer& b, int c, int) { b.move_rows(-1, c, false); }},
    {Key::Down, [](tui::EditorBuffer& b, int c, int) { b.move_rows(1, c, false); }},
    {Key::Home, [](tui::EditorBuffer& b, int, int) { b.move_home(false); }},
    {Key::End, [](tui::EditorBuffer& b, int, int) { b.move_end(false); }},
    {Key::PageUp, [](tui::EditorBuffer& b, int c, int r) { b.move_rows(-std::max(r - 1, 1), c, false); }},
    {Key::PageDown, [](tui::EditorBuffer& b, int c, int r) { b.move_rows(std::max(r - 1, 1), c, false); }},
    {Key::CtrlHome, [](tui::EditorBuffer& b, int, int) { b.move_doc_start(false); }},
    {Key::CtrlEnd, [](tui::EditorBuffer& b, int, int) { b.move_doc_end(false); }},
    {Key::ShiftLeft, [](tui::EditorBuffer& b, int, int) { b.move_left(true); }},
    {Key::ShiftRight, [](tui::EditorBuffer& b, int, int) { b.move_right(true); }},
    {Key::ShiftUp, [](tui::EditorBuffer& b, int c, int) { b.move_rows(-1, c, true); }},
    {Key::ShiftDown, [](tui::EditorBuffer& b, int c, int) { b.move_rows(1, c, true); }},
    {Key::ShiftHome, [](tui::EditorBuffer& b, int, int) { b.move_home(true); }},
    {Key::ShiftEnd, [](tui::EditorBuffer& b, int, int) { b.move_end(true); }},
};

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
    ghost_.clear();
    const tui::VisualRow& row = rows[std::min(visible_row, rows.size() - 1)];
    if (extend)
        buffer_.extend_to_cell(row, std::max(cell, 0));
    else
        buffer_.move_to_cell(row, std::max(cell, 0));
}

void NoteEditor::wheel(int rows) {
    if (rows == 0 || mode_ == Mode::Complete)
        return;
    ghost_.clear();
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
    const std::string ghost = std::exchange(ghost_, {});  // her tuş öneriyi düşürür; yazım yenisini üretir
    if (!ghost.empty() && writable_ && (key == Key::Tab || key == Key::Right)) {
        buffer_.paste(ghost);
        find_matches_.clear();
        return;
    }
    if (apply_motion(key))
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
        open_completion(words);
        return;
    }
    apply_edit(key, text);
    find_matches_.clear();  // ofsetler eski metne ait; F3 yeniden hesaplar
    if (key == Key::Char)
        update_ghost(words);
}

bool NoteEditor::apply_motion(Key key) {
    const auto* motion = std::ranges::find(kMotions, key, &Motion::key);
    if (motion == std::ranges::end(kMotions))
        return false;
    motion->apply(buffer_, columns_, rows_);
    return true;
}

void NoteEditor::apply_edit(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_typable(text))
            buffer_.type_text(text);
        break;
    case Key::Backspace:
        buffer_.backspace();
        break;
    case Key::Delete:
        buffer_.delete_forward();
        break;
    case Key::Enter:
        buffer_.newline();
        break;
    case Key::Tab:
        buffer_.type_text("\t");
        break;
    case Key::CtrlZ:
        buffer_.undo();
        break;
    case Key::CtrlY:
        buffer_.redo();
        break;
    default:
        break;
    }
}

std::string NoteEditor::cut() {
    if (!writable_)
        return copy();
    ghost_.clear();
    find_matches_.clear();
    return buffer_.cut();
}

void NoteEditor::paste(std::string_view text) {
    if (!writable_ || text.empty())
        return;
    ghost_.clear();
    buffer_.paste(text);
    find_matches_.clear();
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
    ghost_.clear();
    find_matches_.clear();
}

// Yalnız satır sonunda: metnin ortasında soluk yazı ardındaki metni örterdi.
void NoteEditor::update_ghost(const tui::WordTable* words) {
    ghost_.clear();
    const std::string& text = buffer_.text();
    const std::size_t cursor = buffer_.cursor();
    const bool at_line_end = cursor == text.size() || text[cursor] == '\n' || text[cursor] == '\r';
    if (!writable_ || !at_line_end || buffer_.selection())
        return;
    ghost_ = tui::suggest_completion(text, cursor, type_, words, limits_.completion).value_or(std::string{});
}

void NoteEditor::open_completion(const tui::WordTable* words) {
    const std::string& text = buffer_.text();
    const std::size_t cursor = buffer_.cursor();
    completion_candidates_ = tui::completion_candidates(text, cursor, type_, words, limits_.completion);
    completion_query_ = std::string(tui::word_prefix_at(text, cursor, limits_.completion.word_bytes));
    completion_selected_ = 0;
    completion_inserts_.clear();
    ++completion_generation_;
    refresh_completion();
    mode_ = Mode::Complete;
}

void NoteEditor::add_completion_items(std::uint64_t generation,
                                      std::span<const core::lsp::CompletionItem> items) {
    if (mode_ != Mode::Complete || generation != completion_generation_)
        return;
    std::vector<std::string> labels;
    for (const core::lsp::CompletionItem& item : items) {
        std::string label = tui::terminal_safe(item.label);
        if (label.empty() || completion_inserts_.contains(label))
            continue;
        // Çok satırlı ya da denetim karakterli metin nota görünmeden satır ya da kaçış sokardı.
        const bool plain = !item.insert_text.empty() && tui::terminal_safe(item.insert_text) == item.insert_text;
        completion_inserts_.emplace(label, plain ? item.insert_text : label);
        labels.push_back(std::move(label));
    }
    // Seçim kaymasın: kullanıcı okları kullanırken gelen cevap seçili adayı değiştirmez.
    const std::string selected =
        completion_items_.empty() ? std::string{} : completion_items_[completion_selected_];
    // Yalnız bu cevabın etiketleri düşer: aynı açılışa daha önce eklenmiş sunucu önerileri kalır.
    const std::set<std::string_view> added(labels.begin(), labels.end());
    std::erase_if(completion_candidates_, [&added](const std::string& word) { return added.contains(word); });
    labels.insert(labels.end(), std::make_move_iterator(completion_candidates_.begin()),
                  std::make_move_iterator(completion_candidates_.end()));
    completion_candidates_ = std::move(labels);
    refresh_completion();
    const auto kept = std::ranges::find(completion_items_, selected);
    completion_selected_ =
        kept == completion_items_.end() ? 0 : static_cast<std::size_t>(kept - completion_items_.begin());
}

const tui::DiagnosticMark* NoteEditor::diagnostic_at_cursor() const {
    const std::size_t cursor = buffer_.cursor();
    const auto found = std::ranges::find_if(diagnostics_, [cursor](const tui::DiagnosticMark& mark) {
        return cursor >= mark.range.begin && cursor <= mark.range.end;
    });
    return found == diagnostics_.end() ? nullptr : &*found;
}

void NoteEditor::refresh_completion() {
    completion_items_ = tui::filter_candidates(completion_candidates_, completion_query_, kCompletionRows);
    completion_selected_ =
        std::min(completion_selected_, completion_items_.empty() ? 0 : completion_items_.size() - 1);
}

void NoteEditor::insert_completion() {
    if (completion_items_.empty())
        return;
    const std::size_t cursor = buffer_.cursor();
    const std::size_t typed = tui::word_prefix_at(buffer_.text(), cursor, limits_.completion.word_bytes).size();
    buffer_.select_range({cursor - typed, cursor});
    const std::string& label = completion_items_[completion_selected_];
    const auto lsp_insert = completion_inserts_.find(label);
    buffer_.paste(lsp_insert == completion_inserts_.end() ? label : lsp_insert->second);
    find_matches_.clear();
}

void NoteEditor::handle_complete(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_typable(text)) {
            completion_query_.append(text);
            refresh_completion();
        }
        break;
    case Key::Backspace:
        pop_code_point(completion_query_);
        refresh_completion();
        break;
    case Key::Up:
    case Key::CtrlP:
        completion_selected_ -= completion_selected_ > 0 ? 1 : 0;
        break;
    case Key::Down:
    case Key::CtrlN:
        completion_selected_ += completion_selected_ + 1 < completion_items_.size() ? 1 : 0;
        break;
    case Key::Enter:
        insert_completion();
        mode_ = Mode::Editing;
        break;
    case Key::Escape:
    case Key::CtrlSpace:
        mode_ = Mode::Editing;
        break;
    default:
        break;
    }
}

void NoteEditor::open_find(const tui::WordTable* words) {
    ghost_.clear();
    mode_ = Mode::Find;
    find_origin_ = buffer_.cursor();
    find_line_.clear();
    find_line_.set_sources(tui::word_input_sources(
        tui::completion_candidates(buffer_.text(), buffer_.cursor(), type_, words, limits_.completion)));
    find_matches_.clear();
    find_missed_ = false;
}

void NoteEditor::close_find() {
    mode_ = Mode::Editing;
    find_matches_.clear();
    find_missed_ = false;
}

std::string NoteEditor::find_label() const {
    if (find_matches_.empty())
        return {};
    return tui::match_label(find_index_, find_matches_.size());
}

void NoteEditor::handle_find(Key key, std::string_view text) {
    const tui::InputLine::Result result = find_line_.handle(key, text);
    if ((key == Key::Backspace || result == tui::InputLine::Result::Edited) && !find_line_.popup())
        update_find();
    if (result != tui::InputLine::Result::Ignored)
        return;
    switch (key) {
    case Key::Enter:
    case Key::Down:
    case Key::CtrlN:
    case Key::F3:
        step_find(1);
        break;
    case Key::Up:
    case Key::CtrlP:
        step_find(-1);
        break;
    case Key::Escape:
        close_find();
        break;
    default:
        break;
    }
}

void NoteEditor::update_find() {
    buffer_.select_range({find_origin_, find_origin_});
    recompute_find(find_origin_);
    select_current_match();
}

void NoteEditor::step_find(int delta) {
    if (find_line_.text().empty())
        return;
    if (find_matches_.empty()) {
        // Düzenleme eşleşmeleri düşürdü: imleçten sonraki ilk eşleşmeye gidilir, atlanmaz.
        recompute_find(buffer_.cursor());
    } else {
        const std::size_t count = find_matches_.size();
        find_index_ = (find_index_ + count + (delta > 0 ? 1 : count - 1)) % count;
    }
    select_current_match();
}

// Eşleşmeleri yeniden hesaplar; geçerli eşleşme `from`dan sonraki ilkidir, yoksa başa sarar.
void NoteEditor::recompute_find(std::size_t from) {
    find_matches_ = tui::find_folded(buffer_.text(), find_line_.text());
    const auto next =
        std::ranges::find_if(find_matches_, [from](const tui::ByteRange& m) { return m.begin >= from; });
    find_index_ = next == find_matches_.end() ? 0 : static_cast<std::size_t>(next - find_matches_.begin());
}

void NoteEditor::select_current_match() {
    find_missed_ = find_matches_.empty() && !find_line_.text().empty();
    if (!find_matches_.empty())
        buffer_.select_range(find_matches_[find_index_]);
}

}  // namespace noteapp::gui
