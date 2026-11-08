#include "app/map_relations.hpp"
#include "app/tabs.hpp"
#include "core/i18n.hpp"
#include "edit/display_text.hpp"
#include "gui/view_internal.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>  // IMGUI_TABLE_MAX_COLUMNS
#include <imgui_stdlib.h>
#include <limits>
#include <utility>

namespace noteapp::gui {

namespace {

// Harita (SPEC-gui "Harita"). Ölçüler yazı boyu (em) ya da yerleşim birimidir (GraphLayout::kIdealEdge).
constexpr float kMapMinZoom = 0.01F;  // 2.000 düğümlük çizge en küçük pencerede de sığar
constexpr float kMapMaxZoom = 4.0F;
constexpr float kMapWheelZoomStep = 1.2F;  // tekerlek çentiği başına
constexpr float kMapKeyZoomStep = 1.25F;   // '+' / '-' başına
constexpr float kMapKeyPanEms = 3.0F;      // ok tuşu başına
constexpr float kMapFitMarginEms = 2.0F;   // sığdırmada pay: kenardaki düğümün etiketi tuvalden taşmasın
constexpr float kMapNodeWorldRadius = 12.0F;
constexpr float kMapNodeMinRadiusEms = 0.25F;  // uzaktan bakınca da düğüm görünsün
constexpr float kMapTagRadiusShare = 0.7F;     // etiket nottan küçük kare
constexpr float kMapHitMinRadiusEms = 0.5F;    // küçük düğüm de tıklanabilsin
constexpr float kMapEdgeHitEms = 0.4F;         // kenara sağ tık: çizgiye bu kadar yakın
constexpr float kMapLabelSpacingEms = 3.5F;    // komşular bu kadar açılınca tüm etiketler görünür
constexpr float kMapArrowSpacingEms = 2.5F;    // ok ucu bundan sıkışık çizgede gürültüdür
// Etiket komşu aralığının bu katında kısalır (yan yana adlar az biner), en çok 12 em; tam yol ipucunda.
constexpr float kMapLabelSpacingShare = 1.6F;
constexpr float kMapLabelMinEms = 3.0F;
constexpr float kMapLabelMaxEms = 12.0F;
constexpr float kMapArrowLengthEms = 0.6F;
constexpr float kMapArrowHalfWidthEms = 0.3F;
constexpr float kMapRingGapEms = 0.2F;  // geçerli notun halkası ve etiket boşluğu
constexpr float kMapRingThickness = 2.0F;
// Kenar metin renginin bu kadarı (G13): koyuda daha soluk, açıkta zeminle karışmasın diye biraz koyu.
constexpr float kMapEdgeAlphaDark = 0.18F;
constexpr float kMapEdgeAlphaLight = 0.25F;
constexpr float kMapRelatedAlpha = 0.8F;  // ilişki renkli ve bağlantıdan belirgin; yönlüsü oklu
constexpr float kMapRelationNameMaxEms = 10.0F;  // kenar etiketi ve menüdeki ilişki adı bundan uzunsa kısalır
constexpr float kMapDimAlpha = 0.2F;      // aramada eşleşmeyen ya da üzerine gelinen düğüme komşu olmayan
constexpr float kMapLabelBackgroundAlpha = 0.8F;
// Düğüm bağlantı sayısıyla büyür (Obsidian kalıbı): r = taban × (1 + 0,25 √derece), en çok 2 × taban.
constexpr float kMapDegreeGrowth = 0.25F;
constexpr float kMapMaxRadiusShare = 2.0F;
constexpr float kMapFilterEms = 8.0F;  // ilişki adı seçicisi ve etiket kutusu (F2)
constexpr float kMapFoundRingEms = 0.45F;  // haritada bul: gösterilen eşleşmenin halkası (geçerli notunkinden geniş)

ImU32 with_alpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

void fit_camera(MapCamera& camera, const std::vector<LayoutPoint>& positions, ImVec2 size) {
    if (positions.empty())
        return;
    // Düğüm derecesiyle iki katına dek büyür (G13): kenardaki büyük düğüm de tuvale sığsın, sınırlar o kadar
    // açılır.
    constexpr float kLargestNode = kMapNodeWorldRadius * kMapMaxRadiusShare;
    static_cast<app::MapCamera&>(camera) =
        app::MapCamera::fit(positions, size.x, size.y, kMapFitMarginEms * ImGui::GetFontSize(), kLargestNode,
                            {kMapMinZoom, kMapMaxZoom});
}

// Kenarlar SDL çizgisiyle çizilir, ImGui üçgeniyle değil. ImGui'nin her dolgusu doku örnekler: SDL'nin
// yazılımsal doldurucusu üçgenin sınır kutusundaki her pikseli sınar ve içerideki her pikselde doku
// koordinatını 64 bitlik bölmeyle hesaplar (SDL_triangle.c). L boyunda çapraz kenar ~L²/2 sınama isterdi;
// yerleşim sürerken ekranı kesen 5.000 kenar kareyi 40 ms'ye çıkarıyordu. SDL çizgisi dokusuz ve boyuyla
// orantılıdır (Bresenham). ImGui_ImplSDLRenderer3_RenderDrawData bu komutu sırasında çağırır: kırpma komutun
// dikdörtgenidir, sonraki ImGui komutları kendi kırpmasını yeniden kurar. View::render SDL ölçeğini çerçeve
// ölçeğine kurduğundan koordinatlar ImGui birimindedir.
void draw_map_lines(const ImDrawList* /*list*/, const ImDrawCmd* command) {
    const auto* batches = static_cast<const std::vector<MapLineBatch>*>(command->UserCallbackData);
    SDL_Renderer* renderer = ImGui_ImplSDLRenderer3_GetRenderState()->Renderer;
    const ImVec4& clip = command->ClipRect;
    const SDL_Rect rect{static_cast<int>(clip.x), static_cast<int>(clip.y), static_cast<int>(clip.z - clip.x),
                        static_cast<int>(clip.w - clip.y)};
    SDL_SetRenderClipRect(renderer, &rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const MapLineBatch& batch : *batches) {
        const ImVec4 color = ImGui::ColorConvertU32ToFloat4(batch.color);
        SDL_SetRenderDrawColorFloat(renderer, color.x, color.y, color.z, color.w);
        for (std::size_t i = 0; i + 3 < batch.coords.size(); i += 4)
            SDL_RenderLine(renderer, batch.coords[i], batch.coords[i + 1], batch.coords[i + 2],
                           batch.coords[i + 3]);
    }
}

void draw_arrow(ImDrawList* draw, ImVec2 from, ImVec2 to, float gap, ImU32 color) {
    const float font = ImGui::GetFontSize();
    const float length = std::hypot(to.x - from.x, to.y - from.y);
    const float arrow = kMapArrowLengthEms * font;
    if (length <= gap + arrow)
        return;
    const float ux = (to.x - from.x) / length;
    const float uy = (to.y - from.y) / length;
    const ImVec2 tip(to.x - ux * gap, to.y - uy * gap);
    const ImVec2 base(tip.x - ux * arrow, tip.y - uy * arrow);
    const float half = kMapArrowHalfWidthEms * font;
    draw->AddTriangleFilled(tip, ImVec2(base.x - uy * half, base.y + ux * half),
                            ImVec2(base.x + uy * half, base.y - ux * half), color);
}

// Düğümün çizilen ekran yarıçapı. Derece büyümesi (`size`) yerleşim yarıçapına uygulanır, en küçük ekran
// yarıçapına değil: uzaktan bakınca (2.000 düğüm) düğümler en küçük boyda kalır, dolgu alanı ve kare süresi
// büyümez; yakınlaşınca boylar ayrışır. Etiket nottan küçük karedir.
float drawn_radius(bool tag, float size, float zoom, float font) {
    const float sized = std::max(kMapNodeWorldRadius * zoom * size, kMapNodeMinRadiusEms * font);
    return tag ? sized * kMapTagRadiusShare : sized;
}

// Fareyle erişim: çizilen boy, küçük düğümde en az yarım em; etikette aynı 0,7 payla (kare çizilenden geniş
// yakalanmasın, yanındaki notu örtmesin).
float hit_radius(bool tag, float size, float zoom, float font) {
    const float least = kMapHitMinRadiusEms * font * (tag ? kMapTagRadiusShare : 1.0F);
    return std::max(drawn_radius(tag, size, zoom, font), least);
}

// İlişki adı güvenilmezdir (senkron): denetim, bidi ve sıfır genişlikli karakter U+FFFD (terminal_safe).
std::string relation_text(const std::string& name) {
    return tui::terminal_safe(name);
}

// Menüdeki ilişki adı: temiz, kısalmış (uzun ad menüyü pencereden taşırmasın) ve `#`siz (label_safe).
std::string menu_name(const std::string& name) {
    return label_safe(keep_start(relation_text(name), kMapRelationNameMaxEms * ImGui::GetFontSize()));
}

std::string map_label(const MapNode& node) {
    if (node.kind == MapNodeKind::Tag)
        return printable("#" + node.key);
    const auto slash = node.key.rfind('/');
    return printable(slash == std::string::npos ? node.key : node.key.substr(slash + 1));
}

}  // namespace

// Harita sol bölmede, ağacın yerinde; editör sağda kalır. Kare başına: girdi, sınırlı yerleşim adımı, çizim.
void View::draw_map() {
    const NoteMap& map = state_.note_map();
    draw_map_controls(map);
    draw_quick_add();  // F2 "Buraya yeni not…": hızlı ekleme satırı haritada da açılır
    draw_map_find(map);
    draw_map_query();
    if (state_.map_query_open() && state_.map_query().table_shown()) {  // F3: tablo tuvalin yerinde
        draw_map_query_table();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::current().canvas);  // zemin Begin'de çizilir
    ImGui::BeginChild("##map", ImVec2(0.0F, 0.0F), theme::kPaneChildFlags,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    map_focused_ = ImGui::IsWindowFocused();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0F),
                      std::max(ImGui::GetContentRegionAvail().y, 1.0F));
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    remember_item_center("map_canvas");
    if (map.nodes.empty()) {
        map_zoom_keys_ = 0;
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.empty_list_hint").c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    if (const std::size_t generation = state_.map_generation(); generation != map_cache_.generation) {
        map_cache_.generation = generation;
        map_cache_.names.clear();
        map_cache_.labels.clear();
        map_cache_.colors.clear();
        map_cache_.sizes.assign(map.nodes.size(), 0.0F);
        for (const MapEdge& edge : map.edges) {
            map_cache_.sizes[edge.from] += 1.0F;
            map_cache_.sizes[edge.to] += 1.0F;
        }
        for (float& size : map_cache_.sizes)
            size = std::min(1.0F + (kMapDegreeGrowth * std::sqrt(size)), kMapMaxRadiusShare);
        for (const MapNode& node : map.nodes) {
            const bool tag = node.kind == MapNodeKind::Tag;
            map_cache_.names.push_back((tag ? "map:#" : "map:") + node.key);
            map_cache_.labels.push_back(map_label(node));
            map_cache_.colors.push_back(tag ? core::IconColor::Default : core::file_icon(node.key).color);
        }
        map_camera_.auto_fit = true;  // çevre değişti: yeni düğümler görünsün
        map_pressed_.reset();
        map_link_from_.reset();  // dizin yeni çizgede başka düğüm olurdu
        map_connect_from_.reset();
        map_focus_node_.reset();
    }
    // Kenar adları haritanın her değişiminde (ad ya da yön değişimi kuşağı artırmaz): eski ad kalmasın.
    if (const std::size_t version = state_.map_version(); version != map_cache_.edges_version) {
        map_cache_.edges_version = version;
        map_cache_.edge_names.clear();
        for (const MapEdge& edge : map.edges)
            map_cache_.edge_names.push_back(edge.kind == MapEdgeKind::Related ? relation_text(edge.name)
                                                                              : std::string());
    }

