#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/commands.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>  // IsKeyChordPressed: yineleme bayrağı
#include <imgui_stdlib.h>
#include <utility>

namespace noteapp::gui {

namespace {

// Gezgin satırı: derinlik başına girinti ve aç/kapa işaretinin yuvası (yazı boyu birimi).
constexpr float kTreeIndentEms = 1.0F;
constexpr float kTreeArrowEms = 1.0F;
// Sıra no sütunu: rakamlardan sonra ada kadar boşluk (yazı boyu birimi).
constexpr float kRowNumberGapEms = 0.5F;
// Ayrıntılar tablosu: ad sütununun en dar hâli; daha dar panelde tablo yatay kaydırılır.
// Boyut ve tarih sütunları içerik genişliğindedir, ad kalanı alır ve "…" ile kısalır; ad bundan daralacaksa
// tablo yatay kaydırılır (varsayılan panel genişliğinin altında).
constexpr float kDetailsNameMinEms = 2.0F;
constexpr std::size_t kDayKeyLength = 10;  // "2026-11-02": kısa tarih biçimi gün değişince yeniden ölçülür
constexpr const char* kNoValue = "—";
constexpr const char* kExpandedMark = "▾";
constexpr const char* kCollapsedMark = "▸";

// Satırın kalıcı adı (test kancası ve ImGui kimliği): sıra değil yol, liste değişince tıklama başka satıra
// kaymasın.
std::string row_item_name(const BrowserRow& row) {
    return (row.kind == BrowserRowKind::Folder ? "folder:" : "note:") + row.path;
}

std::string row_label(const BrowserRow& row) {
    return row.kind == BrowserRowKind::Folder && row.path.empty() ? core::tr("gui.all_notes") : row.name;
}

// Ayrıntılar'ın üst veri alanları (ad sütunundan sonra); hangileri görünür, DetailsColumn seçer.
enum class DetailsField : std::uint8_t { Size, Updated, Created, Sync, Origin };
constexpr const char* kDetailsFieldKeys[View::kDetailsFields] = {"gui.details_size", "gui.details_updated",
                                                                 "gui.details_created", "gui.details_sync",
                                                                 "gui.details_origin"};

// TUI'deki sağ sütunun karşılığı: boyut ve tarih üç sütundur (boyut, değiştirilme, oluşturulma).
std::span<const DetailsField> details_fields(DetailsColumn column) {
    static constexpr DetailsField kSizeAndDate[] = {DetailsField::Size, DetailsField::Updated,
                                                    DetailsField::Created};
    static constexpr DetailsField kSize[] = {DetailsField::Size};
    static constexpr DetailsField kUpdated[] = {DetailsField::Updated};
    static constexpr DetailsField kCreated[] = {DetailsField::Created};
    static constexpr DetailsField kSync[] = {DetailsField::Sync};
    static constexpr DetailsField kOrigin[] = {DetailsField::Origin};
    switch (column) {
    case DetailsColumn::SizeAndDate:
        return kSizeAndDate;
    case DetailsColumn::Size:
        return kSize;
    case DetailsColumn::Updated:
        return kUpdated;
    case DetailsColumn::Created:
        return kCreated;
    case DetailsColumn::Sync:
        return kSync;
    case DetailsColumn::Origin:
        return kOrigin;
    case DetailsColumn::None:
        break;
    }
    return {};
}

// Hücre metni; tarih kısa biçimde (bugünse saat). Kaynak cihaz adı senkrondan gelir: yazdırılabilir yapılır.
std::string details_cell(const BrowserRow& row, DetailsField field,
                         std::chrono::system_clock::time_point now) {
    const auto time_text = [now](const std::optional<std::filesystem::file_time_type>& when) {
        return when ? core::short_time_text(*when, now) : std::string(kNoValue);
    };
    switch (field) {
    case DetailsField::Size:
        return core::human_size(row.size);
    case DetailsField::Updated:
        return time_text(row.updated_at);
    case DetailsField::Created:
        return time_text(row.created_at);
    case DetailsField::Sync:
        return row.sync ? sync_state_text(*row.sync) : std::string(kNoValue);
    case DetailsField::Origin:
        return row.origin ? printable(*row.origin) : std::string(kNoValue);
    }
    return {};
}

core::FileIcon row_icon(const BrowserRow& row) {
    return row.kind == BrowserRowKind::Folder ? core::folder_icon(row.expanded) : core::file_icon(row.name);
}

}  // namespace

void View::draw_folder_menu(const std::string& folder) {
    // Bağlam menüsü sağ tıkla, klavyede Menü tuşu / Shift+F10 ile açılır (ImGui gezinti imlecindeki öğeye
    // bağlar).
    if (!ImGui::BeginPopupContextItem("##folder_menu"))
        return;
    if (ImGui::MenuItem(core::tr("gui.new_folder").c_str()))
        queue([this, folder] { state_.new_folder(folder); });
    if (!folder.empty()) {  // kök "Tümü" yeniden adlandırılamaz/silinemez
        if (ImGui::MenuItem(core::tr("gui.rename_folder").c_str()))
            queue([this, folder] { state_.rename_folder(folder); });
        if (ImGui::MenuItem(core::tr("gui.delete_folder").c_str()))
            queue([this, folder] { state_.delete_folder(folder); });
    }
    ImGui::EndPopup();
}

void View::draw_note_menu(const core::NotePath& path) {
    if (!ImGui::BeginPopupContextItem("##note_menu"))
        return;
    // Eylem o satıra uygulanır: önce seçilir; seçim tutmadıysa (kayıt iptal) satırın notuna dokunulmaz. Kayıt
    // satırı kaydırabildiğinden denetim yola göredir.
    const auto on_row = [this, &path](void (GuiState::*action)()) {
        queue([this, path, action] {
            state_.open_note(path);
            if (state_.current_path() == path)
                (state_.*action)();
        });
    };
    if (ImGui::MenuItem(core::tr("gui.move_note").c_str()))
        on_row(&GuiState::move_note);
    if (ImGui::MenuItem(core::tr("gui.trash_note").c_str()))
        on_row(&GuiState::trash_note);
    if (ImGui::MenuItem(core::tr("gui.delete_note").c_str()))
        on_row(&GuiState::delete_note);
    // Sonda: klavyeyle menüde gezenin alıştığı öğe sırası kaymasın.
    if (ImGui::MenuItem(core::tr("gui.open_in_new_tab").c_str()))
        queue_open(path, true);
    if (ImGui::MenuItem(core::tr(app::find_command("external")->label_key).c_str()))
        on_row(&GuiState::open_external);
    if (ImGui::MenuItem(core::tr(app::find_command("copy")->label_key).c_str()))
        on_row(&GuiState::copy_selected);
    ImGui::EndPopup();
}

// Kaydırılan listenin dışında: çok satırda da düğmeler görünür kalır.
void View::draw_trash_header() {
    const std::string trash_icon = core::icon_text(core::trash_icon(), state_.icon_style());
    ImGui::TextUnformatted(
        ((trash_icon.empty() ? "" : trash_icon + " ") + core::tr("gui.trash_title")).c_str());
    remember_item_center("trash_title");
    struct TrashAction {
        const char* name{};
        const char* label_key{};
        bool enabled{};
        const GuiCommand* command{};
    };
    // Kalıcı silme "delete" komutudur: çöp görünümünde delete_selected seçili öğeyi temizler.
    const TrashAction actions[] = {
        {"trash_restore", "gui.trash_restore", state_.trash_row() >= 0, &gui_command("restore")},
        {"trash_purge", "gui.trash_purge", state_.trash_row() >= 0, &gui_command("delete")},
        {"trash_empty", "gui.trash_empty", !state_.trash_items().empty(), &gui_command("trash.empty")},
        {"trash_back", "gui.trash_back", true, &gui_command("trash.back")},
    };
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const TrashAction& action : actions) {
        const std::string label = core::tr(action.label_key);
        if (!first)
            flow_after_previous(button_width(label), row_start_x, row_width);
        first = false;
        ImGui::BeginDisabled(!action.enabled);
        if (button(action.name, label))
            queue_command(*action.command);
        ImGui::EndDisabled();
    }
}

