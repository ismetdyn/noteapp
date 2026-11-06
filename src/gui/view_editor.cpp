#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "gui/commands.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/view_internal.hpp"
#include "pdf/markdown_blocks.hpp"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>  // IMGUI_TABLE_MAX_COLUMNS
#include <optional>
#include <string_view>

namespace noteapp::gui {

namespace {

// Editör altı paneller (bilgi, bağlantılar): en çok bu kadar satır, kalan alanın en çok bu payı; kalanı
// kaydırılır, editör yaşar.
constexpr std::size_t kPanelMaxRows = 8;
constexpr float kPanelMaxShare = 0.4F;
constexpr float kInfoPairGapEms = 1.5F;  // bilgi panelinde yan yana iki alan çifti arası
constexpr float kPreviewMeasureEms = 46.0F;

// Ham önizlemede bir görüntü satırının en çok baytı. Uzun satır (base64, küçültülmüş JSON) bu boyda parçalara
// bölünür, hiçbir bayt düşmez; ImGui görünen satırı her karede baştan ölçtüğü için kare işi satır boyuyla
// değil bununla sınırlıdır (20 MiB'lık tek satır parçasız ~80 ms/kare). Kapasite sınırı değil, çizim parçası.
constexpr std::size_t kRawRowBytes = 4096;
constexpr std::size_t kRawHookBytes = 32;  // test kancası adına giren satır başı

// Ham metnin görüntü satırları sırayla: '\n' satır sonu, uzun satır kod noktası sınırında kRawRowBytes'lık
// parçalar. Boş satır da bir satırdır.
class RawRows {
public:
    explicit RawRows(std::string_view text) : rest_(text) {}

    std::optional<std::string_view> next() {
        if (line_.empty()) {
            if (done_)
                return std::nullopt;
            const std::size_t eol = rest_.find('\n');
            line_ = rest_.substr(0, eol);
            if (eol == std::string_view::npos)
                done_ = true;
            else
                rest_.remove_prefix(eol + 1);
            if (line_.empty())
                return std::string_view{};
        }
        std::size_t cut = std::min(line_.size(), kRawRowBytes);
        while (cut < line_.size() && cut > 0 && (static_cast<unsigned char>(line_[cut]) & 0xC0) == 0x80)
            --cut;
        if (cut == 0)  // geçerli UTF-8'de olmaz (önizleme düzenlenebilir metindedir); yine de ilerle
            cut = std::min(line_.size(), kRawRowBytes);
        const std::string_view row = line_.substr(0, cut);
        line_.remove_prefix(cut);
        return row;
    }

private:
    std::string_view rest_;
    std::string_view line_;
    bool done_ = false;
};

// Sessiz bölüm başlığı (G6): çerçevesiz, seçili satırın zeminini almaz; üstünde 1 px ayraç. Yüksekliği
// CollapsingHeader'ınki (FramePadding): panel payları (panel_height) değişmez.
bool quiet_header(const std::string& label) {
    constexpr ImGuiTreeNodeFlags kFlags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                          ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                          ImGuiTreeNodeFlags_FramePadding;
    const bool open = ImGui::TreeNodeEx(label.c_str(), kFlags);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float y = std::floor(min.y - (ImGui::GetStyle().ItemSpacing.y / 2.0F));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    return open;
}

}  // namespace

void View::draw_right_pane(float canvas_left) {
    // İçerik tuval tonundadır (sekme şeridi kromda kalır, etkin sekme tuvalle birleşir); zemin kutudan önce
    // çizilir, öğeler ve çocuk pencereler üstüne gelir.
    const auto fill_canvas = [canvas_left] {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 room = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(canvas_left, at.y), ImVec2(at.x + room.x, at.y + room.y), theme::current().canvas);
    };
    ImGui::BeginGroup();
    if (state_.trash_view()) {  // çöp notu salt okunur önizlenir; sekmeler canlı notlarındır
        fill_canvas();
        draw_trash_preview();
        ImGui::EndGroup();
        return;
    }
    draw_note_tabs();
    fill_canvas();
    if (state_.selected_folder()) {  // klasör satırı: düzenlenecek not yok, özet
        draw_folder_summary();
        ImGui::EndGroup();
        return;
    }
    draw_header_row();
    const float available = ImGui::GetContentRegionAvail().y;
    const float links_height = panel_height(state_.link_lines().size(), state_.links_open(), available);
    const InfoColumns info = info_columns();
    const std::size_t info_rows = (state_.note_info().size() + info.pairs - 1) / info.pairs;
    const float info_height = panel_height(info_rows, state_.info_open(), available - links_height);
    if (state_.showing_preview())
        draw_preview(links_height + info_height);
    else
        draw_editor(links_height + info_height);
    if (info_height > 0.0F) {
        // Son panel kalan alanı alır; bağlantılar altta duruyorsa bilgi listesi kendi payıyla sınırlanır
        // (başlık ve iki öğe aralığı payın içinde).
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        draw_info_panel(info,
                        links_height > 0.0F ? info_height - ImGui::GetFrameHeightWithSpacing() - spacing : 0.0F);
    }
    if (links_height > 0.0F)
        draw_links_panel();
    ImGui::EndGroup();
}

