#include "app/commands.hpp"
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>  // GetKeyChordName
#include <imgui_stdlib.h>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::gui {

namespace {

// Paletin görünen satır sayısı; fazlası kayar.
constexpr float kPaletteRows = 12.0F;
// Palet yerleşimi (G12, ölçek 1 pikseli): 36 em genişlik, pencerenin üstünden %18; 32 px girdi, 26 px satır,
// seçili satırda 2 px çubuk; kısayol çipi kod zemininin yarısı, yarıçap 4.
constexpr float kPaletteWidthEms = 36.0F;
constexpr float kPaletteTopShare = 0.18F;
constexpr float kPaletteInputPixels = 32.0F;
constexpr float kPaletteRowPixels = 26.0F;
constexpr float kPaletteBarPixels = 2.0F;
constexpr float kPaletteChipPadPixels = 4.0F;
constexpr float kPaletteChipAlpha = 0.5F;
constexpr float kPaletteChipRounding = 4.0F;
// Yardım listesi pencerenin bu payını geçmez; kalanı kayar.
constexpr float kHelpHeightShare = 0.7F;

// Komutun GUI'deki tuşları ("Ctrl+K, Ctrl+P"); tuşu yoksa boş.
std::string command_chords(std::string_view id) {
    std::string keys;
    for (const GuiCommand& command : kGuiCommands) {
        if (command.id != id || command.chord == 0)
            continue;
        if (!keys.empty())
            keys += ", ";
        keys += ImGui::GetKeyChordName(command.chord);
    }
    return keys;
}

// Yardımdaki tuş sütunu: tuşsuz bağ paletten (düğme, menü), bağsız komut henüz bu pencerede yok.
std::string command_keys(std::string_view id) {
    if (std::string keys = command_chords(id); !keys.empty())
        return keys;
    const bool bound =
        std::ranges::any_of(kGuiCommands, [id](const GuiCommand& command) { return command.id == id; });
    return core::tr(bound ? "gui.help_no_key" : "gui.help_unbound");
}

struct PaletteKeys {
    std::size_t* selected;
    std::size_t count;
};

// Yukarı/Aşağı alanın geçmiş olayıdır: odak listeye kaçmaz, seçim yürür.
int palette_callback(ImGuiInputTextCallbackData* data) {
    auto* keys = static_cast<PaletteKeys*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && keys->count > 0) {
        std::size_t& selected = *keys->selected;
        if (data->EventKey == ImGuiKey_UpArrow && selected > 0)
            --selected;
        else if (data->EventKey == ImGuiKey_DownArrow && selected + 1 < keys->count)
            ++selected;
    }
    return 0;
}

}  // namespace

// Çalışamayan satır da seçilebilir: GuiState nedenini durum çubuğunda söyler (TUI paleti gibi).
// G12: 36 em genişlik, pencerenin üstünden %18 aşağıda; satırda etiket solda, kısayol sağa yaslı çip; engelli
// satır soluk ve nedeni sağda. Kısayolsuz satırın sağı boştur, nasıl çalıştığı ipucundadır.
std::optional<std::string> View::ask_palette() {
    std::string query;
    std::size_t selected = 0;
    std::optional<std::string> result;
    bool first_frame = true;
    run_modal(
        core::tr("gui.palette_title"),
        [&]() -> bool {
            const std::vector<PaletteRow> rows = state_.palette_rows(query);
            selected = rows.empty() ? 0 : std::min(selected, rows.size() - 1);
            if (first_frame)
                ImGui::SetKeyboardFocusHere();
            first_frame = false;
            const ImGuiStyle& style = ImGui::GetStyle();
            const float scale = style.FontScaleDpi;
            const float width =
                std::min(kPaletteWidthEms * ImGui::GetFontSize(),
                         ImGui::GetMainViewport()->WorkSize.x - (4.0F * style.WindowPadding.x));
            ImGui::SetNextItemWidth(std::max(width, ImGui::GetFontSize() * kMinFieldEms));
            const std::string hint = core::tr("gui.palette_hint");
            PaletteKeys keys{&selected, rows.size()};
            const float input_pad = ((kPaletteInputPixels - kFontPixels) / 2.0F) * scale;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, input_pad));
            const bool entered = ImGui::InputTextWithHint("##palette", hint.c_str(), &query,
                                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                                              ImGuiInputTextFlags_CallbackHistory,
                                                          palette_callback, &keys);
            ImGui::PopStyleVar();
            remember_item_center("palette_input");
            if (ImGui::IsItemEdited())
                selected = 0;
            std::optional<std::size_t> clicked;
            const float row_height = kPaletteRowPixels * scale;
            ImGui::BeginChild("##palette_rows", ImVec2(ImGui::GetItemRectSize().x, row_height * kPaletteRows),
                              theme::kPaneChildFlags);
            for (std::size_t index = 0; index < rows.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                if (draw_palette_row(rows[index], index == selected, row_height))
                    clicked = index;
                if (index == selected && !ImGui::IsWindowAppearing() && !ImGui::IsItemVisible())
                    ImGui::SetScrollHereY();
                remember_item_center("palette:" + std::string(rows[index].id));
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (clicked)
                selected = *clicked;
            if ((entered || clicked) && !rows.empty()) {
                result = std::string(rows[selected].id);
                return true;
            }
            return ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        },
        kPaletteTopShare);
    return result;
}

