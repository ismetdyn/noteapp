#include "gui/view.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "gui/icon_font.hpp"
#include "gui/markdown_preview.hpp"
#include "pdf/fonts.hpp"
#include "pdf/markdown_blocks.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>  // sekme çubuğunun sırası (TabBarGetTabOrder), editör imleci, Ctrl+Tab pencere geçişi
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
// Gezgin satırı: derinlik başına girinti ve aç/kapa işaretinin yuvası (yazı boyu birimi).
constexpr float kTreeIndentEms = 1.0F;
constexpr float kTreeArrowEms = 1.0F;
// Sıra no sütunu: rakamlardan sonra ada kadar boşluk (yazı boyu birimi).
constexpr float kRowNumberGapEms = 0.5F;
// Araç çubuğunda eylem grupları arası ayraç payı.
constexpr float kToolbarGroupGapEms = 0.6F;
// Ayrıntılar tablosu: ad sütununun en dar hâli; daha dar panelde tablo yatay kaydırılır.
// Boyut ve tarih sütunları içerik genişliğindedir, ad kalanı alır ve "…" ile kısalır; ad bundan daralacaksa
// tablo yatay kaydırılır (varsayılan panel genişliğinin altında).
constexpr float kDetailsNameMinEms = 2.0F;
constexpr int kDetailsColumns = 4;
constexpr std::size_t kDayKeyLength = 10;  // "2026-11-02": kısa tarih biçimi gün değişince yeniden ölçülür
constexpr const char* kNoValue = "—";
constexpr const char* kExpandedMark = "▾";
constexpr const char* kCollapsedMark = "▸";
constexpr float kDialogTextEms = 32.0F;
// Pencere en küçük boyutun da altındaysa (bileşikçi sınırı uygulamayabilir) alan ve yazı bu kadar kalır.
// Negatif genişlik ImGui'de "sağdan pay" demektir, sarma genişliği 1 piksele iner.
constexpr float kMinFieldEms = 4.0F;
constexpr float kScaleRounding = 100.0F;
constexpr float kMinInlineFieldEms = 8.0F;    // daha darsa yanındaki düğmeler alt satıra iner
constexpr float kMaxLabelColumnShare = 0.5F;  // etiket sütunu alanı yok etmesin; uzun etiket sarılır
constexpr const char* kGeneralKeys[] = {"language", "notes_dir", "default_type", "default_folder",
                                        "types",    "icons",     "editor"};
constexpr const char* kSyncKeys[] = {"sync.enabled",      "sync.remote",      "sync.branch",
                                     "sync.idle_seconds", "sync.author_name", "sync.author_email"};
constexpr const char* kEllipsis = "…";
constexpr ImVec4 kErrorColor(0.75F, 0.22F, 0.17F, 1.0F);  // açık ve koyu temada okunur kırmızı
constexpr ImWchar kReplacementCodepoint = 0xFFFD;
constexpr const char* kLanguages[] = {"auto", "tr", "en"};
constexpr const char* kIconModes[] = {"nerd", "off"};
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

// Sığarsa önceki düğmeyle aynı satıra, sığmazsa alt satıra: dar panelde ve ölçekte kırpılmaz. Aynı satırda
// kaldıysa true.
bool flow_after_previous(float next_width, float row_start_x, float row_width) {
    const float next_end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_width;
    if (next_end > row_start_x + row_width)
        return false;
    ImGui::SameLine();
    return true;
}

// Alanın yanındaki düğmelere yer bırakır; alan çok daralacaksa satırın tamamını alır, düğmeler alta iner.
float field_width_beside(float trailing_width) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float beside = available - trailing_width - ImGui::GetStyle().ItemSpacing.x;
    return beside >= ImGui::GetFontSize() * kMinInlineFieldEms ? beside : available;
}

float settings_label_width(const SettingsPage& page, float available) {
    float widest =
        std::max(text_width(core::tr("gui.token_label")), text_width(core::tr("gui.setting_row_numbers")));
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

// Kod saklanır; kullanıcı okunur adını görür. Elle yazılmış bilinmeyen değer olduğu gibi gösterilir.
std::string choice_label(const std::string& key, std::span<const char* const> choices,
                         const std::string& code) {
    return std::ranges::find(choices, code) != choices.end() ? core::tr("gui." + key + "_" + code) : code;
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

// Editör altı paneller (bilgi, bağlantılar): en çok bu kadar satır, kalan alanın en çok bu payı; kalanı
// kaydırılır, editör yaşar.
constexpr std::size_t kPanelMaxRows = 8;
constexpr float kPanelMaxShare = 0.4F;
constexpr float kInfoPairGapEms = 1.5F;  // bilgi panelinde yan yana iki alan çifti arası

// Metindeki [[hedef]] ve senkrondan gelen ad güvenilmez: denetim baytı yer tutucu olur, çizim satırı kaymaz.
std::string printable(std::string_view raw) {
    constexpr unsigned char kFirstPrintable = 0x20;
    constexpr unsigned char kDelete = 0x7F;
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < kFirstPrintable || byte == kDelete)
            out += "\xEF\xBF\xBD";  // U+FFFD
        else
            out += c;
    }
    return out;
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

// Sabit tonlar: tema değişse de hem açık hem koyu zeminde okunur.
ImU32 icon_color(core::IconColor color) {
    switch (color) {
    case core::IconColor::Blue:
        return IM_COL32(64, 132, 214, 255);
    case core::IconColor::Cyan:
        return IM_COL32(32, 152, 168, 255);
    case core::IconColor::Green:
        return IM_COL32(64, 160, 88, 255);
    case core::IconColor::Yellow:
        return IM_COL32(196, 150, 32, 255);
    case core::IconColor::Red:
        return IM_COL32(212, 84, 72, 255);
    case core::IconColor::Magenta:
        return IM_COL32(170, 96, 200, 255);
    case core::IconColor::Default:
        break;
    }
    return ImGui::GetColorU32(ImGuiCol_Text);
}

// İkonu çizip metne ayrılan genişliği döner. Yuva sabit: orantılı fontta sembol genişlikleri farklı, adlar
// aynı sütunda başlasın.
float draw_icon(ImVec2 position, const core::FileIcon& icon, core::IconStyle style, bool selected) {
    if (style == core::IconStyle::Off)
        return 0.0F;
    const std::string glyph = core::icon_text(icon, style);
    // Seçili satırın vurgu zemininde tür rengi kaybolur (mavi üstüne mavi): metin rengi kullanılır.
    const ImU32 color = selected ? ImGui::GetColorU32(ImGuiCol_Text) : icon_color(icon.color);
    const float font_size = ImGui::GetFontSize();
    const float icon_size = font_size * View::kRowIconScale;
    position.y -= (icon_size - font_size) / 2.0F;  // satır ortasında kalsın
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), icon_size, position, color, glyph.c_str());
    return font_size * View::kRowIconSlotEms;
}

// Satır metni `width` içine sığmazsa sonu üç noktayla kesilir; kesildiyse true.
bool draw_fitted_row_text(ImVec2 position, const std::string& text, float width,
                          const std::vector<std::size_t>& highlights) {
    ImGui::LogText("%s\n",
                   text.c_str());  // çizim listesine yazılan metin ImGui günlüğüne (testler) de girsin
    if (text_width(text) <= width) {
        draw_row_text(position, text, highlights);
        return false;
    }
    const std::string_view visible =
        std::string_view(text).substr(0, fitting_bytes(text, width - text_width(kEllipsis)));
    draw_row_text(position, visible, highlights);
    ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + text_width(visible), position.y),
                                        ImGui::GetColorU32(ImGuiCol_Text), kEllipsis);
    return true;
}

bool plain_selectable(const std::string& text, bool selected, const std::vector<std::size_t>& highlights = {},
                      core::IconStyle icon_style = core::IconStyle::Off) {
    ImVec2 position = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable("##row", selected);
    const float icon_width = draw_icon(position, core::file_icon(text), icon_style, selected);
    position.x += icon_width;
    width -= icon_width;
    if (draw_fitted_row_text(position, text, width, highlights))
        full_text_tooltip(text);
    return clicked;
}

// Satırın kalıcı adı (test kancası ve ImGui kimliği): sıra değil yol, liste değişince tıklama başka satıra
// kaymasın.
std::string row_item_name(const BrowserRow& row) {
    return (row.kind == BrowserRowKind::Folder ? "folder:" : "note:") + row.path;
}

std::string row_label(const BrowserRow& row) {
    return row.kind == BrowserRowKind::Folder && row.path.empty() ? core::tr("gui.all_notes") : row.name;
}

// Ayrıntılar'ın boyut, değiştirilme, oluşturulma hücreleri; tarih kısa biçimde (bugünse saat).
std::array<std::string, 3> details_cells(const BrowserRow& row, std::chrono::system_clock::time_point now) {
    const auto time_text = [now](const std::optional<std::filesystem::file_time_type>& when) {
        return when ? core::short_time_text(*when, now) : std::string(kNoValue);
    };
    return {core::human_size(row.size), time_text(row.updated_at), time_text(row.created_at)};
}

core::FileIcon row_icon(const BrowserRow& row) {
    return row.kind == BrowserRowKind::Folder ? core::folder_icon(row.expanded) : core::file_icon(row.name);
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

// Son öğeye orta tık ya da Ctrl+tık: not yeni sekmede açılır (SPEC-gui "Sekmeler").
bool new_tab_click(bool clicked) {
    return (clicked && ImGui::GetIO().KeyCtrl) || ImGui::IsItemClicked(ImGuiMouseButton_Middle);
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
      frames_left_(kFramesAfterEvent) {
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
        case SDL_EVENT_KEY_DOWN:
            map_zoom_keys_ += map_zoom_step(event.key.key);
            break;
        default:
            break;
        }
    }
}

void View::render(const std::function<void()>& overlay) {
    pump_events();
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
    // Kare dışında: Ayrıntılar'da konum değişimi listeyi süzer, kare içinde çizilen satırlar bozulmasın.
    state_.reveal_selection();
}

