#pragma once

#include "core/note_store.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// REVIEW satırındaki bir [[bağlantı]]: [begin, end) ayraçlar dahil, `target` core::link_target anahtarıdır
// (satırın içine bakar). Deponun taramasıyla aynı kural, satır başına: satırı aşan bağlantı yoktur.
struct WikiSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view target;
};
std::vector<WikiSpan> wiki_spans(std::string_view line);

// REVIEW'un altındaki bağlantı bölümünün saf modeli: düz satırlar, seçim, pencere. Seçim yalnız öğe
// satırlarında gezer; binlerce satırda çizim ve kaydırma görünene orantılıdır, başka kopya yoktur.
class LinkPanel {
public:
    // Görünen satır sınırı: panelin üçte biri, en az bu kadar, en çok bu kadar.
    static constexpr int kMinRows = 3;
    static constexpr int kMaxRows = 12;

    // Aynı notun yeni listesi seçimi korur, başka notunki ilk öğeye döner.
    void set(core::NoteLinks links, bool same_note);
    bool empty() const { return lines_.empty(); }
    const core::NoteLinks& links() const { return links_; }
    std::span<const core::LinkLine> lines() const { return lines_; }

    std::size_t selected() const { return selected_; }
    const core::LinkLine* selected_line() const { return lines_.empty() ? nullptr : &lines_[selected_]; }
    // Seçili satırın notu; eksik hedefte nullptr.
    const core::NotePath* selected_path() const;
    void move(int delta);
    // Başlıkta öğeye inmez, seçim yerinde kalır.
    void select(std::size_t line);

    // Taşan bölümün konum göstergesi: başlıklar sayılmaz, 1 tabanlı.
    std::size_t item_count() const;
    std::size_t selected_ordinal() const;

    // `pane_rows` satırlık panelde kaç satır görünür.
    int visible_rows(int pane_rows) const;
    // Önizlemeden alınan satır: görünenler ve ayırıcı; ayırıcı etiketleri de taşır, yalnız etiket varsa tek
    // satırdır. Bağlantı ve etiket yoksa 0.
    int reserved_rows(int pane_rows) const;
    // Pencerenin ilk satırı: seçili satır görünür kalır.
    std::size_t top(std::size_t visible) const;

private:
    core::NoteLinks links_;
    std::vector<core::LinkLine> lines_;
    std::size_t selected_ = 0;
};

}  // namespace noteapp::tui
