#pragma once

#include <imgui.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Önizlemede fareyle metin seçimi (TUI önizleme seçimi gibi): seçilen, ekranda görüntülenen metindir, özgün
// baytlar değil (onları Ctrl+C seçim yokken kopyalar). Her kare çizilen koşular (yalnız görünenler) kaydedilir;
// uçlar koşunun kararlı anahtarı ve koşu içi bayt olarak tutulur, kaydırmada yerinde kalır. Kayıtların metni tek
// tamponda: kapasite kareler arasında kalır, kare başına ayırma yoktur.
class PreviewSelection {
public:
    struct Point {
        std::uint64_t key = 0;  // okuma sırasıyla artar, kareler arasında aynı koşuyu gösterir
        std::size_t byte = 0;   // koşu içi, kod noktası başı
        auto operator<=>(const Point&) const = default;
    };

    // Kare başı: önceki karenin kayıtları bırakılır. Seçim kalır.
    void begin_frame();
    // Çizilecek koşu; metinden önce çağrılır ki seçim zemini metnin altında kalsın. `line` görüntü satırının
    // kimliğidir (aynı satırdaki koşular arası boşluk, satırlar arası '\n' olur). `width` < 0 ise ölçülür.
    void add(std::uint64_t key, double line, std::string_view text, ImVec2 pos, float height, ImFont* font,
             float size, float width = -1.0F);
    // Kare sonu: sol tık uç koyar, sürükleme genişletir, çift tık sözcüğü seçer. `can_start`: tık bu alanda
    // başlayabilir (önizlemenin iç alanında, üstünde açılır pencere yok). Çift tık bir sözcük seçtiyse true.
    bool update(bool can_start);
    void clear();
    bool empty() const { return anchor_ == focus_; }
    // Son kaydedilen karede görünen seçili metin.
    std::string selected_text() const;
    // Test kancası: `needle`ı içeren ilk koşuda eşleşmenin başının (`end`: sonunun) ekran noktası, satır ortası.
    std::optional<ImVec2> locate(std::string_view needle, bool end = false) const;

private:
    struct Run {
        std::uint64_t key = 0;
        double line = 0.0;
        std::size_t offset = 0;  // text_ içinde
        std::size_t length = 0;
        ImVec2 pos;
        float height = 0.0F;
        ImFont* font = nullptr;
        float size = 0.0F;
        float width = 0.0F;
    };

    std::string_view text_of(const Run& run) const { return std::string_view(text_).substr(run.offset, run.length); }
    float x_of(const Run& run, std::size_t byte) const;
    // Koşu içinde x'e en yakın kod noktası sınırı; `floor`: x'i içeren kod noktasının başı.
    std::size_t byte_at(const Run& run, float x, bool floor) const;
    // Fareye en yakın koşu (önce dikey, sonra yatay uzaklık); `inside`: nokta koşunun içinde.
    const Run* nearest(ImVec2 point, bool* inside) const;
    void select_word(const Run& run, std::size_t byte);

    std::vector<Run> runs_;
    std::string text_;
    Point anchor_;
    Point focus_;
    bool dragging_ = false;
    // add() ardışık koşular arası boşluğu boyamak için önceki koşunun seçili sağ ucu (aynı satırdaysa).
    std::optional<std::pair<double, float>> painted_end_;
};

}  // namespace noteapp::gui
