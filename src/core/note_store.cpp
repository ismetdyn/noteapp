#include "core/note_store.hpp"

#include "core/atomic_file.hpp"
#include "core/log.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <array>
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

// Taşıma/silme yalnız gerçek dosyaya dokunur: sembolik bağ listelenmez, hedefi de silinmemeli.
std::expected<void, Error> require_regular_note(const fs::path& root, const NotePath& path,
                                                const fs::path& resolved) {
    std::error_code ec;
    if (!fs::exists(resolved, ec))
        return std::unexpected(not_found(path.string()));
    if (fs::is_symlink(root / path.value(), ec) || !fs::is_regular_file(resolved, ec))
        return std::unexpected(io_error(path.string(), "not a regular file"));
    return {};
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool is_hidden(const fs::path& p) {
    const std::u8string name = p.filename().u8string();
    return !name.empty() && name.front() == u8'.';
}

bool has_nul_in_head(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<char, kBinarySniffBytes> head{};
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    return std::find(head.begin(), head.begin() + in.gcount(), '\0') != head.begin() + in.gcount();
}

// İlk dolu (yalnız boşluk olmayan) satır; slug bundan türetilir.
std::string first_non_empty_line(std::string_view content) {
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos)
            end = content.size();
        const std::string_view line = content.substr(start, end - start);
        if (line.find_first_not_of(" \t\r") != std::string_view::npos)
            return std::string(line);
        start = end + 1;
    }
    return {};
}

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

NoteStore::NoteStore(fs::path root, std::size_t slug_max_length)
    : root_(std::move(root)), slug_max_length_(slug_max_length) {}

