#pragma once

#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImGuiListClipper;
struct ImVec2;

namespace noteapp::gui {

// Harita kamerası: ekran = tuval köşesi + offset + yerleşim konumu * zoom. auto_fit açıkken her kare çizge
// tuvale sığdırılır; kullanıcı kaydırınca, yakınlaşınca ya da düğüm sürükleyince kapanır, `0` yeniden açar.
struct MapCamera {
    float zoom = 1.0F;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
    bool auto_fit = true;
};

// Harita kenarları ImGui üçgeni değil SDL çizgisi olarak çizilir (draw_map_graph): renk başına uç noktaları
// (x1, y1, x2, y2, ...). Kare boyunca View'da yaşar, çizim listesindeki geri çağrı okur.
struct MapLineBatch {
    std::uint32_t color = 0;  // ImU32
    std::vector<float> coords;
};

// İnce çizim katmanı: her karede GuiState'i çizer, kullanıcı niyetini kuyruğa alır ve kare bittikten sonra
// iletir. Sıra önemlidir: niyet bir karar (diyalog) açabilir ve diyalog kendi karelerini üretir; ImGui karesi
// iç içe açılamaz.
class View {
public:
    // Pencere, çizici ve ImGui kurulur. Hata: stderr'e yazılabilecek açıklama.
    static std::expected<std::unique_ptr<View>, std::string> create(GuiState& state);
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // Üretim kararları: GuiState::deciders gerçek ImGui diyaloglarına bağlanır.
    void install_deciders();
    // Kapanış kabul edilene dek olay döngüsü.
    void run();

    // Tek kare: olayları işler, çizer, sunar, kuyruğa alınan niyetleri çalıştırır.
    void draw_frame();
    // Döngünün bir adımı: en çok timeout_ms bekler; yalnız olay/zamanlayıcı varsa kare üretir.
    void idle_step(int timeout_ms);

    // Test kancaları.
    std::function<void()> on_modal_frame;  // her diyalog karesinden önce: tuş enjekte eder
    std::size_t frames() const { return frames_; }
    bool search_active() const { return search_active_; }
    bool editor_active() const { return editor_active_; }
    bool close_requested() const { return close_requested_; }
    // Son karede çizilen adlı öğeler (ekran koordinatı): testler fareyle tıklar ve çakışma arar. Ad sabit
    // kimliktir, etiket değil. Klasör satırı "folder:<yol>"; kök "folder:".
    struct ItemRect {
        float min_x{};
        float min_y{};
        float max_x{};
        float max_y{};
        bool disabled = false;
    };
    std::pair<float, float> button_center(const std::string& name) const {
        const ItemRect& rect = item_rects_.at(name);
        return {(rect.min_x + rect.max_x) / 2.0F, (rect.min_y + rect.max_y) / 2.0F};
    }
    const std::map<std::string, ItemRect>& item_rects() const { return item_rects_; }
    // Arayüz ölçeğini uygular (stil, yazı, en küçük pencere, bölmeler); dummy sürücüde ekran ölçeği hep 1
    // olduğundan testler de buradan verir.
    void set_ui_scale(float scale);
    float ui_scale() const { return ui_scale_; }
    // Pencere koordinatındaki arayüz ölçeği: SDL_GetWindowDisplayScale / SDL_GetWindowPixelDensity.
    static float ui_scale_for(float display_scale, float pixel_density);
    static int token_input_flags();  // maskeli olduğunun testi için
    // Satır ikonu metinden büyük çizilir, satır yüksekliğine girmez; yuva adların aynı sütunda başlamasını
    // sağlar.
    static constexpr float kRowIconScale = 1.3F;
    static constexpr float kRowIconSlotEms = 1.6F;
    // Harita yerleşimi kare başına en çok bu kadar adım atar; 2.000 düğümde bütçesi (ölçüm: SPEC-gui kabul
    // 11).
    static constexpr int kMapIterationsPerFrame = 1;
    static constexpr double kMapLayoutBudgetMs = 8.0;

