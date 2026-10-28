#include "core/console.hpp"

#ifdef _WIN32
#ifndef NOMINMAX  // MinGW'nin libstdc++'ı zaten tanımlar; MSVC tanımlamaz.
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace noteapp::core {

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
#else
ConsoleUtf8Scope::ConsoleUtf8Scope() = default;
ConsoleUtf8Scope::~ConsoleUtf8Scope() = default;
#endif

}  // namespace noteapp::core
