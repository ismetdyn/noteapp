#include "search/relation_search.hpp"

#include "search/content_index.hpp"
#include "search/fold.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace noteapp::search {

RelationGraph::RelationGraph(std::span<const core::NoteEdge> edges) {
    std::unordered_map<std::string, std::size_t> name_index;
    const auto node = [&](const core::NotePath& path) {
        const auto [found, inserted] = node_of.try_emplace(path.string(), paths.size());
        if (inserted) {
            paths.push_back(found->first);
            notes.push_back(path);
            folded_paths.push_back(fold_to_utf8(found->first));
            edges_of.emplace_back();
        }
        return found->second;
    };
    ends.reserve(edges.size());
    name_of.reserve(edges.size());
    directed.reserve(edges.size());
    for (std::size_t e = 0; e < edges.size(); ++e) {
        const core::NoteEdge& edge = edges[e];
        const std::size_t from = node(edge.from);
        const std::size_t to = node(edge.to);
        ends.emplace_back(from, to);
        edges_of[from].push_back(e);
        edges_of[to].push_back(e);
        directed.push_back(edge.kind == core::EdgeKind::RelatedTo && edge.directed ? 1 : 0);
        if (edge.kind != core::EdgeKind::RelatedTo || edge.name.empty()) {
            name_of.push_back(kNoName);
            continue;
        }
        const auto [named, is_new] = name_index.try_emplace(edge.name, names.size());
        if (is_new) {
            names.push_back(edge.name);
            folded_names.push_back(fold_to_utf8(edge.name));
            edges_named.emplace_back();
        }
        name_of.push_back(named->second);
        edges_named[named->second].push_back(e);
    }
}