int View::idle_timeout_ms() const {
    if (frames_left_ > 0 || map_animating_)
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
    if (modal_depth_ > 0)
        return;
    struct Shortcut {
        ImGuiKeyChord chord;
        void (*action)(GuiState&){};
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
        {ImGuiMod_Ctrl | ImGuiKey_L, [](GuiState& s) { s.toggle_links(); }},
        {ImGuiMod_Ctrl | ImGuiKey_I, [](GuiState& s) { s.toggle_info(); }},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, [](GuiState& s) { s.toggle_preview(); }},
        {ImGuiKey_F5, [](GuiState& s) { s.sync_now(); }},
        // Sekmeler: T(ab), W(indow) kapat; tarayıcı ve editörlerdeki yerleşik eşleşme.
        {ImGuiMod_Ctrl | ImGuiKey_T, [](GuiState& s) { s.new_tab(); }},
        {ImGuiMod_Ctrl | ImGuiKey_W, [](GuiState& s) { s.close_active_tab(); }},
        {ImGuiMod_Ctrl | ImGuiKey_Tab, [](GuiState& s) { s.next_tab(1); }},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, [](GuiState& s) { s.next_tab(-1); }},
        {ImGuiMod_Ctrl | ImGuiKey_PageDown, [](GuiState& s) { s.next_tab(1); }},
        {ImGuiMod_Ctrl | ImGuiKey_PageUp, [](GuiState& s) { s.next_tab(-1); }},
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
    draw_toolbar();
    draw_left_pane();
    ImGui::SameLine();
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
    const float min_width = kMinLeftPaneWidth * ui_scale_;
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

