#include "graph/secure_file.hpp"

#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace noteapp::graph::detail {

namespace {

Error file_error(std::string reason) {
    return Error{Error::Code::Io, "graph.log_io", {{"reason", std::move(reason)}}};
}

#ifdef _WIN32

constexpr NativeFile kNoFile = -1;

HANDLE handle_of(NativeFile file) {
    return reinterpret_cast<HANDLE>(file);
}

NativeFile native_of(HANDLE handle) {
    return reinterpret_cast<NativeFile>(handle);
}

void close_native(NativeFile file) {
    if (file != kNoFile)
        CloseHandle(handle_of(file));
}

// FILE_FLAG_OPEN_REPARSE_POINT: bağın kendisi açılır, hedefi değil; özniteliğine bakıp reddederiz.
Result<std::uint64_t> check_regular(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info))
        return std::unexpected(file_error("not_regular_file"));
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return std::unexpected(file_error("symlink_refused"));
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return std::unexpected(file_error("not_regular_file"));
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
}

Result<std::optional<InputFile>> open_input(const std::filesystem::path& path, auto&& make) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return std::optional<InputFile>{};
        return std::unexpected(file_error("open_failed"));
    }
    const Result<std::uint64_t> size = check_regular(handle);
    if (!size) {
        CloseHandle(handle);
        return std::unexpected(size.error());
    }
    return make(native_of(handle), *size);
}

Result<NativeFile> open_output_new(const std::filesystem::path& path) {
    // CREATE_NEW var olan her yolda (bağ dahil) düşer.
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(file_error("create_failed"));
    return native_of(handle);
}

Result<NativeFile> open_output_append(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(file_error("open_failed"));
    if (const Result<std::uint64_t> regular = check_regular(handle); !regular) {
        CloseHandle(handle);
        return std::unexpected(regular.error());
    }
    return native_of(handle);
}

Result<void> read_exact(NativeFile file, std::uint64_t offset, std::span<std::byte> out) {
    std::size_t done = 0;
    while (done < out.size()) {
        OVERLAPPED position{};
        const std::uint64_t at = offset + done;
        position.Offset = static_cast<DWORD>(at);
        position.OffsetHigh = static_cast<DWORD>(at >> 32U);
        // ReadFile DWORD alır; çerçeve sınırı 64 MiB olduğundan taşmaz.
        DWORD count = 0;
        if (!ReadFile(handle_of(file), out.data() + done, static_cast<DWORD>(out.size() - done), &count,
                      &position) ||
            count == 0)
            return std::unexpected(file_error("read_failed"));
        done += count;
    }
    return {};
}

Result<void> write_all(NativeFile file, std::span<const std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD count = 0;
        if (!WriteFile(handle_of(file), bytes.data() + done, static_cast<DWORD>(bytes.size() - done), &count,
                       nullptr) ||
            count == 0)
            return std::unexpected(file_error("write_failed"));
        done += count;
    }
    return {};
}

Result<void> close_checked(NativeFile file) {
    if (!CloseHandle(handle_of(file)))
        return std::unexpected(file_error("write_failed"));
    return {};
}

#else

constexpr NativeFile kNoFile = -1;

void close_native(NativeFile file) {
    if (file != kNoFile)
        ::close(static_cast<int>(file));
}

// O_NONBLOCK: FIFO açılışta bloklamasın; fstat onu aşağıda eler.
Result<std::uint64_t> check_regular(int file) {
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode))
        return std::unexpected(file_error("not_regular_file"));
    return static_cast<std::uint64_t>(info.st_size);
}

Result<std::optional<InputFile>> open_input(const std::filesystem::path& path, auto&& make) {
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return std::optional<InputFile>{};
        return std::unexpected(file_error(errno == ELOOP ? "symlink_refused" : "open_failed"));
    }
    const Result<std::uint64_t> size = check_regular(file);
    if (!size) {
        ::close(file);
        return std::unexpected(size.error());
    }
    return make(NativeFile{file}, *size);
}

Result<NativeFile> open_output_new(const std::filesystem::path& path) {
    constexpr mode_t kOwnerOnly = 0600;
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kOwnerOnly);
    if (file < 0)
        return std::unexpected(file_error("create_failed"));
    return NativeFile{file};
}

Result<NativeFile> open_output_append(const std::filesystem::path& path) {
    const int file = ::open(path.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0)
        return std::unexpected(file_error(errno == ELOOP ? "symlink_refused" : "open_failed"));
    if (const Result<std::uint64_t> regular = check_regular(file); !regular) {
        ::close(file);
        return std::unexpected(regular.error());
    }
    return NativeFile{file};
}

Result<void> read_exact(NativeFile file, std::uint64_t offset, std::span<std::byte> out) {
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t count = ::pread(static_cast<int>(file), out.data() + done, out.size() - done,
                                      static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(file_error("read_failed"));
        done += static_cast<std::size_t>(count);
    }
    return {};
}

Result<void> write_all(NativeFile file, std::span<const std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::write(static_cast<int>(file), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(file_error("write_failed"));
        done += static_cast<std::size_t>(count);
    }
    return {};
}

Result<void> close_checked(NativeFile file) {
    if (::close(static_cast<int>(file)) != 0)
        return std::unexpected(file_error("write_failed"));
    return {};
}

#endif

}  // namespace

Result<std::optional<InputFile>> InputFile::open(const std::filesystem::path& path) {
    return open_input(path, [](NativeFile file, std::uint64_t size) {
        return std::optional<InputFile>{InputFile(file, size)};
    });
}

InputFile::InputFile(InputFile&& other) noexcept
    : file_(std::exchange(other.file_, kNoFile)), size_(other.size_) {}

InputFile& InputFile::operator=(InputFile&& other) noexcept {
    if (this != &other) {
        close_native(file_);
        file_ = std::exchange(other.file_, kNoFile);
        size_ = other.size_;
    }
    return *this;
}

InputFile::~InputFile() {
    close_native(file_);
}

Result<void> InputFile::read_at(std::uint64_t offset, std::span<std::byte> out) {
    return read_exact(file_, offset, out);
}

Result<OutputFile> OutputFile::create_new(const std::filesystem::path& path) {
    const Result<NativeFile> file = open_output_new(path);
    if (!file)
        return std::unexpected(file.error());
    return OutputFile(*file);
}

Result<OutputFile> OutputFile::open_append(const std::filesystem::path& path) {
    const Result<NativeFile> file = open_output_append(path);
    if (!file)
        return std::unexpected(file.error());
    return OutputFile(*file);
}

OutputFile::OutputFile(OutputFile&& other) noexcept : file_(std::exchange(other.file_, kNoFile)) {}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept {
    if (this != &other) {
        close_native(file_);
        file_ = std::exchange(other.file_, kNoFile);
    }
    return *this;
}

OutputFile::~OutputFile() {
    close_native(file_);
}

Result<void> OutputFile::write(std::span<const std::byte> bytes) {
    return write_all(file_, bytes);
}

Result<void> OutputFile::close() {
    return close_checked(std::exchange(file_, kNoFile));
}

bool is_symbolic_link(const std::filesystem::path& path) {
    std::error_code code;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, code);
    return !code && std::filesystem::is_symlink(status);
}

}  // namespace noteapp::graph::detail
