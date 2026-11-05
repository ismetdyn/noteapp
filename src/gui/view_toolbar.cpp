#include "core/file_icon_table.hpp"
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>  // ImTextCharToUtf8, GetInputTextState: arama kutusunun imleci ve kaydırması
#include <imgui_stdlib.h>
#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

// Araç çubuğunda eylem grupları arası ayraç payı.
constexpr float kToolbarGroupGapEms = 0.6F;
// Seçici açılır penceresi uzun etiket listesinde ekranı kaplamasın: bu yükseklikten sonra kayar.
constexpr float kPickerMaxHeightEms = 20.0F;

// Argüman renkleri TUI'ninkiler: anahtar camgöbeği, değer yeşil, bozuk değer kırmızı.

struct SearchKeys {
    GuiState* state;
    std::function<void(int)> on_arrow;
};

// Modelin bekleyen düzenlemesi (geçmiş, tamamlama, gölge) metin kutusuna: kutu odakta kalır.
void apply_search_edit(GuiState& state, ImGuiInputTextCallbackData* data) {
    if (auto edit = state.take_search_edit()) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, edit->text.data(), edit->text.data() + edit->text.size());
        data->CursorPos = static_cast<int>(edit->cursor);
        data->SelectionStart = data->SelectionEnd = data->CursorPos;
    }
}

// Metin ve imleç ImGui'nindir; model (tui::InputLine) her geri çağrıda eşitlenir. Açık listede yazı listeyi
// süzer, metne girmez; oklar listede gezer (TUI girdi satırı).
int search_callback(ImGuiInputTextCallbackData* data) {
    const auto* keys = static_cast<SearchKeys*>(data->UserData);
    GuiState& state = *keys->state;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (state.search_popup() == nullptr)
            return 0;
        std::array<char, 5> typed{};
        ImTextCharToUtf8(typed.data(), data->EventChar);
        state.search_key(tui::Key::Char, typed.data());
        return 1;  // metne girmez
    }
    apply_search_edit(state, data);
    state.sync_search_line(std::string_view(data->Buf, static_cast<std::size_t>(data->BufTextLen)),
                           static_cast<std::size_t>(data->CursorPos));
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const bool up = data->EventKey == ImGuiKey_UpArrow;
        if (state.search_popup() != nullptr)
            state.search_key(up ? tui::Key::Up : tui::Key::Down);
        else
            keys->on_arrow(up ? -1 : 1);
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        state.search_key(tui::Key::Tab);
    }
    apply_search_edit(state, data);
    return 0;
}

}  // namespace

// InputText'ten önce: bu tuşlar metin kutusuna gitmeden modele verilir (geri çağrıları yoktur). Düzenleme bu
// karenin geri çağrısında kutuya yazılır.
void View::search_line_keys() {
    if (!search_active_)
        return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
        state_.search_history_key(true);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
        state_.search_history_key(false);
    if (state_.search_popup() == nullptr) {
        // Metnin sonunda → gölge öneriyi alır; gölge yoksa imleç ImGui'de yürür.
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !state_.search_ghost().empty())
            state_.search_key(tui::Key::Right);
        return;
    }
    // Açık liste: Enter ekler, Backspace listenin süzgecini siler, Esc kapatır; kutu bu tuşları görmez.
    constexpr ImGuiInputFlags kOverText = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive;
    if (ImGui::Shortcut(ImGuiKey_Enter, kOverText) || ImGui::Shortcut(ImGuiKey_KeypadEnter, kOverText))
        state_.search_key(tui::Key::Enter);
    else if (ImGui::Shortcut(ImGuiKey_Backspace, kOverText | ImGuiInputFlags_Repeat))
        state_.search_key(tui::Key::Backspace);
    else if (escape_pressed_)
        state_.search_key(tui::Key::Escape);
}