void View::draw_trash_rows(bool focus_row) {
    const auto& items = state_.trash_items();
    const int current = state_.trash_row();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(items.size()));
    if (current >= 0 && (current != scrolled_row_ || focus_row))
        clipper.IncludeItemByIndex(current);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            if (focus_row && row == current)
                ImGui::SetKeyboardFocusHere();
            const std::string path = items[static_cast<std::size_t>(row)].path.string();
            if (plain_selectable(path, row == current, {}, state_.icon_style(), nullptr,
                                 state_.is_marked(path, false)))
                queue([this, row] { state_.select_trash_row(row); });
            if (row == current && current != scrolled_row_) {
                ImGui::SetScrollHereY();
                scrolled_row_ = current;
            }
            ImGui::PopID();
        }
    }
}

void View::draw_note_list() {
    const bool trash = state_.trash_view();
    const bool browsing = state_.browsing();
    const bool details = browsing && state_.browser_view() == BrowserView::Details;
    if (trash)
        draw_trash_header();
    if (details)
        draw_details_path();
    draw_quick_add();
    // Ayrıntılar tablosu dar panelde yatay kaydırılır; diğer görünümlerde içerik zaten panel genişliğindedir.
    ImGui::BeginChild("##notes", ImVec2(0.0F, 0.0F), theme::kPaneChildFlags,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // Klavyeyle (arama kutusunda Shift+Tab, Enter) girilince gezinti imleci seçili satırda başlar; yoksa
    // imleç ve seçim ayrı satırlarda yürür, bağlam menüsü ile Del farklı notlara giderdi. Fareyle odak zaten
    // satırı seçer.
    const bool entered_by_keyboard = ImGui::IsWindowFocused() && !list_focused_ && !ImGui::IsAnyMouseDown();
    const bool focus_row = pending_focus_ == FocusRequest::List || entered_by_keyboard;
    if (pending_focus_ == FocusRequest::List) {
        ImGui::SetWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    // Listeye bu karede girildiyse (Enter ile) aynı tuş içeride ikinci kez işlenmesin.
    const bool keys_for_list = list_focused_ && ImGui::IsWindowFocused();
    list_focused_ = ImGui::IsWindowFocused();
    const std::size_t root_rows = details ? 0 : 1;  // ağaçta kök satırı hep vardır
    const bool empty = trash      ? state_.trash_items().empty()
                       : browsing ? state_.browser_rows().size() <= root_rows
                                  : state_.model().row_count() == 0;
    if (empty) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr(trash ? "gui.trash_empty_hint" : "gui.empty_list_hint").c_str());
        ImGui::PopTextWrapPos();
    }
    if (trash)
        draw_trash_rows(focus_row);
    else if (details)
        draw_details_rows(focus_row);
    else if (browsing)
        draw_tree_rows(focus_row);
    else
        draw_note_rows(focus_row);
    // Sayfa tuşları görünen satır kadar gider (komut durumu ölçemez).
    state_.set_page_rows(static_cast<int>(ImGui::GetWindowHeight() / ImGui::GetTextLineHeightWithSpacing()));
    if (keys_for_list) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter))
            queue_open_selected_in_new_tab();
        // List kapsamlı komutlar yalnız liste odaktayken: Del, harfler ve oklar editörde ve arama kutusunda
        // kendi işini görür.
        run_scope_commands(CommandScope::List);
    }
    ImGui::EndChild();
    remember_item_center("note_list");
}

