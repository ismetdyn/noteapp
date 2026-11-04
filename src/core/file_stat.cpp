#include "core/file_stat.hpp"

#include "core/log.hpp"
#include "core/unique_handle.hpp"

#include <chrono>
#include <cwchar>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
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

// Geçersiz UTF-16 (eşsiz vekil) → boş dizge: böyle ad geri dönüştürülemez, çağıran atlar.
std::string to_utf8(const wchar_t* wide) {
    const int length = static_cast<int>(wcslen(wide));
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, out.data(), size, nullptr, nullptr);
    return out;
}

// FIND_FIRST_EX_LARGE_FETCH: dizin girdileri toplu gelir; ad, öznitelik, boyut ve mtime girdiden okunur.
void scan_dir(const fs::path& dir, const std::string& prefix, std::vector<ScannedFile>& out) {
    WIN32_FIND_DATAW data;
    const auto find = own_win_handle<UniqueFind>(FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic,
                                                                  &data, FindExSearchNameMatch, nullptr,
                                                                  FIND_FIRST_EX_LARGE_FETCH));
    if (!find)
        return;  // kök yok ya da erişilemeyen dizin (skip_permission_denied gibi)
    do {
        if (data.cFileName[0] == L'.')
            continue;  // gizli adlar (.git, .keep) ile . ve ..
        const FileStat stat = make_stat(data.dwFileAttributes, is_link(data), data.ftLastWriteTime,
                                        data.nFileSizeHigh, data.nFileSizeLow);
        if (stat.symlink)
            continue;  // bağlar listelenmez, içlerine inilmez: hedef kök dışına kaçabilir
        std::string name = to_utf8(data.cFileName);
        if (name.empty()) {
            log_warn("note list: skipped an entry in '" + prefix + "': name is not valid UTF-16");
            continue;
        }
        std::string relative = prefix.empty() ? std::move(name) : prefix + "/" + name;
        if (stat.directory)
            scan_dir(dir / data.cFileName, relative, out);
        else
            out.push_back(ScannedFile{std::move(relative), stat});
    } while (FindNextFileW(find.get(), &data));
}

#else

fs::file_time_type from_timespec(const timespec& ts) {
    using namespace std::chrono;
    return file_clock::from_sys(sys_time<nanoseconds>(seconds(ts.tv_sec) + nanoseconds(ts.tv_nsec)));
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

#endif

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
        const auto find = own_win_handle<UniqueFind>(
            FindFirstFileExW(path.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0));
        if (!find)
            return std::unexpected(last_error());
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
#ifdef _WIN32
    scan_dir(root, "", out);
#else
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
#endif
    return out;
}

std::optional<fs::file_time_type> file_birth_time(const fs::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0)
        return std::nullopt;
    return from_filetime(data.ftCreationTime);
#else
    const auto from_parts = [](std::int64_t seconds, std::int64_t nanos) {
        const std::chrono::sys_time<std::chrono::nanoseconds> sys{std::chrono::seconds(seconds) +
                                                                  std::chrono::nanoseconds(nanos)};
        return std::chrono::time_point_cast<fs::file_time_type::duration>(
            std::chrono::file_clock::from_sys(sys));
    };
#if defined(__linux__)
    struct statx info{};
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME, &info) != 0 ||
        (info.stx_mask & STATX_BTIME) == 0)
        return std::nullopt;
    return from_parts(info.stx_btime.tv_sec, info.stx_btime.tv_nsec);
#elif defined(__APPLE__)
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0)
        return std::nullopt;
    return from_parts(info.st_birthtimespec.tv_sec, info.st_birthtimespec.tv_nsec);
#else
    static_cast<void>(from_parts);
    return std::nullopt;
#endif
#endif
}

std::chrono::system_clock::time_point to_system_time(fs::file_time_type when) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::file_clock::to_sys(when));
}

std::chrono::system_clock::time_point file_created_time(const fs::path& file, fs::file_time_type mtime) {
    const auto birth = file_birth_time(file);
    return to_system_time(birth && *birth < mtime ? *birth : mtime);
}

}  // namespace noteapp::core
