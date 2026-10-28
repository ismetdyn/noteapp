#include "core/note_store.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/log.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#endif

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBinarySniffBytes = 8192;  // ilk 8 KB'ta NUL varsa ikili
constexpr int kMaxNameAttempts = 10000;          // çakışma döngüsünün sonsuza gitmesine karşı sigorta

Error io_error(std::string_view path, const std::error_code& ec) {
    return Error{Error::Code::Io, "error.io", {{"path", std::string(path)}, {"detail", ec.message()}}};
}

Error io_error(std::string_view path, std::string_view detail) {
    return Error{Error::Code::Io, "error.io", {{"path", std::string(path)}, {"detail", std::string(detail)}}};
}

Error not_found(std::string_view path) {
    return Error{Error::Code::NotFound, "error.not_found", {{"path", std::string(path)}}};
}

Error invalid_path(std::string_view reason, std::string_view path) {
    return Error{Error::Code::InvalidPath,
                 "error.invalid_path",
                 {{"reason", std::string(reason)}, {"path", std::string(path)}}};
}

Error already_exists(std::string_view path) {
    return Error{Error::Code::AlreadyExists, "error.already_exists", {{"path", std::string(path)}}};
}

constexpr const char* kKeepFileName = ".keep";  // boş klasörü senkronda (git) ayakta tutan gizli dosya

// Hedef varsa ezmeyen taşıma. Windows: MOVEFILE_REPLACE_EXISTING verilmez. Linux: renameat2
// (RENAME_NOREPLACE). Diğerleri: dosyada link+unlink, dizinde var-mı denetimi + rename.
// ponytail: link+unlink çökme anında iki ad bırakabilir; dizin yolundaki denetim-rename arası dar bir
// yarış penceresi var. Yalnız renameat2'siz çekirdek/dosya sisteminde devreye girer.
std::error_code move_no_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), 0) != 0)
        return {};
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
        return {};
    if (errno != EINVAL && errno != ENOSYS)
        return {errno, std::generic_category()};
#endif
    std::error_code ec;
    if (fs::is_directory(from, ec)) {
        if (fs::exists(to, ec))
            return std::make_error_code(std::errc::file_exists);
        fs::rename(from, to, ec);
        return ec;
    }
    fs::create_hard_link(from, to, ec);
    if (ec)
        return ec;
    fs::remove(from, ec);
    return ec;
#endif
}

// Taşıma/silme yalnız gerçek dosyaya dokunur. stat_file bağı izlemez, bağı (junction dahil) normal dosya
// saymaz; resolve() bağı zaten reddeder, bu ikinci kilit.
std::expected<void, Error> require_regular_note(const NotePath& path, const fs::path& resolved) {
    std::error_code ec;
    if (!fs::exists(resolved, ec))
        return std::unexpected(not_found(path.string()));
    if (const auto stat = stat_file(resolved); !stat || !stat->regular)
        return std::unexpected(io_error(path.string(), "not a regular file"));
    return {};
}

// dir'in altını alttan üste gezer (önce klasörün içi, sonra kendisi) ve bağları izlemez: stat_file bağı
// klasör saymaz, içine inilmez. visit hata dönerse gezinti durur.
template <typename Visit> std::error_code walk_bottom_up(const fs::path& dir, const Visit& visit) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path entry = it->path();
        const auto stat = stat_file(entry);
        if (!stat)
            return stat.error();
        if (stat->directory) {
            if (const std::error_code sub = walk_bottom_up(entry, visit))
                return sub;
        }
        if (const std::error_code visited = visit(entry, *stat))
            return visited;
    }
    return ec;
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool is_hidden(const fs::path& p) {
    const std::u8string name = p.filename().u8string();
    return !name.empty() && name.front() == u8'.';
}

