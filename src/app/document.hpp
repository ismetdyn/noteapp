#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::app {

// Çakışmada "kopya olarak kaydet" adının eki (`alpha` -> `alpha (copy)`): iki arayüzde aynı ad çıksın.
inline constexpr std::string_view kCopySuffix = " (copy)";

// Notun düzenlenemez olma nedeni (SPEC-tui "Salt okunur"). İki arayüzde aynı kural: salt-okunur not hiçbir
// yoldan yazılmaz.
enum class ReadOnly { No, Binary, InvalidUtf8, EmbeddedNul, TooLarge };

// `max_bytes` Tuning::max_note_bytes.cap()'tir (0 = sınırsız orada çözülür); sınır dahildir.
ReadOnly classify(std::string_view raw, std::uintmax_t max_bytes);
// Durumda gösterilen neden; No'da boş.
std::string read_only_reason(ReadOnly reason, std::uintmax_t max_bytes);
// Yalnız gösterim için: geçersiz bayt ve NUL U+FFFD olur. İmleç ve sarma hesabı geçerli UTF-8 ister.
std::string sanitized_utf8(std::string_view raw);

// Kayıt ModifiedExternally döndüğünde kullanıcının seçimi. Sıra TUI'deki gezinme sırasıdır; varsayılan kopya
// (veri kaybettirmeyen seçenek).
enum class ConflictChoice { SaveCopy, Reload, Overwrite, Cancel };
inline constexpr ConflictChoice kDefaultConflictChoice = ConflictChoice::SaveCopy;

// Açık notun diskteki hali değişti (yeniden tarama, senkron, otomatik kayıt): ne yapılacağı.
enum class DiskChange {
    None,     // değişiklik yok
    Reload,   // temiz tampon: diskteki sürüm sessizce yüklenir
    Changed,  // kirli tampon, disk değişti: tampona dokunulmaz, açık kayıt sorar
    Deleted,  // kirli tampon, not silindi: metin kalır, açık kayıt yeni not yazar
    Close,    // temiz tampon, not silindi: editör kapanır
};
// `disk` notun diskteki revizyonu; nullopt = not yok.
DiskChange disk_change(const std::optional<core::Revision>& disk, const core::Revision& mine, bool dirty);

// Yeni notun klasörü (D15), sırayla: vurgulu klasör satırı, girilen klasör (TUI klasör görünümü, GUI
// konumu), seçili ya da gösterilen notun klasörü, config `default_folder`. TUI kuralıdır; GUI'nin vurgulu
// klasör satırı daha bilgilendirici olduğu için başa eklendi.
struct FolderContext {
    const std::string* selected_folder = nullptr;  // vurgulu klasör satırı
    std::string_view scope_folder;                 // "" = kök ya da düz liste
    const core::NotePath* note = nullptr;          // seçili ya da gösterilen not
    std::string_view default_folder;
};
std::string new_note_folder(const FolderContext& context);

// Bir taslağın ilk kayıtta alacağı yer ve ad.
struct DraftName {
    std::string folder;
    std::string stem;  // boşsa ad içerikten (NoteStore::create kuralı) ya da zaman damgasından gelir
    std::string type;  // boşsa ilk kayıtta detect
    bool timestamp_name = false;  // ad istemi boş bırakıldı: zaman damgalı ad
    bool ask_name = false;        // `n` taslağı: ilk kayıt adı alt satırda sorar
};

// Yazılan adı çözer: `klasör/ad.uzantı`; klasör yoksa `default_folder`, ad slug kuralından geçer, açık uzantı
// kazanır (core::split_typed_name). Geçersiz yol hata döner (dosya oluşturulmaz).
std::expected<DraftName, core::Error> parse_draft_name(std::string_view input,
                                                       std::string_view default_folder,
                                                       std::chrono::system_clock::time_point now,
                                                       std::size_t slug_max_length);
// İlk kayıtta önerilen ad gövdesi: içerikten slug (sır tiplerinde yalnız anahtar, core::note_name_source).
std::string suggested_stem(std::string_view content, std::string_view type,
                           std::chrono::system_clock::time_point now, std::size_t slug_max_length);

struct DocumentOptions {
    // classify'ın sınırı; varsayılan sınırsız (Tuning::max_note_bytes = 0 ile aynı).
    std::uintmax_t max_bytes = std::numeric_limits<std::uintmax_t>::max();
    // Editör yalnız LF tutar (ImGui CR'yi düzenleyemez): baştan sona CRLF olan dosya LF gösterilir ve CRLF
    // yazılır. Karışık satır sonlu dosya olduğu gibi kalır; baytlar her iki durumda aynen geri yazılır.
    bool lf_editor = false;
};

