#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/commands.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/view_internal.hpp"
#include "pdf/markdown_blocks.hpp"

#include <algorithm>
#include <climits>
#include <imgui.h>
#include <imgui_internal.h>  // editör imleci (ImGuiInputTextState), IMGUI_TABLE_MAX_COLUMNS
#include <imgui_stdlib.h>
#include <string_view>

namespace noteapp::gui {

namespace {

// Editör altı paneller (bilgi, bağlantılar): en çok bu kadar satır, kalan alanın en çok bu payı; kalanı
// kaydırılır, editör yaşar.
constexpr std::size_t kPanelMaxRows = 8;
constexpr float kPanelMaxShare = 0.4F;
constexpr float kInfoPairGapEms = 1.5F;  // bilgi panelinde yan yana iki alan çifti arası

// Tek seferlik imleç/seçim: ImGui bunu yalnız etkin InputText'in kendi geri çağrısından kabul eder.
int editor_selection_callback(ImGuiInputTextCallbackData* data) {
    auto* pending = static_cast<std::optional<TextSelection>*>(data->UserData);
    if (*pending) {
        const int start = std::min(static_cast<int>((*pending)->start), data->BufTextLen);
        const int end = std::min(static_cast<int>((*pending)->end), data->BufTextLen);
        data->SetSelection(start, end);
        pending->reset();
    }
    return 0;
}

}  // namespace

void View::draw_right_pane() {
    ImGui::BeginGroup();
    if (state_.trash_view()) {  // çöp notu salt okunur önizlenir; sekmeler canlı notlarındır
        draw_trash_preview();
        ImGui::EndGroup();
        return;
    }
    draw_note_tabs();
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
    ImGui::BeginChild("##trash_preview", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
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
    const bool open = ImGui::CollapsingHeader(label.c_str());
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
    const bool open = ImGui::CollapsingHeader(label.c_str());
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

void View::draw_link_row(const core::LinkLine& line) {
    if (line.is_header) {
        const std::string title =
            core::tr(core::link_group_key(line.group), {{"count", std::to_string(line.index)}});
        ImGui::TextUnformatted(title.c_str());
        return;
    }
    const std::string raw = core::link_text(state_.links(), line);
    const std::string text = printable(raw);
    if (const core::NotePath* path = core::link_path(state_.links(), line)) {
        const bool clicked = plain_selectable(text, false, {}, state_.icon_style());
        if (new_tab_click(clicked))
            queue_open(*path, true);
        else if (clicked)
            queue([this, target = *path] { state_.open_link(target); });
        // İlişki (TUI `L` ilişkili satırda): sağ tık menüsünden onaylı kaldırma.
        if (line.group == core::LinkGroup::Related && ImGui::BeginPopupContextItem("##link_menu")) {
            if (ImGui::MenuItem(core::tr("gui.unlink").c_str()))
                queue([this, target = *path] { state_.unlink_note(target); });
            ImGui::EndPopup();
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
    const bool previewable = state_.previewable_type();
    const std::string toggle_label = core::tr(state_.showing_preview() ? "gui.edit" : "gui.preview");
    const float toggle_width = previewable ? button_width(toggle_label) + spacing : 0.0F;
    const float trailing = toggle_width + (combo_width > 0.0F ? combo_width + spacing : 0.0F);
    // Gizli karakter uyarısı yoldan önce yer alır: yol kısalır, uyarı kısalmaz.
    const std::string hidden_badge = state_.hidden_controls_label();
    const float badge_width = hidden_badge.empty() ? 0.0F : text_width(hidden_badge) + spacing;
    const float header_width = ImGui::GetContentRegionAvail().x - trailing - badge_width;
    ImGui::AlignTextToFramePadding();
    draw_fitted_text("header", state_.header(), header_width, keep_end);
    if (!hidden_badge.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
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
    if (previewable) {
        // Önizlenemeyen md (çok büyük ya da metin değil) düğmeyi kapalı gösterir, nedeni ipucunda.
        const bool can = state_.can_preview();
        const std::string tip =
            !can ? core::tr("gui.preview_unavailable",
                            {{"size", core::human_size(state_.view_limits().view_bytes.cap())}})
                 : core::tr(state_.showing_preview() ? "gui.edit_tip" : "gui.preview_tip");
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

void View::draw_editor(float reserved_height) {
    const std::size_t generation = state_.buffer_generation();
    // Program içinden gelen yeni içerikte ImGui'nin iç kopyası atılmalı (kimlik değişir), yoksa eski metni
    // tampona geri yazardı. Odaktaysa kimlik değişimi odağı düşürmesin.
    if (generation != drawn_buffer_generation_) {
        if (editor_active_ && pending_focus_ == FocusRequest::None)
            pending_focus_ = FocusRequest::Editor;
        drawn_buffer_generation_ = generation;
    }
    if (refocus_editor_) {
        refocus_editor_ = false;
        pending_focus_ = FocusRequest::Editor;
    }
    // Sekme başına kimlik: ImGui editörün kaydırmasını kimlikle tutar, sekmeye dönünce yerinde bulunur.
    ImGui::PushID(static_cast<int>(state_.active_tab()));
    ImGui::PushID(static_cast<int>(generation));
    if (pending_focus_ == FocusRequest::Editor) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;  // Tab odak gezmez, karakter ekler
    if (!state_.editable())
        flags |= ImGuiInputTextFlags_ReadOnly;
    // Geri çağrı yalnız gerektiğinde: ImGui her çağrıda tüm metni yedekler, 1 MB notta kare başına kopya
    // olurdu.
    ImGuiInputTextCallback callback = nullptr;
    std::optional<TextSelection> selection = state_.editor_selection();
    if (selection) {
        flags |= ImGuiInputTextFlags_CallbackAlways;
        callback = editor_selection_callback;
    }
    ImGui::PushFont(platform_->mono_font, 0.0F);
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= reserved_height;
    const bool edited =
        ImGui::InputTextMultiline("##editor", &state_.buffer_for_edit(), size, flags, callback, &selection);
    remember_item_center("editor");
    ImGui::PopFont();
    editor_active_ = ImGui::IsItemActive();
    if (const ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetItemID())) {
        int start = input->GetCursorPos();
        int end = start;
        if (input->HasSelection()) {
            start = std::min(input->GetSelectionStart(), input->GetSelectionEnd());
            end = std::max(input->GetSelectionStart(), input->GetSelectionEnd());
        }
        state_.remember_cursor(TextSelection{static_cast<std::size_t>(start), static_cast<std::size_t>(end)});
    }
    if (state_.editor_selection() && !selection)  // geri çağrı uyguladı
        state_.take_editor_selection();
    if (edited)
        state_.buffer_edited();
    ImGui::PopID();
    ImGui::PopID();
}

void View::draw_preview(float reserved_height) {
    editor_active_ = false;
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= reserved_height;
    // Kaydırma sekme başına: ImGui onu alt pencerenin kimliğiyle tutar.
    const std::string child = "##preview" + std::to_string(state_.active_tab());
    ImGui::BeginChild(child.c_str(), size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    // Önizleme odaktayken TUI REVIEW'un kaydırma tuşları (CommandScope::Preview); istek bir sonraki karede.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    state_.set_preview_rows(ImGui::GetWindowHeight() / line);
    if (ImGui::IsWindowFocused())
        run_scope_commands(CommandScope::Preview);
    if (const float lines = state_.take_preview_scroll(); lines != 0.0F)
        ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + (lines * line), 0.0F, ImGui::GetScrollMaxY()));
    if (state_.preview_kind() == app::PreviewKind::Markdown)
        draw_markdown_preview();
    else
        draw_data_preview();
    ImGui::EndChild();
    remember_item_center("preview");
}

void View::draw_markdown_preview() {
    PreviewCache& cache = *preview_;
    if (cache.source != state_.buffer()) {
        cache.source = state_.buffer();
        cache.blocks = pdf::parse_markdown(cache.source, {.links = true});
        cache.width = -1.0F;
    }
    if (cache.missing != state_.links().missing) {
        cache.missing = state_.links().missing;
        cache.width = -1.0F;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float font_size = ImGui::GetFontSize();
    if (width != cache.width || font_size != cache.font_size) {
        cache.layout = layout_preview(cache.blocks, platform_->preview_fonts, font_size, width,
                                      [this](std::string_view target) { return state_.resolves_link(target); });
        cache.width = width;
        cache.font_size = font_size;
    }
    const PreviewLayout& layout = cache.layout;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float top = ImGui::GetScrollY();
    const float bottom = top + ImGui::GetWindowHeight();
    draw_preview_layout(layout, origin, top, bottom);
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
    for (std::size_t index = 0; index < layout.block_spans.size(); ++index) {  // test kancası
        const auto [block_top, block_bottom] = layout.block_spans[index];
        if (block_bottom >= top && block_top <= bottom)
            item_rects_["preview_block:" + std::to_string(index)] = {origin.x, origin.y + block_top, origin.x + width,
                                                                    origin.y + block_bottom, false};
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(layout.size);
}

void View::draw_data_preview() {
    PreviewCache& cache = *preview_;
    const core::TuiLimits& limits = state_.view_limits();
    const app::PreviewKind kind = state_.preview_kind();
    const app::TableLimits table_limits{.rows = limits.table_rows.cap(),
                                        .columns = limits.table_columns.cap()};
    if (cache.data_source != state_.buffer() || cache.data_kind != kind ||
        cache.data_limits != table_limits) {
        cache.data_source = state_.buffer();
        cache.data_kind = kind;
        cache.data_limits = table_limits;
        cache.csv.reset();
        cache.json.reset();
        if (kind == app::PreviewKind::Csv)
            cache.csv = app::parse_csv(cache.data_source, table_limits);
        else
            cache.json = app::pretty_json(cache.data_source);
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
    ImGui::PushFont(platform_->mono_font, 0.0F);
    ImGui::TextUnformatted(cache.data_source.data(), cache.data_source.data() + cache.data_source.size());
    ImGui::PopFont();
}

// İlk satır başlıktır (başlık zemini). Hücre metni güvenilmezdir: `##` ve `%` yorumlanmaz, denetim baytı
// U+FFFD olur. ImGui tablosunun sütun tavanını aşan sütunlar TUI'nin sığmayan sütunu gibi söylenir.
void View::draw_csv_table(const app::CsvTable& table) {
    std::size_t stored = 0;
    for (const std::vector<std::string>& row : table.rows)
        stored = std::max(stored, row.size());
    const int columns = static_cast<int>(std::min<std::size_t>(stored, IMGUI_TABLE_MAX_COLUMNS));
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    if (columns > 0 && ImGui::BeginTable("##csv", columns, kFlags)) {
        const auto draw_row = [&](std::size_t index) {
            const std::vector<std::string>& cells = table.rows[index];
            ImGui::TableNextRow(index == 0 ? ImGuiTableRowFlags_Headers : ImGuiTableRowFlags_None);
            for (int column = 0; column < columns && static_cast<std::size_t>(column) < cells.size();
                 ++column) {
                ImGui::TableSetColumnIndex(column);
                const std::string text = printable(cells[static_cast<std::size_t>(column)]);
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
            }
        };
        draw_row(0);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min<std::size_t>(table.rows.size() - 1, INT_MAX)));
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                draw_row(static_cast<std::size_t>(row) + 1);
        ImGui::EndTable();
        remember_item_center("preview_table");
    }
    if (table.omitted_rows > 0)
        ImGui::TextDisabled(
            "%s", core::tr("gui.preview_rows_cut", {{"count", std::to_string(table.omitted_rows)}}).c_str());
    if (const std::size_t hidden = table.column_count - static_cast<std::size_t>(columns); hidden > 0)
        ImGui::TextDisabled("%s",
                            core::tr("gui.preview_columns_cut", {{"count", std::to_string(hidden)}}).c_str());
}

// Girinti iç içelik başına iki karakter, genişliğin yarısını geçmez (TUI ile aynı). Belirteçler editörün
// sözdizimi kuralıyla renklenir; yalnız görünen satırlar çizilir, fazlası limits.tui.view_lines'ta kesilir.
void View::draw_json_lines(const std::vector<app::JsonLine>& lines) {
    const std::size_t shown =
        std::min({lines.size(), state_.view_limits().view_lines.cap(), static_cast<std::size_t>(INT_MAX)});
    ImGui::PushFont(platform_->mono_font, 0.0F);
    const float indent_width = ImGui::CalcTextSize("  ").x;
    const float max_indent = ImGui::GetContentRegionAvail().x / 2.0F;
    const std::size_t line_bytes = state_.view_limits().highlight_line_bytes.cap();
    std::vector<tui::SyntaxSpan> spans;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const app::JsonLine& line = lines[static_cast<std::size_t>(index)];
            const float indent = std::min(static_cast<float>(line.depth) * indent_width, max_indent);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            spans.clear();
            tui::highlight_line(tui::SyntaxLanguage::Json, line.text, tui::kInitialSyntaxState, &spans,
                                line_bytes);
            draw_token_line(line.text, spans);
            remember_rect("preview_json:" + std::to_string(index), start,
                          ImVec2(start.x + ImGui::CalcTextSize(line.text.c_str()).x,
                                 start.y + ImGui::GetTextLineHeight()));
        }
    }
    ImGui::PopFont();
    if (lines.size() > shown)
        ImGui::TextDisabled(
            "%s",
            core::tr("gui.preview_lines_cut", {{"count", std::to_string(lines.size() - shown)}}).c_str());
}

}  // namespace noteapp::gui