void View::draw_search_row() {
    ImGui::PushID(static_cast<int>(state_.query_generation()));
    if (pending_focus_ == FocusRequest::Search) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
    }
    search_line_keys();
    std::function<void(int)> on_arrow = [this](int delta) {
        queue([this, delta] { state_.navigate(delta); });
    };
    SearchKeys keys{&state_, std::move(on_arrow)};
    const std::string hint = core::tr("gui.search_placeholder");
    const char* mode_key = "gui.mode_name";
    if (state_.search_mode() == app::SearchMode::Content)
        mode_key = "gui.mode_content";
    else if (state_.search_mode() == app::SearchMode::Global)
        mode_key = "gui.mode_global";
    const std::string mode_label = core::tr(mode_key);
    const std::string sort_label = core::tr("gui.sort");
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(
        -(button_width(mode_label) + button_width(sort_label) + 2.0F * style.ItemSpacing.x));
    // Completion bayrağı Tab'i odak gezintisinden alır: arama kutusunda Tab tamamlar (TUI). Metni kutu
    // saydam çizer, renkli argümanlarla draw_search_text çizer (imleç ve seçim ImGui'nin).
    constexpr ImGuiInputTextFlags kSearchFlags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackAlways |
        ImGuiInputTextFlags_CallbackCharFilter;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool entered = ImGui::InputTextWithHint("##search", hint.c_str(), &state_.query_buffer(),
                                                  kSearchFlags, search_callback, &keys);
    ImGui::PopStyleColor();
    remember_item_center("search");
    search_active_ = ImGui::IsItemActive();
    draw_search_text(state_.query());
    if (ImGui::IsItemEdited())
        queue([this] { state_.query_edited(); });
    if (entered && ImGui::GetIO().KeyCtrl)
        queue_open_selected_in_new_tab();
    else if (entered)
        queue([this] { state_.search_enter(); });
    ImGui::PopID();

    ImGui::SameLine();
    // Düğme odak almaz: arama kutusundan çıkmadan mod çevrilebilsin (Ctrl+Shift+M aynı işi görür).
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (button("mode", mode_label))
        queue_command(gui_command("search.mode"));
    ImGui::SameLine();
    if (button("sort", sort_label))
        queue_command(gui_command("sort"));
    ImGui::SetItemTooltip("%s", core::tr("gui.sort_tip").c_str());
    ImGui::PopItemFlag();
    draw_sort_menu();
    draw_search_popup();
}

// Metin kutusunun üstüne, ImGui'nin koyduğu yere: etkin kutuda yatay kaydırma (InputTextState) hesaba
// katılır. Argüman aralıkları TUI'nin search_spans'ı; gölge yalnız imleç sondayken ve liste kapalıyken.
void View::draw_search_text(const std::string& text) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 frame_min = ImGui::GetItemRectMin();
    const ImVec2 frame_max = ImGui::GetItemRectMax();
    const ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetItemID());
    const float scroll = search_active_ && input != nullptr ? input->Scroll.x : 0.0F;
    const ImVec2 origin(frame_min.x + style.FramePadding.x - scroll, frame_min.y + style.FramePadding.y);
    const float line_height = ImGui::GetTextLineHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(frame_min.x + style.FramePadding.x, frame_min.y),
                       ImVec2(frame_max.x - style.FramePadding.x, frame_max.y), true);
    const auto x_at = [&](std::size_t offset) {
        return origin.x + ImGui::CalcTextSize(text.data(), text.data() + offset).x;
    };
    const auto paint = [&](std::size_t begin, std::size_t end, ImU32 color) {
        draw->AddText(ImVec2(x_at(begin), origin.y), color, text.data() + begin, text.data() + end);
    };
    const ImU32 plain = ImGui::GetColorU32(ImGuiCol_Text);
    std::size_t at = 0;
    for (const tui::InputSpan& span : tui::search_spans(text)) {
        paint(at, span.begin, plain);
        const char* kind = "key";
        // Anahtar ve değer sözdizimi aileleriyle aynı: tip camgöbeği, değer yeşil.
        ImU32 color = theme::current().type;
        if (span.kind == tui::InputSpan::Kind::Value) {
            kind = "value";
            color = theme::current().string;
        } else if (span.kind == tui::InputSpan::Kind::Invalid) {
            kind = "invalid";
            color = theme::current().danger;
        }
        paint(span.begin, span.end, color);
        remember_rect(std::format("search_arg:{}:{}", kind, span.begin), ImVec2(x_at(span.begin), origin.y),
                      ImVec2(x_at(span.end), origin.y + line_height));
        at = span.end;
    }
    paint(at, text.size(), plain);
    if (search_active_ && state_.search_popup() == nullptr) {
        if (const std::string ghost = state_.search_ghost(); !ghost.empty()) {
            const ImVec2 start(x_at(text.size()), origin.y);
            draw->AddText(start, ImGui::GetColorU32(ImGuiCol_TextDisabled), ghost.data(),
                          ghost.data() + ghost.size());
            remember_rect("search_ghost", start,
                          ImVec2(start.x + ImGui::CalcTextSize(ghost.c_str()).x, start.y + line_height));
        }
    }
    draw->PopClipRect();
}

