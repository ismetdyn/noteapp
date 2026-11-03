#include "gui/note_map.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <string_view>
#include <unordered_map>

namespace noteapp::gui {

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
                       edge.kind == core::EdgeKind::RelatedTo ? MapEdgeKind::Related : MapEdgeKind::Link});
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
            map.edges.push_back({node_of[edge.from], node_of[edge.to], edge.kind});
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

}  // namespace noteapp::gui
