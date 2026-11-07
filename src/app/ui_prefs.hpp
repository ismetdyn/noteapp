#pragma once

#include "app/tabs.hpp"
#include "core/folder_tree.hpp"
#include "core/limits.hpp"
#include "search/query.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::app {

// Listede (TUI sağ sütun) ve Ayrıntılar'da (GUI) gösterilen üst veri. Sıra `column` döngüsüdür.
enum class ListColumn { SizeAndDate, Size, Updated, Created, Sync, Origin, None };
// Durum dosyalarındaki `column` değeri ve `column.<ad>` komutları.
inline constexpr std::array<std::pair<ListColumn, std::string_view>, 7> kListColumnNames = {{
    {ListColumn::SizeAndDate, "size_and_date"},
    {ListColumn::Size, "size"},
    {ListColumn::Updated, "updated"},
    {ListColumn::Created, "created"},
    {ListColumn::Sync, "sync"},
    {ListColumn::Origin, "origin"},
    {ListColumn::None, "none"},
}};

// Arayüze özgü düz anahtarın değeri (TUI `list_share`; GUI pencere, bölme, yazı tipi, son not).
using PrefValue = std::variant<bool, std::int64_t, double, std::string>;

// Kalıcı arayüz tercihleri (`<state>/tui-state.toml`, `<state>/gui-state.toml`; TOML). İki arayüzün ortak
// alanları burada tek kuralla okunur ve yazılır; arayüze özgü anahtarlar `extra`dadır.
struct UiPrefs {
    search::SortSpec sort{search::SortField::Updated, true};  // en yeni başta
    ListColumn column = ListColumn::SizeAndDate;
    std::vector<std::string> search_history = {};   // en yeni başta; yalnız yerel, senkrona girmez
    std::set<std::string> open_folders = {};        // ağaçta yerinde açık klasörler
    std::vector<std::string> recent_commands = {};  // palet: en yeni başta
    SavedTabs tabs = {};                            // açık editör sekmeleri
    std::map<std::string, PrefValue, std::less<>> extra = {};
};

struct UiPrefsRules {
    // Okunan arama geçmişinin süzgeci (tui::kept_search_history: sır benzeri, bozuk, yinelenen ve fazla kayıt
    // düşer). app edit katmanına bağımlı olamaz: arayüz verir; boşsa geçmiş olduğu gibi kalır.
    std::function<std::vector<std::string>(std::vector<std::string>)> keep_history = {};
    core::Limit recent_commands = {};  // limits.app.recent_commands (0 = sınırsız)
    // Okunan dosyanın en büyük boyutu: tuning.max_note_bytes (0 = sınırsız). Listeler (açık klasör, geçmiş)
    // bununla sınırlıdır; aşan dosya bozuk sayılır ve kenara alınır.
    core::Limit max_bytes = {};
};

struct LoadedUiPrefs {
    UiPrefs prefs;
    // Dosya okunamadı ve `<ad>.bad` olarak kenara alındı (tek yedek, eskisinin yerine): kullanıcıya bir kez
    // gösterilir. Boş: sorun yok.
    std::string notice = {};
};

// Dosya elle değişmiş olabilir: söz dizimi bozuksa hepsi varsayılandır; bilinmeyen `sort`/`column` adı
// varsayılanda kalır; kayıtlı olmayan komut adı düşer ve liste sınıra iner; geçmiş `keep_history`den geçer.
UiPrefs parse_ui_prefs(std::string_view text, const UiPrefsRules& rules);
// Açık klasörlerden denetim karakterli olan ve `folder_exists` false döneni (ağaçta artık yok) yazılmaz.
std::string format_ui_prefs(const UiPrefs& prefs, const std::function<bool(const std::string&)>& folder_exists);
// `<state>/<file_name>`; yoksa (ilk çalıştırma) varsayılan. Söz dizimi bozuk ya da `max_bytes`tan büyük dosya
// ezilmeden önce `<file_name>.bad` olarak kenara alınır, varsayılanlar kullanılır ve `notice` dolar.
LoadedUiPrefs load_ui_prefs(std::string_view file_name, const UiPrefsRules& rules);
// Açık klasörlerin yazım süzgeci (format_ui_prefs, save_ui_prefs): ağaçta olmayan klasör düşer. Ağaç boşsa
// (ilk tarama bitmedi, depo açılamadı) süzgeç boştur: önceki liste korunur.
std::function<bool(const std::string&)> open_folder_filter(const core::FolderTree& tree);
// Atomik ve yalnız sahibi okuyabilir (arama geçmişi taşır). Yazılamazsa uyarı loglanır; arayüz sürer.
void save_ui_prefs(std::string_view file_name, const UiPrefs& prefs,
                   const std::function<bool(const std::string&)>& folder_exists);

// `extra`daki değer istenen türdeyse; sayı tam sayı yazılmışsa da okunur.
std::optional<bool> pref_bool(const UiPrefs& prefs, std::string_view key);
std::optional<std::int64_t> pref_int(const UiPrefs& prefs, std::string_view key);
std::optional<double> pref_number(const UiPrefs& prefs, std::string_view key);
std::optional<std::string> pref_string(const UiPrefs& prefs, std::string_view key);

}  // namespace noteapp::app
