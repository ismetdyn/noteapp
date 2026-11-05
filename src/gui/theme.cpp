#include "gui/theme.hpp"

namespace noteapp::gui::theme {

namespace {

const Theme* active = &kLight;

constexpr ImU32 kTransparent = 0;
constexpr ImU32 kBlack = rgb(0x000000);
constexpr float kSeparatorHoverAlpha = 0.6F;
constexpr float kResizeGripHoverAlpha = 0.4F;
constexpr float kRowAltAlpha = 0.03F;
constexpr float kDropTargetAlpha = 0.12F;

}  // namespace

void apply_style(ImGuiStyle& style) {
    style.WindowPadding = {12.0F, 12.0F};
    style.FramePadding = {8.0F, 5.0F};
    style.ItemSpacing = {8.0F, 6.0F};
    style.ItemInnerSpacing = {6.0F, 4.0F};
    style.CellPadding = {8.0F, 4.0F};
    style.IndentSpacing = 16.0F;
    style.ScrollbarSize = 10.0F;
    style.ScrollbarRounding = 4.0F;
    style.ScrollbarPadding = 2.0F;
    style.GrabMinSize = 10.0F;
    style.GrabRounding = 4.0F;
    style.WindowRounding = 8.0F;  // yalnız modal/popup: kök pencere kendi PushStyleVar'ıyla 0
    style.ChildRounding = 6.0F;
    style.PopupRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.TabRounding = 5.0F;
    style.MenuItemRounding = 4.0F;
    style.SelectableRounding = 0.0F;  // ImGui başlığı: bitişik seçimde değiştirmek önerilmez
    style.WindowBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.ChildBorderSize = 0.0F;  // kutu içinde kutu yok: bölmeler tonla ayrılır
    style.FrameBorderSize = 1.0F;  // girdi ve onay kutusu sınırı boundary ile 3:1
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextPadding = {0.0F, 6.0F};
    style.WindowTitleAlign = {0.0F, 0.5F};
    style.ButtonTextAlign = {0.5F, 0.5F};
    style.DisabledAlpha = 0.45F;
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesNone;
}

void apply_colors(ImGuiStyle& style, const Theme& theme) {
    active = &theme;
    // Tabandaki ImGui teması tabloda adı geçmeyen yuvaları (çizim grafikleri vb.) okunur tutar.
    if (theme.dark)
        ImGui::StyleColorsDark(&style);
    else
        ImGui::StyleColorsLight(&style);
    const auto set = [&style](ImGuiCol slot, ImU32 color) {
        style.Colors[slot] = ImGui::ColorConvertU32ToFloat4(color);
    };
    const ImU32 modal_dim = with_alpha(kBlack, theme.modal_dim_alpha);
    set(ImGuiCol_Text, theme.text);
    // ImGui TextDisabled'ı ipucu, kesit, kısayol ve boş durum metninde de kullanır: bilgi taşır, muted.
    // faint metne verilmez; yalnız dekoratif yüzeylerde (kaydırma tutamağı) kalır.
    set(ImGuiCol_TextDisabled, theme.muted);
    set(ImGuiCol_WindowBg, theme.chrome);
    set(ImGuiCol_ChildBg, kTransparent);
    set(ImGuiCol_PopupBg, theme.raised);  // opak: arkadaki yazı diyalog metninin içinde görünmesin
    set(ImGuiCol_Border, theme.boundary);
    set(ImGuiCol_BorderShadow, kTransparent);
    set(ImGuiCol_FrameBg, theme.input);
    set(ImGuiCol_FrameBgHovered, theme.hover);
    set(ImGuiCol_FrameBgActive, theme.hover);
    set(ImGuiCol_TitleBg, theme.raised);
    set(ImGuiCol_TitleBgActive, theme.raised);
    set(ImGuiCol_TitleBgCollapsed, theme.raised);
    set(ImGuiCol_MenuBarBg, theme.chrome);
    set(ImGuiCol_ScrollbarBg, kTransparent);
    set(ImGuiCol_ScrollbarGrab, theme.border);
    set(ImGuiCol_ScrollbarGrabHovered, theme.faint);
    set(ImGuiCol_ScrollbarGrabActive, theme.muted);
    set(ImGuiCol_CheckMark, theme.accent);
    set(ImGuiCol_CheckboxSelectedBg, theme.input);
    set(ImGuiCol_SliderGrab, theme.accent);
    set(ImGuiCol_SliderGrabActive, theme.accent_fill);
    set(ImGuiCol_Button, theme.control);
    set(ImGuiCol_ButtonHovered, theme.control_hovered);
    set(ImGuiCol_ButtonActive, theme.control_active);
    set(ImGuiCol_Header, theme.selected);
    set(ImGuiCol_HeaderHovered, theme.hover);
    set(ImGuiCol_HeaderActive, theme.selected);
    set(ImGuiCol_Separator, theme.border);
    set(ImGuiCol_SeparatorHovered, with_alpha(theme.accent, kSeparatorHoverAlpha));
    set(ImGuiCol_SeparatorActive, theme.accent);
    set(ImGuiCol_ResizeGrip, kTransparent);
    set(ImGuiCol_ResizeGripHovered, with_alpha(theme.accent, kResizeGripHoverAlpha));
    set(ImGuiCol_ResizeGripActive, theme.accent);
    set(ImGuiCol_InputTextCursor, theme.accent);
    set(ImGuiCol_Tab, theme.chrome);
    set(ImGuiCol_TabHovered, theme.hover);
    set(ImGuiCol_TabSelected, theme.canvas);
    set(ImGuiCol_TabSelectedOverline, theme.accent);
    set(ImGuiCol_TabDimmed, theme.chrome);
    set(ImGuiCol_TabDimmedSelected, theme.canvas);
    set(ImGuiCol_TabDimmedSelectedOverline, theme.boundary);
    set(ImGuiCol_PlotHistogram, theme.accent);  // artık vurgu rengi değil: çizim kodu accent'i okur
    set(ImGuiCol_TableHeaderBg, theme.chrome);
    set(ImGuiCol_TableBorderStrong, theme.border);
    set(ImGuiCol_TableBorderLight, theme.border);
    set(ImGuiCol_TableRowBg, kTransparent);
    set(ImGuiCol_TableRowBgAlt, with_alpha(theme.text, kRowAltAlpha));
    set(ImGuiCol_TextLink, theme.accent);
    set(ImGuiCol_TextSelectedBg, with_alpha(theme.accent, theme.selection_alpha));
    set(ImGuiCol_TreeLines, theme.border);
    set(ImGuiCol_DragDropTarget, theme.accent);
    set(ImGuiCol_DragDropTargetBg, with_alpha(theme.accent, kDropTargetAlpha));
    set(ImGuiCol_UnsavedMarker, theme.muted);
    set(ImGuiCol_NavCursor, theme.accent);
    set(ImGuiCol_NavWindowingHighlight, theme.accent);
    set(ImGuiCol_NavWindowingDimBg, modal_dim);
    set(ImGuiCol_ModalWindowDimBg, modal_dim);
}

const Theme& current() {
    return *active;
}

}  // namespace noteapp::gui::theme
