#include "core/note_store.hpp"

#include "core/atomic_file.hpp"
#include "core/config.hpp"
#include "core/editor.hpp"
#include "core/file_stat.hpp"
#include "core/graph_limits.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "graph/graph.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ranges>
#include <system_error>
#include <utility>

namespace noteapp::core {

namespace fs = std::filesystem;
namespace graph = noteapp::graph;

namespace {

constexpr std::size_t kBinarySniffBytes = 8192;  // ilk 8 KB'ta NUL varsa ikili
// Önceki sürümün grafında etiket ve sonradan oluşan hedefin kenarı yoktur: açılışta bir kez türetilir, sonra
// bu dosyalar tekrarı önler.
constexpr std::string_view kTagsMarker = "tags-indexed";
constexpr std::string_view kLinksMarker = "links-indexed";

Error not_regular(std::string_view path) {
    return io_error(path, "not a regular file");
}

std::expected<void, Error> validate_new_note(const NewNote& note) {
    // Tip adın uzantısıdır, tek yol bileşeninin parçası: ayırıcı notu başka klasöre, ad kuralının reddettiği
    // karakter (":", sondaki boşluk…) bozuk ada götürürdü. Kural NotePath'inkidir, neden tipe yazılır.
    if (note.type.empty() || note.type.find_first_of("./\\") != std::string::npos ||
        !NotePath::parse("x." + note.type))
        return std::unexpected(invalid_path("invalid_type", note.type));
    if (note.name.find_first_of("/\\") != std::string::npos)
        return std::unexpected(invalid_path("invalid_name", note.name));
    if (!note.folder.empty()) {
        if (auto folder = NotePath::parse(note.folder); !folder)
            return std::unexpected(folder.error());
    }
    return {};
}

// Boş ad arar: canlı not ve klasör yolu doludur. Çöp ayrı ad uzayıdır. Deneme tavanı yok: görüntüdeki dolu ad
// sonludur, en geç onların sayısı + 1'inci denemede boş ad bulunur (ya da ekli ad yol kapısından geçemez).
std::expected<NotePath, Error> free_note_path(const Snapshot& snapshot, const NewNote& note,
                                              std::chrono::system_clock::time_point now,
                                              std::size_t slug_max_length) {
    const std::string base = note.name.empty()
                                 ? slugify(note_name_source(note.content, note.type), now, slug_max_length)
                                 : note.name;
    const std::string prefix = note.folder.empty() ? std::string() : note.folder + "/";
    for (std::size_t attempt = 1;; ++attempt) {
        const std::string stem = attempt == 1 ? base : base + "-" + std::to_string(attempt);
        auto path = NotePath::parse(prefix + stem + "." + note.type);
        if (!path)
            return std::unexpected(path.error());
        if (!snapshot.path_taken(path->string()))
            return path;
        if (note.exact_name)
            return std::unexpected(already_exists(path->string()));
    }
}

std::expected<NotePath, Error> free_restore_path(const Snapshot& snapshot, const NotePath& wanted) {
    for (std::size_t attempt = 1;; ++attempt) {  // tavan yok: free_note_path gibi
        auto candidate =
            attempt == 1 ? std::expected<NotePath, Error>(wanted) : numbered_path(wanted, attempt);
        if (!candidate)
            return std::unexpected(candidate.error());
        if (!snapshot.path_taken(candidate->string()))
            return candidate;
    }
}

// Klasör alt ağacı: kendisi ve "folder/" önekli her yol.
bool in_subtree(std::string_view path, std::string_view folder) {
    return path.size() > folder.size() && path[folder.size()] == '/' && path.starts_with(folder);
}

// Alt ağacın yolu hedefe göre yeniden türer: sınırı aşan alt klasör, içindekilerle birlikte görünmez olurdu.
std::expected<void, Error> check_renamed_subtree(const Snapshot& snapshot, std::string_view source,
                                                 const std::string& target) {
    if (!folder_path_within_limits(target, snapshot.folder_path_bytes))
        return std::unexpected(invalid_path("invalid_folder", target));
    for (const auto& [path, ids] : snapshot.folders) {
        if (!in_subtree(path, source))
            continue;
        const std::string renamed = target + path.substr(source.size());
        if (!folder_path_within_limits(renamed, snapshot.folder_path_bytes))
            return std::unexpected(invalid_path("invalid_folder", renamed));
    }
    return {};
}

std::expected<std::string, Error> read_temp_file(const fs::path& file) {
    const auto stat = stat_file(file);
    if (!stat)
        return std::unexpected(io_error(path_utf8(file), stat.error().message()));
    if (!stat->regular)  // editör bağ bıraktıysa izlenmez: hedefi not içeriği olurdu
        return std::unexpected(not_regular(path_utf8(file)));
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected(io_error(path_utf8(file), "cannot open file"));
    std::string text(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        return std::unexpected(io_error(path_utf8(file), "read failed"));
    return text;
}

// Alt ağacın klasörleri, en derin önce: alt klasörün CHILD_OF kenarı kendisiyle gider. Dönüş: daha var mı.
std::expected<bool, Error> remove_subtree_folders(WriteContext& context, const std::string& key) {
    for (const auto& [path, ids] : std::ranges::reverse_view(context.snapshot.folders)) {
        if (path != key && !in_subtree(path, key))
            continue;
        for (const graph::NodeId& id : ids) {
            if (context.over_budget())
                return true;
            NOTEAPP_TRY(context.delete_node(id, key));
            if (context.again)  // yarım silinen alt klasör üstüne bağlı kalmalı: üst silinirse köke düşerdi
                return true;
        }
    }
    return false;
}

// Bir işlemlik silme: alt ağaçtaki notlar işlem bütçesine kadar, en son klasörler. Dönüş: daha var mı. Kenarı
// çok not delete_node'da işlemlere bölünür.
std::expected<bool, Error> remove_folder_batch(WriteContext& context, const std::string& key, bool force) {
    const Snapshot& snapshot = context.snapshot;
    if (snapshot.live_by_path.contains(key))
        return std::unexpected(io_error(key, "not a directory"));
    if (!snapshot.folders.contains(key))
        return std::unexpected(not_found(key));
    std::vector<const NoteRow*> notes;
    for (const NoteRow& row : snapshot.live)
        if (in_subtree(row.info.path.string(), key))
            notes.push_back(&row);
    if (!force && !notes.empty())
        return std::unexpected(Error{Error::Code::NotEmpty,
                                     "error.not_empty",
                                     {{"path", key}, {"count", std::to_string(notes.size())}}});
    for (const NoteRow* note : notes) {
        if (context.over_budget())
            return true;
        NOTEAPP_TRY(context.delete_node(note->id, key));
    }
    if (context.again)  // yarım silinen not klasörüne bağlı kalmalı: klasör silinirse not köke düşerdi
        return true;
    return remove_subtree_folders(context, key);
}

// Bölünmüş silmenin sonraki geçişi: ilk geçişte bulunan düğümle sürer. Yol aranmaz: arada başka süreç notu
// taşıyıp yerine başka not koyduysa o silinmez. Düğüm başkası tarafından silinmişse iş bitmiştir.
std::expected<std::monostate, Error> continue_delete(WriteContext& context, const graph::NodeId& id,
                                                     const std::string& key) {
    auto exists = context.txn.has_node(id);
    if (!exists)
        return std::unexpected(from_graph(exists.error(), key));
    if (*exists)
        NOTEAPP_TRY(context.delete_node(id, key));
    return std::monostate{};
}

std::string file_type(const NotePath& path) {
    const std::string type = split_note_path(path.string()).type;
    return type.empty() ? "txt" : type;
}

}  // namespace

bool looks_binary(std::string_view bytes) {
    return bytes.substr(0, kBinarySniffBytes).find('\0') != std::string_view::npos;
}

NoteStore::NoteStore(fs::path directory, std::size_t slug_max_length)
    : impl_(std::make_shared<Impl>(std::move(directory), slug_max_length)) {}

NoteStore NoteStore::for_config(const Config& config) {
    NoteStore store(data_dir(), config.tuning.slug_max_length);
    store.impl_->legacy_notes_dir = config.notes_dir;
    store.impl_->batch_ops = config.tuning.graph_batch_ops;
    store.impl_->folder_path_bytes = config.limits.core.folder_path_bytes.cap();
    store.impl_->relation_name_bytes = config.limits.graph.relation_name_bytes.cap();
    store.impl_->graph_limits = graph_limits_of(config.limits.graph);
    store.impl_->view_limits = query_limits_of(config.limits.view);
    return store;
}

std::vector<NoteInfo> NoteStore::list() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    return snapshot ? snapshot->infos : std::vector<NoteInfo>{};
}

NoteMeta meta_of(NoteInfo info) {
    return NoteMeta{.path = std::move(info.path),
                    .size = info.size,
                    .updated_at = info.mtime,
                    .created_at = info.created_at,
                    .origin = std::move(info.origin),
                    .local_seq = info.local_seq,
                    .created_local_seq = info.created_local_seq,
                    .tags = std::move(info.tags)};
}

std::vector<NoteMeta> NoteStore::list_meta() const {
    std::vector<NoteMeta> metas;
    for (NoteInfo& info : list())
        metas.push_back(meta_of(std::move(info)));
    return metas;
}

std::vector<std::string> NoteStore::folders() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    std::vector<std::string> result;
    if (snapshot) {
        result.reserve(snapshot->folders.size());
        for (const auto& [path, ids] : snapshot->folders)
            result.push_back(path);
    }
    return result;
}

