#pragma once

// view*.cpp dosyalarının ortak iç parçaları: View'un .cpp'de tanımlı alt yapıları, birden çok panelin
// kullandığı sabitler ve çizim yardımcıları. Yalnız src/gui/view*.cpp içerir; tanımlar view_widgets.cpp'de
// (wake_event_loop: view.cpp).

#include "core/file_icons.hpp"
#include "edit/syntax.hpp"
#include "gui/icon_font.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/text_selection.hpp"
#include "gui/theme.hpp"
#include "gui/view.hpp"
#include "pdf/markdown_blocks.hpp"
#include "search/content_index.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace noteapp::gui {

// Boşta en fazla bu aralıkla uyanılır (kırpma imleci, zamanlayıcı); kare yalnız iş varsa üretilir.
inline constexpr int kIdleWaitMs = 500;
// İpucu uyanması ImGui gecikmesinden bu kadar sonra: o karede süre kesin dolmuş olsun (kare zamanı kayar).
inline constexpr float kTooltipWakeMarginSeconds = 0.05F;
// ImGui modal arka planını DeltaTime * 6 hızla karartır (1/6 sn'de tam); açılıştan bu kadar sonra tek kare
// onu tamamlar (o karenin DeltaTime'ı aradaki süredir).
inline constexpr std::chrono::milliseconds kModalDimWake{220};

// `timeout_ms`yi `deadline`a dek kısaltır; geçmiş süre 0, süre yoksa olduğu gibi.
inline int shorten_wait(int timeout_ms, std::optional<std::chrono::steady_clock::time_point> deadline,
                        std::chrono::steady_clock::time_point now) {
    if (!deadline)
        return timeout_ms;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
    return static_cast<int>(std::clamp<decltype(left)>(left, 0, timeout_ms));
}
// Tipografi, ölçek 1 pikseli (.engineering/design/ui-redesign.md §3.2; NY-G'de bir kademe büyüdü, kullanıcı
// isteği): gövde 16; bilgi yoğun metin (durum çubuğu, satırdaki boyut ve tarih, ipucu) bir kademe küçük;
// önizlemenin gövdesi okuma için bir kademe büyük. İkincil boy PushFont(nullptr, kSmallFontPixels) ile alınır:
// aynı font, ek atlas yok.
inline constexpr float kFontPixels = 16.0F;
inline constexpr float kSmallFontPixels = 14.0F;
inline constexpr float kPreviewFontPixels = 17.0F;
inline constexpr float kTitleFontPixels = 18.0F;  // sayfa başlığı (Ayarlar)
// ImGui bir etkileşimi birkaç karede tamamlar (fare üzerinde, odak, açılır kutu).
inline constexpr int kFramesAfterEvent = 3;
inline constexpr float kFormWidth = 420.0F;
// Pencere en küçük boyutun da altındaysa (bileşikçi sınırı uygulamayabilir) alan ve yazı bu kadar kalır.
// Negatif genişlik ImGui'de "sağdan pay" demektir, sarma genişliği 1 piksele iner.
inline constexpr float kMinFieldEms = 4.0F;

using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;

struct SdlSession {
    bool ok = SDL_Init(SDL_INIT_VIDEO);
    ~SdlSession() { SDL_Quit(); }
};

struct ImguiSession {
    ImguiSession() { ImGui::CreateContext(); }
    ~ImguiSession() { ImGui::DestroyContext(); }
    ImguiSession(const ImguiSession&) = delete;
    ImguiSession& operator=(const ImguiSession&) = delete;
};