    const auto& positions = state_.map_layout().positions();
    int hovered = -1;
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float font = ImGui::GetFontSize();
        float best = std::numeric_limits<float>::max();  // en yakın düğüm kazanır
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const float dx = origin.x + map_camera_.x + positions[i].x * map_camera_.zoom - mouse.x;
            const float dy = origin.y + map_camera_.y + positions[i].y * map_camera_.zoom - mouse.y;
            const float reach = hit_radius(map.nodes[i].kind == MapNodeKind::Tag, map_cache_.sizes[i],
                                           map_camera_.zoom, font);
            const float distance = (dx * dx) + (dy * dy);
            if (distance <= reach * reach && distance < best) {
                best = distance;
                hovered = static_cast<int>(i);
            }
        }
    }
    handle_map_input(map, origin, size, hovered);
    map_animating_ = state_.map_layout().step(kMapIterationsPerFrame);
    if (map_camera_.auto_fit)
        fit_camera(map_camera_, positions, size);
    if (const auto node = std::exchange(map_focus_node_, std::nullopt); node && *node < positions.size())
        focus_map_node(map, *node, size);
    draw_map_graph(map, origin, hovered);
    draw_map_menus();
    ImGui::EndChild();
}

// Not → not: ilişki mi bağlantı mı sorulur; not ↔ etiket hemen eklenir; etiket → etiket ve boşluk hiçbir şey.
void View::drop_map_link(const NoteMap& map, std::size_t from_node, int hovered) {
    if (hovered < 0 || from_node >= map.nodes.size())
        return;
    const MapNode& from = map.nodes[from_node];
    const MapNode& to = map.nodes[static_cast<std::size_t>(hovered)];
    switch (app::map_drop(from, to)) {
    case app::MapDrop::ChooseNoteEdge:
        map_choice_ = MapPair{from, to};
        ImGui::OpenPopup("##map_connect");
        break;
    case app::MapDrop::AddTag:
        queue([this, from, to] { state_.connect_map_nodes(from, to, MapEdgeKind::Tag); });
        break;
    case app::MapDrop::None:
        break;
    }
}

// Düğüm dışındaki sağ tık işaretçinin yakınındaki kenarı arar (yakınlıkta düğüm önce gelir).
void View::open_map_edge_menu(const NoteMap& map, ImVec2 origin) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MapCamera& camera = map_camera_;
    const app::LayoutPoint at{(mouse.x - origin.x - camera.x) / camera.zoom,
                              (mouse.y - origin.y - camera.y) / camera.zoom};
    const float reach = kMapEdgeHitEms * ImGui::GetFontSize() / camera.zoom;
    const auto edge = app::map_edge_at(map, state_.map_layout().positions(), at, reach);
    if (!edge) {  // F2: boşlukta tuval menüsü (yeni not, sabitlemeler, yol)
        map_canvas_menu_ = at;
        ImGui::OpenPopup("##map_canvas_menu");
        return;
    }
    const MapEdge& hit = map.edges[*edge];
    map_edge_menu_ = MapPair{map.nodes[hit.from], map.nodes[hit.to], hit.kind, hit.relation, hit.name, hit.directed};
    ImGui::OpenPopup("##map_edge_menu");
}

// Düğüm menüsü: Bağla… ve düğümün kaldırılabilir kenarları (ilişki, etiket). Bağlantı notun metnindedir,
// listede yoktur (kenar menüsü nerede silineceğini söyler).
void View::open_map_node_menu(const NoteMap& map, std::size_t node) {
    map_relation_names_ = state_.map_relation_names();  // "Komşuları aç" alt menüsünün adları, açılışta bir kez
    MapNodeMenu menu{.generation = map_cache_.generation, .node = node, .subject = map.nodes[node], .removable = {}};
    for (const MapEdge& edge : map.edges) {
        if ((edge.from == node || edge.to == node) && app::map_edge_removable(edge.kind))
            menu.removable.push_back(
                MapPair{map.nodes[edge.from], map.nodes[edge.to], edge.kind, edge.relation, edge.name, edge.directed});
    }
    map_node_menu_ = std::move(menu);
    ImGui::OpenPopup("##map_node_menu");
}

void View::start_map_connect(const NoteMap& map, std::size_t from) {
    if (from >= map.nodes.size())
        return;
    map_connect_from_ = from;
    map_pick_ = MapPick::Connect;
    ImGui::SetWindowFocus();  // Esc ve `C` tuvalde çalışsın
}