#ifdef _WIN32
// Var olan bileşenin diskteki uzun adı: 8.3 kısa adla ("GIT~1") verilse de gerçek adı (".git") döner
// (FindFirstFileExW kısa adla da eşleşir, cFileName uzun addır).
std::expected<fs::path, std::error_code> long_name(const fs::path& path) {
    WIN32_FIND_DATAW data;
    const HANDLE find =
        FindFirstFileExW(path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE)
        return std::unexpected(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    FindClose(find);
    return fs::path(data.cFileName);
}
#endif

std::string normalize_line_endings(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;  // CR atlanır, LF kalır
        out += text[i];
    }
    return out;
}

}  // namespace

bool looks_binary(std::string_view bytes) {
    return bytes.substr(0, kBinarySniffBytes).find('\0') != std::string_view::npos;
}

NoteStore::NoteStore(fs::path root, std::size_t slug_max_length)
    : root_(std::move(root)), slug_max_length_(slug_max_length) {}

// Her işlemin geçtiği tek kapı. (1) Kökün altındaki var olan her bileşen (ara klasörler ve son öğe) bağ
// olmamalı: notlar hiçbir bağın ardında yaşamaz. MinGW libstdc++ junction'ı bağ saymaz (is_symlink false,
// weakly_canonical çözmez); stat_file sayar, kural her platformda aynıdır. Kökün kendisi bağ olabilir
// (ör. notes_dir → bulut klasörü), denetlenmez. (2) Kanonik yol kökün içinde kalmalı (".." zaten
// NotePath'te elenir). weakly_canonical var olmayan kuyruğu da kabul eder (create için).
std::expected<fs::path, Error> NoteStore::resolve(const NotePath& path) const {
    fs::path current = root_;
    for (const fs::path& component : path.value()) {
        current /= component;
        const auto stat = stat_file(current);
        if (!stat) {
            // Yok ya da üstü dosya: altında bağ olamaz, kuyruk (create) serbest. Başka hata (erişim vb.)
            // bağ olup olmadığını gizler: güvenli tarafta kalınır.
            if (stat.error() == std::errc::no_such_file_or_directory ||
                stat.error() == std::errc::not_a_directory)
                break;
            return std::unexpected(io_error(path.string(), stat.error()));
        }
        if (stat->symlink)
            return std::unexpected(invalid_path("link", path.string()));
#ifdef _WIN32
        // 8.3 kısa adı gizli adın kuralını atlatır: "GIT~1/config" NotePath'ten geçer ama ".git/config"tir
        // (8.3 C:'de açık). Kısa adın uzun adı da gizli ad kuralından geçmeli.
        // ponytail: yalnız '~' içeren bileşen açılır (otomatik 8.3 adları hep '~' taşır); yönetici
        // SetFileShortNameW ile tildesiz kısa ad verdiyse kaçar — gerekirse her bileşen açılır (+1
        // FindFirstFile).
        if (component.native().find(L'~') != std::wstring::npos) {
            const auto name = long_name(current);
            if (!name)
                return std::unexpected(io_error(path.string(), name.error()));
            if (is_hidden(*name))
                return std::unexpected(invalid_path("hidden", path.string()));
        }
#endif
    }

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(root_, ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    const fs::path target = fs::weakly_canonical(root / path.value(), ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    const fs::path relative = target.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..")
        return std::unexpected(invalid_path("outside_root", path.string()));
    return target;
}

std::vector<NoteInfo> NoteStore::list() const {
    std::vector<NoteInfo> notes;
    // Sembolik bağlar listelenmez (scan_files atlar): hedefi kök dışına kaçabilir.
    for (const ScannedFile& file : scan_files(root_)) {
        auto parsed = NotePath::parse(file.relative);
        if (!parsed) {
            log_warn("note list: skipped '" + file.relative + "': invalid path (" +
                     (parsed.error().args.empty() ? "" : parsed.error().args.front().second) + ")");
            continue;
        }
        notes.push_back(NoteInfo{*parsed, file.stat.mtime, file.stat.size});
    }
    std::sort(notes.begin(), notes.end(), [](const NoteInfo& a, const NoteInfo& b) {
        if (a.mtime != b.mtime)
            return a.mtime > b.mtime;
        return a.path.string() < b.path.string();
    });
    return notes;
}

std::vector<std::string> NoteStore::folders() const {
    std::vector<std::string> result;
    std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        if (is_hidden(entry.path())) {
            if (entry.is_directory(entry_ec))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_directory(entry_ec))
            continue;
        // MinGW libstdc++ junction'ı bağ saymaz ve içine iner; stat_file sayar. Bağ listelenmez, içine
        // inilmez.
        if (const auto stat = stat_file(entry.path()); !stat || !stat->directory) {
            it.disable_recursion_pending();
            continue;
        }
        result.push_back(to_utf8(entry.path().lexically_relative(root_)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::expected<fs::path, Error> NoteStore::absolute_path(const NotePath& path) const {
    auto target = resolve(path);
    if (!target)
        return target;
    std::error_code ec;
    fs::path absolute =
        fs::absolute(*target, ec);  // kökün hiçbir parçası yokken weakly_canonical göreli kalır
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    return absolute;
}

std::expected<NoteContent, Error> NoteStore::read(const NotePath& path) const {
    const auto target = resolve(path);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!fs::exists(*target, ec))
        return std::unexpected(not_found(path.string()));
    // mtime içerikten ÖNCE okunur: arada bir yazım olursa elimizdeki mtime eski kalır ve sonraki
    // write() bunu ModifiedExternally olarak yakalar (tersi sırada sessiz veri kaybı olurdu).
    const auto stat = stat_file(*target);
    if (!stat)
        return std::unexpected(io_error(path.string(), stat.error()));
    // stat_file bağı izlemez: bağın kendi mtime'ı hedefteki dış düzenlemeyi göstermezdi, okunmaz.
    if (!stat->regular)
        return std::unexpected(io_error(path.string(), "not a regular file"));
    const auto mtime = stat->mtime;
    std::ifstream in(*target, std::ios::binary);
    if (!in)
        return std::unexpected(io_error(path.string(), "cannot open file"));
    std::string text(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        return std::unexpected(io_error(path.string(), "read failed"));
    return NoteContent{std::move(text), mtime};
}

std::expected<NotePath, Error> NoteStore::create(const NewNote& note,
                                                 std::chrono::system_clock::time_point now) {
    if (note.type.empty() || note.type.find('.') != std::string::npos)
        return std::unexpected(invalid_path("invalid_type", note.type));
    if (note.name.find_first_of("/\\") != std::string::npos)
        return std::unexpected(invalid_path("invalid_name", note.name));
    if (!note.folder.empty()) {
        if (auto folder = NotePath::parse(note.folder); !folder)
            return std::unexpected(folder.error());
    }

    const std::string base =
        note.name.empty() ? slugify(first_non_empty_line(note.content), now, slug_max_length_) : note.name;
    const std::string prefix = note.folder.empty() ? std::string() : note.folder + "/";
    const std::string content = normalize_line_endings(note.content);

    for (int attempt = 1; attempt <= kMaxNameAttempts; ++attempt) {
        const std::string stem = attempt == 1 ? base : base + "-" + std::to_string(attempt);
        auto path = NotePath::parse(prefix + stem + "." + note.type);
        if (!path)
            return std::unexpected(path.error());
        auto target = resolve(*path);
        if (!target)
            return std::unexpected(target.error());

        std::error_code ec;
        fs::create_directories(target->parent_path(), ec);
        if (ec)
            return std::unexpected(io_error(path->string(), ec));

        const FileWriteResult written = write_new_file(*target, content);
        if (written.already_exists)
            continue;  // ad dolu (yarışta da): bir sonraki ek
        if (written.ec) {
            fs::remove(*target, ec);  // yarım dosya bırakma
            return std::unexpected(io_error(path->string(), written.ec));
        }
        return *path;
    }
    return std::unexpected(
        Error{Error::Code::AlreadyExists, "error.already_exists", {{"path", prefix + base}}});
}

std::expected<fs::file_time_type, Error> NoteStore::write(const NotePath& path, std::string_view content,
                                                          fs::file_time_type expected_mtime) {
    const auto target = resolve(path);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!fs::exists(*target, ec))
        return std::unexpected(not_found(path.string()));
    const auto current = stat_file(*target);
    if (!current)
        return std::unexpected(io_error(path.string(), current.error()));
    // ponytail: denetim ile rename arasında küçük bir yarış penceresi var; tek kullanıcılı yerel
    // uygulama için kabul edildi, kilit dosyası gerekirse buraya eklenir. Ayrıca mtime saat tıkı
    // (~1–16 ms) içinde aynı kalır: okumayla aynı tıkta yapılan dış düzenleme kaçar.
    if (current->mtime != expected_mtime)
        return std::unexpected(
            Error{Error::Code::ModifiedExternally, "error.modified_externally", {{"path", path.string()}}});

    // Hata mesajında kullanıcının gördüğü göreli yol kalsın diye "path" argümanı burada yeniden yazılır.
    if (auto written = write_file_atomically(*target, content); !written) {
        Error error = std::move(written.error());
        for (auto& [key, value] : error.args) {
            if (key == "path")
                value = path.string();
        }
        return std::unexpected(std::move(error));
    }
    const auto written = stat_file(*target);
    if (!written)
        return std::unexpected(io_error(path.string(), written.error()));
    return written->mtime;
}

std::expected<NotePath, Error> NoteStore::move(const NotePath& from, const NotePath& to) {
    const auto source = resolve(from);
    if (!source)
        return std::unexpected(source.error());
    const auto target = resolve(to);
    if (!target)
        return std::unexpected(target.error());
    if (auto ok = require_regular_note(from, *source); !ok)
        return std::unexpected(ok.error());

    std::error_code ec;
    if (fs::exists(*target, ec))
        return std::unexpected(already_exists(to.string()));
    fs::create_directories(target->parent_path(), ec);
    if (ec)
        return std::unexpected(io_error(to.string(), ec));
    if (const std::error_code moved = move_no_replace(*source, *target)) {
        // Denetim ile taşıma arasında hedef oluştuysa (yarış) ezmek yerine AlreadyExists.
        if (fs::exists(*target, ec))
            return std::unexpected(already_exists(to.string()));
        return std::unexpected(io_error(from.string(), moved));
    }
    return to;
}

std::expected<void, Error> NoteStore::remove(const NotePath& path) {
    const auto target = resolve(path);
    if (!target)
        return std::unexpected(target.error());
    if (auto ok = require_regular_note(path, *target); !ok)
        return std::unexpected(ok.error());
    std::error_code ec;
    if (!fs::remove(*target, ec) || ec)
        return std::unexpected(io_error(path.string(), ec));
    return {};
}

std::expected<void, Error> NoteStore::create_folder(std::string_view folder) {
    const auto parsed = NotePath::parse(folder);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto target = resolve(*parsed);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (fs::exists(*target, ec) && !fs::is_directory(*target, ec))
        return std::unexpected(already_exists(parsed->string()));
    fs::create_directories(*target, ec);
    if (ec)
        return std::unexpected(io_error(parsed->string(), ec));
    // .keep zaten varsa (already_exists) sorun yok: çağrı yinelenebilir.
    if (const FileWriteResult kept = write_new_file(*target / kKeepFileName, "");
        kept.ec && !kept.already_exists)
        return std::unexpected(io_error(parsed->string(), kept.ec));
    return {};
}

std::expected<void, Error> NoteStore::rename_folder(std::string_view from, std::string_view to) {
    const auto from_path = NotePath::parse(from);
    if (!from_path)
        return std::unexpected(from_path.error());
    const auto to_path = NotePath::parse(to);
    if (!to_path)
        return std::unexpected(to_path.error());
    const auto source = resolve(*from_path);
    if (!source)
        return std::unexpected(source.error());
    const auto target = resolve(*to_path);
    if (!target)
        return std::unexpected(target.error());

    std::error_code ec;
    if (!fs::exists(*source, ec))
        return std::unexpected(not_found(from_path->string()));
    // stat_file bağı (junction dahil) klasör saymaz; resolve() bağı zaten reddeder, bu ikinci kilit.
    if (const auto stat = stat_file(*source); !stat || !stat->directory)
        return std::unexpected(io_error(from_path->string(), "not a directory"));
    const std::string old_prefix = from_path->string() + "/";
    const std::string new_prefix = to_path->string() + "/";
    // Kendi içine taşıma, hedef klasörleri oluşturmadan önce reddedilir (artık dizin bırakmasın).
    if (new_prefix.starts_with(old_prefix))
        return std::unexpected(invalid_path("into_itself", to_path->string()));
    if (fs::exists(*target, ec))
        return std::unexpected(already_exists(to_path->string()));
    fs::create_directories(target->parent_path(), ec);
    if (ec)
        return std::unexpected(io_error(to_path->string(), ec));
    if (const std::error_code moved = move_no_replace(*source, *target)) {
        if (fs::exists(*target, ec))
            return std::unexpected(already_exists(to_path->string()));
        return std::unexpected(io_error(from_path->string(), moved));
    }
    return {};
}

std::expected<void, Error> NoteStore::remove_folder(std::string_view folder, bool force) {
    const auto parsed = NotePath::parse(folder);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto target = resolve(*parsed);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!fs::exists(*target, ec))
        return std::unexpected(not_found(parsed->string()));
    // stat_file bağı (junction dahil) klasör saymaz; resolve() bağı zaten reddeder, bu ikinci kilit.
    if (const auto stat = stat_file(*target); !stat || !stat->directory)
        return std::unexpected(io_error(parsed->string(), "not a directory"));

    // Silmeden önce tam tarama (force'ta da): içeride bağ varsa hiçbir şey silinmez. Bağın hedefi kök
    // dışında olabilir; MinGW libstdc++ fs::remove_all junction'ı klasör sanıp içinden geçerek siler.
    // Klasörler ve .keep dışındaki her şey (gizli/bilinmeyen dosyalar dahil) "dolu" sayılır; tarama
    // belirsizse (hata) silinmez.
    bool has_link = false;
    std::size_t count = 0;
    if (const std::error_code scanned =
            walk_bottom_up(*target, [&](const fs::path& entry, const FileStat& stat) {
                has_link = has_link || stat.symlink;
                if (!stat.directory && entry.filename() != kKeepFileName)
                    ++count;
                return std::error_code{};
            }))
        return std::unexpected(io_error(parsed->string(), scanned));
    if (has_link)
        return std::unexpected(invalid_path("contains_link", parsed->string()));
    if (!force && count > 0)
        return std::unexpected(Error{Error::Code::NotEmpty,
                                     "error.not_empty",
                                     {{"path", parsed->string()}, {"count", std::to_string(count)}}});

    // fs::remove_all yerine bağ izlemeyen kendi gezintimiz: tarama ile silme arasında bir bağ belirse bile
    // yalnız bağın kendisi silinir (fs::remove junction'da RemoveDirectoryW: hedefe dokunmaz), içine inilmez.
    if (const std::error_code removed = walk_bottom_up(*target, [](const fs::path& entry, const FileStat&) {
            std::error_code remove_ec;
            fs::remove(entry, remove_ec);
            return remove_ec;
        }))
        return std::unexpected(io_error(parsed->string(), removed));
    fs::remove(*target, ec);
    if (ec)
        return std::unexpected(io_error(parsed->string(), ec));
    return {};
}

}  // namespace noteapp::core