void View::draw_search_row() {
    ImGui::PushID(static_cast<int>(state_.query_generation()));
    if (pending_focus_ == FocusRequest::Search) {
        ImGui::SetKeyboardFocusHere();
        pending_focus_ = FocusRequest::None;
    }
    std::function<void(int)> on_arrow = [this](int delta) {
        queue([this, delta] { state_.navigate(delta); });
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
    if (entered && ImGui::GetIO().KeyCtrl)
        queue_open_selected_in_new_tab();
    else if (entered)
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

// Saklanan değer koddur; ImGui kimliği de koddan gelir, çeviri kimliği değiştirmez.
void View::draw_choice_combo(const std::string& key, std::span<const char* const> choices,
                             std::string& value) {
    const bool open = ImGui::BeginCombo("##value", choice_label(key, choices, value).c_str());
    remember_item_center("setting:" + key);
    if (!open)
        return;
    for (const char* choice : choices) {
        const std::string option = choice_label(key, choices, choice) + "###" + choice;
        if (ImGui::Selectable(option.c_str(), value == choice))
            value = choice;
        remember_item_center(key + ":" + choice);
    }
    ImGui::EndCombo();
}

void View::draw_setting_input(const std::string& key) {
    SettingsPage& page = state_.settings();
    std::string& value = page.field(key);
    if (key == "language") {
        draw_choice_combo(key, kLanguages, value);
        return;
    }
    if (key == "icons") {
        draw_choice_combo(key, kIconModes, value);
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
    if (key == "editor" || key == "icons" || key == "sync.remote" || key == "sync.idle_seconds") {
        draw_hint(setting_hint(key));
        remember_item_center("setting_hint:" + key);
    }
    if (!page.field_error(key).empty()) {
        draw_error(page.field_error(key));
        remember_item_center("setting_error:" + key);
    }
    ImGui::PopID();
}

// Görünüm tercihi config değildir: Kaydet/Geri al akışına girmez, hemen uygulanır ve yerleşimle saklanır.
void View::draw_row_numbers_setting() {
    begin_setting_row(core::tr("gui.setting_row_numbers"), "setting_label:row_numbers");
    bool on = state_.row_numbers();
    if (ImGui::Checkbox("##row_numbers", &on))
        queue([this, on] { state_.set_row_numbers(on); });
    remember_item_center("setting:row_numbers");
    draw_hint(core::tr("gui.setting_hint_row_numbers"));
    remember_item_center("setting_hint:row_numbers");
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
    // Sonda: config ayarları formun başında, kaydırmadan görünür kalır.
    ImGui::SeparatorText(core::tr("gui.settings_group_display").c_str());
    remember_item_center("settings_group_display");
    if (begin_form_table("##display", label_width)) {
        draw_row_numbers_setting();
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

void View::draw_note_menu(const core::NotePath& path) {
    if (!ImGui::BeginPopupContextItem("##note_menu"))
        return;
    // Eylem o satıra uygulanır: önce seçilir; seçim tutmadıysa (kayıt iptal) satırın notuna dokunulmaz. Kayıt
    // satırı kaydırabildiğinden denetim yola göredir.
    const auto on_row = [this, &path](void (GuiState::*action)()) {
        queue([this, path, action] {
            state_.open_note(path);
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
    // Sonda: klavyeyle menüde gezenin alıştığı öğe sırası kaymasın.
    if (ImGui::MenuItem(core::tr("gui.open_in_new_tab").c_str()))
        queue_open(path, true);
    ImGui::EndPopup();
}

void View::draw_toolbar() {
    struct ToolbarAction {
        const char* name{};
        const char* label_key{};
        const char* tooltip_key{};
        bool enabled{};
        std::function<void()> run;
        std::string icon;           // boşsa etiket ikonsuz; tabloda "sil" karşılığı glif yok
        bool starts_group = false;  // öncesine ayraç
    };
    const auto glyph = [this](const core::FileIcon& icon) {
        return core::icon_text(icon, state_.icon_style());
    };
    const ToolbarAction actions[] = {
        {"toolbar_new_note", "gui.toolbar_new_note", "gui.toolbar_new_note_tip", true,
         [this] { state_.new_note(); }, glyph(core::file_icon(""))},
        {"toolbar_new_folder", "gui.new_folder", "gui.toolbar_new_folder_tip", true,
         [this] { state_.new_folder(state_.target_folder()); }, glyph(core::folder_icon(false))},
        {"toolbar_rename",
         "gui.rename_folder",
         "gui.toolbar_rename_tip",
         state_.can_rename(),
         [this] { state_.rename_selected(); },
         {},
         true},
        {"toolbar_move", "gui.toolbar_move", "gui.toolbar_move_tip", state_.can_move(),
         [this] { state_.move_note(); }, glyph(core::folder_icon(true))},
        {"toolbar_delete",
         "gui.delete",
         "gui.toolbar_delete_tip",
         state_.can_delete(),
         [this] { state_.delete_selected(false); },
         {}},
        {"toolbar_trash", nullptr, "gui.toolbar_trash_tip", true, [this] { state_.toggle_trash(); },
         glyph(core::trash_icon()), true},
        {"toolbar_export", "gui.toolbar_export", "gui.toolbar_export_tip", state_.can_export(),
         [this] { state_.export_pdf(); }, glyph(core::file_icon("a.pdf"))},
    };
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetFontSize() * kToolbarGroupGapEms;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    bool first = true;
    // Fare içindir: her eylemin kısayolu ya da bağlam menüsü var; Tab sırası arama ile liste arasında
    // uzamasın.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const std::string trash_label =
        core::tr("gui.toolbar_trash", {{"count", std::to_string(state_.trash_count())}});
    for (const ToolbarAction& action : actions) {
        const std::string text = action.label_key ? core::tr(action.label_key) : trash_label;
        const std::string label = action.icon.empty() ? text : action.icon + " " + text;
        if (!first) {
            const float lead = action.starts_group ? gap + spacing : 0.0F;
            // Ayraç yalnız aynı satırda: alt satıra inen grup zaten ayrıdır.
            if (flow_after_previous(lead + button_width(label), row_start_x, row_width) && lead > 0.0F)
                draw_toolbar_separator(gap);
        }
        first = false;
        ImGui::BeginDisabled(!action.enabled);
        if (button(action.name, label))
            queue(action.run);
        ImGui::SetItemTooltip(
            "%s", core::tr(action.tooltip_key).c_str());  // kapalı düğmede de neden/kısayol görünsün
        ImGui::EndDisabled();
    }
    draw_view_switch(row_start_x, row_width, gap);
    ImGui::PopItemFlag();
}

void View::draw_toolbar_separator(float gap) {
    ImGui::Dummy(ImVec2(gap, ImGui::GetFrameHeight()));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float x = std::round((min.x + max.x) / 2.0F);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y), ImVec2(x, max.y),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::SameLine();
}

// Görünüm seçici sağa yaslı: sığmazsa alt satıra iner, yine sağda durur.
void View::draw_view_switch(float row_start_x, float row_width, float gap) {
    struct ViewChoice {
        const char* name;
        const char* label_key;
        const char* tooltip_key;
        BrowserView view;
    };
    static constexpr ViewChoice kChoices[] = {
        {"toolbar_view_tree", "gui.view_tree", "gui.view_tree_tip", BrowserView::Tree},
        {"toolbar_view_details", "gui.view_details", "gui.view_details_tip", BrowserView::Details},
        {"toolbar_view_map", "gui.view_map", "gui.view_map_tip", BrowserView::Map},
    };
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = -spacing;
    for (const ViewChoice& choice : kChoices)
        width += button_width(core::tr(choice.label_key)) + spacing;
    flow_after_previous(gap + spacing + width, row_start_x, row_width);
    ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, row_start_x + row_width - width),
                                     ImGui::GetCursorScreenPos().y));
    bool first = true;
    for (const ViewChoice& choice : kChoices) {
        const std::string label = core::tr(choice.label_key);
        if (!first)
            flow_after_previous(button_width(label), row_start_x, row_width);
        first = false;
        const bool active = state_.browser_view() == choice.view;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (button(choice.name, label))
            queue([this, view = choice.view] { state_.set_browser_view(view); });
        if (active)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", core::tr(choice.tooltip_key).c_str());
    }
}

// Kaydırılan listenin dışında: çok satırda da düğmeler görünür kalır.
void View::draw_trash_header() {
    const std::string trash_icon = core::icon_text(core::trash_icon(), state_.icon_style());
    ImGui::TextUnformatted(
        ((trash_icon.empty() ? "" : trash_icon + " ") + core::tr("gui.trash_title")).c_str());
    remember_item_center("trash_title");
    struct TrashAction {
        const char* name{};
        const char* label_key{};
        bool enabled{};
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
            if (plain_selectable(items[static_cast<std::size_t>(row)].path.string(), row == current, {},
                                 state_.icon_style()))
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
    const bool browsing = state_.browsing();
    const bool details = browsing && state_.browser_view() == BrowserView::Details;
    if (trash)
        draw_trash_header();
    if (details)
        draw_details_path();
    // Ayrıntılar tablosu dar panelde yatay kaydırılır; diğer görünümlerde içerik zaten panel genişliğindedir.
    ImGui::BeginChild("##notes", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // Klavyeyle (arama kutusunda Shift+Tab, Enter) girilince gezinti imleci seçili satırda başlar; yoksa
    // imleç ve seçim ayrı satırlarda yürür, bağlam menüsü ile Del farklı notlara giderdi. Fareyle odak zaten
    // satırı seçer.
    const bool entered_by_keyboard = ImGui::IsWindowFocused() && !list_focused_ && !ImGui::IsAnyMouseDown();
    const bool focus_row = pending_focus_ == FocusRequest::List || entered_by_keyboard;
    if (pending_focus_ == FocusRequest::List) {
        ImGui::SetWindowFocus();
        pending_focus_ = FocusRequest::None;
    }
    // Listeye bu karede girildiyse (Enter ile) aynı tuş içeride ikinci kez işlenmesin.
    const bool keys_for_list = list_focused_ && ImGui::IsWindowFocused();
    list_focused_ = ImGui::IsWindowFocused();
    const std::size_t root_rows = details ? 0 : 1;  // ağaçta kök satırı hep vardır
    const bool empty = trash      ? state_.trash_items().empty()
                       : browsing ? state_.browser_rows().size() <= root_rows
                                  : state_.model().row_count() == 0;
    if (empty) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr(trash ? "gui.trash_empty_hint" : "gui.empty_list_hint").c_str());
        ImGui::PopTextWrapPos();
    }
    if (trash)
        draw_trash_rows(focus_row);
    else if (details)
        draw_details_rows(focus_row);
    else if (browsing)
        draw_tree_rows(focus_row);
    else
        draw_note_rows(focus_row);
    if (keys_for_list) {
        const auto on_key = [this](ImGuiKey key, void (GuiState::*action)()) {
            if (ImGui::IsKeyPressed(key)) {
                follow_selection_ = true;
                queue([this, action] { (state_.*action)(); });
            }
        };
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            follow_selection_ = true;
            queue([this] { state_.navigate(-1); });
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            follow_selection_ = true;
            queue([this] { state_.navigate(1); });
        }
        const bool enter =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (enter && ImGui::GetIO().KeyCtrl) {
            queue_open_selected_in_new_tab();
        } else if (browsing) {
            on_key(ImGuiKey_RightArrow, &GuiState::browse_right);
            on_key(ImGuiKey_LeftArrow, &GuiState::browse_left);
            if (enter)
                queue([this] { state_.browse_enter(); });
        }
        // Del yalnız liste odaktayken: editörde/arama kutusunda karakter silmeyi ezmesin.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            queue([this, permanent = ImGui::GetIO().KeyShift] { state_.delete_selected(permanent); });
    }
    ImGui::EndChild();
    remember_item_center("note_list");
}

// Seçim değiştiyse (ya da klavyeyle gelindiyse) seçili satır çizilsin, gezinti imleci ona gelsin,
// görünmüyorsa kaydırılsın. Dönen değer: seçili satırın adı değişti mi.
bool View::begin_selected_row(int current, bool focus_row, ImGuiListClipper& clipper) {
    const auto& rows = state_.browser_rows();
    const bool changed =
        current >= 0 && row_item_name(rows[static_cast<std::size_t>(current)]) != scrolled_item_;
    if (current >= 0 && (changed || focus_row || follow_selection_))
        clipper.IncludeItemByIndex(current);
    return changed;
}

void View::end_selected_row(const std::string& name, bool changed) {
    const ItemRect& rect = item_rects_.at(name);
    if (changed) {
        // Fareyle tıklanan satır yerinde kalır: ortalamak ikinci tıklamayı başka satıra düşürürdü.
        if (!ImGui::IsRectVisible(ImVec2(rect.min_x, rect.min_y), ImVec2(rect.max_x, rect.max_y)))
            ImGui::SetScrollHereY();
        scrolled_item_ = name;
    }
}

void View::draw_tree_rows(bool focus_row) {
    const auto& rows = state_.browser_rows();
    const int current = state_.browser_row();
    ImGuiListClipper clipper;  // 5.000 satırda yalnız görünenler çizilir
    clipper.Begin(static_cast<int>(rows.size()));
    const bool changed = begin_selected_row(current, focus_row, clipper);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            const std::string name = row_item_name(row);
            ImGui::PushID(name.c_str());
            if (index == current && (focus_row || follow_selection_)) {
                ImGui::SetKeyboardFocusHere();
                follow_selection_ = false;
            }
            draw_tree_row(row, name, index == current);
            if (index == current)
                end_selected_row(name, changed);
            ImGui::PopID();
        }
    }
}

void View::draw_tree_row(const BrowserRow& row, const std::string& name, bool selected) {
    const bool folder = row.kind == BrowserRowKind::Folder;
    const bool toggles = folder && !row.path.empty() && row.has_children;
    ImVec2 position = ImGui::GetCursorScreenPos();
    const float row_end = position.x + ImGui::GetContentRegionAvail().x;
    const float font = ImGui::GetFontSize();
    // Derin ağaçta ad yok olmasın: girinti satırın yarısını geçmez.
    const float indent =
        std::min(static_cast<float>(row.depth) * font * kTreeIndentEms, (row_end - position.x) / 2.0F);
    // TUI sırası: girinti, sıra no, ok, ikon, ad.
    const float number_width = state_.row_numbers() ? row_number_width() : 0.0F;
    const float arrow_start = position.x + indent + number_width;
    const float arrow_end = arrow_start + font * kTreeArrowEms;
    const bool clicked = ImGui::Selectable("##row", selected);
    const bool new_tab = !folder && new_tab_click(clicked);
    remember_item_center(name);
    const bool hovered = ImGui::IsItemHovered();
    const bool on_arrow = toggles && hovered && ImGui::GetMousePos().x < arrow_end;
    if (number_width > 0.0F && row.number > 0)
        draw_row_number("row_number:" + name, row.number, ImVec2(position.x + indent, position.y), number_width);
    if (toggles)
        ImGui::GetWindowDrawList()->AddText(ImVec2(arrow_start, position.y), ImGui::GetColorU32(ImGuiCol_Text),
                                            row.expanded ? kExpandedMark : kCollapsedMark);
    position.x = arrow_end;
    position.x += draw_icon(position, row_icon(row), state_.icon_style(), selected);
    if (draw_fitted_row_text(position, row_label(row), row_end - position.x, {}))
        full_text_tooltip(row.path);
    if (clicked || new_tab) {
        if (on_arrow)
            queue([this, path = row.path] { state_.toggle_expanded(path); });
        else if (folder)
            queue([this, path = row.path] { state_.select_folder(path); });
        else if (const auto path = core::NotePath::parse(row.path))
            queue_open(*path, new_tab);
    }
    if (toggles && hovered && !on_arrow && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        queue([this, path = row.path] { state_.toggle_expanded(path); });
    draw_row_menu(row);
}

// En büyük numaranın rakamları ve ada kadar boşluk; rakamlar orantılı fontta da aynı genişliktedir.
float View::row_number_width() {
    const std::size_t digits = std::to_string(std::max<std::size_t>(state_.max_row_number(), 1)).size();
    return (static_cast<float>(digits) * text_width("0")) + (ImGui::GetFontSize() * kRowNumberGapEms);
}

// Sağa yaslı, soluk (TUI'deki gibi); boşluk payı sağda kalır.
void View::draw_row_number(const std::string& name, std::size_t number, ImVec2 position, float width) {
    const std::string text = std::to_string(number);
    ImGui::LogText("%s ", text.c_str());  // çizim listesine yazılan metin ImGui günlüğüne (testler) de girsin
    const float text_x = position.x + width - (ImGui::GetFontSize() * kRowNumberGapEms) - text_width(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, position.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        text.c_str());
    item_rects_[name] = {text_x, position.y, text_x + text_width(text), position.y + ImGui::GetTextLineHeight(),
                         false};
}

void View::draw_row_menu(const BrowserRow& row) {
    if (row.kind == BrowserRowKind::Folder)
        draw_folder_menu(row.path);
    else if (const auto path = core::NotePath::parse(row.path))
        draw_note_menu(*path);
}

// Konum: Yukarı ve her ata için bir düğme (Dolphin'deki yol çubuğu). Kaydırılan listenin dışında kalır.
void View::draw_details_path() {
    const std::string& folder = state_.current_folder();
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float padding = 2.0F * ImGui::GetStyle().FramePadding.x;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::BeginDisabled(folder.empty());
    if (button("details_up", core::tr("gui.details_up")))
        queue([this, parent = core::parent_folder(folder)] { state_.enter_folder(parent); });
    ImGui::SetItemTooltip("%s", core::tr("gui.details_up_tip").c_str());
    ImGui::EndDisabled();
    std::vector<std::string> crumbs{""};
    for (std::size_t slash = folder.find('/'); !folder.empty(); slash = folder.find('/', slash + 1)) {
        crumbs.push_back(folder.substr(0, slash));
        if (slash == std::string::npos)
            break;
    }
    for (const std::string& crumb : crumbs) {
        std::string label = crumb.empty() ? core::tr("gui.all_notes") : core::last_component(crumb);
        if (button_width(label) > row_width)
            label = keep_start(label, row_width - padding);
        flow_after_previous(button_width(label), row_start_x, row_width);
        if (button("crumb:" + crumb, label))
            queue([this, crumb] { state_.enter_folder(crumb); });
        if (!crumb.empty())
            full_text_tooltip(crumb);
    }
    ImGui::PopItemFlag();
}

void View::draw_details_rows(bool focus_row) {
    const auto& rows = state_.browser_rows();
    const int current = state_.browser_row();
    const auto now = std::chrono::system_clock::now();
    const std::array<float, 3>& widths = details_column_widths(rows, now);
    // Sıra no açıksa ilk sütundur; diğer sütunların adları (details_header:N) yerinde kalır.
    const int first = state_.row_numbers() ? 1 : 0;
    const int columns = kDetailsColumns + first;
    const float number_width = first > 0 ? row_number_width() : 0.0F;
    // Sütun başına iki hücre payı ve aradaki çizgi.
    const float chrome = static_cast<float>(columns) * (2.0F * ImGui::GetStyle().CellPadding.x + 1.0F);
    const float fixed = widths[0] + widths[1] + widths[2] + number_width + chrome;
    const float width =
        std::max(ImGui::GetContentRegionAvail().x, fixed + ImGui::GetFontSize() * kDetailsNameMinEms);
    // Yeniden boyutlandırılamaz: ImGui sabit sütunu ancak böyle her karede verilen içerik genişliğinde tutar.
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##details", columns, kFlags, ImVec2(width, 0.0F)))
        return;
    if (first > 0)
        ImGui::TableSetupColumn(core::tr("gui.details_number").c_str(), ImGuiTableColumnFlags_WidthFixed,
                                number_width);
    ImGui::TableSetupColumn(core::tr("gui.details_name").c_str(), ImGuiTableColumnFlags_WidthStretch);
    const char* const fixed_keys[] = {"gui.details_size", "gui.details_updated", "gui.details_created"};
    for (std::size_t column = 0; column < widths.size(); ++column)
        ImGui::TableSetupColumn(core::tr(fixed_keys[column]).c_str(), ImGuiTableColumnFlags_WidthFixed,
                                widths[column]);
    // Başlık elle: sütun konumu test edilir; sabit sütunda kısalan başlığın tamamı ipucunda.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int column = 0; column < columns; ++column) {
        ImGui::TableSetColumnIndex(column);
        const std::string label = ImGui::TableGetColumnName(column);
        ImGui::TableHeader(label.c_str());
        remember_item_center("details_header:" +
                             (column < first ? std::string("number") : std::to_string(column - first)));
        if (text_width(label) > ImGui::GetItemRectSize().x)
            full_text_tooltip(label);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    const bool changed = begin_selected_row(current, focus_row, clipper);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            const std::string name = row_item_name(row);
            ImGui::TableNextRow();
            if (first > 0) {
                ImGui::TableSetColumnIndex(0);
                draw_row_number("details_number:" + row.path, row.number, ImGui::GetCursorScreenPos(),
                                number_width);
            }
            ImGui::TableSetColumnIndex(first);
            ImGui::PushID(name.c_str());
            if (index == current && (focus_row || follow_selection_)) {
                ImGui::SetKeyboardFocusHere();
                follow_selection_ = false;
            }
            draw_details_row(row, name, index == current, now);
            if (index == current)
                end_selected_row(name, changed);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void View::draw_details_row(const BrowserRow& row, const std::string& name, bool selected,
                            std::chrono::system_clock::time_point now) {
    const bool folder = row.kind == BrowserRowKind::Folder;
    ImVec2 position = ImGui::GetCursorScreenPos();
    const float cell_end = position.x + ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns);
    const bool new_tab = !folder && new_tab_click(clicked);
    remember_item_center(name);
    const bool double_clicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    position.x += draw_icon(position, row_icon(row), state_.icon_style(), selected);
    if (draw_fitted_row_text(position, row.name, cell_end - position.x, {}))
        full_text_tooltip(row.path);
    if (clicked || new_tab) {
        if (folder)
            queue([this, path = row.path] { state_.select_folder(path); });
        else if (const auto path = core::NotePath::parse(row.path))
            queue_open(*path, new_tab);
    }
    if (folder && double_clicked)
        queue([this, path = row.path] { state_.enter_folder(path); });
    draw_row_menu(row);
    const int name_column = ImGui::TableGetColumnIndex();
    for (int column = 1; const std::string& cell : details_cells(row, now)) {
        ImGui::TableSetColumnIndex(name_column + column);
        ImGui::TextUnformatted(cell.c_str());
        remember_item_center("details_cell:" + std::to_string(column++) + ":" + row.path);
    }
}

// Tüm satırların en geniş hücresi; yalnız satırlar, yazı boyu ya da gün değişince yeniden ölçülür (kare
// başına binlerce satır ölçülmesin, kaydırınca sütun oynamasın).
const std::array<float, 3>& View::details_column_widths(const std::vector<BrowserRow>& rows,
                                                        std::chrono::system_clock::time_point now) {
    std::string day = core::local_time_text(std::chrono::clock_cast<std::chrono::file_clock>(now));
    day.resize(std::min(day.size(), kDayKeyLength));
    const std::size_t rebuilds = state_.browser_rebuilds();
    const float font = ImGui::GetFontSize();
    if (rebuilds == details_widths_rebuilds_ && font == details_widths_font_ && day == details_widths_day_)
        return details_widths_;
    details_widths_.fill(text_width(kNoValue));
    for (const BrowserRow& row : rows) {
        const auto cells = details_cells(row, now);
        for (std::size_t column = 0; column < cells.size(); ++column)
            details_widths_[column] = std::max(details_widths_[column], text_width(cells[column]));
    }
    details_widths_rebuilds_ = rebuilds;
    details_widths_font_ = font;
    details_widths_day_ = std::move(day);
    return details_widths_;
}

void View::draw_folder_summary() {
    const auto summary = state_.folder_summary();
    ImGui::BeginChild("##folder_summary", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
    if (summary) {
        const std::string icon = core::icon_text(core::folder_icon(true), state_.icon_style());
        const std::string title = summary->path.empty() ? core::tr("gui.all_notes") : summary->path;
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted((icon.empty() ? title : icon + " " + title).c_str());
        ImGui::TextUnformatted(
            core::tr("gui.folder_summary_counts", {{"notes", std::to_string(summary->notes)},
                                                   {"folders", std::to_string(summary->folders)}})
                .c_str());
        ImGui::PopTextWrapPos();
        draw_hint(core::tr("gui.folder_summary_hint"));
    }
    ImGui::EndChild();
    remember_item_center("folder_summary");
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
            const bool clicked = plain_selectable(model.display(row), row == current, model.highlights(row),
                                                  state_.icon_style());
            if (new_tab_click(clicked))
                queue_open(model.note_at(row).path, true);
            else if (clicked)
                queue([this, row] { state_.select_row(row); });
            draw_note_menu(model.note_at(row).path);
            if (row == current && current != scrolled_row_) {
                ImGui::SetScrollHereY();
                scrolled_row_ = current;
            }
            ImGui::PopID();
        }
    }
}

namespace {

// Harita (SPEC-gui "Harita"). Ölçüler yazı boyu (em) ya da yerleşim birimidir (GraphLayout::kIdealEdge).
constexpr float kMapMinZoom = 0.01F;  // 2.000 düğümlük çizge en küçük pencerede de sığar
constexpr float kMapMaxZoom = 4.0F;
constexpr float kMapWheelZoomStep = 1.2F;  // tekerlek çentiği başına
constexpr float kMapKeyZoomStep = 1.25F;   // '+' / '-' başına
constexpr float kMapKeyPanEms = 3.0F;      // ok tuşu başına
constexpr float kMapFitMarginEms = 2.0F;   // sığdırmada pay: kenardaki düğümün etiketi tuvalden taşmasın
constexpr float kMapNodeWorldRadius = 12.0F;
constexpr float kMapNodeMinRadiusEms = 0.25F;  // uzaktan bakınca da düğüm görünsün
constexpr float kMapTagRadiusShare = 0.7F;     // etiket nottan küçük kare
constexpr float kMapHitMinRadiusEms = 0.5F;    // küçük düğüm de tıklanabilsin
constexpr float kMapLabelSpacingEms = 3.5F;    // komşular bu kadar açılınca tüm etiketler görünür
constexpr float kMapArrowSpacingEms = 2.5F;    // ok ucu bundan sıkışık çizgede gürültüdür
// Etiket komşu aralığının bu katında kısalır (yan yana adlar az biner), en çok 12 em; tam yol ipucunda.
constexpr float kMapLabelSpacingShare = 1.6F;
constexpr float kMapLabelMinEms = 3.0F;
constexpr float kMapLabelMaxEms = 12.0F;
constexpr float kMapArrowLengthEms = 0.6F;
constexpr float kMapArrowHalfWidthEms = 0.3F;
constexpr float kMapRingGapEms = 0.2F;  // geçerli notun halkası ve etiket boşluğu
constexpr float kMapRingThickness = 2.0F;
constexpr float kMapEdgeAlpha = 0.35F;
constexpr float kMapRelatedAlpha = 0.8F;  // ilişki yönsüzdür: oksuz, renkli ve bağlantıdan belirgin
constexpr float kMapDimAlpha = 0.2F;      // aramada eşleşmeyen düğüm ve kenar
constexpr float kMapLabelBackgroundAlpha = 0.8F;
constexpr int kMapNodeSegments = 12;  // 2.000 dairede köşe sayısı sınırlı kalsın
constexpr float kMapDepthSliderEms = 7.0F;

ImU32 with_alpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

float checkbox_width(const std::string& label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + text_width(label);
}

void fit_camera(MapCamera& camera, const std::vector<LayoutPoint>& positions, ImVec2 size) {
    if (positions.empty())
        return;
    LayoutPoint low = positions.front();
    LayoutPoint high = positions.front();
    for (const LayoutPoint p : positions) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
    }
    const float margin = kMapFitMarginEms * ImGui::GetFontSize();
    const float room_x = std::max(size.x - 2.0F * margin, 1.0F);
    const float room_y = std::max(size.y - 2.0F * margin, 1.0F);
    const float span_x = std::max(high.x - low.x, GraphLayout::kIdealEdge);
    const float span_y = std::max(high.y - low.y, GraphLayout::kIdealEdge);
    camera.zoom = std::clamp(std::min(room_x / span_x, room_y / span_y), kMapMinZoom, kMapMaxZoom);
    camera.offset_x = size.x / 2.0F - (low.x + high.x) / 2.0F * camera.zoom;
    camera.offset_y = size.y / 2.0F - (low.y + high.y) / 2.0F * camera.zoom;
}

// Kenarlar SDL çizgisiyle çizilir, ImGui üçgeniyle değil. ImGui'nin her dolgusu doku örnekler: SDL'nin
// yazılımsal doldurucusu üçgenin sınır kutusundaki her pikseli sınar ve içerideki her pikselde doku
// koordinatını 64 bitlik bölmeyle hesaplar (SDL_triangle.c). L boyunda çapraz kenar ~L²/2 sınama isterdi;
// yerleşim sürerken ekranı kesen 5.000 kenar kareyi 40 ms'ye çıkarıyordu. SDL çizgisi dokusuz ve boyuyla
// orantılıdır (Bresenham). ImGui_ImplSDLRenderer3_RenderDrawData bu komutu sırasında çağırır: kırpma komutun
// dikdörtgenidir, sonraki ImGui komutları kendi kırpmasını yeniden kurar. View::render SDL ölçeğini çerçeve
// ölçeğine kurduğundan koordinatlar ImGui birimindedir.
void draw_map_lines(const ImDrawList* /*list*/, const ImDrawCmd* command) {
    const auto* batches = static_cast<const std::vector<MapLineBatch>*>(command->UserCallbackData);
    SDL_Renderer* renderer = ImGui_ImplSDLRenderer3_GetRenderState()->Renderer;
    const ImVec4& clip = command->ClipRect;
    const SDL_Rect rect{static_cast<int>(clip.x), static_cast<int>(clip.y), static_cast<int>(clip.z - clip.x),
                        static_cast<int>(clip.w - clip.y)};
    SDL_SetRenderClipRect(renderer, &rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const MapLineBatch& batch : *batches) {
        const ImVec4 color = ImGui::ColorConvertU32ToFloat4(batch.color);
        SDL_SetRenderDrawColorFloat(renderer, color.x, color.y, color.z, color.w);
        for (std::size_t i = 0; i + 3 < batch.coords.size(); i += 4)
            SDL_RenderLine(renderer, batch.coords[i], batch.coords[i + 1], batch.coords[i + 2],
                           batch.coords[i + 3]);
    }
}

void draw_arrow(ImDrawList* draw, ImVec2 from, ImVec2 to, float gap, ImU32 color) {
    const float font = ImGui::GetFontSize();
    const float length = std::hypot(to.x - from.x, to.y - from.y);
    const float arrow = kMapArrowLengthEms * font;
    if (length <= gap + arrow)
        return;
    const float ux = (to.x - from.x) / length;
    const float uy = (to.y - from.y) / length;
    const ImVec2 tip(to.x - ux * gap, to.y - uy * gap);
    const ImVec2 base(tip.x - ux * arrow, tip.y - uy * arrow);
    const float half = kMapArrowHalfWidthEms * font;
    draw->AddTriangleFilled(tip, ImVec2(base.x - uy * half, base.y + ux * half),
                            ImVec2(base.x + uy * half, base.y - ux * half), color);
}

std::string map_label(const MapNode& node) {
    if (node.kind == MapNodeKind::Tag)
        return printable("#" + node.key);
    const auto slash = node.key.rfind('/');
    return printable(slash == std::string::npos ? node.key : node.key.substr(slash + 1));
}

}  // namespace