// Düğüm menüsünün keşif öğeleri (F2): aç, komşular, sabitleme, en kısa yol, çöp. Etiket düğümünde yalnız
// sabitleme. Menü açıkken çizge yeniden kurulduysa dizin başka düğümdür: dizinli işler kuşağı denetler.
void View::draw_map_node_items(const MapNode& subject, std::size_t node, std::size_t generation) {
    const bool note = subject.kind == MapNodeKind::Note;
    std::optional<core::NotePath> parsed;
    if (note) {
        if (auto path = core::NotePath::parse(subject.key))
            parsed = std::move(*path);
    }
    if (parsed) {
        if (ImGui::MenuItem((core::tr("map.open") + "###map_node_open").c_str()))
            queue([this, path = *parsed] { state_.open_map_note(path); });
        remember_item_center("map_node_open");
    }
    if (note) {
        const bool whole = state_.map_options().whole_graph;  // bütün çizgede her komşu görünür
        ImGui::BeginDisabled(whole);
        if (ImGui::BeginMenu((core::tr("map.expand") + "###map_node_expand").c_str())) {
            const auto item = [&](const std::string& label, const std::string& id, const app::MapExpansion& spec) {
                if (ImGui::MenuItem((label + "###" + id).c_str()))
                    queue([this, subject, spec] { state_.expand_map_node(subject, spec); });
                remember_item_center(id);
            };
            item(core::tr("map.expand_all"), "map_expand_all", {});
            item(core::tr("map.expand_links"), "map_expand_links", {.kind = MapEdgeKind::Link});
            item(core::tr("map.expand_related"), "map_expand_related", {.kind = MapEdgeKind::Related});
            for (const core::RelationName& name : map_relation_names_)
                item(core::tr("map.expand_named", {{"name", menu_name(name.display)}}), "map_expand_named:" + name.key,
                     {.kind = MapEdgeKind::Related, .name = name.key});
            ImGui::EndMenu();
        }
        remember_item_center("map_node_expand");
        if (ImGui::MenuItem((core::tr("map.collapse") + "###map_node_collapse").c_str()))
            queue([this, subject] { state_.collapse_map_node(subject); });
        remember_item_center("map_node_collapse");
        ImGui::EndDisabled();
        if (whole)
            ImGui::SetItemTooltip("%s", core::tr("map.whole_graph_open").c_str());
    }
    const GraphLayout& layout = state_.map_layout();
    const bool pinned = generation == map_cache_.generation && node < layout.size() && layout.pinned(node);
    if (ImGui::MenuItem((core::tr(pinned ? "map.unpin" : "map.pin") + "###map_node_pin").c_str()))
        queue([this, node, generation] {
            if (map_cache_.generation == generation)
                state_.toggle_map_pin(node);
        });
    remember_item_center("map_node_pin");
    if (note) {
        const auto pick = [this, node, generation](MapPick kind) {
            queue([this, node, generation, kind] {
                if (map_cache_.generation != generation)
                    return;
                map_connect_from_ = node;
                map_pick_ = kind;
            });
        };
        if (ImGui::MenuItem((core::tr("map.path") + "###map_node_path").c_str()))
            pick(MapPick::Path);
        remember_item_center("map_node_path");
        if (ImGui::MenuItem((core::tr("map.path_directed") + "###map_node_path_directed").c_str()))
            pick(MapPick::DirectedPath);
        remember_item_center("map_node_path_directed");
    }
    if (state_.map_scope().path) {
        if (ImGui::MenuItem((core::tr("map.clear_path") + "###map_node_clear_path").c_str()))
            queue([this] { state_.clear_map_path(); });
        remember_item_center("map_node_clear_path");
    }
    if (parsed) {
        if (ImGui::MenuItem((core::tr("map.trash") + "###map_node_trash").c_str()))
            queue([this, path = *parsed] { state_.trash_map_note(path); });
        remember_item_center("map_node_trash");
    }
    ImGui::Separator();
}

