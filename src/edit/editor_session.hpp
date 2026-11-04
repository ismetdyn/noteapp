#pragma once

#include "app/document.hpp"
#include "core/error.hpp"
#include "core/lsp_protocol.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "edit/clipboard.hpp"
#include "edit/completion.hpp"
#include "edit/editor_buffer.hpp"
#include "edit/input_line.hpp"
#include "edit/note_find.hpp"
#include "edit/syntax.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Taslak adı ve gösterim temizliği arayüzden bağımsızdır (app/document): eski adlar korunur.
using app::DraftName;
using app::parse_draft_name;
using app::sanitized_utf8;

// Dil sunucusunun tanısı, tampondaki bayt aralığıyla; ileti sunucudan gelir ve ham kalır (çizim temizler).
// `severity` hiçbir zaman Unspecified değildir.
struct DiagnosticMark {
    ByteRange range;
    core::lsp::DiagnosticSeverity severity = core::lsp::DiagnosticSeverity::Error;
    std::string message;
};

// Oturumun dış dünyası; testler sahte geri çağrılarla kurar, main gerçek olanlarla.
struct EditorHost {
    core::NoteStore& store;
    // cppcheck-suppress uninitMemberVarNoCtor ; referans üye, ilk atama derlemede zorunlu
    std::mutex& store_mutex;  // yeniden tarama iş parçacığı aynı store'u listeler
    // cppcheck-suppress uninitMemberVarNoCtor ; referans üye, ilk atama derlemede zorunlu
    std::string& clipboard;  // oturumdan uzun yaşar: bir nottan kopyalanan diğerine yapıştırılır
    EditorBuffer::CellWidth cell_width{};
    std::uintmax_t max_bytes{};  // bundan büyük not salt-okunur açılır (Tuning::max_note_bytes)
    std::size_t slug_max_length{};
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
    // Son düzenlemeden bu kadar sonra kayıtlı not kendiliğinden yazılır (`tuning.autosave_ms`); boşsa yok.
    std::optional<std::chrono::milliseconds> autosave_delay = std::nullopt;
    // limits.tui.* ve tuning.completion_min_prefix_bytes; sınırsız SIZE_MAX.
    SyntaxLimits syntax = {};
    std::size_t undo_limit_bytes = EditorBuffer::kDefaultUndoLimitBytes;
    CompletionLimits completion = {};
};

// Sağ paneldeki gömülü editörün saf modeli: tuş yönlendirme, kaydetme, çakışma ve dış değişiklik kararları.
// FTXUI'ye bağlı değildir; görünümü editor_view.cpp çizer.
class EditorSession {
public:
    enum class Mode { Editing, Find, LeavePrompt, ConflictPrompt, NamePrompt, Complete };
    using ReadOnly = app::ReadOnly;
    enum class LeaveChoice { Save, Discard, Cancel };
    using ConflictChoice = app::ConflictChoice;

    explicit EditorSession(EditorHost host)
        : host_(std::move(host)), doc_(app::DocumentOptions{.max_bytes = host_.max_bytes}) {
        if (host_.autosave_delay)
            autosave_.emplace(*host_.autosave_delay);
    }

