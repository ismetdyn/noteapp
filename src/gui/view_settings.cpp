#include "core/i18n.hpp"
#include "core/text.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace noteapp::gui {

namespace {

constexpr float kMinInlineFieldEms = 8.0F;    // daha darsa yanındaki düğmeler alt satıra iner
constexpr float kMaxLabelColumnShare = 0.5F;  // etiket sütunu alanı yok etmesin; uzun etiket sarılır
constexpr const char* kGeneralKeys[] = {"language", "notes_dir", "default_type", "default_folder",
                                        "types",    "icons",     "editor",       "update.check_interval"};
constexpr const char* kSyncKeys[] = {"sync.enabled",      "sync.remote",      "sync.branch",
                                     "sync.idle_seconds", "sync.author_name", "sync.author_email"};
constexpr const char* kLanguages[] = {"auto", "tr", "en"};
constexpr const char* kIconModes[] = {"nerd", "off"};

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

// Alanın yanındaki düğmelere yer bırakır; alan çok daralacaksa satırın tamamını alır, düğmeler alta iner.
float field_width_beside(float trailing_width) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float beside = available - trailing_width - ImGui::GetStyle().ItemSpacing.x;
    return beside >= ImGui::GetFontSize() * kMinInlineFieldEms ? beside : available;
}

float settings_label_width(const SettingsPage& page, float available) {
    float widest =
        std::max({text_width(core::tr("gui.token_label")), text_width(core::tr("gui.setting_row_numbers")),
                  text_width(setting_label("ui_font")), text_width(setting_label("editor_font"))});
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

// Kod saklanır; kullanıcı okunur adını görür. Elle yazılmış bilinmeyen değer olduğu gibi gösterilir.
std::string choice_label(const std::string& key, std::span<const char* const> choices,
                         const std::string& code) {
    return std::ranges::find(choices, code) != choices.end() ? core::tr("gui." + key + "_" + code) : code;
}

}  // namespace

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
    if (key == "editor" || key == "icons" || key == "sync.remote" || key == "sync.idle_seconds" ||
        key == "update.check_interval") {
        draw_hint(setting_hint(key));
        remember_item_center("setting_hint:" + key);
    }
    if (!page.field_error(key).empty()) {
        draw_error(core::terminal_safe(page.field_error(key)));
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

// Görünüm'deki yazı ailesi: sıra no gibi hemen uygulanır, config değildir (gui-state.toml).
void View::draw_font_setting(const char* key, std::span<const char* const> choices,
                             const std::string& current, void (GuiState::*apply)(const std::string&)) {
    begin_setting_row(setting_label(key), std::string("setting_label:") + key);
    ImGui::PushID(key);
    std::string value = current;
    ImGui::SetNextItemWidth(-FLT_MIN);
    draw_choice_combo(key, choices, value);
    if (value != current)
        queue([this, value, apply] { (state_.*apply)(value); });
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
        const std::string state = core::tr(page.token_stored() ? "gui.token_stored" : "app.token_none");
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
    ImGui::TextUnformatted(core::terminal_safe(page.test_result()).c_str());
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
        draw_font_setting("ui_font", kUiFonts, state_.layout().ui_font, &GuiState::set_ui_font);
        draw_font_setting("editor_font", kEditorFonts, state_.layout().editor_font,
                          &GuiState::set_editor_font);
        ImGui::EndTable();
    }
}

// Formun dışında: en küçük pencerede de Geri al ve Kaydet görünür kalır.
void View::draw_settings_footer() {
    const SettingsPage& page = state_.settings();
    const float top = ImGui::GetCursorPosY();
    ImGui::Separator();
    if (!page.global_error().empty()) {
        // Hata dosya yolunu ve toml++ ayrıntısını taşır: denetim karakterleri görünür işaretle çizilir.
        draw_error(core::terminal_safe(page.global_error()));
        remember_item_center("settings_error");
    }
    const bool changed = page.has_changes();
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const float row_width = ImGui::GetContentRegionAvail().x;
    const std::string save_label = core::tr("app.settings_save");
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

// Üst sekme yok (K2): başlık satırında geri düğmesi ve sayfa adı; Ctrl+, de iki yönde geçer.
void View::draw_settings_tab() {
    const float row_y = ImGui::GetCursorPosY();
    const float row_height = ImGui::GetFrameHeight();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // kısayolu var; Tab sırası uzamasın
    if (button("settings_back", "← " + core::tr("gui.tab_notes"), ButtonKind::Ghost))
        queue([this] { state_.leave_settings(); });
    ImGui::PopItemFlag();
    ImGui::SetItemTooltip("%s", core::tr("gui.settings_back_tip").c_str());
    ImGui::SameLine();
    ImGui::PushFont(nullptr, kTitleFontPixels);
    ImGui::SetCursorPosY(row_y + ((row_height - ImGui::GetTextLineHeight()) / 2.0F));
    ImGui::TextUnformatted(core::tr("gui.tab_settings").c_str());
    ImGui::PopFont();
    remember_item_center("settings_title");
    ImGui::BeginChild("##settings_form", ImVec2(0.0F, -settings_footer_height_));
    draw_settings_form();
    ImGui::EndChild();
    remember_item_center("settings_form");
    draw_settings_footer();
}

}  // namespace noteapp::gui
