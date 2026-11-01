#pragma once

// Yalnız program giriş noktaları (main.cpp) içerir: tanımladığı _load_config_used program başına bir kez
// bağlanmalı.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <cstdlib>
#include <windows.h>

#ifdef __MINGW32__
namespace noteapp::core::detail {
constexpr IMAGE_LOAD_CONFIG_DIRECTORY64 make_load_config() {
    IMAGE_LOAD_CONFIG_DIRECTORY64 config{};
    config.Size = sizeof(config);
    config.DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32;
    return config;
}
}  // namespace noteapp::core::detail

// MSVC'nin /DEPENDENTLOADFLAG:0x800 karşılığı: GNU ld 2.46'da bayrak yok, PE yükleme yapılandırma dizinini bu
// sembolden doldurur (bfd/peXXigen.c). main'den önce çözülen içe aktarmalar için tek koruma budur; Windows 10
// 1607 ve sonrasında geçerlidir. Wine 11.20 bu alanı okumaz, dolayısıyla Wine'da kanıtlanamaz.
extern "C" __attribute__((used)) constinit const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used =
    noteapp::core::detail::make_load_config();
#endif

namespace noteapp::core {

// Bundan sonraki LoadLibrary çağrıları (SDL, gecikmeli yüklenen DLL'ler) yalnız System32'ye bakar.
// Başarısızlıkta devam edilmez: exe dizini yeniden arama yoluna girer ve token Credential Manager'dan
// okunabilir.
inline void restrict_dll_search_to_system32() {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        std::fprintf(stderr, "fatal: could not restrict the DLL search path (error %lu)\n", GetLastError());
        std::_Exit(EXIT_FAILURE);
    }
}

}  // namespace noteapp::core

#else

namespace noteapp::core {
inline void restrict_dll_search_to_system32() {}
}  // namespace noteapp::core

#endif