void View::run_scope_commands(CommandScope scope) {
    for (const GuiCommand& command : kGuiCommands) {
        if (command.scope != scope)
            continue;
        const ImGuiInputFlags flags = command.repeats ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None;
        if (ImGui::IsKeyChordPressed(command.chord, flags)) {
            // Gezinme imleci yeni seçili satıra gelsin; önizlemedeki tuş odağı listeye çekmesin.
            if (scope == CommandScope::List)
                follow_selection_ = true;
            queue_command(command);
        }
    }
}

// TUI `a` satırı: listenin üstünde ad alanı; Enter oluşturur, Esc vazgeçer, hata altında kalır.
void View::draw_quick_add() {
    if (!state_.quick_add_open())
        return;
    if (pending_focus_ == FocusRequest::QuickAdd) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
        follow_selection_ = false;  // seçili satır odağı geri almasın (aynı karede sonra çizilir)
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string hint = core::tr("gui.quick_add_hint");
    const bool entered = ImGui::InputTextWithHint("##quick_add", hint.c_str(), &state_.quick_add_buffer(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    remember_item_center("quick_add");
    if (entered)
        queue([this] { state_.submit_quick_add(); });
    else if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        queue([this] { state_.cancel_quick_add(); });
    if (!state_.quick_add_error().empty())
        draw_error(state_.quick_add_error());
}

// Seçim değiştiyse (ya da klavyeyle gelindiyse) seçili satır çizilsin, gezinti imleci ona gelsin,
// görünmüyorsa kaydırılsın. Dönen değer: seçili satırın adı değişti mi.
bool View::begin_selected_row(int current, bool focus_row, ImGuiListClipper& clipper) {
    const auto& rows = state_.browser_rows();
    const bool changed =
        current >= 0 && row_item_name(rows[static_cast<std::size_t>(current)]) != scrolled_item_;
    if (current >= 0 && (changed || focus_row || follow_selection_))
        clipper.IncludeItemByIndex(current);
    return changed;
}

void View::end_selected_row(const std::string& name, bool changed) {
    const ItemRect& rect = item_rects_.at(name);
    if (changed) {
        // Fareyle tıklanan satır yerinde kalır: ortalamak ikinci tıklamayı başka satıra düşürürdü.
        if (!ImGui::IsRectVisible(ImVec2(rect.min_x, rect.min_y), ImVec2(rect.max_x, rect.max_y)))
            ImGui::SetScrollHereY();
        scrolled_item_ = name;
    }
}

void View::draw_tree_rows(bool focus_row) {
    const auto& rows = state_.browser_rows();
    const int current = state_.browser_row();
    ImGuiListClipper clipper;  // 5.000 satırda yalnız görünenler çizilir
    clipper.Begin(static_cast<int>(rows.size()));
    const bool changed = begin_selected_row(current, focus_row, clipper);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            const std::string name = row_item_name(row);
            ImGui::PushID(name.c_str());
            if (index == current && (focus_row || follow_selection_)) {
                ImGui::SetKeyboardFocusHere();
                follow_selection_ = false;
            }
            draw_tree_row(row, name, index == current);
            if (index == current)
                end_selected_row(name, changed);
            ImGui::PopID();
        }
    }
}