// TUI'nin Tab listesi: kutunun altında, odak almaz; tuşlar kutuda kalır (search_line_keys), fareyle seçilen
// aday kutuya yazılır. Kutu odağı bırakınca (fare listede değilse) liste kapanır.
void View::draw_search_popup() {
    const tui::InputLine::Popup* popup = state_.search_popup();
    if (popup != nullptr && !search_active_ && !search_popup_hovered_) {
        state_.close_search_popup();
        popup = nullptr;
    }
    search_popup_hovered_ = false;
    if (popup == nullptr)
        return;
    const ItemRect& anchor = item_rects_.at("search");
    ImGui::SetNextWindowPos(ImVec2(anchor.min_x, anchor.max_y));
    // Satır metni pencereyi genişletmez (seçici gibi): en az kutu kadar, en uzun aday sığsın.
    float widest = anchor.max_x - anchor.min_x;
    for (const std::string& item : popup->items)
        widest = std::max(widest, text_width(item) + (2.0F * ImGui::GetStyle().WindowPadding.x));
    ImGui::SetNextWindowSizeConstraints(ImVec2(widest, 0.0F), ImVec2(FLT_MAX, FLT_MAX));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    ImGui::Begin("##search_popup", nullptr, kFlags);
    if (popup->items.empty())
        ImGui::TextDisabled("%s", core::tr("gui.completion_none").c_str());
    for (std::size_t index = 0; index < popup->items.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        if (plain_selectable(popup->items[index], index == popup->selected))
            queue([this, index] { state_.pick_search_completion(index); });
        ImGui::PopID();
    }
    remember_rect("search_popup", ImGui::GetWindowPos(),
                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    search_popup_hovered_ = ImGui::IsWindowHovered();
    ImGui::End();
}

// TUI'nin `o` menüsü: alan seçimi ve ters çevirme; işaret şu anki alanda ve sıra ters çevrilmişse.
void View::draw_sort_menu() {
    struct SortChoice {
        const GuiCommand* command;
        std::optional<search::SortField> field;  // yoksa ters çevirme
    };
    static constexpr SortChoice kChoices[] = {
        {&gui_command("sort.name"), search::SortField::Name},
        {&gui_command("sort.updated"), search::SortField::Updated},
        {&gui_command("sort.created"), search::SortField::Created},
        {&gui_command("sort.size"), search::SortField::Size},
        {&gui_command("sort.reverse"), std::nullopt},
    };
    if (state_.take_sort_menu_request()) {
        const ItemRect& anchor = item_rects_.at("sort");
        ImGui::SetNextWindowPos(ImVec2(anchor.min_x, anchor.max_y));
        ImGui::OpenPopup("##sort_menu");
    }
    if (!ImGui::BeginPopup("##sort_menu"))
        return;
    const search::SortSpec& sort = state_.sort();
    for (const SortChoice& choice : kChoices) {
        const bool checked = choice.field ? sort.field == *choice.field
                                          : sort.descending != app::sort_for(sort.field).descending;
        const std::string label = core::tr(app::find_command(choice.command->id)->label_key) + "###" +
                                  std::string(choice.command->id);
        if (ImGui::MenuItem(label.c_str(), nullptr, checked))
            queue_command(*choice.command);
        remember_item_center("sort:" + std::string(choice.command->id));
    }
    ImGui::EndPopup();
}

// Etiket ve görünüm seçici (TUI'de `tag:`/`view:` Tab listesi). Açıklık durumdadır; dışarı tıklama ya da Esc
// ImGui'de kapatır, durum da kapanır.
void View::draw_picker() {
    const Picker kind = state_.picker();
    if (kind != Picker::None && !picker_shown_) {
        const ItemRect& anchor = item_rects_.at("search");
        ImGui::SetNextWindowPos(ImVec2(anchor.min_x, anchor.max_y));
        ImGui::OpenPopup("##picker");
    }
    const char* title_key = kind == Picker::View ? app::find_command("search.view")->label_key
                                                 : app::find_command("search.tag")->label_key;
    const std::string title = core::tr(title_key);
    // Satır metni çizim listesine yazılır, pencereyi genişletmez: en uzun etiket sığsın.
    float widest = text_width(title);
    for (const PickerItem& item : state_.picker_items())
        widest = std::max(widest, text_width(item.label));
    const ImGuiStyle& style = ImGui::GetStyle();
    const float min_width = widest + 2.0F * style.WindowPadding.x + style.ScrollbarSize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0F),
                                        ImVec2(FLT_MAX, kPickerMaxHeightEms * ImGui::GetFontSize()));
    if (!ImGui::BeginPopup("##picker")) {
        if (std::exchange(picker_shown_, false) && kind != Picker::None)
            queue([this] { state_.close_picker(); });
        return;
    }
    picker_shown_ = true;
    if (kind == Picker::None) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::Separator();
    if (state_.picker_items().empty())
        ImGui::TextUnformatted(
            core::tr(kind == Picker::Tag ? "gui.picker_no_tags" : "gui.picker_no_views").c_str());
    for (const PickerItem& item : state_.picker_items()) {
        ImGui::PushID(item.value.c_str());
        if (plain_selectable(item.label, false))
            queue([this, value = item.value] { state_.pick(value); });
        remember_item_center("picker:" + item.value);
        ImGui::PopID();
    }
    ImGui::EndPopup();
}

