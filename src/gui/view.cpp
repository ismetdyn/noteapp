#include "gui/view.hpp"

#include "core/file_icon_table.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "gui/commands.hpp"
#include "gui/icon_font.hpp"
#include "gui/markdown_preview.hpp"
#include "gui/view_internal.hpp"
#include "pdf/fonts.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>  // Ctrl+Tab pencere geçişi (GImGui)
#include <optional>
#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

// Ölçek 1'deki alt sınırlar: en uzun Türkçe Ayarlar satırı ve araç çubuğu bu boyutta sığar; ölçekle çarpılır.
constexpr float kMinWindowWidth = 640.0F;
constexpr float kMinWindowHeight = 420.0F;
constexpr float kMinRightPaneWidth = 240.0F;
constexpr float kScaleRounding = 100.0F;
constexpr ImWchar kReplacementCodepoint = 0xFFFD;

std::string sdl_error(const char* step) {
    return std::string(step) + ": " + SDL_GetError();
}

// Harita yakınlaştırma tuşu. SDL tuş kodu değiştiriciyi içerir, ImGui'nin konuma bağlı tuşu içermez: Türkçe
// Q'da
// '+' Shift+4, '-' ise ABD '=' konumundadır.
int map_zoom_step(SDL_Keycode key) {
    switch (key) {
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
        return 1;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        return -1;
    default:
        break;
    }
    return 0;
}

// Gömülü dosya kaynağı: pdf::embedded_font (DejaVu), embedded_ui_font (Inter, JetBrains Mono),
// embedded_icon_font.
using EmbeddedFile = std::optional<std::string_view> (*)(std::string_view);

// Gömülü baytlar programın ömrü boyunca duran statik dizilerdir: atlas onları yerinde okur, kopyalamaz ve
// sahiplenmez (FontDataOwnedByAtlas = false: ImGui kapanışta IM_FREE etmez, imgui_draw.cpp
// ImFontAtlasFontSourceDestroy). ImGui imzası void* ister ama veriyi yalnız okur (stb_truetype const okur);
// const_cast bu yüzden güvenli. `merge` iken yüz önceki fonta eklenir: glif aranırken önce eklenen kaynak
// kazanır, eklenen yalnız eksiği doldurur. `ranges` fontla yaşamalı (ImGui dizinin kopyasını tutmaz). Ad
// (dosya) ImGui'nin font adıdır.
ImFont* add_font(EmbeddedFile source, const char* resource, bool merge = false,
                 const ImWchar* ranges = nullptr) {
    const auto data = source(resource);
    if (!data)
        return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.MergeMode = merge;
    config.GlyphRanges = ranges;
    std::snprintf(config.Name, sizeof(config.Name), "%s", resource);
    void* bytes = const_cast<char*>(data->data());
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(bytes, static_cast<int>(data->size()),
                                                              kFontPixels, &config);
    // DejaVu bidi ve sıfır genişlikli karakterlere boş, genişliksiz glif verir: editörde, önizlemede ve
    // adlarda görünmez kalırlardı. ImGui bidi uygulamaz; U+FFFD glifine yönlendirmek onları yerinde görünür
    // yapar ve InputText'in imleç ölçüsü glifin kendi genişliğini kullandığından tutarlı kalır. Baytlar
    // değişmez.
    if (font != nullptr && !merge)
        for (const core::HiddenControl& hidden : core::hidden_controls())
            font->AddRemapCodepoint(static_cast<ImWchar>(hidden.code_point), kReplacementCodepoint);
    return font;
}

// Seçilebilir yüz ve eksik glifleri için DejaVu yedeği (⟳ ✓ ⚠, kutu çizgileri, U+FFFD); biri yoksa ikisi de
// yok.
ImFont* add_font_with_fallback(const char* face, const char* dejavu) {
    ImFont* font = add_font(embedded_ui_font, face);
    return font != nullptr && add_font(pdf::embedded_font, dejavu, true) != nullptr ? font : nullptr;
}

// İkon alt kümesi son eklenen UI yüzüne birleşir; aralık yalnız tablodaki kod noktalarıdır.
bool merge_icons(const std::vector<ImWchar>& ranges) {
    return add_font(embedded_icon_font, "fonts/NoteappIcons.ttf", true, ranges.data()) != nullptr;
}

}  // namespace

