#include "gui/view.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "pdf/fonts.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>
#include <mutex>
#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

constexpr float kFontPixels = 16.0F;
// Boşta en fazla bu aralıkla uyanılır (kırpma imleci, zamanlayıcı); kare yalnız iş varsa üretilir.
constexpr int kIdleWaitMs = 500;
// ImGui bir etkileşimi birkaç karede tamamlar (fare üzerinde, odak, açılır kutu).
constexpr int kFramesAfterEvent = 3;
constexpr float kFormWidth = 420.0F;
// Ölçek 1'deki alt sınırlar: en uzun Türkçe Ayarlar satırı ve araç çubuğu bu boyutta sığar; ölçekle çarpılır.
constexpr float kMinWindowWidth = 640.0F;
constexpr float kMinWindowHeight = 420.0F;
constexpr float kMinLeftPaneWidth = 180.0F;
constexpr float kMinRightPaneWidth = 240.0F;
constexpr float kMinPaneHeight = 60.0F;
constexpr float kDialogTextEms = 32.0F;
// Pencere en küçük boyutun da altındaysa (bileşikçi sınırı uygulamayabilir) alan ve yazı bu kadar kalır.
// Negatif genişlik ImGui'de "sağdan pay" demektir, sarma genişliği 1 piksele iner.
constexpr float kMinFieldEms = 4.0F;
constexpr float kScaleRounding = 100.0F;
constexpr float kMinInlineFieldEms = 8.0F;    // daha darsa yanındaki düğmeler alt satıra iner
constexpr float kMaxLabelColumnShare = 0.5F;  // etiket sütunu alanı yok etmesin; uzun etiket sarılır
constexpr const char* kGeneralKeys[] = {"language",       "notes_dir", "default_type",
                                        "default_folder", "types",     "editor"};
constexpr const char* kSyncKeys[] = {"sync.enabled",      "sync.remote",      "sync.branch",
                                     "sync.idle_seconds", "sync.author_name", "sync.author_email"};
constexpr const char* kEllipsis = "…";
constexpr ImVec4 kErrorColor(0.75F, 0.22F, 0.17F, 1.0F);  // açık ve koyu temada okunur kırmızı
constexpr const char* kLanguages[] = {"auto", "tr", "en"};
const SDL_DialogFileFilter kPdfFilters[] = {{"PDF", "pdf"}};

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

std::string sdl_error(const char* step) {
    return std::string(step) + ": " + SDL_GetError();
}

// Atlas kendi kopyasına sahip olur: gömülü veri const'tur, ImGui imzası void* ister; const_cast yerine kopya.
ImFont* load_font(const char* resource) {
    const auto font = pdf::embedded_font(resource);
    if (!font)
        return nullptr;
    void* owned = IM_ALLOC(font->size());
    std::memcpy(owned, font->data(), font->size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(font->size()), kFontPixels);
}

std::size_t utf8_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    if (byte >= 0xF0)
        return 4;
    if (byte >= 0xE0)
        return 3;
    return byte >= 0xC0 ? 2 : 1;
}

void wake_event_loop() {
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);  // kuyruk doluysa diğer olaylar zaten uyandırır
}

// Yerel diyaloğun sonucu: SDL geri çağrısı başka iş parçacığından ve modal kapandıktan sonra da gelebilir.
struct NativeDialogResult {
    std::mutex mutex;
    std::optional<std::string> path;
};

void SDLCALL native_dialog_done(void* userdata, const char* const* files, int /*filter*/) {
    const std::unique_ptr<std::shared_ptr<NativeDialogResult>> result(
        static_cast<std::shared_ptr<NativeDialogResult>*>(userdata));
    if (files == nullptr) {  // portal/zenity yok: yol alanı açık kalır, kullanıcı elle yazar
        core::log_warn(std::string("event=file_dialog_failed detail=") + SDL_GetError());
    } else if (files[0] != nullptr) {
        const std::scoped_lock lock((*result)->mutex);
        (*result)->path = files[0];
    }
    wake_event_loop();
}

void draw_error(const std::string& message) {
    if (message.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(kFormWidth * ImGui::GetStyle().FontScaleDpi,
                                                             ImGui::GetContentRegionAvail().x));
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// "sync.remote" -> "gui.setting_sync_remote": etiket metinleri katalogdan gelir.
std::string setting_label(const std::string& key) {
    std::string leaf = key;
    std::ranges::replace(leaf, '.',
                         '_');  // yalnız ayar anahtarında: "gui." bölüm ayracı katalog için gerekli
    return core::tr("gui.setting_" + leaf);
}

std::string setting_hint(const std::string& key) {
    std::string leaf = key;
    std::ranges::replace(leaf, '.', '_');
    return core::tr("gui.setting_hint_" + leaf);
}

float text_width(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

float button_width(const std::string& label) {
    return text_width(label) + 2.0F * ImGui::GetStyle().FramePadding.x;
}

// `width` içine sığan baştaki bayt sayısı; kod noktası ortasında kesmez.
std::size_t fitting_bytes(std::string_view text, float width) {
    const char* end = text.data() + text.size();
    const char* remaining = nullptr;
    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::max(width, 0.0F), 0.0F, text.data(), end,
                                    &remaining);
    return static_cast<std::size_t>(remaining - text.data());
}

// Baş korunur (hata iletisinin nedeni başta olur).
std::string keep_start(std::string_view text, float width) {
    return std::string(text.substr(0, fitting_bytes(text, width - text_width(kEllipsis)))) + kEllipsis;
}

// Son korunur: uzun yolda ayırt edici kısım dosya adı ve kaydedildi/değişti göstergesidir.
// ponytail: her adımda kalan metin yeniden ölçülür (O(n²)); yol uzunluğu işletim sistemiyle sınırlı.
std::string keep_end(std::string_view text, float width) {
    const float room = width - text_width(kEllipsis);
    while (!text.empty() && text_width(text) > room)
        text.remove_prefix(std::min(utf8_length(text.front()), text.size()));
    return kEllipsis + std::string(text);
}

// Diyalog pencerenin içinde kalmalı: yazı ve düğme satırları bundan geniş olamaz.
float dialog_content_width() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available =
        ImGui::GetMainViewport()->WorkSize.x - 4.0F * style.WindowPadding.x - style.ScrollbarSize;
    return std::clamp(available, ImGui::GetFontSize() * kMinFieldEms, ImGui::GetFontSize() * kDialogTextEms);
}

