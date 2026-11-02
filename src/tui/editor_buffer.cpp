#include "tui/editor_buffer.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kLf = "\n";
constexpr std::string_view kUnprintable = "\xEF\xBF\xBD";   // U+FFFD: kontrol baytının ekrandaki yerine
constexpr std::string_view kDottedCircle = "\xE2\x97\x8C";  // U+25CC: tabansız birleşen işaretin tabanı

struct Decoded {
    char32_t code_point = 0;
    std::size_t length = 1;
};

// Tampon geçerli UTF-8 tuttuğundan (kurucu sözleşmesi) doğrulama yapılmaz; yalnız taşma önlenir.
Decoded decode_at(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80)
        return {lead, 1};
    std::size_t length = 2;
    if (lead >= 0xF0)
        length = 4;
    else if (lead >= 0xE0)
        length = 3;
    length = std::min(length, text.size() - at);
    auto code_point = static_cast<char32_t>(lead & (0xFFU >> (length + 1)));
    for (std::size_t i = 1; i < length; ++i)
        code_point = (code_point << 6U) | (static_cast<unsigned char>(text[at + i]) & 0x3FU);
    return {code_point, length};
}

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// FTXUI'nin IsControl tanımıyla aynı sınıf (C0, DEL, C1): C1 de bazı terminallerde kaçış dizisi başlatır.
bool is_control(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

std::size_t code_point_start(std::string_view text, std::size_t at) {
    while (at > 0 && is_continuation(text[at]))
        --at;
    return at;
}

std::size_t edit_bytes(std::string_view removed, std::string_view inserted) {
    return removed.size() + inserted.size() + sizeof(std::size_t) * 4;
}

std::string with_eol(std::string_view text, std::string_view eol) {
    std::string normalized = core::normalize_line_endings(text);
    if (eol == kLf)
        return normalized;
    std::string out;
    out.reserve(normalized.size());
    for (const char c : normalized) {
        if (c == '\n')
            out += eol;
        else
            out += c;
    }
    return out;
}

std::string detect_eol(const std::string& text) {
    return std::string(text.find(kCrlf) != std::string::npos ? kCrlf : kLf);
}

}  // namespace

EditorBuffer::EditorBuffer(std::string text, CellWidth cell_width, std::size_t undo_limit_bytes)
    : text_(std::move(text)), saved_(text_), cell_width_(cell_width), eol_(detect_eol(text_)),
      undo_limit_bytes_(undo_limit_bytes) {
    rebuild_line_starts();
}

std::string_view EditorBuffer::line_text(std::size_t line) const {
    return std::string_view(text_).substr(line_starts_[line], line_end(line) - line_starts_[line]);
}

void EditorBuffer::rebuild_line_starts() {
    row_cache_.reset();
    changed_from_ = 0;
    line_starts_.assign(1, 0);
    for (std::size_t at = text_.find('\n'); at != std::string::npos; at = text_.find('\n', at + 1))
        line_starts_.push_back(at + 1);
}

std::optional<ByteRange> EditorBuffer::selection() const {
    if (!anchor_ || *anchor_ == cursor_)
        return std::nullopt;
    return ByteRange{std::min(*anchor_, cursor_), std::max(*anchor_, cursor_)};
}

std::size_t EditorBuffer::next_boundary(std::size_t offset) const {
    if (offset >= text_.size())
        return text_.size();
    const auto lead = static_cast<unsigned char>(text_[offset]);
    // Metnin çoğu ASCII: uzun satırda her karede binlerce kez dönen yol ölçü fonksiyonuna gitmesin.
    if (lead >= 0x20 && lead < 0x7F &&
        (offset + 1 == text_.size() || static_cast<unsigned char>(text_[offset + 1]) < 0x80))
        return offset + 1;
    if (text_.compare(offset, kCrlf.size(), kCrlf) == 0)
        return offset + kCrlf.size();
    const Decoded base = decode_at(text_, offset);
    std::size_t at = offset + base.length;
    if (is_control(base.code_point))
        return at;
    while (at < text_.size()) {
        const Decoded next = decode_at(text_, at);
        if (is_control(next.code_point) || cell_width_(next.code_point) != 0)
            break;
        at += next.length;
    }
    return at;
}

