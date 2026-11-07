#include "edit/editor_modes.hpp"

#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "edit/note_find.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::size_t kCompletionRows = 8;

struct Motion {
    Key key;
    void (*apply)(EditorBuffer&, int columns, int rows);
};

// Hareketler salt-okunurda da çalışır: "→ yine de kaydırmaya izin verir".
constexpr Motion kMotions[] = {
    {Key::Left, [](EditorBuffer& b, int, int) { b.move_left(false); }},
    {Key::Right, [](EditorBuffer& b, int, int) { b.move_right(false); }},
    {Key::Up, [](EditorBuffer& b, int c, int) { b.move_rows(-1, c, false); }},
    {Key::Down, [](EditorBuffer& b, int c, int) { b.move_rows(1, c, false); }},
    {Key::Home, [](EditorBuffer& b, int, int) { b.move_home(false); }},
    {Key::End, [](EditorBuffer& b, int, int) { b.move_end(false); }},
    {Key::PageUp, [](EditorBuffer& b, int c, int r) { b.move_rows(-std::max(r - 1, 1), c, false); }},
    {Key::PageDown, [](EditorBuffer& b, int c, int r) { b.move_rows(std::max(r - 1, 1), c, false); }},
    {Key::CtrlHome, [](EditorBuffer& b, int, int) { b.move_doc_start(false); }},
    {Key::CtrlEnd, [](EditorBuffer& b, int, int) { b.move_doc_end(false); }},
    {Key::ShiftLeft, [](EditorBuffer& b, int, int) { b.move_left(true); }},
    {Key::ShiftRight, [](EditorBuffer& b, int, int) { b.move_right(true); }},
    {Key::ShiftUp, [](EditorBuffer& b, int c, int) { b.move_rows(-1, c, true); }},
    {Key::ShiftDown, [](EditorBuffer& b, int c, int) { b.move_rows(1, c, true); }},
    {Key::ShiftHome, [](EditorBuffer& b, int, int) { b.move_home(true); }},
    {Key::ShiftEnd, [](EditorBuffer& b, int, int) { b.move_end(true); }},
};

}  // namespace

bool apply_motion_key(EditorBuffer& buffer, Key key, int columns, int rows) {
    const auto* motion = std::ranges::find(kMotions, key, &Motion::key);
    if (motion == std::ranges::end(kMotions))
        return false;
    motion->apply(buffer, columns, rows);
    return true;
}

void apply_edit_key(EditorBuffer& buffer, Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (core::is_typable(text))
            buffer.type_text(text);
        break;
    case Key::Backspace:
        buffer.backspace();
        break;
    case Key::Delete:
        buffer.delete_forward();
        break;
    case Key::Enter:
        buffer.newline();
        break;
    case Key::Tab:
        buffer.type_text("\t");
        break;
    case Key::CtrlZ:
        buffer.undo();
        break;
    case Key::CtrlY:
        buffer.redo();
        break;
    default:
        break;
    }
}

bool EditorModes::accept_ghost(EditorBuffer& buffer, Key key, bool writable) {
    const std::string ghost = std::exchange(ghost_, {});
    if (ghost.empty() || !writable || (key != Key::Tab && key != Key::Right))
        return false;
    buffer.paste(ghost);
    find_matches_.clear();
    return true;
}

// Yalnız satır sonunda: metnin ortasında soluk yazı ardındaki metni örterdi.
void EditorModes::update_ghost(const EditorBuffer& buffer, bool writable, std::string_view type,
                               const WordTable* words) {
    ghost_.clear();
    const std::string& text = buffer.text();
    const std::size_t cursor = buffer.cursor();
    const bool at_line_end = cursor == text.size() || text[cursor] == '\n' || text[cursor] == '\r';
    if (!writable || !at_line_end || buffer.selection())
        return;
    ghost_ = suggest_completion(text, cursor, type, words, limits_).value_or(std::string{});
}

void EditorModes::open_completion(const EditorBuffer& buffer, std::string_view type, const WordTable* words) {
    const std::string& text = buffer.text();
    const std::size_t cursor = buffer.cursor();
    completion_candidates_ = completion_candidates(text, cursor, type, words, limits_);
    completion_query_ = std::string(word_prefix_at(text, cursor, limits_.word_bytes));
    completion_selected_ = 0;
    completion_inserts_.clear();
    ++completion_generation_;
    refresh_completion();
}

