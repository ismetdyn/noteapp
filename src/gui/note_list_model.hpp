#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/query.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::gui {

// İki listeleme aynı notları (yol, mtime, boyut) aynı sırada mı içeriyor? Yeniden tarama "değişti mi" kararı
// için (TUI'deki same_listing ile aynı anlam; TUI kütüphanesine bağlanmamak için burada ayrı).
bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b);

// Süzülmüş not listesi: klasör kapsamı, sorgu argümanları (search::parse_query, TUI/CLI ile aynı) ve kalan
// metinle ad modunda fuzzy sıralama (boş metinde mtime azalan); `sort:` verilirse o sıra.
class NoteListModel {
public:
    // Listeleme değiştiyse yeniden süzer ve true döner; aynıysa dokunmaz.
    bool set_notes(std::vector<core::NoteInfo> notes);
    void set_query(std::string query);
    // Göreli klasör yolu ('/' ayırıcılı); "" = tümü. Yalnız alt ağaçtaki notlar kalır.
    void set_folder(std::string folder);
    // `view:` argümanının notları; bağlanmazsa `view:` hiçbir notu geçirmez.
    void set_view_lookup(search::ViewLookup views) { views_ = std::move(views); }

    // İçerik modu: sıralamayı ContentIndex yapar (GuiState, sorgunun argümansız metniyle); model yalnız
    // sırayı ve sayıları gösterir. nullopt = ad modu (fuzzy). Klasör kapsamı ve argüman dışındaki sonuçlar
    // düşer.
    void set_content_results(std::optional<std::vector<search::ContentResult>> results);

    int row_count() const { return static_cast<int>(rows_.size()); }
    const core::NoteInfo& note_at(int row) const {
        return scope_[rows_[static_cast<std::size_t>(row)].scope_index];
    }
    // Satırın metni: yol; içerik modunda "yol  (N)", N = eşleşen satır sayısı.
    std::string display(int row) const;
    // Yol metnindeki vurgulanacak kod noktalarının bayt ofsetleri (artan).
    const std::vector<std::size_t>& highlights(int row) const {
        return rows_[static_cast<std::size_t>(row)].byte_positions;
    }
    // Yol listede yoksa -1.
    int index_of(const core::NotePath& path) const;
    // Sorgu argümanı geçersizse format_error metni (TUI ile aynı); satırlar son geçerli sorgununkidir.
    const std::optional<std::string>& query_error() const { return query_error_; }
    std::size_t total() const { return notes_.size(); }
    // Satırlar her yeniden kurulduğunda artar: "yalnız değişimde sıfırla" kuralını ölçer.
    std::size_t resets() const { return resets_; }

private:
    struct Row {
        std::size_t scope_index{};
        std::vector<std::size_t> byte_positions;
        std::size_t hits = 0;  // içerik modunda eşleşen satır sayısı
    };

    void rebuild_scope();
    void refilter();
    // Satırları `sort:` alanına göre kararlı sıralar; metas scope_ ile aynı sırada.
    void sort_rows(const search::SortSpec& sort, std::span<const core::NoteMeta> metas);

    std::vector<core::NoteInfo> notes_;
    std::vector<core::NoteInfo> scope_;  // klasör süzgecinden geçenler; tuş başına kopyalamamak için önbellek
    std::vector<Row> rows_;
    std::string query_;
    std::string folder_;
    std::optional<std::vector<search::ContentResult>> content_;
    std::optional<std::string> query_error_;
    search::ViewLookup views_;
    search::ParsedQuery valid_query_;  // son geçerli sorgu; bozuk sorguda satırlar bununla kurulur
    std::size_t resets_ = 0;
};

}  // namespace noteapp::gui
