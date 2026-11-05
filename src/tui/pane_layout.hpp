#pragma once

#include <optional>

namespace noteapp::tui {

// Panel içeriği çerçeveye yapışmaz (ui-redesign §5.5): sol kenarın içinde bir boş sütun. Çizim, fare ve
// editör görüş alanı bu iki sayıyı paylaşır; biri değişirse hepsi birlikte kayar.
inline constexpr int kPaneContentX = 2;     // panelin x'inden içeriğin ilk sütununa: sol kenar ve iç boşluk
inline constexpr int kPaneChromeCells = 3;  // içeriğe kalmayan sütunlar: iki kenar ve iç boşluk

// Panellerin ekrandaki yeri; görünmeyen panel çizilmez.
struct PaneGeometry {
    bool list_visible = true;
    bool right_visible = true;
    int list_width = 0;
    int right_x = 0;
    int right_width = 0;
};

// Panel genişliği ve yakınlaştırma: yalnız oturum boyunca yaşar, config'e yazılmaz. Sınırlar pencere
// genişliğine göre okunurken uygulanır: pencere küçülünce de iki panel kullanılabilir kalır.
class PaneLayout {
public:
    static constexpr int kMinListWidth = 16;
    static constexpr int kMinRightWidth = 24;
    static constexpr int kKeyStep = 5;

    int list_width(int window_width) const;
    void resize(int delta, int window_width);
    void set_list_width(int width, int window_width);
    void toggle_zoom() { zoomed_ = !zoomed_; }
    void set_zoomed(bool zoomed) { zoomed_ = zoomed; }
    bool zoomed() const { return zoomed_; }
    // Yakınlaştırmada odaklı panel tüm genişliği alır; odak değişince gösterilen panel de değişir, böylece
    // odak görünmeyen panelde kalamaz.
    PaneGeometry geometry(int window_width, bool right_focused) const;

    // Fare ile bölücü sürükleme.
    bool on_divider(int x, int window_width) const;
    void begin_drag() { dragging_ = true; }
    void end_drag() { dragging_ = false; }
    bool dragging() const { return dragging_; }

private:
    std::optional<int> width_;  // kullanıcı seçene dek pencere oranı
    bool zoomed_ = false;
    bool dragging_ = false;
};

}  // namespace noteapp::tui
