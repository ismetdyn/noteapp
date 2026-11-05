#include "cli/term.hpp"

#include "core/process.hpp"

#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace noteapp::cli {

namespace {

#ifdef _WIN32
// _isatty NUL gibi her karakter aygıtını TTY sayar; `note < NUL` editör açıp betiği asılı bırakırdı.
// Gerçek konsol yalnız GetConsoleMode'un başarılı olduğu tutamaçtır.
bool is_console(DWORD std_handle) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(std_handle), &mode) != 0;
}
#endif

unsigned env_number(std::string_view name) {
    const std::optional<std::string> text = core::get_env_utf8(name);
    unsigned value = 0;
    if (text)
        std::from_chars(text->data(), text->data() + text->size(), value);
    return value;
}

}  // namespace

bool stdin_is_tty() {
#ifdef _WIN32
    return is_console(STD_INPUT_HANDLE);
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

bool stream_is_tty(Stream stream) {
#ifdef _WIN32
    return is_console(stream == Stream::Out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
#else
    return isatty(stream == Stream::Out ? STDOUT_FILENO : STDERR_FILENO) != 0;
#endif
}

bool color_enabled(Stream stream) {
    // Kullanıcının açık seçimi ortam geleneklerinden üstündür (no-color.org: kullanıcı bayrağı NO_COLOR'ı ezer).
    const std::optional<std::string> mode = core::get_env_utf8("NOTEAPP_COLOR");
    if (mode == "always")
        return true;
    if (mode == "never")
        return false;
    if (core::get_env_utf8("NO_COLOR") || core::get_env_utf8("TERM") == "dumb")
        return false;
    return stream_is_tty(stream);
}

ProgressLine::ProgressLine(std::string_view text)
    : shown_(stream_is_tty(Stream::Err) && core::get_env_utf8("TERM") != "dumb") {
    if (shown_)
        std::cerr << text << std::flush;
}

ProgressLine::~ProgressLine() {
    if (shown_)
        std::cerr << "\r\x1b[K" << std::flush;  // satır başına dön, satırı sil
}

void ProgressLine::update(std::string_view text) {
    if (shown_)
        std::cerr << "\r\x1b[K" << text << std::flush;
}

TermSize term_size() {
    TermSize size;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) != 0) {
        size.columns = static_cast<unsigned>(info.srWindow.Right - info.srWindow.Left + 1);
        size.rows = static_cast<unsigned>(info.srWindow.Bottom - info.srWindow.Top + 1);
    }
#else
    winsize window{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        size.columns = window.ws_col;
        size.rows = window.ws_row;
    }
#endif
    if (size.columns == 0)
        size.columns = env_number("COLUMNS");
    if (size.rows == 0)
        size.rows = env_number("LINES");
    return size;
}

}  // namespace noteapp::cli
