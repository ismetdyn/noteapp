#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Bayt aralığı [begin, end).
struct ByteRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Mantıksal konum (0 tabanlı): satır ve o satırda grapheme sırası.
struct TextPosition {
    std::size_t line = 0;
    std::size_t column = 0;
};

// Satır kaydırmasıyla oluşan görsel satır; satır sonu baytları dahil değildir.
struct VisualRow {
    std::size_t line = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool last_of_line = true;
};

// İmlecin görsel yeri: mantıksal satırın kaçıncı görsel satırı, kaçıncı hücresi.
struct CursorCell {
    std::size_t row = 0;
    int cell = 0;
};

// Pencerenin ilk görsel satırı.
struct ScrollTop {
    std::size_t line = 0;
    std::size_t row = 0;
};

// Bir grapheme'in ekrandaki karşılığı. Kontrol baytları `shown`a asla girmez: dosya içeriği terminale kaçış
// dizisi gönderemez, dosyadaki bayt ise tamponda aynen kalır.
struct Glyph {
    std::size_t next = 0;  // sonraki grapheme'in bayt ofseti
    int cells = 1;
    std::string shown;
    // Görünmez biçim karakteri (core::find_hidden_control) ise kod noktası, değilse 0: işaret uyarı rengiyle
    // çizilir, adı imleç üstündeyken ileti satırında söylenir.
    char32_t hidden = 0;
};

// FTXUI bilmeyen metin tamponu. İçerik dosyanın baytlarıdır (BOM hariç): satır sonları normalleştirilmez, bu
// yüzden CRLF, karışık satır sonu ve kontrol baytları düzenlenmeyen yerlerde bayt bayt korunur. İmleç her
// zaman grapheme sınırındadır: çok baytlı karakter ya da birleşen işaret bölünemez, CRLF tek adımdır.
class EditorBuffer {
public:
    // Kod noktasının hücre genişliği: 0 birleşen işaret, 1, 2 geniş. Dışarıdan verilir ki ekranla aynı ölçü
    // kullanılsın ve tampon terminal kütüphanesine bağlanmasın.
    using CellWidth = int (*)(char32_t);

    static constexpr std::size_t kDefaultUndoLimitBytes = std::size_t{64} * 1024 * 1024;
    static constexpr int kTabCells = 4;
    static constexpr int kMinColumns = 2;  // geniş karakter sığsın

    // `text` geçerli UTF-8 olmalıdır (çağıranın sınırda denetimi; bkz. EditorSession).
    EditorBuffer(std::string text, CellWidth cell_width,
                 std::size_t undo_limit_bytes = kDefaultUndoLimitBytes);

    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    std::optional<ByteRange> selection() const;
    // Metin son kayıtlı halinden farklı mı? Yazıp geri silmek ya da geri almak "temiz"e döner.
    bool dirty() const { return text_ != saved_; }
    void mark_saved() { saved_ = text_; }
    std::size_t line_count() const { return line_starts_.size(); }
    std::size_t undo_bytes() const { return undo_bytes_; }
    // Satır sonu baytları hariç metin.
    std::string_view line_text(std::size_t line) const;
    std::size_t line_start(std::size_t line) const { return line_starts_[line]; }
    std::size_t cursor_line() const { return line_of(cursor_); }
    bool uses_crlf() const { return eol_.size() > 1; }
    // Son sorudan beri değişen ilk satır (yoksa npos): sözdizimi durumları bu satırdan sonrası için
    // geçersizdir.
    std::size_t take_changed_line() const { return std::exchange(changed_from_, std::string_view::npos); }
    // Metni değiştiren her işlemde (düzenleme, geri alma, yeniden yükleme) artar; hareket ve seçim artırmaz.
    // Dil sunucusu eşitlemesi metni kopyalamadan değişikliği buradan anlar.
    std::uint64_t edit_count() const { return edit_count_; }

    // --- Düzenleme ---
    void type_text(std::string_view text);  // tek tuş: ardışık yazım tek geri alma adımında birleşir
    void paste(std::string_view text);      // tek adım; satır sonları dosyanınkine çevrilir
    void newline();
    void backspace();
    void delete_forward();
    std::string copy() const;  // seçim, yoksa imlecin satırı (satır sonuyla)
    std::string cut();
    bool undo();
    bool redo();

