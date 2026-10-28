#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Terminal kütüphanesinden bağımsız tuş soyutlaması: FTXUI olayları main.cpp'de buna çevrilir, böylece
// model hiçbir UI başlığına bağlanmadan test edilir.
enum class Key {
    Char,  // yazdırılabilir karakter; metni handle()'a ayrıca verilir
    Backspace,
    Up,
    Down,
    CtrlP,
    CtrlN,
    PageUp,
    PageDown,
    Tab,
    ShiftTab,
    Left,  // ana listede kullanılmaz; diyaloglar için
    Right,
    Enter,
    Escape,
    CtrlO,
    CtrlR,
    CtrlD,
    CtrlS,
    F1,
    F2,
    CtrlC,
};

// Modelin dış dünyadan istediği iş; yan etkileri (editör, diyalog, çıkış) çağıran yürütür.
enum class Intent {
    None,
    Quit,
    OpenSelected,
    CreateFromQuery,  // eşleşme yokken Enter: sorgu yeni notun ilk satırı olur
    NewNote,
    MoveSelected,  // Ctrl-R: ad/klasör/tip diyaloğu
    DeleteSelected,
    SyncNow,
    ShowHelp,
    ShowSettings,
};

enum class Mode { Name, Content };

// Listedeki bir satır. Ad modunda `positions` (yol içindeki bayt ofsetleri) vurgu içindir; içerik modunda
// `hit_count`/`hits` doludur.
struct Row {
    core::NotePath path;
    std::vector<std::size_t> positions;
    std::size_t hit_count = 0;
    std::vector<search::Hit> hits;
    bool binary = false;  // ikili not: önizleme yerine bilgi satırı
};

// İçerik araması modele dışarıdan verilir: ContentIndex'in G/Ç'si ve tembel kurulumu modelin işi değil.
using ContentSearcher = std::function<std::vector<search::ContentResult>(std::string_view query)>;

// İki listeleme aynı notları (yol, mtime, boyut, ikili) aynı sırada mı içeriyor? Yeniden tarama "değişti mi"
// kararı için.
bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b);

class TuiState {
public:
    // Önizleme sayfa kaydırma adımı (satır).
    static constexpr std::size_t kPreviewPageLines = 10;

    void set_notes(std::vector<core::NoteInfo> notes);
    void set_content_searcher(ContentSearcher searcher) { content_searcher_ = std::move(searcher); }
    // Önizlemenin satır sayısı: kaydırma ofseti son satırın ötesine geçemez.
    void set_preview_line_count(std::size_t lines);

    Intent handle(Key key, std::string_view text = {});

    // Sorguyu temizleyip notu seçer (yeni kaydedilen not süzgeçte kalmasın); yoksa false.
    bool reveal(const core::NotePath& path);

    // Seçimi satır numarasına koyar (son satıra kenetlenir): silmeden sonra komşu satırda kalmak için.
    void select_index(std::size_t index);

    const std::string& query() const { return query_; }
    Mode mode() const { return mode_; }
    const std::vector<Row>& rows() const { return rows_; }
    std::size_t total() const { return notes_.size(); }
    std::size_t selected() const { return selected_; }
    const Row* selected_row() const { return rows_.empty() ? nullptr : &rows_[selected_]; }
    // Önizlemenin ilk görünen satırı (0 tabanlı).
    std::size_t preview_scroll() const { return preview_scroll_; }

private:
    void refilter(bool query_changed);
    void move_selection(int delta);
    void scroll_preview(int delta);

    std::vector<core::NoteInfo> notes_;
    ContentSearcher content_searcher_;
    std::string query_;
    Mode mode_ = Mode::Name;
    std::vector<Row> rows_;
    std::size_t selected_ = 0;
    std::size_t preview_scroll_ = 0;
    std::size_t preview_lines_ = static_cast<std::size_t>(-1);  // bilinmiyor: üst sınır yok
};

}  // namespace noteapp::tui