std::vector<UnreachableNote> NoteStore::unreachable() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    return snapshot ? snapshot->unreachable : std::vector<UnreachableNote>{};
}

std::expected<NoteContent, Error> NoteStore::read_trash(const NotePath& item) const {
    const std::string key = item.string();
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot)
        return std::unexpected(not_found(key));
    const auto row = snapshot->trash_by_path.find(key);
    if (row == snapshot->trash_by_path.end())
        return std::unexpected(not_found(key));
    const NoteRow& note = snapshot->trash[row->second];
    auto body = impl_->read_body(note.id, key);
    if (!body)
        return std::unexpected(body.error());
    return NoteContent{std::move(body->first), body->second, note.info.mtime};
}

std::optional<Error> NoteStore::health() const {
    const std::scoped_lock lock(impl_->mutex);
    if (auto gate = impl_->check_gate(); !gate)
        return gate.error();
    const auto snapshot = impl_->current();
    if (!snapshot)
        return impl_->last_error;
    return std::nullopt;
}

std::uint64_t NoteStore::data_version() const {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->current() ? impl_->snapshot_version : 0;
}

std::expected<NoteContent, Error> NoteStore::read(const NotePath& path) const {
    const std::string key = path.string();
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot) {
        if (impl_->last_error)
            return std::unexpected(*impl_->last_error);
        return std::unexpected(not_found(key));
    }
    if (snapshot->folders.contains(key))
        return std::unexpected(not_regular(key));
    const auto row = snapshot->live_by_path.find(key);
    if (row == snapshot->live_by_path.end())
        return std::unexpected(not_found(key));
    const NoteRow& note = snapshot->live[row->second];
    auto body = impl_->read_body(note.id, key);
    if (!body)
        return std::unexpected(body.error());
    return NoteContent{std::move(body->first), body->second, note.info.mtime};
}

