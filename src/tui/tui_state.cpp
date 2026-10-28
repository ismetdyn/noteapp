#include "tui/tui_state.hpp"

#include "search/fuzzy.hpp"

#include <algorithm>
#include <optional>

namespace noteapp::tui {

namespace {

// UTF-8 devam baytı (10xxxxxx): Backspace bir kod noktasının tamamını silsin.
bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Kontrol karakterleri sorguya girmez (terminal olaylarından sızabilir).
bool is_printable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20;
}

// İçerik modunda önizleme ilk eşleşmeye kayar; üstte birkaç satır bağlam kalır.
constexpr std::size_t kHitContextLines = 2;

std::size_t initial_scroll(const Row& row) {
    if (row.hits.empty() || row.hits.front().line == 0)
        return 0;
    const std::size_t line_index = row.hits.front().line - 1;  // Hit::line 1 tabanlı
    return line_index > kHitContextLines ? line_index - kHitContextLines : 0;
}

}  // namespace

bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b) {
    return std::ranges::equal(a, b, [](const core::NoteInfo& x, const core::NoteInfo& y) {
        return x.path == y.path && x.mtime == y.mtime && x.size == y.size && x.binary == y.binary;
    });
}

void TuiState::set_notes(std::vector<core::NoteInfo> notes) {
    notes_ = std::move(notes);
    refilter(false);
}

void TuiState::set_preview_line_count(std::size_t lines) {
    preview_lines_ = lines;
    scroll_preview(0);  // yeni sınıra kenetle
}

void TuiState::refilter(bool query_changed) {
    std::optional<core::NotePath> previous_path;
    if (const Row* row = selected_row())
        previous_path = row->path;

    std::vector<Row> rows;
    if (mode_ == Mode::Content && !query_.empty()) {
        // Arayıcı yoksa (henüz bağlanmadı) eşleşme yok sayılır; çökmek yerine boş liste.
        if (content_searcher_)
            for (auto& result : content_searcher_(query_))
                rows.push_back(
                    {std::move(result.path), {}, result.hit_count, std::move(result.first_hits), false});
    } else {
        // Ad modu ve içerik modunda boş sorgu: tüm notlar mtime sırasıyla.
        for (auto& ranked : search::rank_notes(query_, notes_)) {
            const core::NoteInfo& info = notes_[ranked.index];
            rows.push_back({info.path, std::move(ranked.match.positions), 0, {}, info.binary});
        }
    }
    rows_ = std::move(rows);

    selected_ = 0;
    if (previous_path) {
        const auto it = std::ranges::find(rows_, *previous_path, &Row::path);
        if (it != rows_.end())
            selected_ = static_cast<std::size_t>(it - rows_.begin());
    }
    const bool same_note = previous_path && selected_row() && selected_row()->path == *previous_path;
    if (!same_note || query_changed)
        preview_scroll_ = selected_row() ? initial_scroll(*selected_row()) : 0;
}

void TuiState::move_selection(int delta) {
    if (rows_.empty())
        return;
    const auto last = static_cast<int>(rows_.size()) - 1;
    const int next = std::clamp(static_cast<int>(selected_) + delta, 0, last);
    if (static_cast<std::size_t>(next) != selected_) {
        selected_ = static_cast<std::size_t>(next);
        preview_scroll_ = initial_scroll(rows_[selected_]);
    }
}

void TuiState::scroll_preview(int delta) {
    const std::size_t max_scroll = preview_lines_ == 0 ? 0 : preview_lines_ - 1;
    const std::size_t step = static_cast<std::size_t>(delta < 0 ? -delta : delta);
    const std::size_t next =
        delta < 0 ? (preview_scroll_ > step ? preview_scroll_ - step : 0) : preview_scroll_ + step;
    preview_scroll_ = std::min(next, max_scroll);
}

Intent TuiState::handle(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (is_printable(text)) {
            query_.append(text);
            refilter(true);
        }
        return Intent::None;
    case Key::Backspace:
        if (!query_.empty()) {
            while (is_continuation(query_.back()) && query_.size() > 1)
                query_.pop_back();
            query_.pop_back();  // baş bayt
            refilter(true);
        }
        return Intent::None;
    case Key::Up:
    case Key::CtrlP:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
    case Key::CtrlN:
        move_selection(1);
        return Intent::None;
    case Key::PageUp:
        scroll_preview(-static_cast<int>(kPreviewPageLines));
        return Intent::None;
    case Key::PageDown:
        scroll_preview(static_cast<int>(kPreviewPageLines));
        return Intent::None;
    case Key::Tab:
        mode_ = mode_ == Mode::Name ? Mode::Content : Mode::Name;
        refilter(true);
        return Intent::None;
    case Key::Enter:
        if (selected_row())
            return Intent::OpenSelected;
        return query_.empty() ? Intent::None : Intent::CreateFromQuery;
    case Key::Escape:
        if (query_.empty())
            return Intent::Quit;
        query_.clear();
        refilter(true);
        return Intent::None;
    case Key::CtrlC:
        return Intent::Quit;
    case Key::CtrlO:
        return Intent::NewNote;
    case Key::CtrlR:
        return selected_row() ? Intent::MoveSelected : Intent::None;
    case Key::CtrlD:
        return selected_row() ? Intent::DeleteSelected : Intent::None;
    case Key::CtrlS:
        return Intent::SyncNow;
    case Key::F1:
        return Intent::ShowHelp;
    case Key::F2:
        return Intent::ShowSettings;
    }
    return Intent::None;
}

}  // namespace noteapp::tui
