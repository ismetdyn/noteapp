#include "core/note_store.hpp"

#include "core/atomic_file.hpp"
#include "core/log.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>

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

}  // namespace noteapp::core