// Seçili çöp notu: yol, bilgi açıksa atılma anı ve boyut, altında metin (değiştirilemez; geri getirilince
// düzenlenir).
void View::draw_trash_preview() {
    const int row = state_.trash_row();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::current().canvas);  // zemin Begin'de çizilir
    ImGui::BeginChild("##trash_preview", ImVec2(0.0F, 0.0F), theme::kPaneChildFlags,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    if (row >= 0) {
        const core::TrashItem& item = state_.trash_items()[static_cast<std::size_t>(row)];
        ImGui::TextDisabled("%s", printable(item.path.string()).c_str());
        if (state_.info_open())
            ImGui::TextDisabled("%s",
                                core::tr("gui.trash_info", {{"when", core::local_time_text(item.trashed_at)},
                                                            {"size", core::human_size(item.size)}})
                                    .c_str());
        ImGui::Separator();
        const std::string& text = state_.trash_preview();
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    } else {
        ImGui::TextUnformatted(core::tr("gui.trash_empty_hint").c_str());
    }
    ImGui::EndChild();
    remember_item_center("trash_preview");
}

// Editör payı önce: panel içeriği ne kadar uzun olursa olsun yükseklik sınırlıdır, kalanı kaydırılır.
float View::panel_height(std::size_t lines, bool open, float available_height) const {
    const float room = available_height * kPanelMaxShare;
    const float header = ImGui::GetFrameHeightWithSpacing();
    // Bileşikçi en küçük pencereyi uygulamayabilir: sığmayan panel çizilmez, başlık negatif genişlik almaz.
    if (lines == 0 || ImGui::GetContentRegionAvail().x <= 0.0F || room < header)
        return 0.0F;
    if (!open)
        return header;
    const float rows = static_cast<float>(std::min(lines, kPanelMaxRows));
    const float list =
        (rows * ImGui::GetTextLineHeightWithSpacing()) + (2.0F * ImGui::GetStyle().WindowPadding.y);
    return std::min(header + list, room);
}

// Alanlar (etiket, değer) çiftleri sığarsa yan yana ikişer: yedi alan dört satıra iner, editöre yer kalır.
View::InfoColumns View::info_columns() const {
    InfoColumns columns;
    for (const InfoField& field : state_.note_info()) {
        columns.label = std::max(columns.label, text_width(field.label));
        columns.value = std::max(columns.value, text_width(printable(field.value)));
    }
    columns.value += ImGui::GetFontSize() * kInfoPairGapEms;  // çiftler arası boşluk
    const float cell_padding = 2.0F * ImGui::GetStyle().CellPadding.x;
    const float pair = columns.label + columns.value + (2.0F * cell_padding);
    columns.pairs = ImGui::GetContentRegionAvail().x >= 2.0F * pair ? 2 : 1;
    return columns;
}