void wake_event_loop() {
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);  // kuyruk doluysa diğer olaylar zaten uyandırır
}

std::expected<std::unique_ptr<View>, std::string> View::create(GuiState& state) {
    auto platform = std::make_unique<Platform>();
    if (!platform->sdl.ok)
        return std::unexpected(sdl_error("SDL_Init"));

    platform->window.reset(SDL_CreateWindow("noteapp", state.layout().width, state.layout().height,
                                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!platform->window)
        return std::unexpected(sdl_error("SDL_CreateWindow"));
    platform->renderer.reset(SDL_CreateRenderer(platform->window.get(), nullptr));
    if (!platform->renderer)
        return std::unexpected(sdl_error("SDL_CreateRenderer"));
    SDL_SetRenderVSync(platform->renderer.get(), 1);  // yazılımsal çizici desteklemezse sorun değil

    platform->imgui.emplace();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // imgui.ini geçerli dizini kirletmesin; yerleşim gui-state.toml'da
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Ctrl+Tab / Ctrl+Shift+Tab editör sekmelerinindir; tek kök pencerede ImGui'nin pencere geçişi iş görmez.
    GImGui->ConfigNavWindowingKeyNext = 0;
    GImGui->ConfigNavWindowingKeyPrev = 0;

    // Ayarlar → Görünüm'deki aileler açılışta eklenir; glif ilk kullanımda üretilir (dinamik atlas), seçim
    // değişince atlas yeniden kurulmaz. İlk font (Fonts[0]) DejaVu'dur, FontSizeBase ondan gelir. DejaVu'nun
    // kalın ve eğik yüzleri PDF ile aynı gömülü aileden. Inter'de olmayan glif (⟳ ✓ ⚠) DejaVu'dan, ikonlar
    // ikisine de.
    PreviewFonts& dejavu = platform->dejavu;
    dejavu.regular = add_font(pdf::embedded_font, "fonts/DejaVuSans.ttf");
    const bool dejavu_icons = dejavu.regular != nullptr && merge_icons(platform->icon_ranges);
    dejavu.mono = add_font(pdf::embedded_font, "fonts/DejaVuSansMono.ttf");
    dejavu.bold = add_font(pdf::embedded_font, "fonts/DejaVuSans-Bold.ttf");
    dejavu.italic = add_font(pdf::embedded_font, "fonts/DejaVuSans-Oblique.ttf");
    dejavu.bold_italic = add_font(pdf::embedded_font, "fonts/DejaVuSans-BoldOblique.ttf");
    PreviewFonts& inter = platform->inter;
    inter.regular = add_font(embedded_ui_font, "fonts/Inter-Regular.ttf");
    const bool inter_icons = inter.regular != nullptr && merge_icons(platform->icon_ranges) &&
                             add_font(pdf::embedded_font, "fonts/DejaVuSans.ttf", true) != nullptr;
    inter.bold = add_font_with_fallback("fonts/Inter-Bold.ttf", "fonts/DejaVuSans-Bold.ttf");
    inter.italic = add_font_with_fallback("fonts/Inter-Italic.ttf", "fonts/DejaVuSans-Oblique.ttf");
    inter.bold_italic =
        add_font_with_fallback("fonts/Inter-BoldItalic.ttf", "fonts/DejaVuSans-BoldOblique.ttf");
    platform->jetbrains_mono =
        add_font_with_fallback("fonts/JetBrainsMono-Regular.ttf", "fonts/DejaVuSansMono.ttf");
    const auto complete = [](const PreviewFonts& family) {
        return family.regular != nullptr && family.bold != nullptr && family.italic != nullptr &&
               family.bold_italic != nullptr;
    };
    if (!complete(dejavu) || dejavu.mono == nullptr || !dejavu_icons || !complete(inter) || !inter_icons ||
        platform->jetbrains_mono == nullptr)
        return std::unexpected("embedded UI font is missing");
    platform->sdl_backend.emplace(platform->window.get(), platform->renderer.get());
    if (!platform->sdl_backend->ok)
        return std::unexpected(sdl_error("ImGui_ImplSDL3_InitForSDLRenderer"));
    platform->renderer_backend.emplace(platform->renderer.get());
    if (!platform->renderer_backend->ok)
        return std::unexpected(sdl_error("ImGui_ImplSDLRenderer3_Init"));

    auto view = std::make_unique<View>(state, std::move(platform));
    view->apply_window_scale();  // temayı da uygular
    return view;
}

View::View(GuiState& state, std::unique_ptr<Platform> platform)
    : state_(state), platform_(std::move(platform)), preview_(std::make_unique<PreviewCache>()),
      frames_left_(kFramesAfterEvent), import_inbox_(std::make_shared<ImportInbox>()),
      day_ends_at_(core::next_local_midnight(std::chrono::system_clock::now())) {
    state_.set_wake(wake_event_loop);
}

View::~View() {
    state_.set_wake(nullptr);      // SDL kapanıyor; işçiler hâlâ post edebilir
    state_.deciders = Deciders{};  // kararlar `this` yakalar; GuiState View'dan uzun yaşar
}

float View::ui_scale_for(float display_scale, float pixel_density) {
    if (display_scale <= 0.0F || pixel_density <= 0.0F)  // SDL hata döndürdü: okunur varsayılan
        return 1.0F;
    // ImGui pencere koordinatlarında çizer, DisplayFramebufferScale pikseli zaten karşılar. Wayland 2x'te
    // SDL_GetWindowDisplayScale 2, yoğunluk 2'dir: ayrıca 2 ile çarpmak arayüzü 4 kat gösterirdi.
    // X11/Windows'ta yoğunluk 1'dir, ölçek doğrudan kullanılır. Kesirli yoğunluk tam sayı piksellerden
    // hesaplandığından boyutla oynar; yuvarlama her boyutlandırmada stilin yeniden kurulmasını önler.
    return std::round(display_scale / pixel_density * kScaleRounding) / kScaleRounding;
}

void View::apply_window_scale() {
    SDL_Window* window = platform_->window.get();
    set_ui_scale(ui_scale_for(SDL_GetWindowDisplayScale(window), SDL_GetWindowPixelDensity(window)));
}

void View::set_ui_scale(float scale) {
    if (scale == ui_scale_)
        return;
    ui_scale_ = scale;
    // ScaleAllSizes çarpımı birikir: yeni ölçek temiz stilden kurulur (ImGui örneği de böyle önerir).
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    theme::apply_style(style);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    apply_theme();
    if (!SDL_SetWindowMinimumSize(platform_->window.get(),
                                  static_cast<int>(std::ceil(kMinWindowWidth * scale)),
                                  static_cast<int>(std::ceil(kMinWindowHeight * scale))))
        core::log_warn(std::string("event=window_min_size_failed detail=") + SDL_GetError());
}

// Ayarlar → Görünüm'deki seçim (gui-state.toml); tanınmayan ad ilk aileye (DejaVu) düşer.
PreviewFonts View::fonts() const {
    const GuiLayout& layout = state_.layout();
    PreviewFonts chosen = layout.ui_font == kUiFonts[1] ? platform_->inter : platform_->dejavu;
    chosen.mono = layout.editor_font == kEditorFonts[1] ? platform_->jetbrains_mono : platform_->dejavu.mono;
    return chosen;
}

void View::apply_theme() {
    // Plan davranış 2: yalnız açık/koyu ayrımı izlenir.
    theme::apply_colors(ImGui::GetStyle(),
                        SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? theme::kDark : theme::kLight);
}

void View::pump_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // Diyalog açıkken de yalnız not edilir: önce diyalog yanıtlanır, kapanış akışı sonra sorar.
            close_requested_ = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            rescan_on_focus_ = true;
            break;
        case SDL_EVENT_SYSTEM_THEME_CHANGED:
            apply_theme();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:  // başka ekrana taşındı ya da ölçek ayarı değişti
            apply_window_scale();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            SDL_GetWindowSize(platform_->window.get(), &state_.layout().width, &state_.layout().height);
            break;
        case SDL_EVENT_KEY_DOWN:
            map_zoom_keys_ += map_zoom_step(event.key.key);
            break;
        case SDL_EVENT_DROP_FILE:  // dosya ya da klasör, bırakılan her öğe için bir olay (UTF-8)
            if (event.drop.data != nullptr) {
                const std::string_view dropped(event.drop.data);
                dropped_.emplace_back(std::u8string(dropped.begin(), dropped.end()));
            }
            break;
        default:
            break;
        }
    }
}