void View::draw_tree_row(const BrowserRow& row, const std::string& name, bool selected) {
    const bool folder = row.kind == BrowserRowKind::Folder;
    const bool toggles = folder && !row.path.empty() && row.has_children;
    ImVec2 position = ImGui::GetCursorScreenPos();
    const float row_end = position.x + ImGui::GetContentRegionAvail().x;
    const float font = ImGui::GetFontSize();
    // Derin ağaçta ad yok olmasın: girinti satırın yarısını geçmez.
    const float indent =
        std::min(static_cast<float>(row.depth) * font * kTreeIndentEms, (row_end - position.x) / 2.0F);
    // TUI sırası: girinti, sıra no, ok, ikon, ad.
    const float number_width = state_.row_numbers() ? row_number_width() : 0.0F;
    const float arrow_start = position.x + indent + number_width;
    const float arrow_end = arrow_start + font * kTreeArrowEms;
    const bool clicked = ImGui::Selectable("##row", selected);
    if (state_.is_marked(row.path, folder))
        draw_mark_background();
    const bool new_tab = !folder && new_tab_click(clicked);
    remember_item_center(name);
    const bool hovered = ImGui::IsItemHovered();
    const bool on_arrow = toggles && hovered && ImGui::GetMousePos().x < arrow_end;
    if (number_width > 0.0F && row.number > 0)
        draw_row_number("row_number:" + name, row.number, ImVec2(position.x + indent, position.y), number_width);
    if (toggles)
        ImGui::GetWindowDrawList()->AddText(ImVec2(arrow_start, position.y), ImGui::GetColorU32(ImGuiCol_Text),
                                            row.expanded ? kExpandedMark : kCollapsedMark);
    position.x = arrow_end;
    position.x += draw_icon(position, row_icon(row), state_.icon_style(), selected);
    if (draw_fitted_row_text(position, row_label(row), row_end - position.x, {}))
        full_text_tooltip(row.path);
    if (clicked || new_tab) {
        if (on_arrow)
            queue([this, path = row.path] { state_.toggle_expanded(path); });
        else if (folder)
            queue([this, path = row.path] { state_.select_folder(path); });
        else if (const auto path = core::NotePath::parse(row.path))
            queue_open(*path, new_tab);
    }
    if (toggles && hovered && !on_arrow && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        queue([this, path = row.path] { state_.toggle_expanded(path); });
    draw_row_menu(row);
}

// En büyük numaranın rakamları ve ada kadar boşluk; rakamlar orantılı fontta da aynı genişliktedir.
float View::row_number_width() {
    const std::size_t digits = std::to_string(std::max<std::size_t>(state_.max_row_number(), 1)).size();
    return (static_cast<float>(digits) * text_width("0")) + (ImGui::GetFontSize() * kRowNumberGapEms);
}

// Sağa yaslı, soluk (TUI'deki gibi); boşluk payı sağda kalır.
void View::draw_row_number(const std::string& name, std::size_t number, ImVec2 position, float width) {
    const std::string text = std::to_string(number);
    ImGui::LogText("%s ", text.c_str());  // çizim listesine yazılan metin ImGui günlüğüne (testler) de girsin
    const float text_x = position.x + width - (ImGui::GetFontSize() * kRowNumberGapEms) - text_width(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, position.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        text.c_str());
    item_rects_[name] = {text_x, position.y, text_x + text_width(text), position.y + ImGui::GetTextLineHeight(),
                         false};
}

void View::draw_row_menu(const BrowserRow& row) {
    if (row.kind == BrowserRowKind::Folder)
        draw_folder_menu(row.path);
    else if (const auto path = core::NotePath::parse(row.path))
        draw_note_menu(*path);
}

// Konum: Yukarı ve her ata için bir düğme (Dolphin'deki yol çubuğu). Kaydırılan listenin dışında kalır.
void View::draw_details_path() {
    const std::string& folder = state_.current_folder();
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float padding = 2.0F * ImGui::GetStyle().FramePadding.x;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::BeginDisabled(folder.empty());
    if (button("details_up", core::tr("gui.details_up")))
        queue([this, parent = core::parent_folder(folder)] { state_.enter_folder(parent); });
    ImGui::SetItemTooltip("%s", core::tr("gui.details_up_tip").c_str());
    ImGui::EndDisabled();
    std::vector<std::string> crumbs{""};
    for (std::size_t slash = folder.find('/'); !folder.empty(); slash = folder.find('/', slash + 1)) {
        crumbs.push_back(folder.substr(0, slash));
        if (slash == std::string::npos)
            break;
    }
    for (const std::string& crumb : crumbs) {
        std::string label = crumb.empty() ? core::tr("gui.all_notes") : core::last_component(crumb);
        if (button_width(label) > row_width)
            label = keep_start(label, row_width - padding);
        flow_after_previous(button_width(label), row_start_x, row_width);
        if (button("crumb:" + crumb, label))
            queue([this, crumb] { state_.enter_folder(crumb); });
        if (!crumb.empty())
            full_text_tooltip(crumb);
    }
    ImGui::PopItemFlag();
}

void View::draw_details_rows(bool focus_row) {
    const auto& rows = state_.browser_rows();
    const int current = state_.browser_row();
    const auto now = std::chrono::system_clock::now();
    const std::array<float, kDetailsFields>& widths = details_column_widths(rows, now);
    const std::span<const DetailsField> fields = details_fields(state_.details_column());
    // Sıra no açıksa ilk sütundur; diğer sütunların adları (details_header:N) yerinde kalır.
    const int first = state_.row_numbers() ? 1 : 0;
    const int columns = 1 + static_cast<int>(fields.size()) + first;
    const float number_width = first > 0 ? row_number_width() : 0.0F;
    // Sütun başına iki hücre payı ve aradaki çizgi.
    const float chrome = static_cast<float>(columns) * (2.0F * ImGui::GetStyle().CellPadding.x + 1.0F);
    float fixed = number_width + chrome;
    for (const DetailsField field : fields)
        fixed += widths[static_cast<std::size_t>(field)];
    const float width =
        std::max(ImGui::GetContentRegionAvail().x, fixed + ImGui::GetFontSize() * kDetailsNameMinEms);
    // Yeniden boyutlandırılamaz: ImGui sabit sütunu ancak böyle her karede verilen içerik genişliğinde tutar.
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##details", columns, kFlags, ImVec2(width, 0.0F)))
        return;
    if (first > 0)
        ImGui::TableSetupColumn(core::tr("gui.details_number").c_str(), ImGuiTableColumnFlags_WidthFixed,
                                number_width);
    ImGui::TableSetupColumn(core::tr("gui.details_name").c_str(), ImGuiTableColumnFlags_WidthStretch);
    for (const DetailsField field : fields)
        ImGui::TableSetupColumn(core::tr(kDetailsFieldKeys[static_cast<std::size_t>(field)]).c_str(),
                                ImGuiTableColumnFlags_WidthFixed, widths[static_cast<std::size_t>(field)]);
    // Başlık elle: sütun konumu test edilir; sabit sütunda kısalan başlığın tamamı ipucunda.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int column = 0; column < columns; ++column) {
        ImGui::TableSetColumnIndex(column);
        const std::string label = ImGui::TableGetColumnName(column);
        ImGui::TableHeader(label.c_str());
        remember_item_center("details_header:" +
                             (column < first ? std::string("number") : std::to_string(column - first)));
        if (text_width(label) > ImGui::GetItemRectSize().x)
            full_text_tooltip(label);
        else
            ImGui::SetItemTooltip("%s", core::tr("gui.columns_hint").c_str());
        ImGui::OpenPopupOnItemClick("##columns", ImGuiPopupFlags_MouseButtonRight);
    }
    draw_column_menu();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    const bool changed = begin_selected_row(current, focus_row, clipper);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            const std::string name = row_item_name(row);
            ImGui::TableNextRow();
            if (first > 0) {
                ImGui::TableSetColumnIndex(0);
                draw_row_number("details_number:" + row.path, row.number, ImGui::GetCursorScreenPos(),
                                number_width);
            }
            ImGui::TableSetColumnIndex(first);
            ImGui::PushID(name.c_str());
            if (index == current && (focus_row || follow_selection_)) {
                ImGui::SetKeyboardFocusHere();
                follow_selection_ = false;
            }
            draw_details_row(row, name, index == current, now);
            if (index == current)
                end_selected_row(name, changed);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void View::draw_details_row(const BrowserRow& row, const std::string& name, bool selected,
                            std::chrono::system_clock::time_point now) {
    const bool folder = row.kind == BrowserRowKind::Folder;
    ImVec2 position = ImGui::GetCursorScreenPos();
    const float cell_end = position.x + ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns);
    if (state_.is_marked(row.path, folder))
        draw_mark_background();
    const bool new_tab = !folder && new_tab_click(clicked);
    remember_item_center(name);
    const bool double_clicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    position.x += draw_icon(position, row_icon(row), state_.icon_style(), selected);
    if (draw_fitted_row_text(position, row.name, cell_end - position.x, {}))
        full_text_tooltip(row.path);
    if (clicked || new_tab) {
        if (folder)
            queue([this, path = row.path] { state_.select_folder(path); });
        else if (const auto path = core::NotePath::parse(row.path))
            queue_open(*path, new_tab);
    }
    if (folder && double_clicked)
        queue([this, path = row.path] { state_.enter_folder(path); });
    draw_row_menu(row);
    const int name_column = ImGui::TableGetColumnIndex();
    for (int column = 1; const DetailsField field : details_fields(state_.details_column())) {
        ImGui::TableSetColumnIndex(name_column + column);
        ImGui::TextUnformatted(details_cell(row, field, now).c_str());
        remember_item_center("details_cell:" + std::to_string(column++) + ":" + row.path);
    }
}

