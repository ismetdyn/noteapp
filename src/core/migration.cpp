#include "core/migration.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/log.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "graph/id.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMarkerFile =
    "migrated-notes-dirs";  // geçişi biten notes_dir'ler, satır başına bir yol
constexpr const char* kSkippedFile = "migration-skipped";
constexpr const char* kDigestFile = "migrated-digests";  // içe alınan dosyaların baytları, purge'ü atlatır
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

// Eski sürümün 0644 bıraktığı kayıt daralır: atomik yazma var olan hedefin modunu korur.
void tighten_marker(const fs::path& marker) {
#ifdef _WIN32
    (void)marker;
#else
    std::error_code ec;
    fs::permissions(marker, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        log_warn("event=migration_marker_chmod_failed detail=" + ec.message());
#endif
}

}  // namespace

std::expected<void, Error> mark_notes_dir_current(const fs::path& notes_dir, const fs::path& data_directory) {
    tighten_marker(data_directory / kMarkerFile);
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
    return write_file_atomically(marker, text, true);
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
// Okunamayan girdi sessizce düşmez: kullanıcı eski dosyaları silerse tek kopyası gider.
struct LegacyTree {
    struct File {
        fs::path absolute;
        std::string relative;
        FileStat stat{};
    };
    std::vector<File> files;
    std::vector<std::string> folders;
    std::vector<SkippedFile> links;
    std::vector<SkippedFile> unreadable;
    std::size_t entries_left = std::numeric_limits<std::size_t>::max();  // find_stray_files sınırı
    bool truncated = false;
};

constexpr const char* kTreeRoot = ".";  // okunamayan kökün rapordaki adı

// Kök hiç yoksa geçirilecek bir şey de yoktur. Alt dizinde "yok" da raporlanır: yol uzunluğu aşımı (MinGW
// _wopendir, MAX_PATH) "bulunamadı" diye döner.
bool path_is_missing(const fs::path& path) {
    std::error_code ec;
    return fs::symlink_status(path, ec).type() == fs::file_type::not_found;
}

std::vector<fs::path> list_entries(const fs::path& dir, const std::string& prefix, LegacyTree& tree) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (tree.entries_left == 0) {
            tree.truncated = true;
            break;
        }
        --tree.entries_left;
        entries.push_back(it->path());
    }
    if (ec)  // içi bilinmiyor (izin, yol uzunluğu); okunabilen kısmı yine gezilir
        tree.unreadable.push_back({prefix.empty() ? std::string(kTreeRoot) : prefix, "unreadable_folder"});
    std::ranges::sort(entries);
    return entries;
}

