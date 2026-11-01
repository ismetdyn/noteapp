#include "core/migration.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/log.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <system_error>

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMarkerFile =
    "migrated-notes-dirs";  // geçişi biten notes_dir'ler, satır başına bir yol
constexpr const char* kTrashDir = ".trash";
constexpr const char* kKeepFile = ".keep";
constexpr const char* kOplogDir = "graph";
constexpr std::uintmax_t kBatchBytes = 32U * 1024U * 1024U;    // işlem başına: çerçeve sınırı 64 MiB
constexpr std::size_t kBatchNotes = 500;                       // işlem başına: çerçeve sınırı 65.536 işlem
constexpr std::uintmax_t kMaxNoteBytes = 60U * 1024U * 1024U;  // tek çerçeveye sığmalı, zarfı için pay

// <32 küçük hex>.oplog ya da kenara alınmış <..>.oplog.aside-*: taşıyıcıdır, not değildir.
bool is_oplog_name(std::string_view name) {
    const auto dot = name.find(".oplog");
    return dot == 32 && std::ranges::all_of(name.substr(0, 32), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool is_carrier(const fs::path& relative) {
    const auto first = relative.begin();
    return first != relative.end() && *first == kOplogDir && std::next(first) != relative.end() &&
           std::next(std::next(first)) == relative.end() && is_oplog_name(path_utf8(relative.filename()));
}

fs::path path_from_utf8_view(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

bool is_hidden_name(const fs::path& name) {
    return path_utf8(name).starts_with('.');
}

fs::path canonical_notes_dir(const fs::path& notes_dir) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(notes_dir, ec);
    return ec ? notes_dir : canonical;
}

bool is_marked_migrated(const fs::path& notes_dir, const fs::path& data_directory) {
    std::ifstream in(data_directory / kMarkerFile, std::ios::binary);
    const std::string wanted = path_utf8(canonical_notes_dir(notes_dir));
    for (std::string line; std::getline(in, line);)
        if (line == wanted)
            return true;
    return false;
}

}  // namespace

std::expected<void, Error> mark_notes_dir_current(const fs::path& notes_dir, const fs::path& data_directory) {
    if (is_marked_migrated(notes_dir, data_directory))
        return {};
    if (const std::error_code made = create_private_directories(data_directory))
        return std::unexpected(io_error(path_utf8(data_directory), made.message()));
    const fs::path marker = data_directory / kMarkerFile;
    std::string text;
    {  // Windows'ta açık dosyanın üzerine atomik yazılamaz: yazmadan önce kapanır
        std::ifstream in(marker, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!text.empty() && !text.ends_with('\n'))
        text += '\n';
    text += path_utf8(canonical_notes_dir(notes_dir)) + "\n";
    return write_file_atomically(marker, text);
}

namespace {

std::expected<std::string, Error> slurp_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected(io_error(path_utf8(file), "cannot open file"));
    std::string text(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        return std::unexpected(io_error(path_utf8(file), "read failed"));
    return text;
}

std::chrono::system_clock::time_point to_system(fs::file_time_type when) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::file_clock::to_sys(when));
}

// Bu dosyanın created_at'ı: doğum zamanı, yoksa mtime; asla geçiş anı. Son değişiklikten sonra olamaz:
// kopyalanmış ya da geri yüklenmiş dosyanın doğumu kopya anıdır, mtime ondan eskidir.
std::chrono::system_clock::time_point created_time(const fs::path& file, fs::file_time_type mtime) {
    const auto birth = file_birth_time(file);
    return to_system(birth && *birth < mtime ? *birth : mtime);
}

// Eski ağacın bağ izlemeyen listesi. Gizli girdiler (.git, .noteapp) atlanır; .trash ayrı kökten gezilir.
struct LegacyTree {
    struct File {
        fs::path absolute;
        std::string relative;
        FileStat stat;
    };
    std::vector<File> files;
    std::vector<std::string> folders;
    std::vector<SkippedFile> links;
};

void walk_legacy(const fs::path& root, const fs::path& dir, const std::string& prefix, LegacyTree& tree) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    std::ranges::sort(entries);
    for (const fs::path& entry : entries) {
        const fs::path name = entry.filename();
        const auto stat = stat_file(entry);
        const std::string relative = prefix.empty() ? path_utf8(name) : prefix + "/" + path_utf8(name);
        if (!stat)
            continue;
        if (stat->symlink) {  // bağ izlenmez: hedefi kök dışına kaçabilir
            if (!is_hidden_name(name))
                tree.links.push_back({relative, "link"});
            continue;
        }
        if (is_hidden_name(name))
            continue;
        if (stat->directory) {
            tree.folders.push_back(relative);
            walk_legacy(root, entry, relative, tree);
        } else if (stat->regular && !is_carrier(entry.lexically_relative(root))) {
            tree.files.push_back({entry, relative, *stat});
        }
    }
}

