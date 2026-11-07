#pragma once

#include "core/limits.hpp"
#include "core/note_path.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::app {

// Editör sekmelerinin arayüzden bağımsız kuralları (SPEC-gui "Sekmeler", SPEC-tui "Sekmeler"). Sekmenin
// içeriği arayüzündedir (GUI anlık görüntü, TUI EditorSession); sıra, sınır, kalıcılık, komşu ve taşıma
// kararı burada tek kez yazılır.

// Durum dosyasındaki `[[tabs]]` girdisi (`gui-state.toml`, `tui-state.toml`; app::UiPrefs okur ve yazar).
struct SavedTab {
    std::string path;
    bool pinned = false;
    bool kept = false;
    bool operator==(const SavedTab&) const = default;
};

struct SavedTabs {
    std::vector<SavedTab> tabs;
    std::size_t active = 0;  // `active_tab`: kayıtlı sekmeler içindeki sıra
    bool operator==(const SavedTabs&) const = default;
};

struct RestoredTab {
    core::NotePath path;
    bool pinned = false;
    bool kept = false;
};

struct RestoredTabs {
    std::vector<RestoredTab> tabs;
    std::size_t active = 0;  // `tabs` içinde; sekme yoksa 0
};

// Açılışta geri gelecek sekmeler: yolu bozuk, artık olmayan (`exists` false) ya da yinelenen girdi atlanır;
// sınır dolunca kalanlar düşer. Kayıtlı etkin sekme atlandıysa ilk sekme etkindir.
RestoredTabs restorable_tabs(const SavedTabs& saved, const std::function<bool(const core::NotePath&)>& exists,
                             const core::Limit& limit);

// Etkin sekme sonra açılan notla değişmez mi (TUI ve GUI aynı kural). Notun sekmesi iğneliyse ya da
// korunuyorsa (yeni sekmede açıldı, düzenlendi). Taslak, metni boş değilse: yalnız boşluk da olsa korunur,
// metin kaybolmaz. Boş taslak korunacak bir şey taşımaz.
bool keeps_tab(bool pinned, bool kept, bool is_draft, std::string_view draft_text);

// limits.app.tabs: `count` sekme açıkken bir sekme daha açılabilir mi (0 = sınırsız).
bool can_add_tab(std::size_t count, const core::Limit& limit);

// Sonraki/önceki sekme: sondan başa, baştan sona döner. `count` 0 ise 0.
std::size_t cycled_tab(std::size_t index, int delta, std::size_t count);

// `index`teki sekme kapandı, `remaining` sekme kaldı: öne gelen sağdaki, yoksa soldaki.
std::size_t neighbour_after_close(std::size_t index, std::size_t remaining);

// Not ya da klasör `from`dan `to`ya taşındı: `path` onunla ya da altındaysa yeni yolu, değilse nullopt.
std::optional<core::NotePath> moved_path(const core::NotePath& path, std::string_view from,
                                         std::string_view to);

// Arayüzün sekme kaydı: kimlik, iğne ve korunma; içeriğin geri kalanı arayüzündedir. Kurulabilirlik burada
// sorulmaz: kayıt arayüz sınıfının içinde tanımlıdır, üye ilk değerleri sınıf bitmeden denetlenemez.
template <class T>
concept TabEntry = requires(T tab) {
    { tab.id } -> std::convertible_to<std::uint64_t>;
    { tab.pinned } -> std::convertible_to<bool>;
    { tab.kept } -> std::convertible_to<bool>;
};