// Esc ve dışarı tıklama ImGui'nin açılır pencere kuralıyla kapatır. Adlar güvenilmez: biçim dizgisine girmez.
void View::draw_map_menus() {
    if (map_node_menu_ && ImGui::BeginPopup("##map_node_menu")) {
        const MapNodeMenu menu = *map_node_menu_;
        draw_map_node_items(menu.subject, menu.node, menu.generation);
        if (ImGui::MenuItem((core::tr("map.connect") + "###map_node_connect").c_str()))
            queue([this, menu] {
                // Menü açıkken çizge yeniden kurulduysa dizin başka düğümdür: kip açılmaz.
                if (map_cache_.generation != menu.generation)
                    return;
                map_connect_from_ = menu.node;
                map_pick_ = MapPick::Connect;
            });
        remember_item_center("map_node_connect");
        if (!menu.removable.empty() && ImGui::BeginMenu((core::tr("map.remove_relations") + "###map_node_remove").c_str())) {
            for (std::size_t i = 0; i < menu.removable.size(); ++i) {
                const MapPair& edge = menu.removable[i];
                const MapNode& other = edge.from.key == menu.subject.key && edge.from.kind == menu.subject.kind
                                           ? edge.to
                                           : edge.from;
                const MapNode& note = edge.from.kind == MapNodeKind::Note ? edge.from : edge.to;
                const MapNode& tag = edge.from.kind == MapNodeKind::Tag ? edge.from : edge.to;
                const std::string label =
                    edge.kind == MapEdgeKind::Tag
                        ? core::tr("map.remove_tag",
                                   {{"tag", label_safe(printable(tag.key))}, {"path", label_safe(printable(note.key))}})
                    : edge.name.empty()
                        ? core::tr("map.remove_related_to", {{"path", label_safe(printable(other.key))}})
                        : core::tr("map.remove_named_related_to",
                                   {{"name", menu_name(edge.name)}, {"path", label_safe(printable(other.key))}});
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(label.c_str()))
                    queue([this, edge] {
                        state_.disconnect_map_nodes(edge.from, edge.to, edge.kind, edge.relation);
                    });
                remember_item_center("map_node_remove:" + other.key +
                                     (edge.name.empty() ? "" : ":" + edge.name));
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (!menu.removable.empty())
            remember_item_center("map_node_remove");
        ImGui::EndPopup();
    }
    if (map_choice_ && ImGui::BeginPopup("##map_connect")) {
        const MapPair pair = *map_choice_;
        // F1: ilişki ad penceresinden geçer (ad, "Yönlü"); bağlantı ve etiket hemen yazılır.
        if (ImGui::MenuItem((core::tr("map.choose_related") + "###map_connect_related").c_str()))
            queue([this, pair] { state_.relate_map_notes(pair.from, pair.to); });
        remember_item_center("map_connect_related");
        const std::string link = core::tr("map.choose_link", {{"from", label_safe(printable(pair.from.key))}});
        if (ImGui::MenuItem((link + "###map_connect_link").c_str()))
            queue([this, pair] { state_.connect_map_nodes(pair.from, pair.to, MapEdgeKind::Link); });
        remember_item_center("map_connect_link");
        ImGui::EndPopup();
    }
    if (map_edge_menu_ && ImGui::BeginPopup("##map_edge_menu")) {
        const MapPair edge = *map_edge_menu_;
        const MapNode& note = edge.from.kind == MapNodeKind::Note ? edge.from : edge.to;
        const MapNode& tag = edge.from.kind == MapNodeKind::Tag ? edge.from : edge.to;
        const std::string label =
            edge.kind == MapEdgeKind::Related
                ? (edge.name.empty() ? core::tr("map.remove_related")
                                     : core::tr("map.remove_named_related", {{"name", menu_name(edge.name)}}))
            : edge.kind == MapEdgeKind::Tag
                ? core::tr("map.remove_tag", {{"tag", label_safe(printable(tag.key))}, {"path", label_safe(printable(note.key))}})
                : core::tr("map.remove_link");
        if (edge.kind == MapEdgeKind::Related)
            draw_relation_items(edge);
        // Bağlantı notun metnindedir: kaldırma kapalı, ipucu nerede silineceğini söyler.
        const bool removable = app::map_edge_removable(edge.kind);
        ImGui::BeginDisabled(!removable);
        if (ImGui::MenuItem((label + "###map_edge_remove").c_str()))
            queue(
                [this, edge] { state_.disconnect_map_nodes(edge.from, edge.to, edge.kind, edge.relation); });
        remember_item_center("map_edge_remove");
        ImGui::EndDisabled();
        if (!removable)
            ImGui::SetItemTooltip("%s", core::tr("map.link_in_text", {{"from", printable(edge.from.key)},
                                                                      {"to", printable(edge.to.key)}})
                                            .c_str());
        ImGui::EndPopup();
    }
    // F2 tuval menüsü: boşluğa sağ tık. Yeni not var olan hızlı ekleme satırından, tıklanan yere sabitlenir.
    if (map_canvas_menu_ && ImGui::BeginPopup("##map_canvas_menu")) {
        const app::LayoutPoint at = *map_canvas_menu_;
        if (ImGui::MenuItem((core::tr("map.new_note") + "###map_canvas_new_note").c_str()))
            queue([this, at] { state_.begin_map_note(at); });
        remember_item_center("map_canvas_new_note");
        ImGui::BeginDisabled(!state_.map_layout().has_pins());
        if (ImGui::MenuItem((core::tr("map.unpin_all") + "###map_canvas_unpin_all").c_str()))
            queue([this] { state_.unpin_map(); });
        remember_item_center("map_canvas_unpin_all");
        ImGui::EndDisabled();
        if (state_.map_scope().path) {
            if (ImGui::MenuItem((core::tr("map.clear_path") + "###map_canvas_clear_path").c_str()))
                queue([this] { state_.clear_map_path(); });
            remember_item_center("map_canvas_clear_path");
        }
        ImGui::EndPopup();
    }
}

// Kenar menüsünün ilişki öğeleri (F1): yeniden adlandır, yön; hepsi kenarın kimliğiyle. Yönlüde from
// kaynaktır: çevirmek kaynağı to yapar; yönsüz, gösterilen uçtan (yoksa from'dan) öbürüne yönlenir.
void View::draw_relation_items(const MapPair& edge) {
    const auto from = core::NotePath::parse(edge.from.key);
    const auto to = core::NotePath::parse(edge.to.key);
    if (!from || !to)
        return;
    const auto direct = [this, edge, first = *from, second = *to](std::optional<core::NotePath> source) {
        queue([this, edge, first, second, source] { state_.direct_relation(first, second, edge.relation, source); });
    };
    if (ImGui::MenuItem((core::tr("map.rename_relation") + "###map_edge_rename").c_str()))
        queue([this, edge, first = *from, second = *to] {
            state_.rename_relation(first, second, edge.relation, edge.name);
        });
    remember_item_center("map_edge_rename");
    // F4: ilişkinin özellikleri denetçide (ad ve yön orada salt okunur, bu menüden değişir).
    if (ImGui::MenuItem((core::tr("inspector.inspect_relation") + "###map_edge_inspect").c_str()))
        queue([this, edge, first = *from, second = *to] {
            state_.inspect_relation(first, second, edge.relation, edge.name, edge.directed);
        });
    remember_item_center("map_edge_inspect");
    if (edge.directed) {
        if (ImGui::MenuItem((core::tr("map.flip_relation") + "###map_edge_flip").c_str()))
            direct(*to);
        remember_item_center("map_edge_flip");
        if (ImGui::MenuItem((core::tr("map.make_undirected") + "###map_edge_undirect").c_str()))
            direct(std::nullopt);
        remember_item_center("map_edge_undirect");
    } else {
        // Gösterilen not uçlardan biriyse ondan başlar (TUI `f` ve bağlantı paneli gibi); değilse yol sırası.
        const bool from_shown_end = state_.current_path() == *to;
        const MapNode& start = from_shown_end ? edge.to : edge.from;
        const MapNode& end = from_shown_end ? edge.from : edge.to;
        const std::string label = core::tr("map.make_directed", {{"from", label_safe(map_label(start))},
                                                                 {"to", label_safe(map_label(end))}});
        if (ImGui::MenuItem((label + "###map_edge_direct").c_str()))
            direct(from_shown_end ? *to : *from);
        remember_item_center("map_edge_direct");
    }
    ImGui::Separator();
}

void View::draw_map_controls(const NoteMap& map) {
    MapOptions options = state_.map_options();
    bool changed = false;
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    // Açma/kapama çipi (G13): seçiliyken seçili satırın tonu, değilken hayalet ve soluk metin.
    const auto chip = [&](const char* name, const std::string& label, bool& on, const char* tip_key) {
        const theme::Theme& colors = theme::current();
        ImGui::PushStyleColor(ImGuiCol_Button, on ? colors.selected : 0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? colors.selected : colors.hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? colors.selected : colors.control_active);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? colors.text : colors.muted);
        if (button(name, label)) {
            on = !on;
            changed = true;
        }
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("%s", core::tr(tip_key).c_str());
    };
    chip("map_whole", core::tr("gui.map_whole"), options.whole_graph, "gui.map_whole_tip");

    // Adımlı derinlik: [−] Derinlik 2 [+]; bir kademe sürgüden okunaklı, uçlarda düğme kapanır.
    const std::string depth_text = core::tr("gui.map_depth", {{"depth", std::to_string(options.depth)}});
    const float step_width = button_width("−");
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    flow_after_previous((2.0F * (step_width + spacing)) + text_width(depth_text), row_start_x, row_width);
    ImGui::BeginDisabled(options.whole_graph);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.depth <= MapOptions::kMinDepth);
    if (button("map_depth_less", "−", ButtonKind::Ghost)) {
        --options.depth;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, spacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(depth_text.c_str());
    ImGui::SameLine(0.0F, spacing);
    ImGui::BeginDisabled(options.depth >= state_.map_max_depth());
    if (button("map_depth_more", "+", ButtonKind::Ghost)) {
        ++options.depth;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::SetItemTooltip("%s", core::tr("gui.map_depth_tip").c_str());
    remember_item_center("map_depth");
    ImGui::EndDisabled();

    const std::string tags = core::tr("gui.map_tags");
    flow_after_previous(button_width(tags), row_start_x, row_width);
    chip("map_tags", tags, options.show_tags, "gui.map_tags_tip");

    // F2 süzgeçler: kenar türü çipleri, ilişki adı seçicisi (NoteStore::relation_names), etiket kutusu (`tag:`).
    const std::string links = core::tr("gui.map_links");
    flow_after_previous(button_width(links), row_start_x, row_width);
    chip("map_links", links, options.show_links, "gui.map_links_tip");
    const std::string related = core::tr("gui.map_related");
    flow_after_previous(button_width(related), row_start_x, row_width);
    chip("map_related", related, options.show_related, "gui.map_related_tip");
    const float font = ImGui::GetFontSize();
    const std::string preview = !options.relation          ? core::tr("gui.map_relation_any")
                                : options.relation->empty() ? core::tr("map.unnamed_relation")
                                                            : menu_name(*options.relation);
    flow_after_previous(kMapFilterEms * font, row_start_x, row_width);
    ImGui::SetNextItemWidth(kMapFilterEms * font);
    if (ImGui::BeginCombo("##map_relation", preview.c_str())) {
        if (ImGui::IsWindowAppearing())  // depo açılışta bir kez okunur, kare başına değil
            map_relation_names_ = state_.map_relation_names();
        const auto choose = [&](const std::string& label, const std::string& id,
                                const std::optional<std::string>& value) {
            if (ImGui::Selectable((label + "###" + id).c_str(), options.relation == value)) {
                options.relation = value;
                changed = true;
            }
            remember_item_center(id);
        };
        choose(core::tr("gui.map_relation_any"), "map_relation_any", std::nullopt);
        choose(core::tr("map.unnamed_relation"), "map_relation_unnamed", std::string());
        for (const core::RelationName& name : map_relation_names_)
            choose(menu_name(name.display), "map_relation:" + name.key, name.key);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", core::tr("gui.map_relation_tip").c_str());
    remember_item_center("map_relation");
    flow_after_previous(kMapFilterEms * font, row_start_x, row_width);
    ImGui::SetNextItemWidth(kMapFilterEms * font);
    if (ImGui::InputTextWithHint("##map_tag", core::tr("gui.map_tag_hint").c_str(), &map_tag_filter_)) {
        const std::size_t first = map_tag_filter_.find_first_not_of(" \t");
        options.tag = first == std::string::npos
                          ? std::string()
                          : map_tag_filter_.substr(first, map_tag_filter_.find_last_not_of(" \t") - first + 1);
        changed = true;
    }
    ImGui::SetItemTooltip("%s", core::tr("gui.map_tag_tip").c_str());
    remember_item_center("map_tag");
    if (changed)
        queue([this, options] { state_.set_map_options(options); });
    // F3 sorgu çubuğu: açıp kapatan çip (tuvalde `Q`); kapanış sonucu ve vurguyu kaldırır.
    const std::string query = core::tr("gui.map_query");
    flow_after_previous(button_width(query), row_start_x, row_width);
    bool query_open = state_.map_query_open();
    chip("map_query_toggle", query, query_open, "gui.map_query_tip");
    if (query_open != state_.map_query_open()) {  // çipin `changed`i seçenekleri yukarıda kuyruğa almıştı
        map_query_focus_ = query_open;
        queue([this] { state_.toggle_map_query(); });
    }

    // İlişki kurmanın yolu görünür olsun (MAP-R keşfedilmiyordu): kipteyken kaynağı ve vazgeçmeyi söyler.
    ImGui::PushFont(nullptr, kSmallFontPixels);
    ImGui::PushTextWrapPos(0.0F);
    const bool picking = map_connect_from_ && *map_connect_from_ < map.nodes.size();
    const std::string hint =
        !picking ? core::tr("map.connect_hint")
                 : core::tr(map_pick_ == MapPick::Connect ? "map.connect_pending" : "map.path_pending",
                            {{"from", map_label(map.nodes[*map_connect_from_])}});
    ImGui::TextDisabled("%s", hint.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    remember_item_center("map_hint");
    if (map.capped()) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.map_capped", {{"shown", std::to_string(map.nodes.size())},
                                                           {"total", std::to_string(map.total)}})
                                   .c_str());
        ImGui::PopTextWrapPos();
        remember_item_center("map_capped");
    }
}