void full_text_tooltip(const std::string& text) {
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(dialog_content_width());
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Sığarsa önceki düğmeyle aynı satıra, sığmazsa alt satıra: dar panelde ve ölçekte kırpılmaz.
void flow_after_previous(float next_width, float row_start_x, float row_width) {
    const float next_end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_width;
    if (next_end <= row_start_x + row_width)
        ImGui::SameLine();
}

// Alanın yanındaki düğmelere yer bırakır; alan çok daralacaksa satırın tamamını alır, düğmeler alta iner.
float field_width_beside(float trailing_width) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float beside = available - trailing_width - ImGui::GetStyle().ItemSpacing.x;
    return beside >= ImGui::GetFontSize() * kMinInlineFieldEms ? beside : available;
}

float settings_label_width(const SettingsPage& page, float available) {
    float widest = text_width(core::tr("gui.token_label"));
    for (const std::string& key : page.keys())
        widest = std::max(widest, text_width(setting_label(key)));
    return std::min(widest + 1.0F, available * kMaxLabelColumnShare);  // +1: yuvarlama etiketi sarmasın
}

bool begin_form_table(const char* id, float label_width) {
    if (!ImGui::BeginTable(id, 2))
        return false;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// Sütuna sarılan soluk açıklama satırı.
void draw_hint(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Dil kodu saklanır; kullanıcı okunur adını görür. Elle yazılmış bilinmeyen değer olduğu gibi gösterilir.
std::string language_label(const std::string& code) {
    return std::ranges::find(kLanguages, code) != std::end(kLanguages) ? core::tr("gui.language_" + code)
                                                                       : code;
}

// Sondaki etiket ya da düğme için yer bırakan form alanı genişliği.
float form_item_width(float trailing_width, float available) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return std::clamp(available - trailing_width - style.ItemSpacing.x, ImGui::GetFontSize() * kMinFieldEms,
                      kFormWidth * style.FontScaleDpi);
}

// Bir kez konumlanan diyalog (kullanıcı da taşıyabilir) pencere küçülünce ya da içerik büyüyünce dışarı
// taşmasın.
void keep_window_in_viewport() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 position = ImGui::GetWindowPos();
    const float x =
        std::clamp(position.x, viewport->WorkPos.x,
                   std::max(viewport->WorkPos.x, viewport->WorkPos.x + viewport->WorkSize.x - size.x));
    const float y =
        std::clamp(position.y, viewport->WorkPos.y,
                   std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y));
    if (x != position.x || y != position.y)
        ImGui::SetWindowPos(ImVec2(x, y));
}

// Dosya adları güvenilmez: ImGui etiketlerindeki "##" ve "%" yorumlanmasın diye metin doğrudan çizilir.
// Eşleşen kod noktaları vurgu rengiyle.
void draw_row_text(ImVec2 position, std::string_view text, const std::vector<std::size_t>& highlights) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const auto draw_piece = [&](std::size_t from, std::size_t to, ImU32 color) {
        if (from >= to)
            return;
        const char* begin = text.data() + from;
        const char* end = text.data() + to;
        draw_list->AddText(position, color, begin, end);
        position.x += ImGui::CalcTextSize(begin, end).x;
    };
    std::size_t done = 0;
    for (const std::size_t offset : highlights) {
        if (offset < done || offset >= text.size())
            continue;
        const std::size_t next = std::min(offset + utf8_length(text[offset]), text.size());
        draw_piece(done, offset, normal);
        draw_piece(offset, next, accent);
        done = next;
    }
    draw_piece(done, text.size(), normal);
}

bool plain_selectable(const std::string& text, bool selected,
                      const std::vector<std::size_t>& highlights = {}) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable("##row", selected);
    if (text_width(text) <= width) {
        draw_row_text(position, text, highlights);
        return clicked;
    }
    const std::string_view visible =
        std::string_view(text).substr(0, fitting_bytes(text, width - text_width(kEllipsis)));
    draw_row_text(position, visible, highlights);
    ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + text_width(visible), position.y),
                                        ImGui::GetColorU32(ImGuiCol_Text), kEllipsis);
    full_text_tooltip(text);
    return clicked;
}

// Editable açılır kutu (ImGui'de yok): serbest metin alanı + seçim listesi. Alanda Enter basıldıysa true.
bool combo_field(const char* id, const std::string& label, std::string& value,
                 const std::vector<std::string>& items, bool root_first, const char* hint = "") {
    ImGui::PushID(id);
    const float arrow = ImGui::GetFrameHeight();
    const float field_width = ImGui::CalcItemWidth() - arrow;
    ImGui::SetNextItemWidth(field_width);
    const bool entered =
        ImGui::InputTextWithHint("##field", hint, &value, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0F, 0.0F);
    if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
        if (root_first && plain_selectable(hint, value.empty()))
            value.clear();
        for (std::size_t i = 0; i < items.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (plain_selectable(items[i], items[i] == value))
                value = items[i];
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!label.empty()) {  // iki sütunlu formda etiket solda, kendi sütununda
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
    }
    ImGui::PopID();
    return entered;
}

struct SearchKeys {
    std::function<void(int)> on_arrow;
    std::function<void()> on_tab;
};

int search_callback(ImGuiInputTextCallbackData* data) {
    const auto* keys = static_cast<SearchKeys*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        keys->on_arrow(data->EventKey == ImGuiKey_UpArrow ? -1 : 1);
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
        keys->on_tab();
    return 0;
}

// Tek seferlik imleç/seçim: ImGui bunu yalnız etkin InputText'in kendi geri çağrısından kabul eder.
int editor_selection_callback(ImGuiInputTextCallbackData* data) {
    auto* pending = static_cast<std::optional<TextSelection>*>(data->UserData);
    if (*pending) {
        const int start = std::min(static_cast<int>((*pending)->start), data->BufTextLen);
        const int end = std::min(static_cast<int>((*pending)->end), data->BufTextLen);
        data->SetSelection(start, end);
        pending->reset();
    }
    return 0;
}

}  // namespace

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
};

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

    platform->ui_font = load_font("fonts/DejaVuSans.ttf");
    platform->mono_font = load_font("fonts/DejaVuSansMono.ttf");
    if (platform->ui_font == nullptr || platform->mono_font == nullptr)
        return std::unexpected("embedded UI font is missing");
    platform->sdl_backend.emplace(platform->window.get(), platform->renderer.get());
    if (!platform->sdl_backend->ok)
        return std::unexpected(sdl_error("ImGui_ImplSDL3_InitForSDLRenderer"));
    platform->renderer_backend.emplace(platform->renderer.get());
    if (!platform->renderer_backend->ok)
        return std::unexpected(sdl_error("ImGui_ImplSDLRenderer3_Init"));

    std::unique_ptr<View> view(new View(state, std::move(platform)));
    view->apply_window_scale();  // temayı da uygular
    return view;
}