// Tüm satırların en geniş hücresi; yalnız satırlar, yazı boyu ya da gün değişince yeniden ölçülür (kare
// başına binlerce satır ölçülmesin, kaydırınca sütun oynamasın).
const std::array<float, View::kDetailsFields>&
View::details_column_widths(const std::vector<BrowserRow>& rows, std::chrono::system_clock::time_point now) {
    std::string day = core::local_time_text(std::chrono::clock_cast<std::chrono::file_clock>(now));
    day.resize(std::min(day.size(), kDayKeyLength));
    const std::size_t rebuilds = state_.browser_rebuilds();
    const float font = ImGui::GetFontSize();
    if (rebuilds == details_widths_rebuilds_ && font == details_widths_font_ && day == details_widths_day_)
        return details_widths_;
    details_widths_.fill(text_width(kNoValue));
    // Tüm alanlar ölçülür: sütun seçimi değişince yeniden ölçüm gerekmesin.
    for (const BrowserRow& row : rows)
        for (std::size_t field = 0; field < kDetailsFields; ++field)
            details_widths_[field] = std::max(details_widths_[field],
                                              text_width(details_cell(row, static_cast<DetailsField>(field), now)));
    details_widths_rebuilds_ = rebuilds;
    details_widths_font_ = font;
    details_widths_day_ = std::move(day);
    return details_widths_;
}