// Sol tuş: düğümde basılıp bırakılırsa açar, sürüklenirse iğneler; boşlukta sürükleme ve orta tuş kaydırır.
// Tuşlar yalnız tuval odaktayken: arama kutusunda yazılan '+' haritayı yakınlaştırmasın.
void View::handle_map_input(const NoteMap& map, ImVec2 origin, ImVec2 size, int hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    MapCamera& camera = map_camera_;
    const auto note_path = [&map](std::size_t node) -> std::optional<core::NotePath> {
        if (map.nodes[node].kind != MapNodeKind::Note)
            return std::nullopt;
        auto path = core::NotePath::parse(map.nodes[node].key);
        return path ? std::optional(std::move(*path)) : std::nullopt;
    };
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        map_pressed_ = hovered >= 0 ? std::optional(static_cast<std::size_t>(hovered)) : std::nullopt;
        map_dragged_ = false;
        // Shift+sürükleme düğümden düğüme ilişki kurar (MAP-R): düğüm iğnelenmez, bırakınca açılmaz.
        map_link_from_ = io.KeyShift ? map_pressed_ : std::nullopt;
    }
    const bool active = ImGui::IsItemActive();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && map_link_from_) {
        // Lastik çizgi draw_map_graph'ta; kamera yerinde kalır.
    } else if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        camera.auto_fit = false;
        if (map_pressed_) {
            state_.map_layout().pin(*map_pressed_, {(io.MousePos.x - origin.x - camera.x) / camera.zoom,
                                                    (io.MousePos.y - origin.y - camera.y) / camera.zoom});
            map_dragged_ = true;
        } else {
            camera.x += io.MouseDelta.x;
            camera.y += io.MouseDelta.y;
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        camera.auto_fit = false;
        camera.x += io.MouseDelta.x;
        camera.y += io.MouseDelta.y;
    }
    if (ImGui::IsItemDeactivated()) {
        if (map_link_from_) {
            drop_map_link(map, *map_link_from_, hovered);
        } else if (map_connect_from_ && !map_dragged_) {
            // Bağla kipinde tıklama hedefi seçer; boşluk ya da aynı düğüm vazgeçer (map_drop None). Yol kipinde
            // (F2) tıklanan düğüm yolun sonudur.
            if (map_pick_ == MapPick::Connect) {
                drop_map_link(map, *map_connect_from_, map_pressed_ ? static_cast<int>(*map_pressed_) : -1);
            } else if (map_pressed_ && *map_pressed_ != *map_connect_from_ && *map_connect_from_ < map.nodes.size()) {
                queue([this, from = map.nodes[*map_connect_from_], to = map.nodes[*map_pressed_],
                       directed = map_pick_ == MapPick::DirectedPath] { state_.find_map_path(from, to, directed); });
            }
            map_connect_from_.reset();
        } else if (map_pressed_ && !map_dragged_) {
            if (const auto path = note_path(*map_pressed_)) {
                if (io.KeyCtrl)
                    queue_open(*path, true);
                else
                    queue([this, path = *path] { state_.open_map_note(path); });
            }
        }
        map_pressed_.reset();
        map_link_from_.reset();
    }
    // Sağ tık: düğümde düğüm menüsü, boşlukta yakındaki kenarın menüsü, kenar yoksa tuval menüsü (F2).
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        map_connect_from_.reset();
        if (hovered >= 0)
            open_map_node_menu(map, static_cast<std::size_t>(hovered));
        else
            open_map_edge_menu(map, origin);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (map_connect_from_)
            map_connect_from_.reset();
        else if (ImGui::IsWindowFocused())  // F3 sorgu, sonra F2 yol (TUI sırası)
            queue([this] { state_.escape_map(); });
    }
    // F2: çift tıklama komşuları açar ya da kapar (Neo4j Browser gibi); ilk tıklama notu açmıştır. Shift
    // basılıyken ikinci basış yeni bir sürüklemedir, çift tıklama sayılmaz.
    if (!map_link_from_ && hovered >= 0 && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        queue([this, node = map.nodes[static_cast<std::size_t>(hovered)]] { state_.toggle_map_neighbours(node); });
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0F)
        zoom_map(origin, io.MousePos, std::pow(kMapWheelZoomStep, io.MouseWheel));

    const int zoom_steps = std::exchange(map_zoom_keys_, 0);
    if (!ImGui::IsWindowFocused())
        return;
    const float pan = kMapKeyPanEms * ImGui::GetFontSize();
    const std::pair<ImGuiKey, ImVec2> arrows[] = {{ImGuiKey_LeftArrow, ImVec2(pan, 0.0F)},
                                                  {ImGuiKey_RightArrow, ImVec2(-pan, 0.0F)},
                                                  {ImGuiKey_UpArrow, ImVec2(0.0F, pan)},
                                                  {ImGuiKey_DownArrow, ImVec2(0.0F, -pan)}};
    for (const auto& [key, shift] : arrows) {
        if (ImGui::IsKeyPressed(key)) {
            camera.auto_fit = false;
            camera.x += shift.x;
            camera.y += shift.y;
        }
    }
    if (zoom_steps != 0)
        zoom_map(origin, ImVec2(origin.x + size.x / 2.0F, origin.y + size.y / 2.0F),
                 std::pow(kMapKeyZoomStep, static_cast<float>(zoom_steps)));
    if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))
        camera.auto_fit = true;
    const auto shown_node = [&]() -> std::optional<std::size_t> {
        const auto current = state_.current_path();
        if (!current)
            return std::nullopt;
        const auto found = std::ranges::find_if(map.nodes, [&](const MapNode& node) {
            return node.kind == MapNodeKind::Note && node.key == current->string();
        });
        return found == map.nodes.end() ? std::nullopt
                                        : std::optional(static_cast<std::size_t>(found - map.nodes.begin()));
    };
    // TUI `C` (connect): gösterilen nottan bağla kipi.
    if (ImGui::IsKeyChordPressed(ImGuiKey_C)) {
        if (const auto node = shown_node())
            start_map_connect(map, *node);
    }
    // F2, TUI ile aynı harfler: P(in) üstündeki düğümü (yoksa gösterilen notu) sabitler/çözer, U(npin) hepsini
    // çözer, Ctrl+F haritada bul (not içi bul değil: View::handle_shortcuts tuvaldeyken geçer).
    if (ImGui::IsKeyChordPressed(ImGuiKey_P)) {
        if (const auto node = hovered >= 0 ? std::optional(static_cast<std::size_t>(hovered)) : shown_node())
            queue([this, node = *node] { state_.toggle_map_pin(node); });
    }
    if (ImGui::IsKeyChordPressed(ImGuiKey_U))
        queue([this] { state_.unpin_map(); });
    // F3, TUI ile aynı harf: Q(uery) sorgu çubuğunu açar (açıksa kapatır).
    if (ImGui::IsKeyChordPressed(ImGuiKey_Q)) {
        map_query_focus_ = !state_.map_query_open();
        queue([this] { state_.toggle_map_query(); });
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        map_find_open_ = true;
        map_find_focus_ = true;
    }
}

