#include "app/document.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace noteapp::gui {

void GuiState::set_map_options(const MapOptions& options) {
    MapOptions clamped = options;
    clamped.depth = std::clamp(options.depth, MapOptions::kMinDepth, MapOptions::kMaxDepth);
    if (clamped == map_options_)
        return;
    map_options_ = clamped;
    map_stale_ = true;
}

const NoteMap& GuiState::note_map() {
    ensure_map();
    return map_;
}

GraphLayout& GuiState::map_layout() {
    ensure_map();
    return map_layout_;
}

// Kenarlar yalnız taramada değişiklik görülünce (listeleme ya da başka bağlantının yazması) tek okumayla
// gelir; görünür harita seçenek ya da çevre merkezi değişince bellekten kurulur.
void GuiState::ensure_map() {
    if (map_edges_stale_) {
        ++map_reads_;
        if (auto edges = store_.edges()) {
            map_edges_ = std::move(*edges);
        } else {
            map_edges_.clear();
            core::log_warn("event=map_edges_failed detail=" + core::format_error(edges.error()));
        }
        map_edges_stale_ = false;
        map_stale_ = true;
    }
    auto center = shown_ && !new_note_ ? std::optional(shown_->path) : std::nullopt;
    if (center != map_center_) {
        map_center_ = std::move(center);
        if (!map_options_.whole_graph)  // bütün çizgede merkez yalnız vurgudur
            map_stale_ = true;
    }
    if (!map_stale_)
        return;
    map_stale_ = false;
    map_ = build_note_map(model_.listing().metas(), map_edges_, map_center_, map_options_);
    std::vector<std::string> keys;
    keys.reserve(map_.nodes.size());
    for (const MapNode& node : map_.nodes)  // önek: "#x.md" adlı not, x.md etiketiyle karışmasın
        keys.push_back((node.kind == MapNodeKind::Tag ? "t:" : "n:") + node.key);
    std::vector<GraphLayout::Edge> edges;
    edges.reserve(map_.edges.size());
    for (const MapEdge& edge : map_.edges)
        edges.push_back({edge.from, edge.to});
    if (map_layout_.set_graph(std::move(keys), std::move(edges)))
        ++map_generation_;
    map_matches_stale_ = true;
}

// Süzgeç listeyle aynıdır (model_): eşleşme yalnız süzgeç ya da harita değişince kurulur, kare başına değil.
const std::vector<char>& GuiState::map_matches() {
    ensure_map();
    if (!map_matches_stale_ && map_matches_resets_ == model_.resets())
        return map_matches_;
    map_matches_stale_ = false;
    map_matches_resets_ = model_.resets();
    map_matches_.assign(map_.nodes.size(), query_.empty() ? 1 : 0);
    if (query_.empty())
        return map_matches_;
    std::unordered_set<std::string> listed;
    listed.reserve(static_cast<std::size_t>(model_.row_count()));
    for (int row = 0; row < model_.row_count(); ++row)
        listed.insert(model_.note_at(row).path.string());
    for (std::size_t i = 0; i < map_.nodes.size(); ++i)
        map_matches_[i] =
            map_.nodes[i].kind == MapNodeKind::Note && listed.contains(map_.nodes[i].key) ? 1 : 0;
    for (const MapEdge& edge : map_.edges) {  // etiket, eşleşen bir notunda yanar
        if (edge.kind == MapEdgeKind::Tag && map_matches_[edge.from] != 0)
            map_matches_[edge.to] = 1;
    }
    return map_matches_;
}

void GuiState::open_map_note(const core::NotePath& path) {
    if (flush_shown())
        open_link(path);
}

}  // namespace noteapp::gui