void View::render(const std::function<void()>& overlay) {
    pump_events();
    ImGui::GetIO().FontDefault = fonts().regular;  // Ayarlar'daki aile: NewFrame onunla başlar
    // TUI gibi her kareden önce: tanılar ve öneriler bu karede çizilsin.
    lsp_send_at_ = state_.update_language_servers(std::chrono::steady_clock::now());
    item_rects_.clear();  // yalnız bu karede çizilenler kalır: gizli sekmenin eski konumu test edilmesin
    // Harita bu karede çizilmezse yerleşim de beklemez.
    map_animating_ = false;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_root();
    if (overlay)
        overlay();
    schedule_tooltip_wake();
    ImGui::Render();
    SDL_Renderer* renderer = platform_->renderer.get();
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
    ++frames_;
}

void View::draw_frame() {
    render({});
    run_intents();
}

void View::queue(std::function<void()> intent) {
    if (modal_depth_ == 0)  // diyalog açıkken arka plan yalnız görüntüdür
        intents_.push_back(std::move(intent));
}

void View::queue_command(const GuiCommand& command) {
    queue([this, command] { run_gui_command(state_, command); });
}

void View::run_intents() {
    // Tarama da diyalog açabilir (gösterilen not silinmişse kaydet diyaloğu): kare dışında olmalı.
    if (std::exchange(rescan_on_focus_, false)) {
        state_.rescan();
        frames_left_ = kFramesAfterEvent;
    }
    // Niyet diyalog açabilir, o da kare üretir; takas yeniden girişte yinelemeyi önler.
    std::vector<std::function<void()>> intents;
    intents.swap(intents_);
    for (const auto& intent : intents)
        intent();
    if (!intents.empty())
        frames_left_ = kFramesAfterEvent;
    import_pending();
    if (state_.take_quit_request())  // `quit`: pencere kapatmayla aynı yol (kaydedilmemiş iş sorulur)
        close_requested_ = true;
    // Kare dışında: Ayrıntılar'da konum değişimi listeyi süzer, kare içinde çizilen satırlar bozulmasın.
    state_.reveal_selection();
}