std::size_t EditorBuffer::previous_boundary(std::size_t offset) const {
    if (offset == 0)
        return 0;
    if (offset >= kCrlf.size() && text_.compare(offset - kCrlf.size(), kCrlf.size(), kCrlf) == 0)
        return offset - kCrlf.size();
    std::size_t at = code_point_start(text_, offset - 1);
    while (at > 0) {
        const char32_t cp = decode_at(text_, at).code_point;
        if (is_control(cp) || cell_width_(cp) != 0)
            break;
        const std::size_t before = code_point_start(text_, at - 1);
        // Kontrol baytından hemen sonraki birleşen işaret kendi başına bir grapheme'dir.
        if (is_control(decode_at(text_, before).code_point))
            break;
        at = before;
    }
    return at;
}

bool EditorBuffer::is_boundary(std::size_t offset) const {
    if (offset == 0 || offset >= text_.size())
        return true;
    if (is_continuation(text_[offset]) || (text_[offset] == '\n' && text_[offset - 1] == '\r'))
        return false;
    const char32_t cp = decode_at(text_, offset).code_point;
    if (is_control(cp) || cell_width_(cp) != 0)
        return true;
    return is_control(decode_at(text_, code_point_start(text_, offset - 1)).code_point);
}

std::size_t EditorBuffer::line_of(std::size_t offset) const {
    const auto it = std::ranges::upper_bound(line_starts_, offset);
    return static_cast<std::size_t>(it - line_starts_.begin()) - 1;
}

std::size_t EditorBuffer::line_end(std::size_t line) const {
    if (line + 1 >= line_starts_.size())
        return text_.size();
    std::size_t end = line_starts_[line + 1] - 1;
    if (end > line_starts_[line] && text_[end - 1] == '\r')
        --end;
    return end;
}

// Satır dizini baştan kurulmaz: 10 MB'ta her tuşta tüm metni taramak gecikme bütçesini yerdi.
void EditorBuffer::replace_range(std::size_t at, std::size_t length, std::string_view inserted) {
    changed_from_ = std::min(changed_from_, line_of(at));
    text_.replace(at, length, inserted);
    row_cache_.reset();
    const auto first = std::upper_bound(line_starts_.begin(), line_starts_.end(), at);
    const auto last = std::upper_bound(first, line_starts_.end(), at + length);
    const auto index = first - line_starts_.begin();
    line_starts_.erase(first, last);
    for (auto it = line_starts_.begin() + index; it != line_starts_.end(); ++it)
        *it = *it + inserted.size() - length;
    std::vector<std::size_t> added;
    for (std::size_t i = inserted.find('\n'); i != std::string_view::npos; i = inserted.find('\n', i + 1))
        added.push_back(at + i + 1);
    line_starts_.insert(line_starts_.begin() + index, added.begin(), added.end());
}

bool EditorBuffer::coalesce(std::size_t at, std::string_view removed, std::string_view inserted,
                            EditKind kind) {
    if (!run_open_ || undo_.empty() || undo_.back().kind != kind)
        return false;
    Edit& last = undo_.back();
    switch (kind) {
    case EditKind::Typing: {
        // Kelime sınırında yeni adım: geri alma tüm paragrafı değil son kelimeyi siler.
        const bool new_word = last.inserted.ends_with(' ') && inserted != " ";
        if (!removed.empty() || new_word || at != last.at + last.inserted.size())
            return false;
        last.inserted += inserted;
        break;
    }
    case EditKind::Backspace:
        if (at + removed.size() != last.at)
            return false;
        last.removed.insert(0, removed);
        last.at = at;
        break;
    case EditKind::DeleteForward:
        if (at != last.at)
            return false;
        last.removed += removed;
        break;
    case EditKind::Other:
        return false;
    }
    undo_bytes_ += removed.size() + inserted.size();
    return true;
}

void EditorBuffer::trim_undo() {
    while (undo_bytes_ > undo_limit_bytes_ && !undo_.empty()) {
        const Edit& oldest = undo_.front();
        undo_bytes_ -= edit_bytes(oldest.removed, oldest.inserted);
        undo_.pop_front();
    }
}

