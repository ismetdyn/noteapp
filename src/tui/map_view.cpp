#include "tui/map_view.hpp"

#include "app/note_map.hpp"
#include "core/file_icons.hpp"
#include "core/i18n.hpp"
#include "edit/display_text.hpp"
#include "tui/input_view.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/screen/string.hpp>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::tui {

namespace {

constexpr int kFitMarginDots = 4;           // kenardaki düğümün etiketi tuvalden taşmasın
constexpr float kFitMinZoom = 0.0005F;      // 2.000 düğüm en küçük panelde de sığar
constexpr float kFitMaxZoom = 0.3F;         // az düğüm panele yayılmasın: ideal kenar en çok 30 nokta
constexpr float kLabelSpacingDots = 14.0F;  // komşular bu kadar açılınca tüm adlar yazılır
constexpr int kLabelMaxCells = 16;          // ad kısalır; tam yol REVIEW'da
constexpr int kNodeRadiusDots = 1;
constexpr int kCurrentRingDots = 3;  // gösterilen notun halkası

// Ad güvenilmezdir (senkron): denetim baytı ve bidi terminale gitmez, en çok `cells` hücre.
std::string label_text(const app::MapNode& node, int cells) {
    const auto slash = node.key.rfind('/');
    std::string name = node.kind == app::MapNodeKind::Tag
                           ? "#" + node.key
                           : (slash == std::string::npos ? node.key : node.key.substr(slash + 1));
    name = terminal_safe(name);
    if (ftxui::string_width(name) <= cells)
        return name;
    std::string cut;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(name)) {
        if (ftxui::string_width(cut + glyph + "…") > cells)
            break;
        cut += glyph;
    }
    return cut + "…";
}

std::string status_text(const app::MapModel& model, int columns) {
    const app::MapOptions& options = model.options();
    const std::string scope = options.whole_graph
                                  ? core::tr("tui.map_whole")
                                  : core::tr("tui.map_depth", {{"depth", std::to_string(options.depth)}});
    constexpr int kDigitKeys = 9;  // derinlik tek haneli tuşla seçilir
    std::string text = core::tr("tui.map_status", {{"scope", scope},
                                                   {"tags", options.show_tags ? core::tr("tui.map_tags") : ""},
                                                   {"max", std::to_string(std::min(model.max_depth(), kDigitKeys))}});
    const app::NoteMap& map = model.map();
    if (map.capped())
        text = core::tr("tui.map_capped",
                        {{"shown", std::to_string(map.nodes.size())}, {"total", std::to_string(map.total)}}) +
               std::string(kHintSeparator) + text;
    return fit_hints(text, columns);
}

// `C` kipi ve sürükleme (MAP-R): kaynak → hedef ve kipin tuşları. Hedefle arada ilişki varsa adı ve yönü
// (F1): ad güvenilmezdir, temizlenir ve kısalır.
std::string link_text(const TuiState& state, const TuiState::MapLink& link, int columns) {
    const auto name = [](const app::MapNode& node) { return label_text(node, kLabelMaxCells); };
    const std::string target = link.target ? name(*link.target) : "?";
    const std::optional<TuiState::MapLinkEdge> edge = link.choosing ? std::nullopt : state.map_link_edge();
    if (!edge || edge->edge.kind != app::MapEdgeKind::Related)
        return fit_hints(core::tr(link.choosing ? "tui.map_choose" : "tui.map_link",
                                  {{"from", name(link.source)}, {"to", target}}),
                         columns);
    std::string relation = terminal_safe(edge->edge.name);
    if (ftxui::string_width(relation) > kLabelMaxCells)
        relation = head_cells(relation, kLabelMaxCells - 1) + "…";
    return fit_hints(core::tr("tui.map_relation",
                              {{"from", name(edge->from)},
                               {"arrow", edge->edge.directed ? "→" : "—"},
                               {"to", name(edge->to)},
                               {"name", relation.empty() ? core::tr("tui.relation_unnamed") : "'" + relation + "'"}}),
                     columns);
}

