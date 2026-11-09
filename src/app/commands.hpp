#pragma once

#include "core/limits.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Komutun anlamlı olduğu yer (bit kümesi). Arayüz kendi görünümünü bunlardan birine eşler: TUI REVIEW ve
// GUI önizlemesi kInPreview'dur. Tuş bağı arayüze özgüdür; bağın geçtiği yerler buradakilerin alt kümesidir.
inline constexpr unsigned kInList = 1U;     // not ve klasör listesi: ağaç, Ayrıntılar, düz arama
inline constexpr unsigned kInPreview = 2U;  // tek not okunuyor
inline constexpr unsigned kInTrash = 4U;
inline constexpr unsigned kInEditor = 8U;
inline constexpr unsigned kAnywhere = kInList | kInPreview | kInTrash | kInEditor;

// Komutun durumdan istediği. Değerlendirme arayüzdedir (seçim, bağlantı, yank kendi modelindedir); arayüz bir
// görünümde daha dar bir koşul isteyebilir (TUI REVIEW'da gösterilen not).
enum class CommandNeeds { Nothing, Note, Row, Links, FolderView, Yank, Sync };

struct CommandSpec {
    std::string_view id;    // kararlı İngilizce ad: tuş tabloları ve son kullanılanlar buna bağlanır
    const char* label_key;  // i18n; iki arayüzün paleti ve yardımı aynı metni gösterir
    unsigned contexts;
    CommandNeeds needs = CommandNeeds::Nothing;
};

// Tek komut kaydı. Yeni komut: buraya bir satır, en.toml/tr.toml [command]'a etiket, her arayüzün tablosuna
// bağ (yoksa tests/support/command_parity_gaps.hpp'ye sahipli satır). Başlıkta: arayüz tabloları kimliği
// derleme anında doğrular.
inline constexpr CommandSpec kCommandRegistry[] = {
    // Oluşturma ve düzenleme
    {"new", "command.new", kInList | kInEditor},
    {"quick.add", "command.quick_add", kInList},
    {"edit", "command.edit", kInList | kInPreview, CommandNeeds::Row},
    {"external", "command.external", kInList | kInPreview, CommandNeeds::Note},
    {"save", "command.save", kInEditor, CommandNeeds::Note},
    {"folder.new", "command.folder_new", kInList},
    {"import", "command.import", kInList},  // dosya/klasör içe alma (app::import_paths)
    // Seçim üzerinde
    {"move", "command.move", kInList | kInPreview | kInEditor, CommandNeeds::Row},
    {"rename", "command.rename", kInList | kInPreview, CommandNeeds::Row},
    {"move.dialog", "command.move_dialog", kInList | kInEditor, CommandNeeds::Row},
    {"trash", "command.trash", kInList, CommandNeeds::Row},
    {"restore", "command.restore", kInList | kInTrash},
    {"delete", "command.delete", kInList | kInTrash, CommandNeeds::Row},
    {"trash.empty", "command.trash_empty", kInTrash, CommandNeeds::Note},
    {"copy", "command.copy", kInList | kInPreview, CommandNeeds::Row},
    {"link", "command.link", kInList | kInPreview, CommandNeeds::Note},
    {"export.pdf", "command.export_pdf", kInList | kInPreview | kInEditor, CommandNeeds::Note},
    // Çoklu seçim ve yank
    {"mark", "command.mark", kInList | kInTrash},
    {"mark.clear", "command.mark_clear", kInList | kInTrash},
    {"mark.all", "command.mark_all", kInList | kInTrash},
    {"yank", "command.yank", kInList},
    {"paste", "command.paste", kInList, CommandNeeds::Yank},
    // Sıra, sütun, ağaç, görünüm
    {"sort", "command.sort", kInList},
    {"sort.name", "command.sort_name", kInList},
    {"sort.updated", "command.sort_updated", kInList},
    {"sort.created", "command.sort_created", kInList},
    {"sort.size", "command.sort_size", kInList},
    {"sort.reverse", "command.sort_reverse", kInList},
    {"column", "command.column", kInList},
    {"column.size_date", "command.column_size_date", kInList},
    {"column.size", "command.column_size", kInList},
    {"column.updated", "command.column_updated", kInList},
    {"column.created", "command.column_created", kInList},
    {"column.sync", "command.column_sync", kInList},
    {"column.origin", "command.column_origin", kInList},
    {"column.none", "command.column_none", kInList},
    {"toggle", "command.toggle", kInList | kInTrash},
    {"tree.toggle_all", "command.tree_toggle_all", kInList, CommandNeeds::FolderView},
    {"details", "command.details", kAnywhere},
    {"details.all", "command.details_all", kInList | kInTrash},
    {"view.tree", "command.view_tree", kInList},
    {"view.details", "command.view_details", kInList},
    {"view.map", "command.view_map", kInList},
    // Arama ve gitme
    {"search", "command.search", kAnywhere},
    {"search.global", "command.search_global", kInList},
    {"search.tag", "command.search_tag", kInList},
    {"search.view", "command.search_view", kInList},
    {"view.save", "command.view_save", kInList},  // harita sorgusunun okuması kayıtlı görünüm olur
    {"view.remove", "command.view_remove", kInList},
    {"search.mode", "command.search_mode", kInList},
    {"find", "command.find", kInPreview},
    {"find.next", "command.find_next", kInPreview},
    {"find.previous", "command.find_previous", kInPreview},
    {"go.root", "command.go_root", kInList | kInTrash},
    {"go.trash", "command.go_trash", kInList | kInTrash},
    {"trash.back", "command.trash_back", kInTrash},
    {"open", "command.open", kInList},
    {"back", "command.back", kInList | kInPreview},
    {"links.focus", "command.links_focus", kInPreview, CommandNeeds::Links},
    {"links.toggle", "command.links_toggle", kInPreview | kInEditor},
    {"view.toggle", "command.view_toggle", kInPreview | kInEditor},
    // Gezinme
    {"up", "command.up", kInList | kInPreview | kInTrash},
    {"down", "command.down", kInList | kInPreview | kInTrash},
    {"top", "command.top", kInList | kInPreview | kInTrash},
    {"bottom", "command.bottom", kInList | kInPreview | kInTrash},
    {"page.up", "command.page_up", kInList | kInPreview | kInTrash},
    {"page.down", "command.page_down", kInList | kInPreview | kInTrash},
    {"half.up", "command.half_up", kInPreview},
    {"half.down", "command.half_down", kInPreview},
    {"big.up", "command.big_up", kInPreview},
    {"big.down", "command.big_down", kInPreview},
    // Panel ve sekme
    {"pane.narrow", "command.pane_narrow", kInList | kInPreview | kInTrash},
    {"pane.wide", "command.pane_wide", kInList | kInPreview | kInTrash},
    {"pane.zoom", "command.pane_zoom", kInList | kInPreview | kInTrash | kInEditor},
    {"tab.open", "command.tab_open", kInList | kInPreview, CommandNeeds::Note},
    {"tab.new", "command.tab_new", kAnywhere},
    {"tab.close", "command.tab_close", kInEditor},
    {"tab.next", "command.tab_next", kAnywhere},
    {"tab.previous", "command.tab_previous", kAnywhere},
    // Uygulama
    {"sync.now", "command.sync_now", kAnywhere, CommandNeeds::Sync},
    {"icons.toggle", "command.icons_toggle", kAnywhere},
    {"update", "command.update", kAnywhere},  // güncelleme penceresi (app::UpdateController)
    {"settings", "command.settings", kAnywhere},
    {"help", "command.help", kAnywhere},
    {"quit", "command.quit", kAnywhere},
};