// list_height 0: panel en alttadır, kalan alanın tümünü alır.
void View::draw_info_panel(const InfoColumns& columns, float list_height) {
    const std::string label = core::tr("gui.info_title") + "###info";
    ImGui::SetNextItemOpen(state_.info_open());
    const bool open = quiet_header(label);
    ImGui::SetItemTooltip("%s", core::tr("gui.info_hint").c_str());
    remember_item_center("info_header");
    if (open != state_.info_open())
        queue([this, open] { state_.set_info_open(open); });
    if (!open)
        return;
    const float height = list_height > 0.0F ? std::max(list_height, ImGui::GetTextLineHeightWithSpacing())
                                            : ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##info_list", ImVec2(0.0F, height));
    if (ImGui::BeginTable("##info", static_cast<int>(2 * columns.pairs), ImGuiTableFlags_SizingFixedFit)) {
        for (std::size_t pair = 0; pair < columns.pairs; ++pair) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, columns.label);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthFixed, columns.value);
        }
        const auto& fields = state_.note_info();
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto column = static_cast<int>(2 * (index % columns.pairs));
            if (column == 0)
                ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(column);
            ImGui::TextDisabled("%s", fields[index].label.c_str());
            ImGui::TableSetColumnIndex(column + 1);
            ImGui::TextUnformatted(printable(fields[index].value).c_str());  // kaynak cihaz adı senkrondan gelir
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    remember_item_center("info_list");
}