void EditorBuffer::record_and_apply(std::size_t at, std::size_t length, std::string_view inserted,
                                    EditKind kind) {
    if (length == 0 && inserted.empty())
        return;
    const std::string removed = text_.substr(at, length);
    for (const Edit& edit : redo_)
        undo_bytes_ -= edit_bytes(edit.removed, edit.inserted);
    redo_.clear();
    if (!coalesce(at, removed, inserted, kind)) {
        undo_.push_back(Edit{at, removed, std::string(inserted), cursor_, anchor_, kind});
        undo_bytes_ += edit_bytes(removed, inserted);
    }
    replace_range(at, length, inserted);
    cursor_ = at + inserted.size();
    // Eklenen metnin ardı birleşen işaretse imleç grapheme ortasında kalmasın.
    while (!is_boundary(cursor_))
        cursor_ += decode_at(text_, cursor_).length;
    anchor_.reset();
    goal_cell_.reset();
    run_open_ = kind != EditKind::Other;
    trim_undo();
}

void EditorBuffer::insert_replacing_selection(std::string_view inserted, EditKind kind) {
    if (const auto range = selection())
        record_and_apply(range->begin, range->end - range->begin, inserted, kind);
    else
        record_and_apply(cursor_, 0, inserted, kind);
}

void EditorBuffer::type_text(std::string_view text) {
    if (core::is_valid_utf8(text))
        insert_replacing_selection(text, EditKind::Typing);
}

void EditorBuffer::paste(std::string_view text) {
    if (core::is_valid_utf8(text))
        insert_replacing_selection(with_eol(text, eol_), EditKind::Other);
}

void EditorBuffer::newline() {
    insert_replacing_selection(eol_, EditKind::Other);
}

void EditorBuffer::backspace() {
    if (selection()) {
        insert_replacing_selection({}, EditKind::Other);
        return;
    }
    const std::size_t begin = previous_boundary(cursor_);
    record_and_apply(begin, cursor_ - begin, {}, EditKind::Backspace);
}

void EditorBuffer::delete_forward() {
    if (selection()) {
        insert_replacing_selection({}, EditKind::Other);
        return;
    }
    record_and_apply(cursor_, next_boundary(cursor_) - cursor_, {}, EditKind::DeleteForward);
}

std::string EditorBuffer::copy() const {
    if (const auto range = selection())
        return text_.substr(range->begin, range->end - range->begin);
    const std::size_t line = line_of(cursor_);
    const std::size_t begin = line_starts_[line];
    const std::size_t end = line + 1 < line_starts_.size() ? line_starts_[line + 1] : text_.size();
    return text_.substr(begin, end - begin);
}

std::string EditorBuffer::cut() {
    std::string copied = copy();
    if (selection())
        insert_replacing_selection({}, EditKind::Other);
    else
        record_and_apply(line_starts_[line_of(cursor_)], copied.size(), {}, EditKind::Other);
    return copied;
}

bool EditorBuffer::undo() {
    if (undo_.empty())
        return false;
    Edit edit = std::move(undo_.back());
    undo_.pop_back();
    replace_range(edit.at, edit.inserted.size(), edit.removed);
    cursor_ = edit.cursor_before;
    anchor_ = edit.anchor_before;
    goal_cell_.reset();
    run_open_ = false;
    redo_.push_back(std::move(edit));
    return true;
}

bool EditorBuffer::redo() {
    if (redo_.empty())
        return false;
    Edit edit = std::move(redo_.back());
    redo_.pop_back();
    replace_range(edit.at, edit.removed.size(), edit.inserted);
    cursor_ = edit.at + edit.inserted.size();
    while (!is_boundary(cursor_))
        cursor_ += decode_at(text_, cursor_).length;
    anchor_.reset();
    goal_cell_.reset();
    run_open_ = false;
    undo_.push_back(std::move(edit));
    return true;
}

void EditorBuffer::begin_motion(bool extend) {
    if (!extend)
        anchor_.reset();
    else if (!anchor_)
        anchor_ = cursor_;
    goal_cell_.reset();
    run_open_ = false;
}

void EditorBuffer::move_left(bool extend) {
    const auto range = selection();
    const std::size_t target = !extend && range ? range->begin : previous_boundary(cursor_);
    begin_motion(extend);
    cursor_ = target;
}