void walk_legacy(const fs::path& root, const fs::path& dir, const std::string& prefix, LegacyTree& tree) {
    for (const fs::path& entry : list_entries(dir, prefix, tree)) {
        const fs::path name = entry.filename();
        const auto stat = stat_file(entry);
        const std::string relative = prefix.empty() ? path_utf8(name) : prefix + "/" + path_utf8(name);
        if (!stat) {
            if (!is_hidden_name(name))
                tree.unreadable.push_back({relative, "unreadable"});
            continue;
        }
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

bool tree_has_content(const fs::path& root, const fs::path& dir);

bool entry_has_content(const fs::path& root, const fs::path& dir, const fs::path& entry) {
    const fs::path name = entry.filename();
    const bool hidden = is_hidden_name(name);
    const bool root_trash = name == kTrashDir && dir == root;
    const auto stat = stat_file(entry);
    if (!stat)  // türü bilinmiyor: not olabilir (.git gibi gizli adlar sayılmaz)
        return !hidden || root_trash;
    if (stat->symlink)
        return false;
    if (hidden)
        return (root_trash && stat->directory && tree_has_content(root, entry)) || name == kKeepFile;
    return stat->directory ? tree_has_content(root, entry)
                           : (stat->regular && !is_carrier(entry.lexically_relative(root)));
}

bool tree_has_content(const fs::path& root, const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (entry_has_content(root, dir, it->path()))
            return true;
    return static_cast<bool>(ec);  // okunamayan dizinin içi bilinmez: boş sayılmaz
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

// Rapordaki yol kullanıcının bulacağı yerdir: çöp ağacı kendi kökünden gezilir, .trash öneki burada gelir.
std::string trash_report_path(const std::string& relative) {
    return relative == kTreeRoot ? std::string(kTrashDir) : std::string(kTrashDir) + "/" + relative;
}

std::string report_path(const LegacyTree::File& file, bool trashed) {
    return trashed ? trash_report_path(file.relative) : file.relative;
}

// İçe alınamayacak dosyanın nedeni; alınabilirse boş.
std::optional<std::string> import_refusal(const LegacyTree::File& file) {
    if (!NotePath::parse(file.relative) || !is_valid_utf8(file.relative))
        return "invalid_name";
    if (!folder_path_within_limits(split_note_path(file.relative).folder))
        return "invalid_folder";
    if (file.stat.size > kMaxNoteBytes)
        return "too_large";
    return std::nullopt;
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
            report.skipped.push_back({report_path(file, trashed), "invalid_name"});
            continue;
        }
        if (!is_valid_utf8(file.relative)) {  // ad metin olarak saklanır; bayt koruması gövde içindir
            report.failed.push_back({report_path(file, trashed), "invalid_name"});
            continue;
        }
        if (!folder_path_within_limits(split_note_path(file.relative).folder)) {
            report.failed.push_back(
                {report_path(file, trashed), "invalid_folder"});  // görünmez düğüm yazılmaz
            continue;
        }
        if (file.stat.size > kMaxNoteBytes) {  // tek çerçeveye sığmaz: geçişi durdurmaz, dosya yerinde kalır
            report.skipped.push_back({report_path(file, trashed), "too_large"});
            continue;
        }
        auto bytes = slurp_file(file.absolute);
        if (!bytes) {
            report.failed.push_back({report_path(file, trashed), "unreadable"});
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

// (köke göreli rapor yolu, baytların sha256'sı hex): bu yoldaki bu baytlar bir kez grafa girdi.
using DigestLedger = std::set<std::pair<std::string, std::string>>;

std::string digest_hex(std::string_view bytes) {
    std::string hex;
    for (const std::byte value : graph::sha256(std::as_bytes(std::span(bytes.data(), bytes.size()))))
        hex += std::format("{:02x}", static_cast<unsigned>(value));
    return hex;
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
// Doğrulanan dosyanın baytları imported'a girer.
std::expected<void, Error> verify_tree(const NoteStore& store, const std::vector<LegacyTree::File>& files,
                                       bool trashed, const LostNotes& lost, MigrationReport& report,
                                       DigestLedger& imported) {
    for (const LegacyTree::File& file : files) {
        if (import_refusal(file))
            continue;  // rapora zaten girdi
        auto bytes = slurp_file(file.absolute);
        if (!bytes)
            continue;
        const auto when = to_system(file.stat.mtime);
        const ImportedNote note{NotePath::parse(file.relative).value(), std::move(*bytes), when, when,
                                trashed ? std::optional(when) : std::nullopt};
        auto verified = store.verify_imported(note);
        if (!verified)
            return std::unexpected(verified.error());
        if (!*verified)
            report.failed.push_back({report_path(file, trashed), "verify_failed"});
        else if (auto reason = lost_reason(note, lost))
            report.failed.push_back({report_path(file, trashed), std::move(*reason)});
        else
            imported.emplace(report_path(file, trashed), digest_hex(note.bytes));
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

// Dosya adı sekme ve satır sonu içerebilir: satır "neden<TAB>yol", özel baytlar %XX.
std::string escape_field(std::string_view field) {
    std::string out;
    for (const char c : field) {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r')
            out += std::format("%{:02X}", static_cast<unsigned char>(c));
        else
            out += c;
    }
    return out;
}

std::optional<std::string> unescape_field(std::string_view field) {
    constexpr std::size_t kEscapeCells = 3;
    constexpr int kHexBase = 16;
    std::string out;
    for (std::size_t at = 0; at < field.size(); ++at) {
        if (field[at] != '%') {
            out += field[at];
            continue;
        }
        if (at + kEscapeCells > field.size())
            return std::nullopt;
        unsigned value = 0;
        const char* const first = field.data() + at + 1;
        const char* const last = first + kEscapeCells - 1;
        const auto parsed = std::from_chars(first, last, value, kHexBase);
        if (parsed.ec != std::errc{} || parsed.ptr != last)
            return std::nullopt;
        out += static_cast<char>(value);
        at += kEscapeCells - 1;
    }
    return out;
}

std::string skipped_prefix(const fs::path& notes_dir) {
    return path_utf8(canonical_notes_dir(notes_dir)) + "/";
}

std::expected<void, Error> remove_skipped_list(const fs::path& data_directory) {
    const fs::path list = data_directory / kSkippedFile;
    std::error_code ec;
    fs::remove(list, ec);
    if (ec)
        return std::unexpected(io_error(path_utf8(list), ec.message()));
    return {};
}

// Başka notes_dir'in kayıtları kalır: aynı veri dizinine ikinci kaynak geçirmek öncekinin uyarısını
// silmemeli.
std::expected<void, Error> save_skipped_files(const fs::path& notes_dir, const fs::path& data_directory,
                                              const MigrationReport& report) {
    auto previous = read_skipped_files(data_directory);
    if (!previous)
        return std::unexpected(previous.error());
    const std::string root = skipped_prefix(notes_dir);
    std::string text;
    const auto add = [&text](const std::string& reason, const std::string& path) {
        text += escape_field(reason) + "\t" + escape_field(path) + "\n";
    };
    for (const SkippedFile& file : *previous)
        if (!file.path.starts_with(root))
            add(file.reason, file.path);
    for (const std::vector<SkippedFile>* files : {&report.skipped, &report.failed})
        for (const SkippedFile& file : *files)
            add(file.reason, root + file.path);
    if (!text.empty())
        return write_file_atomically(data_directory / kSkippedFile, text, true);
    return remove_skipped_list(data_directory);
}

// Kurulabilir klasörler; sınırı aşan rapora girer.
std::vector<std::string> importable_folders(const LegacyTree& live, MigrationReport& report) {
    std::vector<std::string> folders;
    for (const std::string& folder : live.folders) {
        if (!NotePath::parse(folder) || !is_valid_utf8(folder))
            continue;
        if (folder_path_within_limits(folder))
            folders.push_back(folder);
        else
            report.failed.push_back({folder, "invalid_folder"});
    }
    return folders;
}

std::expected<void, Error> import_everything(NoteStore& store, const LegacyTree& live,
                                             const LegacyTree& trash, MigrationReport& report) {
    NOTEAPP_TRY(import_tree(store, live.files, false, report));
    NOTEAPP_TRY(import_tree(store, trash.files, true, report));
    const std::vector<std::string> folders = importable_folders(live, report);
    NOTEAPP_TRY(store.import_folders(folders));
    report.folders = folders.size();
    return store.relink_notes();
}

// "alan<TAB>yol" satırları (atlananlar: neden; içe alınanlar: sha256). Dosya yoksa boş; bozuksa hata.
std::expected<std::vector<SkippedFile>, Error> read_record_list(const fs::path& list) {
    std::error_code ec;
    if (!fs::exists(list, ec) && !ec)
        return std::vector<SkippedFile>{};
    auto text = slurp_file(list);
    if (!text)
        return std::unexpected(text.error());
    std::vector<SkippedFile> files;
    std::string_view rest = *text;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        if (end == std::string_view::npos)
            return std::unexpected(io_error(path_utf8(list), "truncated line"));
        const std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end + 1);
        const std::size_t tab = line.find('\t');
        auto reason = tab == std::string_view::npos ? std::nullopt : unescape_field(line.substr(0, tab));
        auto path = tab == std::string_view::npos ? std::nullopt : unescape_field(line.substr(tab + 1));
        if (!reason || !path)
            return std::unexpected(io_error(path_utf8(list), "malformed line"));
        files.push_back({std::move(*path), std::move(*reason)});
    }
    return files;
}

// İçe alınmış dosyaların baytları, düğüm sonradan silinse de (purge gövdeyi ve migrated_sha'yı da siler)
// kalır: kullanıcının sildiği not eski dosyasından dirilmez. Satır "sha256-hex<TAB>tam yol", tek dosya
// bütün kökler için, 0600.
std::expected<DigestLedger, Error> read_digest_ledger(const fs::path& notes_dir,
                                                      const fs::path& data_directory) {
    auto records = read_record_list(data_directory / kDigestFile);
    if (!records)
        return std::unexpected(records.error());
    const std::string root = skipped_prefix(notes_dir);
    DigestLedger ledger;
    for (SkippedFile& record : *records)
        if (record.path.starts_with(root))
            ledger.emplace(record.path.substr(root.size()), std::move(record.reason));
    return ledger;
}

// Yalnız ekler: eski baytlar da bir kez grafa girmiştir, dosya o baytlara dönerse yine yeni sayılmaz.
std::expected<void, Error> append_digest_ledger(const fs::path& notes_dir, const fs::path& data_directory,
                                                const DigestLedger& added) {
    auto known = read_digest_ledger(notes_dir, data_directory);
    if (!known)
        return std::unexpected(known.error());
    const fs::path file = data_directory / kDigestFile;
    const std::string root = skipped_prefix(notes_dir);
    std::string text;
    for (const auto& [path, digest] : added)
        if (!known->contains({path, digest}))
            text += escape_field(digest) + "\t" + escape_field(root + path) + "\n";
    if (text.empty())
        return {};
    std::string previous;
    {  // Windows'ta açık dosyanın üzerine atomik yazılamaz: yazmadan önce kapanır
        std::ifstream in(file, std::ios::binary);
        previous.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return write_file_atomically(file, previous + text, true);
}

}  // namespace

std::expected<std::vector<SkippedFile>, Error> read_skipped_files(const fs::path& data_directory) {
    return read_record_list(data_directory / kSkippedFile);
}

bool skipped_file_is_under(const SkippedFile& file, const fs::path& notes_dir) {
    return file.path.starts_with(skipped_prefix(notes_dir));
}

std::expected<void, Error> clear_skipped_files(const fs::path& data_directory) {
    return remove_skipped_list(data_directory);
}

namespace {

LegacyTree walk_legacy_trash(const fs::path& trash_root, std::size_t entries_left) {
    LegacyTree trash;
    trash.entries_left = entries_left;
    if (const auto stat = stat_file(trash_root); stat && stat->directory && !stat->symlink)
        walk_legacy(trash_root, trash_root, "", trash);
    else if (!stat && !path_is_missing(trash_root))
        trash.unreadable.push_back({kTreeRoot, "unreadable_folder"});
    return trash;
}

struct LegacyTrees {
    LegacyTree live;
    LegacyTree trash;
};

LegacyTrees walk_trees(const fs::path& notes_dir,
                       std::size_t max_entries = std::numeric_limits<std::size_t>::max()) {
    LegacyTree live;
    live.entries_left = max_entries;
    if (!path_is_missing(notes_dir))
        walk_legacy(notes_dir, notes_dir, "", live);
    LegacyTree trash = walk_legacy_trash(notes_dir / kTrashDir, live.entries_left);
    return {std::move(live), std::move(trash)};
}

void seed_report(const LegacyTrees& trees, MigrationReport& report) {
    report.skipped = trees.live.links;
    report.failed = trees.live.unreadable;
    for (const SkippedFile& link : trees.trash.links)
        report.skipped.push_back({trash_report_path(link.path), link.reason});
    for (const SkippedFile& entry : trees.trash.unreadable)
        report.failed.push_back({trash_report_path(entry.path), entry.reason});
}

// Grafta mı (ya da bir kez girdi mi): bu yolda bu baytlar içe alınmıştı (sonradan silinmiş olabilir), içe
// aktarma kaydı (gövde, migrated_sha, kopya) ya da aynı yolda aynı baytlarla uygulamada açılmış not. Çağıran
// import_refusal'ı geçmiş olmalı.
std::expected<bool, Error> file_in_graph(const NoteStore& store, const DigestLedger& ledger,
                                         const LegacyTree::File& file, std::string bytes, bool trashed) {
    if (ledger.contains({report_path(file, trashed), digest_hex(bytes)}))
        return true;
    const auto when = to_system(file.stat.mtime);
    const ImportedNote note{NotePath::parse(file.relative).value(), std::move(bytes), when, when,
                            trashed ? std::optional(when) : std::nullopt};
    auto verified = store.verify_imported(note);
    if (!verified)
        return std::unexpected(verified.error());
    if (*verified || trashed)
        return *verified;
    // Okunamayan not "aynı" sayılmaz: dosya listelenir, kayıp sessiz kalmaz.
    const auto current = store.read(note.path);
    return current && current->text == note.bytes;
}

// Dosyalardan grafta olmayanlar; içe alınamayacak ve okunamayanlar da kalır, import_tree raporlar. Grafta
// bulunanların baytları known'a girer: kayıttan önceki geçişlerin dosyaları da sonraki silmeye karşı korunur.
std::expected<std::vector<LegacyTree::File>, Error>
files_not_in_graph(const NoteStore& store, const DigestLedger& ledger,
                   const std::vector<LegacyTree::File>& files, bool trashed, DigestLedger& known_out) {
    std::vector<LegacyTree::File> kept;
    for (const LegacyTree::File& file : files) {
        if (import_refusal(file)) {
            kept.push_back(file);
            continue;
        }
        auto bytes = slurp_file(file.absolute);
        if (!bytes) {
            kept.push_back(file);
            continue;
        }
        std::string digest = digest_hex(*bytes);
        auto known = file_in_graph(store, ledger, file, std::move(*bytes), trashed);
        if (!known)
            return std::unexpected(known.error());
        if (*known)
            known_out.emplace(report_path(file, trashed), std::move(digest));
        else
            kept.push_back(file);
    }
    return kept;
}

std::vector<LegacyTree::File> importable_of(const std::vector<LegacyTree::File>& files) {
    std::vector<LegacyTree::File> importable;
    std::ranges::copy_if(files, std::back_inserter(importable),
                         [](const LegacyTree::File& file) { return !import_refusal(file); });
    return importable;
}

// Kayıtlı listenin bu köke ait, köke göreli yolları.
std::expected<std::set<std::string>, Error> recorded_paths(const fs::path& notes_dir,
                                                           const fs::path& data_directory) {
    auto recorded = read_skipped_files(data_directory);
    if (!recorded)
        return std::unexpected(recorded.error());
    const std::string root = skipped_prefix(notes_dir);
    std::set<std::string> paths;
    for (const SkippedFile& file : *recorded)
        if (file.path.starts_with(root))
            paths.insert(file.path.substr(root.size()));
    return paths;
}

struct StrayWalk {
    const NoteStore& store;
    const std::set<std::string>& recorded;
    const DigestLedger& ledger;
    std::uintmax_t bytes_left;
    std::stop_token stop;
    StrayScan scan;
};

std::expected<void, Error> scan_files(const std::vector<LegacyTree::File>& files, bool trashed,
                                      StrayWalk& walk) {
    for (const LegacyTree::File& file : files) {
        if (walk.stop.stop_requested()) {  // yarım tarama "kalan yok" demez
            walk.scan.truncated = true;
            return {};
        }
        std::string path = report_path(file, trashed);
        if (walk.recorded.contains(path))
            continue;
        if (auto refusal = import_refusal(file)) {
            walk.scan.files.push_back({std::move(path), std::move(*refusal)});
            continue;
        }
        if (file.stat.size > walk.bytes_left) {
            walk.scan.truncated = true;
            return {};
        }
        walk.bytes_left -= file.stat.size;
        auto bytes = slurp_file(file.absolute);
        if (!bytes) {
            walk.scan.files.push_back({std::move(path), "unreadable"});
            continue;
        }
        auto known = file_in_graph(walk.store, walk.ledger, file, std::move(*bytes), trashed);
        if (!known)
            return std::unexpected(known.error());
        if (!*known)
            walk.scan.files.push_back({std::move(path), "new_file"});
    }
    return {};
}

}  // namespace

std::expected<StrayScan, Error> find_stray_files(const NoteStore& store, const fs::path& notes_dir,
                                                 const fs::path& data_directory, StrayLimits limits) {
    if (!is_marked_migrated(notes_dir, data_directory))
        return StrayScan{};
    const auto recorded = recorded_paths(notes_dir, data_directory);
    if (!recorded)
        return std::unexpected(recorded.error());
    const auto ledger = read_digest_ledger(notes_dir, data_directory);
    if (!ledger)
        return std::unexpected(ledger.error());
    const LegacyTrees trees = walk_trees(notes_dir, limits.max_entries);
    StrayWalk walk{store, *recorded, *ledger, limits.max_bytes, limits.stop, {}};
    walk.scan.truncated = trees.live.truncated || trees.trash.truncated;
    MigrationReport unreadable;
    seed_report(trees, unreadable);
    for (SkippedFile& entry : unreadable.failed)
        if (!recorded->contains(entry.path))
            walk.scan.files.push_back(std::move(entry));
    NOTEAPP_TRY(scan_files(trees.live.files, false, walk));
    NOTEAPP_TRY(scan_files(trees.trash.files, true, walk));
    return std::move(walk.scan);
}

std::expected<MigrationReport, Error> migrate_new_notes(const fs::path& notes_dir,
                                                        const fs::path& data_directory) {
    MigrationReport report;
    const LegacyTrees trees = walk_trees(notes_dir);
    seed_report(trees, report);
    NoteStore store(data_directory);
    const auto ledger = read_digest_ledger(notes_dir, data_directory);
    if (!ledger)
        return std::unexpected(ledger.error());
    DigestLedger recorded;
    const auto live = files_not_in_graph(store, *ledger, trees.live.files, false, recorded);
    if (!live)
        return std::unexpected(live.error());
    const auto trash = files_not_in_graph(store, *ledger, trees.trash.files, true, recorded);
    if (!trash)
        return std::unexpected(trash.error());

    const std::vector<LegacyTree::File> new_live = importable_of(*live);
    const std::vector<LegacyTree::File> new_trash = importable_of(*trash);
    if (!new_live.empty() || !new_trash.empty()) {
        auto backup = take_backup(notes_dir, data_directory, new_live, new_trash);
        if (!backup)
            return std::unexpected(backup.error());
        report.backup = *backup;
    }
    NOTEAPP_TRY(import_tree(store, *live, false, report));
    NOTEAPP_TRY(import_tree(store, *trash, true, report));
    importable_folders(trees.live, report);  // klasör kurulmaz: yalnız sınır raporu listede kalsın
    if (report.imported > 0 || !report.conflict_copies.empty())
        NOTEAPP_TRY(store.relink_notes());

    const NoteStore fresh(data_directory);
    const LostNotes lost = lost_notes_of(fresh);
    NOTEAPP_TRY(verify_tree(fresh, *live, false, lost, report, recorded));
    NOTEAPP_TRY(verify_tree(fresh, *trash, true, lost, report, recorded));
    NOTEAPP_TRY(append_digest_ledger(notes_dir, data_directory, recorded));
    NOTEAPP_TRY(save_skipped_files(notes_dir, data_directory, report));
    NOTEAPP_TRY(mark_notes_dir_current(notes_dir, data_directory));

    std::set<std::string> left_out;
    for (const SkippedFile& file : report.failed)
        left_out.insert(file.path);
    const auto add_imported = [&](const std::vector<LegacyTree::File>& files, bool trashed) {
        for (const LegacyTree::File& file : files)
            if (std::string path = report_path(file, trashed); !left_out.contains(path))
                report.new_files.push_back(std::move(path));
    };
    add_imported(new_live, false);
    add_imported(new_trash, true);
    report.complete = report.failed.empty();
    return report;
}

std::expected<MigrationReport, Error>
migrate_legacy_notes(const fs::path& notes_dir, const fs::path& data_directory, MigrationOptions options) {
    MigrationReport report;
    if (const std::error_code made = create_private_directories(data_directory))
        return std::unexpected(io_error(path_utf8(data_directory), made.message()));

    const LegacyTrees trees = walk_trees(notes_dir);
    const LegacyTree& live = trees.live;
    const LegacyTree& trash = trees.trash;
    seed_report(trees, report);

    auto backup = take_backup(notes_dir, data_directory, live.files, trash.files);
    if (!backup)
        return std::unexpected(backup.error());
    report.backup = *backup;

    NoteStore store(data_directory);
    NOTEAPP_TRY(import_everything(store, live, trash, report));
    // Taze bağlantı: önbellekten değil veritabanından okunmuş bayt karşılaştırılır.
    const NoteStore fresh(data_directory);
    const LostNotes lost = lost_notes_of(fresh);
    DigestLedger imported;
    NOTEAPP_TRY(verify_tree(fresh, live.files, false, lost, report, imported));
    NOTEAPP_TRY(verify_tree(fresh, trash.files, true, lost, report, imported));
    NOTEAPP_TRY(append_digest_ledger(notes_dir, data_directory, imported));
    NOTEAPP_TRY(save_skipped_files(notes_dir, data_directory, report));
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
