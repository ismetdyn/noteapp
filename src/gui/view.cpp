#include "gui/view.hpp"

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
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>  // Ctrl+Tab pencere geçişi (GImGui)
#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr float kFontPixels = 16.0F;
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

// Atlas kendi kopyasına sahip olur: gömülü veri const'tur, ImGui imzası void* ister; const_cast yerine kopya.
// DejaVu bidi ve sıfır genişlikli karakterlere boş, genişliksiz glif verir: editörde, önizlemede ve adlarda
// görünmez kalırlardı. ImGui bidi uygulamaz; U+FFFD glifine yönlendirmek onları yerinde görünür yapar ve
// InputText'in imleç ölçüsü glifin kendi genişliğini kullandığından tutarlı kalır. Baytlar değişmez.
ImFont* load_font(const char* resource) {
    const auto font = pdf::embedded_font(resource);
    if (!font)
        return nullptr;
    void* owned = IM_ALLOC(font->size());
    std::memcpy(owned, font->data(), font->size());
    ImFont* loaded =
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(font->size()), kFontPixels);
    if (loaded != nullptr)
        for (const core::HiddenControl& hidden : core::hidden_controls())
            loaded->AddRemapCodepoint(static_cast<ImWchar>(hidden.code_point), kReplacementCodepoint);
    return loaded;
}

// Aralıklar yalnız tablodaki kod noktalarıdır; alt küme fontu zaten yalnız onları taşır, atlas da yalnız
// onları ister. `ranges` fontla yaşar (ImGui dizinin kopyasını tutmaz).
bool merge_icon_font(const std::vector<ImWchar>& ranges) {
    const auto font = embedded_icon_font("fonts/NoteappIcons.ttf");
    if (!font)
        return false;
    void* owned = IM_ALLOC(font->size());
    std::memcpy(owned, font->data(), font->size());
    ImFontConfig config;
    config.MergeMode = true;
    config.GlyphRanges = ranges.data();
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(font->size()), kFontPixels,
                                                      &config) != nullptr;
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

    platform->ui_font = load_font("fonts/DejaVuSans.ttf");
    const bool has_icons = merge_icon_font(platform->icon_ranges);
    platform->mono_font = load_font("fonts/DejaVuSansMono.ttf");
    // Önizlemenin kalın ve eğik yüzleri PDF ile aynı gömülü aileden; atlas glifleri ilk kullanımda üretir.
    PreviewFonts& preview = platform->preview_fonts;
    preview = {platform->ui_font, load_font("fonts/DejaVuSans-Bold.ttf"), load_font("fonts/DejaVuSans-Oblique.ttf"),
               load_font("fonts/DejaVuSans-BoldOblique.ttf"), platform->mono_font};
    if (platform->ui_font == nullptr || !has_icons || platform->mono_font == nullptr || preview.bold == nullptr ||
        preview.italic == nullptr || preview.bold_italic == nullptr)
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
      frames_left_(kFramesAfterEvent), import_inbox_(std::make_shared<ImportInbox>()) {
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
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    apply_theme();
    if (!SDL_SetWindowMinimumSize(platform_->window.get(),
                                  static_cast<int>(std::ceil(kMinWindowWidth * scale)),
                                  static_cast<int>(std::ceil(kMinWindowHeight * scale))))
        core::log_warn(std::string("event=window_min_size_failed detail=") + SDL_GetError());
}

void View::apply_theme() {
    // Plan davranış 2: yalnız açık/koyu ayrımı izlenir.
    if (SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK)
        ImGui::StyleColorsDark();
    else
        ImGui::StyleColorsLight();
    // Varsayılan yarı saydam (koyu temada %94): arkadaki yazı diyalog metninin içinde hayalet gibi görünürdü.
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w = 1.0F;
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
    // TUI gibi her kareden önce: tanılar ve öneriler bu karede çizilsin.
    lsp_waiting_ = state_.update_language_servers(std::chrono::steady_clock::now());
    item_rects_.clear();  // yalnız bu karede çizilenler kalır: gizli sekmenin eski konumu test edilmesin
    // Harita bu karede çizilmezse yerleşim de beklemez.
    map_animating_ = false;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_root();
    if (overlay)
        overlay();
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
    queue([this, run = command.run] { run(state_); });
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
    // Dil sunucusuna gidecek değişiklik boşta bekliyor: süre dolunca gönderilsin diye sık uyanılır.
    int timeout = lsp_waiting_ ? kLspPollMs : kIdleWaitMs;
    if (const auto deadline = state_.next_deadline()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline - std::chrono::steady_clock::now());
        timeout = std::clamp(static_cast<int>(left.count()), 0, timeout);
    }
    return timeout;
}

