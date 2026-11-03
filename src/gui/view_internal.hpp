#pragma once

// view*.cpp dosyalarının ortak iç parçaları: View'un .cpp'de tanımlı alt yapıları, birden çok panelin
// kullandığı sabitler ve çizim yardımcıları. Yalnız src/gui/view*.cpp içerir; tanımlar view_widgets.cpp'de
// (wake_event_loop: view.cpp).

#include "core/file_icons.hpp"
#include "edit/syntax.hpp"
#include "gui/icon_font.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/view.hpp"
#include "pdf/markdown_blocks.hpp"
#include "search/content_index.hpp"

#include <SDL3/SDL.h>
#include <cstddef>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::gui {

// Boşta en fazla bu aralıkla uyanılır (kırpma imleci, zamanlayıcı); kare yalnız iş varsa üretilir.
inline constexpr int kIdleWaitMs = 500;
// ImGui bir etkileşimi birkaç karede tamamlar (fare üzerinde, odak, açılır kutu).
inline constexpr int kFramesAfterEvent = 3;
inline constexpr float kFormWidth = 420.0F;
// Pencere en küçük boyutun da altındaysa (bileşikçi sınırı uygulamayabilir) alan ve yazı bu kadar kalır.
// Negatif genişlik ImGui'de "sağdan pay" demektir, sarma genişliği 1 piksele iner.
inline constexpr float kMinFieldEms = 4.0F;
inline constexpr ImVec4 kErrorColor(0.75F, 0.22F, 0.17F, 1.0F);  // açık ve koyu temada okunur kırmızı

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
    ImFont* ui_font = nullptr;
    ImFont* mono_font = nullptr;
    PreviewFonts preview_fonts;
    std::vector<ImWchar> icon_ranges = icon_glyph_ranges();
};

// Önizlemenin ayrıştırma ve yerleşim önbelleği: kare başına yalnız görünen koşular çizilir. Bloklar metin
// değişince, yerleşim genişlik ya da yazı boyu değişince yeniden kurulur.
struct View::PreviewCache {
    std::string source;
    std::vector<pdf::Block> blocks;
    PreviewLayout layout;
    float width = -1.0F;
    float font_size = 0.0F;
    // Yerleşimin kurulduğu andaki eksik hedefler: senkron ya da kayıt hedefi oluşturunca bağlantı canlanır.
    std::vector<std::string> missing;
    // csv/json: metin, tür ya da tablo sınırı değişince yeniden ayrıştırılır; nullopt = geçersiz, ham metin.
    std::string data_source;
    app::PreviewKind data_kind = app::PreviewKind::None;
    app::TableLimits data_limits;
    std::optional<app::CsvTable> csv;
    std::optional<std::vector<app::JsonLine>> json;
};

void wake_event_loop();
void draw_error(const std::string& message);
float text_width(std::string_view text);
float button_width(const std::string& label);
std::string keep_start(std::string_view text, float width);
std::string keep_end(std::string_view text, float width);
float dialog_content_width();
void full_text_tooltip(const std::string& text);
bool flow_after_previous(float next_width, float row_start_x, float row_width);
void draw_hint(const std::string& text);
std::string printable(std::string_view raw);
ImU32 icon_color(core::IconColor color);
// Sözdizimi türünün rengi (TUI'nin 16 renk eşlemesinin tonları); Plain metin rengidir.
ImU32 token_color(tui::TokenKind kind);
// Satır, aralıklarının renkleriyle tek satırda; aralık sınırı kod noktası başına çekilir, metin printable.
void draw_token_line(std::string_view text, const std::vector<tui::SyntaxSpan>& spans);
float draw_icon(ImVec2 position, const core::FileIcon& icon, core::IconStyle style, bool selected);
bool draw_fitted_row_text(ImVec2 position, const std::string& text, float width,
                          const std::vector<std::size_t>& highlights,
                          ImU32 color = ImGui::GetColorU32(ImGuiCol_Text));
bool plain_selectable(const std::string& text, bool selected, const std::vector<std::size_t>& highlights = {},
                      core::IconStyle icon_style = core::IconStyle::Off,
                      const search::Hit* best_line = nullptr, bool marked = false);
// Son öğenin zeminini çoklu seçim rengiyle boyar; metinden önce çağrılır.
inline constexpr float kMarkBarEms = 0.25F;
void draw_mark_background();
bool combo_field(const char* id, const std::string& label, std::string& value,
                 const std::vector<std::string>& items, bool root_first, const char* hint = "");
bool new_tab_click(bool clicked);

}  // namespace noteapp::gui