View::View(GuiState& state, std::unique_ptr<Platform> platform)
    : state_(state), platform_(std::move(platform)), frames_left_(kFramesAfterEvent) {
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

void View::install_deciders() {
    state_.deciders.save_dialog = [this](const SaveDialogRequest& request) {
        return ask_save_details(request);
    };
    state_.deciders.conflict = [this](const std::string& path) {
        return ask_choice(make_conflict_box(path));
    };
    state_.deciders.close_unsaved = [this] { return ask_choice(make_close_box()); };
    state_.deciders.close_unsaved_settings = [this] { return ask_choice(make_close_settings_box()); };
    state_.deciders.confirm_delete = [this](const std::string& message) {
        return ask_choice(make_delete_box(message));
    };
    state_.deciders.folder_name = [this](const std::string& title, const std::string& initial) {
        return ask_folder_name(title, initial);
    };
    state_.deciders.export_path = [this](const std::string& suggested) { return ask_export_path(suggested); };
    state_.deciders.browse_dir = [this](const std::string& initial) {
        return ask_path(core::tr("gui.browse_title"), initial, PathKind::Folder);
    };
    state_.deciders.confirm_replace_origin = [this](const std::string& message) {
        // Güvenli varsayılan: Enter ve Esc vazgeçer.
        return ask_choice(
            ChoiceBox<bool>{core::tr("gui.replace_origin_title"),
                            message,
                            {{core::tr("gui.replace_origin"), true}, {core::tr("gui.cancel"), false}},
                            1,
                            1});
    };
    state_.deciders.info = [this](const std::string& message) {
        ask_choice(ChoiceBox<bool>{"noteapp", message, {{core::tr("gui.ok"), true}}, 0, 0});
    };
    state_.deciders.while_busy = [](const std::function<void()>& work) {
        // Arka uç imleci yalnız değişince kurar: önceki imleç elle geri konur, yoksa bekleme imleci takılı
        // kalırdı.
        const std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)> wait(
            SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT), &SDL_DestroyCursor);
        const struct Restore {
            SDL_Cursor* cursor;
            ~Restore() { SDL_SetCursor(cursor); }
        } restore{SDL_GetCursor()};
        SDL_SetCursor(wait.get());
        work();
    };
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
        default:
            break;
        }
    }
}

void View::render(const std::function<void()>& overlay) {
    pump_events();
    item_rects_.clear();  // yalnız bu karede çizilenler kalır: gizli sekmenin eski konumu test edilmesin
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
}

int View::idle_timeout_ms() const {
    if (frames_left_ > 0)
        return 0;
    int timeout = kIdleWaitMs;
    if (const auto deadline = state_.next_deadline()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline - std::chrono::steady_clock::now());
        timeout = std::clamp(static_cast<int>(left.count()), 0, kIdleWaitMs);
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
    } else if (blinking) {
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
    if (modal_depth_ > 0)
        return;
    struct Shortcut {
        ImGuiKeyChord chord;
        void (*action)(GuiState&);
    };
    static constexpr Shortcut kShortcuts[] = {
        {ImGuiMod_Ctrl | ImGuiKey_K, [](GuiState& s) { s.focus_search(); }},
        {ImGuiMod_Ctrl | ImGuiKey_P, [](GuiState& s) { s.focus_search(); }},
        {ImGuiMod_Ctrl | ImGuiKey_N, [](GuiState& s) { s.new_note(); }},
        {ImGuiMod_Ctrl | ImGuiKey_S, [](GuiState& s) { s.save_now(); }},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, [](GuiState& s) { s.save_as(); }},
        {ImGuiMod_Ctrl | ImGuiKey_M, [](GuiState& s) { s.move_note(); }},
        {ImGuiMod_Ctrl | ImGuiKey_Comma, [](GuiState& s) { s.show_settings_tab(); }},
        {ImGuiMod_Ctrl | ImGuiKey_E, [](GuiState& s) { s.export_pdf(); }},
        {ImGuiKey_F5, [](GuiState& s) { s.sync_now(); }},
    };
    for (const Shortcut& shortcut : kShortcuts) {
        if (ImGui::IsKeyChordPressed(shortcut.chord)) {
            const auto action = shortcut.action;
            queue([this, action] { action(state_); });
        }
    }
    // ImGui Escape'te odaklanıldığından beri yapılan düzenlemeyi geri alır; notta bu sessiz veri kaybıdır.
    // Rota alınınca InputText tuşu görmez. Diyaloglarda (modal_depth_ > 0) Esc serbesttir.
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
    draw_left_pane();
    ImGui::SameLine();
    draw_right_pane();
}

void View::draw_left_pane() {
    GuiLayout& layout = state_.layout();
    // Kayıtlı değer ölçeksizdir. ImGui ResizeX boyutunu yalnız ilk kullanımda alır; sonrası
    // reconcile_pane_size ile uygulanır. Kısıt olmasa büyük bölme sağ paneli pencere dışına iterdi.
    const float min_width = kMinLeftPaneWidth * ui_scale_;
    const float max_width =
        std::max(min_width, ImGui::GetContentRegionAvail().x - kMinRightPaneWidth * ui_scale_);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0F), ImVec2(max_width, FLT_MAX));
    ImGui::BeginChild("##left", ImVec2(layout.left_pane_width * ui_scale_, 0.0F), ImGuiChildFlags_ResizeX);
    const float actual_width = ImGui::GetWindowWidth();
    const float target_width =
        reconcile_pane_size(layout.left_pane_width, left_pane_size_, actual_width, min_width, max_width);
    if (std::abs(actual_width - target_width) >= 1.0F)
        ImGui::SetWindowSize(ImVec2(target_width, ImGui::GetWindowHeight()));
    draw_search_row();
    draw_folder_tree();
    draw_toolbar();
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