void View::idle_step(int timeout_ms) {
    // Olay kuyruktan alınmaz (NULL): draw_frame hepsini tek yerde işler.
    if (SDL_WaitEventTimeout(nullptr, timeout_ms))
        frames_left_ = kFramesAfterEvent;
    if (state_.tick(std::chrono::steady_clock::now()))
        frames_left_ = kFramesAfterEvent;
    // Etkin metin alanı kırpma imleci için yavaş bir tempoda yenilenir (uyanma aralığı kadar).
    const bool blinking = ImGui::GetIO().WantTextInput && timeout_ms > 0;
    if (frames_left_ > 0) {
        --frames_left_;
        draw_frame();
    } else if (blinking || map_animating_) {  // yerleşim durunca kare de durur: boşta CPU yok
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

    const FocusRequest focus = state_.take_focus_request();
    if (focus != FocusRequest::None)
        pending_focus_ = focus;

    ImGui::BeginChild("##content", ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing()));
    if (ImGui::BeginTabBar("##tabs")) {
        // Programatik geçiş (Ctrl+,) ImGui'de bir kare gecikir: o arada görünen eski sekme kullanıcı seçimi
        // sanılmasın.
        const Tab wanted = state_.tab();
        if (wanted != drawn_tab_)
            tab_switch_pending_ = true;
        const auto tab_flags = [&](Tab tab) -> ImGuiTabItemFlags {
            return tab_switch_pending_ && tab == wanted ? ImGuiTabItemFlags_SetSelected
                                                        : ImGuiTabItemFlags_None;
        };
        const auto on_visible = [&](Tab tab) {
            if (!tab_switch_pending_)
                state_.set_tab(tab);
            else if (tab == wanted)
                tab_switch_pending_ = false;
            drawn_tab_ = tab_switch_pending_ ? drawn_tab_ : tab;
        };
        const std::string notes_label = core::tr("gui.tab_notes") + "###notes";
        const std::string settings_label = core::tr("gui.tab_settings") + "###settings";
        if (ImGui::BeginTabItem(notes_label.c_str(), nullptr, tab_flags(Tab::Notes))) {
            on_visible(Tab::Notes);
            draw_notes_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(settings_label.c_str(), nullptr, tab_flags(Tab::Settings))) {
            on_visible(Tab::Settings);
            draw_settings_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    draw_status_bar();
    ImGui::End();
}

void View::draw_notes_tab() {
    draw_toolbar();
    if (!state_.zoomed()) {  // pane.zoom: sağ panel tüm genişlikte
        draw_left_pane();
        ImGui::SameLine();
    }
    draw_right_pane();
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
    if (map)
        draw_map();
    else
        draw_note_list();
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

void View::draw_status_bar() {
    const std::string count = state_.count_label();
    const std::string& status = state_.status();
    const std::string& sync = state_.sync_label();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool can_undo = state_.can_undo_trash();
    const std::string undo_label = core::tr("gui.undo_trash");
    const float undo_width = can_undo ? button_width(undo_label) + spacing : 0.0F;
    const float free_width = ImGui::GetContentRegionAvail().x - text_width(count) - spacing - undo_width;
    // İkisi de uzunsa alan yarı yarıya; kısa olan kendi kadar alır, kalan diğerinin.
    const float sync_width = std::min(text_width(sync), status.empty() ? free_width : free_width / 2.0F);
    ImGui::TextUnformatted(count.c_str());
    remember_item_center("count");
    if (!status.empty()) {
        ImGui::SameLine();
        draw_fitted_text("status", status, free_width - sync_width - spacing, keep_start);
    }
    if (can_undo) {
        ImGui::SameLine();
        if (button("status_undo", undo_label))
            queue([this] { state_.undo_trash(); });
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - sync_width);
    draw_fitted_text("sync", sync, sync_width, keep_start);
}

}  // namespace noteapp::gui