// Kayıttaki komut; bilinmeyen kimlikte nullptr. constexpr: arayüz tabloları derlemede çağırır.
constexpr const CommandSpec* find_command(std::string_view id) {
    const auto found = std::ranges::find(kCommandRegistry, id, &CommandSpec::id);
    return found == std::end(kCommandRegistry) ? nullptr : &*found;
}

// Kimlik kayıtta mı. Derleme anı denetimleri bunu kullanır: UBSan'ın -fno-delete-null-pointer-checks'i statik
// nesnenin adresini nullptr ile karşılaştırmayı sabit ifade olmaktan çıkarır; yineleyici karşılaştırması etkilenmez.
constexpr bool has_command(std::string_view id) {
    return std::ranges::find(kCommandRegistry, id, &CommandSpec::id) != std::end(kCommandRegistry);
}

// Komutun istediği durumun arayüzdeki karşılığı (seçim, bağlantı ve yank arayüzün modelindedir).
struct CommandState {
    bool note = false;         // seçili ya da gösterilen not var
    bool row = false;          // işlenecek satır var (seçim ya da işaret)
    bool links = false;        // bağlantı satırı var ve bölüm gizli değil
    bool folder_view = false;  // klasör görünümü (TUI ağaç, GUI ağaç)
    bool yank = false;         // yapıştırılacak öğe var
    bool paste_folder = false;  // yapıştırma hedefi bir klasör (TUI klasör görünümü, GUI ağaç ya da Ayrıntılar)
    bool sync = false;         // senkron açık
};
// Neden komut çalışamaz; metni arayüz seçer (iletide arayüzün tuş adları geçer).
enum class CommandBlock { None, NoNote, NoRow, NoLinks, NotFolderView, NothingYanked, PasteNeedsFolder, SyncOff };
CommandBlock command_block(CommandNeeds needs, const CommandState& state);

// Paletin aday satırı: kimlik, etiket ve şu an çalışıp çalışamadığı (arayüz kararı).
struct PaletteCandidate {
    std::string_view id;
    const char* label_key;
    bool blocked;
};

// Süzgeçle (kimlik ya da çevrilmiş etiket, bulanık) eşleşen adayların sırası (`candidates` dizinleri):
// çalışanlar önce, sonra puan, sonra son kullanılan (`recent` en yeni başta). Boş süzgeçte puan eşittir,
// sırayı son kullanılan, o da eşitse giriş sırası belirler. `max_candidate`:
// limits.search.fuzzy_candidate_chars (sınırsız SIZE_MAX).
std::vector<std::size_t> rank_palette(std::string_view query, std::span<const PaletteCandidate> candidates,
                                      std::span<const std::string> recent, std::size_t max_candidate);

// Çalıştırılan komutu son kullanılanların başına alır; yinelenen kalkar, liste `limit`'le kısalır (0 =
// sınırsız).
void remember_command(std::vector<std::string>& recent, std::string_view id, const core::Limit& limit);

}  // namespace noteapp::app