// İki arayüzün sekme listesi (TUI EditorTabs, GUI GuiState): sıra, kimlik, etkin sekme, sınır, kapanınca
// komşu, kayıt ve geri yükleme tek yerde. İçerik (oturum, anlık görüntü) ve onun kuralları (park, kayıt
// sorusu, taşınan notu izleme) arayüzündedir. Sınır çağrı başınadır: GUI'de ayarlarla değişebilir.
template <TabEntry Tab> class TabList {
public:
    using iterator = typename std::vector<Tab>::iterator;
    using const_iterator = typename std::vector<Tab>::const_iterator;

    iterator begin() { return tabs_.begin(); }
    iterator end() { return tabs_.end(); }
    const_iterator begin() const { return tabs_.begin(); }
    const_iterator end() const { return tabs_.end(); }
    bool empty() const { return tabs_.empty(); }
    std::size_t size() const { return tabs_.size(); }
    const Tab& front() const { return tabs_.front(); }

    std::uint64_t active_id() const { return active_; }  // 0 = sekme yok
    void activate(std::uint64_t id) { active_ = id; }
    Tab* active() { return find(active_); }
    const Tab* active() const { return find(active_); }

    Tab* find(std::uint64_t id) {
        const auto found = std::ranges::find(tabs_, id, &Tab::id);
        return found == tabs_.end() ? nullptr : &*found;
    }
    const Tab* find(std::uint64_t id) const {
        const auto found = std::ranges::find(tabs_, id, &Tab::id);
        return found == tabs_.end() ? nullptr : &*found;
    }
    template <class Pred> Tab* find_if(Pred pred) {
        const auto found = std::ranges::find_if(tabs_, pred);
        return found == tabs_.end() ? nullptr : &*found;
    }

    // Sona yeni sekme, kimliği verilmiş; etkinleştirmez. `limit` (limits.app.tabs) doluysa nullptr. Dönen
    // gösterici sonraki push'a dek geçerlidir.
    Tab* push(const core::Limit& limit) {
        if (!can_add_tab(tabs_.size(), limit))
            return nullptr;
        Tab& tab = tabs_.emplace_back();
        tab.id = next_id_++;
        return &tab;
    }

    // `gone` dediği sekmeler kapanır. Etkin sekme kapandıysa sağındaki, yoksa solundaki etkin olur (sekme
    // kalmadıysa 0). Dönüş: etkin sekme kapandı mı.
    template <class Pred> bool close_if(Pred gone) {
        const auto at = std::ranges::find(tabs_, active_, &Tab::id);
        const auto active_index = static_cast<std::size_t>(at - tabs_.begin());
        std::size_t before_active = 0;
        bool active_closed = false;
        std::size_t index = 0;
        std::erase_if(tabs_, [&](const Tab& tab) {
            const bool closing = gone(tab);
            before_active += closing && index < active_index ? 1 : 0;
            active_closed = active_closed || (closing && tab.id == active_);
            ++index;
            return closing;
        });
        if (active_closed)
            active_ = tabs_.empty()
                          ? 0
                          : tabs_[neighbour_after_close(active_index - before_active, tabs_.size())].id;
        return active_closed;
    }
    bool close(std::uint64_t id) {
        return close_if([id](const Tab& tab) { return tab.id == id; });
    }

    // Etkin sekmeden `delta` ötedeki sekmenin kimliği (sondan başa döner); sekme yoksa 0.
    std::uint64_t cycled(int delta) const {
        if (tabs_.empty())
            return 0;
        const auto at = std::ranges::find(tabs_, active_, &Tab::id);
        const auto index = static_cast<std::size_t>(at == tabs_.end() ? 0 : at - tabs_.begin());
        return tabs_[cycled_tab(index, delta, tabs_.size())].id;
    }

    // Sürüklemeyle yeni sıra: `order`daki kimlikler o sırayla, yinelenen ve bilinmeyen atlanır, eksikler
    // sonda eski sıralarıyla.
    void reorder(std::span<const std::uint64_t> order) {
        std::vector<Tab> sorted;
        sorted.reserve(tabs_.size());
        const auto taken = [&](std::uint64_t id) {
            return std::ranges::find(sorted, id, &Tab::id) != sorted.end();
        };
        for (const std::uint64_t id : order)
            if (Tab* tab = find(id); tab != nullptr && !taken(id))
                sorted.push_back(std::move(*tab));
        for (Tab& tab : tabs_)  // taşınmış öğenin kimliği yerinde kalır: ikinci kez alınmaz
            if (!taken(tab.id))
                sorted.push_back(std::move(tab));
        tabs_ = std::move(sorted);
    }

    // Durum dosyasındaki sekmeler; `path_of` nullopt dönen (taslak) yazılmaz.
    SavedTabs saved(const std::function<std::optional<core::NotePath>(const Tab&)>& path_of) const {
        SavedTabs out;
        for (const Tab& tab : tabs_) {
            const auto path = path_of(tab);
            if (!path)
                continue;
            if (tab.id == active_)
                out.active = out.tabs.size();
            out.tabs.push_back({path->string(), tab.pinned, tab.kept});
        }
        return out;
    }

    // Açılış: geri gelecek sekmeler (restorable_tabs) listenin yerine geçer, kayıtlı etkin sekme etkin olur;
    // `fill` içeriği kurar. Geri gelecek sekme yoksa liste değişmez, false.
    bool restore(const SavedTabs& saved, const std::function<bool(const core::NotePath&)>& exists,
                 const core::Limit& limit, const std::function<void(Tab&, const RestoredTab&)>& fill) {
        const RestoredTabs plan = restorable_tabs(saved, exists, limit);
        if (plan.tabs.empty())
            return false;
        tabs_.clear();
        for (const RestoredTab& entry : plan.tabs) {
            Tab& tab = tabs_.emplace_back();
            tab.id = next_id_++;
            tab.pinned = entry.pinned;
            tab.kept = entry.kept;
            fill(tab, entry);
        }
        active_ = tabs_[plan.active].id;
        return true;
    }

private:
    std::vector<Tab> tabs_;
    std::uint64_t active_ = 0;
    std::uint64_t next_id_ = 1;
};

}  // namespace noteapp::app
