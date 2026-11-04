#pragma once

#include "core/limits.hpp"
#include "core/note_path.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Editör sekmelerinin arayüzden bağımsız kuralları (SPEC-gui "Sekmeler", SPEC-tui "Sekmeler"). Sekmenin
// içeriği arayüzündedir (GUI anlık görüntü, TUI EditorSession); sıra, sınır, kalıcılık, komşu ve taşıma
// kararı burada tek kez yazılır.

// Durum dosyasındaki `[[tabs]]` girdisi (`gui-state.toml`, `tui-state.toml`; aynı anahtarlar).
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

// `active_tab = N` ve `[[tabs]]` tabloları (TOML). Dosyanın sonuna yazılır: TOML'da tablo başlığından sonraki
// anahtar o tabloya aittir. Sekme yoksa boş.
std::string format_saved_tabs(const SavedTabs& saved);
// Metindeki `active_tab` satırından sonrası TOML olarak okunur (öncesi satır tabanlı tercihlerdir ve TOML'a
// uymayabilir). Bozuk ya da eksik girdi atlanır; okunamazsa boş.
SavedTabs parse_saved_tabs(std::string_view text);

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

// limits.app.tabs: `count` sekme açıkken bir sekme daha açılabilir mi (0 = sınırsız).
bool can_add_tab(std::size_t count, const core::Limit& limit);

// Sonraki/önceki sekme: sondan başa, baştan sona döner. `count` 0 ise 0.
std::size_t cycled_tab(std::size_t index, int delta, std::size_t count);

// `index`teki sekme kapandı, `remaining` sekme kaldı: öne gelen sağdaki, yoksa soldaki.
std::size_t neighbour_after_close(std::size_t index, std::size_t remaining);

// Not ya da klasör `from`dan `to`ya taşındı: `path` onunla ya da altındaysa yeni yolu, değilse nullopt.
std::optional<core::NotePath> moved_path(const core::NotePath& path, std::string_view from,
                                         std::string_view to);

}  // namespace noteapp::app
