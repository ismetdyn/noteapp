#include "core/atomic_file.hpp"

#include <algorithm>
#include <array>
#include <random>

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

Error io_error(const fs::path& path, const std::error_code& ec) {
    const std::u8string u = path.generic_u8string();
    return Error{
        Error::Code::Io,
        "error.io",
        {{"path", std::string(reinterpret_cast<const char*>(u.data()), u.size())}, {"detail", ec.message()}}};
}

}  // namespace

FileWriteResult write_new_file(const fs::path& file, std::string_view content, bool owner_only) {
    FileWriteResult result;
#ifdef _WIN32
    (void)owner_only;  // Windows: izinler üst dizinden miras kalır (kullanıcıya özel durum dizini)
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
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, owner_only ? 0600 : 0666);
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

std::expected<void, Error> write_file_atomically(const fs::path& target, std::string_view bytes) {
    // Aynı dizinde geçici dosya: rename aynı birimde kalır, yani atomik olur.
    fs::path tmp = target.parent_path() / ".";
    tmp += target.filename();
    tmp += "." + random_hex() + ".tmp";

    std::error_code ec;
    const FileWriteResult written = write_new_file(tmp, bytes);
    if (written.ec) {
        if (!written.already_exists)
            fs::remove(tmp, ec);  // başkasının dosyasına dokunma
        return std::unexpected(io_error(target, written.ec));
    }
    // MinGW (GCC 13) ve POSIX'te std::filesystem::rename mevcut dosyanın üzerine yazar.
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return std::unexpected(io_error(target, ec));
    }
    return {};
}

}  // namespace noteapp::core