// Açık notun ya da taslağın belge durumu: dosya baytları ile editör metni arasındaki dönüşüm (BOM, satır
// sonu, salt-okunur neden), revizyon, dış değişiklik bayrakları ve kayıt/çakışma işlemleri. Metnin kendisi
// arayüzün tamponundadır (TUI EditorBuffer, GUI ImGui dizgisi); burada kopyası tutulmaz. Kirli durumu da
// tampon bilir (TUI geri alarak diskle aynı metne dönmeyi temiz sayar).
// Depo kilidi çağıranın işidir (TUI yeniden tarama iş parçacığıyla paylaşır). Kopyalanabilir: GUI sekmeleri
// saklar.
class Document {
public:
    Document() = default;
    explicit Document(DocumentOptions options) : options_(options) {}

    // Notu okur; başarıda belge o nota geçer ve editörün metnini döner. Hatada belge değişmez.
    std::expected<std::string, core::Error> open(const core::NoteStore& store, const core::NotePath& path);
    // Okunmuş içeriği (aynı notun) belgeye alır: revizyon, bayraklar, editörün metni.
    std::string load(core::NoteContent content);
    // Dosyası olmayan taslağa döner.
    void reset_draft();

    const std::optional<core::NotePath>& path() const { return path_; }
    bool is_draft() const { return !path_.has_value(); }
    ReadOnly read_only() const { return read_only_; }
    bool writable() const { return read_only_ == ReadOnly::No; }
    std::string read_only_reason() const { return app::read_only_reason(read_only_, options_.max_bytes); }
    bool has_bom() const { return bom_; }
    const core::Revision& revision() const { return revision_; }
    bool changed_on_disk() const { return changed_on_disk_; }
    bool deleted_on_disk() const { return deleted_on_disk_; }
    // Otomatik kayıt yalnız diskle uyumlu bilinen, yazılabilir kayıtlı notta: soru sormaz, not yaratmaz.
    bool autosaves() const { return path_ && writable() && !changed_on_disk_ && !deleted_on_disk_; }

    // Editör metni -> dosya baytları: BOM geri konur, LF'ye çevrilmiş CRLF geri açılır.
    std::string encode(std::string_view text) const;

    enum class Saved { Written, Conflict, Gone };
    // Yalnız yazılabilir kayıtlı notta çağrılır (ihlal bug'dır: durur). Conflict ve Gone'da hiçbir şey
    // yazılmaz; karar arayüzün (ConflictChoice, yeni not).
    std::expected<Saved, core::Error> save(core::NoteStore& store, std::string_view text);
    // Kullanıcı bilerek ezer: diskteki güncel revizyon okunur, metin yazılır.
    std::expected<void, core::Error> overwrite(core::NoteStore& store, std::string_view text);
    // Diskteki sürümü yükler; editörün yeni metnini döner.
    std::expected<std::string, core::Error> reload(const core::NoteStore& store);
    // Notun yanına yeni not `ad<suffix>.uzantı` (dolu adda -2), baytlar aynen; belge yeni nota geçer.
    std::expected<core::NotePath, core::Error> create_beside(core::NoteStore& store, std::string_view text,
                                                             std::string_view suffix);
    // Taslağın (ya da silinmiş notun) kaydı: NoteStore::create_exact, baytlar aynen; belge yeni nota geçer.
    std::expected<core::NotePath, core::Error> create(core::NoteStore& store, core::NewNote note,
                                                      std::string_view text);
    // Not taşındı (ad/klasör/tip): içerik aynı, yol ve revizyon yeni.
    void moved(core::NotePath to, const core::Revision& revision);
    // Yeniden taramanın ya da otomatik kaydın bulduğu dış değişikliği bayraklara yazar (Changed, Deleted).
    void note_disk_change(DiskChange change);

private:
    void adopt(core::NotePath path, const core::Revision& revision);

    DocumentOptions options_;
    std::optional<core::NotePath> path_;
    core::Revision revision_{};  // write()'ın beklediği, içerikle aynı anda okunan değer
    ReadOnly read_only_ = ReadOnly::No;
    bool bom_ = false;            // tamponda görünmez, kayıtta geri konur
    bool crlf_shown_lf_ = false;  // lf_editor: dosya baştan sona CRLF, editörde LF
    bool changed_on_disk_ = false;
    bool deleted_on_disk_ = false;
};

// Otomatik kayıt zamanlaması (`tuning.autosave_ms`): her düzenleme süreyi yeniden başlatır, ardışık tuşlar
// tek yazmaya birleşir. Hangi notun yazılacağı Document::autosaves()'ın kararıdır.
class Autosave {
public:
    using Clock = std::chrono::steady_clock;
    explicit Autosave(std::chrono::milliseconds delay) { set_delay(delay); }

    // Ayar değişince: bekleyen süre aynen kalır, sonraki düzenleme yeni gecikmeyi alır.
    void set_delay(std::chrono::milliseconds delay) {
        delay_ = std::max(delay, std::chrono::milliseconds(1));
    }

    void edited(Clock::time_point now) { deadline_ = now + delay_; }
    void cancel() { deadline_.reset(); }
    bool due(Clock::time_point now) const { return deadline_ && now >= *deadline_; }
    const std::optional<Clock::time_point>& deadline() const { return deadline_; }

private:
    std::chrono::milliseconds delay_{};
    std::optional<Clock::time_point> deadline_;
};

}  // namespace noteapp::app
