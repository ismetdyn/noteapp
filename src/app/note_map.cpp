#include "app/note_map.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace noteapp::app {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

// Not indisleri üzerinden kenar; ucu listede olmayan (eski kayıt) kenar düşer.
std::vector<MapEdge> indexed_edges(std::span<const core::NoteMeta> notes,
                                   std::span<const core::NoteEdge> edges) {
    std::unordered_map<std::string, std::size_t> index;
    index.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i)
        index.emplace(notes[i].path.string(), i);
    std::vector<MapEdge> out;
    out.reserve(edges.size());
    for (const core::NoteEdge& edge : edges) {
        const auto from = index.find(edge.from.string());
        const auto to = index.find(edge.to.string());
        if (from == index.end() || to == index.end())
            continue;
        out.push_back({from->second, to->second,
                       edge.kind == core::EdgeKind::RelatedTo ? MapEdgeKind::Related : MapEdgeKind::Link,
                       edge.name, edge.directed, edge.relation});
    }
    return out;
}

// Merkezden yönsüz genişlik öncelikli arama: yakın önce, merkez ilk.
std::vector<std::size_t> neighbourhood(std::size_t count, const std::vector<MapEdge>& edges,
                                       std::size_t center, int depth) {
    std::vector<std::vector<std::size_t>> adjacent(count);
    for (const MapEdge& edge : edges) {
        adjacent[edge.from].push_back(edge.to);
        adjacent[edge.to].push_back(edge.from);
    }
    std::vector<int> distance(count, -1);
    distance[center] = 0;
    std::vector<std::size_t> order{center};
    for (std::size_t at = 0; at < order.size(); ++at) {
        const std::size_t node = order[at];
        if (distance[node] >= depth)
            continue;
        for (const std::size_t next : adjacent[node]) {
            if (distance[next] < 0) {
                distance[next] = distance[node] + 1;
                order.push_back(next);
            }
        }
    }
    return order;
}

// Sıradan bağımsız çizge imzası: (tür, anahtar) düğümler ve uçları anahtarla kenarlar, sıralı. Kenarın adı ve
// yönü de imzadadır: yeniden adlandırılan ilişkinin etiketi eskimesin; kimliği de: kaldırma eski kimliği
// hedeflemesin.
using GraphSignature = std::pair<std::vector<std::pair<MapNodeKind, std::string_view>>,
                                 std::vector<std::tuple<std::string_view, std::string_view, MapEdgeKind,
                                                        std::string_view, bool, core::RelationId>>>;

GraphSignature signature(const NoteMap& map) {
    GraphSignature out;
    out.first.reserve(map.nodes.size());
    for (const MapNode& node : map.nodes)
        out.first.emplace_back(node.kind, node.key);
    out.second.reserve(map.edges.size());
    for (const MapEdge& edge : map.edges)
        out.second.emplace_back(map.nodes[edge.from].key, map.nodes[edge.to].key, edge.kind, edge.name,
                                edge.directed, edge.relation);
    std::ranges::sort(out.first);
    std::ranges::sort(out.second);
    return out;
}

}  // namespace

