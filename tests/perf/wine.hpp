#pragma once

// Wine yerli platform değil, Windows ikilisinin test ortamıdır. Yerli sınırla yetişemeyen ölçüm, çarpanı ve
// boş makinedeki Linux/Wine ölçümünü kendi yanında yazar; yerli sınır değişmez.
#ifdef _WIN32
#include <windows.h>
#endif

namespace noteapp::test {

inline constexpr double kWineFactor = 2.0;

// Wine'a özgü kernel32 dışa aktarımı (tests/pdf/pdftotext_wine.cpp ile aynı yol): yerli Windows'ta yoktur.
inline bool running_under_wine() {
#ifdef _WIN32
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name") != nullptr;
#else
    return false;
#endif
}

}  // namespace noteapp::test
