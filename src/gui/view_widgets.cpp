#include "core/i18n.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::gui {

namespace {

constexpr float kDialogTextEms = 32.0F;
constexpr const char* kEllipsis = "…";

std::size_t utf8_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    if (byte >= 0xF0)
        return 4;
    if (byte >= 0xE0)
        return 3;
    return byte >= 0xC0 ? 2 : 1;
}

// `width` içine sığan baştaki bayt sayısı; kod noktası ortasında kesmez.
std::size_t fitting_bytes(std::string_view text, float width) {
    const char* end = text.data() + text.size();
    const char* remaining = nullptr;
    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::max(width, 0.0F), 0.0F, text.data(), end,
                                    &remaining);
    return static_cast<std::size_t>(remaining - text.data());
}

// Dosya adları güvenilmez: ImGui etiketlerindeki "##" ve "%" yorumlanmasın diye metin doğrudan çizilir.
// Eşleşen kod noktaları vurgu rengiyle.
void draw_row_text(ImVec2 position, std::string_view text, const std::vector<std::size_t>& highlights,
                   ImU32 normal) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
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

// Kesitteki eşleşme aralıkları [başlangıç, bitiş) baytı: vurgu kod noktası başlarını ister.
std::vector<std::size_t> hit_offsets(const search::Hit& hit) {
    std::vector<std::size_t> offsets;
    for (const auto& [begin, end] : hit.ranges)
        for (std::size_t at = begin; at < std::min(end, hit.snippet.size());
             at += utf8_length(hit.snippet[at]))
            offsets.push_back(at);
    return offsets;
}

}  // namespace

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

float text_width(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

float button_width(const std::string& label) {
    return text_width(label) + 2.0F * ImGui::GetStyle().FramePadding.x;
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

// Sütuna sarılan soluk açıklama satırı.
void draw_hint(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

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

// Sabit tonlar, TUI'nin eşlemesiyle aynı aileler (editor_view token_styled): açık ve koyu zeminde okunur.
ImU32 token_color(tui::TokenKind kind) {
    switch (kind) {
    case tui::TokenKind::Plain:
    case tui::TokenKind::Emphasis:
        break;
    case tui::TokenKind::Comment:
        return IM_COL32(60, 150, 60, 255);
    case tui::TokenKind::String:
    case tui::TokenKind::Value:
        return IM_COL32(176, 82, 176, 255);
    case tui::TokenKind::Number:
    case tui::TokenKind::Constant:
        return IM_COL32(214, 92, 72, 255);
    case tui::TokenKind::Type:
    case tui::TokenKind::Code:
        return IM_COL32(40, 160, 170, 255);
    case tui::TokenKind::Keyword:
    case tui::TokenKind::Key:
    case tui::TokenKind::Tag:
    case tui::TokenKind::Heading:
        return IM_COL32(64, 132, 214, 255);
    }
    return ImGui::GetColorU32(ImGuiCol_Text);
}

void draw_token_line(std::string_view text, const std::vector<tui::SyntaxSpan>& spans) {
    const auto code_point_start = [&](std::size_t at) {
        constexpr unsigned char kContinuationMask = 0xC0;
        constexpr unsigned char kContinuation = 0x80;
        while (at < text.size() &&
               (static_cast<unsigned char>(text[at]) & kContinuationMask) == kContinuation)
            ++at;
        return std::min(at, text.size());
    };
    std::size_t at = 0;
    bool first = true;
    const auto segment = [&](std::size_t end, ImU32 color) {
        end = code_point_start(end);
        if (end <= at)
            return;
        if (!first)
            ImGui::SameLine(0.0F, 0.0F);
        first = false;
        const std::string shown = printable(text.substr(at, end - at));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
        ImGui::PopStyleColor();
        at = end;
    };
    const ImU32 plain = ImGui::GetColorU32(ImGuiCol_Text);
    for (const tui::SyntaxSpan& span : spans) {
        segment(span.begin, plain);
        segment(span.end, token_color(span.kind));
    }
    segment(text.size(), plain);
    if (first)
        ImGui::TextUnformatted("");
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
                          const std::vector<std::size_t>& highlights, ImU32 color) {
    ImGui::LogText("%s\n",
                   text.c_str());  // çizim listesine yazılan metin ImGui günlüğüne (testler) de girsin
    if (text_width(text) <= width) {
        draw_row_text(position, text, highlights, color);
        return false;
    }
    const std::string_view visible =
        std::string_view(text).substr(0, fitting_bytes(text, width - text_width(kEllipsis)));
    draw_row_text(position, visible, highlights, color);
    ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + text_width(visible), position.y), color,
                                        kEllipsis);
    return true;
}

// `best_line` verilirse satır iki satır boyundadır, ikincisi notun en iyi eşleşen satırı (genel arama, TUI
// gibi); yalnız adı eşleşen satır tek satırdır.
bool plain_selectable(const std::string& text, bool selected, const std::vector<std::size_t>& highlights,
                      core::IconStyle icon_style, const search::Hit* best_line, bool marked) {
    ImVec2 position = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    const bool clicked = ImGui::Selectable(
        "##row", selected, ImGuiSelectableFlags_None,
        ImVec2(0.0F, best_line != nullptr ? line_height + ImGui::GetTextLineHeight() : 0.0F));
    if (marked)
        draw_mark_background();
    const float icon_width = draw_icon(position, core::file_icon(text), icon_style, selected);
    position.x += icon_width;
    width -= icon_width;
    if (draw_fitted_row_text(position, text, width, highlights))
        full_text_tooltip(text);
    if (best_line != nullptr)
        draw_fitted_row_text(ImVec2(position.x, position.y + line_height), best_line->snippet, width,
                             hit_offsets(*best_line), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    return clicked;
}

// Çoklu seçimdeki satır: kehribar zemin ve solda şerit; metin üstüne çizilir. Temanın mavi seçim vurgusundan
// ayrı renk: imleç satırı ile işaretli satır aynı satırda da ayırt edilir, açık ve koyu temada okunur.
void draw_mark_background() {
    constexpr ImU32 kMarkFill = IM_COL32(230, 160, 40, 80);
    constexpr ImU32 kMarkBar = IM_COL32(220, 140, 20, 255);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, kMarkFill);
    draw->AddRectFilled(min, ImVec2(min.x + (ImGui::GetFontSize() * kMarkBarEms), max.y), kMarkBar);
}

// Editable açılır kutu (ImGui'de yok): serbest metin alanı + seçim listesi. Alanda Enter basıldıysa true.
bool combo_field(const char* id, const std::string& label, std::string& value,
                 const std::vector<std::string>& items, bool root_first, const char* hint) {
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

// Son öğeye orta tık ya da Ctrl+tık: not yeni sekmede açılır (SPEC-gui "Sekmeler").
bool new_tab_click(bool clicked) {
    return (clicked && ImGui::GetIO().KeyCtrl) || ImGui::IsItemClicked(ImGuiMouseButton_Middle);
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
    remember_rect(name, min, max, disabled);
}

void View::remember_rect(const std::string& name, ImVec2 min, ImVec2 max, bool disabled) {
    item_rects_.insert_or_assign(name, ItemRect{min.x, min.y, max.x, max.y, disabled});
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

}  // namespace noteapp::gui