namespace {

constexpr std::size_t kUnreached = SIZE_MAX;

using Terms = std::vector<std::string>;  // katlanmış

Terms folded_terms(std::string_view text) {
    Terms terms;
    for (const std::string& raw : split_terms(text))
        terms.push_back(fold_to_utf8(raw));
    return terms;
}

bool contains_all(std::string_view folded, const Terms& terms) {
    return std::ranges::all_of(
        terms, [&](const std::string& term) { return folded.find(term) != std::string::npos; });
}

// graph.index yoksa dizin bu çağrı için kurulur (CLI, testler).
class IndexOf {
public:
    explicit IndexOf(const GraphInput& graph) {
        if (graph.index == nullptr)
            local_.emplace(graph.edges);
        index_ = graph.index != nullptr ? graph.index : &*local_;
    }
    const RelationGraph& operator*() const { return *index_; }

private:
    std::optional<RelationGraph> local_;
    const RelationGraph* index_ = nullptr;
};

bool stronger(const RelationReason& a, const RelationReason& b) {
    return std::pair(a.kind, a.hops) < std::pair(b.kind, b.hops);
}

// Not başına en güçlü neden; yalnız izinli notlar (hedefler). Düğüm -> not eşlemesi sorgu başına kurulur;
// notun düğümü GraphInput::nodes'tan (önbellek) ya da yoldan.
class BestReasons {
public:
    BestReasons(const RelationGraph& graph, std::span<const core::NoteMeta> notes,
                std::span<const std::size_t> allowed, std::span<const std::size_t> nodes)
        : best_(notes.size()), note_of_node_(graph.paths.size(), kUnreached) {
        for (const std::size_t i : allowed) {
            std::size_t node = kNoNode;
            if (!nodes.empty()) {
                node = nodes[i];
            } else if (const auto found = graph.node_of.find(notes[i].path.string());
                       found != graph.node_of.end()) {
                node = found->second;
            }
            if (node != kNoNode) {
                note_of_node_[node] = i;
                ++targets_;
            }
        }
    }
    std::size_t targets() const { return targets_; }
    bool is_target(std::size_t node) const { return note_of_node_[node] != kUnreached; }
    void offer_note(std::size_t index, RelationReason reason) {
        if (!best_[index] || stronger(reason, *best_[index]))
            best_[index] = std::move(reason);
    }
    // Düğüm izinli bir notsa ona; neden yalnız gerekirse kurulur (milyonlarca kenarda kopya olmasın).
    template <typename MakeReason>
    void offer_node(std::size_t node, const RelationReason& probe, MakeReason&& make) {
        const std::size_t index = note_of_node_[node];
        if (index == kUnreached || (best_[index] && !stronger(probe, *best_[index])))
            return;
        best_[index] = make();
    }
    std::vector<std::optional<RelationReason>>& take() { return best_; }

private:
    std::vector<std::optional<RelationReason>> best_;
    std::vector<std::size_t> note_of_node_;
    std::size_t targets_ = 0;  // düğümü olan izinli not
};

core::RelationDirection direction_from(const RelationGraph& graph, std::size_t edge, bool is_from) {
    if (graph.directed[edge] == 0)
        return core::RelationDirection::Undirected;
    return is_from ? core::RelationDirection::Outgoing : core::RelationDirection::Incoming;
}

// Yalnız adı eşleşen ilişkilerin kenarları gezilir.
void offer_relation_names(const RelationGraph& graph, const Terms& terms, BestReasons& best) {
    const RelationReason probe{.kind = RelationReason::Kind::Relation};
    for (std::size_t name = 0; name < graph.names.size(); ++name) {
        if (!contains_all(graph.folded_names[name], terms))
            continue;
        for (const std::size_t e : graph.edges_named[name])
            for (const bool is_from : {true, false}) {
                const std::size_t node = is_from ? graph.ends[e].first : graph.ends[e].second;
                best.offer_node(node, probe, [&] {
                    return RelationReason{.kind = RelationReason::Kind::Relation,
                                          .label = graph.names[name],
                                          .other = graph.paths[graph.other(e, node)],
                                          .direction = direction_from(graph, e, is_from)};
                });
            }
    }
}

// Gezintinin sonucu: düğüm başına adım (kUnreached: ulaşılmadı, 0: adı eşleşti), en yakın eşleşen düğüm ve
// düğüme ulaştıran kenar.
struct Walk {
    std::vector<std::size_t> hops;
    std::vector<std::size_t> seed;
    std::vector<std::size_t> via;
};

// Adı eşleşen düğümlerden aynı anda genişlik öncelikli gezinti: her not en yakın eşleşmeye bağlanır, iş
// düğüm + kenar sayısıyla doğrusal. Hedeflerin (izinli, doğrudan bulunmamış notlar) hepsine ulaşınca durur:
// adların çoğu eşleşince bütün kenarlar gezilmez.
Walk walk_from_matches(const RelationGraph& graph, std::size_t depth, const Terms& terms,
                       const BestReasons& best) {
    const std::size_t count = graph.paths.size();
    Walk walk{std::vector<std::size_t>(count, kUnreached), std::vector<std::size_t>(count),
              std::vector<std::size_t>(count)};
    std::vector<std::size_t> frontier;
    std::size_t waiting = best.targets();  // henüz ulaşılmamış hedef
    for (std::size_t node = 0; node < count; ++node)
        if (contains_all(graph.folded_paths[node], terms)) {
            walk.hops[node] = 0;
            walk.seed[node] = node;
            frontier.push_back(node);
            waiting -= best.is_target(node) ? 1 : 0;  // adı eşleşen hedef ad eşleşmesidir, bağlı olmaz
        }
    for (std::size_t hop = 1; hop <= depth && !frontier.empty() && waiting > 0; ++hop) {
        std::vector<std::size_t> next;
        for (const std::size_t node : frontier)
            for (const std::size_t edge : graph.edges_of[node]) {
                const std::size_t reached = graph.other(edge, node);
                if (walk.hops[reached] != kUnreached)
                    continue;
                walk.hops[reached] = hop;
                walk.seed[reached] = walk.seed[node];
                walk.via[reached] = edge;
                next.push_back(reached);
                waiting -= best.is_target(reached) ? 1 : 0;
            }
        frontier = std::move(next);
    }
    return walk;
}

void offer_linked(const RelationGraph& graph, std::size_t depth, const Terms& terms, BestReasons& best) {
    const Walk walk = walk_from_matches(graph, depth, terms, best);
    for (std::size_t node = 0; node < walk.hops.size(); ++node) {
        const std::size_t hops = walk.hops[node];
        if (hops == kUnreached || hops == 0)
            continue;
        const RelationReason probe{.kind = RelationReason::Kind::Linked, .hops = hops};
        best.offer_node(node, probe, [&] {
            const std::size_t name = graph.name_of[walk.via[node]];
            RelationReason reason = probe;
            // Uzak komşuda son kenarın adı eşleşen notla ilgili değildir.
            if (hops == 1 && name != RelationGraph::kNoName)
                reason.label = graph.names[name];
            reason.other = graph.paths[walk.seed[node]];
            return reason;
        });
    }
}

// [[hedef]] çözümüne benzer: yol, uzantısız yol, dosya adı ya da uzantısız ad (katlanmış).
bool names_note(std::string_view full, std::string_view wanted) {
    const auto slash = full.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? full : full.substr(slash + 1);
    const auto dot = name.rfind('.');
    const std::size_t extension = dot == std::string_view::npos || dot == 0 ? 0 : name.size() - dot;
    return wanted == full || wanted == name || wanted == full.substr(0, full.size() - extension) ||
           wanted == name.substr(0, name.size() - extension);
}

std::set<core::NotePath> graph_filter_notes(const Filter& filter, const RelationGraph& graph) {
    std::set<core::NotePath> notes;
    if (filter.kind == Filter::Kind::Relation) {
        for (std::size_t name = 0; name < graph.names.size(); ++name)
            if (graph.folded_names[name] == filter.text)
                for (const std::size_t e : graph.edges_named[name]) {
                    notes.insert(graph.notes[graph.ends[e].first]);
                    notes.insert(graph.notes[graph.ends[e].second]);
                }
        return notes;
    }
    for (std::size_t node = 0; node < graph.paths.size(); ++node)
        if (names_note(graph.folded_paths[node], filter.text))
            for (const std::size_t e : graph.edges_of[node])
                notes.insert(graph.notes[graph.other(e, node)]);
    return notes;
}

bool is_graph_filter(const Filter& filter) {
    return filter.kind == Filter::Kind::Relation || filter.kind == Filter::Kind::Linked;
}

}  // namespace