// Paletin bir satırı; tıklandıysa true. Metin çizim listesine yazılır (komut adı katalogdandır), günlüğe de.
bool View::draw_palette_row(const PaletteRow& row, bool selected, float row_height) {
    const theme::Theme& colors = theme::current();
    // Seçilebilir öğe satır aralığını kendine katar: toplam boy row_height olsun.
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None,
                                           ImVec2(0.0F, row_height - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float scale = ImGui::GetStyle().FontScaleDpi;
    if (selected)
        draw->AddRectFilled(min, ImVec2(min.x + (kPaletteBarPixels * scale), max.y), colors.accent);
    const float pad = ImGui::GetStyle().FramePadding.x;
    const std::string label = core::tr(row.label_key);
    ImGui::LogText("%s\n", label.c_str());
    draw->AddText(ImVec2(min.x + pad, min.y + ((row_height - ImGui::GetFontSize()) / 2.0F)),
                  row.blocked != nullptr ? colors.faint : colors.text, label.c_str());
    // Sağ taraf ikincil boyda: engelliyse neden (soluk metin), değilse kısayol çipi.
    ImGui::PushFont(nullptr, kSmallFontPixels);
    const float small = ImGui::GetFontSize();
    const float text_y = min.y + ((row_height - small) / 2.0F);
    const std::string chords = command_chords(row.id);
    if (row.blocked != nullptr) {
        const std::string reason = core::tr(row.blocked);
        draw->AddText(ImVec2(max.x - pad - text_width(reason), text_y), colors.muted, reason.c_str());
    } else if (!chords.empty()) {
        const float chip_pad = kPaletteChipPadPixels * scale;
        const ImVec2 chip_min(max.x - pad - text_width(chords) - (2.0F * chip_pad), text_y - chip_pad / 2.0F);
        const ImVec2 chip_max(max.x - pad, text_y + small + (chip_pad / 2.0F));
        draw->AddRectFilled(chip_min, chip_max, theme::with_alpha(colors.code_bg, kPaletteChipAlpha),
                            kPaletteChipRounding * scale);
        draw->AddText(ImVec2(chip_min.x + chip_pad, text_y), colors.muted, chords.c_str());
        remember_rect("palette_keys:" + std::string(row.id), chip_min, chip_max);
    }
    ImGui::PopFont();
    if (chords.empty())
        ImGui::SetItemTooltip("%s", command_keys(row.id).c_str());
    return clicked;
}

// Kayıttaki her komut, GUI'deki tuşlarıyla (komut tablosundan üretilir; elle yazılmış liste yok).
void View::show_help_dialog() {
    run_modal(core::tr("gui.help_title"), [&]() -> bool {
        const float width = dialog_content_width();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextUnformatted(core::tr("gui.help_hint").c_str());
        ImGui::PopTextWrapPos();
        const float height = ImGui::GetMainViewport()->WorkSize.y * kHelpHeightShare;
        ImGui::BeginChild("##help_rows", ImVec2(width, height), theme::kPaneChildFlags);
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##help", 2, kFlags)) {
            const auto help_row = [this](const std::string& name, const std::string& label,
                                         const std::string& keys) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(label.c_str());
                remember_item_center("help:" + name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", keys.c_str());
                ImGui::LogText("%s %s\n", label.c_str(),
                               keys.c_str());  // kaydırılıp görünmese de günlükte (testler)
            };
            help_row("palette", core::tr("gui.help_palette"), ImGui::GetKeyChordName(kPaletteChord));
            for (const app::CommandSpec& spec : app::kCommandRegistry)
                help_row(std::string(spec.id), core::tr(spec.label_key), command_keys(spec.id));
            ImGui::EndTable();
        }
        ImGui::EndChild();
        return button("dialog_ok", core::tr("gui.ok")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    });
}

}  // namespace noteapp::gui
