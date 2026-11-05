#pragma once

#include "core/folder_tree.hpp"
#include "core/limits.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "search/fuzzy.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::app {

// İki listeleme aynı notları (yol, revizyon, mtime, boyut) aynı sırada mı içeriyor? Yeniden tarama "değişti
// mi" kararı için.
bool same_listing(std::span<const core::NoteInfo> a, std::span<const core::NoteInfo> b);

// Yeniden taramanın karşılaştırdığı depo görüntüsü: biri farklıysa liste ve önizleme tazelenir.
struct StoreScan {
    std::vector<core::NoteInfo> notes;
    std::size_t trash = 0;                // senkron yalnız çöpü de değiştirebilir; okunamayan çöp 0
    std::vector<std::string> folders;     // boş klasör not listesini değiştirmez
    std::uint64_t graph_version = 0;      // senkron yalnız ilişki değiştirmiş olabilir: bağlantı bölümü eskir
    std::vector<std::string> views = {};  // kayıtlı görünüm adları; değişimi graph_version'da görünür
};
StoreScan scan_store(const core::NoteStore& store);
// Kayıtlı görünüm adları ("Run view" ve `view:` tamamlaması).
std::vector<std::string> view_names(const core::NoteStore& store);
bool same_scan(const StoreScan& a, const StoreScan& b);

// Sıra alanlarının kalıcı adları: tercih dosyalarındaki `sort` değeri (tui-state.toml, gui-state.toml).
inline constexpr std::array<std::pair<search::SortField, std::string_view>, 4> kSortFieldNames = {{
    {search::SortField::Name, "name"},
    {search::SortField::Size, "size"},
    {search::SortField::Updated, "updated"},
    {search::SortField::Created, "created"},
}};

// Sıralama menüsünde alan seçilince: ad doğal olarak artan; boyut ve tarihte önce en büyük ve en yeni.
constexpr search::SortSpec sort_for(search::SortField field) {
    return {field, field != search::SortField::Name};
}

// Ad: fuzzy yol; içerik: tam metin; genel: ad ve içerik birlikte (search::search_all).
enum class SearchMode { Name, Content, Global };

// İçerik ve genel arama dışarıdan verilir: ContentIndex'in G/Ç'si ve tembel kurulumu listenin işi değil.
using ContentSearcher = std::function<std::vector<search::ContentResult>(std::string_view text)>;
using GlobalSearcher = std::function<std::expected<std::vector<search::GlobalHit>, search::QueryError>(
    std::string_view query, std::span<const core::NoteMeta> notes)>;
// `index` tembel kurulur (ilk aramada). Sonuç sınırı limits.search.content_results; 0 = tüm sonuçlar.
ContentSearcher content_searcher(std::function<const search::ContentIndex&()> index, core::Limit results);

// Listenin bir satırı; iki arayüz kendi çizimini bundan kurar.
struct ListRow {
    core::NotePath path;  // not ya da klasör yolu
    bool is_folder = false;
    std::string name = {};  // ağaç satırında son parça; düz arama satırında boş (tam yol gösterilir)
    std::size_t depth = 0;  // ağaçta girinti: istenen klasörün çocukları 0
    // Ağaçta kendi klasörü içinde 1'den (klasörler ve notlar birlikte sayılır); düz listede sonuç sırası.
    std::size_t number = 0;
    bool has_children = false;                // klasör: içinde not ya da klasör var
    bool open = false;                        // klasör ağaçta yerinde açık
    std::vector<std::size_t> positions = {};  // ad eşleşmesi: yolun UTF-8'inde bayt ofsetleri, artan
    std::size_t hit_count = 0;                // içerik eşleşmesi: eşleşen satır sayısı
    std::vector<search::Hit> hits = {};       // içerik: ilk eşleşen satırlar; genel: en iyi satır
    bool show_best_line = false;              // genel arama: en iyi satır yolun altında gösterilir
};

// Arama sonucu. Sorgu bozuksa `error` doludur ve satırlar son geçerli sorgunun bu listedeki sonucudur
// (bozuk değer metin sayılmaz; kapsam değiştiyse eski satır konumları da taşmaz).
struct SearchResult {
    std::vector<ListRow> rows;
    std::optional<search::QueryError> error;
};

