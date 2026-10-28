#include "core/note_store.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
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

struct FileWriteResult {
    std::error_code ec;
    bool already_exists = false;
};

// Dosyayı YALNIZ yoksa oluşturur (O_EXCL / CREATE_NEW: yarış olsa bile ezmez), içeriği yazar,
// diske zorlar (fsync / FlushFileBuffers) ve kapatır. Hatada dosyayı silmek çağıranın işidir.
FileWriteResult write_new_file(const fs::path& file, std::string_view content) {
    FileWriteResult result;
#ifdef _WIN32
    const HANDLE h =
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        result.already_exists = err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS;
        result.ec = std::error_code(static_cast<int>(err), std::system_category());
        return result;
    }
    std::size_t done = 0;
    while (done < content.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - done, 1U << 30));
        DWORD written = 0;
        if (!WriteFile(h, content.data() + done, chunk, &written, nullptr)) {
            result.ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            break;
        }
        done += written;
    }
    if (!result.ec && !FlushFileBuffers(h))
        result.ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    CloseHandle(h);
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        result.already_exists = errno == EEXIST;
        result.ec = std::error_code(errno, std::generic_category());
        return result;
    }
    std::size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            result.ec = std::error_code(errno, std::generic_category());
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (!result.ec && ::fsync(fd) != 0)
        result.ec = std::error_code(errno, std::generic_category());
    ::close(fd);
#endif
    return result;
}

std::string random_hex() {
    std::random_device rd;
    constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out;
    for (int i = 0; i < 4; ++i) {
        const unsigned byte = rd() & 0xFFU;
        out += digits[byte >> 4];
        out += digits[byte & 0xFU];
    }
    return out;
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

NoteStore::NoteStore(fs::path root) : root_(std::move(root)) {}

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
        std::error_code entry_ec;
        if (entry.is_symlink(entry_ec) || !entry.is_regular_file(entry_ec))
            continue;
        auto parsed = NotePath::parse(to_utf8(entry.path().lexically_relative(root_)));
        if (!parsed)
            continue;  // TODO(T09): log uyarısı
        const auto mtime = entry.last_write_time(entry_ec);
        const auto size = entry.file_size(entry_ec);
        if (entry_ec)
            continue;  // TODO(T09): log uyarısı
        notes.push_back(NoteInfo{*parsed, mtime, size, has_nul_in_head(entry.path())});
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

    const std::string base = note.name.empty() ? slugify(first_non_empty_line(note.content), now) : note.name;
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

    // Aynı dizinde geçici dosya: rename aynı birimde kalır, yani atomik olur.
    fs::path tmp = target->parent_path() / ".";
    tmp += target->filename();
    tmp += "." + random_hex() + ".tmp";

    const FileWriteResult written = write_new_file(tmp, content);
    if (written.ec) {
        if (!written.already_exists)
            fs::remove(tmp, ec);  // başkasının dosyasına dokunma
        return std::unexpected(io_error(path.string(), written.ec));
    }
    // MinGW (GCC 13) ve POSIX'te std::filesystem::rename mevcut dosyanın üzerine yazar.
    fs::rename(tmp, *target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return std::unexpected(io_error(path.string(), ec));
    }
    const auto new_mtime = fs::last_write_time(*target, ec);
    if (ec)
        return std::unexpected(io_error(path.string(), ec));
    return new_mtime;
}

}  // namespace noteapp::core