int View::idle_timeout_ms() const {
    if (frames_left_ > 0 || map_animating_)
        return 0;
    // En yakın zamanlayıcıya dek uyunur: autosave ve dil sunucusuna gidecek değişiklik (süre dolunca
    // gönderilir).
    int timeout = kIdleWaitMs;
    const auto clamp_to = [&timeout](auto left) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
        timeout = static_cast<int>(std::clamp<decltype(ms)>(ms, 0, timeout));
    };
    const auto now = std::chrono::steady_clock::now();
    for (const auto& deadline : {state_.next_deadline(), lsp_send_at_, tooltip_at_})
        if (deadline)
            clamp_to(*deadline - now);
    clamp_to(day_ends_at_ - std::chrono::system_clock::now());
    return timeout;
}

// Boşta kare yok: fare gecikmeli ipucu olan bir öğede durursa ipucu hiç açılmazdı (ImGui gecikmeyi kareler
// arası gerçek süreden sayar). Uyanma her karede ImGui'nin kendi sayaçlarından kalan süreye kurulur: kareler
// yavaşsa (yük) ya da başka kaynaktan geliyorsa gecikme uyanmadan önce dolar, bekleyen uyanma kalmaz.
// HoverItemDelayId: bu karede gecikmeli IsItemHovered soran öğe (SetItemTooltip, BeginItemTooltip; hepsi
// ImGuiHoveredFlags_ForTooltip ile).
void View::schedule_tooltip_wake() {
    const ImGuiContext& g = *GImGui;
    const ImGuiID item = g.HoverItemDelayId;
    if (item == 0) {
        tooltip_frames_ = 0;
        tooltip_at_.reset();
        return;
    }
    const ImGuiStyle& style = g.Style;
    // IsItemHovered'ın ForTooltip seçimi ve CalcDelayFromHoveredFlags (imgui.cpp, static) ile aynı.
    const ImGuiHoveredFlags flags = g.NavHighlightItemUnderNav && g.NavCursorVisible
                                        ? style.HoverFlagsForTooltipNav
                                        : style.HoverFlagsForTooltipMouse;
    const float delay = (flags & ImGuiHoveredFlags_DelayNormal) != 0  ? style.HoverDelayNormal
                        : (flags & ImGuiHoveredFlags_DelayShort) != 0 ? style.HoverDelayShort
                                                                      : 0.0F;
    const bool unlocked =
        (flags & ImGuiHoveredFlags_Stationary) == 0 || g.HoverItemUnlockedStationaryId == item;
    const auto now = std::chrono::steady_clock::now();
    if (unlocked && g.HoverItemDelayTimer >= delay) {
        // İpucu bu karede çizildi. İlk karesi ölçümdür (gizli): görünmesi için hemen bir kare daha.
        tooltip_frames_ = std::min(tooltip_frames_ + 1, 2);
        if (tooltip_frames_ == 1)
            tooltip_at_ = now;
        else
            tooltip_at_.reset();
        return;
    }
    tooltip_frames_ = 0;
    float left = std::max(0.0F, delay - g.HoverItemDelayTimer);
    if (!unlocked)  // kilit, durma süresi dolduktan sonraki karenin başında açılır
        left = std::max(left, style.HoverStationaryDelay - g.MouseStationaryTimer);
    const std::chrono::duration<float> wait(left + kTooltipWakeMarginSeconds);
    tooltip_at_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(wait);
}