// Ayrıntılar başlığında sağ tık: sütun seçimi (TUI `column.*` komutları); işaret seçili olanda.
void View::draw_column_menu() {
    if (!ImGui::BeginPopup("##columns"))
        return;
    for (const auto& [column, name] : kDetailsColumnNames) {
        const std::string id = std::string("column.") + (column == DetailsColumn::SizeAndDate ? "size_date" : name);
        const app::CommandSpec* spec = app::find_command(id);
        if (spec == nullptr)
            continue;
        const auto command = std::ranges::find(kGuiCommands, spec->id, &GuiCommand::id);
        const std::string label = core::tr(spec->label_key) + "###" + id;
        if (ImGui::MenuItem(label.c_str(), nullptr, state_.details_column() == column))
            queue_command(*command);
        remember_item_center("columns:" + id);
    }
    ImGui::EndPopup();
}

void View::draw_folder_summary() {
    const auto summary = state_.folder_summary();
    ImGui::BeginChild("##folder_summary", ImVec2(0.0F, 0.0F), theme::kPaneChildFlags);
    if (summary) {
        const std::string icon = core::icon_text(core::folder_icon(true), state_.icon_style());
        const std::string title = summary->path.empty() ? core::tr("gui.all_notes") : summary->path;
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted((icon.empty() ? title : icon + " " + title).c_str());
        ImGui::TextUnformatted(
            core::tr("gui.folder_summary_counts", {{"notes", std::to_string(summary->notes)},
                                                   {"folders", std::to_string(summary->folders)}})
                .c_str());
        ImGui::PopTextWrapPos();
        draw_hint(core::tr("gui.folder_summary_hint"));
    }
    ImGui::EndChild();
    remember_item_center("folder_summary");
}

