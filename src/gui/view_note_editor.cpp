// Not editörü: ImGui metin kutusu değil, EditorBuffer üstünde çizilen kendi alanımız (TUI ile aynı tampon,
// geri alma, imleç, seçim ve sarma). Model gui/note_editor.hpp'dedir; burası çizim ve girdi çevirisidir.
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>  // IME konumu: GImGui->PlatformImeData (InputText'in kullandığı yol)
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::gui {

namespace {

// Editör odaktayken tuşlar TUI tuş sözlüğüne çevrilir. Ctrl+F, F3 ve Shift+F3 komut tablosundandır (find,
// find.next, find.previous); Ctrl+P ve Ctrl+N yalnız bul satırında ve tamamlama listesinde önceki/sonraki
// (handle_shortcuts genel bağı o kiplerde bırakır).
struct EditorChord {
    ImGuiKeyChord chord;
    tui::Key key;
};
constexpr EditorChord kEditorChords[] = {
    {ImGuiKey_LeftArrow, tui::Key::Left},
    {ImGuiMod_Shift | ImGuiKey_LeftArrow, tui::Key::ShiftLeft},
    {ImGuiKey_RightArrow, tui::Key::Right},
    {ImGuiMod_Shift | ImGuiKey_RightArrow, tui::Key::ShiftRight},
    {ImGuiKey_UpArrow, tui::Key::Up},
    {ImGuiMod_Shift | ImGuiKey_UpArrow, tui::Key::ShiftUp},
    {ImGuiKey_DownArrow, tui::Key::Down},
    {ImGuiMod_Shift | ImGuiKey_DownArrow, tui::Key::ShiftDown},
    {ImGuiKey_Home, tui::Key::Home},
    {ImGuiMod_Shift | ImGuiKey_Home, tui::Key::ShiftHome},
    {ImGuiMod_Ctrl | ImGuiKey_Home, tui::Key::CtrlHome},
    {ImGuiKey_End, tui::Key::End},
    {ImGuiMod_Shift | ImGuiKey_End, tui::Key::ShiftEnd},
    {ImGuiMod_Ctrl | ImGuiKey_End, tui::Key::CtrlEnd},
    {ImGuiKey_PageUp, tui::Key::PageUp},
    {ImGuiKey_PageDown, tui::Key::PageDown},
    {ImGuiKey_Backspace, tui::Key::Backspace},
    {ImGuiKey_Delete, tui::Key::Delete},
    {ImGuiKey_Enter, tui::Key::Enter},
    {ImGuiKey_KeypadEnter, tui::Key::Enter},
    {ImGuiKey_Tab, tui::Key::Tab},
    {ImGuiMod_Ctrl | ImGuiKey_Z, tui::Key::CtrlZ},
    {ImGuiMod_Ctrl | ImGuiKey_Y, tui::Key::CtrlY},
    {ImGuiMod_Ctrl | ImGuiKey_C, tui::Key::CtrlC},
    {ImGuiMod_Ctrl | ImGuiKey_X, tui::Key::CtrlX},
    {ImGuiMod_Ctrl | ImGuiKey_V, tui::Key::CtrlV},
    {ImGuiMod_Ctrl | ImGuiKey_Space, tui::Key::CtrlSpace},
};
constexpr EditorChord kFindChords[] = {
    {ImGuiMod_Ctrl | ImGuiKey_P, tui::Key::CtrlP},
    {ImGuiMod_Ctrl | ImGuiKey_N, tui::Key::CtrlN},
};

constexpr ImU32 kFindMatchColor = IM_COL32(255, 200, 0, 90);
constexpr ImU32 kFindCurrentColor = IM_COL32(255, 170, 0, 190);
constexpr ImU32 kHiddenTextColor = IM_COL32(255, 255, 255, 255);
constexpr float kCurrentLineAlpha = 0.35F;
constexpr float kCursorWidth = 2.0F;
constexpr float kUnderlineInset = 2.0F;  // alt çizgi satırın dibinden bu kadar yukarıda
constexpr float kUnderlineThickness = 1.5F;
constexpr float kCellRounding = 0.5F;  // tık, hücrenin sağ yarısındaysa sonraki hücreye
constexpr int kMinGutterDigits = 2;    // ilk satırlarda sütun zıplamasın (TUI ile aynı)

bool inside(const tui::ByteRange& range, std::size_t offset) {
    return offset >= range.begin && offset < range.end;
}

// Editörlerin alışılmış dili (TUI ile aynı aile): hata kırmızı, uyarı sarı, bilgi mavi, ipucu soluk.
ImU32 severity_color(core::lsp::DiagnosticSeverity severity) {
    using Severity = core::lsp::DiagnosticSeverity;
    switch (severity) {
    case Severity::Unspecified:
    case Severity::Error:
        break;
    case Severity::Warning:
        return IM_COL32(214, 160, 0, 255);
    case Severity::Information:
        return IM_COL32(64, 132, 214, 255);
    case Severity::Hint:
        return ImGui::GetColorU32(ImGuiCol_TextDisabled);
    }
    return ImGui::GetColorU32(kErrorColor);
}

// Örtüşen tanılardan en önemlisi (Error = 1 en küçük değer); yoksa Unspecified.
core::lsp::DiagnosticSeverity worst_diagnostic(std::span<const tui::DiagnosticMark> marks, std::size_t offset) {
    auto worst = core::lsp::DiagnosticSeverity::Unspecified;
    for (const tui::DiagnosticMark& mark : marks)
        if (inside(mark.range, offset) &&
            (worst == core::lsp::DiagnosticSeverity::Unspecified || mark.severity < worst))
            worst = mark.severity;
    return worst;
}

// Satır numarası sütununun hücre sayısı: rakamlar + bir boşluk (TUI EditorSession::gutter_width).
int gutter_cells(std::size_t lines) {
    constexpr std::size_t kDecimalBase = 10;
    int digits = 1;
    for (; lines >= kDecimalBase; lines /= kDecimalBase)
        ++digits;
    return std::max(digits, kMinGutterDigits) + 1;
}

}  // namespace

void View::draw_editor(float reserved_height) {
    if (refocus_editor_) {
        refocus_editor_ = false;
        pending_focus_ = FocusRequest::Editor;
    }
    const bool finding = state_.editor().mode() == NoteEditor::Mode::Find;
    ImVec2 size = ImGui::GetContentRegionAvail();
    // Bul satırı (açıksa) ve durum çubuğu editörün altındadır; payları önce düşülür.
    const float bars = ImGui::GetFrameHeightWithSpacing() * (finding ? 2.0F : 1.0F);
    size.y = std::max(size.y - reserved_height - bars, ImGui::GetFrameHeight());
    if (pending_focus_ == FocusRequest::Editor) {
        ImGui::SetNextWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    // Sabit kimlik: sekme ya da içerik değişse de odak editörde kalır; kaydırma EditorBuffer'dadır.
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;
    ImGui::PushFont(platform_->mono_font, 0.0F);
    ImGui::BeginChild("##editor", size, ImGuiChildFlags_Borders, kFlags);
    editor_active_ = ImGui::IsWindowFocused();
    draw_editor_text();
    ImGui::EndChild();
    ImGui::PopFont();
    remember_item_center("editor");
    if (state_.editor().mode() == NoteEditor::Mode::Find)
        draw_find_bar();
    draw_editor_status();
}

// Yalnız görünen görsel satırlar çizilir (EditorBuffer::visible_rows): 10 MiB'lik notta da kare maliyeti
// pencere boyuyla sınırlı. Konum eş aralıklı fontun hücresidir; fare aynı ölçüden çözülür.
void View::draw_editor_text() {
    // Glif ilerlemesi kesirlidir; CalcTextSize genişliği tam sayıya yukarı yuvarlar, satır boyunca kayardı.
    const float cell = ImGui::GetFontBaked()->GetCharAdvance(static_cast<ImWchar>('M'));
    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const int gutter = gutter_cells(state_.editor().buffer().line_count());
    const float text_x = origin.x + (static_cast<float>(gutter) * cell);
    const int columns = std::max(static_cast<int>(area.x / cell) - gutter, tui::EditorBuffer::kMinColumns);
    const int rows = std::max(static_cast<int>(area.y / row_height), 1);
    state_.editor_layout(columns, rows);

    // Fare: tık imleci koyar, sürükleme ve Shift+tık seçimi genişletir, tekerlek imleci satır satır taşır.
    ImGui::InvisibleButton("##text", ImVec2(std::max(area.x, 1.0F), std::max(area.y, 1.0F)));
    remember_item_center("editor_text");
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto cell_at = [&] {
        const auto row = static_cast<std::size_t>(std::max((mouse.y - origin.y) / row_height, 0.0F));
        const int column = static_cast<int>(std::floor((std::max(mouse.x - text_x, 0.0F) / cell) + kCellRounding));
        return std::pair{row, column};
    };
    if (ImGui::IsItemActivated()) {
        const auto [row, column] = cell_at();
        state_.editor_click(row, column, ImGui::GetIO().KeyShift);
    } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
        const auto [row, column] = cell_at();
        state_.editor_click(row, column, true);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0F)
            state_.editor_wheel(wheel > 0.0F ? -1 : 1);
    }
    if (editor_active_)
        editor_keys();
    state_.editor_layout(columns, rows);  // tuş imleci taşıdıysa pencere onu izler
    const std::optional<ImVec2> cursor = draw_editor_rows(origin, text_x, area.x, cell, row_height);
    if (!cursor)
        return;
    remember_rect("editor_cursor", *cursor, ImVec2(cursor->x + cell, cursor->y + row_height));
    if (!editor_active_)
        return;
    if (state_.editor().mode() == NoteEditor::Mode::Complete)
        draw_completion_popup(*cursor, row_height);
    ImGui::GetWindowDrawList()->AddRectFilled(*cursor, ImVec2(cursor->x + kCursorWidth, cursor->y + row_height),
                                              ImGui::GetColorU32(ImGuiCol_InputTextCursor));
    // SDL3 metin olaylarını (IME, Türkçe klavye) yalnız bir alan metin istediğinde açar; InputText'in yolu.
    ImGuiPlatformImeData& ime = GImGui->PlatformImeData;
    ime.WantVisible = true;
    ime.WantTextInput = true;
    ime.InputPos = *cursor;
    ime.InputLineHeight = row_height;
    ime.ViewportId = ImGui::GetMainViewport()->ID;
}