void View::draw_search_row() {
    ImGui::PushID(static_cast<int>(state_.query_generation()));
    if (pending_focus_ == FocusRequest::Search) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
    }
    std::function<void(int)> on_arrow = [this](int delta) {
        queue([this, delta] { state_.move_selection(delta); });
    };
    SearchKeys keys{std::move(on_arrow), [this] { queue([this] { state_.toggle_search_mode(); }); }};
    const std::string hint = core::tr("gui.search_placeholder");
    const std::string mode_label = core::tr(state_.content_mode() ? "gui.mode_content" : "gui.mode_name");
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(
        -(ImGui::CalcTextSize(mode_label.c_str()).x + 2.0F * style.FramePadding.x + style.ItemSpacing.x));
    // Completion bayrağı Tab'i odak gezintisinden alır: arama kutusunda Tab modu çevirir.
    const bool entered =
        ImGui::InputTextWithHint("##search", hint.c_str(), &state_.query_buffer(),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                     ImGuiInputTextFlags_CallbackCompletion,
                                 search_callback, &keys);
    remember_item_center("search");
    search_active_ = ImGui::IsItemActive();
    if (ImGui::IsItemEdited())
        queue([this] { state_.query_edited(); });
    if (entered)
        queue([this] { state_.search_enter(); });
    ImGui::PopID();

    ImGui::SameLine();
    // Düğme odak almaz: arama kutusundan çıkmadan mod çevrilebilsin (Tab aynı işi görür).
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (button("mode", mode_label))
        queue([this] { state_.toggle_search_mode(); });
    ImGui::PopItemFlag();
}

bool View::button(const std::string& name, const std::string& label) {
    const bool clicked = ImGui::Button((label + "###" + name).c_str());
    remember_item_center(name);
    return clicked;
}

void View::remember_item_center(const std::string& name) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    item_rects_[name] = {min.x, min.y, max.x, max.y, disabled};
}

int View::token_input_flags() {
    return ImGuiInputTextFlags_Password;
}

// Saklanan değer dil kodudur; ImGui kimliği de koddan gelir, çeviri kimliği değiştirmez.
void View::draw_language_combo(std::string& value) {
    const bool open = ImGui::BeginCombo("##value", language_label(value).c_str());
    remember_item_center("setting:language");
    if (!open)
        return;
    for (const char* language : kLanguages) {
        const std::string option = language_label(language) + "###" + language;
        if (ImGui::Selectable(option.c_str(), value == language))
            value = language;
        remember_item_center(std::string("language:") + language);
    }
    ImGui::EndCombo();
}

void View::draw_setting_input(const std::string& key) {
    SettingsPage& page = state_.settings();
    std::string& value = page.field(key);
    if (key == "language") {
        draw_language_combo(value);
        return;
    }
    if (key == "default_type") {
        ImGui::BeginGroup();
        combo_field("value", {}, value, page.types(), false);
        ImGui::EndGroup();
    } else if (key == "sync.enabled") {
        bool enabled = value == "true";
        if (ImGui::Checkbox("##value", &enabled))
            value = enabled ? "true" : "false";
    } else {
        const ImGuiInputTextFlags flags =
            key == "sync.idle_seconds" ? ImGuiInputTextFlags_CharsDecimal : ImGuiInputTextFlags_None;
        ImGui::InputText("##value", &value, flags);
        if (ImGui::IsItemEdited())
            page.field_edited(key);
    }
    remember_item_center("setting:" + key);
}

// Yeni satır: etiket sol sütunda yazılır, imleç alan sütununda kalır.
void View::begin_setting_row(const std::string& label, const std::string& label_name) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopTextWrapPos();
    remember_item_center(label_name);
    ImGui::TableSetColumnIndex(1);
}

void View::draw_setting_field(const std::string& key) {
    SettingsPage& page = state_.settings();
    ImGui::PushID(key.c_str());
    // Senkron kapalıyken değerler saklanır, yalnız düzenlenemez; "Senkronu aç" işaretlenince hemen açılırlar.
    const bool locked =
        key.starts_with("sync.") && key != "sync.enabled" && page.field("sync.enabled") != "true";
    ImGui::BeginDisabled(locked);
    begin_setting_row(setting_label(key), "setting_label:" + key);
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const std::string browse_label = core::tr("gui.browse");
    const bool browsable = key == "notes_dir";
    ImGui::PushItemWidth(browsable ? field_width_beside(button_width(browse_label)) : -FLT_MIN);
    draw_setting_input(key);
    ImGui::PopItemWidth();
    if (browsable) {
        flow_after_previous(button_width(browse_label), row_start_x, row_width);
        if (button("browse", browse_label))
            queue([this] { state_.settings().browse_notes_dir(); });
    }
    ImGui::EndDisabled();
    if (key == "editor" || key == "sync.remote" || key == "sync.idle_seconds") {
        draw_hint(setting_hint(key));
        remember_item_center("setting_hint:" + key);
    }
    if (!page.field_error(key).empty()) {
        draw_error(page.field_error(key));
        remember_item_center("setting_error:" + key);
    }
    ImGui::PopID();
}

void View::draw_token_row() {
    SettingsPage& page = state_.settings();
    begin_setting_row(core::tr("gui.token_label"), "token_label");
    const std::string save_label = core::tr("gui.token_save");
    const std::string clear_label = core::tr("gui.token_clear");
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    // Yalnız https remote'ta etkin; değer asla gösterilmez, config'e yazılmaz. Senkron kapalıyken de açıktır:
    // https'te enable token ister ve kasadaki sır her zaman silinebilmelidir.
    ImGui::BeginDisabled(!page.token_enabled());
    ImGui::SetNextItemWidth(field_width_beside(button_width(save_label) + ImGui::GetStyle().ItemSpacing.x +
                                               button_width(clear_label)));
    ImGui::InputText("##token", &page.token, token_input_flags());
    remember_item_center("token_input");
    flow_after_previous(button_width(save_label), row_start_x, row_width);
    if (button("token_save", save_label))
        queue([this] { state_.settings().save_token(); });
    flow_after_previous(button_width(clear_label), row_start_x, row_width);
    if (button("token_clear", clear_label))
        queue([this] { state_.settings().clear_token(); });
    ImGui::EndDisabled();
    if (page.token_enabled()) {
        const std::string state = core::tr(page.token_stored() ? "gui.token_stored" : "gui.token_none");
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.token_state", {{"state", state}}).c_str());
        ImGui::PopTextWrapPos();
        remember_item_center("token_state");
    } else {
        draw_hint(core::tr("gui.token_https_only"));
        remember_item_center("token_hint");
    }
}

void View::draw_connection_test_row() {
    const SettingsPage& page = state_.settings();
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(page.testing());
    if (button("test", core::tr("gui.test_connection")))
        queue([this] { state_.settings().test_connection(); });
    ImGui::EndDisabled();
    if (page.test_result().empty())
        return;
    // Uzun senkron hatası kalan alanda sarılır; kalan alan çok darsa düğmenin altına iner.
    flow_after_previous(ImGui::GetFontSize() * kMinInlineFieldEms, row_start_x, row_width);
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(page.test_result().c_str());
    ImGui::PopTextWrapPos();
    remember_item_center("test_result");
}

