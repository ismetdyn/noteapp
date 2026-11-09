#pragma once

#include "app/map_camera.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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

struct GuiCommand;
enum class CommandScope;
struct PreviewFonts;
struct PreviewLayout;

// Düğme dili (.engineering/design/ui-redesign.md §3.4): ikincil dolgu, hayalet (zeminsiz; GhostDanger üzerine
// gelince yıkıcı renkte), birincil (accent_fill) ve yıkıcı (danger_fill).
enum class ButtonKind : std::uint8_t { Secondary, Ghost, GhostDanger, Primary, Danger };

// Harita kamerası (app::MapCamera, piksel): ekran = tuval köşesi + (x, y) + yerleşim konumu * zoom. auto_fit
// açıkken her kare çizge tuvale sığdırılır; kullanıcı kaydırınca, yakınlaşınca ya da düğüm sürükleyince
// kapanır, `0` yeniden açar.
struct MapCamera : app::MapCamera {
    bool auto_fit = true;
};

// Harita kenarları ImGui üçgeni değil SDL çizgisi olarak çizilir (draw_map_graph): renk başına uç noktaları
// (x1, y1, x2, y2, ...). Kare boyunca View'da yaşar, çizim listesindeki geri çağrı okur.
struct MapLineBatch {
    std::uint32_t color = 0;  // ImU32
    std::vector<float> coords;
};

// "Klasörü aç" adresi: file:// + yol baytları, ayırıcı ve güvenli karakterler dışında yüzde kodlu.
std::string file_url(const std::string& path);

