#pragma once

// Perf testlerinin bellek ölçümü: süreç yerleşik belleği (RSS). Yalnız Linux'ta (/proc/self/statm ikinci alanı
// yerleşik sayfa sayısıdır); öbür platformlarda (Windows çapraz derleme, Wine) psapi test bağlantısında olmadığı
// için ölçülmez, nullopt döner ve test bellek denetimini atladığını yazar.
#include <algorithm>
#include <cstddef>
#include <optional>

#ifdef __linux__
#include <fstream>
#include <unistd.h>
#endif

namespace noteapp::test {

inline std::optional<std::size_t> resident_bytes() {
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    std::size_t total = 0;
    std::size_t resident = 0;
    if (!(statm >> total >> resident))
        return std::nullopt;
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
    return std::nullopt;
#endif
}

// `before` ile şimdi arasındaki artış (MiB); ölçülemiyorsa nullopt.
inline std::optional<double> resident_growth_mib(std::optional<std::size_t> before) {
    const std::optional<std::size_t> after = resident_bytes();
    if (!before || !after)
        return std::nullopt;
    constexpr double kMib = 1024.0 * 1024.0;
    return static_cast<double>(*after - std::min(*after, *before)) / kMib;
}

}  // namespace noteapp::test
