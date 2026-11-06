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
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
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
    // Süzgeçler (F2): gizlenen kenar çizilmez ve kapsam onun üstünden gezilmez.
    bool show_links = true;    // LINKS_TO
    bool show_related = true;  // RELATED_TO
    // Dolu: ilişkilerden yalnız bu adı taşıyanlar (anahtar tag_key ile eşlenir; "" adsızdır). Bağlantılar
    // show_links'e bağlıdır.
    std::optional<std::string> relation = std::nullopt;
    // Dolu: yalnız bu etiketli notlar, aramanın `tag:` kuralıyla (search::parse_query: `#` yazılabilir, iç içe
    // `proje` `proje/alt`ı da alır). Gösterilen not (merkez) her zaman kalır: kapsam ondan başlar.
    std::string tag = {};
    bool operator==(const MapOptions&) const = default;
};

enum class MapNodeKind { Note, Tag };
enum class MapEdgeKind { Link, Related, Tag };

struct MapNode {
    MapNodeKind kind = MapNodeKind::Note;
    std::string key;  // not yolu ya da etiket anahtarı (tag_key)
};

// Related'da yön ve ad core::NoteEdge'inkidir: yönsüzde from/to sırası anlamsız, yönlüde from kaynaktır.
struct MapEdge {
    std::size_t from{};
    std::size_t to{};
    MapEdgeKind kind = MapEdgeKind::Link;
    std::string name = {};  // ilişkinin adı; boş = adsız. Güvenilmez: arayüz kendi temizliğiyle çizer
    bool directed = false;
    core::RelationId relation = {};  // Related'ın kimliği: kaldırma yalnız bu ilişkiyi siler
};

struct NoteMap {
    std::vector<MapNode> nodes;
    std::vector<MapEdge> edges;
    std::size_t total = 0;  // kapsamdaki düğüm sayısı (sınırdan önce)
    std::vector<std::size_t> path;  // en kısa yolun düğümleri baştan sona (F2); yol yoksa boş
    std::vector<char> path_edges;   // kenar başına 1: yolun ardışık iki düğümünü bağlar (Tag hariç)
    bool capped() const { return nodes.size() < total; }
};

// Komşu açma (F2): düğümden şu an görünen eriminin `depth` adım ötesine, yalnız seçilen türün ya da adın
// kenarlarıyla. Etiket kenarı gezilmez.
struct MapExpansion {
    std::optional<MapEdgeKind> kind = std::nullopt;  // boş: bağlantı ve ilişki
    std::optional<std::string> name = std::nullopt;  // dolu: yalnız bu adlı ilişki (tag_key; "" adsız)
    int depth = 1;
    bool operator==(const MapExpansion&) const = default;
};

struct MapPathResult {
    enum class Status { Found, None, Hidden };
    Status status = Status::None;
    std::size_t hops = 0;  // Found ve Hidden: yolun kenar sayısı
};
// Durum satırı ve bildirim metni (iki arayüz aynı metni gösterir). `from`/`to` arayüzün temizlediği yollardır.
std::string map_path_text(const MapPathResult& result, const std::string& from, const std::string& to,
                          bool directed);

// Geçerli kenar süzgecinin açılışı (GUI çift tıklama, TUI `x`): ad süzgeci o adlı ilişki, tek tür süzgeci o tür,
// ikisi açıkken bütün türler. Süzgecin gizlediği kenar zaten gezilmez; açılış süzgeç kalkınca da aynı kalır.
MapExpansion expansion_for(const MapOptions& options);

// Oturumluk kapsam eklemeleri (F2), not yoluyla. Bütün çizgede açma ve kapama yok sayılır (her şey görünür).
struct MapScope {
    struct Path {
        std::string from;
        std::string to;
        bool directed = false;  // bağlantı ve yönlü ilişki yalnız kaynaktan hedefe gezilir
        bool operator==(const Path&) const = default;
    };
    std::map<std::string, MapExpansion> expanded;
    std::set<std::string> collapsed;  // komşuları gizli: kapsam bu notun içinden geçmez
    std::set<std::string> kept;       // her zaman görünür (haritadan oluşturulan not)
    std::optional<Path> path;         // yolun düğümleri kapsama girer (derinliğin ötesinde de)
    bool operator==(const MapScope&) const = default;
};

// Saf: depo okumaz. `notes` liste sırasında (yeni önce): bütün çizgede sınırı aşınca en yeniler kalır. Çevre
// kapsamında `center`dan bağlantı ve ilişki kenarlarıyla (yönsüz) en çok `depth` adım, yakın önce; merkez
// yoksa bütün çizge. Etiketler gösterilen notların etiketleridir, notlardan sonra sınıra girer; etiket
// üzerinden gezilmez (yaygın bir etiket çevreyi bütün çizgeye çevirirdi). Eksik hedef kenar değildir (depo
// vermez).
// Süzgeçler (MapOptions) ve kapsam eklemeleri (MapScope) de uygulanır. Sıra: merkez, yolun düğümleri, tutulan
// notlar, merkezden derinlik kadar, sonra açılan notların komşuları; sınır en son gelenleri keser.
NoteMap build_note_map(std::span<const core::NoteMeta> notes, std::span<const core::NoteEdge> edges,
                       const std::optional<core::NotePath>& center, const MapOptions& options,
                       std::size_t cap = SIZE_MAX, const MapScope& scope = {});