// Harita sol bölmede, ağacın yerinde; editör sağda kalır. Kare başına: girdi, sınırlı yerleşim adımı, çizim.
void View::draw_map() {
    const NoteMap& map = state_.note_map();
    draw_map_controls(map);
    ImGui::BeginChild("##map", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0F),
                      std::max(ImGui::GetContentRegionAvail().y, 1.0F));
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    remember_item_center("map_canvas");
    if (map.nodes.empty()) {
        map_zoom_keys_ = 0;
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.empty_list_hint").c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    if (const std::size_t generation = state_.map_generation(); generation != map_cache_.generation) {
        map_cache_.generation = generation;
        map_cache_.names.clear();
        map_cache_.labels.clear();
        map_cache_.colors.clear();
        for (const MapNode& node : map.nodes) {
            const bool tag = node.kind == MapNodeKind::Tag;
            map_cache_.names.push_back((tag ? "map:#" : "map:") + node.key);
            map_cache_.labels.push_back(map_label(node));
            map_cache_.colors.push_back(tag ? core::IconColor::Default : core::file_icon(node.key).color);
        }
        map_camera_.auto_fit = true;  // çevre değişti: yeni düğümler görünsün
        map_pressed_.reset();
    }

    const auto& positions = state_.map_layout().positions();
    int hovered = -1;
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float font = ImGui::GetFontSize();
        const float reach = std::max(kMapNodeWorldRadius * map_camera_.zoom, kMapHitMinRadiusEms * font);
        float best = reach * reach;
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const float dx = origin.x + map_camera_.offset_x + positions[i].x * map_camera_.zoom - mouse.x;
            const float dy = origin.y + map_camera_.offset_y + positions[i].y * map_camera_.zoom - mouse.y;
            if (dx * dx + dy * dy <= best) {
                best = dx * dx + dy * dy;
                hovered = static_cast<int>(i);
            }
        }
    }
    handle_map_input(map, origin, size, hovered);
    map_animating_ = state_.map_layout().step(kMapIterationsPerFrame);
    if (map_camera_.auto_fit)
        fit_camera(map_camera_, positions, size);
    draw_map_graph(map, origin, hovered);
    ImGui::EndChild();
}