    // Notu açar, imleç ve pencere `start_line`da; başarıda on_opened, hatada on_error.
    void open(const core::NotePath& path, std::size_t start_line = 0);
    // Boş taslak açar. İlk kayda dek dosya yoktur; ad ilk Ctrl+S'te alt satırda sorulur.
    void begin_new_note(std::string default_folder);
    // Doğrudan taslak (nvALT: eşleşme yoksa sorgu ilk satır); ad ilk kayıtta içerikten gelir.
    void begin_draft(std::string_view first_text, std::string folder);
    void handle(Key key, std::string_view text = {}, const SharedInputSources& sources = {});
    // Çizimden önce çağrılır: boyut değişse de imleç görünür kalır, PgUp/PgDn adımı buradan gelir. `columns`
    // panelin iç genişliğidir; numara sütunu buradan düşülür.
    void layout(int columns, int rows);
    // Yeniden tarama sonucu: temiz tampon yenilenir, kirli tampona asla dokunulmaz.
    void on_listing(std::span<const core::NoteInfo> notes);
    // Notu ya da klasörü taşındı (TUI sekmeleri, liste eylemi): belge yeni yolu izler, sonraki kayıt oraya
    // gider (eski yola giden kayıt Gone olurdu ya da orada sonradan açılan nota çakışırdı). Taşıma gövde
    // revizyonunu değiştirmez. Taslakta etkisiz.
    void follow_move(const core::NotePath& to) {
        if (doc_.path())
            doc_.moved(to, doc_.revision());
    }
    // Uygulama kapanırken: kirli metin normal yoldan yazılır, olmazsa kopyaya ya da geçici dosyaya.
    // Kullanıcıya söylenecek metni döner; yapılacak bir şey yoksa boş.
    std::string flush_on_exit();
    // Otomatik kayıt: süre dolduysa ve düzenleme kipindeyse kirli kayıtlı not yazılır. Soru açmaz, not
    // yaratmaz: çakışma ya da silinmiş not bayrak olur (durum çubuğu), karar açık kayıtta (Ctrl+S, çıkış).
    // Bir iş yaptıysa true (ekran yenilenmeli).
    bool tick(std::chrono::steady_clock::time_point now);
    // Bekleyen otomatik kaydın anı; yoksa boş.
    std::optional<std::chrono::steady_clock::time_point> autosave_deadline() const {
        return autosave_ ? autosave_->deadline() : std::nullopt;
    }

    bool is_open() const { return buffer_.has_value(); }
    Mode mode() const { return mode_; }
    ReadOnly read_only() const { return doc_.read_only(); }
    bool dirty() const { return buffer_ && buffer_->dirty(); }
    const EditorBuffer* buffer() const { return buffer_ ? &*buffer_ : nullptr; }
    // Her yeni tamponda artar: yeni tamponun edit_count'u yine 0'dan başlar, (yol, edit_count) tek başına
    // aynı metni göstermez.
    std::uint64_t buffer_generation() const { return buffer_generation_; }
    const std::optional<core::NotePath>& path() const { return doc_.path(); }
    bool is_draft() const { return draft_.has_value(); }
    // Editörden kapatmadan ayrılmak (sekme değişimi, Ctrl+K): bekleyen düzenleme kendi notuna yazılır (GUI
    // flush_for), taslak kaydedilmeden kalır. Kayıt olmadıysa (çakışma sorusu, hata) ya da soru/girdi açıksa
    // false: ayrılma yok.
    bool park();
    // Sorularda gösterilen ad: notun yolu, taslakta verilen ad, adsız taslakta katalog metni.
    std::string display_name() const;
    const std::string& name_input() const { return name_line_.text(); }
    const std::string& name_folder() const { return name_folder_; }
    std::span<const ByteRange> find_matches() const { return find_matches_; }
    std::string find_label() const;
    const std::string& message() const { return message_; }
    bool message_is_error() const { return message_is_error_; }
    bool deleted_on_disk() const { return doc_.deleted_on_disk(); }
    bool changed_on_disk() const { return doc_.changed_on_disk(); }
    const std::string& find_query() const { return find_line_.text(); }
    const InputLine& find_line() const { return find_line_; }
    const InputLine& name_line() const { return name_line_; }
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
    bool has_bom() const { return doc_.has_bom(); }
    // Gölge öneri: imleçten sonra eklenecek metin; boşsa yok.
    const std::string& ghost() const { return ghost_; }
    const std::string& completion_query() const { return completion_query_; }
    const std::vector<std::string>& completion_items() const { return completion_items_; }
    std::size_t completion_selected() const { return completion_selected_; }
    // Her açılışta artar; dil sunucusu cevabı yalnız aynı açılışa eklenir.
    std::uint64_t completion_generation() const { return completion_generation_; }
    // Dil sunucusunun önerileri açık listeye, sözcük önerilerinin önüne eklenir. Liste kapandıysa ya da
    // `generation` eskiyse yok sayılır. Etiket temizlenip gösterilir; ekleme metni yalnız tek satır ve
    // denetim karaktersizse kullanılır, değilse temiz etiket eklenir.
    void add_completion_items(std::uint64_t generation, std::span<const core::lsp::CompletionItem> items);
    // Dil sunucusunun güncel tanıları; aralıklar bu tampona göre kısılmış olmalıdır.
    void set_diagnostics(std::vector<DiagnosticMark> marks) { diagnostics_ = std::move(marks); }
    std::span<const DiagnosticMark> diagnostics() const { return diagnostics_; }
    // İmlecin üstündeki (ya da hemen sonundaki) tanı; yoksa nullptr.
    const DiagnosticMark* diagnostic_at_cursor() const;
    // Tek seferlik bilgi iletisi (ör. dil sunucusu durdu); sonraki tuş siler.
    void show_notice(std::string text) { show_info(std::move(text)); }
    int columns() const { return columns_; }
    int rows() const { return rows_; }

private:
    enum class AfterLeave { List, Settings };
    enum class SaveResult { Saved, Pending, Failed };