    // --- Hareket; `extend` seçimi genişletir ---
    void move_left(bool extend);
    void move_right(bool extend);
    void move_home(bool extend);
    void move_end(bool extend);
    void move_doc_start(bool extend);
    void move_doc_end(bool extend);
    // Görsel satır adımı; hedef hücre ardışık dikey hareketlerde korunur.
    void move_rows(int delta, int columns, bool extend);
    void move_to(TextPosition position);  // satır/sütun sınırlara kenetlenir, seçim kalkar
    // Aralığı seçer; uçlar dışa doğru grapheme sınırına genişler (imleç karakter ortasında kalamaz).
    void select_range(ByteRange range);
    void clear_selection() { begin_motion(false); }
    void move_to_cell(const VisualRow& row, int cell);  // fare: hücre satırın ötesindeyse satır sonu
    // Fareyle sürükleme ya da Shift+tık: çapa yerinde kalır, imleç hücreye gider (seçim yönü korunur).
    void extend_to_cell(const VisualRow& row, int cell);
    // Pencerenin ilk satırını koyar: Önizlemede görünen satırdan düzenlemeye geçerken ekran oynamasın.
    void set_scroll_line(std::size_t line);

    // --- Yerleşim ---
    TextPosition position() const;
    std::vector<VisualRow> rows_of_line(std::size_t line, int columns) const {
        return cached_rows(line, columns);
    }
    CursorCell cursor_cell(int columns) const;
    Glyph glyph_at(std::size_t offset, int cell) const;
    // Pencereyi imleç görünecek şekilde kaydırır; boyut değişince de çağrılır.
    void scroll_to_cursor(int columns, int rows);
    std::vector<VisualRow> visible_rows(int columns, int rows) const;
    ScrollTop scroll_top() const { return top_; }

    // Diskteki yeni içeriği alır: geçmiş silinir, imleç aynı satır/sütunda kalır (mümkünse).
    void reload(std::string text);

private:
    enum class EditKind { Typing, Backspace, DeleteForward, Other };
    struct Edit {
        std::size_t at = 0;
        std::string removed;
        std::string inserted;
        std::size_t cursor_before = 0;
        std::optional<std::size_t> anchor_before;
        EditKind kind = EditKind::Other;
    };

    void replace_range(std::size_t at, std::size_t length, std::string_view inserted);
    void record_and_apply(std::size_t at, std::size_t length, std::string_view inserted, EditKind kind);
    bool coalesce(std::size_t at, std::string_view removed, std::string_view inserted, EditKind kind);
    void trim_undo();
    void insert_replacing_selection(std::string_view inserted, EditKind kind);
    void rebuild_line_starts();
    void begin_motion(bool extend);
    std::size_t next_boundary(std::size_t offset) const;
    std::size_t previous_boundary(std::size_t offset) const;
    bool is_boundary(std::size_t offset) const;
    std::size_t line_of(std::size_t offset) const;
    std::size_t line_end(std::size_t line) const;
    std::vector<VisualRow> compute_rows(std::size_t line, int columns) const;
    const std::vector<VisualRow>& cached_rows(std::size_t line, int columns) const;
    std::size_t offset_at_cell(const VisualRow& row, int cell, int columns) const;

    std::string text_;
    std::string saved_;
    CellWidth cell_width_;
    std::string eol_ = "\n";
    std::vector<std::size_t> line_starts_;
    // Son sorulan satırın yerleşimi. Tek karede aynı satır birkaç kez istenir; 10 MiB'lik tek satırda bunu
    // yeniden hesaplamak tuş başına 80 ms ölçüldü (bütçe 50 ms). Metin değişince düşer.
    struct RowCache {
        std::size_t line = 0;
        int columns = 0;
        std::vector<VisualRow> rows;
    };
    mutable std::optional<RowCache> row_cache_;
    mutable std::size_t changed_from_ = 0;
    std::uint64_t edit_count_ = 0;
    std::size_t cursor_ = 0;
    std::optional<std::size_t> anchor_;
    std::optional<int> goal_cell_;
    ScrollTop top_;
    std::deque<Edit> undo_;
    std::vector<Edit> redo_;
    std::size_t undo_limit_bytes_;
    std::size_t undo_bytes_ = 0;
    bool run_open_ = false;  // son düzenleme birleşmeye açık mı (hareket ve geri alma kapatır)
};

}  // namespace noteapp::tui