    struct Platform;
    struct PreviewCache;
    // Platform yalnız create()'te kurulur; public olması dışarıya yol açmaz (make_unique için).
    View(GuiState& state, std::unique_ptr<Platform> platform);

private:
    void pump_events();
    void render(const std::function<void()>& overlay);
    void apply_theme();
    void apply_window_scale();
    void run_intents();
    void queue(std::function<void()> intent);
    int idle_timeout_ms() const;

    void draw_root();
    void draw_notes_tab();
    void draw_left_pane();
    // Kayıtlı boyutu yalnız kullanıcı değiştirdiyse günceller; bu karede bölmenin olması gereken boyutu
    // döner.
    float reconcile_pane_size(float& stored, std::optional<float>& last_size, float actual, float min_size,
                              float max_size) const;
    void draw_search_row();
    void draw_toolbar();
    void draw_toolbar_separator(float gap);
    void draw_view_switch(float row_start_x, float row_width, float gap);
    void draw_folder_menu(const std::string& folder);
    void draw_note_menu(const core::NotePath& path);
    void draw_row_menu(const BrowserRow& row);
    float row_number_width();
    void draw_row_number(const std::string& name, std::size_t number, ImVec2 position, float width);
    void draw_note_list();
    bool begin_selected_row(int current, bool focus_row, ImGuiListClipper& clipper);
    void end_selected_row(const std::string& name, bool changed);
    void draw_tree_rows(bool focus_row);
    void draw_tree_row(const BrowserRow& row, const std::string& name, bool selected);
    void draw_details_path();
    void draw_details_rows(bool focus_row);
    void draw_details_row(const BrowserRow& row, const std::string& name, bool selected,
                          std::chrono::system_clock::time_point now);
    const std::array<float, 3>& details_column_widths(const std::vector<BrowserRow>& rows,
                                                      std::chrono::system_clock::time_point now);
    void draw_folder_summary();
    void draw_note_rows(bool focus_row);
    void draw_map();
    void draw_map_controls(const NoteMap& map);
    void handle_map_input(const NoteMap& map, ImVec2 origin, ImVec2 size, int hovered);
    void draw_map_graph(const NoteMap& map, ImVec2 origin, int hovered);
    void zoom_map(ImVec2 origin, ImVec2 anchor, float factor);
    void draw_trash_header();
    void draw_trash_rows(bool focus_row);
    void draw_right_pane();
    void draw_note_tabs();
    void request_note_tab(std::function<void()> intent);
    void queue_open(const core::NotePath& path, bool new_tab);
    void queue_open_selected_in_new_tab();
    void draw_header_row();
    void draw_editor(float reserved_height);
    void draw_preview(float reserved_height);
    float panel_height(std::size_t lines, bool open, float available_height) const;
    struct InfoColumns {
        float label = 0.0F;
        float value = 0.0F;
        std::size_t pairs = 1;  // satır başına alan çifti
    };
    InfoColumns info_columns() const;
    void draw_info_panel(const InfoColumns& columns, float list_height);
    void draw_links_panel();
    void draw_link_row(const core::LinkLine& line);
    void draw_status_bar();
    void draw_fitted_text(const std::string& name, const std::string& text, float width,
                          std::string (*shorten)(std::string_view, float));
    void draw_settings_tab();
    void draw_settings_form();
    void draw_settings_footer();
    void begin_setting_row(const std::string& label, const std::string& label_name);
    void draw_setting_field(const std::string& key);
    void draw_setting_input(const std::string& key);
    void draw_choice_combo(const std::string& key, std::span<const char* const> choices, std::string& value);
    void draw_row_numbers_setting();
    void draw_token_row();
    void draw_connection_test_row();
    bool button(const std::string& name, const std::string& label);
    void remember_item_center(const std::string& name);
    void handle_shortcuts();