void View::draw_map_controls(const NoteMap& map) {
    MapOptions options = state_.map_options();
    bool changed = false;
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const std::string whole = core::tr("gui.map_whole");
    changed |= ImGui::Checkbox((whole + "###map_whole").c_str(), &options.whole_graph);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_whole_tip").c_str());
    remember_item_center("map_whole");

    const float depth_width = kMapDepthSliderEms * ImGui::GetFontSize();
    flow_after_previous(depth_width, row_start_x, row_width);
    ImGui::BeginDisabled(options.whole_graph);
    ImGui::SetNextItemWidth(depth_width);
    // Biçim katalogdan gelir ama içine yalnız "%d" konur: ImGui biçim dizgisi çevirmen metninden kurulmaz.
    const std::string depth_format = core::tr("gui.map_depth", {{"depth", "%d"}});
    changed |= ImGui::SliderInt("##map_depth", &options.depth, MapOptions::kMinDepth, MapOptions::kMaxDepth,
                                depth_format.c_str(), ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_depth_tip").c_str());
    remember_item_center("map_depth");
    ImGui::EndDisabled();

    const std::string tags = core::tr("gui.map_tags");
    flow_after_previous(checkbox_width(tags), row_start_x, row_width);
    changed |= ImGui::Checkbox((tags + "###map_tags").c_str(), &options.show_tags);
    ImGui::SetItemTooltip("%s", core::tr("gui.map_tags_tip").c_str());
    remember_item_center("map_tags");
    if (changed)
        queue([this, options] { state_.set_map_options(options); });

    if (map.capped()) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(core::tr("gui.map_capped", {{"shown", std::to_string(map.nodes.size())},
                                                           {"total", std::to_string(map.total)}})
                                   .c_str());
        ImGui::PopTextWrapPos();
        remember_item_center("map_capped");
    }
}