void EditorBuffer::move_right(bool extend) {
    const auto range = selection();
    const std::size_t target = !extend && range ? range->end : next_boundary(cursor_);
    begin_motion(extend);
    cursor_ = target;
}

void EditorBuffer::move_home(bool extend) {
    begin_motion(extend);
    cursor_ = line_starts_[line_of(cursor_)];
}

void EditorBuffer::move_end(bool extend) {
    begin_motion(extend);
    cursor_ = line_end(line_of(cursor_));
}

void EditorBuffer::move_doc_start(bool extend) {
    begin_motion(extend);
    cursor_ = 0;
}

void EditorBuffer::move_doc_end(bool extend) {
    begin_motion(extend);
    cursor_ = text_.size();
}

void EditorBuffer::move_rows(int delta, int columns, bool extend) {
    const CursorCell here = cursor_cell(columns);
    const int goal = goal_cell_.value_or(here.cell);
    std::size_t line = line_of(cursor_);
    std::size_t row = here.row;
    std::vector<VisualRow> rows = rows_of_line(line, columns);
    for (int step = 0; step < std::abs(delta); ++step) {
        if (delta > 0 && row + 1 < rows.size()) {
            ++row;
        } else if (delta > 0 && line + 1 < line_starts_.size()) {
            rows = rows_of_line(++line, columns);
            row = 0;
        } else if (delta < 0 && row > 0) {
            --row;
        } else if (delta < 0 && line > 0) {
            rows = rows_of_line(--line, columns);
            row = rows.size() - 1;
        } else {
            break;
        }
    }
    begin_motion(extend);
    cursor_ = offset_at_cell(rows[row], goal, columns);
    goal_cell_ = goal;
}

void EditorBuffer::move_to(TextPosition position) {
    begin_motion(false);
    const std::size_t line = std::min(position.line, line_starts_.size() - 1);
    const std::size_t end = line_end(line);
    cursor_ = line_starts_[line];
    for (std::size_t column = 0; column < position.column && cursor_ < end; ++column)
        cursor_ = next_boundary(cursor_);
}

void EditorBuffer::move_to_cell(const VisualRow& row, int cell) {
    begin_motion(false);
    cursor_ = offset_at_cell(row, cell, 0);
}

void EditorBuffer::select_range(ByteRange range) {
    begin_motion(false);
    std::size_t begin = std::min(range.begin, text_.size());
    std::size_t end = std::min(std::max(range.end, begin), text_.size());
    while (!is_boundary(begin))
        begin = code_point_start(text_, begin - 1);
    const bool empty = range.begin >= range.end;
    while (!is_boundary(end))
        ++end;  // devam baytında olabilir: kod noktası çözülemez
    if (!empty && begin != end)
        anchor_ = begin;
    cursor_ = end;
}

void EditorBuffer::set_scroll_line(std::size_t line) {
    top_ = {std::min(line, line_starts_.size() - 1), 0};
}

TextPosition EditorBuffer::position() const {
    const std::size_t line = line_of(cursor_);
    std::size_t column = 0;
    for (std::size_t at = line_starts_[line]; at < cursor_; at = next_boundary(at))
        ++column;
    return {line, column};
}

Glyph EditorBuffer::glyph_at(std::size_t offset, int cell) const {
    const std::size_t next = next_boundary(offset);
    const Decoded base = decode_at(text_, offset);
    if (base.code_point == '\t') {
        const int cells = kTabCells - cell % kTabCells;
        return {next, cells, std::string(static_cast<std::size_t>(cells), ' ')};
    }
    if (is_control(base.code_point))
        return {next, 1, std::string(kUnprintable)};
    std::string shown = text_.substr(offset, next - offset);
    const int width = cell_width_(base.code_point);
    if (width == 0)
        return {next, 1, std::string(kDottedCircle) + shown};
    return {next, width, std::move(shown)};
}

const std::vector<VisualRow>& EditorBuffer::cached_rows(std::size_t line, int columns) const {
    if (!row_cache_ || row_cache_->line != line || row_cache_->columns != columns)
        row_cache_ = RowCache{line, columns, compute_rows(line, columns)};
    return row_cache_->rows;
}