bool View::tooltip_visible() const {
    return tooltip_frames_ > 1;
}

bool View::tooltip_due(std::chrono::steady_clock::time_point now) {
    if (!tooltip_at_ || now < *tooltip_at_)
        return false;
    tooltip_at_.reset();
    return true;
}

bool View::caret_blinks() const {
    // ImGui metin alanı kırpma imleci için yavaş bir tempoda yenilenir (uyanma aralığı kadar). Not editörünün
    // imleci yanıp sönmez (her karede çizilir); IME için WantTextInput'u o da açar, boşta kare istemez.
    return ImGui::GetIO().WantTextInput && !editor_active_;
}

void View::idle_step(int timeout_ms) {
    // Olay kuyruktan alınmaz (NULL): draw_frame hepsini tek yerde işler.
    if (SDL_WaitEventTimeout(nullptr, timeout_ms))
        frames_left_ = kFramesAfterEvent;
    const auto now = std::chrono::steady_clock::now();
    if (state_.tick(now))
        frames_left_ = kFramesAfterEvent;
    if (tooltip_due(now))
        frames_left_ = kFramesAfterEvent;  // ipucu penceresi ilk karede ölçülür, sonrakinde görünür
    // Gün dönünce "bugün" saatleri (HH:MM) tarihe döner: bir kez çizilir. Duvar saatiyle: uyku sonrası da.
    if (const auto wall = std::chrono::system_clock::now(); wall >= day_ends_at_) {
        day_ends_at_ = core::next_local_midnight(wall);
        frames_left_ = kFramesAfterEvent;
    }
    const bool blinking = caret_blinks() && timeout_ms > 0;
    // Değişiklik render'da gönderilir (update_language_servers): süresi dolunca tek kare.
    const bool lsp_due = lsp_send_at_ && now >= *lsp_send_at_;
    if (frames_left_ > 0) {
        --frames_left_;
        draw_frame();
    } else if (blinking || lsp_due || map_animating_) {  // yerleşim durunca kare de durur: boşta CPU yok
        draw_frame();
    }
}

void View::run() {
    for (;;) {
        idle_step(idle_timeout_ms());
        if (close_requested_) {
            close_requested_ = false;
            if (state_.request_close())
                return;
            frames_left_ = kFramesAfterEvent;
        }
    }
}