// Liang–Barsky: kenar tuvale kırpılır; yakınlaştırınca binlerce noktalık çizgi tuval dışında yürünmez.
bool clip_line(float& x1, float& y1, float& x2, float& y2, float width, float height) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    float enter = 0.0F;
    float leave = 1.0F;
    const auto edge = [&](float p, float q) {
        if (p == 0.0F)
            return q >= 0.0F;
        const float t = q / p;
        if (p < 0.0F)
            enter = std::max(enter, t);
        else
            leave = std::min(leave, t);
        return enter <= leave;
    };
    if (!edge(-dx, x1) || !edge(dx, width - 1.0F - x1) || !edge(-dy, y1) || !edge(dy, height - 1.0F - y1))
        return false;
    x2 = x1 + leave * dx;
    y2 = y1 + leave * dy;
    x1 += enter * dx;
    y1 += enter * dy;
    return true;
}

struct Labels {
    int columns;
    int rows;
    std::vector<char> taken = std::vector<char>(static_cast<std::size_t>(columns * rows), 0);

    // Başka adın üstüne binen ad yazılmaz; tuval kenarında içeri kaydırılır.
    bool place(int& x, int y, int width) {
        if (y < 0 || y >= rows || width > columns)
            return false;
        x = std::clamp(x, 0, columns - width);
        const auto row = static_cast<std::size_t>(y * columns);
        for (int cell = x; cell < x + width; ++cell)
            if (taken[row + static_cast<std::size_t>(cell)] != 0)
                return false;
        for (int cell = x; cell < x + width; ++cell)
            taken[row + static_cast<std::size_t>(cell)] = 1;
        return true;
    }
};

}  // namespace

MapCamera fit_map_camera(std::span<const app::LayoutPoint> positions, int dots_x, int dots_y) {
    if (positions.empty())
        return {};
    app::LayoutPoint low = positions.front();
    app::LayoutPoint high = positions.front();
    for (const app::LayoutPoint p : positions) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
    }
    const float room_x = std::max(static_cast<float>(dots_x - 2 * kFitMarginDots), 1.0F);
    const float room_y = std::max(static_cast<float>(dots_y - 2 * kFitMarginDots), 1.0F);
    const float span_x = std::max(high.x - low.x, app::GraphLayout::kIdealEdge);
    const float span_y = std::max(high.y - low.y, app::GraphLayout::kIdealEdge);
    MapCamera camera;
    camera.zoom = std::clamp(std::min(room_x / span_x, room_y / span_y), kFitMinZoom, kFitMaxZoom);
    camera.x = static_cast<float>(dots_x) / 2.0F - (low.x + high.x) / 2.0F * camera.zoom;
    camera.y = static_cast<float>(dots_y) / 2.0F - (low.y + high.y) / 2.0F * camera.zoom;
    return camera;
}