std::expected<CreatedNote, Error> NoteStore::create_exact(const NewNote& note,
                                                          std::chrono::system_clock::time_point now) {
    if (auto valid = validate_new_note(note); !valid)
        return std::unexpected(valid.error());
    return impl_->write<CreatedNote>([&](WriteContext& context) -> std::expected<CreatedNote, Error> {
        auto path = free_note_path(context.snapshot, note, now, impl_->slug_max_length);
        if (!path)
            return std::unexpected(path.error());
        auto id = context.create_note(*path, note.content, now, kOrigin, note.created_at.value_or(now));
        if (!id)
            return std::unexpected(id.error());
        auto revision = context.body_revision(*id, path->string());
        if (!revision)
            return std::unexpected(revision.error());
        return CreatedNote{*path, *revision};
    });
}

std::expected<NotePath, Error> NoteStore::create(const NewNote& note,
                                                 std::chrono::system_clock::time_point now) {
    NewNote normalized = note;
    normalized.content = normalize_line_endings(note.content);
    auto created = create_exact(normalized, now);
    if (!created)
        return std::unexpected(created.error());
    return created->path;
}

std::expected<Revision, Error> NoteStore::write(const NotePath& path, std::string_view content,
                                                const Revision& expected,
                                                std::chrono::system_clock::time_point now) {
    const std::string key = path.string();
    return impl_->write<Revision>([&](WriteContext& context) -> std::expected<Revision, Error> {
        const auto row = context.snapshot.live_by_path.find(key);
        if (row == context.snapshot.live_by_path.end())
            return std::unexpected(not_found(key));
        const NoteRow& note = context.snapshot.live[row->second];
        if (note.info.revision != expected)
            return std::unexpected(
                Error{Error::Code::ModifiedExternally, "error.modified_externally", {{"path", key}}});
        return context.write_body(note, content, now);
    });
}