// Satır numarası, geçerli satır zemini, seçim/arama/gizli karakter zemini ve metin. İmlecin ekran konumunu
// döner (görünmüyorsa boş).
std::optional<ImVec2> View::draw_editor_rows(ImVec2 origin, float text_x, float width, float cell,
                                             float row_height) {
    const NoteEditor& editor = state_.editor();
    const tui::EditorBuffer& buffer = editor.buffer();
    const float text_offset = (row_height - ImGui::GetFontSize()) / 2.0F;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 selection_color = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    const ImU32 hidden_color = ImGui::GetColorU32(kErrorColor);
    const std::size_t cursor = buffer.cursor();
    const std::size_t current_line = buffer.cursor_line();
    const std::optional<tui::ByteRange> selection = buffer.selection();
    const std::span<const tui::ByteRange> matches = editor.find_matches();
    const std::size_t current_match = matches.empty() ? 0 : matches[editor.find_index()].begin;
    std::optional<ImVec2> cursor_at;
    std::optional<std::size_t> painted_line;
    std::vector<tui::SyntaxSpan> spans;     // satır içi ofset
    std::vector<tui::ByteRange> misspelled;  // satır içi ofset
    std::vector<tui::DiagnosticMark> line_marks;  // yalnız bu satıra değenler: glif başına tüm liste taranmasın
    const std::vector<tui::VisualRow> visible = buffer.visible_rows(editor.columns(), editor.rows());
    for (std::size_t index = 0; index < visible.size(); ++index) {
        const tui::VisualRow& row = visible[index];
        const float y = origin.y + (static_cast<float>(index) * row_height);
        const bool current = row.line == current_line;
        if (current)
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + row_height),
                                ImGui::GetColorU32(ImGuiCol_HeaderHovered, kCurrentLineAlpha));
        if (row.begin == buffer.line_start(row.line)) {  // sarılan satırın devamında numara yok
            const std::string number = std::to_string(row.line + 1);
            const float number_x = text_x - (static_cast<float>(number.size() + 1) * cell);
            draw->AddText(ImVec2(number_x, y + text_offset), current ? text_color : dim_color, number.c_str());
            remember_rect("editor_line:" + std::to_string(row.line + 1), ImVec2(number_x, y),
                          ImVec2(text_x, y + row_height));
        }
        // Sarılan satır aynı aralıkları yeniden hesaplamaz: sözdizimi ve yazım satır başına bir kez.
        const std::size_t line_start = buffer.line_start(row.line);
        if (painted_line != row.line) {
            painted_line = row.line;
            spans = editor.syntax_spans(row.line);
            misspelled = state_.misspelled(buffer.line_text(row.line));
            const std::size_t line_end = line_start + buffer.line_text(row.line).size();
            line_marks.clear();
            for (const tui::DiagnosticMark& mark : editor.diagnostics())
                if (mark.range.begin <= line_end && mark.range.end > line_start)
                    line_marks.push_back(mark);
            if (!spans.empty())
                remember_rect("editor_syntax:" + std::to_string(row.line + 1), ImVec2(text_x, y),
                              ImVec2(std::max(text_x, origin.x + width), y + row_height));
        }
        // Eşleşmeler ve aralıklar sıralı: satıra değenlerden başlanır, satır boyunca yalnız ileri gidilir.
        auto match = std::ranges::upper_bound(matches, row.begin, {}, &tui::ByteRange::end);
        auto span = spans.begin();
        std::string run;  // aynı renkte ardışık tek hücreli ASCII glifler tek çizimde
        float run_x = text_x;
        ImU32 run_color = text_color;
        const auto flush = [&] {
            if (!run.empty())
                draw->AddText(ImVec2(run_x, y + text_offset), run_color, run.data(), run.data() + run.size());
            run.clear();
        };
        int column = 0;
        for (std::size_t at = row.begin; at < row.end;) {
            const tui::Glyph glyph = buffer.glyph_at(at, column);
            const float x = text_x + (static_cast<float>(column) * cell);
            const float glyph_width = static_cast<float>(glyph.cells) * cell;
            const std::size_t local = at - line_start;
            if (at == cursor)
                cursor_at = ImVec2(x, y);
            while (match != matches.end() && match->end <= at)
                ++match;
            while (span != spans.end() && span->end <= local)
                ++span;
            const tui::TokenKind kind =
                span != spans.end() && span->begin <= local ? span->kind : tui::TokenKind::Plain;
            ImU32 background = 0;
            if (glyph.hidden != 0)
                background = hidden_color;  // seçim ve aramanın altında da görünür (TUI ile aynı)
            else if (selection && inside(*selection, at))
                background = selection_color;
            else if (match != matches.end() && inside(*match, at))
                background = match->begin == current_match ? kFindCurrentColor : kFindMatchColor;
            if (background != 0)
                draw->AddRectFilled(ImVec2(x, y), ImVec2(x + glyph_width, y + row_height), background);
            for (const tui::ByteRange& word : misspelled) {  // satırda birkaç sözcük: doğrusal yeter
                if (!inside(word, local))
                    continue;
                draw->AddLine(ImVec2(x, y + row_height - kUnderlineInset),
                              ImVec2(x + glyph_width, y + row_height - kUnderlineInset), hidden_color,
                              kUnderlineThickness);
                if (local == word.begin)
                    remember_rect(std::format("editor_spell:{}:{}", row.line + 1, word.begin), ImVec2(x, y),
                                  ImVec2(x + glyph_width, y + row_height));
            }
            if (const auto severity = worst_diagnostic(line_marks, at);
                severity != core::lsp::DiagnosticSeverity::Unspecified) {
                draw->AddLine(ImVec2(x, y + row_height - kUnderlineInset),
                              ImVec2(x + glyph_width, y + row_height - kUnderlineInset), severity_color(severity),
                              kUnderlineThickness);
                for (const tui::DiagnosticMark& mark : line_marks)
                    if (mark.range.begin == at || (at == row.begin && inside(mark.range, at)))
                        remember_rect(std::format("editor_diagnostic:{}:{}", row.line + 1, local), ImVec2(x, y),
                                      ImVec2(x + glyph_width, y + row_height));
            }
            const ImU32 color = glyph.hidden != 0                ? kHiddenTextColor
                                : kind == tui::TokenKind::Plain ? text_color
                                                                : token_color(kind);
            const bool plain = glyph.hidden == 0 && glyph.cells == 1 && glyph.shown.size() == 1;
            if (!plain || color != run_color)
                flush();
            if (plain) {
                if (run.empty()) {
                    run_x = x;
                    run_color = color;
                }
                run += glyph.shown;
            } else {
                draw->AddText(ImVec2(x, y + text_offset), color, glyph.shown.data(),
                              glyph.shown.data() + glyph.shown.size());
            }
            column += glyph.cells;
            at = glyph.next;
        }
        flush();
        // Satır sonu imleci: sarılan satırın bitiş ofseti sonraki görsel satırın başında çizilir. Gölge öneri
        // imleçten sonra soluk, panele kırpılır (metin değildir).
        if (cursor == row.end && row.last_of_line) {
            cursor_at = ImVec2(text_x + (static_cast<float>(column) * cell), y);
            if (const std::string ghost = printable(editor.ghost()); !ghost.empty()) {
                draw->AddText(ImVec2(cursor_at->x, y + text_offset), dim_color, ghost.c_str());
                remember_rect("editor_ghost", *cursor_at,
                              ImVec2(cursor_at->x + text_width(ghost), y + row_height));
            }
        }
    }
    return cursor_at;
}

