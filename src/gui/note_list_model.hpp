#pragma once

#include "app/listing.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/query.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Tek tanım src/app'tedir; bu ad mevcut çağıranlar ve testler için kalır.
using app::same_listing;

// Arama kutusunun düz listesi. Satırları app::Listing kurar (klasör kapsamı, sorgu argümanları,
// ad/içerik/genel arama, sıra; TUI ile aynı kural); model yalnız son sonucu ve satırın çizim metnini tutar.
// Ağaç ve Ayrıntılar satırları aynı Listing'den gelir (listing()).
class NoteListModel {
public:
    // Listeleme değiştiyse yeniden süzer ve true döner; aynıysa dokunmaz.
    bool set_notes(std::vector<core::NoteInfo> notes);
    void set_query(std::string query);
    // Göreli klasör yolu ('/' ayırıcılı); "" = tümü. Yalnız alt ağaçtaki notlar kalır.
    void set_folder(std::string folder);
    // `view:` argümanının notları; bağlanmazsa `view:` hiçbir notu geçirmez.
    void set_view_lookup(search::ViewLookup views) { listing_.set_view_lookup(std::move(views)); }
    // Ad (fuzzy), içerik ya da genel arama; içerik ve genel aramanın kaynağı listing()'e bağlanır.
    void set_mode(app::SearchMode mode);
    app::SearchMode mode() const { return mode_; }
    // Satırları aynı sorguyla yeniden kurar: içerik indeksi ya da senkron durumları değişti.
    void refresh() { refilter(); }

    app::Listing& listing() { return listing_; }
    const app::Listing& listing() const { return listing_; }

    int row_count() const { return static_cast<int>(rows_.size()); }
    const app::ListRow& row(int row) const { return rows_[static_cast<std::size_t>(row)]; }
    // Satırın notu; satırlar her zaman listedeki notlardan kurulur.
    const core::NoteInfo& note_at(int row) const;
    // Satırın metni: yol; içerik ve genel aramada eşleşen satır varsa "yol  (N)".
    std::string display(int row) const;
    // Yalnız ilişkiyle bulunan satırın nedeni (app::reason_text); öbür satırlarda boş.
    std::string reason(int row) const;
    // Yol metnindeki vurgulanacak kod noktalarının bayt ofsetleri (artan).
    const std::vector<std::size_t>& highlights(int row) const { return this->row(row).positions; }
    // Yol listede yoksa -1.
    int index_of(const core::NotePath& path) const;
    // Sorgu argümanı geçersizse format_error metni (TUI ile aynı); satırlar son geçerli sorgununkidir.
    const std::optional<std::string>& query_error() const { return query_error_; }
    std::size_t total() const { return listing_.notes().size(); }
    // Satırlar her yeniden kurulduğunda artar: "yalnız değişimde sıfırla" kuralını ölçer.
    std::size_t resets() const { return resets_; }

private:
    void refilter();

    app::Listing listing_;
    std::vector<app::ListRow> rows_;
    std::string query_;
    std::string folder_;
    app::SearchMode mode_ = app::SearchMode::Name;
    std::optional<std::string> query_error_;
    std::size_t resets_ = 0;
};

}  // namespace noteapp::gui
