#pragma once

#include "core/lsp_protocol.hpp"
#include "edit/completion.hpp"
#include "edit/editor_buffer.hpp"
#include "edit/input_line.hpp"
#include "edit/key.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Dil sunucusunun tanısı, tampondaki bayt aralığıyla; ileti sunucudan gelir ve ham kalır (çizim temizler).
// `severity` hiçbir zaman Unspecified değildir.
struct DiagnosticMark {
    ByteRange range;
    core::lsp::DiagnosticSeverity severity = core::lsp::DiagnosticSeverity::Error;
    std::string message;
};

// Hareket tuşu (ok, Home/End, PgUp/PgDn, Shift ile seçim): tampona uygulanırsa true. Salt-okunurda da
// çalışır.
bool apply_motion_key(EditorBuffer& buffer, Key key, int columns, int rows);
// Arayüzlerin ortak düzenleme tuşları (yazım, silme, satır, sekme, geri al/yinele); diğerleri etkisiz.
void apply_edit_key(EditorBuffer& buffer, Key key, std::string_view text);

// Editörün kip mantığı (TUI EditorSession ve GUI NoteEditor ortak): bul, tamamlama listesi, gölge öneri ve
// tanılar. Tampon ve geçerli kip bu sınıfta değil, sahibindedir: çağrılar tamponu argüman alır, kip geçişini
// dönüşle bildirir. İleti, çizim ve tuş eşlemesi arayüzdedir.
class EditorModes {
public:
    explicit EditorModes(CompletionLimits limits) : limits_(limits) {}

    // --- Gölge öneri ---
    const std::string& ghost() const { return ghost_; }
    void clear_ghost() { ghost_.clear(); }
    // Her tuş öneriyi düşürür; yazılabilirde Tab ya da → onu yazar. Yazıldıysa true.
    bool accept_ghost(EditorBuffer& buffer, Key key, bool writable);
    // Yazımdan sonra: yalnız satır sonunda ve seçimsiz yazılabilir tamponda öneri üretir.
    void update_ghost(const EditorBuffer& buffer, bool writable, std::string_view type, const WordTable* words);

    // --- Tamamlama listesi (Ctrl+Space) ---
    enum class CompleteResult { Open, Inserted, Closed };
    // Açılış; sahip kipi Complete yapar.
    void open_completion(const EditorBuffer& buffer, std::string_view type, const WordTable* words);
    // Inserted ya da Closed: sahip düzenleme kipine döner.
    CompleteResult handle_complete(EditorBuffer& buffer, Key key, std::string_view text);
    // Dil sunucusunun önerileri açık listeye, sözcük önerilerinin önüne; `generation` eskiyse yok sayılır.
    // Sahip yalnız liste açıkken çağırır.
    void add_completion_items(std::uint64_t generation, std::span<const core::lsp::CompletionItem> items);
    const std::vector<std::string>& completion_items() const { return completion_items_; }
    std::size_t completion_selected() const { return completion_selected_; }
    const std::string& completion_query() const { return completion_query_; }
    std::uint64_t completion_generation() const { return completion_generation_; }

    // --- Bul ---
    enum class FindResult { Searched, Closed, Handled, Ignored };
    // Açılış; sahip kipi Find yapar.
    void open_find(const EditorBuffer& buffer, std::string_view type, const WordTable* words);
    // Sonraki/önceki eşleşme. Sorgu boşsa hiçbir şey yapmaz, false döner.
    bool step_find(EditorBuffer& buffer, int delta);
    // Searched: arama koştu (find_missed geçerli). Closed: Esc, sahip düzenleme kipine döner. Handled: tuşu
    // girdi satırı ya da öneri listesi karşıladı (ya da boş sorguda adım), sahibin yapacağı bir şey yok.
    // Ignored: tuş bul kipinde karşılıksız (ör. F9): sahip kendi eylemine bakar.
    FindResult handle_find(EditorBuffer& buffer, Key key, std::string_view text);
    void close_find();
    // Metin değişti: ofsetler eski metne ait; F3 yeniden hesaplar.
    void clear_find_matches() { find_matches_.clear(); }
    const InputLine& find_line() const { return find_line_; }
    std::span<const ByteRange> find_matches() const { return find_matches_; }
    std::size_t find_index() const { return find_index_; }
    std::string find_label() const;                    // "3/12"; eşleşme yoksa boş
    bool find_missed() const { return find_missed_; }  // son aramada sorgu dolu, eşleşme yok

    // --- Dil sunucusu tanıları (aralıklar tampona kısılmış) ---
    void set_diagnostics(std::vector<DiagnosticMark> marks) { diagnostics_ = std::move(marks); }
    std::span<const DiagnosticMark> diagnostics() const { return diagnostics_; }
    const DiagnosticMark* diagnostic_at_cursor(const EditorBuffer& buffer) const;

private:
    void refresh_completion();
    void insert_completion(EditorBuffer& buffer);
    void update_find(EditorBuffer& buffer);
    void recompute_find(const EditorBuffer& buffer, std::size_t from);
    void select_current_match(EditorBuffer& buffer);

    CompletionLimits limits_;
    std::string ghost_;
    std::vector<std::string> completion_candidates_;
    std::vector<std::string> completion_items_;
    std::string completion_query_;
    std::size_t completion_selected_ = 0;
    std::uint64_t completion_generation_ = 0;
    std::map<std::string, std::string> completion_inserts_;  // dil sunucusu etiketi -> eklenecek metin
    InputLine find_line_{InputKind::Find};
    std::size_t find_origin_ = 0;
    std::vector<ByteRange> find_matches_;
    std::size_t find_index_ = 0;
    bool find_missed_ = false;
    std::vector<DiagnosticMark> diagnostics_;
};

}  // namespace noteapp::tui