// Sol tuş: düğümde basılıp bırakılırsa açar, sürüklenirse iğneler; boşlukta sürükleme ve orta tuş kaydırır.
// Tuşlar yalnız tuval odaktayken: arama kutusunda yazılan '+' haritayı yakınlaştırmasın.
void View::handle_map_input(const NoteMap& map, ImVec2 origin, ImVec2 size, int hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    MapCamera& camera = map_camera_;
    const auto note_path = [&map](std::size_t node) -> std::optional<core::NotePath> {
        if (map.nodes[node].kind != MapNodeKind::Note)
            return std::nullopt;
        auto path = core::NotePath::parse(map.nodes[node].key);
        return path ? std::optional(std::move(*path)) : std::nullopt;
    };
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        map_pressed_ = hovered >= 0 ? std::optional(static_cast<std::size_t>(hovered)) : std::nullopt;
        map_dragged_ = false;
    }
    const bool active = ImGui::IsItemActive();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        camera.auto_fit = false;
        if (map_pressed_) {
            state_.map_layout().pin(*map_pressed_,
                                    {(io.MousePos.x - origin.x - camera.offset_x) / camera.zoom,
                                     (io.MousePos.y - origin.y - camera.offset_y) / camera.zoom});
            map_dragged_ = true;
        } else {
            camera.offset_x += io.MouseDelta.x;
            camera.offset_y += io.MouseDelta.y;
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        camera.auto_fit = false;
        camera.offset_x += io.MouseDelta.x;
        camera.offset_y += io.MouseDelta.y;
    }
    if (ImGui::IsItemDeactivated()) {
        if (map_pressed_ && !map_dragged_) {
            if (const auto path = note_path(*map_pressed_)) {
                if (io.KeyCtrl)
                    queue_open(*path, true);
                else
                    queue([this, path = *path] { state_.open_map_note(path); });
            }
        }
        map_pressed_.reset();
    }
    if (hovered >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (const auto path = note_path(static_cast<std::size_t>(hovered))) {
            queue([this, path = *path] {
                state_.open_map_note(path);
                if (state_.current_path() == path)  // kayıttan vazgeçildiyse görünüm de kalır
                    state_.set_browser_view(BrowserView::Tree);
            });
        }
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0F)
        zoom_map(origin, io.MousePos, std::pow(kMapWheelZoomStep, io.MouseWheel));

    const int zoom_steps = std::exchange(map_zoom_keys_, 0);
    if (!ImGui::IsWindowFocused())
        return;
    const float pan = kMapKeyPanEms * ImGui::GetFontSize();
    const std::pair<ImGuiKey, ImVec2> arrows[] = {{ImGuiKey_LeftArrow, ImVec2(pan, 0.0F)},
                                                  {ImGuiKey_RightArrow, ImVec2(-pan, 0.0F)},
                                                  {ImGuiKey_UpArrow, ImVec2(0.0F, pan)},
                                                  {ImGuiKey_DownArrow, ImVec2(0.0F, -pan)}};
    for (const auto& [key, shift] : arrows) {
        if (ImGui::IsKeyPressed(key)) {
            camera.auto_fit = false;
            camera.offset_x += shift.x;
            camera.offset_y += shift.y;
        }
    }
    if (zoom_steps != 0)
        zoom_map(origin, ImVec2(origin.x + size.x / 2.0F, origin.y + size.y / 2.0F),
                 std::pow(kMapKeyZoomStep, static_cast<float>(zoom_steps)));
    if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))
        camera.auto_fit = true;
}

// `anchor` altındaki yerleşim noktası yerinde kalır.
void View::zoom_map(ImVec2 origin, ImVec2 anchor, float factor) {
    MapCamera& camera = map_camera_;
    const float zoom = std::clamp(camera.zoom * factor, kMapMinZoom, kMapMaxZoom);
    const float world_x = (anchor.x - origin.x - camera.offset_x) / camera.zoom;
    const float world_y = (anchor.y - origin.y - camera.offset_y) / camera.zoom;
    camera.offset_x = anchor.x - origin.x - world_x * zoom;
    camera.offset_y = anchor.y - origin.y - world_y * zoom;
    camera.zoom = zoom;
    camera.auto_fit = false;
}

// Tuval dışındaki kenar ve düğüm çizilmez. Etiketler düğümlerden sonra: çizgilerin üstünde okunur. Ad
// güvenilmez: biçim dizgisine girmez, denetim baytı U+FFFD'dir (map_label).
void View::draw_map_graph(const NoteMap& map, ImVec2 origin, int hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Yazılımsal çizicide maliyet üçgen alanıyla büyür: yumuşatma şeritleri 5.000 kenarda kare bütçesini
    // aşırır. Bayrak yalnız haritanın kendi çizim listesinde ve bu kare için (ImGui her kare stilden yeniden
    // kurar).
    draw->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex |
                     ImDrawListFlags_AntiAliasedFill);
    const auto& positions = state_.map_layout().positions();
    const auto& matches = state_.map_matches();
    const bool filtering = !state_.query().empty();
    const MapCamera& camera = map_camera_;
    const float font = ImGui::GetFontSize();
    const auto screen = [&](std::size_t node) {
        return ImVec2(origin.x + camera.offset_x + positions[node].x * camera.zoom,
                      origin.y + camera.offset_y + positions[node].y * camera.zoom);
    };
    const auto lit = [&](std::size_t node) { return !filtering || matches[node] != 0; };
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float spacing = GraphLayout::kIdealEdge * camera.zoom;
    const bool all_labels = spacing >= kMapLabelSpacingEms * font;
    const bool arrows = spacing >= kMapArrowSpacingEms * font;
    const float radius = std::max(kMapNodeWorldRadius * camera.zoom, kMapNodeMinRadiusEms * font);
    const auto node_radius = [&](std::size_t node) {
        return map.nodes[node].kind == MapNodeKind::Tag ? radius * kMapTagRadiusShare : radius;
    };

    // Kenar türü başına (MapEdgeKind sırası) iki toplu çizgi: aramada eşleşen ve soluk.
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const std::array<ImU32, 3> kind_colors = {
        with_alpha(text, kMapEdgeAlpha), with_alpha(icon_color(core::IconColor::Magenta), kMapRelatedAlpha),
        ImGui::GetColorU32(ImGuiCol_TextDisabled, kMapEdgeAlpha)};
    map_lines_.resize(2 * kind_colors.size());
    for (std::size_t kind = 0; kind < kind_colors.size(); ++kind) {
        map_lines_[2 * kind].color = kind_colors[kind];
        map_lines_[2 * kind + 1].color = with_alpha(kind_colors[kind], kMapDimAlpha);
    }
    for (MapLineBatch& batch : map_lines_)
        batch.coords.clear();  // kapasite kareler arası kalır
    const auto batch_of = [&](const MapEdge& edge) -> MapLineBatch& {
        return map_lines_[2 * static_cast<std::size_t>(edge.kind) + (lit(edge.from) && lit(edge.to) ? 0 : 1)];
    };
    const auto visible = [&](ImVec2 a, ImVec2 b) {
        return std::max(a.x, b.x) >= clip_min.x && std::min(a.x, b.x) <= clip_max.x &&
               std::max(a.y, b.y) >= clip_min.y && std::min(a.y, b.y) <= clip_max.y;
    };
    for (const MapEdge& edge : map.edges) {
        const ImVec2 a = screen(edge.from);
        const ImVec2 b = screen(edge.to);
        if (visible(a, b))
            batch_of(edge).coords.insert(batch_of(edge).coords.end(), {a.x, a.y, b.x, b.y});
    }
    draw->AddCallback(draw_map_lines, &map_lines_);  // düğüm ve oklardan önce: onlar çizgilerin üstünde
    for (const MapEdge& edge : map.edges) {          // ok yalnız yakınken: görünen kenar az
        if (!arrows || edge.kind != MapEdgeKind::Link)
            continue;
        const ImVec2 a = screen(edge.from);
        const ImVec2 b = screen(edge.to);
        if (visible(a, b))
            draw_arrow(draw, a, b, node_radius(edge.to), batch_of(edge).color);
    }

    const auto current = state_.current_path();
    const std::string current_key = current ? current->string() : std::string{};
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    std::vector<std::size_t> labelled;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        const ImVec2 at = screen(i);
        const float r = node_radius(i);
        if (at.x + r < clip_min.x || at.x - r > clip_max.x || at.y + r < clip_min.y || at.y - r > clip_max.y)
            continue;
        const bool tag = map.nodes[i].kind == MapNodeKind::Tag;
        ImU32 color = tag ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : icon_color(map_cache_.colors[i]);
        if (!lit(i))
            color = with_alpha(color, kMapDimAlpha);
        if (tag)
            draw->AddRectFilled(ImVec2(at.x - r, at.y - r), ImVec2(at.x + r, at.y + r), color);
        else
            draw->AddCircleFilled(at, r, color, kMapNodeSegments);
        const bool is_current = !tag && map.nodes[i].key == current_key;
        if (is_current)
            draw->AddCircle(at, r + kMapRingGapEms * font, accent, 0, kMapRingThickness * ui_scale_);
        item_rects_[map_cache_.names[i]] = {at.x - r, at.y - r, at.x + r, at.y + r};
        // Sıkışık görünümde yalnız geçerli ve üstündeki not adlanır: aramada eşleşenler renkle seçilir.
        const bool priority = is_current || static_cast<int>(i) == hovered;
        if (priority)
            labelled.insert(labelled.begin(), i);
        else if (all_labels)
            labelled.push_back(i);
    }
    // Önce geçerli ve üstündeki not; başka bir adın üstüne binecek ad çizilmez (ipucunda, yakınlaşınca
    // görünür). ponytail: çizilen adlarla doğrusal karşılaştırma; tuvale sığan ad sayısı aralıkla sınırlı
    // (~yüz).
    const ImU32 label_background = ImGui::GetColorU32(ImGuiCol_WindowBg, kMapLabelBackgroundAlpha);
    const float max_width =
        std::clamp(spacing * kMapLabelSpacingShare, kMapLabelMinEms * font, kMapLabelMaxEms * font);
    std::vector<ImVec4> placed;  // (min x, min y, max x, max y)
    for (const std::size_t i : labelled) {
        const std::string& full = map_cache_.labels[i];
        const std::string label = text_width(full) > max_width ? keep_start(full, max_width) : full;
        const float width = text_width(label);
        const ImVec2 at = screen(i);
        // Kenardaki düğümün adı tuvalin içine kaydırılır, kırpılmaz.
        const float left =
            std::clamp(at.x - width / 2.0F, clip_min.x, std::max(clip_min.x, clip_max.x - width));
        const float top = at.y + node_radius(i) + kMapRingGapEms * font;
        const ImVec4 box(left, top, left + width, top + font);
        if (std::ranges::any_of(placed, [&](const ImVec4& other) {
                return box.x < other.z && other.x < box.z && box.y < other.w && other.y < box.w;
            }))
            continue;
        placed.push_back(box);
        draw->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.z, box.w), label_background);
        draw->AddText(ImVec2(box.x, box.y), lit(i) ? text : with_alpha(text, kMapDimAlpha), label.c_str());
    }
    if (hovered >= 0 && ImGui::IsItemHovered()) {
        const MapNode& node = map.nodes[static_cast<std::size_t>(hovered)];
        ImGui::SetTooltip("%s", printable(node.kind == MapNodeKind::Tag ? "#" + node.key : node.key).c_str());
    }
}

