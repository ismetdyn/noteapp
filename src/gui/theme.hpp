#pragma once

// Arayüzün renk token'ları ve ImGui stil değerleri ("Sessiz mürekkep", .engineering/design/ui-redesign.md
// §3.1, §3.3). Çizim kodu sabit renk tutmaz, rengi buradan alır; kontrast oranları
// tests/gui/theme_test.cpp'de.

#include "edit/syntax.hpp"

#include <cstdint>
#include <imgui.h>

namespace noteapp::gui {

// Sözdizimi türünün geçerli temadaki rengi; aile tablosu TUI ile ortak (tui::syntax_family). Tanım view_widgets.cpp.
ImU32 token_color(tui::TokenKind kind);

}  // namespace noteapp::gui

namespace noteapp::gui::theme {

// 0xRRGGBB → ImGui'nin paketli rengi; bayt sırası ImGui'nin IM_COL32_*_SHIFT tanımından gelir.
constexpr ImU32 rgb(std::uint32_t hex) {
    constexpr std::uint32_t kByte = 0xFF;
    constexpr int kRedShift = 16;
    constexpr int kGreenShift = 8;
    return (((hex >> kRedShift) & kByte) << IM_COL32_R_SHIFT) |
           (((hex >> kGreenShift) & kByte) << IM_COL32_G_SHIFT) | ((hex & kByte) << IM_COL32_B_SHIFT) |
           (kByte << IM_COL32_A_SHIFT);
}

// Rengin saydamlığını `alpha` (0..1) yapar.
constexpr ImU32 with_alpha(ImU32 color, float alpha) {
    constexpr float kMaxByte = 255.0F;
    const auto byte = static_cast<std::uint32_t>((alpha * kMaxByte) + 0.5F);
    return (color & ~IM_COL32_A_MASK) | (byte << IM_COL32_A_SHIFT);
}

struct Theme {
    bool dark = false;
    // Yüzeyler: krom (kök pencere, kenar çubuğu) içerikten (tuval) koyu/gri; tuval editör, önizleme, harita.
    ImU32 chrome = 0;
    ImU32 canvas = 0;
    ImU32 raised = 0;  // popup, modal, palet, ipucu
    ImU32 input = 0;   // FrameBg
    ImU32 hover = 0;
    ImU32 control = 0;  // ikincil buton zemini
    ImU32 control_hovered = 0;
    ImU32 control_active = 0;
    ImU32 selected = 0;  // seçili satır
    ImU32 border = 0;    // bilgi taşımayan ayraç
    ImU32 boundary = 0;  // girdi, onay kutusu, popup kenarı: 3:1
    // Metin
    ImU32 text = 0;
    ImU32 muted = 0;
    ImU32 faint = 0;  // yalnız devre dışı ve dekoratif (WCAG 1.4.3 dışı); TextDisabled değil
    ImU32 accent = 0;
    ImU32 accent_fill = 0;
    ImU32 on_fill = 0;  // dolu (accent_fill, danger_fill) zemin üstündeki metin
    ImU32 danger = 0;
    ImU32 danger_fill = 0;
    ImU32 warning = 0;
    ImU32 success = 0;
    ImU32 code_bg = 0;
    float selection_alpha = 0.0F;  // TextSelectedBg = accent × bu
    float modal_dim_alpha = 0.0F;  // ModalWindowDimBg = siyah × bu
    // Sözdizimi aileleri; TUI aynı aileleri ANSI-16 ile çizer (SPEC-tui "Sözdizimi vurgusu").
    ImU32 comment = 0;
    ImU32 string = 0;
    ImU32 number = 0;
    ImU32 type = 0;
    ImU32 keyword = 0;
    ImU32 heading = 0;
};

inline constexpr Theme kDark{
    .dark = true,
    .chrome = rgb(0x15171B),
    .canvas = rgb(0x1C1F24),
    .raised = rgb(0x24272E),
    .input = rgb(0x111317),
    .hover = rgb(0x2B2F37),
    .control = rgb(0x2B2F37),
    .control_hovered = rgb(0x343943),
    .control_active = rgb(0x3D434D),
    .selected = rgb(0x2C3B55),
    .border = rgb(0x2E323A),
    .boundary = rgb(0x737B88),
    .text = rgb(0xDDE1E7),
    .muted = rgb(0xA4ACB7),
    .faint = rgb(0x6B7380),
    .accent = rgb(0x7AA7F5),
    .accent_fill = rgb(0x3B6FD1),
    .on_fill = rgb(0xFFFFFF),
    .danger = rgb(0xF2787D),
    .danger_fill = rgb(0xB83A3F),
    .warning = rgb(0xE3B65B),
    .success = rgb(0x7CC68E),
    .code_bg = rgb(0x3A3F48),
    .selection_alpha = 0.30F,
    .modal_dim_alpha = 0.45F,
    // Koyu sözdizimi pastelleri Catppuccin Frappé'den uyarlandı (MIT).
    .comment = rgb(0x7D8796),
    .string = rgb(0xA6D189),
    .number = rgb(0xEF9F76),
    .type = rgb(0x81C8BE),
    .keyword = rgb(0xCA9EE6),
    .heading = rgb(0x8CAAEE),
};

inline constexpr Theme kLight{
    .dark = false,
    .chrome = rgb(0xF2F3F5),
    .canvas = rgb(0xFFFFFF),
    .raised = rgb(0xFFFFFF),
    .input = rgb(0xFFFFFF),
    .hover = rgb(0xE6E8EC),
    .control = rgb(0xE6E8EC),
    .control_hovered = rgb(0xDCDFE4),
    .control_active = rgb(0xD0D4DA),
    .selected = rgb(0xD8E3F8),
    .border = rgb(0xDADDE2),
    .boundary = rgb(0x7A828C),
    .text = rgb(0x1F2328),
    .muted = rgb(0x59636E),
    .faint = rgb(0x8B939D),
    .accent = rgb(0x2A62C9),
    .accent_fill = rgb(0x2A62C9),
    .on_fill = rgb(0xFFFFFF),
    .danger = rgb(0xC42B30),
    .danger_fill = rgb(0xC42B30),
    .warning = rgb(0x8A5A00),
    .success = rgb(0x1D7A3A),
    .code_bg = rgb(0xCDD2D8),
    .selection_alpha = 0.22F,
    .modal_dim_alpha = 0.25F,
    .comment = rgb(0x6A737D),
    .string = rgb(0x2F7D32),
    .number = rgb(0xB5501C),
    .type = rgb(0x0F7B80),
    .keyword = rgb(0x8A3FB5),
    .heading = rgb(0x2A5BBF),
};

// Temadan bağımsız saydamlıklar (§3.1): bul vurgusu ve çoklu seçim kehribar (warning) ailesindedir.
inline constexpr float kFindMatchAlpha = 0.25F;
inline constexpr float kFindCurrentAlpha = 0.55F;
inline constexpr float kMarkFillAlpha = 0.18F;

// ChildBorderSize 0 iken ImGui çerçeveli çocuk pencerenin iç boşluğunu da sıfırlar: bölme içeriği kenara
// yapışmasın, satır vurgusu bölmenin dışına taşmasın diye boşluk korunur.
inline constexpr ImGuiChildFlags kPaneChildFlags =
    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;

// Rengin kanallarını `factor` ile çarpar (>1 açar, <1 koyulaştırır, 1'de kırpılır); saydamlık değişmez.
ImU32 scaled(ImU32 color, float factor);

// Boyutlar ölçek 1 pikselindedir: temiz ImGuiStyle() sonrası, ScaleAllSizes'tan ÖNCE çağrılır.
void apply_style(ImGuiStyle& style);
// Renkleri temadan kurar ve `current()`'ı ona çevirir. Boyutlara dokunmaz: sistem teması değişince yalnız bu.
void apply_colors(ImGuiStyle& style, const Theme& theme);
// Son `apply_colors` teması (önce: açık). Çizim kodu token'ı buradan okur.
const Theme& current();

}  // namespace noteapp::gui::theme
