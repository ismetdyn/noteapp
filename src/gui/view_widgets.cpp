#include "core/i18n.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::gui {

namespace {

constexpr float kDialogTextEms = 32.0F;
constexpr const char* kEllipsis = "…";
// Dolu (birincil, yıkıcı) düğmenin üzerinde ve basılıyken parlaklığı (tasarım §3.4: ±%8).
constexpr float kFillHoverScale = 1.08F;
constexpr float kFillActiveScale = 0.92F;

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
    const ImU32 accent = theme::current().accent;
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

std::size_t utf8_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    if (byte >= 0xF0)
        return 4;
    if (byte >= 0xE0)
        return 3;
    return byte >= 0xC0 ? 2 : 1;
}

void draw_error(const std::string& message) {
    if (message.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::current().danger);
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

void align_right(float width, float room) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(room - width, 0.0F));
}

// Sütuna sarılan soluk açıklama satırı, ikincil boyda.
void draw_hint(const std::string& text) {
    ImGui::PushFont(nullptr, kSmallFontPixels);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
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

// Aile tablosu TUI ile ortak (tui::syntax_family); tonlar temanın, tuvalde ≥ 4,5. Vurgu tek fontta metin
// rengi.
ImU32 token_color(tui::TokenKind kind) {
    const theme::Theme& colors = theme::current();
    switch (tui::syntax_family(kind)) {
    case tui::SyntaxFamily::Plain:
    case tui::SyntaxFamily::Bold:
        break;
    case tui::SyntaxFamily::Muted:
        return colors.comment;
    case tui::SyntaxFamily::Green:
        return colors.string;
    case tui::SyntaxFamily::Orange:
        return colors.number;
    case tui::SyntaxFamily::Cyan:
        return colors.type;
    case tui::SyntaxFamily::Magenta:
        return colors.keyword;
    case tui::SyntaxFamily::Blue:
        return colors.heading;
    }
    return colors.text;
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

// İkon aileleri sözdizimi ailelerinin tonlarıdır (sarı = warning, kırmızı = danger): temaya göre okunur.
ImU32 icon_color(core::IconColor color) {
    const theme::Theme& colors = theme::current();
    switch (color) {
    case core::IconColor::Blue:
        return colors.heading;
    case core::IconColor::Cyan:
        return colors.type;
    case core::IconColor::Green:
        return colors.string;
    case core::IconColor::Yellow:
        return colors.warning;
    case core::IconColor::Red:
        return colors.danger;
    case core::IconColor::Magenta:
        return colors.keyword;
    case core::IconColor::Default:
        break;
    }
    return colors.text;
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
    const ImU32 bar = theme::current().warning;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, theme::with_alpha(bar, theme::kMarkFillAlpha));
    draw->AddRectFilled(min, ImVec2(min.x + (ImGui::GetFontSize() * kMarkBarEms), max.y), bar);
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
    const ImVec2 field_min = ImGui::GetItemRectMin();
    ImGui::SameLine(0.0F, 0.0F);
    // BeginCombo listeyi yalnız ok düğmesine bağlar: liste ok kadar dar ve en çok 8 satır olur; satırlar metni
    // ölçü bildirmeden çizdiğinden (plain_selectable) içerik de genişletmez, adlar kesilir. Bu yüzden liste alan +
    // ok kutusuna bağlanır, en uzun ad kadar genişler; yüksekliği yalnız ekranla sınırlıdır (taşarsa kayar).
    const ImGuiID popup_id = ImGui::GetID("##list");
    if (ImGui::ArrowButton("##pick", ImGuiDir_Down) && !ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None))
        ImGui::OpenPopup(popup_id);
    if (ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None)) {  // kapalıyken binlerce ad her karede ölçülmesin
        float longest = root_first ? text_width(hint) : 0.0F;
        for (const std::string& item : items)
            longest = std::max(longest, text_width(item));
        // Yatay iç boşluk FramePadding.x'tir (BeginComboPopup). Kaydırma çubuğu en küçük genişliğin içinden
        // yer alır; payı eklenmezse uzun listede en uzun ad kesilir.
        const ImGuiStyle& style = ImGui::GetStyle();
        // Liste alanın altına ya da üstüne sığacak kadar uzar, fazlası kayar: ikisine de sığmayan liste ImGui'de
        // alanın üstüne biner ve oka yeniden tıklamak onu kapatamazdı.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float below = viewport->WorkPos.y + viewport->WorkSize.y - ImGui::GetItemRectMax().y;
        const float above = field_min.y - viewport->WorkPos.y;
        const float tallest = std::max(below, above) - (2.0F * style.DisplaySafeAreaPadding.y);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(longest + (2.0F * style.FramePadding.x) + style.ScrollbarSize, 0.0F),
            ImVec2(FLT_MAX, std::max(tallest, ImGui::GetFrameHeight())));
    }
    if (ImGui::BeginComboPopup(popup_id, ImRect(field_min, ImGui::GetItemRectMax()), ImGuiComboFlags_None)) {
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

// Çerçevesiz (sınır yalnız girdi ve onay kutusunda). Dolu düğmede üzerine gelince %8 açık, basılıyken %8
// koyu.
bool View::button(const std::string& name, const std::string& label, ButtonKind kind) {
    const std::string id = label + "###" + name;
    const theme::Theme& colors = theme::current();
    int pushed = 0;
    const auto push = [&pushed](ImGuiCol slot, ImU32 color) {
        ImGui::PushStyleColor(slot, color);
        ++pushed;
    };
    const auto fill = [&](ImU32 color) {
        push(ImGuiCol_Button, color);
        push(ImGuiCol_ButtonHovered, theme::scaled(color, kFillHoverScale));
        push(ImGuiCol_ButtonActive, theme::scaled(color, kFillActiveScale));
        push(ImGuiCol_Text, colors.on_fill);
    };
    switch (kind) {
    case ButtonKind::Secondary:
        break;
    case ButtonKind::Ghost:
    case ButtonKind::GhostDanger:
        push(ImGuiCol_Button, 0);
        push(ImGuiCol_ButtonHovered, colors.hover);
        push(ImGuiCol_ButtonActive, colors.control_active);
        // Renk Button'dan önce verilir: üzerinde olup olmadığı geçen karenin sonucudur (fare olayı kare
        // üretir).
        if (kind == ButtonKind::GhostDanger && GImGui->HoveredIdPreviousFrame == ImGui::GetID(id.c_str()))
            push(ImGuiCol_Text, colors.danger);
        break;
    case ButtonKind::Primary:
        fill(colors.accent_fill);
        break;
    case ButtonKind::Danger:
        fill(colors.danger_fill);
        break;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    const bool clicked = ImGui::Button(id.c_str());
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(pushed);
    remember_item_center(name);
    return clicked;
}

// Diyalog altı (G11): Tamam birincil, Vazgeç hayalet, ikisi sağa yaslı; dar diyalogda alt satıra iner.
View::DialogAnswer View::dialog_ok_cancel() {
    const std::string ok = core::tr("gui.ok");
    const std::string cancel = core::tr("gui.cancel");
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float room = dialog_content_width();
    align_right(button_width(ok) + spacing + button_width(cancel), room);
    DialogAnswer answer;
    answer.ok = button("dialog_ok", ok, ButtonKind::Primary);
    flow_after_previous(button_width(cancel), row_start_x, room);
    answer.cancel = button("dialog_cancel", cancel, ButtonKind::Ghost);
    return answer;
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
