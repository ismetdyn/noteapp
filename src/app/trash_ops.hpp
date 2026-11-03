#pragma once

#include "app/listing.hpp"
#include "core/error.hpp"
#include "core/limits.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Toplu işlemin öğe öğe sonucu: biri düşerse kalanlar sürer. Metni arayüz kurar (terminal/ImGui güvenliği
// onun işi); burada yalnız yol, hata ve yeni ad durur, oluş sırasıyla.
struct BatchReport {
    struct Line {
        std::string path;
        std::optional<core::Error> error = std::nullopt;  // dolu: öğe başarısız
        std::string renamed_to = {};                      // dolu: öğe başka adla geri geldi
    };
    std::size_t done = 0;
    std::size_t failed = 0;
    std::vector<Line> lines = {};
    bool wrote = false;  // öğe tamamlanmasa da depo değişti: senkron haber almalı

    void fail(std::string path, core::Error error);
    void renamed(std::string from, std::string to);
};

// Onay isteyen işlem ve metnin sayıları; metni arayüz kurar.
enum class ConfirmKind {
    TrashFolder,   // notlu klasör çöpe (notsuz klasör sorulmaz: geri alma klasörü getirir)
    TrashBatch,    // seçimde notlu klasör var
    Delete,        // canlı not kalıcı
    Purge,         // çöpteki not kalıcı
    DeleteFolder,  // klasör ve içi kalıcı
    DeleteBatch,   // seçim kalıcı (çöp görünümünde çöpten)
    EmptyTrash,
};

struct Confirm {
    ConfirmKind kind;
    std::string path = {};    // tek öğe ya da klasör
    std::size_t items = 0;    // toplu: öğe; çöp boşaltma: çöpteki not
    std::size_t notes = 0;    // klasör/toplu: alt ağaçtaki not
    std::size_t folders = 0;  // klasör: alt klasör (kendisi hariç)
};

// Onay kararı. Kalıcı silme ve notlu klasörün çöpe atılması hep sorulur; varsayılan Vazgeç: bağlanmamış
// istem hiçbir şeyi silmez, çöpe atmaz.
struct Prompts {
    std::function<bool(const Confirm&)> confirm = [](const Confirm&) { return false; };
};

struct TrashTarget {
    core::NotePath path;
    bool is_folder = false;
};

// Klasörün çöpe atılması: notlar tek tek atılır, sonra boşalan klasör kaldırılır.
struct FolderTrashed {
    std::size_t notes = 0;                              // atılan not
    std::optional<core::Error> failure = std::nullopt;  // ilk hata; klasör ve kalan not yerinde
};

// Geri alma adımının sonucu.
struct Undone {
    BatchReport report = {};
    bool single = false;                                 // tek notluk adım: hata varsa report'un tek satırı
    std::string folder = {};                             // klasör silmesiydi: seçilecek satır
    std::optional<core::NotePath> first = std::nullopt;  // ilk geri gelen not (yeni adıyla)
};

// Çöpe atma, geri alma yığını, geri getirme ve kalıcı silme: TUI ve GUI aynı kuralı buradan çağırır. Çöp not
// başınadır: klasör silmesi notları atıp klasörü kaldıran tek adımdır, geri alma adımın tamamını (boş alt
// klasörler dahil) getirir. Yığın oturumluktur, derinliği `limits.app.trash_undo_steps` (0 = sınırsız);
// dolunca en eski adım düşer. Depo kilidi çağıranın işidir (TUI yeniden tarama iş parçacığıyla paylaşır).
// Klasörün içeriği ve onaydaki sayılar arayüzün `Listing`inden: kullanıcıya ne gösterildiyse o sorulur ve
// atılır ya da kalıcı silinir, fazlası değil. Liste eskiyse (senkron, başka süreç) eksik not öğe hatası olur;
// klasör ancak boşalınca kalkar, gösterilmeyen not klasörde kalır (kalıcı silmede NotEmpty, yenile ipucuyla).
// Geri alma öğeyi yol ve düğüm kimliğiyle (TrashItem::id) tanır: çöpte aynı yola düşen
// başka not (senkron) geri getirilmez.
class TrashOps {
public:
    explicit TrashOps(core::Limit undo_steps = {}) : undo_steps_(std::move(undo_steps)) {}

