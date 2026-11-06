#pragma once

// İç başlık (core ve note_sync). Uygulama şeması burada: motor nottan habersizdir (SPEC-graph ilke 5).
#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_props.hpp"
#include "core/note_store.hpp"
#include "core/text_merger.hpp"
#include "graph/graph.hpp"

#include <algorithm>
#include <array>
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::core {

// Motor hatasını core::Error'a çevirip yayar.
#define NOTEAPP_TRY_GRAPH(expr, path)                                                                        \
    do {                                                                                                     \
        if (auto noteapp_try_result = (expr); !noteapp_try_result)                                           \
            return std::unexpected(::noteapp::core::from_graph(noteapp_try_result.error(), (path)));         \
    } while (false)

inline constexpr std::string_view kNoteLabel = "Note";
inline constexpr std::string_view kFolderLabel = "Folder";
inline constexpr std::string_view kInFolder = "IN_FOLDER";
inline constexpr std::string_view kChildOf = "CHILD_OF";
inline constexpr std::string_view kLinksTo = "LINKS_TO";
inline constexpr std::string_view kRelatedTo = "RELATED_TO";
inline constexpr std::string_view kTagLabel = "Tag";
inline constexpr std::string_view kTagged = "TAGGED";
// Kaydedilmiş görünüm (core/note_views.hpp): (:View {name, query, created_at, updated_at}).
inline constexpr std::string_view kViewLabel = "View";
inline constexpr std::string_view kViewQuery = "query";
// Tag.name eşleştirme anahtarıdır (tag_key); gösterim etiketin ilk yazıldığı hali.
inline constexpr std::string_view kTagDisplay = "display";
inline constexpr std::string_view kName = "name";
// RELATED_TO kenarında yalnız true yazılır; yoksa yönsüz (SPEC-graph "Adlı ilişkiler").
inline constexpr std::string_view kDirected = "directed";
inline constexpr std::string_view kType = "type";
inline constexpr std::string_view kBody = "body";
inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kUpdatedAt = "updated_at";
inline constexpr std::string_view kNoteOrigin = "origin";
inline constexpr std::string_view kDeletedAt = "deleted_at";
// Silinen klasör ya da taşıma yolu değiştirse de çöp, atıldığı yolu bilir (geri yükleme oraya döner).
inline constexpr std::string_view kTrashedFrom = "trashed_from";
// Geçişin içe aktardığı baytların özeti: eski dosya bu tabanla aynıysa kopya üretmez (SPEC-graph "Geçiş").
inline constexpr std::string_view kMigratedSha = "migrated_sha";
// Gövde yazmasının gördüğü head'ler: (cihaz 16 bayt, seq 8 bayt LE) dizisi. Üç yönlü birleştirmenin tabanı
// buradan bulunur.
inline constexpr std::string_view kBodyParents = "body_parents";
// Gövdedeki [[hedef]]ler (scan_links), '\n' ile birleşik; bağlantısız notta yok. Gövdenin saf fonksiyonudur:
// her cihaz aynı değeri yazar, gidip gelmez. Sonradan oluşan hedefin kaynaklarını gövde taramadan bulur
// (SPEC-graph "Sonradan oluşan hedef").
inline constexpr std::string_view kLinkTargets = "link_targets";

#if defined(__ANDROID__)
inline constexpr std::string_view kOrigin = "android";
#elif defined(_WIN32)
inline constexpr std::string_view kOrigin = "windows";
#elif defined(__APPLE__)
inline constexpr std::string_view kOrigin = "macos";
#else
inline constexpr std::string_view kOrigin = "linux";
#endif
inline constexpr std::string_view kOriginUnknown = "unknown";

// Görüntünün çözdüğü en uzun klasör yolu (limits.core.folder_path_bytes; derinlik sınırı yok). Yazma aynı
// sınırı dayatır: yoksa kayıt başarılı döner, not görünmez kalırdı. Değer cihaza özgüdür: küçüğünü seçen
// cihaz uzun yoldaki notları unreachable raporlar, graf değişmez.
bool folder_path_within_limits(std::string_view folder, std::size_t max_bytes);

Error io_error(std::string_view path, std::string_view detail);
Error not_found(std::string_view path);
Error invalid_path(std::string_view reason, std::string_view path);
Error already_exists(std::string_view path);
Error from_graph(const graph::Error& error, std::string_view path);
std::string path_utf8(const std::filesystem::path& path);
std::string id_hex(const graph::NodeId& id);
inline std::span<const std::byte> as_bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}
// Kimliğin ham 16 baytı: türetilmiş kenar kimliklerinin girdisi.
inline std::string id_text(const graph::NodeId& id) {
    return std::string(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
}
std::expected<NotePath, Error> numbered_path(const NotePath& path, std::size_t number);

struct NoteRow {
    graph::NodeId id;
    NoteInfo info;  // info.path gösterilen yol: çakışmada -2 alır, saklanmaz
    std::filesystem::file_time_type trashed_at{};
    bool conflicted = false;   // gövdede birden çok head
    std::string link_targets;  // kLinkTargets değeri
};

// [[hedef]] metni -> canlı not. Yazma ve panel aynı kuralla çözer.
using LinkIndex = std::unordered_map<std::string, graph::NodeId>;

// Bir okumanın tüm türetilmiş yolları. Yazma ve okuma aynı kuralı kullansın diye tek yerde kurulur.
struct Snapshot {
    std::vector<NoteRow> live;
    std::vector<NoteRow> trash;
    std::vector<NoteInfo> infos;
    std::unordered_map<std::string, std::size_t> live_by_path;
    std::map<graph::NodeId, std::size_t> live_by_id;
    std::unordered_map<std::string, std::size_t> trash_by_path;
    // İlk çözümde kurulur (link_index_of), hiç bağlantısı olmayan kullanıcı bedel ödemez. Impl::mutex
    // altında.
    mutable std::optional<LinkIndex> link_index;
    std::vector<UnreachableNote> unreachable;
    std::map<std::string, std::vector<graph::NodeId>>
        folders;                 // aynı yolda eşzamanlı kurulmuş birden çok düğüm olabilir
    std::vector<TagCount> tags;  // canlı notu olan etiketler, ada göre
    // `view:` süzgecinin sonucu, görünüm anahtarı -> notlar ya da hata. İlk istekte kurulur (view_notes): tuş
    // başına süzme sorguyu yeniden çalıştırmaz, görüntü değişince düşer. Hata da saklanır: senkronla gelen
    // bozuk görünüm (süre sınırı, not döndürmeyen sonuç) her tuşta kilidi saniyelerce tutmasın. Impl::mutex
    // altında.
    mutable std::map<std::string, std::expected<std::vector<NotePath>, Error>> view_results;
    // relation_names() sonucu; ilk istekte kurulur, görüntüyle düşer (seçici her tuşta sorar). Impl::mutex
    // altında.
    mutable std::shared_ptr<const std::vector<RelationName>> relation_names;
    // shared_edges() sonucu; ilk istekte kurulur, görüntüyle düşer (arama her tuşta sorar). Impl::mutex
    // altında.
    mutable std::shared_ptr<const std::vector<NoteEdge>> edges;
    // notes_with_property() sonucu, anahtar başına; ilk istekte kurulur, görüntüyle düşer (`prop:` her tuşta
    // sorar). Impl::mutex altında.
    mutable std::map<std::string, std::shared_ptr<const PropertyNotes>> property_notes;
    mutable std::vector<std::string>
        property_order;  // property_notes'un anahtarları, en eski kullanılan önde
    std::size_t folder_path_bytes =
        kDefaultFolderPathBytes;                         // görüntünün kurulduğu sınır; yazma da bunu dayatır
    std::size_t name_bytes = kDefaultRelationNameBytes;  // okunan etiket adının sınırı (build_snapshot)
    bool path_taken(const std::string& path) const {
        return live_by_path.contains(path) || folders.contains(path);
    }
};

// name_bytes: okunan ilişki ve etiket adının sınırı (limits.graph.relation_name_bytes; sınırsız SIZE_MAX).
graph::Result<Snapshot> build_snapshot(graph::ReadTxn& txn, const std::array<std::byte, 16>& device,
                                       std::size_t folder_path_bytes, std::size_t name_bytes);
// Senkronla ya da Cypher'la gelmiş, yazma kuralından geçmemiş ad (ilişki, etiket): kırpılır, bozuk ya da
// yasak karakter (C0, DEL, C1, görünmez biçim) U+FFFD olur, max_bytes'ta kod noktası sınırında kesilir.
// Graf değişmez. Anahtar bu biçimden türer.
std::string shown_name(std::string_view raw, std::size_t max_bytes);
// Canlı notların NoteInfo::tags'ini ve snapshot.tags'i doldurur; live_by_id kurulmuş olmalı. Adlar
// shown_name'den geçer (snapshot.name_bytes): her tüketici temiz metin görür, `tag:` bu biçimle eşleşir.
graph::Result<void> load_tags(graph::ReadTxn& txn, Snapshot& snapshot);
// Notun TAGGED komşularının gösterim adları (shown_name), anahtar sırasıyla.
graph::Result<std::vector<std::string>> read_note_tags(graph::ReadTxn& txn, const graph::NodeId& id,
                                                       std::size_t max_name_bytes);

// link_target: core/note_store.hpp (arayüzler de kullanır).
// [[bağlantı]] hedefleri (link_target kuralıyla), sıralı ve tekil.
std::vector<std::string> scan_links(std::string_view body);
const LinkIndex& link_index_of(const Snapshot& snapshot);
// kLinkTargets'inde anahtarlardan biri olan notlar (canlı ve çöp). Bellekteki küçük metinler üzerinde
// doğrusal bakar, gövde okumaz.
std::set<graph::NodeId> notes_linking(const Snapshot& snapshot, std::span<const std::string> keys);
// Bir yolun çözüm anahtarları, öncelik sırasıyla: yol, uzantısız yol, dosya adı, yalın ad; anlamsız düzey
// boş.
std::array<std::string, 4> link_keys(std::string_view path);
std::vector<std::string> placed_keys(std::span<const std::string> paths);
// Canlı ya da çöpteki notun yolu; görüntüde yoksa boş.
std::optional<std::string> note_path_in(const Snapshot& snapshot, const graph::NodeId& id);

// Sorgu çubuğunun koruması (core/note_query.cpp SchemaGuard), tek değişiklik için: özellik denetçisi de aynı
// kancadan geçer, iki yol aynı anahtarları reddeder. Ret InvalidArgument'tır.
graph::Result<void> check_user_change(graph::ReadTxn& txn, const graph::QueryChange& change,
                                      std::size_t relation_name_bytes);
// Note düğümünün uygulamaya ait anahtarı mı (gövde, ad, tip, zamanlar...).
bool protected_note_key(std::string_view key);

std::expected<bool, Error> verify_import(graph::ReadTxn& txn, const ImportedNote& note);
// Geçiş notunun baytlarını tutabilecek düğümler (asıl ve kopya), UnreachableNote::id biçiminde.
std::array<std::string, 2> imported_note_ids(const ImportedNote& note);

// Açık bir yazma işleminde uygulama şemasının yazmaları.
class WriteContext {
public:
    WriteContext(graph::WriteTxn& txn_in, const Snapshot& snapshot_in) : txn(txn_in), snapshot(snapshot_in) {}

    // `created`: Note.created_at; updated_at `now`dır.
    std::expected<graph::NodeId, Error> create_note(const NotePath& path, std::string_view bytes,
                                                    std::chrono::system_clock::time_point now,
                                                    std::string_view origin,
                                                    std::chrono::system_clock::time_point created);
    std::expected<Revision, Error> body_revision(const graph::NodeId& id, std::string_view path);
    std::expected<Revision, Error> write_body(const NoteRow& note, std::string_view content,
                                              std::chrono::system_clock::time_point now);
    std::expected<void, Error> place_note(const graph::NodeId& id, const NotePath& target);
    std::expected<void, Error> delete_node(const graph::NodeId& id, std::string_view path);
    std::expected<void, Error> mark_trashed(const graph::NodeId& id, std::string_view path,
                                            std::chrono::system_clock::time_point now);
    std::expected<void, Error> clear_trashed(const graph::NodeId& id, std::string_view path);
    std::expected<std::optional<graph::NodeId>, Error> ensure_folder(std::string_view path);
    std::expected<void, Error> rename_folder(std::span<const graph::NodeId> ids, std::string_view target);
    // Bütçe dolana dek içe aktarır (en az bir not); consumed işlenen not sayısıdır, kalan yeni işleme kalır.
    std::expected<ImportOutcome, Error> import_notes(std::span<const ImportedNote> notes,
                                                     std::size_t& consumed);
    std::expected<void, Error> update_links(const graph::NodeId& id, std::string_view path,
                                            std::string_view body);
    // Yalnız ekler: gövdenin index'te çözülen ama kenarı olmayan hedeflerine kenar kurar, hiçbir kenarı
    // silmez. Türetilmiş eklemedir; iki cihaz aynı kenara varır, gidip gelmez.
    std::expected<void, Error> add_missing_links(const graph::NodeId& id, std::string_view path,
                                                 std::string_view body, const LinkIndex& index);
    // Not bu işlemde yeni bir yola geldi (oluşturma, taşıma, geri alma, klasör adı): commit'ten önce
    // link_appeared onu bağlayan kaynakların kenarlarını kurar.
    void note_placed(const graph::NodeId& id, std::string path) { placed_.emplace_back(id, std::move(path)); }
    // Yeni yola gelen notları [[metin]]le bağlayan notlara kenar ekler; çözüm işlem sonrası durumla yapılır.
    // write_locked commit'ten önce çağırır.
    std::expected<void, Error> link_appeared(const std::array<std::byte, 16>& device);
    // Gövdenin `#etiket`lerine göre TAGGED kenarlarını kurar; sahipsiz kalan Tag silinir. path tipi verir.
    std::expected<void, Error> update_tags(const graph::NodeId& id, std::string_view path,
                                           std::string_view body);
    // Notun canlı ya da çöpteki yolu; görüntüde yoksa boş (etiket tipi yoldan okunur).
    std::optional<std::string> path_of(const graph::NodeId& id) const;
    // Gövdede farklı değerli head'ler varsa kaybedenleri kopya nota çıkarır, gövdeyi kazananda birleştirir.
    // Kopyanın kimliği ve adı (note, kaybeden dot)'tan türer: iki cihaz aynı düğüme varır. Dönüş: yeni
    // kopyalar.
    std::expected<std::vector<NotePath>, Error> resolve_body_conflict(const graph::NodeId& id);
    // Aynı işi tek işlemlik adımlarla yapar: her kopya ayrı çerçevede. more true ise yeni işlemde yeniden
    // çağrılır.
    struct ConflictStep {
        std::optional<NotePath> copy;
        bool more = false;
    };
    std::expected<ConflictStep, Error> resolve_conflict_step(const graph::NodeId& id,
                                                             const TextMerger& merger = {});

    // Bütçe doldu mu: yeni iş birimi bu işlemde başlamaz.
    bool over_budget() const { return txn.pending_ops() >= budget; }

    graph::WriteTxn& txn;
    const Snapshot& snapshot;
    std::size_t budget = kDefaultGraphBatchOps;
    // İşlem başına gövde baytı (Impl::batch_bytes): sonraki not aşırıyorsa işlem kapanır.
    std::size_t batch_bytes = graph::GraphLimits{}.frame_bytes / 2;
    // İş bu işleme sığmadı: write_locked commit eder, gövdeyi yeni işlemde yeniden çağırır. Gövde yinelenince
    // kalan işi yapacak biçimde yazılır (yolu yeniden arar, yapılmışı atlar).
    bool again = false;
    // Türetilmiş veri bu işlemde tamamlanmadı: commit'ten önce türetme işaretleri düşer, süreç arada ölürse
    // sonraki açılış tamamlar (index_derived_once).
    bool derived_incomplete = false;
    // Bütçeye sığmayan, eksik kenarları commit'ten hemen sonra ayrı işlemlerde eklenecek notlar.
    std::vector<graph::NodeId> deferred;
    // Kendi bağlantı ya da etiket kenarları bu işleme sığmayan notlar (not başına tavan yok, L3): commit'ten
    // hemen sonra update_links + update_tags ayrı işlemlerde sürer. Sessiz atlama yoktur.
    std::vector<graph::NodeId> unfinished;

private:
    std::expected<graph::NodeId, Error> add_folder_node(const std::optional<graph::NodeId>& parent,
                                                        const std::string& name, std::string_view path);
    std::expected<void, Error> init_note(const graph::NodeId& id, const NotePath& path,
                                         std::string_view bytes, std::int64_t created_ms,
                                         std::int64_t updated_ms, std::string_view origin);
    std::expected<void, Error> set_folder(const graph::NodeId& id, const std::optional<graph::NodeId>& folder,
                                          std::string_view edge_type, std::string_view path);
    std::expected<void, Error> set_text_if_changed(const graph::NodeId& id, std::string_view key,
                                                   const std::string& value, std::string_view path);
    std::expected<void, Error> import_one(const ImportedNote& note, ImportOutcome& outcome);
    std::expected<void, Error> import_existing(const ImportedNote& note, const graph::NodeId& id,
                                               ImportOutcome& outcome);
    std::expected<NotePath, Error> conflict_copy_path(const graph::NodeId& id, const graph::NodeId& copy,
                                                      std::int64_t at);
    // Farklı head'lerin temiz üç yönlü birleşimi; taban yok, ikili, geçersiz UTF-8 ya da çakışma: boş.
    std::expected<std::optional<std::string>, Error>
    try_merge(const graph::NodeId& id, std::span<const graph::Version> versions, const TextMerger& merger);
    std::expected<std::optional<std::string>, Error>
    common_base(const graph::NodeId& id, std::span<const graph::Version* const> heads);
    std::expected<bool, Error> apply_merge(const graph::NodeId& id, std::span<const graph::Version> versions,
                                           const TextMerger& merger);
    std::expected<std::optional<NotePath>, Error> copy_losing_head(const graph::NodeId& id,
                                                                   const graph::Version& version);
    std::expected<void, Error> set_body_parents(const graph::NodeId& id, std::string_view path);
    std::expected<void, Error> ensure_tag(const graph::NodeId& tag, const std::string& key,
                                          const std::string& display, std::string_view path);
    std::expected<void, Error> remove_orphan_tag(const graph::NodeId& tag, std::string_view path);
    std::expected<std::vector<std::pair<graph::EdgeId, graph::NodeId>>, Error>
    neighbor_edges(const graph::NodeId& id, graph::Direction direction, std::optional<std::string_view> type,
                   std::string_view path);
    std::expected<bool, Error> is_stale_import(const graph::NodeId& id, const graph::Version& version);
    std::expected<std::int64_t, Error> version_time(const graph::NodeId& id, const graph::Version& version);

    std::expected<void, Error> set_link_targets(const graph::NodeId& id, std::string_view path,
                                                const std::vector<std::string>& targets);
    std::expected<void, Error> put_link(const graph::NodeId& from, const graph::NodeId& to,
                                        std::string_view path);

    // Bütçe doldu: notun kalan türetmesi sonraki işlemlere kalır.
    std::expected<void, Error> leave_unfinished(const graph::NodeId& id);
    void defer(const graph::NodeId& id);

    std::map<std::string, graph::NodeId> added_folders_;
    std::vector<std::pair<graph::NodeId, std::string>> placed_;
    std::set<std::string> written_targets_;  // bu işlemde kLinkTargets'i yazılan notların hedefleri
};

struct NoteStore::Impl {
    Impl(std::filesystem::path directory_in, std::size_t slug)
        : directory(std::move(directory_in)), slug_max_length(slug) {}

    std::filesystem::path database() const { return directory / "graph.db"; }
    // Veritabanı yoksa ya da açılamıyorsa boş (neden last_error'da). mutex tutulurken çağrılır.
    std::shared_ptr<const Snapshot> current();
    std::expected<void, Error> check_gate();
    std::expected<void, Error> open_for_write();
    // Tags: yalnız etiketler. Links: bağlantılar baştan ve etiketler (geçiş, yarım kalan kendi yazması).
    // MissingLinks: yalnız eksik bağlantı kenarları ve kLinkTargets (eski veri, sonradan oluşan hedef).
    // TagsAndMissingLinks: etiketler ve eksik bağlantı kenarları (senkronla gelen gövde).
    enum class Derive { Tags, Links, MissingLinks, TagsAndMissingLinks };
    // Gövdeden türeyen veriyi her canlı notta yeniden kurar; işlem bütçesinde bölünür. mutex tutulurken
    // çağrılır.
    std::expected<void, Error> rederive_locked(Derive what);
    // Önceki sürümün grafı: ilk açılışta bir kez etiketler ve sonradan oluşan hedeflerin kenarlarını kurar
    // (SPEC-graph "Etiketler", "Sonradan oluşan hedef").
    void index_derived_once();
    std::expected<std::pair<std::string, Revision>, Error> read_body(const graph::NodeId& id,
                                                                     std::string_view path);

    template <class R, class F> std::expected<R, Error> write(F&& body) {
        const std::scoped_lock lock(mutex);
        if (auto gate = check_gate(); !gate)
            return std::unexpected(gate.error());
        return write_locked<R>(std::forward<F>(body));
    }
    // Geçişin kendisi kapıdan muaftır: kapıyı açan odur.
    template <class R, class F> std::expected<R, Error> write_ungated(F&& body) {
        const std::scoped_lock lock(mutex);
        return write_locked<R>(std::forward<F>(body));
    }
    // keep_snapshot: çağıran art arda küçük yazmalar yapıyor ve eski görüntü işine yarıyor; her adımda
    // yeniden kurulmaz.
    // Kullanıcı işlemi çerçeve sınırına takılmaz: sığmayan iş (again) gövdenin yeni işlemde yinelenmesiyle,
    // sığmayan türetilmiş kenarlar (deferred) commit'ten hemen sonraki işlemlerde sürer. Her işlem atomiktir.
    template <class R, class F> std::expected<R, Error> write_locked(F&& body, bool keep_snapshot = false) {
        if (auto opened = open_for_write(); !opened)
            return std::unexpected(opened.error());
        while (true) {
            Pass pass;
            auto result = write_pass<R>(body, pass);
            if (!result)
                return result;
            if (!keep_snapshot || pass.again || !pass.deferred.empty() || !pass.unfinished.empty())
                snapshot.reset();
            if (!pass.unfinished.empty())
                derive_follow_up(pass.unfinished, Derive::Links);
            if (!pass.deferred.empty())
                derive_follow_up(pass.deferred, Derive::MissingLinks);
            if (!pass.again)
                return result;
            if (!follow_up_allowed_now())
                return std::unexpected(io_error(path_utf8(database()), "interrupted"));
        }
    }
    // Kullanıcı işleminden sonra bütçeye sığmayan türetilmiş kenarlar. Hata kullanıcı işlemini geri almaz:
    // loglanır, işaretler düştüğünden sonraki açılış tamamlar. mutex tutulurken çağrılır.
    void derive_follow_up(const std::vector<graph::NodeId>& ids, Derive what);
    // Notların gövdeden türeyen verisi, bütçeli işlemlerle; bütçeyi tek başına aşan not sonraki işlemde
    // kaldığı yerden sürer (yapılmış kenar yeniden yazılmaz). mutex tutulurken.
    std::expected<void, Error> derive_notes(const std::vector<graph::NodeId>& ids, Derive what);
    // Türetme tamamlanmadan commit edilecek işlemden önce: sonraki açılış türetmeyi baştan tamamlar.
    void drop_derived_markers();
    bool follow_up_allowed_now() const { return !follow_up_allowed || follow_up_allowed(); }

    std::filesystem::path directory;
    std::size_t slug_max_length;
    std::optional<std::filesystem::path> legacy_notes_dir;
    bool gate_open = false;        // geçiş bir kez doğrulandıysa süreç boyunca yeniden taranmaz
    bool derived_checked = false;  // index_derived_once süreç başına bir kez bakar
    std::mutex mutex;
    std::optional<graph::Graph> graph;
    // open_for_write ya da current başarılıysa dolu, sonra hiç boşalmaz. İhlal bug'dır: UB yerine durur.
    graph::Graph& opened_graph() {
        if (!graph)
            std::abort();
        return *graph;
    }
    std::shared_ptr<const Snapshot> snapshot;
    std::uint64_t snapshot_version = 0;
    std::optional<Error> last_error;
    // Ayar: limits.graph.* (for_config). Graf bununla açılır.
    graph::GraphLimits graph_limits{};
    // Ayar: limits.view.* (for_config). Görünüm (`view:`, `note view run`) her zaman bu korumalı profille
    // çalışır: View düğümü kökenini taşımaz, başka cihazdan gelmiş olabilir.
    graph::QueryLimits view_limits{};
    // Test dikişi: tek işlemin işlem sayısı tavanı. Üretimde sınırsızdır (L4b: işlem çerçeveye bağlı değil,
    // export kayıtlara böler). Küçültülünce bütçe en çok yarısıdır ve aşan işlem commit edilmez: 65.000 not
    // kurmadan işin bütçeyle bölündüğü sınanır.
    std::size_t txn_ops_limit = SIZE_MAX;
    // İşlem başına gövde baytı: etkin kayıt sınırının yarısı (Graph::max_frame_bytes: frame_bytes = 0 da
    // depolamanın tek değer sınırına kırpılır); tek büyük not kendi işleminde kalır. Graf açılamazsa config
    // değeri kalır: yazma da aynı hatayla zaten durur.
    std::size_t batch_bytes() {
        if (!open_for_write())
            return graph_limits.frame_bytes / 2;
        return opened_graph().max_frame_bytes() / 2;
    }
    // Ayar: [tuning] graph_batch_ops (for_config). Bir işlemin bütçesi; sınır değildir, iş bölünür.
    std::size_t batch_ops = kDefaultGraphBatchOps;
    // Ayar: limits.graph.relation_name_bytes (for_config); sınırsız SIZE_MAX.
    std::size_t relation_name_bytes = kDefaultRelationNameBytes;
    // Ayar: limits.search.property_keys (for_config): `prop:` önbelleğinin anahtar sayısı; sınırsız SIZE_MAX.
    std::size_t property_cache_keys = kDefaultPropertyCacheKeys;
    // Ayar: limits.core.folder_path_bytes (for_config); sınırsız SIZE_MAX.
    std::size_t folder_path_bytes = kDefaultFolderPathBytes;
    // Test dikişi: commit edilmiş bir kullanıcı işleminin ardından gelen işlemlerden önce sorulur; false
    // süreç orada ölmüş gibi durdurur (sonraki açılışın tamamladığı sınanır).
    std::function<bool()> follow_up_allowed;
    // Test dikişi: copy_folder'ın iki işlemi arası, kilit tutulmazken. Eşzamanlı yazarı belirlenimli kılar.
    std::function<void()> after_copy_batch;
    // Test dikişi: rederive_locked'ın iki işlemi arası. Başka bağlantının yazmasını belirlenimli kılar.
    std::function<void()> after_rederive_batch;
    // Test dikişi: görünüm sorgusu motora gitmeden hemen önce. Önbelleğin motoru kaç kez çalıştırdığını
    // sayar.
    std::function<void()> before_view_run;
    // Test dikişi: kullanıcı sorgusunun sonucu adlanmadan hemen önce (core/note_query); dolu hata adlandırmanın
    // düşmesi gibi davranır (commit edilmiş yazmanın raporu sınanır).
    std::function<std::optional<Error>()> query_naming_fault;
    // Test dikişi: görünüm aranırken (find_view); dolu hata deponun okunamaması gibi davranır (Busy, G/Ç).
    std::function<std::optional<Error>()> view_lookup_fault;

private:
    struct Pass {
        bool again = false;
        std::vector<graph::NodeId> deferred;
        std::vector<graph::NodeId> unfinished;
    };
    template <class R, class F> std::expected<R, Error> write_pass(F& body, Pass& pass) {
        auto txn = opened_graph().write();
        if (!txn)
            return std::unexpected(from_graph(txn.error(), path_utf8(database())));
        auto fresh = refresh_in(*txn);
        if (!fresh)
            return std::unexpected(fresh.error());
        WriteContext context(*txn, **fresh);
        context.budget = std::max<std::size_t>(1, std::min(batch_ops, txn_ops_limit / 2));
        context.batch_bytes = batch_bytes();
        auto result = body(context);
        if (!result)
            return result;
        if (auto linked = context.link_appeared(opened_graph().device()); !linked)
            return std::unexpected(linked.error());
        if (txn->pending_ops() > txn_ops_limit)  // üretimde motor da reddeder; dikişte küçük sınır burada
            return std::unexpected(from_graph(graph::Error{graph::Error::Code::LimitExceeded,
                                                           "graph.limit_exceeded",
                                                           {{"limit", "frame_ops"}}},
                                              path_utf8(database())));
        if (context.derived_incomplete)
            drop_derived_markers();
        if (auto committed = txn->commit(); !committed)
            return std::unexpected(from_graph(committed.error(), path_utf8(database())));
        pass.again = context.again;
        pass.deferred = std::move(context.deferred);
        pass.unfinished = std::move(context.unfinished);
        return result;
    }
    std::expected<std::shared_ptr<const Snapshot>, Error> refresh_in(graph::ReadTxn& txn);
};

}  // namespace noteapp::core