// Haritada bul (F2): yazdıkça eşleşmeler yanar, ilki ortalanır; Enter sonraki (Shift+Enter önceki), Esc kapatır.
// Eşleşme kuralı app::find_map_nodes (TUI `/` ile aynı).
void View::draw_map_find(const NoteMap& map) {
    if (!map_find_open_)
        return;
    if (std::exchange(map_find_focus_, false))
        ImGui::SetKeyboardFocusHere();
    const std::string counter =
        map_find_.empty()     ? std::string()
        : map_found_.empty() ? core::tr("map.find_none")
                             : core::tr("map.find_count", {{"at", std::to_string(map_found_at_ + 1)},
                                                           {"count", std::to_string(map_found_.size())}});
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - text_width(counter) - spacing, 1.0F));
    const bool entered = ImGui::InputTextWithHint("##map_find", core::tr("map.find_hint").c_str(), &map_find_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    remember_item_center("map_find");
    const bool edited = ImGui::IsItemEdited();
    const bool escape = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (!counter.empty()) {
        ImGui::SameLine(0.0F, spacing);
        ImGui::TextUnformatted(counter.c_str());
    }
    if (escape) {
        map_find_open_ = false;
        map_find_.clear();
        map_found_.clear();
        return;
    }
    if (edited || map_found_version_ != state_.map_version()) {  // harita değişince dizinler yenilenir
        map_found_ = app::find_map_nodes(map, map_find_);
        map_found_version_ = state_.map_version();
        map_found_at_ = edited ? 0 : std::min(map_found_at_, map_found_.empty() ? 0 : map_found_.size() - 1);
        if (edited && !map_found_.empty())
            map_focus_node_ = map_found_.front();
    }
    if (entered && !map_found_.empty()) {
        map_found_at_ = app::cycled_tab(map_found_at_, ImGui::GetIO().KeyShift ? -1 : 1, map_found_.size());
        map_focus_node_ = map_found_[map_found_at_];
    }
    if (entered)
        map_find_focus_ = true;  // Enter girdiyi bırakır: sonraki Enter da eşleşmeyi değiştirsin
}

namespace {

// ↑ ↓ sorgu geçmişi (tui::InputLine, arama geçmişiyle aynı yardımcı): yürüyüşün metni kutuya yazılır.
int map_query_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory)
        return 0;
    auto& state = *static_cast<GuiState*>(data->UserData);
    const auto next = state.map_query_history_step(
        std::string_view(data->Buf, static_cast<std::size_t>(data->BufTextLen)), data->EventKey == ImGuiKey_UpArrow);
    if (next) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, next->data(), next->data() + next->size());
    }
    return 0;
}

}  // namespace