bool tree_has_content(const fs::path& root, const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path name = it->path().filename();
        const auto stat = stat_file(it->path());
        if (!stat || stat->symlink)
            continue;
        if (is_hidden_name(name)) {
            if ((name == kTrashDir && stat->directory && dir == root && tree_has_content(root, it->path())) ||
                name == kKeepFile)
                return true;
            continue;
        }
        if (stat->directory ? tree_has_content(root, it->path())
                            : (stat->regular && !is_carrier(it->path().lexically_relative(root))))
            return true;
    }
    return false;
}

// Yedek: yalnız kopya (.git ve oplog hariç). Mevcut graph.db de yanına: içe aktarma onu değiştirir. Notlar
// sırdır: dizin 0700, dosyalar 0600, kaynak dosyaların modu ne olursa olsun.
std::expected<fs::path, Error> take_backup(const fs::path& notes_dir, const fs::path& data_directory,
                                           const std::vector<LegacyTree::File>& files,
                                           const std::vector<LegacyTree::File>& trash) {
    const std::string stamp = std::format(
        "{:%Y%m%dT%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    fs::path backup = data_directory / ("migration-backup-" + stamp);
    std::error_code probe;
    for (int attempt = 2; fs::exists(backup, probe); ++attempt)
        backup = data_directory / ("migration-backup-" + stamp + "-" + std::to_string(attempt));
    if (const std::error_code made = create_private_directories(backup))
        return std::unexpected(io_error(path_utf8(backup), made.message()));
    const auto copy_one = [&](const fs::path& source, const fs::path& target) -> std::expected<void, Error> {
        std::error_code ec = create_private_directories(target.parent_path());
        if (!ec)
            fs::copy_file(source, target, fs::copy_options::none, ec);
#ifndef _WIN32
        if (!ec)
            fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                            ec);
#endif
        if (ec)
            return std::unexpected(io_error(path_utf8(target), ec.message()));
        return {};
    };
    // Okunamayan dosya yedeği durdurmaz: içe aktarma onu adıyla raporlar, özgün dosya yerinde kalır.
    const auto copy_note = [&](const LegacyTree::File& file,
                               const fs::path& target) -> std::expected<void, Error> {
        std::ifstream readable(file.absolute, std::ios::binary);
        if (!readable)
            return {};
        return copy_one(file.absolute, target);
    };
    for (const LegacyTree::File& file : files)
        NOTEAPP_TRY(copy_note(file, backup / "notes" / path_from_utf8_view(file.relative)));
    for (const LegacyTree::File& file : trash)
        NOTEAPP_TRY(copy_note(file, backup / "trash" / path_from_utf8_view(file.relative)));
    for (const char* name : {"graph.db", "graph.db-wal", "graph.db-shm"})
        if (std::error_code ec; fs::exists(data_directory / name, ec))
            NOTEAPP_TRY(copy_one(data_directory / name, backup / name));
    log_info("note migrate: backup at " + path_utf8(backup) + " (notes_dir " + path_utf8(notes_dir) + ")");
    return backup;
}

// Bir ağacın dosyalarını partiler halinde içe aktarır; dosya başına sorun rapora girer, parti sürer.
std::expected<void, Error> import_tree(NoteStore& store, const std::vector<LegacyTree::File>& files,
                                       bool trashed, MigrationReport& report) {
    std::vector<ImportedNote> batch;
    std::uintmax_t batch_bytes = 0;
    const auto flush = [&]() -> std::expected<void, Error> {
        if (batch.empty())
            return {};
        auto outcome = store.import_notes(batch);
        if (!outcome)
            return std::unexpected(outcome.error());
        report.imported += outcome->imported;
        report.unchanged += outcome->unchanged;
        report.conflict_copies.insert(report.conflict_copies.end(), outcome->conflict_copies.begin(),
                                      outcome->conflict_copies.end());
        batch.clear();
        batch_bytes = 0;
        return {};
    };
    for (const LegacyTree::File& file : files) {
        auto parsed = NotePath::parse(file.relative);
        if (!parsed) {
            report.skipped.push_back({file.relative, "invalid_name"});
            continue;
        }
        if (!is_valid_utf8(file.relative)) {  // ad metin olarak saklanır; bayt koruması gövde içindir
            report.failed.push_back({file.relative, "invalid_name"});
            continue;
        }
        if (!folder_path_within_limits(split_note_path(file.relative).folder)) {
            report.failed.push_back({file.relative, "invalid_folder"});  // görünmez düğüm yazılmaz
            continue;
        }
        if (file.stat.size > kMaxNoteBytes) {  // tek çerçeveye sığmaz: geçişi durdurmaz, dosya yerinde kalır
            report.skipped.push_back({file.relative, "too_large"});
            continue;
        }
        auto bytes = slurp_file(file.absolute);
        if (!bytes) {
            report.failed.push_back({file.relative, "unreadable"});
            continue;
        }
        if (batch_bytes + bytes->size() > kBatchBytes || batch.size() >= kBatchNotes)
            if (auto flushed = flush(); !flushed)
                return flushed;
        batch_bytes += bytes->size();
        const auto when = to_system(file.stat.mtime);
        batch.push_back(ImportedNote{std::move(*parsed), std::move(*bytes), when,
                                     created_time(file.absolute, file.stat.mtime),
                                     trashed ? std::optional(when) : std::nullopt});
    }
    return flush();
}