NoteMap build_note_map(std::span<const core::NoteMeta> notes, std::span<const core::NoteEdge> edges,
                       const std::optional<core::NotePath>& center, const MapOptions& options,
                       std::size_t cap) {
    const std::vector<MapEdge> note_edges = indexed_edges(notes, edges);
    const auto start = center ? std::ranges::find(notes, *center, &core::NoteMeta::path) : notes.end();
    std::vector<std::size_t> order;
    if (options.whole_graph || start == notes.end()) {
        order.resize(notes.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
    } else {
        order = neighbourhood(notes.size(), note_edges, static_cast<std::size_t>(start - notes.begin()),
                              options.depth);
    }

    NoteMap map;
    map.total = order.size();
    std::vector<std::size_t> node_of(notes.size(), kNone);
    const std::size_t shown = std::min(order.size(), cap);
    for (std::size_t i = 0; i < shown; ++i) {
        node_of[order[i]] = map.nodes.size();
        map.nodes.push_back({MapNodeKind::Note, notes[order[i]].path.string()});
    }
    for (const MapEdge& edge : note_edges) {
        if (node_of[edge.from] != kNone && node_of[edge.to] != kNone)
            map.edges.push_back(
                {node_of[edge.from], node_of[edge.to], edge.kind, edge.name, edge.directed, edge.relation});
    }
    if (!options.show_tags)
        return map;

    std::set<std::string_view> scope_tags;
    for (const std::size_t note : order)
        scope_tags.insert(notes[note].tags.begin(), notes[note].tags.end());
    map.total += scope_tags.size();
    std::unordered_map<std::string_view, std::size_t> tag_node;
    for (std::size_t i = 0; i < shown; ++i) {
        for (const std::string& tag : notes[order[i]].tags) {
            auto found = tag_node.find(tag);
            if (found == tag_node.end()) {
                if (map.nodes.size() >= cap)
                    continue;
                found = tag_node.emplace(tag, map.nodes.size()).first;
                map.nodes.push_back({MapNodeKind::Tag, tag});
            }
            map.edges.push_back({i, found->second, MapEdgeKind::Tag});
        }
    }
    return map;
}

void MapModel::set_options(const MapOptions& options) {
    MapOptions clamped = options;
    clamped.depth = std::clamp(options.depth, MapOptions::kMinDepth, max_depth());
    if (clamped == options_)
        return;
    options_ = clamped;
    stale_ = true;
}

void MapModel::set_node_limit(core::Limit limit) {
    if (limit == node_limit_)
        return;
    node_limit_ = std::move(limit);
    stale_ = true;
}

void MapModel::set_depth_limit(core::Limit limit) {
    depth_limit_ = std::move(limit);
    set_options(options_);
}

int MapModel::max_depth() const {
    return static_cast<int>(std::min<std::size_t>(depth_limit_.cap(), std::numeric_limits<int>::max()));
}

void MapModel::update(const EdgeReader& read, std::span<const core::NoteMeta> notes,
                      const std::optional<core::NotePath>& center) {
    if (edges_stale_) {
        ++reads_;
        if (auto edges = read()) {
            edges_ = std::move(*edges);
        } else {
            edges_.clear();
            core::log_warn("event=map_edges_failed detail=" + core::format_error(edges.error()));
        }
        edges_stale_ = false;
        stale_ = true;
    }
    if (center != center_) {
        center_ = center;
        if (!options_.whole_graph)  // bütün çizgede merkez yalnız vurgudur
            stale_ = true;
    }
    if (!stale_)
        return;
    stale_ = false;
    NoteMap built = build_note_map(notes, edges_, center_, options_, node_limit_.cap());
    // Merkez değişti ama çevre aynı not ve kenar kümesi: yalnız BFS sırası farklıdır. Yeni çizge sayılsaydı
    // yerleşim ılınır, oturmuş düğümler kare kare sıçrar ve kamera yeniden sığardı (seçimde titreme). Eski
    // sıra ve konumlar kalır.
    if (built.total == map_.total && signature(built) == signature(map_))
        return;
    map_ = std::move(built);
    ++version_;
    std::vector<std::string> keys;
    keys.reserve(map_.nodes.size());
    for (const MapNode& node : map_.nodes)  // önek: "#x.md" adlı not, x.md etiketiyle karışmasın
        keys.push_back((node.kind == MapNodeKind::Tag ? "t:" : "n:") + node.key);
    std::vector<GraphLayout::Edge> edges;
    edges.reserve(map_.edges.size());
    for (const MapEdge& edge : map_.edges)
        edges.push_back({edge.from, edge.to});
    if (layout_.set_graph(std::move(keys), std::move(edges)))
        ++generation_;
    matched_.reset();
}

const std::vector<char>&
MapModel::refresh_matches(bool filtering, std::size_t version,
                          const std::function<std::unordered_set<std::string>()>& listed) {
    if (matched_ == std::pair(filtering, version))
        return matches_;
    matched_ = std::pair(filtering, version);
    matches_.assign(map_.nodes.size(), filtering ? 0 : 1);
    if (!filtering)
        return matches_;
    const std::unordered_set<std::string> shown = listed();
    for (std::size_t i = 0; i < map_.nodes.size(); ++i)
        matches_[i] = map_.nodes[i].kind == MapNodeKind::Note && shown.contains(map_.nodes[i].key) ? 1 : 0;
    for (const MapEdge& edge : map_.edges) {  // etiket, eşleşen bir notunda yanar
        if (edge.kind == MapEdgeKind::Tag && matches_[edge.from] != 0)
            matches_[edge.to] = 1;
    }
    return matches_;
}

}  // namespace noteapp::app
