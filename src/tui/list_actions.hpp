#pragma once

#include "app/item_ops.hpp"
#include "app/trash_ops.hpp"
#include "core/config.hpp"
#include "core/note_store.hpp"
#include "edit/clipboard.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <expected>
#include <filesystem>
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
ListPrefs parse_list_prefs(std::string_view text, const SearchHistoryLimits& history = {});
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
    core::Config* config = nullptr;  // palet `icons.toggle` günceller; yalnız UI iş parçacığı yazar
    // Not ya da klasör taşındı (F2, `m`, klasör adı): açık sekmeler yeni yolu izler. Listeden önce çağrılır.
    std::function<void(const std::string& from, const std::string& to)> on_moved = {};
};

class ListActions {
public:
    explicit ListActions(ListHost host)
        : host_(std::move(host)),
          trash_(host_.config ? host_.config->limits.app.trash_undo_steps : core::Limit{}) {}

    void run(Intent intent, std::optional<ConfirmState>& confirm);
    // `L` yol girdisi açar: main'e dal eklemeden aynı yoldan yürür.
    void run(Intent intent, std::optional<ConfirmState>& confirm, std::optional<RenameState>& rename);
    // Onay evet: not kalıcı silinir (çöp görünümünde çöpten) ya da çöp boşaltılır.
    void finish_delete(std::optional<ConfirmState>& confirm);
    // Fareyi modele uygular; REVIEW'da sürükleme bittiyse seçimi panoya kopyalar.
    void on_mouse(const MouseInput& input, EditorSession& editor);
    // `F2`: girdi seçili notun yoluyla açılır. Enter NoteStore::move'dan geçer: hedef varsa hata girdide
    // kalır, ezme yok. Esc ya da değişmeyen yol hiçbir şey yazmaz.
    void begin_rename(std::optional<RenameState>& rename);
    // `m`: seçim ya da satır için hedef klasör girdisi, geçerli klasörle dolu; Tab klasörleri önerir.
    void begin_move_to(std::optional<RenameState>& rename);
    // `L`: boş girdi, Tab not yollarını önerir; Enter NoteStore::add_relation, hata girdide kalır.
    void begin_link(std::optional<RenameState>& rename);
    // Palet `import`: boş yol girdisi; Enter app::import_paths'le `folder`a alır (GUI ile aynı kural). Hiçbir
    // şey alınamazsa neden girdide kalır; alınınca özet durum satırında, ilk not REVIEW'da açılır.
    void begin_import(std::optional<RenameState>& rename, std::string folder);
    // `a` Enter: `/` ile biten ad klasördür, Listede kalır; değilse boş not oluşur ve `open` çağrılır. Ad
    // klasör görünümünde geçerli klasöre göredir. Hata satırda kalır.
    void submit_new_row(std::optional<NewRowState>& row, const NewRowDefaults& defaults,
                        const std::function<void(const core::NotePath&)>& open);
    void handle_rename_key(std::optional<RenameState>& rename, Key key, std::string_view text);
    // Yeniden tarama: çöp görünümü açıksa o da yenilenir (senkron çöpü değiştirmiş olabilir).
    void refresh_trash_view();
    // Palet son kullanılanları değiştirdiyse `tui-state.toml`a yazar.
    void save_prefs_if_changed();

private:
    void toggle_icons();
    void copy_selected_note();
    void copy_preview_selection();
    void copy_text(std::string text);
    void trash_selected();
    void restore_last();
    void restore_selected();
    void toggle_trash();
    void begin_delete(std::optional<ConfirmState>& confirm);
    void begin_empty_trash(std::optional<ConfirmState>& confirm);
    void delete_note(const core::NotePath& target, const app::Prompts& prompts);
    void empty_trash(const app::Prompts& prompts);
    void save_prefs();
    void moved(const std::string& from, const std::string& to) const;
    void submit_rename(std::optional<RenameState>& rename);
    void submit_move_to(std::optional<RenameState>& rename);
    void submit_link(std::optional<RenameState>& rename);
    void submit_import(std::optional<RenameState>& rename);
    // `P`: GUI gibi `<ad>.pdf` önerir; PDF diskteki baytlardan noteapp_pdf ile üretilir. Var olan dosya ancak
    // aynı yolda ikinci Enter'la ezilir.
    void begin_export(std::optional<RenameState>& rename);
    // REVIEW'da seçili [[bağlantı]]: depo çözer, not açılır; çözülemezse neden alt satırda. Yazma yok:
    // REVIEW'a editör kapanınca (kayıt ya da vazgeçme sorusundan sonra) dönülür, bekleyen düzenleme olmaz.
    void follow_wiki_link();
    void submit_export(std::optional<RenameState>& rename);
    std::expected<void, core::Error> export_note(const core::NotePath& note,
                                                 const std::filesystem::path& output) const;
    void begin_unlink(std::optional<ConfirmState>& confirm);
    void finish_unlink();
    // Haritada kenar kur ya da kaldır (TuiState::take_map_edit, app::map_relations); sonuç durum satırında.
    void apply_map_edit();
    void create_folder_from_row(std::optional<NewRowState>& row);
    void submit_folder_rename(std::optional<RenameState>& rename, const std::string& target);
    void trash_folder(const std::string& folder, const app::Prompts& prompts);
    void copy_selected();
    void delete_folder(const std::string& folder, const app::Prompts& prompts);
    // Çoklu seçim: işlem kalanla sürer, öğe öğe sonuç özette.
    struct BatchReport;
    void begin_trash(std::optional<ConfirmState>& confirm);
    void begin_delete_action(std::optional<ConfirmState>& confirm);
    void restore_action();
    void trash_marked(const app::Prompts& prompts);
    void delete_marked(const app::Prompts& prompts);
    void restore_marked();
    void paste();
    void publish(BatchReport& report, const char* done_key);
    void publish(const app::BatchReport& report, const char* done_key);
    // Onay penceresi modaldır, karar sonra gelir: istem soruyu `confirm`a yazar ve "hayır" der; Evet'te
    // finish_delete `retry`ı koşar, o da aynı işlemi onaylı istemle yeniden çağırır.
    app::Prompts ask_later(std::optional<ConfirmState>& confirm, std::function<void()> retry);

    ListHost host_;
    // Çöp, geri alma yığını ve kalıcı silme kuralı GUI ile ortak (app::TrashOps); yığın bu oturumluktur.
    app::TrashOps trash_;
    std::function<void()> pending_;  // açık onayın Evet'i; Vazgeç main'de kalır, bir sonraki run() düşürür
    // Açık onay ilişki için (seçili not, satırdaki not). Vazgeçilirse sonraki run() düşürür.
    std::optional<std::pair<core::NotePath, core::NotePath>> relation_target_;
    std::filesystem::path export_replace_;  // uyarısı gösterilmiş var olan çıktı; sıradaki Enter onu ezer
};

}  // namespace noteapp::tui
