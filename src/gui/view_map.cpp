#include "core/i18n.hpp"
#include "gui/view_internal.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>
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
constexpr float kMapEdgeAlpha = 0.35F;
constexpr float kMapRelatedAlpha = 0.8F;  // ilişki yönsüzdür: oksuz, renkli ve bağlantıdan belirgin
constexpr float kMapDimAlpha = 0.2F;      // aramada eşleşmeyen düğüm ve kenar
constexpr float kMapLabelBackgroundAlpha = 0.8F;
constexpr int kMapNodeSegments = 12;  // 2.000 dairede köşe sayısı sınırlı kalsın
constexpr float kMapDepthSliderEms = 7.0F;

ImU32 with_alpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

float checkbox_width(const std::string& label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + text_width(label);
}

void fit_camera(MapCamera& camera, const std::vector<LayoutPoint>& positions, ImVec2 size) {
    if (positions.empty())
        return;
    LayoutPoint low = positions.front();
    LayoutPoint high = positions.front();
    for (const LayoutPoint p : positions) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
    }
    const float margin = kMapFitMarginEms * ImGui::GetFontSize();
    const float room_x = std::max(size.x - 2.0F * margin, 1.0F);
    const float room_y = std::max(size.y - 2.0F * margin, 1.0F);
    const float span_x = std::max(high.x - low.x, GraphLayout::kIdealEdge);
    const float span_y = std::max(high.y - low.y, GraphLayout::kIdealEdge);
    camera.zoom = std::clamp(std::min(room_x / span_x, room_y / span_y), kMapMinZoom, kMapMaxZoom);
    camera.offset_x = size.x / 2.0F - (low.x + high.x) / 2.0F * camera.zoom;
    camera.offset_y = size.y / 2.0F - (low.y + high.y) / 2.0F * camera.zoom;
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
    ImGui::BeginChild("##map", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
        for (const MapNode& node : map.nodes) {
            const bool tag = node.kind == MapNodeKind::Tag;
            map_cache_.names.push_back((tag ? "map:#" : "map:") + node.key);
            map_cache_.labels.push_back(map_label(node));
            map_cache_.colors.push_back(tag ? core::IconColor::Default : core::file_icon(node.key).color);
        }
        map_camera_.auto_fit = true;  // çevre değişti: yeni düğümler görünsün
        map_pressed_.reset();
    }

    const auto& positions = state_.map_layout().positions();
    int hovered = -1;
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float font = ImGui::GetFontSize();
        const float reach = std::max(kMapNodeWorldRadius * map_camera_.zoom, kMapHitMinRadiusEms * font);
        float best = reach * reach;
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const float dx = origin.x + map_camera_.offset_x + positions[i].x * map_camera_.zoom - mouse.x;
            const float dy = origin.y + map_camera_.offset_y + positions[i].y * map_camera_.zoom - mouse.y;
            if (dx * dx + dy * dy <= best) {
                best = dx * dx + dy * dy;
                hovered = static_cast<int>(i);
            }
        }
    }
    handle_map_input(map, origin, size, hovered);
    map_animating_ = state_.map_layout().step(kMapIterationsPerFrame);
    if (map_camera_.auto_fit)
        fit_camera(map_camera_, positions, size);
    draw_map_graph(map, origin, hovered);
    ImGui::EndChild();
}

void View::draw_map_controls(const NoteMap& map) {
    MapOptions options = state_.map_options();
    bool changed = false;
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const std::string whole = core::tr("gui.map_whole");
    changed |= ImGui::Checkbox((whole + "###map_whole").c_str(), &options.whole_graph);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_whole_tip").c_str());
    remember_item_center("map_whole");

    const float depth_width = kMapDepthSliderEms * ImGui::GetFontSize();
    flow_after_previous(depth_width, row_start_x, row_width);
    ImGui::BeginDisabled(options.whole_graph);
    ImGui::SetNextItemWidth(depth_width);
    // Biçim katalogdan gelir ama içine yalnız "%d" konur: ImGui biçim dizgisi çevirmen metninden kurulmaz.
    const std::string depth_format = core::tr("gui.map_depth", {{"depth", "%d"}});
    changed |= ImGui::SliderInt("##map_depth", &options.depth, MapOptions::kMinDepth, MapOptions::kMaxDepth,
                                depth_format.c_str(), ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_depth_tip").c_str());
    remember_item_center("map_depth");
    ImGui::EndDisabled();

    const std::string tags = core::tr("gui.map_tags");
    flow_after_previous(checkbox_width(tags), row_start_x, row_width);
    changed |= ImGui::Checkbox((tags + "###map_tags").c_str(), &options.show_tags);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_tags_tip").c_str());
    remember_item_center("map_tags");
    if (changed)
        queue([this, options] { state_.set_map_options(options); });

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
    }
    const bool active = ImGui::IsItemActive();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        camera.auto_fit = false;
        if (map_pressed_) {
            state_.map_layout().pin(*map_pressed_,
                                    {(io.MousePos.x - origin.x - camera.offset_x) / camera.zoom,
                                     (io.MousePos.y - origin.y - camera.offset_y) / camera.zoom});
            map_dragged_ = true;
        } else {
            camera.offset_x += io.MouseDelta.x;
            camera.offset_y += io.MouseDelta.y;
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        camera.auto_fit = false;
        camera.offset_x += io.MouseDelta.x;
        camera.offset_y += io.MouseDelta.y;
    }
    if (ImGui::IsItemDeactivated()) {
        if (map_pressed_ && !map_dragged_) {
            if (const auto path = note_path(*map_pressed_)) {
                if (io.KeyCtrl)
                    queue_open(*path, true);
                else
                    queue([this, path = *path] { state_.open_map_note(path); });
            }
        }
        map_pressed_.reset();
    }
    if (hovered >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (const auto path = note_path(static_cast<std::size_t>(hovered))) {
            queue([this, path = *path] {
                state_.open_map_note(path);
                if (state_.current_path() == path)  // kayıttan vazgeçildiyse görünüm de kalır
                    state_.set_browser_view(BrowserView::Tree);
            });
        }
    }
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
            camera.offset_x += shift.x;
            camera.offset_y += shift.y;
        }
    }
    if (zoom_steps != 0)
        zoom_map(origin, ImVec2(origin.x + size.x / 2.0F, origin.y + size.y / 2.0F),
                 std::pow(kMapKeyZoomStep, static_cast<float>(zoom_steps)));
    if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))
        camera.auto_fit = true;
}

