#pragma once

#include "edit/completion.hpp"
#include "edit/editor_buffer.hpp"
#include "edit/editor_modes.hpp"
#include "edit/input_line.hpp"
#include "edit/key.hpp"
#include "core/lsp_protocol.hpp"
#include "edit/syntax.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::gui {

// Kod noktasının hücre genişliği (0 birleşen işaret, 1, 2 geniş); EditorBuffer'ın ölçüsü. Editör eş aralıklı
// fontla hücre hücre çizilir, fare de aynı hücrelerden çözülür: ölçü fonttan değil Unicode'dan gelir ki
// ImGui bağlamı olmayan model testleri de aynı sonucu görsün.
int cell_width(char32_t code_point);

// Editörün sınırları: limits.tui.* (TUI ile aynı anahtarlar) ve tuning.completion_min_prefix_bytes.
struct EditorLimits {
    std::size_t undo_bytes = tui::EditorBuffer::kDefaultUndoLimitBytes;
    tui::SyntaxLimits syntax = {};
    tui::CompletionLimits completion = {};
};

// GUI editörünün saf modeli (ImGui bilmez): TUI EditorSession'ın düzenleme, bul ve tamamlama kipleriyle aynı
// tuş eşlemesi, aynı EditorBuffer (geri alma, imleç, seçim, sarma). Kayıt, çakışma ve otomatik kayıt
// GuiState'tedir; pano GuiState'in (copy/cut/paste). Metni değiştiren her çağrıdan sonra GuiState kirli
// durumu ve otomatik kaydı buffer().edit_count() ile günceller.
class NoteEditor {
public:
    enum class Mode { Editing, Find, Complete };

    NoteEditor() : NoteEditor(std::string{}, false, std::string{}, EditorLimits{}) {}
    // `text` geçerli UTF-8 olmalıdır (app::Document salt-okunur notu arındırır).
    NoteEditor(std::string text, bool writable, std::string type, EditorLimits limits);

    const tui::EditorBuffer& buffer() const { return buffer_; }
    const std::string& text() const { return buffer_.text(); }
    bool writable() const { return writable_; }
    // İnceleme kipi (REVIEW) tamponu kilitler: hareket, kopya ve bul kalır; açık tamamlama ve öneri düşer.
    void set_writable(bool writable);
    Mode mode() const { return mode_; }
    const std::string& type() const { return type_; }
    void set_type(std::string type);

    // Tuş: TUI tuş sözlüğü. Salt-okunurda yalnız hareket, kopya ve bul çalışır. `words` diğer notların
    // sözcükleri (WordTableBuilder); boşsa yalnız bu not önerilir.
    void handle(tui::Key key, std::string_view text = {}, const tui::WordTable* words = nullptr);
    // Panelin metin alanı (numara sütunu hariç) hücre ölçüsünde; çizimden önce her karede. İmleç görünür
    // kalır.
    void layout(int columns, int rows);
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    // Fare: görünen satır sırası ve hücre. `extend`: sürükleme ya da Shift+tık seçimi genişletir.
    void click(std::size_t visible_row, int cell, bool extend);
    // Tekerlek TUI gibi imleci satır satır taşır (pencere onunla kayar).
    void wheel(int rows);

    std::string copy() const { return buffer_.copy(); }  // seçim, yoksa imlecin satırı
    std::string cut();                                    // yazılabilirde; yoksa yalnız kopya
    void paste(std::string_view text);                    // tek geri alma adımı
    // Tüm metni tek düzenleme adımında değiştirir (geri alınabilir).
    void replace_all(std::string_view text);
    void select(tui::ByteRange range) { buffer_.select_range(range); }
    void clear_selection() { buffer_.clear_selection(); }
    void move_to(tui::TextPosition position) { buffer_.move_to(position); }
    void move_to_end() { buffer_.move_doc_end(false); }
    void mark_saved() { buffer_.mark_saved(); }
    bool dirty() const { return buffer_.dirty(); }

    // --- Bul (Ctrl+F, F3, Shift+F3) ---
    void open_find(const tui::WordTable* words);
    void step_find(int delta);
    void close_find();
    const tui::InputLine& find_line() const { return modes_.find_line(); }
    std::span<const tui::ByteRange> find_matches() const { return modes_.find_matches(); }
    std::size_t find_index() const { return modes_.find_index(); }
    std::string find_label() const { return modes_.find_label(); }  // "3/12"; eşleşme yoksa boş
    bool find_missed() const { return modes_.find_missed(); }       // sorgu dolu, eşleşme yok

    // --- Tamamlama (gölge öneri, Ctrl+Space listesi) ---
    const std::string& ghost() const { return modes_.ghost(); }
    const std::vector<std::string>& completion_items() const { return modes_.completion_items(); }
    std::size_t completion_selected() const { return modes_.completion_selected(); }
    const std::string& completion_query() const { return modes_.completion_query(); }
    // Her Ctrl+Space açılışında artar; dil sunucusu cevabı yalnız aynı açılışa eklenir.
    std::uint64_t completion_generation() const { return modes_.completion_generation(); }
    // Dil sunucusunun önerileri açık listeye, sözcük önerilerinin önüne (TUI EditorSession ile aynı kural):
    // etiket temizlenir, ekleme metni yalnız tek satır ve denetim karaktersizse kullanılır.
    void add_completion_items(std::uint64_t generation, std::span<const core::lsp::CompletionItem> items);

    // --- Dil sunucusu tanıları (aralıklar bu tampona kısılmış) ---
    void set_diagnostics(std::vector<tui::DiagnosticMark> marks) { modes_.set_diagnostics(std::move(marks)); }
    std::span<const tui::DiagnosticMark> diagnostics() const { return modes_.diagnostics(); }
    // İmlecin üstündeki ya da hemen sonundaki.
    const tui::DiagnosticMark* diagnostic_at_cursor() const { return modes_.diagnostic_at_cursor(buffer_); }

    // Çizimde yalnız görünen satır için; eşik üstünde ya da bilinmeyen tipte boş.
    std::vector<tui::SyntaxSpan> syntax_spans(std::size_t line) const;

private:
    void handle_editing(tui::Key key, std::string_view text, const tui::WordTable* words);
    void handle_find(tui::Key key, std::string_view text);
    void handle_complete(tui::Key key, std::string_view text);

    tui::EditorBuffer buffer_;
    bool writable_ = false;
    std::string type_;
    tui::SyntaxLanguage language_ = tui::SyntaxLanguage::Plain;
    EditorLimits limits_;
    mutable tui::SyntaxStateCache syntax_cache_;  // çizimde tembel dolar
    Mode mode_ = Mode::Editing;
    int columns_ = 80;
    int rows_ = 24;
    tui::EditorModes modes_{limits_.completion};  // bul, tamamlama, gölge öneri, tanılar (TUI ile ortak)
};

}  // namespace noteapp::gui