void View::draw_note_rows(bool focus_row) {
    const NoteListModel& model = state_.model();
    const int current = state_.current_row();
    const auto draw_row = [&](int row) {
        ImGui::PushID(row);
        if (focus_row && row == current)
            ImGui::SetKeyboardFocusHere();
        const app::ListRow& shown = model.row(row);
        const bool clicked =
            plain_selectable(model.display(row), row == current, model.highlights(row), state_.icon_style(),
                             shown.show_best_line && !shown.hits.empty() ? &shown.hits.front() : nullptr,
                             state_.is_marked(model.note_at(row).path.string(), false));
        remember_item_center("row:" + model.note_at(row).path.string());
        if (new_tab_click(clicked))
            queue_open(model.note_at(row).path, true);
        else if (clicked)
            queue([this, row] { state_.select_row(row); });
        draw_note_menu(model.note_at(row).path);
        if (row == current && current != scrolled_row_) {
            ImGui::SetScrollHereY();
            scrolled_row_ = current;
        }
        ImGui::PopID();
    };
    // Genel aramada satır boyu değişir (en iyi satır yalnız içerik eşleşmesinde), kırpıcı eşit boy ister.
    // ponytail: genel aramanın satırları kırpıcısız çizilir; sonuç sınırı (limits) kaldırılıp binlerce satır
    // gelirse satır başına boy önbelleğiyle elle kırpılır.
    if (state_.search_mode() == app::SearchMode::Global) {
        for (int row = 0; row < model.row_count(); ++row)
            draw_row(row);
        return;
    }
    ImGuiListClipper clipper;  // 5.000 satırda yalnız görünenler çizilir
    clipper.Begin(model.row_count());
    if (current >= 0 && (current != scrolled_row_ || focus_row))
        clipper.IncludeItemByIndex(current);
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            draw_row(row);
}

}  // namespace noteapp::gui
