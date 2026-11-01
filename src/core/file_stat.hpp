#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace noteapp::core {

// Karşılaştırılan her mtime bu dosyadan gelir. std::filesystem::last_write_time MinGW libstdc++'ta
// (_wstat64) saniyeye keser: aynı saniyedeki dış düzenleme write()'ın ModifiedExternally denetiminden
// kaçardı. Windows'ta FILETIME (100 ns), POSIX'te st_mtim (ns) okunur; değerler file_time_type'tır.
struct FileStat {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    bool regular;
    bool directory;
    bool symlink;  // sembolik bağ / junction (Windows: ad vekili reparse point; OneDrive yer tutucusu değil)
};

// Sembolik bağı izlemez (bağın kendi bilgisi döner).
std::expected<FileStat, std::error_code> stat_file(const std::filesystem::path& path);

// Dosyanın doğum zamanı (Linux statx btime, Windows oluşturma zamanı, macOS st_birthtime); bağı izlemez.
// Dosya sistemi vermiyorsa boş.
std::optional<std::filesystem::file_time_type> file_birth_time(const std::filesystem::path& path);

struct ScannedFile {
    std::string relative;  // köke göre, '/' ayırıcılı UTF-8
    FileStat stat;
};

// root altındaki normal dosyalar, özyinelemeli. '.' ile başlayan adlar ve bağlar atlanır, içlerine
// inilmez; erişilemeyen dizin sessizce atlanır, root yoksa boş döner. Windows'ta FindFirstFileExW: mtime
// ve boyut dizin girdisinden gelir, dosya başına stat yok.
std::vector<ScannedFile> scan_files(const std::filesystem::path& root);

}  // namespace noteapp::core