// `from`dan `to`ya en az kenarlı yol, düğüm dizisi olarak (genişlik öncelikli arama, eşitlikte kenar sırası).
// directed: Link ve yönlü Related yalnız from → to gezilir, yönsüz Related iki yöne; değilse hepsi iki yöne.
// Tag kenarı gezilmez. Yol yoksa nullopt; from == to tek düğümdür.
std::optional<std::vector<std::size_t>> shortest_path(std::size_t count, std::span<const MapEdge> edges,
                                                      std::size_t from, std::size_t to, bool directed);

// Haritada bul (F2): katlanmış (search::fold_to_utf8) ve kırpılmış `typed`i yolunda ya da `#etiket`inde
// geçiren düğümler, harita sırasıyla. Boş metin hiçbir şey bulmaz.
std::vector<std::size_t> find_map_nodes(const NoteMap& map, std::string_view typed);

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
    // Harita her değiştiğinde artar, yalnız ilişkinin adı ya da yönü değişse de (yerleşim ve kuşak aynı
    // kalır): kenar başına çizim önbelleği buna bağlıdır, kameranın sığdırması generation'a.
    std::size_t version() const { return version_; }
    std::size_t reads() const { return reads_; }            // test kancası: okuyucu çağrısı

    // Arama süzgeci (listeyle aynı ayrıştırıcı) eşleşen notları yakar, gerisi söner; etiket eşleşen bir
    // notunda yanar. `filtering` false ise hepsi yanar. `version` süzgecin ya da listenin sürümüdür: o ve
    // harita değişmedikçe yeniden kurulmaz (kare başına iş yok). `listed`: süzgeçten geçen notların yolları.
    const std::vector<char>& refresh_matches(bool filtering, std::size_t version,
                                             const std::function<std::unordered_set<std::string>()>& listed);
    // Son refresh_matches sonucu (düğüm sırasıyla 1/0); hiç kurulmadıysa boş.
    const std::vector<char>& matches() const { return matches_; }

    // --- Keşif (F2): oturumluk, sonraki update'te uygulanır ---
    const MapScope& scope() const { return scope_; }
    // Açılış düğümden şu an görünen erimin ötesine gider (merkezde derinlik + n, çevrenin sınırında + n). Aynı
    // tür ve adla yeniden açmak bir adım ekler (n en çok max_depth); başka açılış bir adımdan başlar. Kapamayı
    // kaldırır.
    void expand(const std::string& note, const MapExpansion& spec);
    // Açılmışsa yalnız açılışı geri alır (harita açılıştan önceki hâli); değilse komşuları gizler.
    void collapse(const std::string& note);
    // Açılmışsa kapatır (false), değilse `spec`le bir adım açar (true): GUI çift tıklama, TUI `x`.
    bool toggle_neighbours(const std::string& note, const MapExpansion& spec);
    // Haritadan oluşturulan not kapsamda kalır; `pin_at` doluysa düğüm haritaya girince oraya sabitlenir.
    void keep(const std::string& note, std::optional<LayoutPoint> pin_at = std::nullopt);
    // En kısa yol (MapScope::Path): süzgeçlerden geçen notlar ve kenarlar üzerinde, kapsamdan bağımsız.
    // Found: yol sonraki update'te çizilir (kenarlar değişince yeniden aranır). None ve Hidden'da (yol var ama
    // limits.app.map_nodes düğümlerini çizmeye yetmez) eski yol silinir; hops Hidden'da da doludur.
    MapPathResult find_path(std::span<const core::NoteMeta> notes, const std::string& from, const std::string& to,
                            bool directed);
    void clear_path();

private:
    void forget_missing(std::span<const core::NoteMeta> notes);  // kaybolan notun kapsam anahtarları düşer

    MapOptions options_;
    core::Limit node_limit_;
    core::Limit depth_limit_;
    std::vector<core::NoteEdge> edges_;
    NoteMap map_;
    GraphLayout layout_;
    std::optional<core::NotePath> center_;
    std::vector<char> matches_;
    MapScope scope_;
    std::map<std::string, LayoutPoint> pending_pins_;  // keep(pin_at): düğüm haritaya girince sabitlenir
    std::optional<std::pair<bool, std::size_t>> matched_;  // eşleşmelerin kurulduğu (filtering, version)
    bool edges_stale_ = true;
    bool stale_ = true;
    std::size_t reads_ = 0;
    std::size_t generation_ = 0;
    std::size_t version_ = 0;
};

}  // namespace noteapp::app