// K3: simgeler açıkken eylemler yalnız simgedir ("Yeni not" birincil düğme, simge ve adıyla); ad, kısayol ve
// kapalı düğmenin nedeni ipucundadır. Simgeler kapalıysa (`icons = off`) düğmeler adlarıyla kalır.
void View::draw_toolbar() {
    namespace icons = core::icon_table;
    struct ToolbarAction {
        const char* name{};
        const char* label_key{};
        const char* tooltip_key{};
        bool enabled{};
        const GuiCommand* command{};
        core::FileIcon icon{};
        bool starts_group = false;  // öncesine ayraç
        ButtonKind kind = ButtonKind::Ghost;
    };
    const ToolbarAction actions[] = {
        {"toolbar_new_note", "gui.toolbar_new_note", "gui.toolbar_new_note_tip", true, &gui_command("new"),
         icons::kNewNote, false, ButtonKind::Primary},
        {"toolbar_new_folder", "gui.new_folder", "gui.toolbar_new_folder_tip", true,
         &gui_command("folder.new"), icons::kNewFolder},
        {"toolbar_import", "gui.toolbar_import", "gui.toolbar_import_tip", true, &gui_command("import"),
         icons::kImport},
        {"toolbar_rename", "gui.rename_folder", "gui.toolbar_rename_tip", state_.can_rename(),
         &gui_command("rename"), icons::kRename, true},
        {"toolbar_move", "gui.toolbar_move", "gui.toolbar_move_tip", state_.can_move(), &gui_command("move"),
         icons::kMove},
        {"toolbar_delete", "gui.delete", "gui.toolbar_delete_tip", state_.can_delete(), &gui_command("trash"),
         icons::kDelete, false, ButtonKind::GhostDanger},
        {"toolbar_trash", nullptr, "gui.toolbar_trash_tip", true, &gui_command("go.trash"), icons::kTrash,
         true},
        {"toolbar_export", "gui.toolbar_export", "gui.toolbar_export_tip", state_.can_export(),
         &gui_command("export.pdf"), icons::kExportPdf},
    };
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetFontSize() * kToolbarGroupGapEms;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    bool first = true;
    // Fare içindir: her eylemin kısayolu ya da bağlam menüsü var; Tab sırası arama ile liste arasında
    // uzamasın.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const std::string trash_count = std::to_string(state_.trash_count());
    for (const ToolbarAction& action : actions) {
        const std::string text = action.label_key ? core::tr(action.label_key)
                                                  : core::tr("gui.toolbar_trash", {{"count", trash_count}});
        const std::string glyph = core::icon_text(action.icon, state_.icon_style());
        std::string label = text;
        if (!glyph.empty() && action.kind == ButtonKind::Primary)
            label = glyph + " " + text;
        else if (!glyph.empty())
            label =
                action.label_key ? glyph : glyph + " " + trash_count;  // çöpte sayı simgenin yanında kalır
        if (!first) {
            const float lead = action.starts_group ? gap + spacing : 0.0F;
            // Ayraç yalnız aynı satırda: alt satıra inen grup zaten ayrıdır.
            if (flow_after_previous(lead + button_width(label), row_start_x, row_width) && lead > 0.0F)
                draw_toolbar_separator(gap);
        }
        first = false;
        ImGui::BeginDisabled(!action.enabled);
        if (button(action.name, label, action.kind)) {
            if (action.command == &gui_command("import"))
                ImGui::OpenPopup("##import_menu");  // dosya mı klasör mü: yerel diyaloglar ayrıdır
            else
                queue_command(*action.command);
        }
        // Kapalı düğmede de neden ve kısayol görünsün; adı yazmayan düğmenin adı ipucunun başında.
        const std::string tip = core::tr(action.tooltip_key);
        if (label == text)
            ImGui::SetItemTooltip("%s", tip.c_str());
        else
            ImGui::SetItemTooltip("%s\n%s", text.c_str(), tip.c_str());
        ImGui::EndDisabled();
    }
    if (ImGui::BeginPopup("##import_menu")) {
        if (ImGui::MenuItem(core::tr("gui.import_files").c_str()))
            queue([this] { state_.request_import(GuiState::ImportRequest::Files); });
        remember_item_center("import:files");
        if (ImGui::MenuItem(core::tr("gui.import_folder").c_str()))
            queue([this] { state_.request_import(GuiState::ImportRequest::Folders); });
        remember_item_center("import:folders");
        ImGui::EndPopup();
    }
    draw_view_switch(row_start_x, row_width, gap);
    ImGui::PopItemFlag();
}