void View::draw_settings_form() {
    const float label_width = settings_label_width(state_.settings(), ImGui::GetContentRegionAvail().x);
    ImGui::SeparatorText(core::tr("gui.settings_group_general").c_str());
    remember_item_center("settings_group_general");
    if (begin_form_table("##general", label_width)) {
        for (const char* key : kGeneralKeys)
            draw_setting_field(key);
        ImGui::EndTable();
    }
    ImGui::SeparatorText(core::tr("gui.settings_group_sync").c_str());
    remember_item_center("settings_group_sync");
    if (begin_form_table("##sync", label_width)) {
        for (const char* key : kSyncKeys)
            draw_setting_field(key);
        draw_token_row();
        draw_connection_test_row();
        ImGui::EndTable();
    }
}

// Formun dışında: en küçük pencerede de Geri al ve Kaydet görünür kalır.
void View::draw_settings_footer() {
    const SettingsPage& page = state_.settings();
    const float top = ImGui::GetCursorPosY();
    ImGui::Separator();
    if (!page.global_error().empty()) {
        draw_error(page.global_error());
        remember_item_center("settings_error");
    }
    const bool changed = page.has_changes();
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const std::string save_label = core::tr("gui.settings_save");
    ImGui::BeginDisabled(!changed);
    if (button("revert", core::tr("gui.settings_revert")))
        queue([this] { state_.settings().revert(); });
    flow_after_previous(button_width(save_label), row_start_x, row_width);
    if (button("save", save_label))
        queue([this] { state_.settings().save(); });
    ImGui::EndDisabled();
    if (changed) {
        const std::string unsaved = core::tr("gui.settings_unsaved");
        flow_after_previous(text_width(unsaved), row_start_x, row_width);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(unsaved.c_str());
        remember_item_center("settings_unsaved");
    }
    settings_footer_height_ = ImGui::GetCursorPosY() - top;
}

void View::draw_settings_tab() {
    ImGui::BeginChild("##settings_form", ImVec2(0.0F, -settings_footer_height_));
    draw_settings_form();
    ImGui::EndChild();
    remember_item_center("settings_form");
    draw_settings_footer();
}

void View::draw_folder_menu(const std::string& folder) {
    // Bağlam menüsü sağ tıkla, klavyede Menü tuşu / Shift+F10 ile açılır (ImGui gezinti imlecindeki öğeye
    // bağlar).
    if (!ImGui::BeginPopupContextItem("##folder_menu"))
        return;
    if (ImGui::MenuItem(core::tr("gui.new_folder").c_str()))
        queue([this, folder] { state_.new_folder(folder); });
    if (!folder.empty()) {  // kök "Tümü" yeniden adlandırılamaz/silinemez
        if (ImGui::MenuItem(core::tr("gui.rename_folder").c_str()))
            queue([this, folder] { state_.rename_folder(folder); });
        if (ImGui::MenuItem(core::tr("gui.delete_folder").c_str()))
            queue([this, folder] { state_.delete_folder(folder); });
    }
    ImGui::EndPopup();
}

void View::draw_note_menu(int row) {
    if (!ImGui::BeginPopupContextItem("##note_menu"))
        return;
    // Eylem o satıra uygulanır: önce seçilir; seçim tutmadıysa (kayıt iptal) satırın notuna dokunulmaz. Kayıt
    // satırı kaydırabildiğinden denetim yola göredir.
    const auto on_row = [this, path = state_.model().note_at(row).path, row](void (GuiState::*action)()) {
        queue([this, path, row, action] {
            state_.select_row(row);
            if (state_.current_path() == path)
                (state_.*action)();
        });
    };
    if (ImGui::MenuItem(core::tr("gui.move_note").c_str()))
        on_row(&GuiState::move_note);
    if (ImGui::MenuItem(core::tr("gui.trash_note").c_str()))
        on_row(&GuiState::trash_note);
    if (ImGui::MenuItem(core::tr("gui.delete_note").c_str()))
        on_row(&GuiState::delete_note);
    ImGui::EndPopup();
}

bool View::draw_folder_row(const std::string& folder, bool has_children) {
    const auto slash = folder.rfind('/');
    const char* leaf = folder.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!has_children)
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (state_.current_folder() == folder)
        flags |= ImGuiTreeNodeFlags_Selected;
    const float label_width = ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing();
    const bool fits = text_width(leaf) <= label_width;
    const bool open =
        ImGui::TreeNodeEx(folder.c_str(), flags, "%s", fits ? leaf : keep_start(leaf, label_width).c_str());
    remember_item_center("folder:" + folder);
    if (!fits)
        full_text_tooltip(folder);
    // Enter: klavyeyle gezinen de klasörü seçebilsin (IsItemClicked yalnız fareyi görür).
    if ((ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) ||
        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
        queue([this, chosen = folder] { state_.set_folder(chosen); });
    draw_folder_menu(folder);
    return open;
}

void View::draw_folder_children(std::size_t& index, const std::string& parent) {
    const auto& folders = state_.folders();
    while (index < folders.size()) {
        const std::string& folder = folders[index];
        if (!parent.empty() && !folder.starts_with(parent + '/'))
            return;
        ++index;  // sıralı: çocuklar hemen ebeveynin ardından gelir
        const bool has_children = index < folders.size() && folders[index].starts_with(folder + '/');
        if (draw_folder_row(folder, has_children)) {
            draw_folder_children(index, folder);
            ImGui::TreePop();
        } else {
            while (index < folders.size() && folders[index].starts_with(folder + '/'))
                ++index;
        }
    }
}

void View::draw_folder_tree() {
    GuiLayout& layout = state_.layout();
    const float min_height = kMinPaneHeight * ui_scale_;
    const float max_height = std::max(min_height, ImGui::GetContentRegionAvail().y - min_height -
                                                      toolbar_height_ - ImGui::GetStyle().ItemSpacing.y);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0F, min_height), ImVec2(FLT_MAX, max_height));
    ImGui::BeginChild("##folders", ImVec2(0.0F, layout.folder_pane_height * ui_scale_),
                      ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);
    const float actual_height = ImGui::GetWindowHeight();
    const float target_height = reconcile_pane_size(layout.folder_pane_height, folder_pane_size_,
                                                    actual_height, min_height, max_height);
    if (std::abs(actual_height - target_height) >= 1.0F)
        ImGui::SetWindowSize(ImVec2(ImGui::GetWindowWidth(), target_height));
    const std::string all = core::tr("gui.all_notes");
    ImGuiTreeNodeFlags root_flags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (state_.current_folder().empty())
        root_flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx("##all", root_flags, "%s", all.c_str());
    remember_item_center("folder:");
    if ((ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) ||
        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
        queue([this] { state_.set_folder({}); });
    draw_folder_menu({});
    if (open) {
        std::size_t index = 0;
        draw_folder_children(index, {});
        ImGui::TreePop();
    }
    ImGui::EndChild();
    remember_item_center("folder_tree");
}