// İnce çizim katmanı: her karede GuiState'i çizer, kullanıcı niyetini kuyruğa alır ve kare bittikten sonra
// iletir. Sıra önemlidir: niyet bir karar (diyalog) açabilir ve diyalog kendi karelerini üretir; ImGui karesi
// iç içe açılamaz.
struct ImportInbox;  // view_internal.hpp

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

    // Güncelleme penceresinin düğmeleri değişince (denetim bitti, iptal edildi) Kur bu süre kapalı kalır:
    // İptal'in yerine geçen Kur'a, İptal'e yönelmiş tıklama ya da Enter düşmesin.
    static constexpr std::chrono::milliseconds kUpdateArmDelay{500};

    // Test kancaları.
    std::function<void()> on_modal_frame;  // her diyalog karesinden önce: tuş enjekte eder
    std::chrono::milliseconds update_arm_delay = kUpdateArmDelay;
    // Kur kilidinin süresini doldurur (kilit yoksa bir şey yapmaz): testler duvar saati beklemeden "süre doldu"
    // anını kurar; açılış yine idle_step'te olur.
    void expire_update_arm() {
        if (update_armed_at_)
            update_armed_at_ = std::chrono::steady_clock::time_point{};
    }
    std::size_t frames() const { return frames_; }
    bool search_active() const { return search_active_; }
    bool editor_active() const { return editor_active_; }
    bool close_requested() const { return close_requested_; }
    bool tooltip_visible() const;  // son karede gecikmeli ipucu ekranda (ilk ölçüm karesi gizlidir)
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
    static constexpr std::size_t kDetailsFields = 5;  // Ayrıntılar'ın üst veri alanı sayısı
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
    PreviewFonts fonts() const;  // Ayarlar → Görünüm'de seçilen UI ve editör aileleri
    void run_intents();
    void queue(std::function<void()> intent);
    void queue_command(const GuiCommand& command);  // gui/commands.hpp tablosundaki eylem
    int idle_timeout_ms() const;
    bool caret_blinks() const;  // odaktaki ImGui metin alanının kırpma imleci boşta da yenilenmeli
    void schedule_tooltip_wake();
    bool tooltip_due(std::chrono::steady_clock::time_point now);  // süre dolduysa tüketir ve true

    void draw_root();
    void draw_notes_tab();
    void draw_notices(float status_height);  // sağ altta bildirimler (toast), durum çubuğunun üstünde
    void draw_update_dialog();               // güncelleme penceresi (GuiState::update_open)
    void draw_left_pane();
    // Kayıtlı boyutu yalnız kullanıcı değiştirdiyse günceller; bu karede bölmenin olması gereken boyutu
    // döner.
    float reconcile_pane_size(float& stored, std::optional<float>& last_size, float actual, float min_size,
                              float max_size) const;
    void draw_search_row();
    void search_line_keys();  // InputText'ten önce: geçmiş, gölge, açık listenin tuşları
    void draw_search_text(const std::string& text);  // renkli argümanlar ve gölge öneri
    void draw_search_popup();
    void draw_sort_menu();
    void draw_picker();
    void draw_toolbar();
    void draw_toolbar_separator(float gap);
    void draw_view_switch(float row_start_x, float row_width, float gap);
    void draw_folder_menu(const std::string& folder);
    void draw_note_menu(const core::NotePath& path);
    void draw_row_menu(const BrowserRow& row);
    void draw_selection_bar();  // son öğe seçili satır; liste odakta değilse çizmez
    float row_number_width();
    void draw_row_number(const std::string& name, std::size_t number, ImVec2 position, float width);
    void draw_note_list();
    void draw_empty_notes();       // hiç not yok: ortalanmış blok ve eylemler
    void draw_quick_add();
    void run_scope_commands(CommandScope scope);  // kapsamın tuşlarına basıldıysa komutu kuyruğa alır
    void draw_column_menu();
    void draw_trash_preview();
    bool begin_selected_row(int current, bool focus_row, ImGuiListClipper& clipper);
    void end_selected_row(const std::string& name, bool changed);
    void draw_tree_rows(bool focus_row);
    void draw_tree_row(const BrowserRow& row, const std::string& name, bool selected);
    void draw_details_path();
    void draw_details_rows(bool focus_row);
    void draw_details_row(const BrowserRow& row, const std::string& name, bool selected,
                          std::chrono::system_clock::time_point now);
    const std::array<float, kDetailsFields>& details_column_widths(const std::vector<BrowserRow>& rows,
                                                                   std::chrono::system_clock::time_point now);
    void draw_folder_summary();
    void draw_note_rows(bool focus_row);
    void draw_map();
    void draw_map_controls(const NoteMap& map);
    void handle_map_input(const NoteMap& map, ImVec2 origin, ImVec2 size, int hovered);
    void draw_map_graph(const NoteMap& map, ImVec2 origin, int hovered);
    // MAP-R: Shift+sürüklemenin bırakılışı ve kenara sağ tık; seçim ve kaldırma menüleri.
    void drop_map_link(const NoteMap& map, std::size_t from, int hovered);
    void open_map_edge_menu(const NoteMap& map, ImVec2 origin);
    void open_map_node_menu(const NoteMap& map, std::size_t node);
    void start_map_connect(const NoteMap& map, std::size_t from);
    void draw_map_menus();
    void draw_map_node_items(const MapNode& subject, std::size_t node, std::size_t generation);  // F2 keşif
    void draw_map_find(const NoteMap& map);  // F2: Ctrl+F satırı
    void draw_map_query();                   // F3: sorgu çubuğu (görünümler, metin, Çalıştır, Tablo), hata satırı
    void draw_map_query_table();             // F3: sonucun tablosu, tuvalin yerinde (ImGuiListClipper)
    void focus_map_node(const NoteMap& map, std::size_t node, ImVec2 size);
    void draw_relation_tooltip(const NoteMap& map, ImVec2 origin);  // F1: işaretçinin yakınındaki ilişki
    void zoom_map(ImVec2 origin, ImVec2 anchor, float factor);
    void draw_trash_header();
    void draw_trash_rows(bool focus_row);
    void draw_right_pane(float canvas_left);  // tuval zemini bölme ayracından başlar
    void draw_note_tabs();
    void request_note_tab(std::function<void()> intent);
    void queue_open(const core::NotePath& path, bool new_tab);
    void queue_open_selected_in_new_tab();
    void draw_header_row();
    void draw_editor(float reserved_height);
    void draw_editor_text();
    std::optional<ImVec2> draw_editor_rows(ImVec2 origin, float text_x, float width, float cell, float row_height);
    void editor_keys();  // odaktaki editörün tuşları ve yazılan karakterler (SDL metin olayları)
    void draw_completion_popup(ImVec2 cursor, float row_height);
    void draw_find_bar();
    // Durum çubuğu iletisi: metin, renk (ImU32) ve önem; çubukta yalnız en önemlisi yazılır, ötekiler çipte.
    enum class StatusRank : std::uint8_t { Info, Warning, Error };
    struct StatusMessage {
        std::string text;
        std::uint32_t color = 0;
        StatusRank rank = StatusRank::Info;
    };
    StatusMessage editor_notice() const;  // editörün iletisi; yoksa metni boş
    void draw_preview(float reserved_height);
    // request: bu karede istenen klavye kaydırması (piksel); to_end: sona atlama (End).
    void draw_markdown_preview(float request, bool to_end);
    void draw_preview_links(const PreviewLayout& layout, ImVec2 origin, float top, float bottom);
    void draw_raw_preview();  // limits.tui.view_bytes üstü: neden + ham metin (TUI REVIEW gibi)
    // csv ve json: TUI REVIEW'un ayrıştırması; not bir kez dizinlenir (app::index_csv, app::index_json), yalnız
    // görünen satırlar ayrıştırılıp çizilir.
    void draw_data_preview();
    void draw_csv_table(const app::CsvIndex& index);
    void draw_json_lines(const app::JsonIndex& index);
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
    void draw_relation_row_menu(const core::RelatedNote& selected);  // F1: ilişkili satırın sağ tık menüsü
    // F4 özellik denetçisi; list_height 0: panel en alttadır, kalan alanı alır.
    void draw_inspector_panel(float list_height);
    void draw_inspector_row(const app::InspectorRow& row);
    void draw_inspector_add_row();
    std::optional<std::string> inspector_editing_;  // satır içi düzenlenen özelliğin anahtarı (ham)
    std::string inspector_edit_text_;
    int inspector_edit_type_ = 0;  // 0 otomatik, 1 metin, 2 sayı, 3 mantıksal
    bool inspector_edit_focus_ = false;
    std::string inspector_new_key_;
    std::string inspector_new_value_;
    int inspector_new_type_ = 0;
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
    void draw_font_setting(const char* key, std::span<const char* const> choices, const std::string& current,
                           void (GuiState::*apply)(const std::string&));
    void draw_token_row();
    void draw_connection_test_row();
    bool button(const std::string& name, const std::string& label, ButtonKind kind = ButtonKind::Secondary);
    struct DialogAnswer {
        bool ok = false;
        bool cancel = false;
    };
    DialogAnswer dialog_ok_cancel();  // Tamam ve Vazgeç düğmeleri ("dialog_ok", "dialog_cancel")
    void remember_item_center(const std::string& name);
    // Adlı alanı test kancasına yazar (item_rects_). Tek yazma noktası: MinGW GCC -O3, std::function içine
    // satır içi açılan lambdada map::operator[]'ı yanlış düğüm boyuyla görüp -Wstringop-overflow verir.
    void remember_rect(const std::string& name, ImVec2 min, ImVec2 max, bool disabled = false);
    void handle_shortcuts();

    // `top_share` verilirse diyalog ortada değil, pencerenin üstünden o payda başlar (palet).
    void run_modal(const std::string& title, const std::function<bool()>& body,
                   std::optional<float> top_share = std::nullopt);
    template <typename Choice> Choice ask_choice(const ChoiceBox<Choice>& box);
    std::optional<SaveDetails> ask_save_details(const SaveDialogRequest& request);
    // Tek satırlık ad istemi; alan etiketi verilmezse "Klasör adı". `trim`: baş/son boşluk atılır (klasör);
    // kapalıysa ad olduğu gibi döner, kuralı çağıran uygular (görünüm adı). Boş ad vazgeçmedir.
    std::optional<std::string> ask_folder_name(const std::string& title, const std::string& initial,
                                               const std::string& field_label = {}, bool trim = true);
    std::optional<std::string> ask_palette();  // seçilen komut kimliği
    // F1 ilişki adı ve "Yönlü" (deciders.relation); nullopt = vazgeçti.
    std::optional<core::RelationSpec> ask_relation(const RelationRequest& request);
    bool draw_palette_row(const PaletteRow& row, bool selected, float row_height);  // tıklandıysa true
    void show_help_dialog();
    enum class PathKind { SaveFile, Folder };
    // Yol elle yazılır ya da yerel dosya diyaloğu (portal/zenity) ile seçilir; yerel diyalog yoksa alan kalır
    // (plan Riskler: Linux dosya diyaloğu).
    std::optional<std::string> ask_path(const std::string& title, const std::string& initial, PathKind kind);
    std::optional<std::string> ask_export_path(const std::string& suggested);
    // Kare dışında: istenen içe alma diyaloğunu açar, diyalogdan ve sürükle-bıraktan gelen yolları içe alır.
    void import_pending();

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
        std::vector<float> sizes;  // taban yarıçapın katı: bağlantı sayısından, çizge değişince bir kez
        std::size_t edges_version = SIZE_MAX;  // edge_names'in kurulduğu harita sürümü (MapModel::version)
        std::vector<std::string> edge_names;   // kenar başına ilişkinin adı, terminal_safe (F1); adsızda boş
    };
    MapCamera map_camera_;
    MapCache map_cache_;
    std::vector<MapLineBatch> map_lines_;
    std::optional<std::size_t>
        map_pressed_;  // basılı tutulan düğüm: bırakınca açılır, sürüklenince iğnelenir
    bool map_dragged_ = false;
    std::optional<std::size_t> map_link_from_;  // Shift+sürükleme sürüyor: kaynak düğüm (lastik çizgi)
    // Menünün düğümleri kopyadır: menü açıkken çizge yeniden kurulsa da işlem açıldığı kenara gider.
    struct MapPair {
        MapNode from;
        MapNode to;
        MapEdgeKind kind = MapEdgeKind::Related;
        core::RelationId relation = {};  // kaldırılacak ilişki (Related)
        std::string name = {};           // ilişkinin adı; güvenilmez, terminal_safe ile çizilir
        bool directed = false;           // ilişki yönlü: from kaynaktır
    };
    std::optional<MapPair> map_choice_;     // not → not bırakıldı: ilişki mi bağlantı mı
    std::optional<MapPair> map_edge_menu_;  // sağ tıklanan kenar
    void draw_relation_items(const MapPair& edge);  // kenar menüsünün ilişki öğeleri (F1)
    // Sağ tıklanan düğüm ve onun kaldırılabilir kenarları (ilişki, etiket): menü açılırken kopyalanır.
    struct MapNodeMenu {
        std::size_t generation{};  // açıldığı çizge: değiştiyse `node` başka düğümü gösterir
        std::size_t node{};
        MapNode subject;
        std::vector<MapPair> removable;
    };
    std::optional<MapNodeMenu> map_node_menu_;
    // "Bağla…" ya da `C`: kaynak düğüm; sonraki tıklanan düğüm hedeftir (Shift+sürüklemenin bırakması), Esc ya da
    // boşluğa tıklama vazgeçer.
    std::optional<std::size_t> map_connect_from_;
    // F2: kaynak seçildikten sonraki tıklamanın işi: bağla ya da en kısa yol (yönlü ya da değil).
    enum class MapPick { Connect, Path, DirectedPath };
    MapPick map_pick_ = MapPick::Connect;
    std::optional<app::LayoutPoint> map_canvas_menu_;  // boşluğa sağ tık: yerleşim noktası (yeni not oraya)
    bool map_focused_ = false;  // geçen karede tuval odaktaydı: Ctrl+F not içi bul değil haritada bul
    bool map_find_open_ = false;
    bool map_find_focus_ = false;  // satır açılınca ya da Enter'dan sonra odak satırda kalsın
    std::string map_find_;
    std::vector<std::size_t> map_found_;  // eşleşen düğümler (app::find_map_nodes), harita sırasıyla
    std::size_t map_found_at_ = 0;
    std::optional<std::size_t> map_focus_node_;  // kamera bu düğüme ortalanacak (tuvalin boyu bilinince)
    std::size_t map_found_version_ = SIZE_MAX;  // eşleşmelerin kurulduğu harita sürümü
    std::string map_tag_filter_;                // etiket süzgecinin kutusu (MapOptions::tag'a yazılır)
    bool map_query_focus_ = false;              // sorgu çubuğu açılınca ve çalıştırınca odak kutuda kalsın
    std::vector<std::string> map_view_names_;   // görünümler listesi açılınca okunur, kare başına değil
    std::vector<core::RelationName> map_relation_names_;  // seçici açılınca okunur, kare başına değil
    int map_zoom_keys_ = 0;                 // SDL tuş kodundan: '+' ve '-' klavye düzeninden bağımsız okunur
    bool map_animating_ = false;            // bu karede yerleşim sürdü: boşta beklemeden yeni kare
    bool close_requested_ = false;
    bool rescan_on_focus_ = false;  // diyalogda gelen odak da kaybolmaz: tarama kare dışında çalışır
    bool search_active_ = false;
    bool escape_pressed_ = false;        // bu karede Esc (genel rota): açık tamamlama listesini kapatır
    bool search_popup_hovered_ = false;  // fare tamamlama listesinde: kutu odağı bıraksa da liste kalır
    bool picker_shown_ = false;  // seçici açılır pencereyi geçen karede çizdi: kapanışı kullanıcı yaptı mı
    bool editor_active_ = false;
    bool preview_link_hovered_ = false;  // bu karede önizlemede bir bağlantının üstünde: çift tık düzenlemez
    bool editor_drawn_ = false;  // bu karede editör çizildi: alanları ve iletisi durum çubuğunda
    // Boşta bekleyen dil sunucusu değişikliğinin gönderileceği an (LanguageServers::Update::send_at).
    std::optional<std::chrono::steady_clock::time_point> lsp_send_at_;
    // Güncelleme penceresinin son çizilen düğmeleri ve Kur'un yeniden açılacağı an (kUpdateArmDelay); an
    // gelince tek kare çizilir (idle_step).
    std::vector<app::UpdateAction> update_actions_;
    std::optional<std::chrono::steady_clock::time_point> update_armed_at_;
    bool list_focused_ = false;
    bool refocus_editor_ = false;
    int scrolled_row_ = -1;
    std::string scrolled_item_;  // gezginde en son kaydırılan seçili satır (ad: yol, sıra değil)
    bool follow_selection_ = false;
    // Ayrıntılar'ın üst veri alanlarının genişlikleri (boyut, değiştirilme, oluşturulma, senkron, kaynak) ve
    // ölçüldükleri durum.
    std::array<float, kDetailsFields> details_widths_{};
    std::size_t details_widths_rebuilds_ = 0;
    float details_widths_font_ = 0.0F;
    const void* details_widths_face_ = nullptr;  // ImFont: Ayarlar'da aile değişince yeniden ölçülür
    std::string details_widths_day_;  // klavyeyle seçim değişti: gezinti imleci yeni seçili satıra gelsin
    // Editör sekmeleri: programatik geçiş ImGui'de bir kare gecikir.
    std::uint64_t drawn_note_tab_ = 0;
    bool note_tab_pending_ = false;
    FocusRequest pending_focus_ = FocusRequest::None;
    std::map<std::string, ItemRect> item_rects_;
    // İçe alma: diyalog geri çağrısı başka iş parçacığından, View kapandıktan sonra da gelebilir (paylaşılan
    // kutu); bırakılan dosyalar kare dışında içe alınır (diyalog açıkken gelenler de beklemez, kaybolmaz).
    std::shared_ptr<ImportInbox> import_inbox_;
    // Bekleyen ipucunun gecikmesinin dolduğu an (schedule_tooltip_wake).
    std::optional<std::chrono::steady_clock::time_point> tooltip_at_;
    int tooltip_frames_ = 0;  // gecikmesi dolan ipucunun art arda çizildiği kare (2'de durur; ilki gizli)
    // Sonraki yerel gece yarısı (core::next_local_midnight).
    std::chrono::system_clock::time_point day_ends_at_;
    std::vector<std::filesystem::path> dropped_;
};

}  // namespace noteapp::gui