// Ctrl+Space listesi imlecin altında (TUI'nin fzf-tab penceresi): sorgu üstte, süzülmüş adaylar altında.
void View::draw_completion_popup(ImVec2 cursor, float row_height) {
    const NoteEditor& editor = state_.editor();
    ImGui::SetNextWindowPos(ImVec2(cursor.x, cursor.y + row_height));
    ImGui::BeginTooltip();
    ImGui::TextDisabled("%s", printable(editor.completion_query()).c_str());
    const std::vector<std::string>& items = editor.completion_items();
    if (items.empty())
        ImGui::TextDisabled("%s", core::tr("tui.editor_not_found").c_str());
    for (std::size_t index = 0; index < items.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::Selectable(printable(items[index]).c_str(), index == editor.completion_selected());
        ImGui::PopID();
    }
    remember_rect("editor_completion", ImGui::GetWindowPos(),
                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    ImGui::EndTooltip();
}

void View::editor_keys() {
    for (const EditorChord& entry : kEditorChords)
        if (ImGui::IsKeyChordPressed(entry.chord, ImGuiInputFlags_Repeat))
            state_.editor_key(entry.key);
    if (state_.editor().mode() != NoteEditor::Mode::Editing)
        for (const EditorChord& entry : kFindChords)
            if (ImGui::IsKeyChordPressed(entry.chord, ImGuiInputFlags_Repeat))
                state_.editor_key(entry.key);
    if (escape_pressed_)
        state_.editor_key(tui::Key::Escape);
    // Yalnız Ctrl basılıyken gelen karakter kısayolun parçasıdır; AltGr (Ctrl+Alt) ile yazılan karakter metindir.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.KeyAlt)
        return;
    for (const ImWchar character : io.InputQueueCharacters) {
        const auto code_point = static_cast<char32_t>(character);
        state_.editor_key(tui::Key::Char, core::encode_utf8(std::u32string_view(&code_point, 1)));
    }
}

void View::draw_find_bar() {
    const NoteEditor& editor = state_.editor();
    const tui::InputLine& line = editor.find_line();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(core::tr("tui.find_label").c_str());
    ImGui::SameLine();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const std::string query = printable(line.text());
    ImGui::TextUnformatted(query.c_str());
    remember_item_center("editor_find");
    if (editor_active_) {
        const float cursor_x =
            start.x + text_width(printable(std::string_view(line.text()).substr(0, line.cursor())));
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cursor_x, start.y), ImVec2(cursor_x + kCursorWidth, start.y + ImGui::GetTextLineHeight()),
            ImGui::GetColorU32(ImGuiCol_InputTextCursor));
    }
    if (const std::string ghost = printable(line.ghost()); !ghost.empty()) {
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::TextDisabled("%s", ghost.c_str());
    }
    ImGui::SameLine();
    if (editor.find_missed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextUnformatted(core::tr("tui.editor_not_found").c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(editor.find_label().c_str());
    }
    remember_item_center("editor_find_label");
    ImGui::SameLine();
    draw_fitted_text("editor_find_hints", core::tr("gui.find_hints"), ImGui::GetContentRegionAvail().x, keep_start);
    if (const tui::InputLine::Popup* popup = line.popup()) {  // Tab: nottaki sözcükler
        ImGui::SetNextWindowPos(start, ImGuiCond_Always, ImVec2(0.0F, 1.0F));
        ImGui::BeginTooltip();
        for (std::size_t index = 0; index < popup->items.size(); ++index) {
            const std::string item = printable(popup->items[index]);
            if (index == popup->selected)
                ImGui::TextUnformatted(item.c_str());
            else
                ImGui::TextDisabled("%s", item.c_str());
        }
        ImGui::EndTooltip();
    }
}

