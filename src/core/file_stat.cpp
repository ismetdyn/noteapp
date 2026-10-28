#include "core/file_stat.hpp"

#include "core/log.hpp"

#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

std::error_code last_error() {
    return {static_cast<int>(GetLastError()), std::system_category()};
}

// FILETIME: 1601-01-01'den beri 100 ns. stat_file ve tarama aynı dönüşümü kullanır: aynı dosya için
// list(), read() ve write() birebir eşit mtime görür.
// ponytail: file_time_type aralığı dışındaki (libstdc++: ~1882–2466) damgalar taşar; libstdc++'ın kendi
// stat dönüşümü de aynı sınırda.
fs::file_time_type from_filetime(const FILETIME& ft) {
    using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    constexpr std::int64_t kTicksFrom1601To1970 = 116'444'736'000'000'000;
    const auto ticks = static_cast<std::int64_t>((std::uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
    return std::chrono::file_clock::from_sys(
        std::chrono::sys_time<Ticks>(Ticks(ticks - kTicksFrom1601To1970)));
}

FileStat make_stat(DWORD attributes, bool link, const FILETIME& mtime, DWORD size_high, DWORD size_low) {
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return FileStat{from_filetime(mtime), (std::uintmax_t{size_high} << 32) | size_low, !link && !directory,
                    !link && directory, link};
}

// Yalnız ad vekili reparse point'ler (sembolik bağ, junction) bağdır: OneDrive yer tutucusu da reparse
// point'tir ama normal dosya/dizin gibi listelenmeli. Etiket dwReserved0'dadır.
bool is_link(const WIN32_FIND_DATAW& data) {
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           IsReparseTagNameSurrogate(data.dwReserved0);
}

#else

fs::file_time_type from_timespec(const timespec& ts) {
    using namespace std::chrono;
    return file_clock::from_sys(sys_time<nanoseconds>(seconds(ts.tv_sec) + nanoseconds(ts.tv_nsec)));
}

#endif

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace

std::expected<FileStat, std::error_code> stat_file(const fs::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return std::unexpected(last_error());
    bool link = false;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        // Reparse etiketi yalnız dizin girdisinde var; nadir yol (bağ ya da bulut yer tutucusu).
        WIN32_FIND_DATAW entry;
        const HANDLE find =
            FindFirstFileExW(path.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
            return std::unexpected(last_error());
        FindClose(find);
        link = is_link(entry);
    }
    return make_stat(data.dwFileAttributes, link, data.ftLastWriteTime, data.nFileSizeHigh,
                     data.nFileSizeLow);
#else
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0)
        return std::unexpected(std::error_code(errno, std::generic_category()));
#ifdef __APPLE__
    const fs::file_time_type mtime = from_timespec(st.st_mtimespec);
#else
    const fs::file_time_type mtime = from_timespec(st.st_mtim);
#endif
    return FileStat{mtime, static_cast<std::uintmax_t>(st.st_size), S_ISREG(st.st_mode), S_ISDIR(st.st_mode),
                    S_ISLNK(st.st_mode)};
#endif
}

std::vector<ScannedFile> scan_files(const fs::path& root) {
    std::vector<ScannedFile> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& path = it->path();
        if (const std::u8string name = path.filename().u8string(); !name.empty() && name.front() == u8'.') {
            std::error_code type_ec;
            if (it->is_directory(type_ec))
                it.disable_recursion_pending();  // .git gibi dizinlere inilmez
            continue;
        }
        const auto stat = stat_file(path);
        if (!stat) {
            log_warn("note list: skipped '" + to_utf8(path) + "': " + stat.error().message());
            continue;
        }
        if (stat->regular)  // bağlar (lstat) ve özel dosyalar listelenmez
            out.push_back(ScannedFile{to_utf8(path.lexically_relative(root)), *stat});
    }
    return out;
}

}  // namespace noteapp::core
