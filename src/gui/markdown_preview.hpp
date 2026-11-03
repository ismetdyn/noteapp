#pragma once

#include "pdf/markdown_blocks.hpp"

#include <functional>
#include <imgui.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Önizlemenin yazı aileleri; hepsi atlasta yüklü olmalı (View::create).
struct PreviewFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* bold_italic = nullptr;
    ImFont* mono = nullptr;
};

// Rengin rolü: renk çizimde temadan alınır, tema değişince yerleşim yeniden kurulmaz.
// Missing: çözülmeyen [[hedef]] — bağlantı panelindeki "Eksik" gibi soluk, üstü çizili, tıklanmaz.
enum class PreviewInk { Text, Muted, Link, Missing };

// Satır içi kodun zemini: temanın kendi rengi, açık ve koyu temada pencere zemininden ayırt edilir.
inline constexpr ImGuiCol kInlineCodeBackground = ImGuiCol_TableBorderLight;

// [[hedef]] canlı bir nota çözülüyor mu (GuiState::resolves_link).
using LinkResolves = std::function<bool(std::string_view target)>;

// Tek satırlık metin parçası; konum yerleşimin sol üstüne göredir. Metin blokların içine işaret eder.
struct PreviewRun {
    ImFont* font = nullptr;
    float size = 0.0F;
    ImVec2 pos;
    float width = 0.0F;
    std::string_view text;
    PreviewInk ink = PreviewInk::Text;
    bool underline = false;
    bool code_background = false;  // satır içi kod
    // Koşunun satırı: koşu sırasıyla azalmaz (pos.y azalabilir: büyük başlığın satırında küçük im aşağıda
    // ortalanır). Görünen aralık bununla aranır.
    float line_top = 0.0F;
    float line_bottom = 0.0F;
};

struct PreviewBox {
    enum class Kind { CodeBlock, QuoteBar, Line };
    Kind kind = Kind::Line;
    ImVec2 min;
    ImVec2 max;
};

// Bağlantının tıklanır alanı: aynı satırdaki ardışık sözcükleri tek dikdörtgen.
struct PreviewHit {
    ImVec2 min;
    ImVec2 max;
    std::string_view target;
    bool wiki = false;
};

// Blokların konumlu çizim listesi. Koşular satır sırasıyla dizilidir (görünen aralık ikili aramayla
// bulunur). `blocks` yerleşimden uzun yaşamalıdır.
struct PreviewLayout {
    std::vector<PreviewRun> runs;
    std::vector<PreviewBox> boxes;
    std::vector<PreviewHit> hits;
    std::vector<std::pair<float, float>> block_spans;  // bloğun dikey aralığı (üst, alt)
    ImVec2 size;
};

// `font_size` ölçekli gövde yazı boyudur; `width` metnin sarıldığı genişlik (kod ve tablo taşabilir, yatay
// kaydırılır). Kare içinde çağrılmalı: ölçüm ImGui atlasıyla yapılır.
PreviewLayout layout_preview(const std::vector<pdf::Block>& blocks, const PreviewFonts& fonts, float font_size,
                             float width, const LinkResolves& resolves);

// [top, bottom] dikey aralığına (yerleşim koordinatı) düşen koşular.
std::span<const PreviewRun> visible_runs(const PreviewLayout& layout, float top, float bottom);

// Yalnız [top, bottom] dikey aralığındaki (yerleşim koordinatı) kutu ve koşuları geçerli pencereye çizer;
// `origin` yerleşimin ekrandaki sol üstü.
void draw_preview_layout(const PreviewLayout& layout, ImVec2 origin, float top, float bottom);

}  // namespace noteapp::gui