// TUI bilgi satırının alanları sağda; solda sırayla: tek seferlik ileti (dil sunucusu), imlecin üstündeki
// görünmez karakterin adı, imlecin üstündeki tanı (sunucu metni güvenilmez: printable).
void View::draw_editor_status() {
    const tui::EditorBuffer& buffer = state_.editor().buffer();
    std::string notice = state_.editor_notice();
    ImU32 notice_color = ImGui::GetColorU32(ImGuiCol_Text);
    if (notice.empty() && buffer.cursor() < buffer.text().size()) {
        const char32_t hidden = buffer.glyph_at(buffer.cursor(), 0).hidden;
        if (const core::HiddenControl* control = core::find_hidden_control(hidden))
            notice = core::tr("tui.editor_hidden_char",
                              {{"code", std::format("U+{:04X}", static_cast<std::uint32_t>(control->code_point))},
                               {"name", std::string(control->name)}});
        notice_color = ImGui::GetColorU32(kErrorColor);
    }
    if (notice.empty()) {
        if (const tui::DiagnosticMark* mark = state_.editor().diagnostic_at_cursor()) {
            notice = printable(mark->message);
            notice_color = severity_color(mark->severity);
        }
    }
    const std::string fields = state_.editor_fields();
    const float fields_width = text_width(fields);
    ImGui::AlignTextToFramePadding();
    if (!notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, notice_color);
        draw_fitted_text("editor_notice", notice,
                         ImGui::GetContentRegionAvail().x - fields_width - ImGui::GetStyle().ItemSpacing.x,
                         keep_start);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - fields_width));
    ImGui::TextUnformatted(fields.c_str());
    remember_item_center("editor_status");
}

}  // namespace noteapp::gui
