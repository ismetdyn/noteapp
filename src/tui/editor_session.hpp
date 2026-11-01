#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "tui/clipboard.hpp"
#include "tui/completion.hpp"
#include "tui/editor_buffer.hpp"
#include "tui/note_find.hpp"
#include "tui/syntax.hpp"
#include "tui/tui_state.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Bir taslağın ilk kayıtta alacağı yer ve ad.
struct DraftName {
    std::string folder;
    std::string stem;  // boşsa ad içerikten (NoteStore::create kuralı) ya da zaman damgasından gelir
    std::string type;  // boşsa ilk kayıtta detect
    bool timestamp_name = false;  // ad istemi boş bırakıldı: zaman damgalı ad
    bool ask_name = false;        // `n` taslağı: ilk kayıt adı alt satırda sorar
};

// Ad istemindeki metni çözer: `klasör/ad.uzantı`; klasör yoksa `default_folder`, ad slug kuralından geçer,
// açık uzantı kazanır. Geçersiz yol hata döner (dosya oluşturulmaz).
std::expected<DraftName, core::Error> parse_draft_name(std::string_view input,
                                                       std::string_view default_folder,
                                                       std::chrono::system_clock::time_point now,
                                                       std::size_t slug_max_length);

// Yalnız gösterim için: geçersiz bayt ve NUL U+FFFD olur. İmleç ve sarma hesabı geçerli UTF-8 ister.
std::string sanitized_utf8(std::string_view raw);

// Oturumun dış dünyası; testler sahte geri çağrılarla kurar, main gerçek olanlarla.
struct EditorHost {
    core::NoteStore& store;
    std::mutex& store_mutex;  // yeniden tarama iş parçacığı aynı store'u listeler
    std::string& clipboard;   // oturumdan uzun yaşar: bir nottan kopyalanan diğerine yapıştırılır
    EditorBuffer::CellWidth cell_width;
    std::uintmax_t max_bytes;  // bundan büyük not salt-okunur açılır (Tuning::max_note_bytes)
    std::size_t slug_max_length;
    std::function<std::string(std::string_view content)> suggest_type;  // ilk kayıtta tip (detect)
    std::function<void(const core::NotePath&)> on_saved;  // diske yazıldı: liste, indeks, senkron
    std::function<void()> on_opened;                      // odak editöre geçti
    // Odak çıktı: satır varsa Önizleme oradan sürer; yoksa (taslak, silinen not) Liste.
    std::function<void(std::optional<std::size_t> preview_line)> on_closed;
    std::function<void(const core::Error&)> on_error;  // not açılamadı
    std::function<void()> open_settings;               // F2 ile çıkış tamamlandı
    // Sistem panosu; boşsa yalnız iç pano. Kopyalama kullanıcıya gösterilecek metni döner, yapıştırma iç
    // panoyu (argüman) yedek alır.
    std::function<std::string(std::string_view text)> system_copy = {};
    std::function<PasteChoice(const std::string& internal)> system_paste = {};
    // Diğer notların sözcükleri (WordTableBuilder); boşsa yalnız bu not önerilir.
    std::function<std::shared_ptr<const WordTable>()> word_table = {};
};

// Sağ paneldeki gömülü editörün saf modeli: tuş yönlendirme, kaydetme, çakışma ve dış değişiklik kararları.
// FTXUI'ye bağlı değildir; görünümü editor_view.cpp çizer.
class EditorSession {
public:
    enum class Mode { Editing, Find, LeavePrompt, ConflictPrompt, NamePrompt, Complete };
    enum class ReadOnly { No, Binary, InvalidUtf8, EmbeddedNul, TooLarge };
    enum class LeaveChoice { Save, Discard, Cancel };
    enum class ConflictChoice { SaveCopy, LoadDisk, Overwrite, Cancel };

    explicit EditorSession(EditorHost host) : host_(std::move(host)) {}

    // Notu açar, imleç ve pencere `start_line`da; başarıda on_opened, hatada on_error.
    void open(const core::NotePath& path, std::size_t start_line = 0);
    // Boş taslak açar. İlk kayda dek dosya yoktur; ad ilk Ctrl+S'te alt satırda sorulur.
    void begin_new_note(std::string default_folder);
    // Doğrudan taslak (nvALT: eşleşme yoksa sorgu ilk satır); ad ilk kayıtta içerikten gelir.
    void begin_draft(std::string first_text, std::string folder);
    void handle(Key key, std::string_view text = {});
    // Çizimden önce çağrılır: boyut değişse de imleç görünür kalır, PgUp/PgDn adımı buradan gelir. `columns`
    // panelin iç genişliğidir; numara sütunu buradan düşülür.
    void layout(int columns, int rows);
    // Yeniden tarama sonucu: temiz tampon yenilenir, kirli tampona asla dokunulmaz.
    void on_listing(std::span<const core::NoteInfo> notes);
    // Uygulama kapanırken: kirli metin normal yoldan yazılır, olmazsa kopyaya ya da geçici dosyaya.
    // Kullanıcıya söylenecek metni döner; yapılacak bir şey yoksa boş.
    std::string flush_on_exit();