using LostNotes = std::map<std::string, std::string>;  // kimlik -> neden

LostNotes lost_notes_of(const NoteStore& store) {
    LostNotes lost;
    for (const UnreachableNote& note : store.unreachable())
        lost.emplace(note.id, note.reason);
    return lost;
}

// Bayt doğrulaması düğüme bakar, yola değil: düğüm listeye çıkamıyorsa dosya geçmiş sayılmaz.
std::optional<std::string> lost_reason(const ImportedNote& note, const LostNotes& lost) {
    for (const std::string& id : imported_note_ids(note))
        if (const auto found = lost.find(id); found != lost.end())
            return found->second;
    return std::nullopt;
}

// Diskten yeniden okur: bellekte tüm notları tutmaz, arada değişen dosya da doğrulamadan kaçmaz.
std::expected<void, Error> verify_tree(const NoteStore& store, const std::vector<LegacyTree::File>& files,
                                       bool trashed, const LostNotes& lost, MigrationReport& report) {
    for (const LegacyTree::File& file : files) {
        auto parsed = NotePath::parse(file.relative);
        if (!parsed || !is_valid_utf8(file.relative) ||
            !folder_path_within_limits(split_note_path(file.relative).folder) ||
            file.stat.size > kMaxNoteBytes)
            continue;  // rapora zaten girdi
        auto bytes = slurp_file(file.absolute);
        if (!bytes)
            continue;
        const auto when = to_system(file.stat.mtime);
        const ImportedNote note{std::move(*parsed), std::move(*bytes), when, when,
                                trashed ? std::optional(when) : std::nullopt};
        auto verified = store.verify_imported(note);
        if (!verified)
            return std::unexpected(verified.error());
        if (!*verified)
            report.failed.push_back({file.relative, "verify_failed"});
        else if (auto reason = lost_reason(note, lost))
            report.failed.push_back({file.relative, std::move(*reason)});
    }
    return {};
}

}  // namespace

bool legacy_notes_pending(const fs::path& notes_dir, const fs::path& data_directory) {
    std::error_code ec;
    if (!fs::is_directory(notes_dir, ec))
        return false;
    if (is_marked_migrated(notes_dir, data_directory))
        return false;
    return tree_has_content(notes_dir, notes_dir);
}

namespace {

std::expected<void, Error> import_everything(NoteStore& store, const LegacyTree& live,
                                             const LegacyTree& trash, MigrationReport& report) {
    NOTEAPP_TRY(import_tree(store, live.files, false, report));
    NOTEAPP_TRY(import_tree(store, trash.files, true, report));
    std::vector<std::string> folders;
    for (const std::string& folder : live.folders) {
        if (!NotePath::parse(folder) || !is_valid_utf8(folder))
            continue;
        if (folder_path_within_limits(folder))
            folders.push_back(folder);
        else
            report.failed.push_back({folder, "invalid_folder"});
    }
    NOTEAPP_TRY(store.import_folders(folders));
    report.folders = folders.size();
    return store.relink_notes();
}

}  // namespace

std::expected<MigrationReport, Error>
migrate_legacy_notes(const fs::path& notes_dir, const fs::path& data_directory, MigrationOptions options) {
    MigrationReport report;
    if (const std::error_code made = create_private_directories(data_directory))
        return std::unexpected(io_error(path_utf8(data_directory), made.message()));

    LegacyTree live;
    walk_legacy(notes_dir, notes_dir, "", live);
    LegacyTree trash;
    const fs::path trash_root = notes_dir / kTrashDir;
    if (const auto stat = stat_file(trash_root); stat && stat->directory && !stat->symlink)
        walk_legacy(trash_root, trash_root, "", trash);
    report.skipped = live.links;
    report.skipped.insert(report.skipped.end(), trash.links.begin(), trash.links.end());

    auto backup = take_backup(notes_dir, data_directory, live.files, trash.files);
    if (!backup)
        return std::unexpected(backup.error());
    report.backup = *backup;

    NoteStore store(data_directory);
    NOTEAPP_TRY(import_everything(store, live, trash, report));
    // Taze bağlantı: önbellekten değil veritabanından okunmuş bayt karşılaştırılır.
    const NoteStore fresh(data_directory);
    const LostNotes lost = lost_notes_of(fresh);
    NOTEAPP_TRY(verify_tree(fresh, live.files, false, lost, report));
    NOTEAPP_TRY(verify_tree(fresh, trash.files, true, lost, report));
    if (!report.failed.empty() && !options.skip_failed)
        return report;  // kapı kapalı kalır; tekrar çağrı kalanı tamamlar
    NOTEAPP_TRY(mark_notes_dir_current(notes_dir, data_directory));
    report.complete = true;
    return report;
}

