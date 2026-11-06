#pragma once

#include "app/commands.hpp"
#include "gui/gui_state.hpp"

#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <string_view>

namespace noteapp::gui {

// Bağın dinlendiği yer: Global her karede (diyalog yokken), List yalnız liste odaktayken, Preview yalnız
// önizleme odaktayken, Widget düğmede, bağlam menüsünde ya da palette.
enum class CommandScope { Global, List, Preview, Widget };

// GUI'nin komut tablosu: kısayol, araç çubuğu ve çöp düğmeleri eylemini kimlikle buradan alır; yardım ve
// palet her komutu bu arayüzdeki bağıyla buradan listeler. Aynı kimlik birden çok satırda olabilir (iki
// kısayol).
struct GuiCommand {
    std::string_view id;  // app::kCommandRegistry
    ImGuiKeyChord chord;  // 0: tuşsuz, yalnız düğme/palet
    CommandScope scope;
    void (*run)(GuiState&);
    bool repeats = false;  // basılı tutulunca yinelenir (gezinme)
};

// Palet kayıttaki bir komut değildir (TUI'de `:`): bağı burada, yardım da bunu gösterir.
inline constexpr ImGuiKeyChord kPaletteChord = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P;
inline constexpr int kBigScrollLines = 5;  // TUI REVIEW `J` `K`

inline constexpr GuiCommand kGuiCommands[] = {
    {"search", ImGuiMod_Ctrl | ImGuiKey_K, CommandScope::Global, [](GuiState& s) { s.focus_search(); }},
    {"search", ImGuiMod_Ctrl | ImGuiKey_P, CommandScope::Global, [](GuiState& s) { s.focus_search(); }},
    {"new", ImGuiMod_Ctrl | ImGuiKey_N, CommandScope::Global, [](GuiState& s) { s.new_note(); }},
    {"save", ImGuiMod_Ctrl | ImGuiKey_S, CommandScope::Global, [](GuiState& s) { s.save_now(); }},
    {"move.dialog", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, CommandScope::Global,
     [](GuiState& s) { s.save_as(); }},
    {"move", ImGuiMod_Ctrl | ImGuiKey_M, CommandScope::Global, [](GuiState& s) { s.move_selected(); }},
    {"settings", ImGuiMod_Ctrl | ImGuiKey_Comma, CommandScope::Global,
     [](GuiState& s) { s.toggle_settings_tab(); }},
    {"export.pdf", ImGuiMod_Ctrl | ImGuiKey_E, CommandScope::Global, [](GuiState& s) { s.export_pdf(); }},
    {"links.toggle", ImGuiMod_Ctrl | ImGuiKey_L, CommandScope::Global, [](GuiState& s) { s.toggle_links(); }},
    {"details", ImGuiMod_Ctrl | ImGuiKey_I, CommandScope::Global, [](GuiState& s) { s.toggle_info(); }},
    {"view.toggle", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, CommandScope::Global,
     [](GuiState& s) { s.toggle_preview(); }},
    {"sync.now", ImGuiKey_F5, CommandScope::Global, [](GuiState& s) { s.sync_now(); }},
    // Not içi bul (TUI editörü Ctrl+F, F3): bul satırı editörün altında; sonraki/önceki eşleşme.
    {"find", ImGuiMod_Ctrl | ImGuiKey_F, CommandScope::Global, [](GuiState& s) { s.find_in_note(); }},
    {"find.next", ImGuiKey_F3, CommandScope::Global, [](GuiState& s) { s.find_step(1); }},
    {"find.previous", ImGuiMod_Shift | ImGuiKey_F3, CommandScope::Global,
     [](GuiState& s) { s.find_step(-1); }},
    // Arama: F(ind) her yerde, T(ag) seçici, R(un view) seçici.
    {"search.global", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, CommandScope::Global,
     [](GuiState& s) { s.search_global(); }},
    {"search.tag", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T, CommandScope::Global,
     [](GuiState& s) { s.open_picker(Picker::Tag); }},
    {"search.view", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R, CommandScope::Global,
     [](GuiState& s) { s.open_picker(Picker::View); }},
    // Sekmeler: T(ab), W(indow) kapat; tarayıcı ve editörlerdeki yerleşik eşleşme.
    {"tab.new", ImGuiMod_Ctrl | ImGuiKey_T, CommandScope::Global, [](GuiState& s) { s.new_tab(); }},
    // Satır menüsü ve palet; listede Ctrl+Enter aynı işi görür (view_browser).
    {"tab.open", 0, CommandScope::Widget,
     [](GuiState& s) {
         if (const auto path = s.current_path()) {
             s.open_in_new_tab(*path);
             s.request_focus(FocusRequest::Editor);
         }
     }},
    {"tab.close", ImGuiMod_Ctrl | ImGuiKey_W, CommandScope::Global,
     [](GuiState& s) { s.close_active_tab(); }},
    {"tab.next", ImGuiMod_Ctrl | ImGuiKey_Tab, CommandScope::Global, [](GuiState& s) { s.next_tab(1); }},
    {"tab.previous", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, CommandScope::Global,
     [](GuiState& s) { s.next_tab(-1); }},
    {"tab.next", ImGuiMod_Ctrl | ImGuiKey_PageDown, CommandScope::Global, [](GuiState& s) { s.next_tab(1); }},
    {"tab.previous", ImGuiMod_Ctrl | ImGuiKey_PageUp, CommandScope::Global,
     [](GuiState& s) { s.next_tab(-1); }},
    // Del yalnız liste odaktayken: editörde ve arama kutusunda karakter silmeyi ezmesin.
    {"trash", ImGuiKey_Delete, CommandScope::List, [](GuiState& s) { s.delete_selected(false); }},
    {"delete", ImGuiMod_Shift | ImGuiKey_Delete, CommandScope::List,
     [](GuiState& s) { s.delete_selected(true); }},
    // Ad/İçerik/Tümü: M(ode); arama kutusunda Tab tamamlar (TUI girdi satırı).
    {"search.mode", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, CommandScope::Global,
     [](GuiState& s) { s.toggle_search_mode(); }},
    {"folder.new", 0, CommandScope::Widget, [](GuiState& s) { s.new_folder(s.target_folder()); }},
    // Palet ve araç çubuğu menüsü: dosya diyaloğu; klasör diyaloğu menünün ikinci satırıdır.
    {"import", 0, CommandScope::Widget,
     [](GuiState& s) { s.request_import(GuiState::ImportRequest::Files); }},
    {"rename", 0, CommandScope::Widget, [](GuiState& s) { s.rename_selected(); }},
    {"go.trash", 0, CommandScope::Widget, [](GuiState& s) { s.toggle_trash(); }},
    {"view.tree", 0, CommandScope::Widget, [](GuiState& s) { s.set_browser_view(BrowserView::Tree); }},
    {"view.details", 0, CommandScope::Widget, [](GuiState& s) { s.set_browser_view(BrowserView::Details); }},
    {"view.map", 0, CommandScope::Widget, [](GuiState& s) { s.set_browser_view(BrowserView::Map); }},
    {"restore", 0, CommandScope::Widget, [](GuiState& s) { s.restore_trashed(); }},
    {"trash.empty", 0, CommandScope::Widget, [](GuiState& s) { s.empty_trash(); }},
    {"trash.back", 0, CommandScope::Widget, [](GuiState& s) { s.close_trash(); }},
    // Sıralama düğmesi ve menüsü (TUI `o`).
    {"sort", 0, CommandScope::Widget, [](GuiState& s) { s.open_sort_menu(); }},
    {"sort.name", 0, CommandScope::Widget, [](GuiState& s) { s.choose_sort(search::SortField::Name); }},
    {"sort.updated", 0, CommandScope::Widget, [](GuiState& s) { s.choose_sort(search::SortField::Updated); }},
    {"sort.created", 0, CommandScope::Widget, [](GuiState& s) { s.choose_sort(search::SortField::Created); }},
    {"sort.size", 0, CommandScope::Widget, [](GuiState& s) { s.choose_sort(search::SortField::Size); }},
    {"sort.reverse", 0, CommandScope::Widget, [](GuiState& s) { s.reverse_sort(); }},
    // Liste odaktayken tek harf: metin kutusu yok, harf arama kutusuna gitmez. Harfler İngilizce adlardan:
    // A(dd), M(ark), Y(ank), P(aste), C(olumn), E(dit).
    {"quick.add", ImGuiKey_A, CommandScope::List, [](GuiState& s) { s.begin_quick_add(); }},
    {"mark", ImGuiKey_M, CommandScope::List, [](GuiState& s) { s.toggle_mark(); }},
    {"mark.clear", ImGuiMod_Shift | ImGuiKey_M, CommandScope::List, [](GuiState& s) { s.clear_marks(); }},
    {"mark.all", ImGuiMod_Ctrl | ImGuiKey_A, CommandScope::List, [](GuiState& s) { s.mark_all(); }},
    {"yank", ImGuiKey_Y, CommandScope::List, [](GuiState& s) { s.yank(); }},
    {"paste", ImGuiKey_P, CommandScope::List, [](GuiState& s) { s.paste(); }},
    {"edit", ImGuiKey_E, CommandScope::List, [](GuiState& s) { s.edit_selected(); }},
    // Dış editör (TUI `E`): Shift+E; kopya (TUI `c`): GUI'nin alışılmış Ctrl+C'si (C sütun döngüsünde).
    {"external", ImGuiMod_Shift | ImGuiKey_E, CommandScope::List, [](GuiState& s) { s.open_external(); }},
    {"external", ImGuiMod_Shift | ImGuiKey_E, CommandScope::Preview, [](GuiState& s) { s.open_external(); }},
    // İncelemede düzenlemeye geçiş (TUI REVIEW `e` `Enter`); kilitli editörde aynı tuşlar GuiState::editor_key'de.
    {"edit", ImGuiKey_E, CommandScope::Preview, [](GuiState& s) { s.edit_selected(); }},
    {"edit", ImGuiKey_Enter, CommandScope::Preview, [](GuiState& s) { s.edit_selected(); }},
    {"edit", ImGuiKey_KeypadEnter, CommandScope::Preview, [](GuiState& s) { s.edit_selected(); }},
    {"copy", ImGuiMod_Ctrl | ImGuiKey_C, CommandScope::List, [](GuiState& s) { s.copy_selected(); }},
    {"copy", ImGuiMod_Ctrl | ImGuiKey_C, CommandScope::Preview, [](GuiState& s) { s.copy_selected(); }},
    {"toggle", ImGuiKey_Space, CommandScope::List, [](GuiState& s) { s.toggle_row(); }},
    {"tree.toggle_all", ImGuiMod_Shift | ImGuiKey_Space, CommandScope::List,
     [](GuiState& s) { s.toggle_tree_all(); }},
    {"details.all", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, CommandScope::Global,
     [](GuiState& s) { s.toggle_details_all(); }},
    {"go.root", ImGuiMod_Alt | ImGuiKey_Home, CommandScope::Global, [](GuiState& s) { s.go_root(); }},
    // Ayrıntılar sütunları: C döngü; tek tek seçim Ayrıntılar başlığının sağ tık menüsünde.
    {"column", ImGuiKey_C, CommandScope::List, [](GuiState& s) { s.cycle_details_column(); }},
    {"column.size_date", 0, CommandScope::Widget,
     [](GuiState& s) { s.set_details_column(DetailsColumn::SizeAndDate); }},
    {"column.size", 0, CommandScope::Widget, [](GuiState& s) { s.set_details_column(DetailsColumn::Size); }},
    {"column.updated", 0, CommandScope::Widget,
     [](GuiState& s) { s.set_details_column(DetailsColumn::Updated); }},
    {"column.created", 0, CommandScope::Widget,
     [](GuiState& s) { s.set_details_column(DetailsColumn::Created); }},
    {"column.sync", 0, CommandScope::Widget, [](GuiState& s) { s.set_details_column(DetailsColumn::Sync); }},
    {"column.origin", 0, CommandScope::Widget,
     [](GuiState& s) { s.set_details_column(DetailsColumn::Origin); }},
    {"column.none", 0, CommandScope::Widget, [](GuiState& s) { s.set_details_column(DetailsColumn::None); }},
    // Liste gezinmesi: oklar, Home/End, sayfa; sağ ok/Enter açar, sol ok/Backspace geri.
    {"up", ImGuiKey_UpArrow, CommandScope::List, [](GuiState& s) { s.navigate(-1); }, true},
    {"down", ImGuiKey_DownArrow, CommandScope::List, [](GuiState& s) { s.navigate(1); }, true},
    {"top", ImGuiKey_Home, CommandScope::List, [](GuiState& s) { s.select_edge(false); }},
    {"bottom", ImGuiKey_End, CommandScope::List, [](GuiState& s) { s.select_edge(true); }},
    {"page.up", ImGuiKey_PageUp, CommandScope::List, [](GuiState& s) { s.page(-1); }, true},
    {"page.down", ImGuiKey_PageDown, CommandScope::List, [](GuiState& s) { s.page(1); }, true},
    {"open", ImGuiKey_RightArrow, CommandScope::List, [](GuiState& s) { s.browse_right(); }},
    {"open", ImGuiKey_Enter, CommandScope::List, [](GuiState& s) { s.browse_enter(); }},
    {"open", ImGuiKey_KeypadEnter, CommandScope::List, [](GuiState& s) { s.browse_enter(); }},
    {"back", ImGuiKey_LeftArrow, CommandScope::List, [](GuiState& s) { s.browse_left(); }},
    {"back", ImGuiKey_Backspace, CommandScope::List, [](GuiState& s) { s.browse_left(); }},
    // Önizleme kaydırması (TUI REVIEW): oklar satır, Shift+ok büyük adım, Ctrl+U/Ctrl+D yarım sayfa.
    {"up", ImGuiKey_UpArrow, CommandScope::Preview, [](GuiState& s) { s.scroll_preview(-1.0F); }, true},
    {"down", ImGuiKey_DownArrow, CommandScope::Preview, [](GuiState& s) { s.scroll_preview(1.0F); }, true},
    {"top", ImGuiKey_Home, CommandScope::Preview, [](GuiState& s) { s.scroll_preview(-FLT_MAX); }},
    {"bottom", ImGuiKey_End, CommandScope::Preview, [](GuiState& s) { s.scroll_preview(FLT_MAX); }},
    {"page.up", ImGuiKey_PageUp, CommandScope::Preview, [](GuiState& s) { s.scroll_preview_half(-2); }, true},
    {"page.down", ImGuiKey_PageDown, CommandScope::Preview, [](GuiState& s) { s.scroll_preview_half(2); },
     true},
    {"half.up", ImGuiMod_Ctrl | ImGuiKey_U, CommandScope::Preview,
     [](GuiState& s) { s.scroll_preview_half(-1); }, true},
    {"half.down", ImGuiMod_Ctrl | ImGuiKey_D, CommandScope::Preview,
     [](GuiState& s) { s.scroll_preview_half(1); }, true},
    {"big.up", ImGuiMod_Shift | ImGuiKey_UpArrow, CommandScope::Preview,
     [](GuiState& s) { s.scroll_preview(-kBigScrollLines); }, true},
    {"big.down", ImGuiMod_Shift | ImGuiKey_DownArrow, CommandScope::Preview,
     [](GuiState& s) { s.scroll_preview(kBigScrollLines); }, true},
    // İlişki R(elate), bağlantı paneline odak; bölme N(arrow), W(ide), Z(oom); yardım F1, çıkış Q(uit).
    {"link", ImGuiMod_Ctrl | ImGuiKey_R, CommandScope::Global, [](GuiState& s) { s.link_note(); }},
    {"links.focus", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, CommandScope::Global,
     [](GuiState& s) { s.focus_links(); }},
    {"pane.narrow", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_N, CommandScope::Global,
     [](GuiState& s) { s.resize_pane(-1); }},
    {"pane.wide", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_W, CommandScope::Global,
     [](GuiState& s) { s.resize_pane(1); }},
    {"pane.zoom", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Z, CommandScope::Global,
     [](GuiState& s) { s.toggle_zoom(); }},
    {"icons.toggle", 0, CommandScope::Widget, [](GuiState& s) { s.toggle_icons(); }},
    {"help", ImGuiKey_F1, CommandScope::Global, [](GuiState& s) { s.show_help(); }},
    {"quit", ImGuiMod_Ctrl | ImGuiKey_Q, CommandScope::Global, [](GuiState& s) { s.request_quit(); }},
};

static_assert(std::ranges::all_of(kGuiCommands,
                                  [](const GuiCommand& command) { return app::has_command(command.id); }),
              "every GUI command id is in app::kCommandRegistry");

// Kimliğin ilk satırı; bilinmeyen kimlik derleme hatasıdır.
consteval const GuiCommand& gui_command(std::string_view id) {
    for (const GuiCommand& command : kGuiCommands)
        if (command.id == id)
            return command;
    throw "unknown GUI command id";
}

// Ayarlar açıkken de yerinde koşan komutlar: kendi geçişi, görünmeyen iş (senkron, yardım, ikon ayarı) ve kendi
// sorusunu soran çıkış.
inline constexpr std::string_view kSettingsCommands[] = {"settings", "sync.now", "help", "quit", "icons.toggle"};

// Kısayolun, düğmenin ve paletin tek yolu. Notları değiştiren komut önce Ayarlar'dan çıkar: kaydedilmemiş formda
// Kaydet / At / Vazgeç sorulur, Vazgeç'te komut koşmaz (yeni not Ayarlar'ın arkasında açılmaz).
// false: Ayarlar'da kalındı (Vazgeç), komut koşmamalı.
inline bool leave_settings_for(GuiState& state, const GuiCommand& command) {
    return std::ranges::find(kSettingsCommands, command.id) != std::end(kSettingsCommands) ||
           state.leave_settings();
}

inline void run_gui_command(GuiState& state, const GuiCommand& command) {
    if (leave_settings_for(state, command))
        command.run(state);
}

}  // namespace noteapp::gui
