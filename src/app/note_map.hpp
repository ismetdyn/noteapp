#pragma once

#include "app/graph_layout.hpp"
#include "core/error.hpp"
#include "core/limits.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noteapp::app {

// Harita kapsamı (SPEC-gui "Harita", SPEC-tui "Harita"): oturumluk, durum dosyasına yazılmaz.
struct MapOptions {
    static constexpr int kMinDepth = 1;  // üst sınır config'te: limits.app.map_depth (MapModel::max_depth)
    static constexpr int kDefaultDepth = 2;
    bool whole_graph = false;  // false: gösterilen notun çevresi
    int depth = kDefaultDepth;
    bool show_tags = false;
    bool operator==(const MapOptions&) const = default;
};

enum class MapNodeKind { Note, Tag };
enum class MapEdgeKind { Link, Related, Tag };

struct MapNode {
    MapNodeKind kind = MapNodeKind::Note;
    std::string key;  // not yolu ya da etiket anahtarı (tag_key)
};

struct MapEdge {
    std::size_t from{};
    std::size_t to{};
    MapEdgeKind kind = MapEdgeKind::Link;
};

struct NoteMap {
    std::vector<MapNode> nodes;
    std::vector<MapEdge> edges;
    std::size_t total = 0;  // kapsamdaki düğüm sayısı (sınırdan önce)
    bool capped() const { return nodes.size() < total; }
};

// Saf: depo okumaz. `notes` liste sırasında (yeni önce): bütün çizgede sınırı aşınca en yeniler kalır. Çevre
// kapsamında `center`dan bağlantı ve ilişki kenarlarıyla (yönsüz) en çok `depth` adım, yakın önce; merkez
// yoksa bütün çizge. Etiketler gösterilen notların etiketleridir, notlardan sonra sınıra girer; etiket
// üzerinden gezilmez (yaygın bir etiket çevreyi bütün çizgeye çevirirdi). Eksik hedef kenar değildir (depo
// vermez).
NoteMap build_note_map(std::span<const core::NoteMeta> notes, std::span<const core::NoteEdge> edges,
                       const std::optional<core::NotePath>& center, const MapOptions& options,
                       std::size_t cap = SIZE_MAX);

// Haritanın arayüzden bağımsız durumu (plan-parity karar 8; GUI ve TUI aynı modeli çizer): kenar önbelleği,
// kapsam, kurulan harita, yerleşim ve arama eşleşmesi. Depo okumaz: kenarları çağıranın okuyucusu verir,
// yalnız eskiyince (taramada değişiklik görüldü). Harita görünür değilken update çağrılmaz, hiç okunmaz.
class MapModel {
public:
    using EdgeReader = std::function<std::expected<std::vector<core::NoteEdge>, core::Error>()>;

    MapModel() = default;
    MapModel(core::Limit node_limit, core::Limit depth_limit)
        : node_limit_(std::move(node_limit)), depth_limit_(std::move(depth_limit)) {}

    const MapOptions& options() const { return options_; }
    // Derinlik 1..max_depth()'e kıstırılır; değiştiyse harita sonraki update'te yeniden kurulur.
    void set_options(const MapOptions& options);
    // limits.app.map_nodes (0 = sınırsız): çizilen en çok düğüm; değiştiyse harita yeniden kurulur.
    // Verilmezse sınırsız.
    void set_node_limit(core::Limit limit);
    // limits.app.map_depth (0 = sınırsız): derinliğin üstü; küçülürse geçerli derinlik ona iner. Verilmezse
    // sınırsız.
    void set_depth_limit(core::Limit limit);
    // Arayüzlerin derinlik denetiminin üstü (GUI adımlayıcısı, TUI tuşları); sınırsızda INT_MAX.
    int max_depth() const;
    // Notlar ya da ilişkiler değişti: kenarlar sonraki update'te bir kez okunur.
    void edges_changed() { edges_stale_ = true; }
    // Harita güncel olur: kenarlar eskiyse okunur, seçenek ya da çevre merkezi değiştiyse bellekten kurulur
    // (bütün çizgede merkez yalnız vurgudur). Konumlar anahtarla korunur; çizge aynıysa yerleşim uyanmaz.
    void update(const EdgeReader& read, std::span<const core::NoteMeta> notes,
                const std::optional<core::NotePath>& center);
    const NoteMap& map() const { return map_; }
    GraphLayout& layout() { return layout_; }
    const GraphLayout& layout() const { return layout_; }
    const std::optional<core::NotePath>& center() const { return center_; }
    std::size_t generation() const { return generation_; }  // yerleşimin çizgesi her değiştiğinde artar
    std::size_t reads() const { return reads_; }            // test kancası: okuyucu çağrısı

    // Arama süzgeci (listeyle aynı ayrıştırıcı) eşleşen notları yakar, gerisi söner; etiket eşleşen bir
    // notunda yanar. `filtering` false ise hepsi yanar. `version` süzgecin ya da listenin sürümüdür: o ve
    // harita değişmedikçe yeniden kurulmaz (kare başına iş yok). `listed`: süzgeçten geçen notların yolları.
    const std::vector<char>& refresh_matches(bool filtering, std::size_t version,
                                             const std::function<std::unordered_set<std::string>()>& listed);
    // Son refresh_matches sonucu (düğüm sırasıyla 1/0); hiç kurulmadıysa boş.
    const std::vector<char>& matches() const { return matches_; }

private:
    MapOptions options_;
    core::Limit node_limit_;
    core::Limit depth_limit_;
    std::vector<core::NoteEdge> edges_;
    NoteMap map_;
    GraphLayout layout_;
    std::optional<core::NotePath> center_;
    std::vector<char> matches_;
    std::optional<std::pair<bool, std::size_t>> matched_;  // eşleşmelerin kurulduğu (filtering, version)
    bool edges_stale_ = true;
    bool stale_ = true;
    std::size_t reads_ = 0;
    std::size_t generation_ = 0;
};

}  // namespace noteapp::app