void View::draw_toolbar() {
    struct ToolbarAction {
        const char* name;
        const char* label_key;
        const char* tooltip_key;
        bool enabled;
        std::function<void()> run;
    };
    const ToolbarAction actions[] = {
        {"toolbar_new_note", "gui.toolbar_new_note", "gui.toolbar_new_note_tip", true,
         [this] { state_.new_note(); }},
        {"toolbar_new_folder", "gui.new_folder", "gui.toolbar_new_folder_tip", true,
         [this, folder = state_.current_folder()] { state_.new_folder(folder); }},
        {"toolbar_delete", "gui.delete", "gui.toolbar_delete_tip", state_.can_delete(),
         [this] { state_.delete_selected(false); }},
        {"toolbar_trash", nullptr, "gui.toolbar_trash_tip", true, [this] { state_.toggle_trash(); }},
        {"toolbar_move", "gui.toolbar_move", "gui.toolbar_move_tip", state_.can_move(),
         [this] { state_.move_note(); }},
        {"toolbar_export", "gui.toolbar_export", "gui.toolbar_export_tip", state_.can_export(),
         [this] { state_.export_pdf(); }},
    };
    const float top = ImGui::GetCursorPosY();
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    bool first = true;
    // Fare içindir: her eylemin kısayolu ya da bağlam menüsü var; Tab sırası liste ile ağaç arasında
    // uzamasın.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const std::string trash_label =
        core::tr("gui.toolbar_trash", {{"count", std::to_string(state_.trash_count())}});
    for (const ToolbarAction& action : actions) {
        const std::string label = action.label_key ? core::tr(action.label_key) : trash_label;
        if (!first)
            flow_after_previous(button_width(label), row_start_x, row_width);
        first = false;
        ImGui::BeginDisabled(!action.enabled);
        if (button(action.name, label))
            queue(action.run);
        ImGui::SetItemTooltip(
            "%s", core::tr(action.tooltip_key).c_str());  // kapalı düğmede de neden/kısayol görünsün
        ImGui::EndDisabled();
    }
    ImGui::PopItemFlag();
    toolbar_height_ = ImGui::GetCursorPosY() - top;
}

// Kaydırılan listenin dışında: çok satırda da düğmeler görünür kalır.
void View::draw_trash_header() {
    ImGui::TextUnformatted(core::tr("gui.trash_title").c_str());
    remember_item_center("trash_title");
    struct TrashAction {
        const char* name;
        const char* label_key;
        bool enabled;
        std::function<void()> run;
    };
    const TrashAction actions[] = {
        {"trash_restore", "gui.trash_restore", state_.trash_row() >= 0, [this] { state_.restore_trashed(); }},
        {"trash_purge", "gui.trash_purge", state_.trash_row() >= 0, [this] { state_.purge_trashed(); }},
        {"trash_empty", "gui.trash_empty", !state_.trash_items().empty(), [this] { state_.empty_trash(); }},
        {"trash_back", "gui.trash_back", true, [this] { state_.close_trash(); }},
    };
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const TrashAction& action : actions) {
        const std::string label = core::tr(action.label_key);
        if (!first)
            flow_after_previous(button_width(label), row_start_x, row_width);
        first = false;
        ImGui::BeginDisabled(!action.enabled);
        if (button(action.name, label))
            queue(action.run);
        ImGui::EndDisabled();
    }
}

void View::draw_trash_rows(bool focus_row) {
    const auto& items = state_.trash_items();
    const int current = state_.trash_row();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(items.size()));
    if (current >= 0 && (current != scrolled_row_ || focus_row))
        clipper.IncludeItemByIndex(current);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            if (focus_row && row == current)
                ImGui::SetKeyboardFocusHere();
            if (plain_selectable(items[static_cast<std::size_t>(row)].path.string(), row == current))
                queue([this, row] { state_.select_trash_row(row); });
            if (row == current && current != scrolled_row_) {
                ImGui::SetScrollHereY();
                scrolled_row_ = current;
            }
            ImGui::PopID();
        }
    }
}

void View::draw_note_list() {
    const bool trash = state_.trash_view();
    if (trash)
        draw_trash_header();
    ImGui::BeginChild("##notes", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
    // Klavyeyle (arama kutusunda Shift+Tab, Enter) girilince gezinti imleci seçili satırda başlar; yoksa
    // imleç ve seçim ayrı satırlarda yürür, bağlam menüsü ile Del farklı notlara giderdi. Fareyle odak zaten
    // satırı seçer.
    const bool entered_by_keyboard = ImGui::IsWindowFocused() && !list_focused_ && !ImGui::IsAnyMouseDown();
    const bool focus_row = pending_focus_ == FocusRequest::List || entered_by_keyboard;
    if (pending_focus_ == FocusRequest::List) {
        ImGui::SetWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    list_focused_ = ImGui::IsWindowFocused();
    if (trash ? state_.trash_items().empty() : state_.model().row_count() == 0) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr(trash ? "gui.trash_empty_hint" : "gui.empty_list_hint").c_str());
        ImGui::PopTextWrapPos();
    }
    if (trash)
        draw_trash_rows(focus_row);
    else
        draw_note_rows(focus_row);
    if (list_focused_) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            queue([this] { state_.move_selection(-1); });
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            queue([this] { state_.move_selection(1); });
        // Del yalnız liste odaktayken: editörde/arama kutusunda karakter silmeyi ezmesin.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            queue([this, permanent = ImGui::GetIO().KeyShift] { state_.delete_selected(permanent); });
    }
    ImGui::EndChild();
    remember_item_center("note_list");
}

void View::draw_note_rows(bool focus_row) {
    const NoteListModel& model = state_.model();
    const int current = state_.current_row();
    ImGuiListClipper clipper;  // 5.000 satırda yalnız görünenler çizilir
    clipper.Begin(model.row_count());
    if (current >= 0 && (current != scrolled_row_ || focus_row))
        clipper.IncludeItemByIndex(current);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            if (focus_row && row == current)
                ImGui::SetKeyboardFocusHere();
            if (plain_selectable(model.display(row), row == current, model.highlights(row)))
                queue([this, row] { state_.select_row(row); });
            draw_note_menu(row);
            if (row == current && current != scrolled_row_) {
                ImGui::SetScrollHereY();
                scrolled_row_ = current;
            }
            ImGui::PopID();
        }
    }
}

