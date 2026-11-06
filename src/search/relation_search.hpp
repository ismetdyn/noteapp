#pragma once

#include "core/note_store.hpp"
#include "search/query.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noteapp::search {

// Notun ad ya da içerik dışında, çizgeden gelen eşleşme nedeni (SPEC-search "İlişki araması"). Tür sırası
// güçten zayıfa: etiket, ilişki adı, bağlı not.
struct RelationReason {
    enum class Kind { Tag, Relation, Linked };
    Kind kind = Kind::Tag;
    // Tag: etiket anahtarı (NoteMeta::tags). Relation: eşleşen ilişki adı. Linked: komşuya bağlayan kenarın
    // adı, yalnız tek adımda (boş: adsız ilişki ya da [[bağlantı]]). Senkronla gelebilir: güvenilmez.
    std::string label = {};
    // Relation: kenarın öbür ucunun yolu; Linked: adı eşleşen notun yolu; Tag: boş. Yol metindir, NotePath
    // değil: büyük çizgede on binlerce sonuçta yol bileşenleri kopyalanmasın.
    std::string other = {};
    core::RelationDirection direction = core::RelationDirection::Undirected;  // Relation: notun gözünden
    std::size_t hops = 0;                                                     // Linked: komşuya adım
    bool operator==(const RelationReason&) const = default;
};

struct RelationHit {
    std::size_t index{};  // notes içindeki sıra
    RelationReason reason;
};

// Kenar listesinin arama dizini: düğümler (yol, katlanmış yol), komşuluk ve ilişki adları. Kurulumu kenar
// sayısıyla doğrusaldır; sorgu ise eşleşen ada ve gezilen komşuya bağlıdır. Arayüz kenar listesi değişince
// (görüntü başına) bir kez kurar, tuş başına yeniden kullanır (app::Listing). Kenar listesine bağlı kalmaz.
struct RelationGraph {
    explicit RelationGraph(std::span<const core::NoteEdge> edges);

    std::vector<std::string> paths;                         // düğüm -> yol (NotePath::string)
    std::vector<core::NotePath> notes;                      // düğüm -> yol; `rel:`/`linked:` süzgeci için
    std::vector<std::string> folded_paths;                  // düğüm -> katlanmış yol
    std::unordered_map<std::string, std::size_t> node_of;   // yol -> düğüm
    std::vector<std::vector<std::size_t>> edges_of;         // düğüm -> kenar
    std::vector<std::pair<std::size_t, std::size_t>> ends;  // kenar -> (from, to) düğümü
    std::vector<std::size_t> name_of;                       // kenar -> names sırası; kNoName: ad yok
    std::vector<char> directed;                             // kenar -> yönlü RELATED_TO mı
    std::vector<std::string> names;                         // RELATED_TO adları (gösterim), tekil
    std::vector<std::string> folded_names;                  // names'in katlanmışı
    std::vector<std::vector<std::size_t>> edges_named;      // names sırası -> kenarlar

    static constexpr std::size_t kNoName = SIZE_MAX;
    std::size_t other(std::size_t edge, std::size_t node) const {
        return ends[edge].first == node ? ends[edge].second : ends[edge].first;
    }
};

// İlişki araması girdisi. Arayüz kenarları depodan bir kez okur (app::search_edges), derinliği config'ten
// verir (app::relation_depth).
struct GraphInput {
    std::span<const core::NoteEdge> edges = {};
    // Komşu adının arandığı en çok adım (limits.search.relation_depth; sınırsız SIZE_MAX). nullopt: ilişki
    // araması kapalı (tuning.search_relations); `rel:` ve `linked:` yine süzer.
    std::optional<std::size_t> depth = 1;
    // edges'in hazır dizini (görüntü başına önbellek); nullptr ise çağrıda kurulur.
    const RelationGraph* index = nullptr;
    // index varken: notes[i]'nin düğümü (kNoNode: kenarı yok), notes ile aynı boyda. Boşsa yoldan bulunur;
    // ön yüz bunu da önbellekler (50.000 notta tuş başına yol üretimi olmasın).
    std::span<const std::size_t> nodes = {};
};

inline constexpr std::size_t kNoNode = SIZE_MAX;
// notes[i]'nin index'teki düğümü (kNoNode: kenarı yok). GraphInput::nodes'un önbelleği bununla kurulur.
std::vector<std::size_t> note_nodes(const RelationGraph& index, std::span<const core::NoteMeta> notes);

// `text`in katlanmış terimlerinin hepsi aynı etikette, aynı ilişki adında ya da `depth` adım içindeki bir
// komşunun yolunda alt dizgi olarak geçen notlar; not başına en güçlü neden (tür, sonra adım). Sonuç yalnız
// `allowed`dakilerdir, eşleşen komşu süzgeç dışında da olabilir. Adı eşleşen notun kendisi (adım 0) dönmez:
// o ad eşleşmesidir. Sıra: neden türü, adım, sonra güncelleme azalan. Satır başına sorgu yok.
std::vector<RelationHit> relation_hits(std::string_view text, std::span<const core::NoteMeta> notes,
                                       std::span<const std::size_t> allowed, const GraphInput& graph);

// `rel:` ve `linked:` süzgeçlerinin notlarını (Filter::notes) kenarlardan kurar. Bağlanmamış süzgeç hiçbir
// notu geçirmez (`view:` kuralı). graph.index varsa o kullanılır.
void bind_graph_filters(std::span<Filter> filters, const GraphInput& graph);
// Sorguda kenar isteyen süzgeç var mı (`rel:`, `linked:`).
bool has_graph_filter(std::span<const Filter> filters);

}  // namespace noteapp::search
