#pragma once

// İç başlık (core ve note_sync). Uygulama şeması burada: motor nottan habersizdir (SPEC-graph ilke 5).
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "core/text_merger.hpp"
#include "graph/graph.hpp"

#include <array>
#include <chrono>
#include <cstddef>
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
inline constexpr std::string_view kName = "name";
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

// Görüntünün çözdüğü en derin ve en uzun klasör yolu; sınır aşan yol kümenin saf fonksiyonudur, her cihazda
// aynı klasörler geçersiz sayılır. Yazma aynı sınırı dayatır: yoksa kayıt başarılı döner, not görünmez
// kalırdı.
inline constexpr std::size_t kMaxFolderDepth = 128;
inline constexpr std::size_t kMaxFolderPathBytes = 4096;
bool folder_path_within_limits(std::string_view folder);

Error io_error(std::string_view path, std::string_view detail);
Error not_found(std::string_view path);
Error invalid_path(std::string_view reason, std::string_view path);
Error already_exists(std::string_view path);
Error from_graph(const graph::Error& error, std::string_view path);
std::string path_utf8(const std::filesystem::path& path);
std::string id_hex(const graph::NodeId& id);
std::expected<NotePath, Error> numbered_path(const NotePath& path, int number);

struct NoteParts {
    std::string folder;  // "" kök
    std::string name;
    std::string type;  // uzantısızsa ""
};
NoteParts split_note_path(std::string_view path);

struct NoteRow {
    graph::NodeId id;
    NoteInfo info;  // info.path gösterilen yol: çakışmada -2 alır, saklanmaz
    std::filesystem::file_time_type trashed_at{};
    bool conflicted = false;  // gövdede birden çok head
};

// Bir okumanın tüm türetilmiş yolları. Yazma ve okuma aynı kuralı kullansın diye tek yerde kurulur.
struct Snapshot {
    std::vector<NoteRow> live;
    std::vector<NoteRow> trash;
    std::vector<NoteInfo> infos;
    std::unordered_map<std::string, std::size_t> live_by_path;
    std::unordered_map<std::string, std::size_t> trash_by_path;
    std::vector<UnreachableNote> unreachable;
    std::map<std::string, std::vector<graph::NodeId>>
        folders;  // aynı yolda eşzamanlı kurulmuş birden çok düğüm olabilir
    bool path_taken(const std::string& path) const {
        return live_by_path.contains(path) || folders.contains(path);
    }
};

graph::Result<Snapshot> build_snapshot(graph::ReadTxn& txn, const std::array<std::byte, 16>& device);

// [[bağlantı]] hedefleri; "|takma ad" ve "#başlık" atılır.
std::vector<std::string> scan_links(std::string_view body);

std::expected<bool, Error> verify_import(graph::ReadTxn& txn, const ImportedNote& note);
// Geçiş notunun baytlarını tutabilecek düğümler (asıl ve kopya), UnreachableNote::id biçiminde.
std::array<std::string, 2> imported_note_ids(const ImportedNote& note);

// Açık bir yazma işleminde uygulama şemasının yazmaları.
class WriteContext {
public:
    WriteContext(graph::WriteTxn& txn_in, const Snapshot& snapshot_in) : txn(txn_in), snapshot(snapshot_in) {}

    std::expected<graph::NodeId, Error> create_note(const NotePath& path, std::string_view bytes,
                                                    std::chrono::system_clock::time_point now,
                                                    std::string_view origin);
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
    std::expected<ImportOutcome, Error> import_notes(std::span<const ImportedNote> notes);
    std::expected<void, Error> update_links(const graph::NodeId& id, std::string_view path,
                                            std::string_view body);
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

    graph::WriteTxn& txn;
    const Snapshot& snapshot;

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
    const std::unordered_map<std::string, graph::NodeId>& link_index();
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
    std::expected<bool, Error> is_stale_import(const graph::NodeId& id, const graph::Version& version);
    std::expected<std::int64_t, Error> version_time(const graph::NodeId& id, const graph::Version& version);

    std::map<std::string, graph::NodeId> added_folders_;
    std::optional<std::unordered_map<std::string, graph::NodeId>> link_index_;
};

struct NoteStore::Impl {
    Impl(std::filesystem::path directory_in, std::size_t slug)
        : directory(std::move(directory_in)), slug_max_length(slug) {}

    std::filesystem::path database() const { return directory / "graph.db"; }
    // Veritabanı yoksa ya da açılamıyorsa boş (neden last_error'da). mutex tutulurken çağrılır.
    std::shared_ptr<const Snapshot> current();
    std::expected<void, Error> check_gate();
    std::expected<void, Error> open_for_write();
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
    template <class R, class F> std::expected<R, Error> write_locked(F&& body, bool keep_snapshot = false) {
        if (auto opened = open_for_write(); !opened)
            return std::unexpected(opened.error());
        auto txn = opened_graph().write();
        if (!txn)
            return std::unexpected(from_graph(txn.error(), path_utf8(database())));
        auto fresh = refresh_in(*txn);
        if (!fresh)
            return std::unexpected(fresh.error());
        WriteContext context(*txn, **fresh);
        auto result = std::forward<F>(body)(context);
        if (!result)
            return result;
        if (auto committed = txn->commit(); !committed)
            return std::unexpected(from_graph(committed.error(), path_utf8(database())));
        if (!keep_snapshot)
            snapshot.reset();
        return result;
    }

    std::filesystem::path directory;
    std::size_t slug_max_length;
    std::optional<std::filesystem::path> legacy_notes_dir;
    bool gate_open = false;  // geçiş bir kez doğrulandıysa süreç boyunca yeniden taranmaz
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
    // Test dikişi: copy_folder'ın iki işlemi arası, kilit tutulmazken. Eşzamanlı yazarı belirlenimli kılar.
    std::function<void()> after_copy_batch;

private:
    std::expected<std::shared_ptr<const Snapshot>, Error> refresh_in(graph::ReadTxn& txn);
};

}  // namespace noteapp::core