std::expected<bool, Error>
NoteStore::edit_in_temp_file(const NotePath& path,
                             const std::function<std::expected<void, Error>(const fs::path&)>& tool) {
    const auto original = read(path);
    if (!original)
        return std::unexpected(original.error());
    const auto file = create_private_temp_file(file_type(path), original->text);
    if (!file)
        return std::unexpected(file.error());

    const auto ran = tool(*file);
    const auto edited = read_temp_file(*file);
    const auto keep_with = [&](const Error& cause) {
        log_warn("note edit: kept temp file after failed save: " + path_utf8(*file));
        return Error{
            cause.code, "error.edit_kept", {{"file", path_utf8(*file)}, {"detail", format_error(cause)}}};
    };
    if (!edited)
        return std::unexpected(keep_with(edited.error()));
    bool changed = false;
    if (*edited != original->text) {
        if (auto saved = write(path, *edited, original->revision); !saved)
            return std::unexpected(keep_with(saved.error()));
        changed = true;
    }
    std::error_code ec;
    fs::remove(*file, ec);
    if (ec)
        log_warn("note edit: cannot remove temp file " + path_utf8(*file) + ": " + ec.message());
    if (!ran)
        return std::unexpected(ran.error());
    return changed;
}

std::expected<NotePath, Error> NoteStore::move(const NotePath& from, const NotePath& to) {
    const std::string source = from.string();
    return impl_->write<NotePath>([&](WriteContext& context) -> std::expected<NotePath, Error> {
        const Snapshot& snapshot = context.snapshot;
        if (snapshot.folders.contains(source))
            return std::unexpected(not_regular(source));
        const auto row = snapshot.live_by_path.find(source);
        if (row == snapshot.live_by_path.end())
            return std::unexpected(not_found(source));
        if (snapshot.path_taken(to.string()))
            return std::unexpected(already_exists(to.string()));
        const graph::NodeId id = snapshot.live[row->second].id;
        if (auto placed = context.place_note(id, to); !placed)
            return std::unexpected(placed.error());
        // Tip etiketin kuralını değiştirir (kodda `#include` etiket değildir).
        if (split_note_path(source).type != split_note_path(to.string()).type) {
            auto body = context.txn.property(id, kBody);
            if (!body)
                return std::unexpected(from_graph(body.error(), source));
            const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*body);
            NOTEAPP_TRY(
                context.update_tags(id, to.string(), bytes == nullptr ? std::string_view{} : bytes->data));
        }
        return to;
    });
}

std::expected<void, Error> NoteStore::remove(const NotePath& path) {
    const std::string key = path.string();
    std::optional<graph::NodeId> doomed;
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            if (doomed)
                return continue_delete(context, *doomed, key);
            if (context.snapshot.folders.contains(key))
                return std::unexpected(not_regular(key));
            const auto row = context.snapshot.live_by_path.find(key);
            if (row == context.snapshot.live_by_path.end())
                return std::unexpected(not_found(key));
            doomed = context.snapshot.live[row->second].id;
            if (auto gone = context.delete_node(*doomed, key); !gone)
                return std::unexpected(gone.error());
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

std::expected<TrashItem, Error> NoteStore::trash(const NotePath& path) {
    const std::string key = path.string();
    const auto id =
        impl_->write<graph::NodeId>([&](WriteContext& context) -> std::expected<graph::NodeId, Error> {
            if (context.snapshot.folders.contains(key))
                return std::unexpected(not_regular(key));
            const auto row = context.snapshot.live_by_path.find(key);
            if (row == context.snapshot.live_by_path.end())
                return std::unexpected(not_found(key));
            const graph::NodeId note = context.snapshot.live[row->second].id;
            if (auto marked = context.mark_trashed(note, key, std::chrono::system_clock::now()); !marked)
                return std::unexpected(marked.error());
            return note;
        });
    if (!id)
        return std::unexpected(id.error());
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (snapshot)
        for (const NoteRow& row : snapshot->trash)
            if (row.id == *id)
                return TrashItem{row.info.path,       row.trashed_at,  row.info.size,
                                 row.info.created_at, row.info.origin, id_hex(row.id)};
    return std::unexpected(io_error(key, "trashed note not visible"));
}

std::expected<std::vector<TrashItem>, Error> NoteStore::list_trash() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    std::vector<TrashItem> items;
    if (!snapshot) {
        if (impl_->last_error)
            return std::unexpected(*impl_->last_error);
        return items;
    }
    items.reserve(snapshot->trash.size());
    for (const NoteRow& row : snapshot->trash)
        items.push_back(TrashItem{row.info.path, row.trashed_at, row.info.size, row.info.created_at,
                                  row.info.origin, id_hex(row.id)});
    std::ranges::sort(items, [](const TrashItem& a, const TrashItem& b) {
        if (a.trashed_at != b.trashed_at)
            return a.trashed_at > b.trashed_at;
        return a.path.string() < b.path.string();
    });
    return items;
}

