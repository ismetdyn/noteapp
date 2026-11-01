#pragma once

#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::gui {

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
        float min_x;
        float min_y;
        float max_x;
        float max_y;
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

private:
    struct Platform;

    View(GuiState& state, std::unique_ptr<Platform> platform);
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
    void draw_folder_tree();
    void draw_toolbar();
    bool draw_folder_row(const std::string& folder, bool has_children);
    void draw_folder_children(std::size_t& index, const std::string& parent);
    void draw_folder_menu(const std::string& folder);
    void draw_note_menu(int row);
    void draw_note_list();
    void draw_note_rows(bool focus_row);
    void draw_trash_header();
    void draw_trash_rows(bool focus_row);
    void draw_right_pane();
    void draw_header_row();
    void draw_editor();
    void draw_status_bar();
    void draw_fitted_text(const std::string& name, const std::string& text, float width,
                          std::string (*shorten)(std::string_view, float));
    void draw_settings_tab();
    void draw_settings_form();
    void draw_settings_footer();
    void begin_setting_row(const std::string& label, const std::string& label_name);
    void draw_setting_field(const std::string& key);
    void draw_setting_input(const std::string& key);
    void draw_language_combo(std::string& value);
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
    std::vector<std::function<void()>> intents_;
    std::size_t frames_ = 0;
    int frames_left_ = 0;  // olaydan sonra ImGui'nin yerleşmesi için üretilecek kare sayısı
    int modal_depth_ = 0;
    float toolbar_height_ = 0.0F;            // geçen karenin ölçümü: klasör bölmesi araç çubuğuna yer bırakır
    float ui_scale_ = 0.0F;                  // ilk apply_window_scale öncesi ölçek yok
    float settings_footer_height_ = 0.0F;    // geçen karenin ölçümü: form altbilgiye yer bırakır
    std::optional<float> left_pane_size_;    // geçen karede uygulanan boyut; ilk çizime dek yok
    std::optional<float> folder_pane_size_;  // aynısı, klasör bölmesi
    bool close_requested_ = false;
    bool rescan_on_focus_ = false;  // diyalogda gelen odak da kaybolmaz: tarama kare dışında çalışır
    bool search_active_ = false;
    bool editor_active_ = false;
    bool list_focused_ = false;
    bool refocus_editor_ = false;
    std::size_t drawn_buffer_generation_ = 0;
    int scrolled_row_ = -1;
    Tab drawn_tab_ = Tab::Notes;
    bool tab_switch_pending_ = false;
    FocusRequest pending_focus_ = FocusRequest::None;
    std::map<std::string, ItemRect> item_rects_;
};

}  // namespace noteapp::gui
