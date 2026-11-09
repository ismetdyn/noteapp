#pragma once

#include "app/data_preview.hpp"
#include "edit/syntax.hpp"
#include "pdf/markdown_blocks.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Önizlemenin yazı aileleri; hepsi atlasta yüklü olmalı (View::create).
class PreviewSelection;

struct PreviewFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* bold_italic = nullptr;
    ImFont* mono = nullptr;

    bool operator==(const PreviewFonts&) const = default;
};

// Rengin rolü: renk çizimde temadan alınır, tema değişince yerleşim yeniden kurulmaz.
// Missing: çözülmeyen [[hedef]] — bağlantı panelindeki "Eksik" gibi soluk, üstü çizili, tıklanmaz.
enum class PreviewInk { Text, Muted, Link, Missing };

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
    bool strike = false;           // ~~üstü çizili~~
    // Koşunun satırı: koşu sırasıyla azalmaz (pos.y azalabilir: büyük başlığın satırında küçük im aşağıda
    // ortalanır). Görünen aralık bununla aranır.
    float line_top = 0.0F;
    float line_bottom = 0.0F;
    // Kod bloğunda sözdizimi türü (TUI ile aynı tokenizer ve aile tablosu, token_color); Plain metin rengidir.
    tui::TokenKind token = tui::TokenKind::Plain;
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
// kaydırılır). Kare içinde çağrılmalı: ölçüm ImGui atlasıyla yapılır. `following`: bloklardan sonra gelen ilk
// blok (sonraki parçanın); son blokla arasındaki boşluk ona göre, yerleşimin yüksekliğine dahildir.
// `highlight_line_bytes`: kod satırı renklendirmesinin sınırı (limits.tui.highlight_line_bytes, TUI ile aynı).
PreviewLayout layout_preview(const std::vector<pdf::Block>& blocks, const PreviewFonts& fonts, float font_size,
                             float width, const LinkResolves& resolves, const pdf::Block* following = nullptr,
                             std::size_t highlight_line_bytes = tui::kDefaultHighlightLineBytes);

// Notun önizlemesi parça parça (app::markdown_chunks): yalnız görünen parçalar ayrıştırılıp yerleştirilir,
// uzaktakiler bırakılır. Ölçülmemiş parçanın yüksekliği bayt oranından tahmindir. Kare işi ve bellek notla değil
// görünen alanla orantılıdır; tek parçalık not bütün yerleşimle aynıdır.
class PreviewDocument {
public:
    // Görünen parça: belge koordinatında üstü ve yerleşimi; `first_block` ilk bloğunun belgedeki sırası (önceki
    // bir parça hiç ayrıştırılmadıysa bilinmez).
    struct Placed {
        float top = 0.0F;
        const PreviewLayout* layout = nullptr;
        std::size_t chunk = 0;
        std::optional<std::size_t> first_block;
    };

    // Metin ya da parçalama korumaları değişti: parçalar baştan. `source` çağıranda yaşar ve değişmez.
    void reset(std::string_view source, const app::MarkdownChunkLimits& limits = {});
    const app::MarkdownChunkLimits& chunk_limits() const { return limits_; }
    // Bağlantı çözümü değişti: yerleşimler atılır, yükseklikler tahmin olarak kalır.
    void relayout();
    // Kod renklendirmesinin satır sınırı (limits.tui.highlight_line_bytes); değişince yeniden yerleşir.
    void set_highlight_line_bytes(std::size_t bytes);
    // [scroll, scroll + view_height] alanındaki parçaları yerleştirir; yazı ya da genişlik değiştiyse önce relayout.
    // `to_end`: sona atlama istendi (End); ölçüm toplamı değiştirse de sonda kalınır. `end_slack`: pencerenin
    // içeriğe eklediği dolgu (ImGui'nin en büyük kaydırması toplam + dolgu - görünen yükseklik).
    // Ölçüm görünen ilk ölçülü parçanın üstündeki yüksekliği değiştirdiyse onu ekranda yerinde tutan kaydırmayı
    // döner (değişmediyse `scroll`). Kare içinde çağrılmalı.
    float update(float scroll, float view_height, const PreviewFonts& fonts, float font_size, float width,
                 const LinkResolves& resolves, bool to_end = false, float end_slack = 0.0F);
    std::vector<Placed> visible(float top, float bottom) const;
    ImVec2 size() const;

private:
    struct Chunk {
        app::MarkdownChunk range;
        std::optional<std::vector<pdf::Block>> blocks = {};
        std::optional<std::size_t> block_count = {};  // bir kez ayrıştırıldıysa
        std::optional<PreviewLayout> layout = {};
        std::optional<float> measured = {};  // son ölçülen yükseklik (yerleşim atılınca da tahmin olarak kalır)
        float right = 0.0F;
    };

    float height_of(const Chunk& chunk) const;
    std::vector<float> tops() const;
    const std::vector<pdf::Block>& parsed(std::size_t index);
    void lay_out(std::size_t index, const LinkResolves& resolves);
    void release_outside(const std::vector<float>& tops, float top, float bottom);

    std::string_view source_;
    app::MarkdownChunkLimits limits_;
    app::MarkdownDefinitions definitions_;  // birden çok parçada notun başvuru tanımları (her parçaya taşınır)
    std::vector<Chunk> chunks_;
    float pixels_per_byte_ = 0.0F;  // ilk ölçümden; ölçülmemiş parçanın tahmini
    PreviewFonts fonts_;
    float font_size_ = 0.0F;
    float width_ = -1.0F;
    std::size_t highlight_line_bytes_ = tui::kDefaultHighlightLineBytes;
};

// [top, bottom] dikey aralığına (yerleşim koordinatı) düşen koşular.
std::span<const PreviewRun> visible_runs(const PreviewLayout& layout, float top, float bottom);

// Yalnız [top, bottom] dikey aralığındaki (yerleşim koordinatı) kutu ve koşuları geçerli pencereye çizer;
// `origin` yerleşimin ekrandaki sol üstü. `selection` verilirse çizilen koşular ona kaydedilir: anahtar
// `key_base | koşu sırası`, satır kimliği `line_base + line_top` (belge koordinatı).
void draw_preview_layout(const PreviewLayout& layout, ImVec2 origin, float top, float bottom,
                         PreviewSelection* selection = nullptr, std::uint64_t key_base = 0,
                         double line_base = 0.0);

}  // namespace noteapp::gui
