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

// Panel genişliği ve yakınlaştırma. Seçilen genişlik `tui-state.toml`a pencere oranı olarak yazılır
// (`chosen_share`; TuiState::store_list_share), yakınlaştırma oturumluktur. Sınırlar pencere genişliğine göre
// okunurken uygulanır: pencere küçülünce de iki panel kullanılabilir kalır.
class PaneLayout {
public:
    static constexpr int kMinListWidth = 16;
    static constexpr int kMinRightWidth = 24;
    static constexpr int kKeyStep = 5;
    // tuning.tui_list_share'in defaults.toml'daki değeri (config'siz kurulan nesneler ve testler için).
    static constexpr double kDefaultListShare = 0.33;

    int list_width(int window_width) const;
    // Kullanıcı genişlik seçene dek sol panelin pencereye oranı (0..1); seçilen genişlik bunu geçer.
    void set_share(double share) { share_ = share; }
    void resize(int delta, int window_width);
    void set_list_width(int width, int window_width);
    // Seçilen genişliğin pencereye oranı; seçilmediyse ya da pencere bilinmiyorsa yok. Oran, sütun sayısından
    // iyi dayanır: terminal büyüyüp küçülünce liste orantılı kalır.
    std::optional<double> chosen_share(int window_width) const;
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
    double share_ = kDefaultListShare;
    bool zoomed_ = false;
    bool dragging_ = false;
};

}  // namespace noteapp::tui
