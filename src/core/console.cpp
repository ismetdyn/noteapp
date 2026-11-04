#include "core/console.hpp"

#include <cstddef>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX  // MinGW'nin libstdc++'ı zaten tanımlar; MSVC tanımlamaz.
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <shellapi.h>  // CommandLineToArgvW; windows.h'tan sonra gelmeli
// clang-format on
#endif

namespace noteapp::core {

std::vector<std::string> utf8_arguments(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int wide_argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide == nullptr)
        return out;
    for (int i = 1; i < wide_argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), size, nullptr, nullptr);
        out.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

#ifdef _WIN32
ConsoleUtf8Scope::ConsoleUtf8Scope() : input_cp_(GetConsoleCP()), output_cp_(GetConsoleOutputCP()) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // stdout yönlendirilmişse GetConsoleMode başarısız olur: dokunmayız (boruya kaçış kodu sızmaz).
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode) &&
        (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0 &&
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        stdout_mode_ = mode;
        vt_changed_ = true;
    }
}

ConsoleUtf8Scope::~ConsoleUtf8Scope() {
    if (vt_changed_)
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), stdout_mode_);
    // 0: süreç bir konsola bağlı değildi; geri yüklenecek bir şey yok.
    if (input_cp_ != 0)
        SetConsoleCP(input_cp_);
    if (output_cp_ != 0)
        SetConsoleOutputCP(output_cp_);
}
#endif

}  // namespace noteapp::core