std::vector<RelationHit> relation_hits(std::string_view text, std::span<const core::NoteMeta> notes,
                                       std::span<const std::size_t> allowed, const GraphInput& graph) {
    const Terms terms = folded_terms(text);
    if (terms.empty() || !graph.depth)
        return {};
    const IndexOf index(graph);
    BestReasons best(*index, notes, allowed,
                     graph.index != nullptr ? graph.nodes : std::span<const std::size_t>{});
    std::unordered_map<std::string, bool> tag_matches;  // aynı etiket sorgu başına bir kez katlanır
    for (const std::size_t i : allowed)
        for (const std::string& tag : notes[i].tags) {
            const auto [found, inserted] = tag_matches.try_emplace(tag, false);
            if (inserted)
                found->second = contains_all(fold_to_utf8(tag), terms);
            if (found->second) {
                best.offer_note(i, RelationReason{.kind = RelationReason::Kind::Tag, .label = tag});
                break;
            }
        }
    if (best.targets() > 0) {
        offer_relation_names(*index, terms, best);
        offer_linked(*index, *graph.depth, terms, best);
    }

    std::vector<RelationHit> hits;
    std::vector<std::optional<RelationReason>>& reasons = best.take();
    for (const std::size_t i : allowed)
        if (reasons[i])
            hits.push_back({i, std::move(*reasons[i])});
    std::ranges::stable_sort(hits, [&](const RelationHit& a, const RelationHit& b) {
        if (a.reason.kind != b.reason.kind || a.reason.hops != b.reason.hops)
            return stronger(a.reason, b.reason);
        return notes[a.index].updated_at > notes[b.index].updated_at;
    });
    return hits;
}

std::vector<std::size_t> note_nodes(const RelationGraph& index, std::span<const core::NoteMeta> notes) {
    std::vector<std::size_t> nodes(notes.size(), kNoNode);
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (const auto found = index.node_of.find(notes[i].path.string()); found != index.node_of.end())
            nodes[i] = found->second;
    return nodes;
}

void bind_graph_filters(std::span<Filter> filters, const GraphInput& graph) {
    if (!has_graph_filter(filters))
        return;
    const IndexOf index(graph);
    for (Filter& filter : filters)
        if (is_graph_filter(filter))
            filter.notes =
                std::make_shared<const std::set<core::NotePath>>(graph_filter_notes(filter, *index));
}

bool has_graph_filter(std::span<const Filter> filters) {
    return std::ranges::any_of(filters, is_graph_filter);
}

}  // namespace noteapp::search