void View::handle_shortcuts() {
    escape_pressed_ = false;
    if (modal_depth_ > 0)
        return;
    // Arama kutusunda Ctrl+P geçmiştir, Ctrl+N geçmişte gezerken öyle (TUI girdi satırı): draw_search_row
    // alır.
    // Editörün bul satırında Ctrl+P/Ctrl+N önceki/sonraki eşleşmedir (TUI): editor_keys alır.
    const auto search_line_key = [this](ImGuiKeyChord chord) {
        if (map_focused_ && chord == (ImGuiMod_Ctrl | ImGuiKey_F))  // F2: tuvalde haritada bul (view_map)
            return true;
        const bool previous_next = chord == (ImGuiMod_Ctrl | ImGuiKey_P) || chord == (ImGuiMod_Ctrl | ImGuiKey_N);
        if (editor_active_ && state_.editor().mode() != NoteEditor::Mode::Editing)
            return previous_next;
        return search_active_ && (chord == (ImGuiMod_Ctrl | ImGuiKey_P) ||
                                  (chord == (ImGuiMod_Ctrl | ImGuiKey_N) && state_.search_walking()));
    };
    for (const GuiCommand& command : kGuiCommands)
        if (command.scope == CommandScope::Global && ImGui::IsKeyChordPressed(command.chord) &&
            !search_line_key(command.chord))
            queue_command(command);
    if (ImGui::IsKeyChordPressed(kPaletteChord))
        queue([this] { state_.open_palette(); });
    // ImGui Escape'te odaklanıldığından beri yapılan düzenlemeyi geri alır; notta bu sessiz veri kaybıdır.
    // Rota alınınca InputText tuşu görmez. Diyaloglarda (modal_depth_ > 0) ve açılır menüde (sıralama,
    // seçici) Esc serbesttir: rota alınsa açılır pencere Esc'i görmez, kapanmazdı.
    escape_pressed_ =
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive);
}

void View::draw_root() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::Begin("##root", nullptr, kRootFlags);
    ImGui::PopStyleVar();
    handle_shortcuts();
    // Kısayollar önceki karenin değerini okudu; editör bu karede çizilmezse (çöp, klasör özeti, harita,
    // Ayarlar) odakta sayılmaz, yoksa arama kutusunun kırpma imleci duruyordu (caret_blinks).
    editor_active_ = false;
    editor_drawn_ = false;

    const FocusRequest focus = state_.take_focus_request();
    if (focus != FocusRequest::None)
        pending_focus_ = focus;

    // Durum çubuğu ikincil boydadır: payı da o boyun satırıdır.
    ImGui::PushFont(nullptr, kSmallFontPixels);
    const float status_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::PopFont();
    // Üst sekme satırı yok (K2): Ayarlar tüm içerik alanıdır, araç çubuğundaki ⚙ ya da Ctrl+, ile açılır.
    ImGui::BeginChild("##content", ImVec2(0.0F, -status_height));
    if (state_.tab() == Tab::Settings)
        draw_settings_tab();
    else
        draw_notes_tab();
    ImGui::EndChild();
    draw_status_bar();
    ImGui::End();
    draw_notices(status_height);
}

void View::draw_notes_tab() {
    draw_toolbar();
    float canvas_left = ImGui::GetCursorScreenPos().x;
    if (!state_.zoomed()) {  // pane.zoom: sağ panel tüm genişlikte
        draw_left_pane();
        // Bölmeler çizgiyle değil tonla ayrılır (sol krom, sağ tuval); aradaki 1 px, sol bölmenin
        // boyutlandırma kenarıdır (üzerine gelince ImGui SeparatorHovered ile vurgular).
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, max.y),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        canvas_left = max.x + 1.0F;
        ImGui::SameLine();
    }
    draw_right_pane(canvas_left);
}

