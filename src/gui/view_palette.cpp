#include "app/commands.hpp"
#include "core/i18n.hpp"
#include "gui/commands.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>  // GetKeyChordName
#include <imgui_stdlib.h>
#include <iterator>
#include <string>
#include <vector>

namespace noteapp::gui {

namespace {

// Paletin görünen satır sayısı; fazlası kayar.
constexpr float kPaletteRows = 12.0F;
// Yardım listesi pencerenin bu payını geçmez; kalanı kayar.
constexpr float kHelpHeightShare = 0.7F;

// Komutun GUI'deki tuşları ("Ctrl+K, Ctrl+P"); tuşsuz bağ paletten, bağsız komut henüz bu pencerede yok.
std::string command_keys(std::string_view id) {
    std::string keys;
    bool bound = false;
    for (const GuiCommand& command : kGuiCommands) {
        if (command.id != id)
            continue;
        bound = true;
        if (command.chord == 0)
            continue;
        if (!keys.empty())
            keys += ", ";
        keys += ImGui::GetKeyChordName(command.chord);
    }
    if (!keys.empty())
        return keys;
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
std::optional<std::string> View::ask_palette() {
    std::string query;
    std::size_t selected = 0;
    std::optional<std::string> result;
    bool first_frame = true;
    run_modal(core::tr("gui.palette_title"), [&]() -> bool {
        const std::vector<PaletteRow> rows = state_.palette_rows(query);
        selected = rows.empty() ? 0 : std::min(selected, rows.size() - 1);
        if (first_frame)
            ImGui::SetKeyboardFocusHere();
        first_frame = false;
        const float width = dialog_content_width();
        ImGui::SetNextItemWidth(width);
        const std::string hint = core::tr("gui.palette_hint");
        PaletteKeys keys{&selected, rows.size()};
        const bool entered = ImGui::InputTextWithHint("##palette", hint.c_str(), &query,
                                                      ImGuiInputTextFlags_EnterReturnsTrue |
                                                          ImGuiInputTextFlags_CallbackHistory,
                                                      palette_callback, &keys);
        remember_item_center("palette_input");
        if (ImGui::IsItemEdited())
            selected = 0;
        std::optional<std::size_t> clicked;
        ImGui::BeginChild("##palette_rows",
                          ImVec2(width, ImGui::GetTextLineHeightWithSpacing() * kPaletteRows),
                          ImGuiChildFlags_Borders);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const PaletteRow& row = rows[index];
            std::string label = core::tr(row.label_key) + "  ·  " + command_keys(row.id);
            if (row.blocked != nullptr)
                label += "  (" + core::tr(row.blocked) + ")";
            ImGui::PushID(static_cast<int>(index));
            if (row.blocked != nullptr)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), index == selected))
                clicked = index;
            if (row.blocked != nullptr)
                ImGui::PopStyleColor();
            if (index == selected && ImGui::IsWindowAppearing() == false && !ImGui::IsItemVisible())
                ImGui::SetScrollHereY();
            remember_item_center("palette:" + std::string(row.id));
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
    });
    return result;
}

// Kayıttaki her komut, GUI'deki tuşlarıyla (komut tablosundan üretilir; elle yazılmış liste yok).
void View::show_help_dialog() {
    run_modal(core::tr("gui.help_title"), [&]() -> bool {
        const float width = dialog_content_width();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextUnformatted(core::tr("gui.help_hint").c_str());
        ImGui::PopTextWrapPos();
        const float height = ImGui::GetMainViewport()->WorkSize.y * kHelpHeightShare;
        ImGui::BeginChild("##help_rows", ImVec2(width, height), ImGuiChildFlags_Borders);
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