std::vector<VisualRow> EditorBuffer::compute_rows(std::size_t line, int columns) const {
    const int limit = std::max(columns, kMinColumns);
    const std::size_t end = line_end(line);
    std::vector<VisualRow> rows;
    std::size_t row_begin = line_starts_[line];
    int cell = 0;
    for (std::size_t at = row_begin; at < end;) {
        const auto lead = static_cast<unsigned char>(text_[at]);
        const std::size_t next = next_boundary(at);
        // ASCII'de Glyph kurulmaz: uzun satırda her tuşta milyonlarca dizgi ayırmak gecikme bütçesini yerdi.
        const bool plain = next == at + 1 && lead >= 0x20 && lead < 0x7F;
        int cells = plain ? 1 : glyph_at(at, cell).cells;
        if (cell > 0 && cell + cells > limit) {
            rows.push_back({line, row_begin, at, false});
            row_begin = at;
            cell = 0;
            cells = plain ? 1 : glyph_at(at, cell).cells;
        }
        cell += cells;
        at = next;
    }
    if (cell >= limit) {  // tam dolu satırda satır sonu imlecine yer kalsın
        rows.push_back({line, row_begin, end, false});
        row_begin = end;
    }
    rows.push_back({line, row_begin, end, true});
    return rows;
}

CursorCell EditorBuffer::cursor_cell(int columns) const {
    const auto& rows = cached_rows(line_of(cursor_), columns);
    std::size_t row = rows.size() - 1;
    while (row > 0 && rows[row].begin > cursor_)
        --row;
    int cell = 0;
    for (std::size_t at = rows[row].begin; at < cursor_;) {
        const Glyph glyph = glyph_at(at, cell);
        cell += glyph.cells;
        at = glyph.next;
    }
    return {row, cell};
}

std::size_t EditorBuffer::offset_at_cell(const VisualRow& row, int cell, int) const {
    int at_cell = 0;
    std::size_t last_start = row.begin;
    for (std::size_t at = row.begin; at < row.end;) {
        const Glyph glyph = glyph_at(at, at_cell);
        if (at_cell + glyph.cells > cell)
            return at;
        at_cell += glyph.cells;
        last_start = at;
        at = glyph.next;
    }
    // Kaydırılan satırın bitiş ofseti sonraki görsel satırda görünür: imleç bu satırda kalsın diye son
    // grapheme.
    return row.last_of_line ? row.end : last_start;
}

void EditorBuffer::scroll_to_cursor(int columns, int rows) {
    const std::size_t cursor_line = line_of(cursor_);
    const std::size_t cursor_row = cursor_cell(columns).row;
    top_.line = std::min(top_.line, line_starts_.size() - 1);
    top_.row = std::min(top_.row, cached_rows(top_.line, columns).size() - 1);
    if (cursor_line < top_.line || (cursor_line == top_.line && cursor_row < top_.row)) {
        top_ = {cursor_line, cursor_row};
        return;
    }
    // İmleçten yukarı yürünür: pencerenin üstüne varmadan yer biterse imleç son satıra oturur.
    auto room = static_cast<std::size_t>(std::max(rows, 1) - 1);
    std::size_t line = cursor_line;
    std::size_t row = cursor_row;
    while (line > top_.line && row < room) {
        room -= row + 1;
        row = cached_rows(--line, columns).size() - 1;
    }
    if (line > top_.line)
        top_ = {line, row - room};
    else if (row > top_.row + room)
        top_.row = row - room;
}

std::vector<VisualRow> EditorBuffer::visible_rows(int columns, int rows) const {
    std::vector<VisualRow> visible;
    const auto wanted = static_cast<std::size_t>(std::max(rows, 0));
    std::size_t skip = top_.row;
    for (std::size_t line = top_.line; line < line_starts_.size() && visible.size() < wanted; ++line) {
        for (const VisualRow& row : cached_rows(line, columns)) {
            if (skip > 0)
                --skip;
            else if (visible.size() < wanted)
                visible.push_back(row);
        }
    }
    return visible;
}

void EditorBuffer::reload(std::string text) {
    const TextPosition kept = position();
    text_ = std::move(text);
    saved_ = text_;
    eol_ = detect_eol(text_);
    rebuild_line_starts();
    undo_.clear();
    redo_.clear();
    undo_bytes_ = 0;
    cursor_ = 0;
    move_to(kept);
}

}  // namespace noteapp::tui