std::expected<NotePath, Error> NoteStore::restore(const NotePath& item) {
    const std::string key = item.string();
    return impl_->write<NotePath>([&](WriteContext& context) -> std::expected<NotePath, Error> {
        const auto row = context.snapshot.trash_by_path.find(key);
        if (row == context.snapshot.trash_by_path.end())
            return std::unexpected(not_found(key));
        const auto target = free_restore_path(context.snapshot, item);
        if (!target)
            return std::unexpected(target.error());
        const graph::NodeId note = context.snapshot.trash[row->second].id;
        if (auto cleared = context.clear_trashed(note, key); !cleared)
            return std::unexpected(cleared.error());
        if (auto placed = context.place_note(note, *target); !placed)
            return std::unexpected(placed.error());
        // Çöpteyken oluşan hedefler: kaynak geri gelince kenarlarını kurar.
        auto body = context.txn.property(note, kBody);
        if (!body)
            return std::unexpected(from_graph(body.error(), key));
        if (const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*body))
            NOTEAPP_TRY(context.add_missing_links(note, target->string(), bytes->data,
                                                  link_index_of(context.snapshot)));
        return *target;
    });
}

std::expected<void, Error> NoteStore::purge(const NotePath& item) {
    const std::string key = item.string();
    std::optional<graph::NodeId> doomed;
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            if (doomed)
                return continue_delete(context, *doomed, key);
            const auto row = context.snapshot.trash_by_path.find(key);
            if (row == context.snapshot.trash_by_path.end())
                return std::unexpected(not_found(key));
            doomed = context.snapshot.trash[row->second].id;
            if (auto gone = context.delete_node(*doomed, key); !gone)
                return std::unexpected(gone.error());
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

std::expected<void, Error> NoteStore::empty_trash() {
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto snapshot = impl_->current(); !snapshot || snapshot->trash.empty())
            return {};
    }
    bool more = true;
    while (more) {
        const auto deleted = impl_->write<bool>([&](WriteContext& context) -> std::expected<bool, Error> {
            for (const NoteRow& row : context.snapshot.trash) {
                if (context.over_budget())
                    return true;
                NOTEAPP_TRY(context.delete_node(row.id, row.info.path.string()));
            }
            return false;
        });
        if (!deleted)
            return std::unexpected(deleted.error());
        more = *deleted;
    }
    return {};
}