void View::draw_right_pane() {
    ImGui::BeginGroup();
    draw_note_tabs();
    if (state_.selected_folder()) {  // klasör satırı: düzenlenecek not yok, özet
        draw_folder_summary();
        ImGui::EndGroup();
        return;
    }
    draw_header_row();
    const float available = ImGui::GetContentRegionAvail().y;
    const float links_height = panel_height(state_.link_lines().size(), state_.links_open(), available);
    const InfoColumns info = info_columns();
    const std::size_t info_rows = (state_.note_info().size() + info.pairs - 1) / info.pairs;
    const float info_height = panel_height(info_rows, state_.info_open(), available - links_height);
    if (state_.showing_preview())
        draw_preview(links_height + info_height);
    else
        draw_editor(links_height + info_height);
    if (info_height > 0.0F) {
        // Son panel kalan alanı alır; bağlantılar altta duruyorsa bilgi listesi kendi payıyla sınırlanır
        // (başlık ve iki öğe aralığı payın içinde).
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        draw_info_panel(info,
                        links_height > 0.0F ? info_height - ImGui::GetFrameHeightWithSpacing() - spacing : 0.0F);
    }
    if (links_height > 0.0F)
        draw_links_panel();
    ImGui::EndGroup();
}

void View::queue_open(const core::NotePath& path, bool new_tab) {
    if (new_tab)
        queue([this, path] { state_.open_in_new_tab(path); });
    else
        queue([this, path] { state_.open_note(path); });
}

// Ctrl+Enter (liste, arama kutusu): seçili not yeni sekmede; zaten gösteriliyorsa sekmesi korunur.
void View::queue_open_selected_in_new_tab() {
    queue([this] {
        if (const auto path = state_.current_path()) {
            state_.open_in_new_tab(*path);
            state_.request_focus(FocusRequest::Editor);
        }
    });
}

// Seçimi ImGui değil model verir: istek reddedilirse (kayıt olmadı, vazgeçildi) ImGui başka sekmede kalıp her
// karede isteği yinelemesin; sonraki kare modelin etkin sekmesini yeniden seçtirir.
void View::request_note_tab(std::function<void()> intent) {
    note_tab_pending_ = true;
    queue(std::move(intent));
}

// Editör sekmeleri (SPEC-gui "Sekmeler"). İçerik sekme öğesinin içinde değil altında çizilir: etkin sekme
// GuiState'tedir; ImGui'nin seçimi programatik geçişte bir kare gecikse de editör doğru notu gösterir.
void View::draw_note_tabs() {
    const std::vector<TabSummary> tabs = state_.tabs();
    // Bileşikçi en küçük pencereyi uygulamayabilir: genişliği olmayan bölmede çubuk çizilmez.
    if (tabs.empty() || ImGui::GetContentRegionAvail().x <= 0.0F)
        return;
    constexpr ImGuiTabBarFlags kFlags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // kısayolları var; Tab sırası uzamasın
    if (!ImGui::BeginTabBar("##note_tabs", kFlags)) {
        ImGui::PopItemFlag();
        return;
    }
    const std::uint64_t active = state_.active_tab();
    if (active != drawn_note_tab_)
        note_tab_pending_ = true;
    std::vector<std::pair<ImGuiID, std::uint64_t>> items;
    items.reserve(tabs.size());
    ImGuiID rescroll_id = 0;
    for (const TabSummary& tab : tabs) {
        const std::string label = printable(tab.title) + "###note_tab" + std::to_string(tab.id);
        const ImGuiID item_id = ImGui::GetID(label.c_str());  // BeginTabItem'ın hesapladığı kimlik
        items.emplace_back(item_id, tab.id);
        // Kapanış kararı GuiState'te (kaydedilmemiş iş sorulur): ImGui sekmeyi kendiliğinden düşürmesin.
        ImGuiTabItemFlags flags = ImGuiTabItemFlags_NoAssumedClosure | ImGuiTabItemFlags_NoTooltip;
        if (tab.unsaved)
            flags |= ImGuiTabItemFlags_UnsavedDocument;
        if (note_tab_pending_ && tab.id == active)
            flags |= ImGuiTabItemFlags_SetSelected;
        const bool replaceable = !tab.pinned && !tab.kept;  // sonra açılan not bunun yerine geçer: eğik yazı
        if (replaceable)
            ImGui::PushFont(platform_->preview_fonts.italic, 0.0F);
        bool open = true;
        const bool selected = ImGui::BeginTabItem(label.c_str(), tab.pinned ? nullptr : &open, flags);
        if (replaceable)
            ImGui::PopFont();
        remember_item_center("note_tab:" + std::to_string(tab.id));
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        std::string tip = tab.path.empty() ? tab.title : printable(tab.path);
        if (tab.pinned)
            tip += "  ·  " + core::tr("gui.tab_pinned");
        ImGui::SetItemTooltip("%s\n%s", tip.c_str(), core::tr("gui.tab_hint").c_str());
        if (ImGui::BeginPopupContextItem()) {  // kimlik sekmenin kendisi: sekme başına ayrı menü
            if (ImGui::MenuItem(core::tr(tab.pinned ? "gui.tab_unpin" : "gui.tab_pin").c_str()))
                queue([this, id = tab.id, pinned = !tab.pinned] { state_.set_tab_pinned(id, pinned); });
            if (ImGui::MenuItem(core::tr("gui.tab_close").c_str(), "Ctrl+W"))
                queue([this, id = tab.id] { state_.close_tab(id); });
            ImGui::EndPopup();
        }
        if (selected) {
            if (tab.id == active) {
                if (std::exchange(note_tab_pending_, false))
                    rescroll_id = item_id;
                drawn_note_tab_ = active;
            } else if (!note_tab_pending_) {  // kullanıcı ImGui'de başka sekme seçti
                request_note_tab([this, id = tab.id] { state_.activate_tab(id); });
            }
            ImGui::EndTabItem();
        }
        // Etkin sekmeye tıklama da sayılır: klasör seçiliyken notu geri getirir, odak editöre.
        if (clicked)
            request_note_tab([this, id = tab.id] { state_.activate_tab(id); });
        if (!open)  // ImGui kapatılmak istenen sekmeyi seçer (NoAssumedClosure)
            request_note_tab([this, id = tab.id] { state_.close_tab(id); });
    }
    // Sürükleyerek sıralama ImGui'nin içinde kalır: sıra modele geri yazılır (Ctrl+Tab ve gui-state onu
    // izler).
    ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
    // GEÇİCİ: ImGui 1.93 seçilen sekmeye kaydırmayı, kaydırma okları aynı karede belirince önceki yerleşimin
    // genişliğiyle hesaplar; yeni sekme okların altında yarım kalır. Seçim oturduktan bir kare sonra kaydırma
    // yeniden istenir.
    // BEDEL: iç alana (ImGuiTabBar::NextScrollToTabId) bağlı; ImGui yükseltmesinde alan değişirse derleme kırılır.
    // NE ZAMAN KALDIRILIR: ImGui TabBarLayout kaydırma oklarını güncel WidthAllTabs ile açtığında; bu satırlar
    // silinince "the editor tab bar fits above the editor..." testi yeşil kalıyorsa.
    if (rescroll_id != 0)
        bar->NextScrollToTabId = rescroll_id;
    std::vector<std::pair<int, std::uint64_t>> order;
    for (const auto& [item_id, tab_id] : items) {
        if (ImGuiTabItem* item = ImGui::TabBarFindTabByID(bar, item_id))
            order.emplace_back(ImGui::TabBarGetTabOrder(bar, item), tab_id);
    }
    std::ranges::sort(order);
    const bool reordered =
        order.size() == tabs.size() &&
        !std::ranges::equal(order, tabs, {}, &std::pair<int, std::uint64_t>::second, &TabSummary::id);
    if (reordered) {
        std::vector<std::uint64_t> ids;
        for (const auto& item : order)
            ids.push_back(item.second);
        queue([this, ids = std::move(ids)] { state_.reorder_tabs(ids); });
    }
    item_rects_["note_tabs"] = {bar->BarRect.Min.x, bar->BarRect.Min.y, bar->BarRect.Max.x,
                                bar->BarRect.Max.y, false};
    ImGui::EndTabBar();
    ImGui::PopItemFlag();
}

// Editör payı önce: panel içeriği ne kadar uzun olursa olsun yükseklik sınırlıdır, kalanı kaydırılır.
float View::panel_height(std::size_t lines, bool open, float available_height) const {
    const float room = available_height * kPanelMaxShare;
    const float header = ImGui::GetFrameHeightWithSpacing();
    // Bileşikçi en küçük pencereyi uygulamayabilir: sığmayan panel çizilmez, başlık negatif genişlik almaz.
    if (lines == 0 || ImGui::GetContentRegionAvail().x <= 0.0F || room < header)
        return 0.0F;
    if (!open)
        return header;
    const float rows = static_cast<float>(std::min(lines, kPanelMaxRows));
    const float list =
        (rows * ImGui::GetTextLineHeightWithSpacing()) + (2.0F * ImGui::GetStyle().WindowPadding.y);
    return std::min(header + list, room);
}

// Alanlar (etiket, değer) çiftleri sığarsa yan yana ikişer: yedi alan dört satıra iner, editöre yer kalır.
View::InfoColumns View::info_columns() const {
    InfoColumns columns;
    for (const InfoField& field : state_.note_info()) {
        columns.label = std::max(columns.label, text_width(field.label));
        columns.value = std::max(columns.value, text_width(printable(field.value)));
    }
    columns.value += ImGui::GetFontSize() * kInfoPairGapEms;  // çiftler arası boşluk
    const float cell_padding = 2.0F * ImGui::GetStyle().CellPadding.x;
    const float pair = columns.label + columns.value + (2.0F * cell_padding);
    columns.pairs = ImGui::GetContentRegionAvail().x >= 2.0F * pair ? 2 : 1;
    return columns;
}