// `anchor` altındaki yerleşim noktası yerinde kalır.
void View::zoom_map(ImVec2 origin, ImVec2 anchor, float factor) {
    MapCamera& camera = map_camera_;
    const float zoom = std::clamp(camera.zoom * factor, kMapMinZoom, kMapMaxZoom);
    const float world_x = (anchor.x - origin.x - camera.offset_x) / camera.zoom;
    const float world_y = (anchor.y - origin.y - camera.offset_y) / camera.zoom;
    camera.offset_x = anchor.x - origin.x - world_x * zoom;
    camera.offset_y = anchor.y - origin.y - world_y * zoom;
    camera.zoom = zoom;
    camera.auto_fit = false;
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
        return ImVec2(origin.x + camera.offset_x + positions[node].x * camera.zoom,
                      origin.y + camera.offset_y + positions[node].y * camera.zoom);
    };
    const auto lit = [&](std::size_t node) { return !filtering || matches[node] != 0; };
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float spacing = GraphLayout::kIdealEdge * camera.zoom;
    const bool all_labels = spacing >= kMapLabelSpacingEms * font;
    const bool arrows = spacing >= kMapArrowSpacingEms * font;
    const float radius = std::max(kMapNodeWorldRadius * camera.zoom, kMapNodeMinRadiusEms * font);
    const auto node_radius = [&](std::size_t node) {
        return map.nodes[node].kind == MapNodeKind::Tag ? radius * kMapTagRadiusShare : radius;
    };

    // Kenar türü başına (MapEdgeKind sırası) iki toplu çizgi: aramada eşleşen ve soluk.
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const std::array<ImU32, 3> kind_colors = {
        with_alpha(text, kMapEdgeAlpha), with_alpha(icon_color(core::IconColor::Magenta), kMapRelatedAlpha),
        ImGui::GetColorU32(ImGuiCol_TextDisabled, kMapEdgeAlpha)};
    map_lines_.resize(2 * kind_colors.size());
    for (std::size_t kind = 0; kind < kind_colors.size(); ++kind) {
        map_lines_[2 * kind].color = kind_colors[kind];
        map_lines_[2 * kind + 1].color = with_alpha(kind_colors[kind], kMapDimAlpha);
    }
    for (MapLineBatch& batch : map_lines_)
        batch.coords.clear();  // kapasite kareler arası kalır
    const auto batch_of = [&](const MapEdge& edge) -> MapLineBatch& {
        return map_lines_[2 * static_cast<std::size_t>(edge.kind) + (lit(edge.from) && lit(edge.to) ? 0 : 1)];
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
    for (const MapEdge& edge : map.edges) {          // ok yalnız yakınken: görünen kenar az
        if (!arrows || edge.kind != MapEdgeKind::Link)
            continue;
        const ImVec2 a = screen(edge.from);
        const ImVec2 b = screen(edge.to);
        if (visible(a, b))
            draw_arrow(draw, a, b, node_radius(edge.to), batch_of(edge).color);
    }

    const auto current = state_.current_path();
    const std::string current_key = current ? current->string() : std::string{};
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
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
            draw->AddCircleFilled(at, r, color, kMapNodeSegments);
        const bool is_current = !tag && map.nodes[i].key == current_key;
        if (is_current)
            draw->AddCircle(at, r + kMapRingGapEms * font, accent, 0, kMapRingThickness * ui_scale_);
        item_rects_[map_cache_.names[i]] = {at.x - r, at.y - r, at.x + r, at.y + r};
        // Sıkışık görünümde yalnız geçerli ve üstündeki not adlanır: aramada eşleşenler renkle seçilir.
        const bool priority = is_current || static_cast<int>(i) == hovered;
        if (priority)
            labelled.insert(labelled.begin(), i);
        else if (all_labels)
            labelled.push_back(i);
    }
    // Önce geçerli ve üstündeki not; başka bir adın üstüne binecek ad çizilmez (ipucunda, yakınlaşınca
    // görünür). ponytail: çizilen adlarla doğrusal karşılaştırma; tuvale sığan ad sayısı aralıkla sınırlı
    // (~yüz).
    const ImU32 label_background = ImGui::GetColorU32(ImGuiCol_WindowBg, kMapLabelBackgroundAlpha);
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
        const ImVec4 box(left, top, left + width, top + font);
        if (std::ranges::any_of(placed, [&](const ImVec4& other) {
                return box.x < other.z && other.x < box.z && box.y < other.w && other.y < box.w;
            }))
            continue;
        placed.push_back(box);
        draw->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.z, box.w), label_background);
        draw->AddText(ImVec2(box.x, box.y), lit(i) ? text : with_alpha(text, kMapDimAlpha), label.c_str());
    }
    if (hovered >= 0 && ImGui::IsItemHovered()) {
        const MapNode& node = map.nodes[static_cast<std::size_t>(hovered)];
        ImGui::SetTooltip("%s", printable(node.kind == MapNodeKind::Tag ? "#" + node.key : node.key).c_str());
    }
}

}  // namespace noteapp::gui
