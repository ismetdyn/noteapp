#include "tui/mouse.hpp"

#include "tui/editor_buffer.hpp"
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

bool right_has_focus(Focus focus) {
    return focus == Focus::Preview || focus == Focus::PreviewFind || focus == Focus::Editor;
}

bool in_pane_rows(const TuiState& state, int y) {
    return y >= kFirstPaneRow && y < state.window_height() - kBottomChromeRows;
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
    if (right)
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

void click_list(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (!leave_editor(state, editor))
        return;
    const auto visible = static_cast<std::size_t>(state.window_height() - kFirstPaneRow - kBottomChromeRows);
    const auto line = static_cast<std::size_t>(input.y - kFirstPaneRow);
    // Seçim pencereyi kaydırabilir: satır, tıklamadan önceki pencereye göre çözülür.
    const std::size_t row = state.row_at(visible, line);
    // İşaret ve ardındaki boşluk: tek hücrelik hedef fareyle zor.
    const int marker = row_marker_column(state);
    const bool on_marker =
        (input.x == marker || input.x == marker + 1) && state.line_starts_row(visible, line);
    state.click_row(row);
    if (on_marker)
        state.toggle_expanded(row);
}

MouseResult click_top_line(const MouseInput& input, TuiState& state, EditorSession& editor) {
    if (state.new_note_view() || input.x >= ftxui::string_width(trash_indicator(state)))
        return MouseResult::None;
    return leave_editor(state, editor) ? MouseResult::OpenTrash : MouseResult::None;
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
    const int rows = std::max(state.window_height() - kFirstPaneRow - kBottomChromeRows, 1);
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
    return state.finish_preview_selection() ? MouseResult::SelectionFinished : MouseResult::None;
}

void click_right(const MouseInput& input, TuiState& state, EditorSession& editor,
                 const PaneGeometry& geometry) {
    if (state.focus() == Focus::Editor) {
        editor.click_at(input.x - geometry.right_x - kFrameCells, input.y - kFirstPaneRow);
        return;
    }
    if (state.trash_view())
        return;  // çöpteki not salt okunur gösterilir; tuşlar çöp listesinde kalır
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
    if (over_right(input.x, geometry))
        click_right(input, state, editor, geometry);
    else
        click_list(input, state, editor);
    return MouseResult::None;
}

}  // namespace

MouseResult route_mouse(const MouseInput& input, TuiState& state, EditorSession& editor) {
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
    const PaneGeometry geometry = layout.geometry(state.window_width(), right_has_focus(state.focus()));
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
    case MouseButton::Other:
        break;
    }
    return MouseResult::None;
}

}  // namespace noteapp::tui
