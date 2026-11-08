#include "cli/term.hpp"

#include "core/process.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
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

// mintty/MSYS/Cygwin pty'si: konsol değil, adı is_msys_pty_pipe_name kalıbında bir boru.
bool is_msys_pty(DWORD std_handle) {
    const HANDLE handle = GetStdHandle(std_handle);
    if (GetFileType(handle) != FILE_TYPE_PIPE)
        return false;
    // pty adı ~50 karakterdir; daha uzun ad ERROR_MORE_DATA ile düşer ve zaten pty değildir. Uzunluk ve ad memcpy
    // ile okunur: FILE_NAME_INFO::FileName tek öğeli dizidir, ötesine indekslemek tanımsız davranıştır.
    constexpr std::size_t kMaxNameChars = MAX_PATH;
    constexpr std::size_t kNameOffset = offsetof(FILE_NAME_INFO, FileName);
    alignas(FILE_NAME_INFO) std::array<std::byte, kNameOffset + kMaxNameChars * sizeof(wchar_t)> buffer{};
    if (GetFileInformationByHandleEx(handle, FileNameInfo, buffer.data(), static_cast<DWORD>(buffer.size())) == 0)
        return false;
    DWORD name_bytes = 0;
    std::memcpy(&name_bytes, buffer.data() + offsetof(FILE_NAME_INFO, FileNameLength), sizeof name_bytes);
    std::wstring name(std::min<std::size_t>(name_bytes, buffer.size() - kNameOffset) / sizeof(wchar_t), L'\0');
    std::memcpy(name.data(), buffer.data() + kNameOffset, name.size() * sizeof(wchar_t));
    return is_msys_pty_pipe_name(name);
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

bool is_msys_pty_pipe_name(std::wstring_view name) {
    constexpr std::wstring_view kPty = L"-pty";
    for (const std::wstring_view prefix : {L"\\msys-", L"\\cygwin-"}) {
        if (!name.starts_with(prefix))
            continue;
        // Önekten sonra boş olmayan anahtar, `-pty` ve pty numarası.
        const std::wstring_view rest = name.substr(prefix.size());
        const std::size_t key_end = rest.find(kPty);
        if (key_end == 0 || key_end == std::wstring_view::npos)
            return false;
        const std::wstring_view number = rest.substr(key_end + kPty.size());
        return !number.empty() && number.front() >= L'0' && number.front() <= L'9';
    }
    return false;
}

bool stdin_is_tty() {
#ifdef _WIN32
    return is_console(STD_INPUT_HANDLE) || is_msys_pty(STD_INPUT_HANDLE);
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

bool stream_is_tty(Stream stream) {
#ifdef _WIN32
    const DWORD handle = stream == Stream::Out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
    return is_console(handle) || is_msys_pty(handle);
#else
    return isatty(stream == Stream::Out ? STDOUT_FILENO : STDERR_FILENO) != 0;
#endif
}

std::optional<std::string> read_hidden_line(std::istream& in, std::ostream& out, std::string_view prompt,
                                             std::string_view refusal, bool echo_hidden) {
    if (!echo_hidden) {
        out << refusal << '\n';
        return std::nullopt;
    }
    out << prompt << std::flush;
    std::string line;
    std::getline(in, line);
    return line;
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