struct SdlBackend {
    bool ok;
    SdlBackend(SDL_Window* window, SDL_Renderer* renderer)
        : ok(ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {}
    ~SdlBackend() {
        if (ok)
            ImGui_ImplSDL3_Shutdown();
    }
    SdlBackend(const SdlBackend&) = delete;
    SdlBackend& operator=(const SdlBackend&) = delete;
};

struct RendererBackend {
    bool ok;
    explicit RendererBackend(SDL_Renderer* renderer) : ok(ImGui_ImplSDLRenderer3_Init(renderer)) {}
    ~RendererBackend() {
        if (ok)
            ImGui_ImplSDLRenderer3_Shutdown();
    }
    RendererBackend(const RendererBackend&) = delete;
    RendererBackend& operator=(const RendererBackend&) = delete;
};

// Yıkım sırası bildirimin tersidir: arka uçlar → ImGui → çizici → pencere → SDL.
struct View::Platform {
    SdlSession sdl;
    WindowPtr window{nullptr, &SDL_DestroyWindow};
    RendererPtr renderer{nullptr, &SDL_DestroyRenderer};
    std::optional<ImguiSession> imgui;
    std::optional<SdlBackend> sdl_backend;
    std::optional<RendererBackend> renderer_backend;
    // Seçilebilir aileler (View::fonts seçer): DejaVu'nun eş aralıklısı kendi `mono`'sudur, Inter'inki
    // boştur.
    PreviewFonts dejavu;
    PreviewFonts inter;
    ImFont* jetbrains_mono = nullptr;
    std::vector<ImWchar> icon_ranges = icon_glyph_ranges();
};

// Önizlemenin ayrıştırma ve yerleşim önbelleği: kare başına yalnız görünen parçalar ve koşular çizilir.
// Parçalar metin değişince, yerleşim genişlik ya da yazı boyu değişince yeniden kurulur.
struct View::PreviewCache {
    std::string source;  // `document` buna işaret eder
    // Parça parça yerleşim; yazı ailesi (Ayarlar), boy ya da genişlik değişince kendisi yeniden yerleştirir.
    PreviewDocument document;
    // Yerleşimin kurulduğu andaki eksik hedefler: senkron ya da kayıt hedefi oluşturunca bağlantı canlanır.
    std::vector<std::string> missing;
    // csv/json: metin ya da tür değişince yeniden dizinlenir; nullopt = geçersiz, ham metin. Dizin satır başına
    // (csv) ya da kontrol noktası başına (json) küçüktür; hücre ve satır metni çizerken kurulur.
    std::string data_source;
    app::PreviewKind data_kind = app::PreviewKind::None;
    std::optional<app::CsvIndex> csv;
    std::optional<app::JsonIndex> json;
    // Fareyle seçim, bütün metin önizlemelerinde (md, csv, json, ham metin). Gösterilen metin (sekme, tür, boy,
    // ham/biçimli) değişince kalkar; md'de yerleşim (genişlik, yazı boyu) değişince de: anahtarı koşu sırasıdır.
    PreviewSelection selection;
    std::tuple<std::uint64_t, app::PreviewKind, std::size_t, bool, bool> selection_source;
    std::pair<float, float> selection_layout;
    // PDF: oturumun (GuiState::pdf_serial) ve çizim ölçeğinin sayfa dokuları; ikisinden biri değişince atılır.
    // limits.pdf.cache_pages'le sınırlı, görünen sayfalar sayılmaz (en uzun süredir görülmeyen düşer). Dokular
    // platform_'un çizicisinden önce yıkılır (View'da preview_ ondan sonra bildirilir).
    struct PdfTexture {
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture{nullptr, &SDL_DestroyTexture};
        float width = 0.0F;  // piksel
        float height = 0.0F;
        std::size_t used = 0;  // son çizildiği kare
        std::string error;     // doku kurulamadı: sayfanın yerinde yerelleştirilmiş ileti (SDL metni yalnız logda)
    };
    std::map<int, PdfTexture> pdf_textures;
    std::uint64_t pdf_session = 0;
    float pdf_scale = 0.0F;
    ImVec2 pdf_page_size{};                       // ilk çizilen sayfanın boyu: bütün sayfaların yuvası
    std::pair<int, int> pdf_visible{-1, -1};      // geçen karenin görünen sayfaları [ilk, son]
};

// İçe alma diyaloğunun sonucu (SDL geri çağrısı yazar, View kare dışında boşaltır).
struct ImportInbox {
    std::mutex mutex;
    std::vector<std::filesystem::path> paths;
    std::string error;  // SDL_GetError: diyalog açılamadı (portal/zenity yok)
};

void wake_event_loop();
// UTF-8 öncü baytına göre kod noktasının bayt sayısı (devam baytında 1).
std::size_t utf8_length(char lead);
void draw_error(const std::string& message);
float text_width(std::string_view text);
float button_width(const std::string& label);
std::string keep_start(std::string_view text, float width);
std::string keep_end(std::string_view text, float width);
float dialog_content_width();
void full_text_tooltip(const std::string& text);
bool flow_after_previous(float next_width, float row_start_x, float row_width);
// Satırı sağa yaslar: `width` genişliğindeki öğeler `room` içinde sağda biter; sığmazsa satır soldan başlar.
void align_right(float width, float room);
void draw_hint(const std::string& text);
std::string printable(std::string_view raw);
// ImGui etiketteki `##` ve `###`'ı kimlik ayırıcısı sayar, ötesini çizmez: addan ya da yoldan gelen her `#`
// görünür U+FF03 olur (senkronla gelen ad menüde gerçek yolu gizleyemesin). Yalnız etikete giren değerler
// içindir; biçim dizgisine girmeyen metin (TextUnformatted, AddText) bunu gerektirmez.
std::string label_safe(std::string_view text);
ImU32 icon_color(core::IconColor color);
// Sözdizimi türünün rengi (TUI'nin 16 renk eşlemesinin tonları); Plain metin rengidir.
// Satır, aralıklarının renkleriyle tek satırda; aralık sınırı kod noktası başına çekilir, metin printable.
void draw_token_line(std::string_view text, const std::vector<tui::SyntaxSpan>& spans);
float draw_icon(ImVec2 position, const core::FileIcon& icon, core::IconStyle style, bool selected);
bool draw_fitted_row_text(ImVec2 position, const std::string& text, float width,
                          const std::vector<std::size_t>& highlights,
                          ImU32 color = ImGui::GetColorU32(ImGuiCol_Text));
// Satırın isteğe bağlı parçaları.
struct RowExtras {
    const search::Hit* best_line =
        nullptr;                      // verilirse satır iki satır boyunda, ikincisi en iyi eşleşen satır
    bool marked = false;              // çoklu seçimde
    std::string_view icon_path = {};  // boş değilse ikon ondan (ilişki satırı: metin ok ve ad da taşır)
    // Satırın sağında soluk ikincil metin (ilişkiyle bulunan arama satırının nedeni); satır boyu değişmez.
    std::string secondary = {};
};
bool plain_selectable(const std::string& text, bool selected, const std::vector<std::size_t>& highlights = {},
                      core::IconStyle icon_style = core::IconStyle::Off, const RowExtras& extras = {});
// Yukarı/Aşağı tek satırlık alanın geçmiş olayıdır: seçim listede yürür (palet, ilişki adı penceresi).
// `selected` SIZE_MAX ise (seçim yok) Aşağı ilk satırı seçer.
struct PaletteKeys {
    std::size_t* selected;
    std::size_t count;
};
int palette_callback(ImGuiInputTextCallbackData* data);
// Son öğenin zeminini çoklu seçim rengiyle boyar; metinden önce çağrılır.
inline constexpr float kMarkBarEms = 0.25F;
void draw_mark_background();
bool combo_field(const char* id, const std::string& label, std::string& value,
                 const std::vector<std::string>& items, bool root_first, const char* hint = "");
bool new_tab_click(bool clicked);

}  // namespace noteapp::gui
