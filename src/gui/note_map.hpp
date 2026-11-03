#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noteapp::gui {

// Harita kapsamı (SPEC-gui "Harita"): oturumluk, gui-state.toml'a yazılmaz.
struct MapOptions {
    static constexpr int kMinDepth = 1;
    static constexpr int kMaxDepth = 6;
    static constexpr int kDefaultDepth = 2;
    bool whole_graph = false;  // false: gösterilen notun çevresi
    int depth = kDefaultDepth;
    bool show_tags = false;
    bool operator==(const MapOptions&) const = default;
};

// Çizilen düğüm sınırı: üstü okunmaz ve yazılımsal çizicide kare bütçesini aşar (SPEC-gui kabul 11).
inline constexpr std::size_t kMaxMapNodes = 2000;

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
                       std::size_t cap = kMaxMapNodes);

}  // namespace noteapp::gui