    void dispatch(Key key, std::string_view text, const SharedInputSources& sources);
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
    void apply_text(std::string text);
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
    std::expected<core::NotePath, core::Error>
    adopt_created(std::expected<core::NotePath, core::Error> created);
    std::expected<core::NotePath, core::Error> flush_write();
    SaveResult save_draft();
    SaveResult open_name_prompt();
    void submit_name();
    std::string name_stem(const std::string& type) const;
    std::string note_type() const;
    void refresh_type();
    void note_draft_edit(std::size_t size_before);
    void adopt_saved();
    void restart_autosave();
    bool overwrite_disk();
    bool load_disk();
    std::string body() const;
    // handle() ve açma yolları dışında çağrılmaz: tampon doluyken ya taslak ya yol vardır. İhlal bug'dır:
    // durur.
    auto& open_buffer(this auto& self) {
        if (!self.buffer_)
            std::abort();
        return *self.buffer_;
    }
    const core::NotePath& note_path() const {
        if (!doc_.path())
            std::abort();
        return *doc_.path();
    }

    EditorHost host_;
    // Yol, revizyon, BOM, salt-okunur neden ve dış değişiklik bayrakları; taslakta yolu yoktur.
    app::Document doc_;
    std::optional<app::Autosave> autosave_;
    std::optional<EditorBuffer> buffer_;
    std::optional<DraftName> draft_;  // doluysa dosya henüz yok: ilk kayıt oluşturur
    std::string name_folder_;
    Mode mode_ = Mode::Editing;
    AfterLeave after_leave_ = AfterLeave::List;
    bool leave_pending_ = false;  // Kaydet seçildi, çakışma diyaloğu sonucu bekleniyor
    LeaveChoice leave_choice_ = LeaveChoice::Save;
    ConflictChoice conflict_choice_ = app::kDefaultConflictChoice;
    std::string ghost_;
    std::vector<std::string> completion_candidates_;
    std::vector<std::string> completion_items_;
    std::string completion_query_;
    std::size_t completion_selected_ = 0;
    std::uint64_t completion_generation_ = 0;
    std::uint64_t buffer_generation_ = 0;
    std::map<std::string, std::string> completion_inserts_;  // dil sunucusu etiketi -> eklenecek metin
    std::vector<DiagnosticMark> diagnostics_;
    InputLine find_line_{InputKind::Find};
    InputLine name_line_{InputKind::Path};
    std::size_t find_origin_ = 0;
    std::vector<ByteRange> find_matches_;
    std::size_t find_index_ = 0;
    std::string message_;
    bool message_is_error_ = false;
    std::string shown_type_;
    SyntaxLanguage language_ = SyntaxLanguage::Plain;
    mutable SyntaxStateCache syntax_cache_;  // çizimde tembel dolar, tampon gibi görünmez önbellek
    app::DraftTypeTracker draft_type_;       // taslağın canlı detect'i ne zaman yeniden koşar
    int columns_ = 80;
    int rows_ = 24;
};

}  // namespace noteapp::tui