// Sorgu çubuğu (F3): kayıtlı görünümler, tek satırlık Cypher (Enter ya da Ctrl+Enter çalıştırır, ↑ ↓ geçmiş),
// Çalıştır ve Tablo. Hata çubuğun altında; metin güvenilmez olabilir (sorgu motorun iletisine girer).
void View::draw_map_query() {
    if (!state_.map_query_open())
        return;
    const float font = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(kMapFilterEms * font);
    if (ImGui::BeginCombo("##map_query_views", core::tr("gui.map_query_views").c_str())) {
        if (ImGui::IsWindowAppearing())  // depo açılışta bir kez okunur, kare başına değil
            map_view_names_ = state_.map_view_names();
        if (map_view_names_.empty())
            ImGui::TextDisabled("%s", core::tr("gui.map_query_no_views").c_str());
        for (const std::string& name : map_view_names_) {
            // Ad senkronla gelebilir: etikette `##` kimliği kesemez, gizli karakter çizilmez.
            if (ImGui::Selectable((label_safe(tui::terminal_safe(name)) + "###map_query_view:" + name).c_str()))
                queue([this, name] { state_.run_map_view(name); });
            remember_item_center("map_query_view:" + name);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", core::tr("gui.map_query_views_tip").c_str());
    remember_item_center("map_query_views");

    const std::string run = core::tr("gui.map_query_run");
    const std::string table = core::tr("gui.map_query_table");
    const std::string save = core::tr("gui.map_query_save");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - button_width(run) -
                                         button_width(table) - button_width(save) - 3.0F * spacing,
                                     kMapFilterEms * font));
    if (std::exchange(map_query_focus_, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::PushID(static_cast<int>(state_.map_query_generation()));  // boşalan metin kutuya yeniden okunur
    const bool entered = ImGui::InputTextWithHint(
        "##map_query", core::tr("gui.map_query_hint").c_str(), &state_.map_query_buffer(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, map_query_callback, &state_);
    ImGui::PopID();
    remember_item_center("map_query");
    ImGui::SameLine();
    if (button("map_query_run", run, ButtonKind::Primary) || entered) {
        map_query_focus_ = true;  // sonuç ya da hata: yazmaya devam edilir
        queue([this] { state_.run_map_query(); });
    }
    ImGui::SameLine();
    const bool has_result = state_.map_query().outcome().has_value();
    ImGui::BeginDisabled(!has_result);
    const theme::Theme& colors = theme::current();
    const bool shown = state_.map_query().table_shown();
    ImGui::PushStyleColor(ImGuiCol_Button, shown ? colors.selected : 0);
    if (button("map_query_table", table, ButtonKind::Ghost))
        queue([this] { state_.toggle_map_query_table(); });
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr("gui.map_query_table_tip").c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(state_.map_query().read_query().empty());  // görünüm yalnız okumanın sorgusudur
    if (button("map_query_save", save, ButtonKind::Ghost))
        queue([this] { state_.save_map_view(); });
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr("gui.map_query_save_tip").c_str());
    if (const std::string& error = state_.map_query().error(); !error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.danger);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(tui::terminal_safe(error).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        remember_item_center("map_query_error");
    }
}

// Sonucun tablosu (F3): yalnız görünen satırlar çizilir (ImGuiListClipper). Hücreler güvenilmez kaynaktandır ama
// query_outcome'da bir kez temizlenmiş ve kısaltılmıştır: TextUnformatted ile çizilir, etiket olmaz (başlık
// etikettir: label_safe). Not hücresi tıklanınca not açılır.
void View::draw_map_query_table() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıran table_shown'a baktı: sonuç dolu
    const app::QueryOutcome& outcome = *state_.map_query().outcome();
    ImGui::TextUnformatted(tui::terminal_safe(outcome.summary).c_str());
    const app::QueryTable& table = outcome.table;
    if (table.columns.empty()) {
        ImGui::TextDisabled("%s", core::tr("app.map_query_no_columns").c_str());
        return;
    }
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    // BeginTable `columns < IMGUI_TABLE_MAX_COLUMNS` ister (assert): fazlası çizilmez, kaçı gizlendiği söylenir.
    constexpr std::size_t kMaxColumns = IMGUI_TABLE_MAX_COLUMNS - 1;
    const int columns = static_cast<int>(std::min(table.columns.size(), kMaxColumns));
    if (table.columns.size() > kMaxColumns) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.map_query_columns_hidden",
                                        {{"count", std::to_string(table.columns.size() - kMaxColumns)},
                                         {"shown", std::to_string(kMaxColumns)}})
                                   .c_str());
        ImGui::PopTextWrapPos();
        remember_item_center("map_query_hidden_columns");
    }
    if (!ImGui::BeginTable("##map_query_rows", columns, kFlags, ImVec2(0.0F, 0.0F)))
        return;
    remember_rect("map_query_rows", ImGui::GetWindowPos(),
                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int column = 0; column < columns; ++column)  // başlık bir etikettir: `#` kimliği kesemez
        ImGui::TableSetupColumn(
            (label_safe(table.columns[static_cast<std::size_t>(column)]) + "###column" +
             std::to_string(column))
                .c_str());
    ImGui::TableHeadersRow();
    const ImU32 link = theme::current().accent;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(table.rows.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& cells = table.rows[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            for (int column = 0; column < columns && static_cast<std::size_t>(column) < cells.size(); ++column) {
                ImGui::TableSetColumnIndex(column);
                const app::QueryCell& cell = cells[static_cast<std::size_t>(column)];
                const bool note = cell.kind == app::QueryCell::Kind::Note;
                if (note)
                    ImGui::PushStyleColor(ImGuiCol_Text, link);
                ImGui::TextUnformatted(cell.text.c_str());  // query_outcome temizledi
                if (note) {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        if (auto path = core::NotePath::parse(cell.note))
                            queue([this, path = std::move(*path)] { state_.open_map_note(path); });
                }
                remember_rect("map_query_cell:" + std::to_string(row) + ":" + std::to_string(column),
                              ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
        }
    }
    ImGui::EndTable();
}

// Düğüm tuvalin ortasına gelir, yakınlık kalır; kendiliğinden sığdırma kapanır (yoksa sonraki kare geri sığardı).
void View::focus_map_node(const NoteMap& map, std::size_t node, ImVec2 size) {
    const auto& positions = state_.map_layout().positions();
    if (node >= positions.size() || node >= map.nodes.size())
        return;
    map_camera_.auto_fit = false;
    map_camera_.x = size.x / 2.0F - positions[node].x * map_camera_.zoom;
    map_camera_.y = size.y / 2.0F - positions[node].y * map_camera_.zoom;
}

// `anchor` altındaki yerleşim noktası yerinde kalır.
void View::zoom_map(ImVec2 origin, ImVec2 anchor, float factor) {
    map_camera_.zoom_at(anchor.x - origin.x, anchor.y - origin.y, factor, {kMapMinZoom, kMapMaxZoom});
    map_camera_.auto_fit = false;
}

// Tuval dışındaki kenar ve düğüm çizilmez. Etiketler düğümlerden sonra: çizgilerin üstünde okunur. Ad
// güvenilmez: biçim dizgisine girmez, denetim baytı U+FFFD'dir (map_label).
void View::draw_map_graph(const NoteMap& map, ImVec2 origin, int hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Yazılımsal çizicide maliyet üçgen alanıyla büyür: yumuşatma şeritleri 5.000 kenarda kare bütçesini
    // aşırır. Bayrak yalnız haritanın kendi çizim listesinde ve bu kare için (ImGui her kare stilden yeniden
    // kurar).
    draw->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex |
                     ImDrawListFlags_AntiAliasedFill);
    const auto& positions = state_.map_layout().positions();
    const auto& matches = state_.map_matches();
    const bool filtering = !state_.query().empty();
    const MapCamera& camera = map_camera_;
    const float font = ImGui::GetFontSize();
    const auto screen = [&](std::size_t node) {
        return ImVec2(origin.x + camera.x + positions[node].x * camera.zoom,
                      origin.y + camera.y + positions[node].y * camera.zoom);
    };
    // Üzerine gelinen düğüm ve komşuları tam opak, ötekiler aramadaki eşleşmeyenler gibi soluk (G13).
    // Komşular yalnız üzerine gelinirken, kenar listesinden (doğrusal) bulunur.
    std::vector<char> near_hovered;
    if (hovered >= 0) {
        near_hovered.assign(map.nodes.size(), 0);
        near_hovered[static_cast<std::size_t>(hovered)] = 1;
        for (const MapEdge& edge : map.edges) {
            if (static_cast<int>(edge.from) == hovered)
                near_hovered[edge.to] = 1;
            else if (static_cast<int>(edge.to) == hovered)
                near_hovered[edge.from] = 1;
        }
    }
    // Haritada bul açıkken (F2) yalnız eşleşmeler yanar; maske yalnız o sırada, eşleşme listesinden.
    std::vector<char> found;
    if (map_find_open_ && !map_find_.empty()) {
        found.assign(map.nodes.size(), 0);
        for (const std::size_t node : map_found_)
            if (node < found.size())
                found[node] = 1;
    }
    // Sorgu vurgusu (F3): yalnız sonuçtaki düğümler ve kenarlar yanar (app::MapModel::highlighted_*).
    const std::vector<char>& query_nodes = state_.map_highlighted_nodes();
    const std::vector<char>& query_edges = state_.map_highlighted_edges();
    const bool querying = query_nodes.size() == map.nodes.size() && query_edges.size() == map.edges.size() &&
                          state_.map_query().outcome().has_value();
    const auto lit = [&](std::size_t node) {
        return (!filtering || matches[node] != 0) && (near_hovered.empty() || near_hovered[node] != 0) &&
               (found.empty() || found[node] != 0) && (!querying || query_nodes[node] != 0);
    };
    const auto edge_lit = [&](const MapEdge& edge) {
        const auto index = static_cast<std::size_t>(&edge - map.edges.data());
        return lit(edge.from) && lit(edge.to) && (!querying || query_edges[index] != 0) &&
               (hovered < 0 || static_cast<int>(edge.from) == hovered ||
                static_cast<int>(edge.to) == hovered);
    };
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float spacing = GraphLayout::kIdealEdge * camera.zoom;
    const bool all_labels = spacing >= kMapLabelSpacingEms * font;
    const bool arrows = spacing >= kMapArrowSpacingEms * font;
    const auto node_radius = [&](std::size_t node) {
        return drawn_radius(map.nodes[node].kind == MapNodeKind::Tag, map_cache_.sizes[node], camera.zoom,
                            font);
    };

    // Kenar türü başına (MapEdgeKind sırası) iki toplu çizgi: aramada eşleşen ve soluk.
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const float edge_alpha = theme::current().dark ? kMapEdgeAlphaDark : kMapEdgeAlphaLight;
    const std::array<ImU32, 3> kind_colors = {
        with_alpha(text, edge_alpha), with_alpha(icon_color(core::IconColor::Magenta), kMapRelatedAlpha),
        ImGui::GetColorU32(ImGuiCol_TextDisabled, edge_alpha)};
    map_lines_.resize(2 * kind_colors.size());
    for (std::size_t kind = 0; kind < kind_colors.size(); ++kind) {
        map_lines_[2 * kind].color = kind_colors[kind];
        map_lines_[2 * kind + 1].color = with_alpha(kind_colors[kind], kMapDimAlpha);
    }
    for (MapLineBatch& batch : map_lines_)
        batch.coords.clear();  // kapasite kareler arası kalır
    const auto batch_of = [&](const MapEdge& edge) -> MapLineBatch& {
        return map_lines_[2 * static_cast<std::size_t>(edge.kind) + (edge_lit(edge) ? 0 : 1)];
    };
    const auto visible = [&](ImVec2 a, ImVec2 b) {
        return std::max(a.x, b.x) >= clip_min.x && std::min(a.x, b.x) <= clip_max.x &&
               std::max(a.y, b.y) >= clip_min.y && std::min(a.y, b.y) <= clip_max.y;
    };
    for (const MapEdge& edge : map.edges) {
        const ImVec2 a = screen(edge.from);
        const ImVec2 b = screen(edge.to);
        if (visible(a, b))
            batch_of(edge).coords.insert(batch_of(edge).coords.end(), {a.x, a.y, b.x, b.y});
    }
    draw->AddCallback(draw_map_lines, &map_lines_);  // düğüm ve oklardan önce: onlar çizgilerin üstünde
    // En kısa yol (F2): vurgu renginde kalın çizgi, ince kenarın üstünde. Yol kısadır: ImGui çizgisi yeter.
    const ImU32 accent = theme::current().accent;
    const float thick = kMapRingThickness * ui_scale_;
    const std::size_t path_edges = map.path.empty() ? 0 : std::min(map.edges.size(), map.path_edges.size());
    for (std::size_t e = 0; e < path_edges; ++e) {  // yol yokken kare başına kenar taraması yok
        if (map.path_edges[e] == 0)
            continue;
        const ImVec2 a = screen(map.edges[e].from);
        const ImVec2 b = screen(map.edges[e].to);
        if (visible(a, b))
            draw->AddLine(a, b, accent, thick);
    }
    for (std::size_t e = 0; querying && e < map.edges.size(); ++e) {  // F3: sorgunun döndürdüğü kenar (2)
        if (query_edges[e] != 2)
            continue;
        const ImVec2 a = screen(map.edges[e].from);
        const ImVec2 b = screen(map.edges[e].to);
        if (visible(a, b))
            draw->AddLine(a, b, accent, thick);
    }
    for (const MapEdge& edge : map.edges) {          // ok yalnız yakınken: görünen kenar az
        const bool pointed = edge.kind == MapEdgeKind::Link || (edge.kind == MapEdgeKind::Related && edge.directed);
        if (!arrows || !pointed)
            continue;
        const ImVec2 a = screen(edge.from);
        const ImVec2 b = screen(edge.to);
        if (visible(a, b))
            draw_arrow(draw, a, b, node_radius(edge.to), batch_of(edge).color);
    }

    const auto current = state_.current_path();
    const std::string current_key = current ? current->string() : std::string{};
    std::vector<char> on_path;  // yol yokken boş: kare başına ayırma yok
    if (!map.path.empty())
        on_path.assign(map.nodes.size(), 0);
    for (const std::size_t node : map.path)
        if (node < on_path.size())
            on_path[node] = 1;
    const std::optional<std::size_t> shown_match =
        !found.empty() && map_found_at_ < map_found_.size() ? std::optional(map_found_[map_found_at_]) : std::nullopt;
    std::vector<std::size_t> labelled;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        const ImVec2 at = screen(i);
        const float r = node_radius(i);
        if (at.x + r < clip_min.x || at.x - r > clip_max.x || at.y + r < clip_min.y || at.y - r > clip_max.y)
            continue;
        const bool tag = map.nodes[i].kind == MapNodeKind::Tag;
        ImU32 color = tag ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : icon_color(map_cache_.colors[i]);
        if (!lit(i))
            color = with_alpha(color, kMapDimAlpha);
        if (tag)
            draw->AddRectFilled(ImVec2(at.x - r, at.y - r), ImVec2(at.x + r, at.y + r), color);
        else
            draw->AddCircleFilled(at, r, color, 0);  // 0: ImGui'nin önbellekli tessellasyonu, küçükte az köşe
        const bool is_current = !tag && map.nodes[i].key == current_key;
        if (is_current || (!on_path.empty() && on_path[i] != 0))
            draw->AddCircle(at, r + kMapRingGapEms * font, accent, 0, thick);
        if (shown_match == i)
            draw->AddCircle(at, r + kMapFoundRingEms * font, accent, 0, 2.0F * thick);
        item_rects_[map_cache_.names[i]] = {at.x - r, at.y - r, at.x + r, at.y + r};
        // Sıkışık görünümde yalnız geçerli ve üstündeki not adlanır (bulunan eşleşme de): aramada eşleşenler
        // renkle seçilir.
        const bool priority = is_current || static_cast<int>(i) == hovered || shown_match == i;
        if (priority)
            labelled.insert(labelled.begin(), i);
        else if (all_labels)
            labelled.push_back(i);
    }
    // Shift+sürükleme ya da Bağla kipi: kaynaktan işaretçiye lastik çizgi, bırakılacak düğüm halkalı.
    const std::optional<std::size_t> link_source =
        map_link_from_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left) ? map_link_from_ : map_connect_from_;
    if (link_source && *link_source < map.nodes.size()) {
        const float thickness = kMapRingThickness * ui_scale_;
        draw->AddLine(screen(*link_source), ImGui::GetIO().MousePos, accent, thickness);
        if (hovered >= 0) {
            const auto target = static_cast<std::size_t>(hovered);
            draw->AddCircle(screen(target), node_radius(target) + kMapRingGapEms * font, accent, 0,
                            thickness);
        }
    }
    // Önce geçerli ve üstündeki not; başka bir adın üstüne binecek ad çizilmez (ipucunda, yakınlaşınca
    // görünür). ponytail: çizilen adlarla doğrusal karşılaştırma; tuvale sığan ad sayısı aralıkla sınırlı
    // (~yüz).
    // Etiketler ikincil boyda ve soluk; üzerine gelinen ve gösterilen not metin renginde (G13).
    ImGui::PushFont(nullptr, kSmallFontPixels);
    const float label_font = ImGui::GetFontSize();
    const ImU32 label_background = theme::with_alpha(theme::current().canvas, kMapLabelBackgroundAlpha);
    const ImU32 label_muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float max_width =
        std::clamp(spacing * kMapLabelSpacingShare, kMapLabelMinEms * font, kMapLabelMaxEms * font);
    std::vector<ImVec4> placed;  // (min x, min y, max x, max y)
    for (const std::size_t i : labelled) {
        const std::string& full = map_cache_.labels[i];
        const std::string label = text_width(full) > max_width ? keep_start(full, max_width) : full;
        const float width = text_width(label);
        const ImVec2 at = screen(i);
        // Kenardaki düğümün adı tuvalin içine kaydırılır, kırpılmaz.
        const float left =
            std::clamp(at.x - width / 2.0F, clip_min.x, std::max(clip_min.x, clip_max.x - width));
        const float top = at.y + node_radius(i) + kMapRingGapEms * font;
        const ImVec4 box(left, top, left + width, top + label_font);
        if (std::ranges::any_of(placed, [&](const ImVec4& other) {
                return box.x < other.z && other.x < box.z && box.y < other.w && other.y < box.w;
            }))
            continue;
        placed.push_back(box);
        draw->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.z, box.w), label_background);
        const bool emphasised = static_cast<int>(i) == hovered ||
                                (map.nodes[i].kind == MapNodeKind::Note && map.nodes[i].key == current_key);
        const ImU32 ink = emphasised ? text : label_muted;
        draw->AddText(ImVec2(box.x, box.y), lit(i) ? ink : with_alpha(ink, kMapDimAlpha), label.c_str());
    }
    // İlişkinin adı kenarın ortasında, yalnız adların okunduğu yakınlıkta (F1); başka ada binen çizilmez.
    if (all_labels) {
        const float name_width = kMapRelationNameMaxEms * font;
        for (std::size_t e = 0; e < map.edges.size() && e < map_cache_.edge_names.size(); ++e) {
            const std::string& full = map_cache_.edge_names[e];
            if (full.empty())
                continue;
            const ImVec2 a = screen(map.edges[e].from);
            const ImVec2 b = screen(map.edges[e].to);
            const ImVec2 middle((a.x + b.x) / 2.0F, (a.y + b.y) / 2.0F);
            if (middle.x < clip_min.x || middle.x > clip_max.x || middle.y < clip_min.y || middle.y > clip_max.y)
                continue;
            const std::string label = text_width(full) > name_width ? keep_start(full, name_width) : full;
            const float width = text_width(label);
            const ImVec4 box(middle.x - width / 2.0F, middle.y - label_font / 2.0F, middle.x + width / 2.0F,
                             middle.y + label_font / 2.0F);
            if (std::ranges::any_of(placed, [&](const ImVec4& other) {
                    return box.x < other.z && other.x < box.z && box.y < other.w && other.y < box.w;
                }))
                continue;
            placed.push_back(box);
            draw->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.z, box.w), label_background);
            const ImU32 ink = edge_lit(map.edges[e]) ? kind_colors[1] : with_alpha(kind_colors[1], kMapDimAlpha);
            draw->AddText(ImVec2(box.x, box.y), ink, label.c_str());
            remember_rect("map_edge_label:" + full, ImVec2(box.x, box.y), ImVec2(box.z, box.w));
        }
    }
    ImGui::PopFont();
    if (hovered >= 0 && ImGui::IsItemHovered()) {
        const MapNode& node = map.nodes[static_cast<std::size_t>(hovered)];
        ImGui::SetTooltip("%s", printable(node.kind == MapNodeKind::Tag ? "#" + node.key : node.key).c_str());
    } else if (ImGui::IsItemHovered()) {
        draw_relation_tooltip(map, origin);
    }
}

// İşaretçinin yakınındaki ilişki: adı (adsızsa "adsız") ve uçları yönüyle; her yakınlıkta (F1).
void View::draw_relation_tooltip(const NoteMap& map, ImVec2 origin) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MapCamera& camera = map_camera_;
    const app::LayoutPoint at{(mouse.x - origin.x - camera.x) / camera.zoom,
                              (mouse.y - origin.y - camera.y) / camera.zoom};
    const float reach = kMapEdgeHitEms * ImGui::GetFontSize() / camera.zoom;
    const auto hit = app::map_edge_at(map, state_.map_layout().positions(), at, reach);
    if (!hit || map.edges[*hit].kind != MapEdgeKind::Related)
        return;
    const MapEdge& edge = map.edges[*hit];
    const std::string name = *hit < map_cache_.edge_names.size() ? map_cache_.edge_names[*hit] : std::string();
    const std::string ends = map_label(map.nodes[edge.from]) + (edge.directed ? " → " : " — ") +
                             map_label(map.nodes[edge.to]);
    ImGui::SetTooltip("%s\n%s", (name.empty() ? core::tr("map.unnamed_relation") : name).c_str(), ends.c_str());
}

}  // namespace noteapp::gui