void View::draw_right_pane() {
    ImGui::BeginGroup();
    draw_header_row();
    draw_editor();
    ImGui::EndGroup();
}

void View::draw_header_row() {
    const float combo_width = state_.type_enabled() ? ImGui::GetFontSize() * 8.0F : 0.0F;
    const float header_width = ImGui::GetContentRegionAvail().x -
                               (combo_width > 0.0F ? combo_width + ImGui::GetStyle().ItemSpacing.x : 0.0F);
    ImGui::AlignTextToFramePadding();
    draw_fitted_text("header", state_.header(), header_width, keep_end);
    if (!state_.type_enabled())
        return;
    const std::string current = state_.current_type();
    // Grup içinde SameLine'ın konumu pencereye göre değil grup başına göre kayar: sağa yaslama kalan alandan.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0F, ImGui::GetContentRegionAvail().x - combo_width));
    ImGui::SetNextItemWidth(combo_width);
    const bool type_open = ImGui::BeginCombo("##type", current.c_str());
    remember_item_center("type");
    if (type_open) {
        const auto choices = state_.type_choices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (plain_selectable(choices[i], choices[i] == current))
                queue([this, chosen = choices[i]] { state_.change_type(chosen); });
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void View::draw_editor() {
    const std::size_t generation = state_.buffer_generation();
    // Program içinden gelen yeni içerikte ImGui'nin iç kopyası atılmalı (kimlik değişir), yoksa eski metni
    // tampona geri yazardı. Odaktaysa kimlik değişimi odağı düşürmesin.
    if (generation != drawn_buffer_generation_) {
        if (editor_active_ && pending_focus_ == FocusRequest::None)
            pending_focus_ = FocusRequest::Editor;
        drawn_buffer_generation_ = generation;
    }
    if (refocus_editor_) {
        refocus_editor_ = false;
        pending_focus_ = FocusRequest::Editor;
    }
    ImGui::PushID(static_cast<int>(generation));
    if (pending_focus_ == FocusRequest::Editor) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;  // Tab odak gezmez, karakter ekler
    if (!state_.editable())
        flags |= ImGuiInputTextFlags_ReadOnly;
    // Geri çağrı yalnız gerektiğinde: ImGui her çağrıda tüm metni yedekler, 1 MB notta kare başına kopya
    // olurdu.
    ImGuiInputTextCallback callback = nullptr;
    std::optional<TextSelection> selection = state_.editor_selection();
    if (selection) {
        flags |= ImGuiInputTextFlags_CallbackAlways;
        callback = editor_selection_callback;
    }
    ImGui::PushFont(platform_->mono_font, 0.0F);
    const bool edited = ImGui::InputTextMultiline(
        "##editor", &state_.buffer_for_edit(), ImGui::GetContentRegionAvail(), flags, callback, &selection);
    remember_item_center("editor");
    ImGui::PopFont();
    editor_active_ = ImGui::IsItemActive();
    if (state_.editor_selection() && !selection)  // geri çağrı uyguladı
        state_.take_editor_selection();
    if (edited)
        state_.buffer_edited();
    ImGui::PopID();
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

// Tek satır, en çok `width`: sığmayan kısalır, tam metin ipucunda. Çok satırlı hata iletisi (git çıktısı)
// durum çubuğundan taşmasın diye satır sonları boşluğa iner.
void View::draw_fitted_text(const std::string& name, const std::string& text, float width,
                            std::string (*shorten)(std::string_view, float)) {
    std::string line = text;
    std::ranges::replace_if(line, [](char c) { return c == '\n' || c == '\r'; }, ' ');
    const bool fits = text_width(line) <= width;
    ImGui::TextUnformatted(fits ? line.c_str() : shorten(line, width).c_str());
    remember_item_center(name);
    if (!fits)
        full_text_tooltip(text);
}

void View::run_modal(const std::string& title, const std::function<bool()>& body) {
    const bool was_editing = editor_active_;
    // Diyalog akışını istisnaya karşı sayaçla korur: modal_depth_ takılırsa kısayollar ölü kalırdı.
    struct Depth {
        int& depth;
        explicit Depth(int& d) : depth(d) { ++depth; }
        ~Depth() { --depth; }
    } depth(modal_depth_);
    const std::string id = title + "###modal";
    frames_left_ = kFramesAfterEvent;
    for (bool finished = false; !finished;) {
        if (on_modal_frame)  // test kancası tuşları kendisi üretir: beklemek yalnız testi yavaşlatır
            on_modal_frame();
        else if (frames_left_ > 0)
            --frames_left_;
        else if (SDL_WaitEventTimeout(nullptr, kIdleWaitMs))
            frames_left_ = kFramesAfterEvent;
        render([&] {
            if (!ImGui::IsPopupOpen(id.c_str()))
                ImGui::OpenPopup(id.c_str());
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
            // Kısa pencerede içerik taşarsa diyalog büyümez, kaydırma çubuğu çıkar.
            const float margin = 2.0F * ImGui::GetStyle().WindowPadding.x;
            const ImVec2 work_size = ImGui::GetMainViewport()->WorkSize;
            // Negatif üst sınırı ImGui "kısıt yok" sayar: çok küçük pencerede diyalog taşardı.
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(0.0F, 0.0F),
                ImVec2(std::max(0.0F, work_size.x - margin), std::max(0.0F, work_size.y - margin)));
            if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                finished = body();
                keep_window_in_viewport();
                const ImVec2 modal_pos = ImGui::GetWindowPos();
                const ImVec2 modal_size = ImGui::GetWindowSize();
                item_rects_["modal"] = {modal_pos.x, modal_pos.y, modal_pos.x + modal_size.x,
                                        modal_pos.y + modal_size.y};
                if (finished)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        });
    }
    frames_left_ = kFramesAfterEvent;
    // Diyalog odağı aldı: kullanıcı yazmaya kaldığı yerden devam edebilsin.
    refocus_editor_ = was_editing;
}

std::optional<std::string> View::ask_folder_name(const std::string& title, const std::string& initial) {
    std::string name = initial;
    std::optional<std::string> result;
    bool first_frame = true;
    run_modal(title, [&]() -> bool {
        if (first_frame)
            ImGui::SetKeyboardFocusHere();
        first_frame = false;
        const std::string label = core::tr("gui.folder_name");
        ImGui::SetNextItemWidth(form_item_width(text_width(label), dialog_content_width()));
        bool submitted = ImGui::InputText("##folder_name", &name, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("dialog_folder_name");
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş: vazgeçti
        if (!submitted)
            return false;
        // Boş ad vazgeçmedir: hata gösterecek bir şey yok.
        const auto first = name.find_first_not_of(" \t");
        if (first != std::string::npos)
            result = name.substr(first, name.find_last_not_of(" \t") - first + 1);
        return true;
    });
    return result;
}

std::optional<std::string> View::ask_path(const std::string& title, const std::string& initial,
                                          PathKind kind) {
    std::string path = initial;
    std::optional<std::string> result;
    const auto native = std::make_shared<NativeDialogResult>();
    bool first_frame = true;
    run_modal(title, [&]() -> bool {
        {
            const std::scoped_lock lock(native->mutex);
            if (native->path)
                path = *std::exchange(native->path, std::nullopt);
        }
        if (first_frame)
            ImGui::SetKeyboardFocusHere();
        first_frame = false;
        const std::string browse_label = core::tr("gui.browse");
        ImGui::SetNextItemWidth(form_item_width(button_width(browse_label), dialog_content_width()));
        bool submitted = ImGui::InputText("##path", &path, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("dialog_path");
        ImGui::SameLine();
        const bool browse_clicked = ImGui::Button(browse_label.c_str());
        remember_item_center("dialog_browse");
        if (browse_clicked) {
            auto* userdata = new std::shared_ptr<NativeDialogResult>(native);  // sahibi native_dialog_done
            SDL_Window* window = platform_->window.get();
            if (kind == PathKind::Folder)
                SDL_ShowOpenFolderDialog(native_dialog_done, userdata, window, path.c_str(), false);
            else
                SDL_ShowSaveFileDialog(native_dialog_done, userdata, window, kPdfFilters, 1, path.c_str());
        }
        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş: vazgeçti
        if (!submitted)
            return false;
        if (!path.empty())
            result = path;
        return true;
    });
    return result;
}

std::optional<std::string> View::ask_export_path(const std::string& suggested) {
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);  // SDL sonda ayırıcıyı kendi koyar
    const std::string initial = documents != nullptr ? std::string(documents) + suggested : suggested;
    auto chosen = ask_path(core::tr("gui.export_dialog_title"), initial, PathKind::SaveFile);
    if (!chosen)
        return chosen;
    // Elle yazılan yol da var olan dosyayı sormadan ezmesin (yerel diyalog kendisi de sorar).
    std::error_code ignored;
    if (!std::filesystem::exists(std::filesystem::path(std::u8string(chosen->begin(), chosen->end())),
                                 ignored))
        return chosen;
    const bool replace =
        ask_choice(ChoiceBox<bool>{core::tr("gui.export_dialog_title"),
                                   core::tr("gui.confirm_overwrite", {{"path", *chosen}}),
                                   {{core::tr("gui.overwrite"), true}, {core::tr("gui.cancel"), false}},
                                   1,
                                   1});
    return replace ? chosen : std::nullopt;
}

template <typename Choice> Choice View::ask_choice(const ChoiceBox<Choice>& box) {
    std::size_t chosen = box.escape_button;
    run_modal(box.title, [&]() -> bool {
        const float content_width = dialog_content_width();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
        ImGui::TextUnformatted(box.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float row_start_x = ImGui::GetCursorScreenPos().x;
        bool answered = false;
        for (std::size_t i = 0; i < box.buttons.size(); ++i) {
            if (i > 0)
                flow_after_previous(button_width(box.buttons[i].first), row_start_x, content_width);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button(box.buttons[i].first.c_str())) {
                chosen = i;
                answered = true;
            }
            remember_item_center("choice:" + std::to_string(i));
            // Enter görünür gezinme imlecindeki düğmeyi etkinleştirir: imleç ilk karede varsayılan düğmeye
            // konur.
            if (i == box.default_button && ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
                ImGui::SetNavCursorVisible(true);
            }
            ImGui::PopID();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            chosen = box.escape_button;
            answered = true;
        }
        return answered;
    });
    return box.buttons[chosen].second;
}

std::optional<SaveDetails> View::ask_save_details(const SaveDialogRequest& request) {
    SaveForm form(request);
    std::optional<SaveDetails> result;
    bool first_frame = true;
    const std::string title = core::tr(
        request.mode == SaveDialogRequest::Mode::Move ? "gui.move_dialog_title" : "gui.save_dialog_title");
    run_modal(title, [&]() -> bool {
        const float trailing_width =
            std::max({text_width(core::tr("gui.field_name")), text_width(core::tr("gui.field_folder")),
                      text_width(core::tr("gui.field_type"))});
        ImGui::PushItemWidth(form_item_width(trailing_width, dialog_content_width()));
        bool submitted = false;

        ImGui::BeginDisabled(request.folder_only);
        if (first_frame && !request.folder_only)
            ImGui::SetKeyboardFocusHere();
        submitted |= ImGui::InputText("##name", &form.fields.name, ImGuiInputTextFlags_EnterReturnsTrue);
        remember_item_center("save_name");
        ImGui::SameLine();
        ImGui::TextUnformatted(core::tr("gui.field_name").c_str());
        ImGui::EndDisabled();

        if (first_frame && request.folder_only)
            ImGui::SetKeyboardFocusHere();
        const std::string root_hint = core::tr("gui.folder_root");
        submitted |= combo_field("folder", core::tr("gui.field_folder"), form.fields.folder, request.folders,
                                 true, root_hint.c_str());
        remember_item_center("save_folder");

        ImGui::BeginDisabled(request.folder_only);
        submitted |= combo_field("type", core::tr("gui.field_type"), form.fields.type, request.types, false);
        remember_item_center("save_type");
        ImGui::EndDisabled();
        ImGui::PopItemWidth();

        if (!request.hint.empty())
            ImGui::TextUnformatted(request.hint.c_str());
        draw_error(form.error);
        first_frame = false;

        submitted |= button("dialog_ok", core::tr("gui.ok"));
        ImGui::SameLine();
        if (button("dialog_cancel", core::tr("gui.cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            return true;  // result boş kalır: vazgeçti
        // Tek satırlı alanda Enter EnterReturnsTrue ile gelir; alan etkin değilken de Enter kabul sayılır.
        submitted |= !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        if (submitted && form.try_accept()) {
            result = form.details();
            return true;
        }
        return false;
    });
    return result;
}

}  // namespace noteapp::gui
