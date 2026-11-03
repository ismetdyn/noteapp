#include "tui/mouse.hpp"

#include "edit/editor_buffer.hpp"
#include "tui/editor_view.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <ftxui/screen/string.hpp>
#include <limits>

namespace noteapp::tui {

namespace {

constexpr int kFirstPaneRow = 2;      // üst çubuk + panel kenarı
constexpr int kBottomChromeRows = 3;  // panel kenarı, toplamlar ya da editör durumu, alt çubuk
constexpr int kWheelStep = 3;
constexpr int kFrameCells = 1;  // panel kenarı

bool in_pane_rows(const TuiState& state, int y) {
    return y >= kFirstPaneRow && y < state.window_height() - kBottomChromeRows;
}

int pane_rows(const TuiState& state) {
    return state.window_height() - kFirstPaneRow - kBottomChromeRows;
}

// Bağlantı bölümü panelin altındadır: ayırıcı dahil ilk satırı.
int links_first_row(const TuiState& state) {
    return kFirstPaneRow + pane_rows(state) - state.links_reserved_rows(pane_rows(state));
}

bool over_links(const TuiState& state, int y) {
    return state.links_reserved_rows(pane_rows(state)) > 0 && y >= links_first_row(state);
}

bool over_right(int x, const PaneGeometry& geometry) {
    return geometry.right_visible && x >= geometry.right_x;
}

// Soru, arama ya da ad girdisi açıkken fare bağlamı değiştirmez: tuşlar neyi beklediğini bilmeli.
bool editor_busy(const TuiState& state, const EditorSession& editor) {
    return state.focus() == Focus::Editor && editor.mode() != EditorSession::Mode::Editing;
}

void handle_wheel(const MouseInput& input, TuiState& state, EditorSession& editor, bool right) {
    const int direction = input.button == MouseButton::WheelUp ? -1 : 1;
    if (state.focus() == Focus::Editor) {
        if (right)
            for (int i = 0; i < kWheelStep; ++i)
                editor.handle(direction < 0 ? Key::Up : Key::Down);
        return;
    }
    if (right && over_links(state, input.y))
        state.scroll_links(direction * kWheelStep);
    else if (right)
        state.scroll_preview(direction * kWheelStep);
    else
        state.move_selection(direction);
}

// Editör açıksa kapatmayı dener; false: kaydedilmemiş değişiklik soruluyor, tıklama burada biter.
bool leave_editor(TuiState& state, EditorSession& editor) {
    if (state.focus() != Focus::Editor)
        return true;
    editor.handle(Key::CtrlQ);
    return state.focus() != Focus::Editor;
}

MouseResult click_list(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (!leave_editor(state, editor))
        return MouseResult::None;
    const auto visible = static_cast<std::size_t>(state.window_height() - kFirstPaneRow - kBottomChromeRows);
    const auto line = static_cast<std::size_t>(input.y - kFirstPaneRow);
    // Seçim pencereyi kaydırabilir: satır, tıklamadan önceki pencereye göre çözülür.
    const std::size_t row = state.row_at(visible, line);
    // Ok ve ardındaki boşluk: tek hücrelik hedef fareyle zor. Notta ok yoktur, tıklama yalnız seçer.
    const bool is_folder = row < state.rows().size() && state.rows()[row].is_folder;
    const int arrow = is_folder ? row_marker_column(state, row) : 0;
    const bool on_arrow =
        is_folder && (input.x == arrow || input.x == arrow + 1) && state.line_starts_row(visible, line);
    state.click_row(row);
    if (on_arrow) {
        state.toggle_tree(row);
        return MouseResult::SavePrefs;
    }
    state.note_row_click(row);
    return MouseResult::None;
}

// Sağ tık: satır seçilir, GUI'nin satır menüsündeki eylemler palette açılır. Satırsız yerde bir şey olmaz.
void open_list_menu(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (!leave_editor(state, editor))
        return;
    const auto visible = static_cast<std::size_t>(state.window_height() - kFirstPaneRow - kBottomChromeRows);
    const std::size_t row = state.row_at(visible, static_cast<std::size_t>(input.y - kFirstPaneRow));
    if (row >= state.rows().size())
        return;
    state.click_row(row);
    state.open_row_menu();
}

MouseResult click_top_line(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (state.new_note_view())
        return MouseResult::None;
    if (input.x < ftxui::string_width(trash_indicator(state)))
        return leave_editor(state, editor) ? MouseResult::OpenTrash : MouseResult::None;
    if (const auto folder = breadcrumb_folder_at(state, input.x); folder && leave_editor(state, editor))
        state.go_to_folder(*folder);
    return MouseResult::None;
}

// Panel içi hücre -> metindeki konum; çizimle aynı sarma. Metin bittiyse son satırın sonu. Metin yoksa
// (ikili, hata) nullopt.
std::optional<PreviewPoint> preview_point_at(const TuiState& state, int cell, int row, int columns) {
    const std::vector<std::string>* lines = state.preview_text();
    if (!lines || lines->empty())
        return std::nullopt;
    auto remaining = static_cast<std::size_t>(std::max(row, 0));
    for (std::size_t line = state.preview_scroll(); line < lines->size(); ++line) {
        EditorBuffer buffer((*lines)[line], terminal_cell_width);
        const std::vector<VisualRow> rows = buffer.rows_of_line(0, columns);
        const bool last_line = line + 1 == lines->size();
        if (remaining < rows.size() || last_line) {
            const bool past_text = remaining >= rows.size();
            const VisualRow& visual = rows[std::min(remaining, rows.size() - 1)];
            buffer.move_to_cell(visual, past_text ? std::numeric_limits<int>::max() : cell);
            const std::size_t begin = buffer.cursor();
            buffer.move_right(false);
            return PreviewPoint{line, begin, buffer.cursor()};
        }
        remaining -= rows.size();
    }
    return std::nullopt;
}

// Fare panelin kenarını geçse de seçim panel içinde kalır: işaretçi en yakın hücreye kenetlenir.
struct PaneCell {
    int x = 0;
    int y = 0;
    int columns = 1;
};

PaneCell clamped_pane_cell(const MouseInput& input, const TuiState& state, const PaneGeometry& geometry) {
    const int columns = std::max(geometry.right_width - 2 * kFrameCells, 1);
    const int rows = std::max(pane_rows(state) - state.links_reserved_rows(pane_rows(state)), 1);
    return {std::clamp(input.x - geometry.right_x - kFrameCells, 0, columns - 1),
            std::clamp(input.y - kFirstPaneRow, 0, rows - 1), columns};
}

void begin_selection(const MouseInput& input, TuiState& state, const PaneGeometry& geometry) {
    const PaneCell cell = clamped_pane_cell(input, state, geometry);
    if (const auto point = preview_point_at(state, cell.x, cell.y, cell.columns))
        state.begin_preview_selection(*point, cell.x, cell.y);
}

void extend_selection(const MouseInput& input, TuiState& state, const PaneGeometry& geometry) {
    const PaneCell cell = clamped_pane_cell(input, state, geometry);
    if (const auto point = preview_point_at(state, cell.x, cell.y, cell.columns))
        state.extend_preview_selection(*point, cell.x, cell.y);
}

// Seçim sürerken gelen olay. Sol tuş basılı değilse sürükleme biter (Release kaybolmuş olabilir).
MouseResult continue_selection(const MouseInput& input, TuiState& state, const PaneGeometry& geometry) {
    const bool wheel = input.button == MouseButton::WheelUp || input.button == MouseButton::WheelDown;
    if (wheel) {
        // Çapa mantıksal konumdur; uç kayan pencerede aynı hücreye gelir.
        state.scroll_preview((input.button == MouseButton::WheelUp ? -1 : 1) * kWheelStep);
        const int columns = std::max(geometry.right_width - 2 * kFrameCells, 1);
        const int x = state.preview_selection_x();
        const int y = state.preview_selection_y();
        if (const auto point = preview_point_at(state, x, y, columns))
            state.extend_preview_selection(*point, x, y);
        return MouseResult::None;
    }
    if (input.motion == MouseMotion::Move && input.button == MouseButton::Left) {
        extend_selection(input, state, geometry);
        return MouseResult::None;
    }
    const std::optional<PreviewPoint> click = state.preview_click_point();
    if (state.finish_preview_selection())
        return MouseResult::SelectionFinished;
    return click && state.select_wiki_link_at(click->line, click->begin) ? MouseResult::FollowLink
                                                                         : MouseResult::None;
}

void click_right(const MouseInput& input, TuiState& state, EditorSession& editor,
                 const PaneGeometry& geometry) {
    if (state.focus() == Focus::Editor) {
        editor.click_at(input.x - geometry.right_x - kFrameCells, input.y - kFirstPaneRow);
        return;
    }
    if (state.trash_view())
        return;  // çöpteki not salt okunur gösterilir; tuşlar çöp listesinde kalır
    if (over_links(state, input.y)) {
        // İlk satır ayırıcıdır, tıklama bir şey yapmaz.
        if (const int row = input.y - links_first_row(state) - 1; row >= 0)
            state.click_link(static_cast<std::size_t>(row));
        return;
    }
    state.focus_preview();
    if (state.focus() == Focus::Preview)
        begin_selection(input, state, geometry);
}

MouseResult handle_left_press(const MouseInput& input, TuiState& state, EditorSession& editor,
                              const PaneGeometry& geometry) {
    if (input.y == 0)
        return click_top_line(input, state, editor);
    PaneLayout& layout = state.layout();
    if (input.y < state.window_height() - 1 && layout.on_divider(input.x, state.window_width())) {
        layout.begin_drag();
        return MouseResult::None;
    }
    if (!in_pane_rows(state, input.y))
        return MouseResult::None;
    if (!over_right(input.x, geometry))
        return click_list(input, state, editor);
    click_right(input, state, editor, geometry);
    return MouseResult::None;
}

}  // namespace

MouseResult route_mouse(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (!state.summary().empty() || state.palette_open())
        return MouseResult::None;  // özet penceresi ve palet sıradaki tuşu bekler
    PaneLayout& layout = state.layout();
    if (layout.dragging()) {
        if (input.motion == MouseMotion::Move && input.button == MouseButton::Left) {
            layout.set_list_width(input.x + 1, state.window_width());
            return MouseResult::None;
        }
        // Release, modal açıkken düşmüş ya da terminalde kaybolmuş olabilir: sol tuş basılı değilse sürükleme
        // biter ve olay normal işlenir, bölücü taşınmaz.
        layout.end_drag();
    }
    const PaneGeometry geometry = layout.geometry(state.window_width(), right_pane_focused(state.focus()));
    if (state.preview_selection_dragging())
        return continue_selection(input, state, geometry);
    if (editor_busy(state, editor) || input.motion != MouseMotion::Press)
        return MouseResult::None;
    const bool right = over_right(input.x, geometry);
    switch (input.button) {
    case MouseButton::WheelUp:
    case MouseButton::WheelDown:
        handle_wheel(input, state, editor, right);
        break;
    case MouseButton::Left:
        state.clear_preview_selection();
        return handle_left_press(input, state, editor, geometry);
    case MouseButton::Right:
        if (in_pane_rows(state, input.y) && !right && !state.trash_view())
            open_list_menu(input, state, editor);
        break;
    case MouseButton::Other:
        break;
    }
    return MouseResult::None;
}

}  // namespace noteapp::tui