// Kanonik yol kökün içinde değilse reddeder: ".." zaten NotePath'te elenir, burada asıl hedef
// sembolik bağ üzerinden kaçıştır. weakly_canonical var olmayan kuyruğu da kabul eder (create için).
std::expected<fs::path, Error> NoteStore::resolve(const NotePath& path) const {
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
    std::unordered_map<std::string, BinaryCacheEntry> seen;  // silinen dosyaların kaydı düşsün
    std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        if (is_hidden(entry.path())) {
            if (entry.is_directory(ec))
                it.disable_recursion_pending();  // .git gibi dizinlere inilmez
            continue;
        }
        // Sembolik bağlar listelenmez: hedefi kök dışına kaçabilir.
        // Tek symlink_status çağrısı: ayrı is_symlink + is_regular_file her dosyada fazladan stat demekti.
        std::error_code entry_ec;
        if (entry.symlink_status(entry_ec).type() != fs::file_type::regular)
            continue;
        const std::string relative = to_utf8(entry.path().lexically_relative(root_));
        auto parsed = NotePath::parse(relative);
        if (!parsed) {
            log_warn("note list: skipped '" + relative + "': invalid path (" +
                     (parsed.error().args.empty() ? "" : parsed.error().args.front().second) + ")");
            continue;
        }
        const auto mtime = entry.last_write_time(entry_ec);
        const auto size = entry.file_size(entry_ec);
        if (entry_ec) {
            log_warn("note list: skipped '" + relative + "': " + entry_ec.message());
            continue;
        }
        bool binary = false;
        if (const auto cached = binary_cache_.find(relative);
            cached != binary_cache_.end() && cached->second.mtime == mtime && cached->second.size == size) {
            binary = cached->second.binary;
        } else {
            // ponytail: aynı mtime + aynı boyutla içeriği değişen dosya (mtime çözünürlüğü içinde) eski
            // bayrakta kalır; gerekirse anahtara içerik özeti eklenir.
            ++head_reads_;
            binary = has_nul_in_head(entry.path());
        }
        seen.insert_or_assign(relative, BinaryCacheEntry{mtime, size, binary});
        notes.push_back(NoteInfo{*parsed, mtime, size, binary});
    }
    binary_cache_ = std::move(seen);
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
        if (!entry.is_symlink(entry_ec) && entry.is_directory(entry_ec))
            result.push_back(to_utf8(entry.path().lexically_relative(root_)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::expected<NoteContent, Error> NoteStore::read(const NotePath& path) const {
    const auto target = resolve(path);
    if (!target)
        return std::unexpected(target.error());
    std::error_code ec;
    if (!fs::exists(*target, ec))
        return std::unexpected(not_found(path.string()));
    if (!fs::is_regular_file(*target, ec))
        return std::unexpected(io_error(path.string(), "not a regular file"));
    // mtime içerikten ÖNCE okunur: arada bir yazım olursa elimizdeki mtime eski kalır ve sonraki
    // write() bunu ModifiedExternally olarak yakalar (tersi sırada sessiz veri kaybı olurdu).
    const auto mtime = fs::last_write_time(*target, ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
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
    const auto current = fs::last_write_time(*target, ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    // ponytail: denetim ile rename arasında küçük bir yarış penceresi var; tek kullanıcılı yerel
    // uygulama için kabul edildi, kilit dosyası gerekirse buraya eklenir.
    if (current != expected_mtime)
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
    const auto new_mtime = fs::last_write_time(*target, ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    return new_mtime;
}

std::expected<NotePath, Error> NoteStore::move(const NotePath& from, const NotePath& to) {
    const auto source = resolve(from);
    if (!source)
        return std::unexpected(source.error());
    const auto target = resolve(to);
    if (!target)
        return std::unexpected(target.error());
    if (auto ok = require_regular_note(root_, from, *source); !ok)
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

    binary_cache_.erase(to.string());
    if (auto node = binary_cache_.extract(from.string())) {
        node.key() = to.string();
        binary_cache_.insert(std::move(node));
    }
    return to;
}

std::expected<void, Error> NoteStore::remove(const NotePath& path) {
    const auto target = resolve(path);
    if (!target)
        return std::unexpected(target.error());
    if (auto ok = require_regular_note(root_, path, *target); !ok)
        return std::unexpected(ok.error());
    std::error_code ec;
    if (!fs::remove(*target, ec) || ec)
        return std::unexpected(io_error(path.string(), ec));
    binary_cache_.erase(path.string());
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
    if (fs::is_symlink(root_ / from_path->value(), ec) || !fs::is_directory(*source, ec))
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

    // Klasörün altındaki önbellek kayıtları yeni yola taşınır (dosyaların başı yeniden okunmasın).
    using Node = decltype(binary_cache_)::node_type;
    std::vector<Node> moved_entries;
    for (auto it = binary_cache_.begin(); it != binary_cache_.end();) {
        auto next = std::next(it);
        if (it->first.starts_with(old_prefix))
            moved_entries.push_back(binary_cache_.extract(it));
        it = next;
    }
    for (Node& node : moved_entries) {
        node.key() = new_prefix + node.key().substr(old_prefix.size());
        binary_cache_.insert(std::move(node));
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
    // Bağ yoluyla kök içindeki başka bir klasörün silinmesine izin verilmez.
    if (fs::is_symlink(root_ / parsed->value(), ec) || !fs::is_directory(*target, ec))
        return std::unexpected(io_error(parsed->string(), "not a directory"));

    if (!force) {
        // Dizinler ve .keep dışındaki her şey (gizli/bilinmeyen dosyalar, bağlar dahil) "dolu" sayılır;
        // tarama belirsizse (hata) silinmez.
        std::size_t count = 0;
        fs::recursive_directory_iterator it(*target, fs::directory_options::none, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code entry_ec;
            const bool is_dir = it->symlink_status(entry_ec).type() == fs::file_type::directory;
            if (!is_dir && it->path().filename() != kKeepFileName)
                ++count;
        }
        if (ec)
            return std::unexpected(io_error(parsed->string(), ec));
        if (count > 0)
            return std::unexpected(Error{Error::Code::NotEmpty,
                                         "error.not_empty",
                                         {{"path", parsed->string()}, {"count", std::to_string(count)}}});
    }

    fs::remove_all(*target, ec);
    if (ec)
        return std::unexpected(io_error(parsed->string(), ec));
    const std::string prefix = parsed->string() + "/";
    std::erase_if(binary_cache_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
    return {};
}

}  // namespace noteapp::core