    bool is_open() const { return buffer_.has_value(); }
    Mode mode() const { return mode_; }
    ReadOnly read_only() const { return read_only_; }
    bool dirty() const { return buffer_ && buffer_->dirty(); }
    const EditorBuffer* buffer() const { return buffer_ ? &*buffer_ : nullptr; }
    const std::optional<core::NotePath>& path() const { return path_; }
    bool is_draft() const { return draft_.has_value(); }
    // Sorularda gösterilen ad: notun yolu, taslakta verilen ad, adsız taslakta katalog metni.
    std::string display_name() const;
    const std::string& name_input() const { return name_input_; }
    const std::string& name_folder() const { return name_folder_; }
    std::span<const ByteRange> find_matches() const { return find_matches_; }
    std::string find_label() const;
    const std::string& message() const { return message_; }
    bool message_is_error() const { return message_is_error_; }
    bool deleted_on_disk() const { return deleted_on_disk_; }
    bool changed_on_disk() const { return changed_on_disk_; }
    const std::string& find_query() const { return find_query_; }
    LeaveChoice leave_choice() const { return leave_choice_; }
    ConflictChoice conflict_choice() const { return conflict_choice_; }
    SyntaxLanguage syntax_language() const { return language_; }
    // Görünen tip: uzantı, kaydedilmemiş taslakta canlı detect.
    const std::string& shown_type() const { return shown_type_; }
    // Çizimde yalnız görünen satır için; eşik üstünde ya da bilinmeyen tipte boş.
    std::vector<SyntaxSpan> syntax_spans(std::size_t line) const;
    // Satır numarası sütunu (numara + boşluk), satır sayısıyla büyür; metin değildir, `columns()` hariç
    // tutar.
    int gutter_width() const;
    // Panel içi hücre (numara sütunu dahil): imleç oraya gider. Sütun numara üstündeyse satır başı.
    void click_at(int x, int y);
    bool has_bom() const { return bom_; }
    // Gölge öneri: imleçten sonra eklenecek metin; boşsa yok.
    const std::string& ghost() const { return ghost_; }
    const std::string& completion_query() const { return completion_query_; }
    const std::vector<std::string>& completion_items() const { return completion_items_; }
    std::size_t completion_selected() const { return completion_selected_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }

private:
    enum class AfterLeave { List, Settings };
    enum class SaveResult { Saved, Pending, Failed };

    void handle_editing(Key key, std::string_view text);
    void handle_find(Key key, std::string_view text);
    void handle_leave(Key key);
    void handle_conflict(Key key);
    bool apply_motion(Key key);
    void apply_edit(Key key, std::string_view text);
    void update_ghost();
    std::shared_ptr<const WordTable> current_word_table() const;
    void insert_completion();
    void open_completion();
    void refresh_completion();
    void handle_complete(Key key, std::string_view text);
    void copy_to_clipboard(std::string text);
    void paste_from_clipboard();
    void apply_content(std::string raw, const core::Revision& revision);
    void request_leave(AfterLeave after);
    void finish_pending_leave();
    void close();
    void cancel_pending_leave();
    void handle_name(Key key, std::string_view text);
    void start_draft(DraftName name);
    void update_find();
    void step_find(int delta);
    void recompute_find(std::size_t from);
    void select_current_match();
    void show_error(const core::Error& error);
    void show_info(std::string text);

    SaveResult save();
    SaveResult save_as_new_note(std::string_view name_suffix, const char* message_key);
    std::expected<core::NotePath, core::Error> create_note(std::string_view name_suffix);
    std::expected<core::NotePath, core::Error> create_draft();
    std::expected<core::NotePath, core::Error> create_and_adopt(core::NewNote note);
    std::expected<core::NotePath, core::Error> flush_write();
    SaveResult save_draft();
    SaveResult open_name_prompt();
    void submit_name();
    std::string name_stem(const std::string& type) const;
    std::string note_type() const;
    void refresh_type();
    void note_draft_edit(std::size_t size_before);
    void adopt_saved(const core::Revision& revision);
    bool overwrite_disk();
    bool load_disk();
    std::string body() const;

    EditorHost host_;
    std::optional<EditorBuffer> buffer_;
    std::optional<core::NotePath> path_;
    std::optional<DraftName> draft_;  // doluysa dosya henüz yok: ilk kayıt oluşturur
    std::string name_input_;
    std::string name_folder_;
    core::Revision revision_{};  // write()'ın beklediği, içerikle aynı anda okunan değer
    bool bom_ = false;           // tamponda görünmez, kayıtta geri konur
    ReadOnly read_only_ = ReadOnly::No;
    Mode mode_ = Mode::Editing;
    AfterLeave after_leave_ = AfterLeave::List;
    bool leave_pending_ = false;  // Kaydet seçildi, çakışma diyaloğu sonucu bekleniyor
    LeaveChoice leave_choice_ = LeaveChoice::Save;
    ConflictChoice conflict_choice_ = ConflictChoice::SaveCopy;
    std::string ghost_;
    std::vector<std::string> completion_candidates_;
    std::vector<std::string> completion_items_;
    std::string completion_query_;
    std::size_t completion_selected_ = 0;
    std::string find_query_;
    std::size_t find_origin_ = 0;
    std::vector<ByteRange> find_matches_;
    std::size_t find_index_ = 0;
    std::string message_;
    bool message_is_error_ = false;
    bool deleted_on_disk_ = false;
    bool changed_on_disk_ = false;
    std::string shown_type_;
    SyntaxLanguage language_ = SyntaxLanguage::Plain;
    mutable SyntaxStateCache syntax_cache_;  // çizimde tembel dolar, tampon gibi görünmez önbellek
    std::size_t draft_changes_ = 0;          // son detect'ten beri değişen bayt
    int columns_ = 80;
    int rows_ = 24;
};

}  // namespace noteapp::tui
