#include "app/note_map.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "search/fold.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace noteapp::app {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

using NoteIndex = std::unordered_map<std::string, std::size_t>;

NoteIndex index_notes(std::span<const core::NoteMeta> notes) {
    NoteIndex index;
    index.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i)
        index.emplace(notes[i].path.string(), i);
    return index;
}

std::optional<std::size_t> note_at(const NoteIndex& index, const std::string& key) {
    const auto found = index.find(key);
    return found == index.end() ? std::nullopt : std::optional(found->second);
}

// Not indisleri üzerinden kenar; ucu listede olmayan (eski kayıt) kenar düşer.
std::vector<MapEdge> indexed_edges(const NoteIndex& index, std::span<const core::NoteEdge> edges) {
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

// İlişki adları ilişki kuralıyla eşlenir (core::tag_key: Türkçe İ/I, büyük/küçük harf).
bool same_name(const std::string& name, const std::string& wanted) {
    return core::tag_key(name) == core::tag_key(wanted);
}

bool shown_edge(const MapEdge& edge, const MapOptions& options) {
    if (edge.kind == MapEdgeKind::Link)
        return options.show_links;
    return options.show_related && (!options.relation || same_name(edge.name, *options.relation));
}

bool follows(const MapEdge& edge, const MapExpansion& spec) {
    if (spec.kind && edge.kind != *spec.kind)
        return false;
    return !spec.name || (edge.kind == MapEdgeKind::Related && same_name(edge.name, *spec.name));
}

bool same_expansion(const MapExpansion& a, const MapExpansion& b) {
    return a.kind == b.kind && a.name.has_value() == b.name.has_value() && (!a.name || same_name(*a.name, *b.name));
}

// Etiket süzgeci aramanın `tag:` kuralıdır (search::parse_query + apply): iki yer ayrı kural taşımasın. Boşluklu
// metin tek etiket değildir (sorguda iki sözcük olurdu): hiçbir not geçmez.
std::vector<char> tagged_notes(std::span<const core::NoteMeta> notes, const std::string& tag) {
    std::vector<char> kept(notes.size(), 0);
    if (tag.find_first_of(" \t\r\n") != std::string::npos)
        return kept;
    const auto parsed = search::parse_query("tag:" + tag);
    if (!parsed)
        return kept;
    std::vector<search::Filter> filters;
    std::ranges::copy_if(parsed->filters, std::back_inserter(filters),
                         [](const search::Filter& filter) { return filter.kind == search::Filter::Kind::Tag; });
    if (filters.empty())
        return kept;
    for (const std::size_t note : search::apply(filters, std::nullopt, notes))
        kept[note] = 1;
    return kept;
}

// Süzgeçlerden geçen notlar ve aralarındaki kenarlar (not indisleriyle): kapsam ve yol yalnız bunlarda gezer.
struct FilteredGraph {
    std::vector<char> kept;
    std::vector<MapEdge> edges;
};

FilteredGraph filtered_graph(std::span<const core::NoteMeta> notes, std::span<const core::NoteEdge> edges,
                             const NoteIndex& index, std::optional<std::size_t> center, const MapOptions& options) {
    FilteredGraph graph;
    graph.kept = options.tag.empty() ? std::vector<char>(notes.size(), 1) : tagged_notes(notes, options.tag);
    if (center)
        graph.kept[*center] = 1;  // kapsam merkezden başlar
    for (MapEdge& edge : indexed_edges(index, edges))
        if (graph.kept[edge.from] != 0 && graph.kept[edge.to] != 0 && shown_edge(edge, options))
            graph.edges.push_back(std::move(edge));
    return graph;
}

// Düğüm başına (komşu, kenar dizini), iki yön.
using Adjacency = std::vector<std::vector<std::pair<std::size_t, std::size_t>>>;

Adjacency adjacency(std::size_t count, const std::vector<MapEdge>& edges) {
    Adjacency adjacent(count);
    for (std::size_t e = 0; e < edges.size(); ++e) {
        adjacent[edges[e].from].emplace_back(edges[e].to, e);
        adjacent[edges[e].to].emplace_back(edges[e].from, e);
    }
    return adjacent;
}

// `start`tan yönsüz genişlik öncelikli arama, en çok `depth` adım, `accepts`in kabul ettiği kenarlarla: yakın
// önce, başlangıç ilk; `reached` düğümü ve uzaklığını alır. `stop`taki düğüme ulaşılır ama içinden geçilmez
// (kapatılmış komşular).
template <typename Accepts, typename Reached>
void walk(const Adjacency& adjacent, const std::vector<MapEdge>& edges, std::size_t start, int depth,
          const std::vector<char>& stop, const Accepts& accepts, const Reached& reached) {
    std::vector<int> distance(adjacent.size(), -1);
    distance[start] = 0;
    std::vector<std::size_t> queue{start};
    for (std::size_t at = 0; at < queue.size(); ++at) {
        const std::size_t node = queue[at];
        reached(node, distance[node]);
        if (distance[node] >= depth || stop[node] != 0)
            continue;
        for (const auto& [next, edge] : adjacent[node]) {
            if (distance[next] < 0 && accepts(edges[edge])) {
                distance[next] = distance[node] + 1;
                queue.push_back(next);
            }
        }
    }
}

// Yolun ardışık iki düğümünü bağlayan kenarlar (etiket kenarı hariç) yanar. Yönlü yolda bağlantı ve yönlü ilişki
// yalnız yolun yönündeyse yanar: ters yöndeki paralel kenar yolun adımı değildir.
void mark_path_edges(NoteMap& map, bool directed) {
    map.path_edges.assign(map.edges.size(), 0);
    std::set<std::pair<std::size_t, std::size_t>> steps;  // (önceki, sonraki)
    for (std::size_t i = 0; i + 1 < map.path.size(); ++i)
        steps.emplace(map.path[i], map.path[i + 1]);
    for (std::size_t e = 0; e < map.edges.size(); ++e) {
        const MapEdge& edge = map.edges[e];
        if (edge.kind == MapEdgeKind::Tag)
            continue;
        const bool one_way = directed && (edge.kind == MapEdgeKind::Link || edge.directed);
        if (steps.contains({edge.from, edge.to}) || (!one_way && steps.contains({edge.to, edge.from})))
            map.path_edges[e] = 1;
    }
}

std::optional<std::size_t> map_note(const NoteMap& map, std::string_view key) {
    const auto found = std::ranges::find_if(
        map.nodes, [&](const MapNode& node) { return node.kind == MapNodeKind::Note && node.key == key; });
    return found == map.nodes.end() ? std::nullopt
                                    : std::optional(static_cast<std::size_t>(found - map.nodes.begin()));
}

// keep(pin_at): düğüm haritaya girdiyse bekleyen konumuna sabitlenir.
void apply_pins(const NoteMap& map, GraphLayout& layout, std::map<std::string, LayoutPoint>& pending) {
    for (auto it = pending.begin(); it != pending.end();) {
        if (const auto node = map_note(map, it->first); node && *node < layout.size()) {
            layout.pin(*node, it->second);
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
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
                       std::size_t cap, const MapScope& scope) {
    const NoteIndex index = index_notes(notes);
    const std::optional<std::size_t> start = center ? note_at(index, center->string()) : std::nullopt;
    const FilteredGraph graph = filtered_graph(notes, edges, index, start, options);

    std::vector<std::size_t> order;
    std::vector<char> listed(notes.size(), 0);
    const auto add = [&](std::size_t note) {
        if (graph.kept[note] != 0 && listed[note] == 0) {
            listed[note] = 1;
            order.push_back(note);
        }
    };
    if (start)
        add(*start);
    // Yol kapsamdan bağımsızdır: düğümleri derinliğin ötesinde de, sınır kesmesin diye öne girer.
    std::vector<std::size_t> path;
    if (scope.path) {
        const auto from = note_at(index, scope.path->from);
        const auto to = note_at(index, scope.path->to);
        if (from && to)
            path = shortest_path(notes.size(), graph.edges, *from, *to, scope.path->directed)
                       .value_or(std::vector<std::size_t>{});
    }
    std::ranges::for_each(path, add);
    for (const std::string& key : scope.kept)
        if (const auto note = note_at(index, key))
            add(*note);
    if (options.whole_graph || !start) {
        for (std::size_t note = 0; note < notes.size(); ++note)
            add(note);
    } else {
        std::vector<char> collapsed(notes.size(), 0);
        for (const std::string& key : scope.collapsed)
            if (const auto note = note_at(index, key))
                collapsed[*note] = 1;
        std::vector<const MapExpansion*> expansion(notes.size(), nullptr);
        for (const auto& [key, spec] : scope.expanded)
            if (const auto note = note_at(index, key))
                expansion[*note] = &spec;
        const Adjacency adjacent = adjacency(notes.size(), graph.edges);
        // reach: düğümden şu an görünen erim (kalan adım). Açılış o erimin `depth` adım ötesine gider: merkezde
        // derinlik + n, çevrenin sınırında + n. Yalnız açılışın eklediği, açılış kalkınca gider.
        std::vector<int> reach(notes.size(), 0);
        const auto reached_within = [&](int total) {
            return [&, total](std::size_t note, int distance) {
                reach[note] = std::max(reach[note], total - distance);
                add(note);
            };
        };
        walk(adjacent, graph.edges, *start, options.depth, collapsed, [](const MapEdge&) { return true; },
             reached_within(options.depth));
        // Açılan not kapsamdaysa komşuları eklenir; eklenen açılmış not da sırası gelince açılır.
        for (std::size_t at = 0; at < order.size(); ++at) {
            const std::size_t note = order[at];
            if (const MapExpansion* spec = expansion[note]) {
                const int total = static_cast<int>(std::min<long long>(
                    static_cast<long long>(reach[note]) + spec->depth, std::numeric_limits<int>::max()));
                walk(adjacent, graph.edges, note, total, collapsed,
                     [spec](const MapEdge& edge) { return follows(edge, *spec); }, reached_within(total));
            }
        }
    }

    NoteMap map;
    map.total = order.size();
    std::vector<std::size_t> node_of(notes.size(), kNone);
    const std::size_t shown = std::min(order.size(), cap);
    for (std::size_t i = 0; i < shown; ++i) {
        node_of[order[i]] = map.nodes.size();
        map.nodes.push_back({MapNodeKind::Note, notes[order[i]].path.string()});
    }
    for (const MapEdge& edge : graph.edges) {
        if (node_of[edge.from] != kNone && node_of[edge.to] != kNone)
            map.edges.push_back(
                {node_of[edge.from], node_of[edge.to], edge.kind, edge.name, edge.directed, edge.relation});
    }
    if (std::ranges::all_of(path, [&](std::size_t note) { return node_of[note] != kNone; }))
        std::ranges::transform(path, std::back_inserter(map.path), [&](std::size_t note) { return node_of[note]; });

    if (options.show_tags) {
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
    }
    mark_path_edges(map, scope.path && scope.path->directed);
    return map;
}

std::optional<std::vector<std::size_t>> shortest_path(std::size_t count, std::span<const MapEdge> edges,
                                                      std::size_t from, std::size_t to, bool directed) {
    if (from >= count || to >= count)
        return std::nullopt;
    std::vector<std::vector<std::size_t>> next(count);
    for (const MapEdge& edge : edges) {
        if (edge.kind == MapEdgeKind::Tag || edge.from >= count || edge.to >= count)
            continue;
        next[edge.from].push_back(edge.to);
        if (!directed || (edge.kind == MapEdgeKind::Related && !edge.directed))
            next[edge.to].push_back(edge.from);
    }
    std::vector<std::size_t> parent(count, kNone);
    parent[from] = from;
    std::vector<std::size_t> queue{from};
    for (std::size_t at = 0; at < queue.size() && parent[to] == kNone; ++at) {
        for (const std::size_t node : next[queue[at]]) {
            if (parent[node] == kNone) {
                parent[node] = queue[at];
                queue.push_back(node);
            }
        }
    }
    if (parent[to] == kNone)
        return std::nullopt;
    std::vector<std::size_t> path{to};
    while (path.back() != from)
        path.push_back(parent[path.back()]);
    std::ranges::reverse(path);
    return path;
}

std::string map_path_text(const MapPathResult& result, const std::string& from, const std::string& to,
                          bool directed) {
    const std::string hops = std::to_string(result.hops);
    switch (result.status) {
    case MapPathResult::Status::Found:
        return core::tr("map.path_found", {{"from", from}, {"to", to}, {"hops", hops}});
    case MapPathResult::Status::Hidden:
        return core::tr("map.path_hidden", {{"from", from}, {"to", to}, {"hops", hops}});
    case MapPathResult::Status::None:
        break;
    }
    return core::tr(directed ? "map.path_none_directed" : "map.path_none", {{"from", from}, {"to", to}});
}

MapExpansion expansion_for(const MapOptions& options) {
    if (options.relation)
        return {.kind = MapEdgeKind::Related, .name = options.relation};
    if (options.show_links != options.show_related)
        return {.kind = options.show_links ? MapEdgeKind::Link : MapEdgeKind::Related};
    return {};
}

std::vector<std::size_t> find_map_nodes(const NoteMap& map, std::string_view typed) {
    constexpr std::string_view kBlank = " \t";
    const std::size_t first = typed.find_first_not_of(kBlank);
    if (first == std::string_view::npos)
        return {};
    const std::string wanted = search::fold_to_utf8(typed.substr(first, typed.find_last_not_of(kBlank) - first + 1));
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        const MapNode& node = map.nodes[i];
        const std::string label = node.kind == MapNodeKind::Tag ? "#" + node.key : node.key;
        if (search::fold_to_utf8(label).find(wanted) != std::string::npos)
            found.push_back(i);
    }
    return found;
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
    forget_missing(notes);
    NoteMap built = build_note_map(notes, edges_, center_, options_, node_limit_.cap(), scope_);
    // Merkez değişti ama çevre aynı not ve kenar kümesi: yalnız BFS sırası farklıdır. Yeni çizge sayılsaydı
    // yerleşim ılınır, oturmuş düğümler kare kare sıçrar ve kamera yeniden sığardı (seçimde titreme). Eski
    // sıra ve konumlar kalır; yalnız yol (F2) eski sıranın dizinlerine çevrilir.
    if (built.total == map_.total && signature(built) == signature(map_)) {
        std::vector<std::size_t> path;
        for (const std::size_t node : built.path)
            if (const auto kept = map_note(map_, built.nodes[node].key))
                path.push_back(*kept);
        const std::vector<char> marked = std::move(map_.path_edges);
        const bool moved = path != map_.path;
        map_.path = std::move(path);
        mark_path_edges(map_, scope_.path && scope_.path->directed);
        if (moved || map_.path_edges != marked)  // çizim önbelleği yeni yolu görsün
            ++version_;
        apply_pins(map_, layout_, pending_pins_);
        return;
    }
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
    apply_pins(map_, layout_, pending_pins_);
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


void MapModel::expand(const std::string& note, const MapExpansion& spec) {
    MapExpansion next = spec;
    next.depth = std::clamp(spec.depth, MapOptions::kMinDepth, max_depth());
    if (const auto found = scope_.expanded.find(note);
        found != scope_.expanded.end() && same_expansion(found->second, spec))
        next.depth = std::min(found->second.depth, max_depth() - 1) + 1;
    scope_.expanded.insert_or_assign(note, std::move(next));
    scope_.collapsed.erase(note);
    stale_ = true;
}

// Açılmış not kapanınca yalnız açılışın eklediği gider (harita açılıştan önceki hâline döner); açılmamış notun
// komşuları gizlenir.
void MapModel::collapse(const std::string& note) {
    if (scope_.expanded.erase(note) == 0)
        scope_.collapsed.insert(note);
    stale_ = true;
}

// Açık olma görünürlükten değil kayıttan okunur: GUI'de çift tıklamanın ilk tıklaması notu açar ve merkez yapar,
// merkezin komşuları zaten görünür; görünürlüğe bakan geçiş onları hemen kapatırdı.
bool MapModel::toggle_neighbours(const std::string& note, const MapExpansion& spec) {
    if (scope_.expanded.contains(note)) {
        collapse(note);
        return false;
    }
    expand(note, spec);
    return true;
}

void MapModel::keep(const std::string& note, std::optional<LayoutPoint> pin_at) {
    scope_.kept.insert(note);
    if (pin_at)
        pending_pins_.insert_or_assign(note, *pin_at);
    stale_ = true;
}

MapPathResult MapModel::find_path(std::span<const core::NoteMeta> notes, const std::string& from, const std::string& to,
                                 bool directed) {
    const NoteIndex index = index_notes(notes);
    const auto start = note_at(index, from);
    const auto end = note_at(index, to);
    const std::optional<std::size_t> center = center_ ? note_at(index, center_->string()) : std::nullopt;
    const FilteredGraph graph = filtered_graph(notes, edges_, index, center, options_);
    const auto found = start && end && graph.kept[*start] != 0 && graph.kept[*end] != 0
                           ? shortest_path(notes.size(), graph.edges, *start, *end, directed)
                           : std::nullopt;
    if (!found) {
        clear_path();
        return {};
    }
    const std::size_t hops = found->size() - 1;
    // Harita merkezle yolun düğümlerini önce alır: ikisi sınıra sığmazsa yol çizilemez, "bulundu" denmez.
    const bool center_off_path = center && std::ranges::find(*found, *center) == found->end();
    if (found->size() + (center_off_path ? 1 : 0) > node_limit_.cap()) {
        clear_path();
        return {MapPathResult::Status::Hidden, hops};
    }
    scope_.path = MapScope::Path{from, to, directed};
    stale_ = true;
    return {MapPathResult::Status::Found, hops};
}

// Yeniden adlandırılan ya da çöpe giden notun anahtarları kalmasın: yeni adlı ya da geri gelen not eski açılışı,
// kapanışı ya da bekleyen iğneyi devralırdı.
void MapModel::forget_missing(std::span<const core::NoteMeta> notes) {
    const NoteIndex index = index_notes(notes);
    const auto missing = [&](const std::string& key) { return !index.contains(key); };
    std::erase_if(scope_.expanded, [&](const auto& entry) { return missing(entry.first); });
    std::erase_if(scope_.collapsed, missing);
    std::erase_if(scope_.kept, missing);
    std::erase_if(pending_pins_, [&](const auto& entry) { return missing(entry.first); });
    if (scope_.path && (missing(scope_.path->from) || missing(scope_.path->to)))
        scope_.path.reset();
}

void MapModel::clear_path() {
    if (!scope_.path)
        return;
    scope_.path.reset();
    stale_ = true;
}

}  // namespace noteapp::app