void View::draw_links_panel() {
    const std::size_t items =
        state_.link_lines().size() -
        static_cast<std::size_t>(std::ranges::count_if(state_.link_lines(), &core::LinkLine::is_header));
    const std::string label = core::tr("gui.links_title", {{"count", std::to_string(items)}}) + "###links";
    ImGui::SetNextItemOpen(state_.links_open());
    const bool open = quiet_header(label);
    ImGui::SetItemTooltip("%s", core::tr("gui.links_hint").c_str());
    remember_item_center("links_header");
    if (open != state_.links_open())
        queue([this, open] { state_.set_links_open(open); });
    if (!open)
        return;
    if (pending_focus_ == FocusRequest::Links) {  // links.focus: oklar bağlantı satırlarında yürür
        ImGui::SetNextWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    ImGui::BeginChild("##links_list", ImVec2(0.0F, ImGui::GetContentRegionAvail().y));
    ImGuiListClipper clipper;  // binlerce bağlantıda yalnız görünenler çizilir
    clipper.Begin(static_cast<int>(state_.link_lines().size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            draw_link_row(state_.link_lines()[static_cast<std::size_t>(row)]);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    remember_item_center("links_list");
}

// Gösterilen notun gözünden: "Yönü çevir" çıkanı gelen, geleni ve yönsüzü çıkan yapar (app::flipped_source).
void View::draw_relation_row_menu(const core::RelatedNote& selected) {
    const auto shown = state_.current_path();
    if (!shown)
        return;
    const core::RelatedNote relation = selected;  // panel yeniden yüklenince satır yok olur: kopya
    const core::NotePath from = *shown;
    if (ImGui::MenuItem((core::tr("map.rename_relation") + "###link_rename").c_str()))
        queue([this, from, relation] { state_.rename_relation(from, relation.path, relation.id, relation.name); });
    remember_item_center("link_rename");
    const bool undirected = relation.direction == core::RelationDirection::Undirected;
    const std::string flip =
        undirected ? core::tr("map.make_directed", {{"from", label_safe(printable(from.string()))}, {"to", label_safe(printable(relation.path.string()))}})
                   : core::tr("map.flip_relation");
    if (ImGui::MenuItem((flip + "###link_flip").c_str()))
        queue([this, from, relation] {
            state_.direct_relation(from, relation.path, relation.id,
                                   app::flipped_source(from, relation.path, relation.direction));
        });
    remember_item_center("link_flip");
    if (!undirected) {
        if (ImGui::MenuItem((core::tr("map.make_undirected") + "###link_undirect").c_str()))
            queue([this, from, relation] { state_.direct_relation(from, relation.path, relation.id, std::nullopt); });
        remember_item_center("link_undirect");
    }
    ImGui::Separator();
    if (ImGui::MenuItem((core::tr("gui.unlink") + "###link_unlink").c_str()))
        queue([this, relation] { state_.unlink_note(relation); });
    remember_item_center("link_unlink");
}

void View::draw_link_row(const core::LinkLine& line) {
    if (line.is_header) {
        const std::string title =
            core::tr(core::link_group_key(line.group), {{"count", std::to_string(line.index)}});
        ImGui::TextUnformatted(title.c_str());
        return;
    }
    const std::string raw = core::link_text(state_.links(), line);
    const bool related = line.group == core::LinkGroup::Related;
    // İlişki satırı yön oku ve adla (F1, TUI ile aynı metin); ad güvenilmez: terminal_safe.
    const std::string text =
        related ? tui::terminal_safe(app::related_row_text(state_.links().related[line.index])) : printable(raw);
    if (const core::NotePath* path = core::link_path(state_.links(), line)) {
        const bool clicked = plain_selectable(text, false, {}, state_.icon_style(), {.icon_path = raw});
        if (new_tab_click(clicked))
            queue_open(*path, true);
        else if (clicked)
            queue([this, target = *path] { state_.open_link(target); });
        // İlişki (TUI ilişkili satırda `n` `f` `u` `L`): sağ tık menüsü, hepsi satırın ilişkisinin kimliğiyle.
        if (related) {
            // Menünün kimliği ilişkinin kimliğini de taşır: panel açıkken yeniden okunup satırlar kayarsa
            // (senkron) menü başka ilişkiye geçmez, kapanır.
            const core::RelatedNote& relation = state_.links().related[line.index];
            const auto* id = reinterpret_cast<const char*>(relation.id.bytes.data());
            ImGui::PushID(id, id + relation.id.bytes.size());
            if (ImGui::BeginPopupContextItem("##link_menu")) {
                draw_relation_row_menu(relation);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        remember_item_center("link:" + raw);
        return;
    }
    // Çözülmemiş metin: tıklanacak not yok. Köşeli ayraçlar renksiz de ayırt ettirir.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    draw_fitted_text("link_missing:" + raw, "[[" + text + "]]", ImGui::GetContentRegionAvail().x, keep_start);
    ImGui::PopStyleColor();
}

void View::draw_header_row() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float combo_width = state_.type_enabled() ? ImGui::GetFontSize() * 8.0F : 0.0F;
    // İnceleme ↔ düzenleme düğmesi kayıtlı notta (TUI REVIEW / EDIT); taslak hep düzenlemededir.
    const bool toggles = state_.type_enabled();
    const bool reviewing = state_.reviewing();
    const std::string toggle_label =
        core::tr(reviewing ? "gui.edit" : (state_.previewable_type() ? "gui.preview" : "gui.review"));
    const float toggle_width = toggles ? button_width(toggle_label) + spacing : 0.0F;
    const float trailing = toggle_width + (combo_width > 0.0F ? combo_width + spacing : 0.0F);
    // Gizli karakter uyarısı yoldan önce yer alır: yol kısalır, uyarı kısalmaz.
    const std::string hidden_badge = state_.hidden_controls_label();
    const float badge_width = hidden_badge.empty() ? 0.0F : text_width(hidden_badge) + spacing;
    const float header_width = ImGui::GetContentRegionAvail().x - trailing - badge_width;
    ImGui::AlignTextToFramePadding();
    draw_fitted_text("header", state_.header(), header_width, keep_end);
    if (!hidden_badge.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::current().danger);
        draw_fitted_text("hidden_controls", hidden_badge, badge_width - spacing, keep_start);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", core::tr("gui.hidden_controls_tip").c_str());
    }
    if (!state_.type_enabled())
        return;
    const std::string current = state_.current_type();
    // Grup içinde SameLine'ın konumu pencereye göre değil grup başına göre kayar: sağa yaslama kalan alandan.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0F, ImGui::GetContentRegionAvail().x - (trailing - spacing)));
    if (toggles) {
        // Salt-okunur not (ikili, geçersiz UTF-8, sınır üstü) düzenlenemez: düğme kapalı, nedeni ipucunda.
        const bool can = !reviewing || state_.can_edit();
        const std::string tip = !can         ? core::tr("gui.edit_unavailable")
                                : reviewing  ? core::tr("gui.edit_tip")
                                : state_.previewable_type() ? core::tr("gui.preview_tip")
                                                            : core::tr("gui.review_tip");
        ImGui::BeginDisabled(!can);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // kısayolu var; Tab sırası uzamasın
        if (button("preview_toggle", toggle_label))
            queue([this] { state_.toggle_preview(); });
        ImGui::PopItemFlag();
        ImGui::SetItemTooltip("%s", tip.c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(combo_width);
    const bool type_open = ImGui::BeginCombo("##type", current.c_str());
    remember_item_center("type");
    if (type_open) {
        const auto choices = state_.type_choices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (plain_selectable(choices[i], choices[i] == current))
                queue([this, chosen = choices[i]] { state_.change_type(chosen); });
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void View::draw_preview(float reserved_height) {
    editor_active_ = false;
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= reserved_height;
    // Kaydırma sekme başına: ImGui onu alt pencerenin kimliğiyle tutar.
    const std::string child = "##preview" + std::to_string(state_.active_tab());
    // Tuval tonu: içerik kromdan tonla ayrılır; sözdizimi karşıtlığı tuvale göredir (theme_test).
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::current().canvas);  // zemin Begin'de çizilir
    ImGui::BeginChild(child.c_str(), size, theme::kPaneChildFlags, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    // İncelemeye dönüş (Esc, düğme) ve sekme geçişi odağı önizlemeye verir: `E`/Enter hemen çalışsın.
    if (pending_focus_ == FocusRequest::Preview || pending_focus_ == FocusRequest::Editor) {
        ImGui::SetWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    // Önizleme odaktayken TUI REVIEW'un tuşları (CommandScope::Preview: kaydırma, `E`/Enter düzenle); istek bir
    // sonraki karede.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    state_.set_preview_rows(ImGui::GetWindowHeight() / line);
    if (ImGui::IsWindowFocused())
        run_scope_commands(CommandScope::Preview);
    // Klavye isteği piksel olarak; ImGui onu sonraki karede uygular.
    float request = 0.0F;
    const float lines = state_.take_preview_scroll();
    if (lines != 0.0F) {
        request = std::clamp(ImGui::GetScrollY() + (lines * line), 0.0F, ImGui::GetScrollMaxY()) - ImGui::GetScrollY();
        ImGui::SetScrollY(ImGui::GetScrollY() + request);
    }
    preview_link_hovered_ = false;
    if (state_.preview_too_large())
        draw_raw_preview();
    else if (state_.preview_kind() == app::PreviewKind::Markdown)
        draw_markdown_preview(request, lines >= FLT_MAX);  // End: scroll_preview(FLT_MAX)
    else
        draw_data_preview();
    // Metne çift tık düzenlemeye geçer (bağlantı kendi tıkını alır).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !preview_link_hovered_ &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        queue([this] { state_.begin_edit(); });
    ImGui::EndChild();
    remember_item_center("preview");
}

void View::draw_markdown_preview(float request, bool to_end) {
    ImGui::PushFont(nullptr, kPreviewFontPixels);  // okuma metni bir kademe büyük; ölçek PushFont'ta
    PreviewCache& cache = *preview_;
    const app::MarkdownChunkLimits chunk_limits = app::markdown_chunk_limits_of(state_.view_limits());
    if (cache.source != state_.buffer() || cache.document.chunk_limits() != chunk_limits) {
        cache.source = state_.buffer();
        cache.document.reset(cache.source, chunk_limits);
    }
    if (cache.missing != state_.links().missing) {
        cache.missing = state_.links().missing;
        cache.document.relayout();
    }
    cache.document.set_highlight_line_bytes(state_.view_limits().highlight_line_bytes.cap());
    // Okuma ölçüsü (G10): satır en çok 46 em (UI boyu; 15 px'te ≈ 690 px, 65–75 karakter), geniş pencerede
    // ortalı; dar pencerede tam genişlik. Kod ve tablo bunu aşarsa yatay kaydırılır.
    const float available = ImGui::GetContentRegionAvail().x;
    const float width =
        std::min(available, kPreviewMeasureEms * kFontPixels * ImGui::GetStyle().FontScaleDpi);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((available - width) / 2.0F));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float scrolled = ImGui::GetScrollY();
    // Görünen yükseklik kaydırma çubuğu ve kenarlıksız (InnerRect): ImGui'nin en büyük kaydırması da ona göredir,
    // geniş kod bloğu yatay çubuk açınca End onun yüksekliği kadar eksik kalmasın.
    const float height = ImGui::GetCurrentWindow()->InnerRect.GetHeight();
    // Ölçülen parça yukarıdaki tahmini değiştirdiyse görünen metin yerinde kalsın: bu kare düzeltilmiş kaydırmayla
    // çizilir, ImGui'ye sonraki kare için bildirilir.
    const float top = cache.document.update(
        scrolled, height, fonts(), ImGui::GetFontSize(), width,
        [this](std::string_view target) { return state_.resolves_link(target); }, to_end,
        2.0F * ImGui::GetStyle().WindowPadding.y);
    if (top != scrolled)  // aynı karedeki klavye isteği düzeltmenin üstüne eklenir, kaybolmaz (End'de sonun kendisi)
        ImGui::SetScrollY(to_end ? top : top + request);
    const float bottom = top + height;
    const ImVec2 origin(start.x, start.y + scrolled - top);
    for (const PreviewDocument::Placed& placed : cache.document.visible(top, bottom)) {
        const ImVec2 chunk_origin(origin.x, origin.y + placed.top);
        draw_preview_layout(*placed.layout, chunk_origin, top - placed.top, bottom - placed.top);
        ImGui::PushID(static_cast<int>(placed.chunk));
        draw_preview_links(*placed.layout, chunk_origin, top - placed.top, bottom - placed.top);
        ImGui::PopID();
        if (!placed.first_block)
            continue;
        const auto& spans = placed.layout->block_spans;
        for (std::size_t index = 0; index < spans.size(); ++index) {  // test kancası
            const auto [block_top, block_bottom] = spans[index];
            if (placed.top + block_bottom >= top && placed.top + block_top <= bottom)
                item_rects_["preview_block:" + std::to_string(*placed.first_block + index)] = {
                    origin.x, chunk_origin.y + block_top, origin.x + width, chunk_origin.y + block_bottom, false};
        }
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(cache.document.size());
    ImGui::PopFont();
}

// Yerleşimin [top, bottom] aralığındaki bağlantıları tıklanır yapar; `origin` yerleşimin ekrandaki sol üstü.
void View::draw_preview_links(const PreviewLayout& layout, ImVec2 origin, float top, float bottom) {
    for (std::size_t index = 0; index < layout.hits.size(); ++index) {
        const PreviewHit& hit = layout.hits[index];
        if (hit.max.y < top || hit.min.y > bottom)
            continue;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + hit.min.x, origin.y + hit.min.y));
        ImGui::PushID(static_cast<int>(index));
        const bool clicked = ImGui::InvisibleButton("##link", ImVec2(hit.max.x - hit.min.x, hit.max.y - hit.min.y));
        const bool new_tab = new_tab_click(clicked);
        ImGui::PopID();
        const std::string target(hit.target);
        if (ImGui::IsItemHovered()) {
            preview_link_hovered_ = true;
            ImGui::SetMouseCursor(hit.wiki ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
            if (!hit.wiki)  // dış adres açılmaz (not güvenilmez): yalnız gösterilir
                ImGui::SetTooltip("%s", printable(target).c_str());
        }
        if (!hit.wiki)
            continue;
        remember_item_center("preview_link:" + target);
        if (clicked || new_tab)
            queue([this, target, new_tab] { state_.open_wiki_link(target, new_tab); });
    }
}

// limits.tui.view_bytes üstü (TUI REVIEW'un kuralı): biçimlenmez; nedeni ve ham metin. Ayrıştırma, yerleşim
// ve kopya yok: satırlar karede iki kez sayılır (toplam ve görünene kadar, memchr hızında), yalnız görünenler
// ölçülüp çizilir.
void View::draw_raw_preview() {
    const std::string limit = core::human_size(state_.view_limits().view_bytes.cap());
    ImGui::TextDisabled("%s", core::tr("gui.preview_too_large", {{"limit", limit}}).c_str());
    const std::string_view text = state_.buffer();
    std::size_t total = 0;
    for (RawRows counter(text); counter.next();)
        ++total;
    ImGui::PushFont(fonts().mono, 0.0F);
    // Satırlar tek metin bloğu gibi bitişik (TextUnformatted'in çok satırlı çizimi gibi).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
    RawRows rows(text);
    int index = 0;  // `rows`un sıradaki satırı
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(std::min<std::size_t>(total, INT_MAX)));
    while (clipper.Step()) {
        if (clipper.DisplayStart < index) {  // aralıklar artan sıradadır; değilse baştan say
            rows = RawRows(text);
            index = 0;
        }
        for (; index < clipper.DisplayStart; ++index)
            rows.next();
        for (; index < clipper.DisplayEnd; ++index) {
            const std::string_view row = rows.next().value_or(std::string_view{});
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (row.empty())
                ImGui::TextUnformatted("");
            else
                ImGui::TextUnformatted(row.data(), row.data() + row.size());
            // Test kancası: ImGui günlüğü açıkken kırpıcı kırpmaz, çizilen satırlar buradan görülür.
            remember_rect("preview_raw:" + std::string(row.substr(0, kRawHookBytes)), at, ImGui::GetItemRectMax());
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void View::draw_data_preview() {
    PreviewCache& cache = *preview_;
    const app::PreviewKind kind = state_.preview_kind();
    if (cache.data_source != state_.buffer() || cache.data_kind != kind) {
        cache.data_source = state_.buffer();
        cache.data_kind = kind;
        cache.csv.reset();
        cache.json.reset();
        if (kind == app::PreviewKind::Csv)
            cache.csv = app::index_csv(cache.data_source);
        else
            cache.json = app::index_json(cache.data_source);
    }
    if (cache.csv) {
        draw_csv_table(*cache.csv);
        return;
    }
    if (cache.json) {
        draw_json_lines(*cache.json);
        return;
    }
    // TUI gibi: ayrıştırılamayan not nedeniyle birlikte ham metin olarak okunur.
    ImGui::TextDisabled(
        "%s", core::tr(kind == app::PreviewKind::Csv ? "gui.preview_not_table" : "gui.preview_invalid_json")
                  .c_str());
    ImGui::PushFont(fonts().mono, 0.0F);
    ImGui::TextUnformatted(cache.data_source.data(), cache.data_source.data() + cache.data_source.size());
    ImGui::PopFont();
}

// İlk satır başlıktır (başlık zemini). Hücre metni güvenilmezdir: `##` ve `%` yorumlanmaz, denetim baytı
// U+FFFD olur. ImGui tablosunun sütun tavanını aşan sütunlar TUI'nin sığmayan sütunu gibi söylenir. Satırlar ve
// sütunlar sanaldır: yalnız görünen satırlar ayrıştırılır (app::csv_rows), görünmeyen sütunun hücresi çizilmez.
void View::draw_csv_table(const app::CsvIndex& index) {
    const core::TuiLimits& limits = state_.view_limits();
    const std::size_t stored = std::min(index.column_count, limits.table_columns.cap());
    const int columns = static_cast<int>(std::min<std::size_t>(stored, IMGUI_TABLE_MAX_COLUMNS));
    const std::size_t rows = std::min(index.row_count, limits.table_rows.cap());
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    if (columns > 0 && rows > 0 && ImGui::BeginTable("##csv", columns, kFlags)) {
        // Görünen satırlar tek seferde ayrıştırılır (en yakın dizin bloğundan bir tarama).
        const auto draw_rows = [&](std::size_t first, std::size_t count) {
            std::size_t row = first;
            for (const app::CsvRow& csv :
                 app::csv_rows(preview_->data_source, index, first, count, static_cast<std::size_t>(columns))) {
                ImGui::TableNextRow(row++ == 0 ? ImGuiTableRowFlags_Headers : ImGuiTableRowFlags_None);
                for (int column = 0; column < columns && static_cast<std::size_t>(column) < csv.cells.size();
                     ++column) {
                    if (!ImGui::TableSetColumnIndex(column))
                        continue;  // yatayda görünmeyen sütun
                    const std::string text = printable(csv.cells[static_cast<std::size_t>(column)]);
                    ImGui::TextUnformatted(text.data(), text.data() + text.size());
                }
            }
        };
        draw_rows(0, 1);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min<std::size_t>(rows - 1, INT_MAX)));
        while (clipper.Step())
            draw_rows(static_cast<std::size_t>(clipper.DisplayStart) + 1,
                      static_cast<std::size_t>(clipper.DisplayEnd - clipper.DisplayStart));
        ImGui::EndTable();
        remember_item_center("preview_table");
    }
    if (const std::size_t omitted = index.row_count - rows; omitted > 0)
        ImGui::TextDisabled("%s", core::tr("gui.preview_rows_cut", {{"count", std::to_string(omitted)}}).c_str());
    if (const std::size_t hidden = index.column_count - static_cast<std::size_t>(columns); hidden > 0)
        ImGui::TextDisabled("%s",
                            core::tr("gui.preview_columns_cut", {{"count", std::to_string(hidden)}}).c_str());
}

// Girinti iç içelik başına iki karakter, genişliğin yarısını geçmez (TUI ile aynı). Belirteçler editörün
// sözdizimi kuralıyla renklenir; yalnız görünen satırlar kurulur (app::json_lines) ve çizilir, fazlası
// limits.tui.view_lines'ta kesilir.
void View::draw_json_lines(const app::JsonIndex& index) {
    const std::size_t shown =
        std::min({index.line_count, state_.view_limits().view_lines.cap(), static_cast<std::size_t>(INT_MAX)});
    ImGui::PushFont(fonts().mono, 0.0F);
    const float indent_width = ImGui::CalcTextSize("  ").x;
    const float max_indent = ImGui::GetContentRegionAvail().x / 2.0F;
    const std::size_t line_bytes = state_.view_limits().highlight_line_bytes.cap();
    std::vector<tui::SyntaxSpan> spans;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown));
    while (clipper.Step()) {
        const std::vector<app::JsonLine> lines =
            app::json_lines(preview_->data_source, index, static_cast<std::size_t>(clipper.DisplayStart),
                            static_cast<std::size_t>(clipper.DisplayEnd - clipper.DisplayStart));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const app::JsonLine& line = lines[i];
            const int number = clipper.DisplayStart + static_cast<int>(i);
            const float indent = std::min(static_cast<float>(line.depth) * indent_width, max_indent);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            spans.clear();
            tui::highlight_line(tui::SyntaxLanguage::Json, line.text, tui::kInitialSyntaxState, &spans,
                                line_bytes);
            draw_token_line(line.text, spans);
            remember_rect("preview_json:" + std::to_string(number), start,
                          ImVec2(start.x + ImGui::CalcTextSize(line.text.c_str()).x,
                                 start.y + ImGui::GetTextLineHeight()));
        }
    }
    ImGui::PopFont();
    if (index.line_count > shown)
        ImGui::TextDisabled(
            "%s",
            core::tr("gui.preview_lines_cut", {{"count", std::to_string(index.line_count - shown)}}).c_str());
}

}  // namespace noteapp::gui