// Harita (SPEC-tui "Harita"): kapsam satırı ve braille tuvali. Kenar çizgi, düğüm nokta, gösterilen not sarı
// halkalı ve sarı adlı, Tab'la seçilen not köşeli parantezde; aramada eşleşmeyen söner. Komşular yeterince
// açıkken tüm adlar, değilse yalnız gösterilen ve seçilen not.
ftxui::Element map_pane(const TuiState& state, int columns, int rows) {
    using namespace ftxui;
    const app::MapModel& model = state.map_model();
    const app::NoteMap& map = model.map();
    const std::optional<TuiState::MapLink>& link = state.map_link();
    Element status =
        link ? text(link_text(state, *link, columns)) | color(Color::Cyan) : text(status_text(model, columns)) | dim;
    if (map.nodes.empty())
        return vbox({std::move(status), paragraph(core::tr("tui.map_empty")) | dim, filler()});
    const int canvas_rows = std::max(rows - 1, 1);
    Canvas canvas(columns * kMapDotsPerCellX, canvas_rows * kMapDotsPerCellY);
    const MapCamera camera = state.map_camera();
    const std::vector<app::LayoutPoint>& positions = model.layout().positions();
    const std::vector<char>& matches = model.matches();
    const auto lit = [&](std::size_t node) { return node >= matches.size() || matches[node] != 0; };
    const auto dot = [&](std::size_t node) {
        return std::pair(static_cast<int>(std::lround(camera.x + positions[node].x * camera.zoom)),
                         static_cast<int>(std::lround(camera.y + positions[node].y * camera.zoom)));
    };

    const auto line = [&](std::pair<int, int> from, std::pair<int, int> to, Color color) {
        auto x1 = static_cast<float>(from.first);
        auto y1 = static_cast<float>(from.second);
        auto x2 = static_cast<float>(to.first);
        auto y2 = static_cast<float>(to.second);
        if (clip_line(x1, y1, x2, y2, static_cast<float>(canvas.width()),
                      static_cast<float>(canvas.height())))
            canvas.DrawPointLine(static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)),
                                 static_cast<int>(std::lround(x2)), static_cast<int>(std::lround(y2)), color);
    };
    for (const app::MapEdge& edge : map.edges) {
        Color color = edge.kind == app::MapEdgeKind::Related ? Color::Magenta : Color::GrayLight;
        if (edge.kind == app::MapEdgeKind::Tag || !lit(edge.from) || !lit(edge.to))
            color = Color::GrayDark;
        line(dot(edge.from), dot(edge.to), color);
    }
    // İlişki kurulurken kaynaktan hedefe (sürüklerken işaretçiye) çizgi; hedef köşeli parantezde.
    const auto node_of = [&](const app::MapNode& wanted) -> std::optional<std::size_t> {
        const auto found = std::ranges::find_if(map.nodes, [&](const app::MapNode& node) {
            return node.kind == wanted.kind && node.key == wanted.key;
        });
        return found == map.nodes.end() ? std::nullopt
                                        : std::optional(static_cast<std::size_t>(found - map.nodes.begin()));
    };
    const std::optional<std::size_t> link_source = link ? node_of(link->source) : std::nullopt;
    const std::optional<std::size_t> link_target =
        link && link->target ? node_of(*link->target) : std::nullopt;
    if (link_source && link->drag)
        line(dot(*link_source), *link->drag, Color::Cyan);
    else if (link_source && link_target)
        line(dot(*link_source), dot(*link_target), Color::Cyan);

    const std::string current = model.center() ? model.center()->string() : std::string{};
    const std::string& selected = state.map_selected_key();
    std::optional<std::size_t> current_node;
    std::optional<std::size_t> selected_node;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        const app::MapNode& node = map.nodes[i];
        const bool tag = node.kind == app::MapNodeKind::Tag;
        const Color color = !lit(i) ? Color::GrayDark
                            : tag   ? Color::Default
                                    : icon_color(core::file_icon(node.key).color);
        const auto [x, y] = dot(i);
        if (tag)
            canvas.DrawPoint(x, y, true, color);
        else
            canvas.DrawPointCircleFilled(x, y, kNodeRadiusDots, color);
        if (!tag && node.key == current) {
            canvas.DrawPointCircle(x, y, kCurrentRingDots, Color::Yellow);
            current_node = i;
        }
        if (!tag && node.key == selected)
            selected_node = i;
    }

    // Önce gösterilen ve seçilen not: sıkışık haritada da adları yazılır.
    std::vector<std::size_t> order;
    for (const auto node : {link_source, link_target, current_node, selected_node})
        if (node && std::ranges::find(order, *node) == order.end())
            order.push_back(*node);
    if (app::GraphLayout::kIdealEdge * camera.zoom >= kLabelSpacingDots)
        for (std::size_t i = 0; i < map.nodes.size(); ++i)
            if (std::ranges::find(order, i) == order.end())
                order.push_back(i);
    Labels labels{columns, canvas_rows};
    for (const std::size_t i : order) {
        // Kip açıkken köşeli parantez hedefindir: Tab seçimi değil.
        const bool is_selected = link ? link_target == i : selected_node == i;
        std::string label = label_text(map.nodes[i], kLabelMaxCells);
        if (is_selected)
            label = "[" + label + "]";
        const auto [x, y] = dot(i);
        int cell_x = x / kMapDotsPerCellX + 1;  // düğümün sağında
        const int cell_y = y / kMapDotsPerCellY;
        if (x < 0 || y < 0 || !labels.place(cell_x, cell_y, ftxui::string_width(label)))
            continue;
        const bool is_current = current_node == i;
        const bool is_source = link_source == i;
        const bool is_lit = lit(i);
        canvas.DrawText(cell_x * kMapDotsPerCellX, cell_y * kMapDotsPerCellY, label,
                        [is_current, is_selected, is_source, is_lit](Cell& cell) {
                            if (is_source) {
                                cell.foreground_color = Color::Cyan;
                                cell.bold = true;
                            } else if (is_current) {
                                cell.foreground_color = Color::Yellow;
                                cell.bold = true;
                            } else if (!is_lit) {
                                cell.foreground_color = Color::GrayDark;
                            }
                            cell.inverted = is_selected;
                        });
    }
    return vbox({std::move(status), ftxui::canvas(std::move(canvas)) | flex});
}

}  // namespace noteapp::tui
