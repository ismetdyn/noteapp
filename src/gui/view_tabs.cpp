#include "core/i18n.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>  // sekme çubuğunun sırası (TabBarGetTabOrder) ve kaydırma durumu (ImGuiTabBar)
#include <utility>

namespace noteapp::gui {

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
    ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
    // Birden çok sekmede ImGui (1.93) kaydırma oklarını çubuğun sağına koyar (her biri 1 em'den dar) ve
    // sekmeye kaydırırken iki yanda 1 em komşu payı bırakır: sekme bunların dışında kalan yere sığmalı.
    constexpr float kScrollReserveEms = 4.0F;
    const float reserve = tabs.size() > 1 ? kScrollReserveEms * ImGui::GetFontSize() : 0.0F;
    const float tab_room = bar->BarRect.GetWidth() - reserve;
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
            ImGui::PushFont(fonts().italic, 0.0F);
        // Kaydırmalı çubuk sekmeyi daraltmaz: dar bölmede tek sekme çubuktan geniş olup etkin sekme taşardı.
        // Sınır yalnız taşan sekmede; başlık ImGui'nin üç noktasıyla kısalır, tam adı ipucunda.
        const bool has_marker = !tab.pinned || tab.unsaved;  // kapatma düğmesi ya da kaydedilmedi işareti
        if (ImGui::TabItemCalcSize(label.c_str(), has_marker).x > tab_room)
            ImGui::SetNextItemWidth(std::max(tab_room, 1.0F));
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
    remember_rect("note_tabs", bar->BarRect.Min, bar->BarRect.Max);
    ImGui::EndTabBar();
    ImGui::PopItemFlag();
}

}  // namespace noteapp::gui