std::expected<void, Error> NoteStore::create_folder(std::string_view folder) {
    const auto parsed = NotePath::parse(folder);
    if (!parsed)
        return std::unexpected(parsed.error());
    const std::string key = parsed->string();
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            if (context.snapshot.live_by_path.contains(key))
                return std::unexpected(already_exists(key));
            if (auto made = context.ensure_folder(key); !made)
                return std::unexpected(made.error());
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

std::expected<void, Error> NoteStore::rename_folder(std::string_view from, std::string_view to) {
    const auto from_path = NotePath::parse(from);
    if (!from_path)
        return std::unexpected(from_path.error());
    const auto to_path = NotePath::parse(to);
    if (!to_path)
        return std::unexpected(to_path.error());
    const std::string source = from_path->string();
    const std::string target = to_path->string();
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            const Snapshot& snapshot = context.snapshot;
            if (snapshot.live_by_path.contains(source))
                return std::unexpected(io_error(source, "not a directory"));
            const auto folder = snapshot.folders.find(source);
            if (folder == snapshot.folders.end())
                return std::unexpected(not_found(source));
            // Kendi içine taşıma: türetilen yolda döngü olurdu.
            if (target == source || in_subtree(target, source))
                return std::unexpected(invalid_path("into_itself", target));
            if (snapshot.path_taken(target))
                return std::unexpected(already_exists(target));
            if (auto fits = check_renamed_subtree(snapshot, source, target); !fits)
                return std::unexpected(fits.error());
            if (auto moved = context.rename_folder(folder->second, target); !moved)
                return std::unexpected(moved.error());
            for (const NoteRow& row : snapshot.live)
                if (const std::string path = row.info.path.string(); in_subtree(path, source))
                    context.note_placed(row.id, target + path.substr(source.size()));
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

namespace {

// Kopyanın ilk adımı: denetimler. Hiçbir denetim düşmeden yazılmaz.
std::expected<void, Error> check_folder_copy(const WriteContext& context, const std::string& source,
                                             const std::string& target) {
    const Snapshot& snapshot = context.snapshot;
    if (!snapshot.folders.contains(source))
        return std::unexpected(not_found(source));
    if (target == source || in_subtree(target, source))
        return std::unexpected(invalid_path("into_itself", target));
    if (snapshot.path_taken(target))
        return std::unexpected(already_exists(target));
    return check_renamed_subtree(snapshot, source, target);
}

// Hedef klasörler bütçeye sığdığı kadar; kurulmuş olan atlanır (görüntüde vardır). Dönüş: hepsi var mı.
std::expected<bool, Error> ensure_copy_folders(WriteContext& context, const std::string& source,
                                               const std::string& target) {
    if (auto made = context.ensure_folder(target); !made)
        return std::unexpected(made.error());
    for (const auto& [path, ids] : context.snapshot.folders) {
        if (!in_subtree(path, source))
            continue;
        if (context.over_budget())
            return false;
        if (auto made = context.ensure_folder(target + path.substr(source.size())); !made)
            return std::unexpected(made.error());
    }
    return true;
}

std::expected<void, Error> copy_note_body(WriteContext& context, const NoteRow& row,
                                          const std::string& copy_path,
                                          std::chrono::system_clock::time_point now,
                                          std::size_t& bytes_in_batch) {
    const std::string path = row.info.path.string();
    // Görüntü taze: başka yazar hedefe not koyduysa ezme de -2'li ikiz de yok.
    if (context.snapshot.path_taken(copy_path))
        return std::unexpected(already_exists(copy_path));
    auto body = context.txn.property(row.id, kBody);
    if (!body)
        return std::unexpected(from_graph(body.error(), path));
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*body);
    const std::string_view content = bytes == nullptr ? std::string_view{} : bytes->data;
    auto copy = NotePath::parse(copy_path);
    if (!copy)
        return std::unexpected(copy.error());
    if (auto made = context.create_note(*copy, content, now, kOrigin, now); !made)
        return std::unexpected(made.error());
    bytes_in_batch += content.size();
    return {};
}

struct CopyBatch {
    std::string last_path;  // bu işlemde kopyalanan son kaynak yol; boşsa cursor ilerlemez
    std::size_t notes = 0;
    bool more = false;
};

// after'dan sonraki notları yol sırasıyla, bir çerçeveye sığdığı kadar kopyalar. Sıra konumla değil yolla
// sürer: işlemler arasında başka yazar kaynağa not ekleyip silebilir. Cursor'dan önce eklenen not bu kopyaya
// girmez.
std::expected<CopyBatch, Error> copy_next_batch(WriteContext& context, const std::string& source,
                                                const std::string& target, const std::string& after,
                                                std::chrono::system_clock::time_point now) {
    std::vector<const NoteRow*> rows;
    for (const NoteRow& row : context.snapshot.live) {
        const std::string path = row.info.path.string();
        if (in_subtree(path, source) && path > after)
            rows.push_back(&row);
    }
    std::ranges::sort(rows, {}, [](const NoteRow* row) { return row->info.path.string(); });
    CopyBatch batch;
    std::size_t bytes_in_batch = 0;
    for (const NoteRow* row : rows) {
        batch.more = context.over_budget() ||
                     (bytes_in_batch > 0 && bytes_in_batch + row->info.size > context.batch_bytes);
        if (batch.more)
            return batch;
        std::string path = row->info.path.string();
        NOTEAPP_TRY(copy_note_body(context, *row, target + path.substr(source.size()), now, bytes_in_batch));
        batch.last_path = std::move(path);
        ++batch.notes;
    }
    return batch;
}

Error partial_copy(const std::string& target, std::size_t notes, const Error& cause) {
    return Error{Error::Code::PartialCopy,
                 "error.copy_folder_partial",
                 {{"path", target}, {"count", std::to_string(notes)}, {"detail", format_error(cause)}}};
}

}  // namespace

std::expected<void, Error> NoteStore::copy_folder(std::string_view from, std::string_view to) {
    const auto from_path = NotePath::parse(from);
    if (!from_path)
        return std::unexpected(from_path.error());
    const auto to_path = NotePath::parse(to);
    if (!to_path)
        return std::unexpected(to_path.error());
    const std::string source = from_path->string();
    const std::string target = to_path->string();
    const auto now = std::chrono::system_clock::now();
    std::string cursor;  // yazılmamış işlem ilerletmez: yalnız commit'ten sonra güncellenir
    std::size_t copied = 0;
    bool first = true;
    bool more = true;
    while (more) {
        const auto step =
            impl_->write<CopyBatch>([&](WriteContext& context) -> std::expected<CopyBatch, Error> {
                if (first)
                    NOTEAPP_TRY(check_folder_copy(context, source, target));
                auto ready = ensure_copy_folders(context, source, target);
                if (!ready)
                    return std::unexpected(ready.error());
                if (!*ready)  // klasörler işlemlere bölünür; notlar hepsi kurulunca
                    return CopyBatch{{}, 0, true};
                return copy_next_batch(context, source, target, cursor, now);
            });
        if (!step)  // ilk işlem bütünüyle geri alınır; sonrası yerinde kalır
            return std::unexpected(first ? step.error() : partial_copy(target, copied, step.error()));
        first = false;
        copied += step->notes;
        if (step->notes > 0)
            cursor = step->last_path;
        more = step->more;
        if (more && impl_->after_copy_batch)
            impl_->after_copy_batch();
    }
    return {};
}

std::expected<void, Error> NoteStore::remove_folder(std::string_view folder, bool force) {
    const auto parsed = NotePath::parse(folder);
    if (!parsed)
        return std::unexpected(parsed.error());
    const std::string key = parsed->string();
    bool more = true;
    while (more) {
        const auto deleted = impl_->write<bool>(
            [&](WriteContext& context) { return remove_folder_batch(context, key, force); });
        if (!deleted)
            return std::unexpected(deleted.error());
        more = *deleted;
    }
    return {};
}

// Parti işlem bütçesine bölünür: not başına iş (etiketler, kenarlar) değişkendir, sabit not sayısı çerçeveyi
// aşabilirdi. Her işlem atomik, içe aktarma idempotenttir (kimlik yoldan türer): arada ölüm yinelemede
// tamamlanır.
std::expected<ImportOutcome, Error> NoteStore::import_notes(std::span<const ImportedNote> notes) {
    ImportOutcome total;
    std::size_t at = 0;
    while (at < notes.size()) {
        std::size_t consumed = 0;
        auto outcome = impl_->write_ungated<ImportOutcome>(
            [&](WriteContext& context) -> std::expected<ImportOutcome, Error> {
                // Eski dosya, başka cihazın görmediği gerçek düzenlemesini HLC'yle geçemez (SPEC-graph
                // "Geçiş").
                NOTEAPP_TRY_GRAPH(context.txn.set_lowest_priority(), "import");
                return context.import_notes(notes.subspan(at), consumed);
            });
        if (!outcome)
            return std::unexpected(outcome.error());
        total.imported += outcome->imported;
        total.unchanged += outcome->unchanged;
        std::ranges::move(outcome->conflict_copies, std::back_inserter(total.conflict_copies));
        at += consumed;
    }
    return total;
}

std::expected<void, Error> NoteStore::import_folders(std::span<const std::string> folders) {
    std::size_t at = 0;
    while (at < folders.size()) {
        auto step = impl_->write_ungated<std::size_t>(
            [&](WriteContext& context) -> std::expected<std::size_t, Error> {
                NOTEAPP_TRY_GRAPH(context.txn.set_lowest_priority(), "import");
                std::size_t next = at;
                for (; next < folders.size() && (next == at || !context.over_budget()); ++next) {
                    if (context.snapshot.live_by_path.contains(folders[next]))
                        continue;  // aynı adlı not var: boş klasörü kurmak yol çakışması olurdu
                    if (auto made = context.ensure_folder(folders[next]); !made)
                        return std::unexpected(made.error());
                }
                return next;
            });
        if (!step)
            return std::unexpected(step.error());
        at = *step;
    }
    return {};
}

std::expected<void, Error> NoteStore::relink_notes() {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->rederive_locked(Impl::Derive::Links);
}

namespace {

// Bir notun gövdeden türeyen verisi. false: bütçeye sığmadı; not yeni işlemde baştan yinelenir, yapılmış
// kenarlar atlanır (her işlem en az bir kenar ilerler).
std::expected<bool, Error> derive_note(WriteContext& context, const graph::NodeId& id,
                                       const std::string& path, NoteStore::Impl::Derive what) {
    using Derive = NoteStore::Impl::Derive;
    auto body = context.txn.property(id, kBody);
    if (!body)
        return std::unexpected(from_graph(body.error(), path));
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*body);
    if (bytes == nullptr)
        return true;
    if (what == Derive::Links)
        NOTEAPP_TRY(context.update_links(id, path, bytes->data));
    if (what != Derive::MissingLinks)
        NOTEAPP_TRY(context.update_tags(id, path, bytes->data));
    if (what == Derive::MissingLinks || what == Derive::TagsAndMissingLinks)
        NOTEAPP_TRY(context.add_missing_links(id, path, bytes->data, link_index_of(context.snapshot)));
    // Çağıran döngü yarım notu kendisi sürdürür: write_locked ayrıca izleyen işlem açmaz.
    const bool finished = context.unfinished.empty() && context.deferred.empty();
    context.unfinished.clear();
    context.deferred.clear();
    return finished;
}

}  // namespace