// list_height 0: panel en alttadır, kalan alanın tümünü alır.
void View::draw_info_panel(const InfoColumns& columns, float list_height) {
    const std::string label = core::tr("gui.info_title") + "###info";
    ImGui::SetNextItemOpen(state_.info_open());
    const bool open = ImGui::CollapsingHeader(label.c_str());
    ImGui::SetItemTooltip("%s", core::tr("gui.info_hint").c_str());
    remember_item_center("info_header");
    if (open != state_.info_open())
        queue([this, open] { state_.set_info_open(open); });
    if (!open)
        return;
    const float height = list_height > 0.0F ? std::max(list_height, ImGui::GetTextLineHeightWithSpacing())
                                            : ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##info_list", ImVec2(0.0F, height));
    if (ImGui::BeginTable("##info", static_cast<int>(2 * columns.pairs), ImGuiTableFlags_SizingFixedFit)) {
        for (std::size_t pair = 0; pair < columns.pairs; ++pair) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, columns.label);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthFixed, columns.value);
        }
        const auto& fields = state_.note_info();
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto column = static_cast<int>(2 * (index % columns.pairs));
            if (column == 0)
                ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(column);
            ImGui::TextDisabled("%s", fields[index].label.c_str());
            ImGui::TableSetColumnIndex(column + 1);
            ImGui::TextUnformatted(printable(fields[index].value).c_str());  // kaynak cihaz adı senkrondan gelir
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    remember_item_center("info_list");
}

void View::draw_links_panel() {
    const std::size_t items =
        state_.link_lines().size() -
        static_cast<std::size_t>(std::ranges::count_if(state_.link_lines(), &core::LinkLine::is_header));
    const std::string label = core::tr("gui.links_title", {{"count", std::to_string(items)}}) + "###links";
    ImGui::SetNextItemOpen(state_.links_open());
    const bool open = ImGui::CollapsingHeader(label.c_str());
    ImGui::SetItemTooltip("%s", core::tr("gui.links_hint").c_str());
    remember_item_center("links_header");
    if (open != state_.links_open())
        queue([this, open] { state_.set_links_open(open); });
    if (!open)
        return;
    ImGui::BeginChild("##links_list", ImVec2(0.0F, ImGui::GetContentRegionAvail().y));
    ImGuiListClipper clipper;  // binlerce bağlantıda yalnız görünenler çizilir
    clipper.Begin(static_cast<int>(state_.link_lines().size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            draw_link_row(state_.link_lines()[static_cast<std::size_t>(row)]);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    remember_item_center("links_list");
}

void View::draw_link_row(const core::LinkLine& line) {
    if (line.is_header) {
        const std::string title =
            core::tr(core::link_group_key(line.group), {{"count", std::to_string(line.index)}});
        ImGui::TextUnformatted(title.c_str());
        return;
    }
    const std::string raw = core::link_text(state_.links(), line);
    const std::string text = printable(raw);
    if (const core::NotePath* path = core::link_path(state_.links(), line)) {
        const bool clicked = plain_selectable(text, false, {}, state_.icon_style());
        if (new_tab_click(clicked))
            queue_open(*path, true);
        else if (clicked)
            queue([this, target = *path] { state_.open_link(target); });
        remember_item_center("link:" + raw);
        return;
    }
    // Çözülmemiş metin: tıklanacak not yok. Köşeli ayraçlar renksiz de ayırt ettirir.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    draw_fitted_text("link_missing:" + raw, "[[" + text + "]]", ImGui::GetContentRegionAvail().x, keep_start);
    ImGui::PopStyleColor();
}

void View::draw_header_row() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float combo_width = state_.type_enabled() ? ImGui::GetFontSize() * 8.0F : 0.0F;
    const bool previewable = state_.previewable_type();
    const std::string toggle_label = core::tr(state_.showing_preview() ? "gui.edit" : "gui.preview");
    const float toggle_width = previewable ? button_width(toggle_label) + spacing : 0.0F;
    const float trailing = toggle_width + (combo_width > 0.0F ? combo_width + spacing : 0.0F);
    // Gizli karakter uyarısı yoldan önce yer alır: yol kısalır, uyarı kısalmaz.
    const std::string hidden_badge = state_.hidden_controls_label();
    const float badge_width = hidden_badge.empty() ? 0.0F : text_width(hidden_badge) + spacing;
    const float header_width = ImGui::GetContentRegionAvail().x - trailing - badge_width;
    ImGui::AlignTextToFramePadding();
    draw_fitted_text("header", state_.header(), header_width, keep_end);
    if (!hidden_badge.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        draw_fitted_text("hidden_controls", hidden_badge, badge_width - spacing, keep_start);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", core::tr("gui.hidden_controls_tip").c_str());
    }
    if (!state_.type_enabled())
        return;
    const std::string current = state_.current_type();
    // Grup içinde SameLine'ın konumu pencereye göre değil grup başına göre kayar: sağa yaslama kalan alandan.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0F, ImGui::GetContentRegionAvail().x - (trailing - spacing)));
    if (previewable) {
        // Önizlenemeyen md (çok büyük ya da metin değil) düğmeyi kapalı gösterir, nedeni ipucunda.
        const bool can = state_.can_preview();
        const std::string tip =
            !can ? core::tr("gui.preview_unavailable", {{"size", core::human_size(GuiState::kMaxPreviewBytes)}})
                 : core::tr(state_.showing_preview() ? "gui.edit_tip" : "gui.preview_tip");
        ImGui::BeginDisabled(!can);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // kısayolu var; Tab sırası uzamasın
        if (button("preview_toggle", toggle_label))
            queue([this] { state_.toggle_preview(); });
        ImGui::PopItemFlag();
        ImGui::SetItemTooltip("%s", tip.c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
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

void View::draw_editor(float reserved_height) {
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
    // Sekme başına kimlik: ImGui editörün kaydırmasını kimlikle tutar, sekmeye dönünce yerinde bulunur.
    ImGui::PushID(static_cast<int>(state_.active_tab()));
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
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= reserved_height;
    const bool edited =
        ImGui::InputTextMultiline("##editor", &state_.buffer_for_edit(), size, flags, callback, &selection);
    remember_item_center("editor");
    ImGui::PopFont();
    editor_active_ = ImGui::IsItemActive();
    if (const ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetItemID())) {
        int start = input->GetCursorPos();
        int end = start;
        if (input->HasSelection()) {
            start = std::min(input->GetSelectionStart(), input->GetSelectionEnd());
            end = std::max(input->GetSelectionStart(), input->GetSelectionEnd());
        }
        state_.remember_cursor(TextSelection{static_cast<std::size_t>(start), static_cast<std::size_t>(end)});
    }
    if (state_.editor_selection() && !selection)  // geri çağrı uyguladı
        state_.take_editor_selection();
    if (edited)
        state_.buffer_edited();
    ImGui::PopID();
    ImGui::PopID();
}

void View::draw_preview(float reserved_height) {
    editor_active_ = false;
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= reserved_height;
    // Kaydırma sekme başına: ImGui onu alt pencerenin kimliğiyle tutar.
    const std::string child = "##preview" + std::to_string(state_.active_tab());
    ImGui::BeginChild(child.c_str(), size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    PreviewCache& cache = *preview_;
    if (cache.source != state_.buffer()) {
        cache.source = state_.buffer();
        cache.blocks = pdf::parse_markdown(cache.source, {.links = true});
        cache.width = -1.0F;
    }
    if (cache.missing != state_.links().missing) {
        cache.missing = state_.links().missing;
        cache.width = -1.0F;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float font_size = ImGui::GetFontSize();
    if (width != cache.width || font_size != cache.font_size) {
        cache.layout = layout_preview(cache.blocks, platform_->preview_fonts, font_size, width,
                                      [this](std::string_view target) { return state_.resolves_link(target); });
        cache.width = width;
        cache.font_size = font_size;
    }
    const PreviewLayout& layout = cache.layout;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float top = ImGui::GetScrollY();
    const float bottom = top + ImGui::GetWindowHeight();
    draw_preview_layout(layout, origin, top, bottom);
    for (std::size_t index = 0; index < layout.hits.size(); ++index) {
        const PreviewHit& hit = layout.hits[index];
        if (hit.max.y < top || hit.min.y > bottom)
            continue;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + hit.min.x, origin.y + hit.min.y));
        ImGui::PushID(static_cast<int>(index));
        const bool clicked = ImGui::InvisibleButton("##link", ImVec2(hit.max.x - hit.min.x, hit.max.y - hit.min.y));
        const bool new_tab = new_tab_click(clicked);
        ImGui::PopID();
        const std::string target(hit.target);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(hit.wiki ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
            if (!hit.wiki)  // dış adres açılmaz (not güvenilmez): yalnız gösterilir
                ImGui::SetTooltip("%s", printable(target).c_str());
        }
        if (!hit.wiki)
            continue;
        remember_item_center("preview_link:" + target);
        if (clicked || new_tab)
            queue([this, target, new_tab] { state_.open_wiki_link(target, new_tab); });
    }
    for (std::size_t index = 0; index < layout.block_spans.size(); ++index) {  // test kancası
        const auto [block_top, block_bottom] = layout.block_spans[index];
        if (block_bottom >= top && block_top <= bottom)
            item_rects_["preview_block:" + std::to_string(index)] = {origin.x, origin.y + block_top, origin.x + width,
                                                                    origin.y + block_bottom, false};
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(layout.size);
    ImGui::EndChild();
    remember_item_center("preview");
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
            auto userdata = std::make_unique<std::shared_ptr<NativeDialogResult>>(native);
            SDL_Window* window = platform_->window.get();
            // release: sahiplik SDL üzerinden native_dialog_done'a geçer, geri çağrı her durumda çalışır.
            if (kind == PathKind::Folder)
                SDL_ShowOpenFolderDialog(native_dialog_done, userdata.release(), window, path.c_str(), false);
            else
                SDL_ShowSaveFileDialog(native_dialog_done, userdata.release(), window, kPdfFilters, 1,
                                       path.c_str());
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
