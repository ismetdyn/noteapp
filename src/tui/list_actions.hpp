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
// `a` için config değerleri; `folder` düz listede klasörsüz adın gittiği yerdir.
struct NewRowDefaults {
    std::string folder;
    std::string type;
    std::size_t slug_max_length = 0;
};

struct ListHost {
    // cppcheck-suppress uninitMemberVarNoCtor ; referans üye, ilk atama derlemede zorunlu
    std::optional<core::NoteStore>& store;
    // cppcheck-suppress uninitMemberVarNoCtor ; referans üye, ilk atama derlemede zorunlu
    std::mutex& store_mutex;  // yeniden tarama iş parçacığı aynı store'u listeler
    // cppcheck-suppress uninitMemberVarNoCtor ; referans üye, ilk atama derlemede zorunlu
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
    // `m`: seçim ya da satır için hedef klasör girdisi, geçerli klasörle dolu; Tab klasörleri önerir.
    void begin_move_to(std::optional<RenameState>& rename);
    // `a` Enter: `/` ile biten ad klasördür, Listede kalır; değilse boş not oluşur ve `open` çağrılır. Ad
    // klasör görünümünde geçerli klasöre göredir. Hata satırda kalır.
    void submit_new_row(std::optional<NewRowState>& row, const NewRowDefaults& defaults,
                        const std::function<void(const core::NotePath&)>& open);
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
    void submit_move_to(std::optional<RenameState>& rename);
    std::optional<core::Error> move_collision(const RenameState& rename, const std::string& dest) const;
    void forget_trashed(const core::NotePath& item);
    void create_folder_from_row(std::optional<NewRowState>& row);
    void submit_folder_rename(std::optional<RenameState>& rename, const std::string& target);
    void begin_trash_folder(const std::string& folder, std::optional<ConfirmState>& confirm);
    void trash_folder(const std::string& folder);
    void restore_batch();
    void copy_selected();
    void finish_folder_delete(const std::string& folder);
    // Çoklu seçim: işlem kalanla sürer, öğe öğe sonuç özette.
    struct BatchReport;
    enum class BatchAction { None, Trash, Delete };
    struct TrashBatch;
    void begin_trash(std::optional<ConfirmState>& confirm);
    void begin_delete_action(std::optional<ConfirmState>& confirm);
    void restore_action();
    void trash_marked();
    void delete_marked();
    void restore_marked();
    void paste();
    void paste_item(const ListItem& item, const std::string& dest, BatchReport& report);
    void copy_folder_item(const std::string& folder, const std::string& dest, BatchReport& report);
    void publish(BatchReport& report, const char* done_key);
    std::size_t note_count_of(const std::vector<ListItem>& items) const;
    std::optional<core::Error> trash_folder_into(TrashBatch& batch, const std::string& folder);

    ListHost host_;
    // Çöp not başınadır: klasör silme, notları tek tek atıp klasörü kaldıran bir toplu adımdır. `u` adımın
    // tamamını geri alır; klasör alanları boş alt klasörleri de geri getirir.
    struct TrashBatch {
        std::vector<core::NotePath> notes = {};
        std::vector<std::string> folders = {};
        std::string folder = {};  // klasör silmesi: geri alınca seçilecek satır
    };
    std::vector<TrashBatch> trashed_;                // bu oturumda; `u` sondan alır
    BatchAction batch_pending_ = BatchAction::None;  // açık onay çoklu seçim için
    std::optional<core::NotePath> delete_target_;
    std::optional<std::string> folder_target_;  // açık onay klasör için
    bool folder_permanent_ = false;             // `D`: kalıcı; `d`: çöpe
    bool empty_pending_ =
        false;  // açık onay tüm çöp için; Vazgeç main'de kalır, bir sonraki begin_* sıfırlar
};

}  // namespace noteapp::tui