// Görüntü her işlemde yeniden kurulur; başka yazar arada not ekleyip silebilir. Sıra konum değil kimliktir:
// son tamamlanan kimlikten sonrası sürer, hiçbir not atlanmaz (arada eklenen, kimliği gerideyse kendi
// yazmasıyla zaten türetilmiştir). Bütçeye sığmayan not sonraki işlemde kaldığı yerden sürer.
std::expected<void, Error> NoteStore::Impl::rederive_locked(Derive what) {
    std::optional<graph::NodeId> last;
    bool more = true;
    while (more) {
        const auto step = write_locked<bool>([&](WriteContext& context) -> std::expected<bool, Error> {
            const Snapshot& snapshot = context.snapshot;
            auto it = last ? snapshot.live_by_id.upper_bound(*last) : snapshot.live_by_id.begin();
            for (; it != snapshot.live_by_id.end(); ++it) {
                if (context.over_budget())
                    return true;
                const NoteRow& row = snapshot.live[it->second];
                auto done = derive_note(context, row.id, row.info.path.string(), what);
                if (!done)
                    return std::unexpected(done.error());
                if (!*done)
                    return true;
                last = row.id;
            }
            return false;
        });
        if (!step)
            return std::unexpected(step.error());
        more = *step;
        if (more && after_rederive_batch)
            after_rederive_batch();
    }
    return {};
}