void View::draw_left_pane() {
    GuiLayout& layout = state_.layout();
    // Harita kendi genişliğini tutar: dar ağaç bölmesine sığmaz, geniş harita da ağacı genişletmesin. Geçişte
    // geçen karenin boyutu öbür görünümündür, kullanıcı değişikliği sayılmasın.
    const bool map = state_.browser_view() == BrowserView::Map && !state_.trash_view();
    float& stored_width = map ? layout.map_pane_width : layout.left_pane_width;
    std::optional<float>& last_size = map ? map_pane_size_ : left_pane_size_;
    if (std::exchange(map_pane_, map) != map)
        last_size.reset();
    // Kayıtlı değer ölçeksizdir. ImGui ResizeX boyutunu yalnız ilk kullanımda alır; sonrası
    // reconcile_pane_size ile uygulanır. Kısıt olmasa büyük bölme sağ paneli pencere dışına iterdi.
    const float min_width = GuiLayout::kMinLeftPaneWidth * ui_scale_;
    const float max_width =
        std::max(min_width, ImGui::GetContentRegionAvail().x - kMinRightPaneWidth * ui_scale_);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0F), ImVec2(max_width, FLT_MAX));
    ImGui::BeginChild("##left", ImVec2(stored_width * ui_scale_, 0.0F), ImGuiChildFlags_ResizeX);
    const float actual_width = ImGui::GetWindowWidth();
    const float target_width =
        reconcile_pane_size(stored_width, last_size, actual_width, min_width, max_width);
    if (std::abs(actual_width - target_width) >= 1.0F)
        ImGui::SetWindowSize(ImVec2(target_width, ImGui::GetWindowHeight()));
    draw_search_row();
    draw_picker();
    // Bozuk sorgu argümanı: TUI'nin metni kutunun altında, liste son geçerli haliyle kalır.
    if (const std::optional<std::string>& error = state_.query_error()) {
        draw_error(*error);
        remember_item_center("search_error");
    }
    if (map) {
        draw_map();
    } else {
        map_focused_ = false;
        draw_note_list();
    }
    ImGui::EndChild();
    remember_item_center("left_pane");
}

// ImGui kısıta kırptığı boyutu saklar, kısıt gevşeyince geri açmaz; kayıtlı değer bu yüzden kullanıcının tek
// kaynağıdır. Boyut, geçen karede uygulananın bu karenin kısıtına kırpılmış hâlinden farklıysa onu kullanıcı
// değiştirmiştir (sürükleme ya da kenara çift tıklama). Kısıt ve ölçek değişimi bu farkı üretmez. Eşik 1
// piksel: ImGui boyutu tam sayıya keser, ölçekle çarpıp geri bölmenin payı bölmeyi kaydırmasın.
float View::reconcile_pane_size(float& stored, std::optional<float>& last_size, float actual, float min_size,
                                float max_size) const {
    if (last_size && std::abs(actual - std::clamp(*last_size, min_size, max_size)) >= 1.0F)
        stored = actual / ui_scale_;
    last_size = std::clamp(stored * ui_scale_, min_size, max_size);
    return *last_size;
}