// Notlar, klasörler, senkron durumları ve görünüm araması üstünde tek listeleme kuralı: ağaç satırları,
// klasör (Ayrıntılar) satırları ve düz arama satırları. G/Ç yapmaz; depo verisini çağıran verir.
class Listing {
public:
    // Listeleme aynıysa (same_listing) dokunmaz ve false döner.
    bool set_notes(std::vector<core::NoteInfo> notes);
    // Boş klasörler de görünsün diye deponun klasör listesi; notlardan türeyenlerle birleşir. Aynıysa false.
    bool set_folders(std::vector<std::string> folders);
    // İşçiden gelen senkron durumları: `sync:` süzgeci ve üst veri bunları görür. Aynıysa dokunmaz ve false
    // döner.
    bool set_sync_states(std::map<core::NotePath, core::SyncState> states);
    // `view:` süzgecinin notları; bağlanmazsa `view:` hiçbir notu geçirmez.
    void set_view_lookup(search::ViewLookup views) { views_ = std::move(views); }
    void set_content_searcher(ContentSearcher searcher) { content_searcher_ = std::move(searcher); }
    void set_global_searcher(GlobalSearcher searcher) { global_searcher_ = std::move(searcher); }
    // Kullanıcının tercih ettiği sıra: ağaç, Ayrıntılar ve metinsiz arama bu sırayla; `sort:` onu ezer.
    void set_sort(search::SortSpec sort) { sort_ = sort; }
    const search::SortSpec& sort() const { return sort_; }

    std::span<const core::NoteInfo> notes() const { return notes_; }
    std::span<const core::NoteMeta> metas() const { return metas_; }
    // Listedeki notun üst verisi; yoksa nullptr.
    const core::NoteMeta* meta_of(const core::NotePath& path) const;
    // Listedeki notun kendisi (revizyonla); yoksa nullptr.
    const core::NoteInfo* note_of(const core::NotePath& path) const;
    const core::FolderTree& tree() const { return tree_; }

    // Ağaçta yerinde açık klasörler (kalıcılığı arayüzün işi).
    bool is_open(const std::string& folder) const { return open_.contains(folder); }
    void set_open(const std::string& folder, bool open);
    void set_open_folders(std::set<std::string> folders) { open_ = std::move(folders); }
    const std::set<std::string>& open_folders() const { return open_; }
    // Ağaçta artık olmayan klasörleri bırakır.
    void prune_open_folders();

    // `folder`ın çocukları, açık klasörlerin çocukları kendi satırlarının altında; her düzeyde önce
    // klasörler, sonra notlar, tercih edilen sırayla. Klasör yoksa boş. GUI gezgininin kök satırı (hep açık,
    // numarasız, çocukları derinlik 1) burada yoktur: o arayüzün çizim satırıdır, GUI ekler.
    std::vector<ListRow> tree_rows(std::string_view folder) const;
    // Ayrıntılar: yalnız `folder`ın doğrudan çocukları, aynı sırayla ve numarayla.
    std::vector<ListRow> folder_rows(std::string_view folder) const;

    // Düz arama: argümanlar (search::parse_query) süzer; kalan metin moda göre sıralar (ad: fuzzy, içerik:
    // ContentSearcher, genel: GlobalSearcher). Metin yoksa süzülmüş liste tercih edilen sırayla. `sort:`
    // verilirse o sıra (eşitlikte arama sırası). `scope` boş değilse yalnız o klasörün alt ağacındaki notlar.
    SearchResult search(std::string_view query, SearchMode mode, std::string_view scope = {});
    // Arama dışı görünüme (klasör, çöp) geçildi: sonraki bozuk sorgu eski aramanın satırlarını kurmaz; ilk
    // açılıştaki gibi sorgusuz liste ve hata döner.
    void forget_last_query() { last_valid_query_.clear(); }
    // limits.search.fuzzy_candidate_chars (sınırsız SIZE_MAX): ad aramasında vurgu konumu üretilen en uzun
    // aday.
    void set_fuzzy_candidate_chars(std::size_t chars) { fuzzy_candidate_chars_ = chars; }
    std::size_t fuzzy_candidate_chars() const { return fuzzy_candidate_chars_; }

private:
    // Geçerli sorgunun satırları; yalnız genel arama hata döndürebilir.
    std::expected<std::vector<ListRow>, search::QueryError> rows_for(const search::ParsedQuery& parsed,
                                                                     std::string_view query, SearchMode mode,
                                                                     std::string_view scope) const;
    void append_children(const core::FolderNode& node, std::size_t depth, bool recurse,
                         std::vector<ListRow>& rows) const;
    void sort_rows(std::vector<ListRow>& rows, const search::SortSpec& sort) const;
    void rebuild_metas();

    std::vector<core::NoteInfo> notes_;
    std::vector<core::NoteMeta> metas_;  // notes_ ile aynı sırada
    std::map<core::NotePath, std::size_t> meta_index_;
    std::vector<std::string> folders_;
    std::map<core::NotePath, core::SyncState> sync_states_;
    core::FolderTree tree_;
    std::set<std::string> open_;
    search::SortSpec sort_{search::SortField::Updated, true};  // TUI ListPrefs varsayılanı: en yeni başta
    search::ViewLookup views_;
    ContentSearcher content_searcher_;
    GlobalSearcher global_searcher_;
    std::string last_valid_query_;  // bozuk sorguda satırlar bununla kurulur
    // Arayüz config'ten verir (set_fuzzy_candidate_chars); verilmezse defaults.toml'daki değer.
    std::size_t fuzzy_candidate_chars_ = search::kDefaultFuzzyCandidateChars;
};

}  // namespace noteapp::app