    void run_modal(const std::string& title, const std::function<bool()>& body);
    template <typename Choice> Choice ask_choice(const ChoiceBox<Choice>& box);
    std::optional<SaveDetails> ask_save_details(const SaveDialogRequest& request);
    std::optional<std::string> ask_folder_name(const std::string& title, const std::string& initial);
    enum class PathKind { SaveFile, Folder };
    // Yol elle yazılır ya da yerel dosya diyaloğu (portal/zenity) ile seçilir; yerel diyalog yoksa alan kalır
    // (plan Riskler: Linux dosya diyaloğu).
    std::optional<std::string> ask_path(const std::string& title, const std::string& initial, PathKind kind);
    std::optional<std::string> ask_export_path(const std::string& suggested);

    GuiState& state_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<PreviewCache> preview_;
    std::vector<std::function<void()>> intents_;
    std::size_t frames_ = 0;
    int frames_left_ = 0;  // olaydan sonra ImGui'nin yerleşmesi için üretilecek kare sayısı
    int modal_depth_ = 0;
    float ui_scale_ = 0.0F;                // ilk apply_window_scale öncesi ölçek yok
    float settings_footer_height_ = 0.0F;  // geçen karenin ölçümü: form altbilgiye yer bırakır
    std::optional<float> left_pane_size_;  // geçen karede uygulanan boyut; ilk çizime dek yok
    std::optional<float> map_pane_size_;   // Harita'daki sol bölmenin aynısı; görünüm geçişinde sıfırlanır
    bool map_pane_ = false;                // geçen karede sol bölme haritayı mı çizdi
    // Harita: kamera, düğüm başına sabit metinler (çizge değişince kurulur), basılı düğüm, '+'/'-' tuşları.
    struct MapCache {
        std::size_t generation = SIZE_MAX;
        std::vector<std::string> names;   // item_rects adı: "map:<yol>", etiket "map:#<ad>"
        std::vector<std::string> labels;  // not: dosya adı; etiket: "#ad" (denetim baytı U+FFFD)
        std::vector<core::IconColor> colors;
    };
    MapCamera map_camera_;
    MapCache map_cache_;
    std::vector<MapLineBatch> map_lines_;
    std::optional<std::size_t>
        map_pressed_;  // basılı tutulan düğüm: bırakınca açılır, sürüklenince iğnelenir
    bool map_dragged_ = false;
    int map_zoom_keys_ = 0;       // SDL tuş kodundan: '+' ve '-' klavye düzeninden bağımsız okunur
    bool map_animating_ = false;  // bu karede yerleşim sürdü: boşta beklemeden yeni kare
    bool close_requested_ = false;
    bool rescan_on_focus_ = false;  // diyalogda gelen odak da kaybolmaz: tarama kare dışında çalışır
    bool search_active_ = false;
    bool editor_active_ = false;
    bool list_focused_ = false;
    bool refocus_editor_ = false;
    std::size_t drawn_buffer_generation_ = 0;
    int scrolled_row_ = -1;
    std::string scrolled_item_;  // gezginde en son kaydırılan seçili satır (ad: yol, sıra değil)
    bool follow_selection_ = false;
    // Ayrıntılar'ın sabit sütun genişlikleri (boyut, değiştirilme, oluşturulma) ve ölçüldükleri durum.
    std::array<float, 3> details_widths_{};
    std::size_t details_widths_rebuilds_ = 0;
    float details_widths_font_ = 0.0F;
    std::string details_widths_day_;  // klavyeyle seçim değişti: gezinti imleci yeni seçili satıra gelsin
    Tab drawn_tab_ = Tab::Notes;
    bool tab_switch_pending_ = false;
    // Editör sekmeleri: programatik geçiş ImGui'de bir kare gecikir (üst sekmelerle aynı kural).
    std::uint64_t drawn_note_tab_ = 0;
    bool note_tab_pending_ = false;
    FocusRequest pending_focus_ = FocusRequest::None;
    std::map<std::string, ItemRect> item_rects_;
};

}  // namespace noteapp::gui