void NoteStore::Impl::drop_derived_markers() {
    for (const std::string_view name : {kTagsMarker, kLinksMarker}) {
        std::error_code ec;
        fs::remove(directory / name, ec);
        if (ec)  // kullanıcının işlemi durmaz; yalnız çökmede onarım bir sonraki işaret düşüşüne kalır
            log_warn("note store: cannot drop marker " + std::string(name) + ": " + ec.message());
    }
}

void NoteStore::Impl::derive_follow_up(const std::vector<graph::NodeId>& ids, Derive what) {
    if (auto done = derive_notes(ids, what); !done)
        log_warn("note store: cannot derive deferred links and tags: " + format_error(done.error()));
}

std::expected<void, Error> NoteStore::Impl::derive_notes(const std::vector<graph::NodeId>& ids, Derive what) {
    std::size_t next = 0;
    while (next < ids.size()) {
        if (!follow_up_allowed_now())
            return {};
        auto step =
            write_locked<std::size_t>([&](WriteContext& context) -> std::expected<std::size_t, Error> {
                std::size_t at = next;
                for (; at < ids.size() && (at == next || !context.over_budget()); ++at) {
                    const auto path = context.path_of(ids[at]);
                    if (!path)
                        continue;
                    auto done = derive_note(context, ids[at], *path, what);
                    if (!done)
                        return std::unexpected(done.error());
                    if (!*done)
                        return at;  // yarım not yeni işlemde sürer
                }
                return at;
            });
        if (!step)
            return std::unexpected(step.error());
        next = *step;
    }
    return {};
}

// Hata okumayı durdurmaz: türetilmemiş veri notların kaybı değildir; işaret yazılmaz, sonraki açılış yeniden
// dener. Geçiş bekliyorsa beklenir: geçiş relink_notes'la hepsini kurar.
void NoteStore::Impl::index_derived_once() {
    if (derived_checked)
        return;
    derived_checked = true;
    for (const auto& [marker_name, what] :
         {std::pair{kTagsMarker, Derive::Tags}, std::pair{kLinksMarker, Derive::MissingLinks}}) {
        const fs::path marker = directory / marker_name;
        std::error_code ec;
        if (fs::exists(marker, ec) || ec)
            continue;
        if (!check_gate())
            return;
        if (auto done = rederive_locked(what); !done) {
            log_warn("note store: cannot derive " + std::string(marker_name) + ": " +
                     format_error(done.error()));
            continue;
        }
        if (auto written = write_file_atomically(marker, "", true); !written)
            log_warn("note store: cannot write marker " + std::string(marker_name) + ": " +
                     format_error(written.error()));
    }
}

std::expected<bool, Error> NoteStore::verify_imported(const ImportedNote& note) const {
    const std::scoped_lock lock(impl_->mutex);
    if (auto opened = impl_->open_for_write(); !opened)
        return std::unexpected(opened.error());
    auto txn = impl_->opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), note.path.string()));
    return verify_import(*txn, note);
}

}  // namespace noteapp::core