void EditorModes::add_completion_items(std::uint64_t generation,
                                       std::span<const core::lsp::CompletionItem> items) {
    if (generation != completion_generation_)
        return;
    std::vector<std::string> labels;
    for (const core::lsp::CompletionItem& item : items) {
        std::string label = terminal_safe(item.label);
        if (label.empty() || completion_inserts_.contains(label))
            continue;
        // Çok satırlı ya da denetim karakterli metin nota görünmeden kaçış dizisi veya satır sokardı.
        const bool plain = !item.insert_text.empty() && terminal_safe(item.insert_text) == item.insert_text;
        completion_inserts_.emplace(label, plain ? item.insert_text : label);
        labels.push_back(std::move(label));
    }
    // Seçim kaymasın: kullanıcı okları kullanırken gelen cevap seçili adayı değiştirmez.
    const std::string selected =
        completion_items_.empty() ? std::string{} : completion_items_[completion_selected_];
    // Yalnız bu cevabın etiketleriyle aynı olan adaylar düşer: aynı açılışa daha önce eklenmiş sunucu
    // önerileri de completion_inserts_'tedir, onlar silinmemeli (bir dilin birden çok sunucusu).
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

void EditorModes::refresh_completion() {
    completion_items_ = filter_candidates(completion_candidates_, completion_query_, kCompletionRows);
    completion_selected_ =
        std::min(completion_selected_, completion_items_.empty() ? 0 : completion_items_.size() - 1);
}

void EditorModes::insert_completion(EditorBuffer& buffer) {
    const std::size_t cursor = buffer.cursor();
    const std::size_t typed = word_prefix_at(buffer.text(), cursor, limits_.word_bytes).size();
    buffer.select_range({cursor - typed, cursor});
    const std::string& label = completion_items_[completion_selected_];
    const auto lsp_insert = completion_inserts_.find(label);
    buffer.paste(lsp_insert == completion_inserts_.end() ? label : lsp_insert->second);
    find_matches_.clear();
}

EditorModes::CompleteResult EditorModes::handle_complete(EditorBuffer& buffer, Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (core::is_typable(text)) {
            completion_query_.append(text);
            refresh_completion();
        }
        break;
    case Key::Backspace:
        core::pop_code_point(completion_query_);
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
        if (completion_items_.empty())
            return CompleteResult::Closed;
        insert_completion(buffer);
        return CompleteResult::Inserted;
    case Key::Escape:
    case Key::CtrlSpace:
        return CompleteResult::Closed;
    default:
        break;
    }
    return CompleteResult::Open;
}

void EditorModes::open_find(const EditorBuffer& buffer, std::string_view type, const WordTable* words) {
    ghost_.clear();
    find_origin_ = buffer.cursor();
    find_line_.clear();
    find_line_.set_sources(
        word_input_sources(completion_candidates(buffer.text(), buffer.cursor(), type, words, limits_)));
    find_matches_.clear();
    find_missed_ = false;
}

void EditorModes::close_find() {
    find_matches_.clear();
    find_missed_ = false;
}

std::string EditorModes::find_label() const {
    if (find_matches_.empty())
        return {};
    return match_label(find_index_, find_matches_.size());
}

EditorModes::FindResult EditorModes::handle_find(EditorBuffer& buffer, Key key, std::string_view text) {
    const InputLine::Result result = find_line_.handle(key, text);
    if ((key == Key::Backspace || result == InputLine::Result::Edited) && !find_line_.popup()) {
        update_find(buffer);
        return FindResult::Searched;  // boş sorguda Ignored Backspace'in aşağıda karşılığı yok
    }
    if (result != InputLine::Result::Ignored)
        return FindResult::Handled;
    switch (key) {
    case Key::Enter:
    case Key::Down:
    case Key::CtrlN:
    case Key::F3:
        return step_find(buffer, 1) ? FindResult::Searched : FindResult::Handled;
    case Key::Up:
    case Key::CtrlP:
        return step_find(buffer, -1) ? FindResult::Searched : FindResult::Handled;
    case Key::Escape:
        close_find();
        return FindResult::Closed;
    default:
        return FindResult::Ignored;
    }
}

void EditorModes::update_find(EditorBuffer& buffer) {
    buffer.select_range({find_origin_, find_origin_});
    recompute_find(buffer, find_origin_);
    select_current_match(buffer);
}

bool EditorModes::step_find(EditorBuffer& buffer, int delta) {
    if (find_line_.text().empty())
        return false;
    if (find_matches_.empty()) {
        // Düzenleme eşleşmeleri düşürdü: imleçten sonraki ilk eşleşmeye gidilir, atlanmaz.
        recompute_find(buffer, buffer.cursor());
    } else {
        const std::size_t count = find_matches_.size();
        find_index_ = (find_index_ + count + (delta > 0 ? 1 : count - 1)) % count;
    }
    select_current_match(buffer);
    return true;
}

// Eşleşmeleri yeniden hesaplar; geçerli eşleşme `from`dan sonraki ilkidir, yoksa başa sarar.
void EditorModes::recompute_find(const EditorBuffer& buffer, std::size_t from) {
    find_matches_ = find_folded(buffer.text(), find_line_.text());
    const auto next =
        std::ranges::find_if(find_matches_, [from](const ByteRange& m) { return m.begin >= from; });
    find_index_ = next == find_matches_.end() ? 0 : static_cast<std::size_t>(next - find_matches_.begin());
}

void EditorModes::select_current_match(EditorBuffer& buffer) {
    find_missed_ = find_matches_.empty() && !find_line_.text().empty();
    if (!find_matches_.empty())
        buffer.select_range(find_matches_[find_index_]);
}

const DiagnosticMark* EditorModes::diagnostic_at_cursor(const EditorBuffer& buffer) const {
    const std::size_t cursor = buffer.cursor();
    const auto found = std::ranges::find_if(diagnostics_, [cursor](const DiagnosticMark& mark) {
        return cursor >= mark.range.begin && cursor <= mark.range.end;
    });
    return found == diagnostics_.end() ? nullptr : &*found;
}

}  // namespace noteapp::tui