    // Tek not, onaysız (geri alınır).
    std::expected<core::TrashItem, core::Error> trash_note(core::NoteStore& store,
                                                           const core::NotePath& path);
    // nullopt = vazgeçildi. Not yoksa sorulmaz.
    std::optional<FolderTrashed> trash_folder(core::NoteStore& store, const Listing& listing,
                                              std::string_view folder, const Prompts& prompts);
    // Seçim: her öğe denenir, atılanlar tek adımda geri gelir. Notlu klasör varsa sorulur; nullopt =
    // vazgeçildi.
    std::optional<BatchReport> trash_items(core::NoteStore& store, const Listing& listing,
                                           std::span<const TrashTarget> items, const Prompts& prompts);

    bool can_undo() const { return !steps_.empty(); }
    std::size_t undo_depth() const { return steps_.size(); }
    // Son adım. nullopt = geri alınacak bir şey yok. Düşen öğe adımda kalır (kilit kalkınca yeniden denenir);
    // çöpte artık olmayan öğe (NotFound) adımı kilitlemesin diye düşer, öğesi kalmayan adım yığından çıkar.
    std::optional<Undone> undo(core::NoteStore& store);

    // Çöp görünümünden geri getirme: öğe yığından da düşer.
    std::expected<core::NotePath, core::Error> restore(core::NoteStore& store, const core::NotePath& item);
    BatchReport restore_items(core::NoteStore& store, std::span<const core::NotePath> items);

    // Kalıcı silme: hep sorulur. false = vazgeçildi.
    std::expected<bool, core::Error> remove(core::NoteStore& store, const core::NotePath& path,
                                            const Prompts& prompts);
    std::expected<bool, core::Error> purge(core::NoteStore& store, const core::NotePath& item,
                                           const Prompts& prompts);
    std::expected<bool, core::Error> remove_folder(core::NoteStore& store, const Listing& listing,
                                                   std::string_view folder, const Prompts& prompts);
    // `from_trash`: öğeler çöp öğeleridir (purge). nullopt = vazgeçildi.
    std::optional<BatchReport> delete_items(core::NoteStore& store, const Listing& listing,
                                            std::span<const TrashTarget> items, bool from_trash,
                                            const Prompts& prompts);
    std::expected<bool, core::Error> empty_trash(core::NoteStore& store, const Prompts& prompts);

private:
    struct Step {
        std::vector<core::TrashItem> notes = {};  // çöp öğeleri (yol + kimlik), atılma sırasıyla
        std::vector<std::string> folders = {};    // geri alınca yeniden kurulacak klasörler, ata önce
        std::string folder = {};                  // klasör silmesi: geri alınca seçilecek satır
    };
    void push(Step step);
    // Kalıcı silinen çöp öğesi geri alınamaz: adımdan düşer. Klasör adımı klasörleri için kalır.
    void forget(const core::NotePath& item);
    std::optional<core::Error> trash_folder_into(core::NoteStore& store, const Listing& listing, Step& step,
                                                 const std::string& folder);
    Undone undo_batch(core::NoteStore& store);
    // Atılma anı tutmayan öğe artık bu adımın notu değildir: NotFound.
    static std::expected<core::NotePath, core::Error> restore_entry(core::NoteStore& store,
                                                                    const core::TrashItem& entry);
    // Kullanıcının gördüğü notları kalıcı siler, sonra boşalan klasörü kaldırır.
    static std::expected<void, core::Error>
    remove_shown_folder(core::NoteStore& store, const Listing& listing, const std::string& folder);

    core::Limit undo_steps_;
    std::deque<Step> steps_;
};

}  // namespace noteapp::app
