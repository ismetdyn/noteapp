#include "tui/editor_view.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "tui/input_view.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <ftxui/screen/string.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

namespace {

// Soru pencerelerinin iç genişliği; en küçük pencerede (60) kenarlara sığar.
constexpr int kPromptWidth = 52;
constexpr std::string_view kHintGap = "  ";
constexpr std::string_view kFieldGap = "  ";
constexpr std::string_view kEncodingName =
    "UTF-8";  // tampon her zaman UTF-8'dir; geçersiz not salt-okunur açılır
constexpr std::string_view kBomMark = "BOM";
constexpr std::string_view kPastEndMark = "~";
constexpr int kInfoBarPadding = 4;  // kenar boşlukları + ad ile alanlar arası
// Parlak siyah: 16 ANSI içinde hem açık hem koyu temada satırı metni bozmadan ayırır.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) -- palet değerinden kurulum ayırma yapmaz
const ftxui::Color kCurrentLineBackground = ftxui::Color::GrayDark;

enum class CellStyle { Plain, Highlight, Inverted };

// Bir hücrenin görünüşü: arama ve seçim her zaman sözdizimi renginin önüne geçer.
struct CellLook {
    CellStyle style = CellStyle::Plain;
    TokenKind token = TokenKind::Plain;
    bool misspelled = false;
    // Unspecified: tanı yok (DiagnosticMark bu değeri taşımaz).
    core::lsp::DiagnosticSeverity diagnostic = core::lsp::DiagnosticSeverity::Unspecified;
    bool hidden = false;  // görünmez bidi/sıfır genişlikli karakterin işareti
    bool dimmed = false;
    bool operator==(const CellLook&) const = default;
};

// Editörlerin alışılmış dili: hata kırmızı, uyarı sarı, bilgi mavi; ipucu yalnız soluk alt çizgi. Sarı arama
// vurgusu kalın çizilir ve tanının önüne geçer, alt çizgi uyarıyı ondan ayırır.
ftxui::Element severity_styled(ftxui::Element element, core::lsp::DiagnosticSeverity severity) {
    using namespace ftxui;
    using Severity = core::lsp::DiagnosticSeverity;
    switch (severity) {
    case Severity::Unspecified:
        return element;
    case Severity::Error:
        return element | color(Color::Red);
    case Severity::Warning:
        return element | color(Color::Yellow);
    case Severity::Information:
        return element | color(Color::BlueLight);
    case Severity::Hint:
        return element | dim;
    }
    return element;
}

}  // namespace

// Yalnız 16 ANSI rengi: açık ve koyu terminalde de okunur. Aile tablosu GUI ile ortak (syntax_family); yorum
// soluk kalır ki içerikten parlak olmasın. Sarı arama vurgusuna ayrılmıştır.
ftxui::Element token_styled(ftxui::Element element, TokenKind token) {
    using namespace ftxui;
    switch (syntax_family(token)) {
    case SyntaxFamily::Plain:
        return element;
    case SyntaxFamily::Muted:
        return element | dim;
    case SyntaxFamily::Green:
        return element | color(Color::Green);
    case SyntaxFamily::Orange:
        return element | color(Color::RedLight);
    case SyntaxFamily::Cyan:
        return element | color(Color::Cyan);
    case SyntaxFamily::Magenta:
        return element | color(Color::Magenta);
    case SyntaxFamily::Blue:
        return element | bold | color(Color::BlueLight);
    case SyntaxFamily::Bold:
        return element | bold;
    }
    return element;
}