namespace {

// Yol üstünde bağ ya da dosya varsa içinden geçilmez.
bool parents_are_safe(const fs::path& dir, const fs::path& relative) {
    fs::path current = dir;
    for (const fs::path& component : relative) {
        current /= component;
        const auto stat = stat_file(current);
        if (!stat)
            return true;  // yok: oluşturulacak
        if (!stat->directory)
            return false;
    }
    return true;
}

// Notlar sırdır: yalnız yeni açılan dizin daralır, kullanıcının var olan dizininin izni değişmez.
std::error_code make_export_directories(const fs::path& dir) {
    std::error_code ec;
    const fs::file_status status = fs::status(dir, ec);
    if (fs::exists(status))
        return fs::is_directory(status) ? std::error_code{}
                                        : std::make_error_code(std::errc::not_a_directory);
    return create_private_directories(dir);
}

// Boş klasörler de düz ağaçta durur.
void export_folders(const NoteStore& store, const fs::path& dir, ExportAllReport& report) {
    for (const std::string& folder : store.folders()) {
        auto checked = NotePath::parse(folder);
        if (!checked || !parents_are_safe(dir, checked->value())) {
            report.refused.push_back({folder, !checked ? "invalid_name" : "link"});
            continue;
        }
        if (make_export_directories(dir / checked->value()))
            report.refused.push_back({folder, "unreadable"});
    }
}

// Reddin nedeni; yazıldıysa boş. Yalnız yerel G/Ç hatası expected'ın hatasıdır. relative: dir altındaki yer.
std::expected<std::optional<std::string>, Error>
write_exported(const fs::path& dir, const fs::path& relative,
               const std::expected<NoteContent, Error>& content) {
    if (!parents_are_safe(dir, relative.parent_path()))
        return std::optional<std::string>("link");
    const fs::path target = dir / relative;
    if (make_export_directories(target.parent_path()) || !content)
        return std::optional<std::string>("unreadable");
    std::error_code ec;
    // Dışlayıcı oluşturma: bağ izlenmez, var olan dosyanın üzerine yazılmaz.
    const FileWriteResult written = write_new_file(target, content->text, true);
    if (written.already_exists || (written.ec && fs::exists(target, ec)))
        return std::optional<std::string>(
            "exists");  // Windows'ta yalnız büyük/küçük harf farkı da buraya düşer
    if (written.ec)
        return std::unexpected(io_error(path_utf8(target), written.ec.message()));
    return std::optional<std::string>{};
}

}  // namespace

std::expected<ExportAllReport, Error> export_all_notes(const NoteStore& store, const fs::path& dir) {
    ExportAllReport report;
    if (const std::error_code ec = make_export_directories(dir))
        return std::unexpected(io_error(path_utf8(dir), ec.message()));
    const auto root = stat_file(dir);
    if (!root || !root->directory)  // kök de bağ olamaz: yazma hedefi başka yere kaçar
        return std::unexpected(invalid_path("link", path_utf8(dir)));
    export_folders(store, dir, report);
    auto notes = store.list();
    std::ranges::sort(notes, {}, [](const NoteInfo& info) { return info.path.string(); });
    for (const NoteInfo& info : notes) {
        // Kaynak güvenilmezdir: yol yeniden doğrulanır (ilk savunma snapshot'ta).
        auto refused = NotePath::parse(info.path.string())
                           ? write_exported(dir, info.path.value(), store.read(info.path))
                           : std::optional<std::string>("invalid_name");
        if (!refused)
            return std::unexpected(refused.error());
        if (*refused)
            report.refused.push_back({info.path.string(), **refused});
        else
            report.written.push_back({info.path, info.path.value()});
    }
    auto trash = store.list_trash();
    if (!trash)
        return std::unexpected(trash.error());
    for (const TrashItem& item : *trash) {
        const fs::path relative = fs::path(kTrashDir) / item.path.value();
        auto refused = write_exported(dir, relative, store.read_trash(item.path));
        if (!refused)
            return std::unexpected(refused.error());
        if (*refused)
            report.refused.push_back({std::string(kTrashDir) + "/" + item.path.string(), **refused});
        else
            report.written.push_back({item.path, relative});
    }
    report.unreachable = store.unreachable();
    return report;
}

}  // namespace noteapp::core
