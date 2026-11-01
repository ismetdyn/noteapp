#pragma once

#include "core/note_store.hpp"
#include "tui/clipboard.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

class EditorSession;

// Sıralama ve sağ sütun `<state>/tui-state.toml`da kalır; bozuk değer varsayılana döner. Sürüm işareti
// olmayan eski dosyada `column = "none"` o sürümün varsayılanıdır, seçim sayılmaz: yeni varsayılana taşınır.
std::string format_list_prefs(const ListPrefs& prefs);
ListPrefs parse_list_prefs(std::string_view text);
void load_list_prefs(TuiState& state);

// Liste ve Önizleme eylemlerinin yan etkileri (pano, çöp, sıralama kalıcılığı). main'in karmaşıklığı artmasın
// diye orada değil; tuş kararı TuiState'te kalır, burası Intent'i yürütür.
struct ListHost {
    std::optional<core::NoteStore>& store;
    std::mutex& store_mutex;  // yeniden tarama iş parçacığı aynı store'u listeler
    std::string& clipboard;
    SystemClipboard& system_clipboard;
    TuiState& state;
    PreviewCache& preview;
    StatusLine& status;
    std::function<void()> refresh_notes;
    std::function<void()> notify_changed;
};

class ListActions {
public:
    explicit ListActions(ListHost host) : host_(std::move(host)) {}

    void run(Intent intent, std::optional<ConfirmState>& confirm);
    // Onay evet: not kalıcı silinir (çöp görünümünde çöpten) ya da çöp boşaltılır.
    void finish_delete(std::optional<ConfirmState>& confirm);
    // Fareyi modele uygular; REVIEW'da sürükleme bittiyse seçimi panoya kopyalar.
    void on_mouse(const MouseInput& input, EditorSession& editor);
    // `F2`: girdi seçili notun yoluyla açılır. Enter NoteStore::move'dan geçer: hedef varsa hata girdide
    // kalır, ezme yok. Esc ya da değişmeyen yol hiçbir şey yazmaz.
    void begin_rename(std::optional<RenameState>& rename);
    void handle_rename_key(std::optional<RenameState>& rename, Key key, std::string_view text);
    // Yeniden tarama: çöp görünümü açıksa o da yenilenir (senkron çöpü değiştirmiş olabilir).
    void refresh_trash_view();

private:
    void copy_selected_note();
    void copy_preview_selection();
    void copy_text(std::string text);
    void trash_selected();
    void restore_last();
    void restore_selected();
    void toggle_trash();
    void begin_delete(std::optional<ConfirmState>& confirm);
    void begin_empty_trash(std::optional<ConfirmState>& confirm);
    void empty_trash();
    void save_prefs();
    void submit_rename(std::optional<RenameState>& rename);
    void forget_trashed(const core::NotePath& item);

    ListHost host_;
    std::vector<core::NotePath> trashed_;  // bu oturumda çöpe atılanlar; `u` sondan alır
    std::optional<core::NotePath> delete_target_;
    bool empty_pending_ =
        false;  // açık onay tüm çöp için; Vazgeç main'de kalır, bir sonraki begin_* sıfırlar
};

}  // namespace noteapp::tui