namespace {

ftxui::Element styled(const std::string& run, CellLook look) {
    using namespace ftxui;
    Element element = text(run);
    // Arka plan uyarısı seçim, arama ve sözdizimi renginin altında da görünür: işaret hiçbir bakışta
    // sıradan bir U+FFFD gibi okunmaz.
    if (look.hidden)
        element = element | color(Color::White) | bgcolor(Color::Red);
    const bool diagnosed = look.diagnostic != core::lsp::DiagnosticSeverity::Unspecified;
    if (look.misspelled || diagnosed)
        element = element | underlined;
    if (look.dimmed)
        element = element | dim;
    switch (look.style) {
    case CellStyle::Highlight:
        return element | bold | color(Color::Yellow);
    case CellStyle::Inverted:
        return element | inverted;
    case CellStyle::Plain:
        break;
    }
    if (diagnosed)
        return severity_styled(std::move(element), look.diagnostic);
    return token_styled(std::move(element), look.token);
}

bool is_misspelled(std::size_t offset, const RowPaint& paint) {
    return std::ranges::any_of(paint.misspelled, [offset](const ByteRange& range) {
        return offset >= range.begin && offset < range.end;
    });
}

// Örtüşen tanılardan en önemlisi (Error = 1 en küçük değerdir).
core::lsp::DiagnosticSeverity diagnostic_at(std::size_t offset, const RowPaint& paint) {
    auto worst = core::lsp::DiagnosticSeverity::Unspecified;
    for (const DiagnosticMark& mark : paint.diagnostics)
        if (offset >= mark.range.begin && offset < mark.range.end &&
            (worst == core::lsp::DiagnosticSeverity::Unspecified || mark.severity < worst))
            worst = mark.severity;
    return worst;
}

std::size_t first_code_point_bytes(std::string_view text) {
    std::size_t length = 1;
    while (length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        ++length;
    return length;
}

// Gölge öneri: ilk harf imlecin altında, tümü soluk ve panele kırpılır (sarmaya girmez, metin değildir).
void append_ghost(ftxui::Elements& parts, std::string_view ghost, int room) {
    using namespace ftxui;
    const std::string safe = terminal_safe(ghost);
    std::string_view rest = safe;
    const std::size_t first = first_code_point_bytes(rest);
    parts.push_back(text(std::string(rest.substr(0, first))) | dim | focusCursorBlock);
    int used = string_width(std::string(rest.substr(0, first)));
    rest.remove_prefix(first);
    std::string shown;
    while (!rest.empty()) {
        const std::string_view next = rest.substr(0, first_code_point_bytes(rest));
        used += string_width(std::string(next));
        if (used > room)
            break;
        shown += next;
        rest.remove_prefix(next.size());
    }
    if (!shown.empty())
        parts.push_back(text(std::move(shown)) | dim);
}

// Aralıklar sıralıdır ve satır soldan sağa çizilir: geri dönmeyen imleç satır başına doğrusal kalır.
class TokenCursor {
public:
    TokenCursor(std::span<const SyntaxSpan> spans, std::size_t origin) : spans_(spans), origin_(origin) {}
    TokenKind at(std::size_t offset) {
        const std::size_t local = offset - origin_;
        while (next_ < spans_.size() && spans_[next_].end <= local)
            ++next_;
        return next_ < spans_.size() && spans_[next_].begin <= local ? spans_[next_].kind : TokenKind::Plain;
    }

private:
    std::span<const SyntaxSpan> spans_;
    std::size_t origin_;
    std::size_t next_ = 0;
};

CellStyle style_at(std::size_t offset, const RowPaint& paint) {
    const auto inside = [offset](const ByteRange& range) {
        return offset >= range.begin && offset < range.end;
    };
    if (paint.selection && inside(*paint.selection))
        return CellStyle::Inverted;
    return std::ranges::any_of(paint.highlights, inside) ? CellStyle::Highlight : CellStyle::Plain;
}

// Numara sağa yaslı; sarılan satırın devamında boş kalır. Satır numarası metin değildir: tampona ve imleç
// hesabına girmez.
ftxui::Element gutter_cell(const EditorBuffer& buffer, const VisualRow& row, int width, bool current) {
    using namespace ftxui;
    const bool first_row = row.begin == buffer.line_start(row.line);
    std::string label = first_row ? std::to_string(row.line + 1) + " " : std::string{};
    label.insert(0, static_cast<std::size_t>(std::max(width - static_cast<int>(label.size()), 0)), ' ');
    return current ? text(std::move(label)) | bold : text(std::move(label)) | dim;
}

// İmlecin üstündeki görünmez karakterin adı: "Gizli karakter U+202E RIGHT-TO-LEFT OVERRIDE"; değilse boş.
std::string hidden_char_notice(const EditorBuffer& buffer) {
    if (buffer.cursor() >= buffer.text().size())
        return {};
    const char32_t hidden = buffer.glyph_at(buffer.cursor(), 0).hidden;
    const core::HiddenControl* control = core::find_hidden_control(hidden);
    if (control == nullptr)
        return {};
    return core::tr("tui.editor_hidden_char",
                    {{"code", std::format("U+{:04X}", static_cast<std::uint32_t>(control->code_point))},
                     {"name", std::string(control->name)}});
}

std::string status_notice(const EditorSession& session) {
    if (!session.message().empty())
        return session.message();
    if (session.deleted_on_disk())
        return core::tr("tui.editor_deleted_on_disk");
    if (session.changed_on_disk())
        return core::tr("tui.editor_changed_on_disk");
    return {};
}

ftxui::Elements choice_rows(std::initializer_list<std::string> labels, int selected) {
    using namespace ftxui;
    Elements rows;
    int index = 0;
    for (const std::string& label : labels) {
        Element row = text("[ " + label + " ]");
        rows.push_back(index == selected ? row | inverted : row);
        ++index;
    }
    return rows;
}

ftxui::Element prompt_box(const std::string& title, const std::string& question, ftxui::Elements choices) {
    using namespace ftxui;
    // Soru not adını taşır (senkrondan gelebilir): çizim noktasında arınır.
    Elements body = {paragraph(terminal_safe(question)) | bold, text("")};
    for (Element& choice : choices)
        body.push_back(std::move(choice));
    return window(text(" " + title + " ") | bold, vbox(std::move(body))) | size(WIDTH, EQUAL, kPromptWidth) |
           clear_under;
}

// fzf-tab gibi: sorgu üstte, süzülmüş adaylar altında; yazı metne değil sorguya gider.
}  // namespace

int terminal_cell_width(char32_t code_point) {
    return ftxui::string_width(core::encode_utf8(std::u32string_view(&code_point, 1)));
}

ftxui::Element paint_row(const EditorBuffer& buffer, const VisualRow& row, const RowPaint& paint) {
    ftxui::Elements parts;
    std::string run;
    CellLook run_look;
    TokenCursor tokens(paint.syntax, paint.syntax_origin);
    int cell = 0;
    for (std::size_t at = row.begin; at < row.end;) {
        const Glyph glyph = buffer.glyph_at(at, cell);
        const CellLook look{style_at(at, paint),
                            tokens.at(at),
                            is_misspelled(at, paint),
                            diagnostic_at(at, paint),
                            glyph.hidden != 0,
                            std::ranges::any_of(paint.dimmed, [at](const ByteRange& range) {
                                return at >= range.begin && at < range.end;
                            })};
        const bool at_cursor = paint.cursor && at == *paint.cursor;
        if ((look != run_look || at_cursor) && !run.empty()) {
            parts.push_back(styled(run, run_look));
            run.clear();
        }
        if (at_cursor) {
            parts.push_back(styled(glyph.shown, look) | ftxui::focusCursorBlock);
        } else {
            run_look = look;
            run += glyph.shown;
        }
        cell += glyph.cells;
        at = glyph.next;
    }
    if (!run.empty())
        parts.push_back(styled(run, run_look));
    // Satır sonu imleci: kaydırılan satırın bitiş ofseti zaten sonraki görsel satırın başında çizilir.
    if (paint.cursor && *paint.cursor == row.end && row.last_of_line) {
        if (paint.ghost.empty())
            parts.push_back(ftxui::text(" ") | ftxui::focusCursorBlock);
        else
            append_ghost(parts, paint.ghost, paint.columns - cell);
    }
    // Boş hbox yükseklik almaz: boş satır pencereden düşmesin.
    if (parts.empty())
        parts.push_back(ftxui::text(" "));
    return ftxui::hbox(std::move(parts));
}

ftxui::Element editor_pane(const EditorSession& session, bool show_cursor, SpellChecker* spell) {
    using namespace ftxui;
    const EditorBuffer& buffer = *session.buffer();
    const int gutter = session.gutter_width();
    const std::size_t current_line = buffer.cursor_line();
    const bool editing = show_cursor && session.mode() == EditorSession::Mode::Editing;
    const std::optional<std::size_t> cursor = editing ? std::optional(buffer.cursor()) : std::nullopt;
    Elements rows;
    std::vector<SyntaxSpan> spans;
    std::vector<ByteRange> misspelled;
    std::vector<DiagnosticMark> line_marks;  // yalnız bu satıra değenler: hücre başına tüm liste taranmasın
    std::optional<std::size_t> spans_line;   // sarılan satır aynı aralıkları yeniden hesaplamasın
    const bool check_spelling = spell && spellcheck_applies_to(session.shown_type());
    for (const VisualRow& row : buffer.visible_rows(session.columns(), session.rows())) {
        if (spans_line != row.line) {
            spans = session.syntax_spans(row.line);
            spans_line = row.line;
            const std::size_t line_begin = buffer.line_start(row.line);
            const std::size_t line_end = line_begin + buffer.line_text(row.line).size();
            line_marks.clear();
            for (const DiagnosticMark& mark : session.diagnostics())
                if (mark.range.begin <= line_end && mark.range.end > line_begin)
                    line_marks.push_back(mark);
            misspelled.clear();
            if (check_spelling)
                misspelled = spell->misspelled(buffer.line_text(row.line));
            for (ByteRange& range : misspelled) {
                range.begin += buffer.line_start(row.line);
                range.end += buffer.line_start(row.line);
            }
        }
        const RowPaint paint{cursor,          buffer.selection(),          session.find_matches(),
                             spans,           buffer.line_start(row.line), misspelled,
                             session.ghost(), session.columns(),           line_marks};
        const bool current = row.line == current_line;
        Element line =
            hbox({gutter_cell(buffer, row, gutter, current), paint_row(buffer, row, paint), filler()});
        if (current)
            line = line | bgcolor(kCurrentLineBackground);
        rows.push_back(std::move(line));
    }
    while (static_cast<int>(rows.size()) < session.rows())
        rows.push_back(text(std::string(kPastEndMark)) | dim);
    return vbox(std::move(rows));
}

ftxui::Element editor_info_bar(const EditorSession& session, int width) {
    using namespace ftxui;
    const EditorBuffer& buffer = *session.buffer();
    const TextPosition position = buffer.position();
    std::string fields = session.shown_type() + std::string(kFieldGap) + std::string(kEncodingName) +
                         std::string(kFieldGap) + (buffer.uses_crlf() ? "CRLF" : "LF");
    if (session.has_bom())
        fields += std::string(kFieldGap) + std::string(kBomMark);
    fields += std::string(kFieldGap) + std::to_string(position.line + 1) + ":" +
              std::to_string(position.column + 1) + std::string(kFieldGap) +
              core::tr("tui.status_lines", {{"count", std::to_string(buffer.line_count())}});
    // Alan bilgisi yerinde kalır; sığmazsa dosya adının sonu görünür (uzantı ve ad ayırt edici kısımdır).
    const std::string name =
        (session.path() ? session.path()->string() : "[" + session.display_name() + "]") +
        (session.dirty() ? std::string(kDirtyMark) : "");
    const int room = width - string_width(fields) - kInfoBarPadding;
    // Yol senkrondan gelebilir: kesmeden önce arınır, hücre ölçüsü çizilen metinle aynı olsun.
    const std::string shown = tail_cells(terminal_safe(name), static_cast<std::size_t>(std::max(room, 1)));
    return hbox({text(" " + shown), filler(), text(fields + " ")}) | inverted;
}

ftxui::Element editor_status_bar(const EditorSession& session, int width, ftxui::Color key_color) {
    using namespace ftxui;
    if (session.mode() == EditorSession::Mode::NamePrompt) {
        // Klasörsüz ad buraya yazılır: kullanıcı nereye kaydedeceğini görsün.
        const std::string& folder = session.name_folder();
        const std::string label =
            core::tr("tui.new_note_label") + (folder.empty() ? std::string{} : " " + folder + "/");
        return input_line_bar(label, session.name_line(), {}, session.name_line().ghost(),
                              core::tr("tui.new_note_hints"), width, {}, session.message());
    }
    if (session.mode() == EditorSession::Mode::Find)
        return input_line_bar(core::tr("tui.find_label"), session.find_line(), {},
                              session.find_line().ghost(), core::tr("tui.editor_find_hints"), width,
                              session.find_label(), session.message());

    const bool read_only = session.read_only() != EditorSession::ReadOnly::No;
    // İleti çoğu kez yol taşır (kopya, yeni not, hata ayrıntısı): kaynağı ne olursa olsun burada arınır.
    const std::string notice = terminal_safe(status_notice(session));
    Elements cells;
    int used = 0;
    if (!notice.empty()) {
        Element shown = text(notice) | bold;
        cells.push_back(session.message_is_error() ? shown | color(Color::Red) : shown);
        used += string_width(notice) + static_cast<int>(kHintGap.size());
    } else if (const std::string hidden = hidden_char_notice(*session.buffer()); !hidden.empty()) {
        // Tanıdan önce: işaret hücresi yalnız "burada bir şey var" der, ne olduğunu bu satır söyler.
        cells.push_back(text(hidden) | bold | color(Color::Red));
        used += string_width(hidden) + static_cast<int>(kHintGap.size());
    } else if (const DiagnosticMark* mark = session.diagnostic_at_cursor()) {
        // Sunucu metni güvenilmez: denetim/bidi karakteri terminale gitmez, satır sonu ileti satırını bölmez.
        std::string message = terminal_safe(mark->message);
        cells.push_back(severity_styled(text(message), mark->severity));
        used += string_width(message) + static_cast<int>(kHintGap.size());
    }
    // Hata ya da ileti varken kısayol sığdığı kadar yanında kalır; kelime ortasında kesilmez.
    if (!cells.empty())
        cells.push_back(text(std::string(kHintGap)));
    cells.push_back(hint_bar(core::tr(read_only ? "tui.key_hints_editor_readonly" : "tui.key_hints_editor"),
                             width - used, key_color) |
                    flex);
    return hbox(std::move(cells));
}

ftxui::Element editor_modal(const EditorSession& session) {
    switch (session.mode()) {
    case EditorSession::Mode::LeavePrompt:
        return prompt_box(core::tr("tui.editor_leave_title"),
                          core::tr("tui.editor_leave_question", {{"path", session.display_name()}}),
                          choice_rows({core::tr("tui.editor_choice_save"), core::tr("tui.discard_yes"),
                                       core::tr("tui.discard_no")},
                                      static_cast<int>(session.leave_choice())));
    case EditorSession::Mode::ConflictPrompt:
        return prompt_box(core::tr("tui.editor_conflict_title"),
                          core::tr("tui.editor_conflict_question", {{"path", session.display_name()}}),
                          choice_rows({core::tr("tui.editor_choice_copy"), core::tr("tui.editor_choice_load"),
                                       core::tr("tui.editor_choice_overwrite"), core::tr("tui.discard_no")},
                                      static_cast<int>(session.conflict_choice())));
    case EditorSession::Mode::Editing:
        break;
    case EditorSession::Mode::Find:
        return session.find_line().popup() ? input_popup_box(*session.find_line().popup()) : nullptr;
    case EditorSession::Mode::NamePrompt:
        return session.name_line().popup() ? input_popup_box(*session.name_line().popup()) : nullptr;
    case EditorSession::Mode::Complete:
        return completion_popup(session.completion_query(), session.completion_items(),
                                session.completion_selected());
    }
    return nullptr;
}

}  // namespace noteapp::tui