void View::draw_toolbar_separator(float gap) {
    ImGui::Dummy(ImVec2(gap, ImGui::GetFrameHeight()));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float x = std::round((min.x + max.x) / 2.0F);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y), ImVec2(x, max.y),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::SameLine();
}

// Görünüm seçici segmentli denetimdir: üç bitişik düğme tek zeminde; etkin segment seçili tonda, ötekiler
// soluk metinle. Ardından Ayarlar (⚙). İkisi sağa yaslı ve birlikte kalır: sığmazsa alt satıra inerler, yine
// sağda.
void View::draw_view_switch(float row_start_x, float row_width, float gap) {
    namespace icons = core::icon_table;
    struct ViewChoice {
        const char* name;
        const char* label_key;
        const char* tooltip_key;
        BrowserView view;
        const GuiCommand* command;
        core::FileIcon icon;
    };
    static constexpr ViewChoice kChoices[] = {
        {"toolbar_view_tree", "gui.view_tree", "gui.view_tree_tip", BrowserView::Tree,
         &gui_command("view.tree"), icons::kViewTree},
        {"toolbar_view_details", "gui.view_details", "gui.view_details_tip", BrowserView::Details,
         &gui_command("view.details"), icons::kViewDetails},
        {"toolbar_view_map", "gui.view_map", "gui.view_map_tip", BrowserView::Map, &gui_command("view.map"),
         icons::kViewMap},
    };
    const auto with_icon = [this](const core::FileIcon& icon, const std::string& text) {
        const std::string glyph = core::icon_text(icon, state_.icon_style());
        return glyph.empty() ? text : glyph + " " + text;
    };
    const std::string settings_glyph = core::icon_text(icons::kSettings, state_.icon_style());
    const std::string settings_label = settings_glyph.empty() ? core::tr("gui.tab_settings") : settings_glyph;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float segments = 0.0F;
    for (const ViewChoice& choice : kChoices)
        segments += button_width(with_icon(choice.icon, core::tr(choice.label_key)));
    const float trailing = segments + spacing + button_width(settings_label);
    flow_after_previous(gap + spacing + trailing, row_start_x, row_width);
    const ImVec2 start(std::max(ImGui::GetCursorScreenPos().x, row_start_x + row_width - trailing),
                       ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(start);
    const theme::Theme& colors = theme::current();
    ImGui::GetWindowDrawList()->AddRectFilled(start,
                                              ImVec2(start.x + segments, start.y + ImGui::GetFrameHeight()),
                                              colors.control, ImGui::GetStyle().FrameRounding);
    bool first = true;
    for (const ViewChoice& choice : kChoices) {
        if (!first)
            ImGui::SameLine(0.0F, 0.0F);
        first = false;
        const bool active = state_.browser_view() == choice.view;
        // Doygun ButtonActive yok: etkin segment seçili satırın tonudur.
        ImGui::PushStyleColor(ImGuiCol_Button, active ? colors.selected : 0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? colors.selected : colors.control_hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? colors.selected : colors.control_active);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? colors.text : colors.muted);
        if (button(choice.name, with_icon(choice.icon, core::tr(choice.label_key))))
            queue_command(*choice.command);
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("%s", core::tr(choice.tooltip_key).c_str());
    }
    ImGui::SameLine();
    if (button("settings_open", settings_label, ButtonKind::Ghost))
        queue_command(gui_command("settings"));
    ImGui::SetItemTooltip("%s", core::tr("gui.settings_open_tip").c_str());
}

}  // namespace noteapp::gui