// Tek durum çubuğu (G7), öncelikli yerleşim: solda sayaç ve tek ileti, en önemlisi (hata > uyarı > bilgi;
// eşitte genel ileti, senkron, editör sırası); kısalırsa tamamı ipucunda. Öbür iletiler bir sayı çipine ("⚠
// 2") toplanır, tam metinleri çipin ipucunda. Sağda editör alanları ve kısa senkron göstergesi hiç kısalmaz.
// 13 px, üstte 1 px.
void View::draw_status_bar() {
    namespace icons = core::icon_table;
    const theme::Theme& colors = theme::current();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float top_line = std::floor(top.y - (ImGui::GetStyle().ItemSpacing.y / 2.0F));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, top_line),
                                        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), top_line),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::PushFont(nullptr, kSmallFontPixels);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.muted);
    const std::string warning_glyph = core::icon_text(icons::kWarning, state_.icon_style());
    const std::string warning_mark = warning_glyph.empty() ? std::string("⚠") : warning_glyph;

    // Senkron etiketi ⟳ / ✓ / ⚠ ile başlar: simgeler açıkken baştaki sembol aynı anlamın simgesiyle değişir.
    std::string sync = state_.sync_label();
    ImU32 sync_color = colors.muted;
    core::FileIcon sync_icon{};
    switch (state_.sync_indicator()) {
    case SyncIndicator::Disabled:
        break;
    case SyncIndicator::Running:
        sync_color = colors.accent;
        sync_icon = icons::kSync;
        break;
    case SyncIndicator::Ok:
        sync_color = colors.success;
        sync_icon = icons::kSyncOk;
        break;
    case SyncIndicator::Failed:
        sync_color = colors.danger;
        sync_icon = icons::kWarning;
        break;
    }
    if (const std::string glyph = core::icon_text(sync_icon, state_.icon_style());
        sync_icon.nerd != 0 && !glyph.empty() && !sync.empty())
        sync = glyph + sync.substr(utf8_length(sync.front()));

    std::vector<StatusMessage> messages;
    if (const std::string& status = state_.status(); !status.empty()) {
        const bool error = state_.status_is_error();
        messages.push_back({error && !warning_glyph.empty() ? warning_glyph + " " + status : status,
                            error ? colors.danger : colors.muted,
                            error ? StatusRank::Error : StatusRank::Info});
    }
    const bool sync_error = state_.sync_indicator() == SyncIndicator::Failed;
    if (sync_error)
        messages.push_back({sync, colors.danger, StatusRank::Error});
    if (editor_drawn_) {
        if (StatusMessage notice = editor_notice(); !notice.text.empty())
            messages.push_back(std::move(notice));
    }
    std::ranges::stable_sort(messages, std::ranges::greater{}, &StatusMessage::rank);

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const std::string count = state_.count_label();
    const bool can_undo = state_.can_undo_trash();
    const std::string undo_label = core::tr("gui.undo_trash");
    const bool importing = state_.importing();  // ilerleme iletidedir; Durdur yanında
    const std::string cancel_label = core::tr("gui.import_cancel");
    const std::string fields = editor_drawn_ ? state_.editor_fields() : std::string{};
    // Senkron hatası iletilerdedir; sağda yalnız işareti kalır (sığar, ipucunda tamamı).
    const std::string sync_short = sync_error ? warning_mark : sync;
    std::string more;
    if (messages.size() > 1) {
        const bool serious =
            std::ranges::any_of(messages.begin() + 1, messages.end(),
                                [](const StatusMessage& m) { return m.rank != StatusRank::Info; });
        more = (serious ? warning_mark : std::string("+")) + " " + std::to_string(messages.size() - 1);
    }
    const float right_width = (fields.empty() ? 0.0F : text_width(fields) + spacing) + text_width(sync_short);
    const float fixed = text_width(count) + (more.empty() ? 0.0F : text_width(more) + spacing) +
                        (can_undo ? button_width(undo_label) + spacing : 0.0F) +
                        (importing ? button_width(cancel_label) + spacing : 0.0F) + right_width + spacing;
    const float message_room = ImGui::GetContentRegionAvail().x - fixed - spacing;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(count.c_str());
    remember_item_center("count");
    if (!messages.empty() && message_room > 0.0F) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, messages.front().color);
        draw_fitted_text("status", messages.front().text, message_room, keep_start);
        ImGui::PopStyleColor();
    }
    if (!more.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, more.starts_with('+') ? colors.muted : colors.warning);
        ImGui::TextUnformatted(more.c_str());
        ImGui::PopStyleColor();
        remember_item_center("status_more");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(dialog_content_width());
            for (auto message = messages.begin() + 1; message != messages.end(); ++message) {
                ImGui::PushStyleColor(ImGuiCol_Text, message->color);
                ImGui::TextUnformatted(message->text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (can_undo) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
        if (button("status_undo", undo_label, ButtonKind::Ghost))
            queue([this] { state_.undo_trash(); });
        ImGui::PopStyleColor();
    }
    if (importing) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
        if (button("status_cancel_import", cancel_label, ButtonKind::Ghost))
            queue([this] { state_.cancel_import(); });
        ImGui::PopStyleColor();
    }
    const float right = ImGui::GetContentRegionMax().x;
    if (!fields.empty()) {
        ImGui::SameLine(right - right_width);
        ImGui::TextUnformatted(fields.c_str());
        remember_item_center("editor_status");
    }
    ImGui::SameLine(right - text_width(sync_short));
    ImGui::PushStyleColor(ImGuiCol_Text, sync_color);
    ImGui::TextUnformatted(sync_short.c_str());
    ImGui::PopStyleColor();
    remember_item_center("sync");
    if (sync_short != sync)
        full_text_tooltip(sync);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

}  // namespace noteapp::gui
