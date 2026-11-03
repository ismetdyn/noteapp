#include "edit/clipboard.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/child_process.hpp"
#include "edit/display_text.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "core/process.hpp"
#endif

namespace noteapp::tui {

namespace {

constexpr std::string_view kOsc52Prefix = "\x1B]52;c;";
constexpr std::string_view kOsc52Suffix = "\x07";

std::string base64(std::string_view bytes) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::size_t count = std::min<std::size_t>(bytes.size() - at, 3);
        std::uint32_t group = 0;
        for (std::size_t i = 0; i < 3; ++i)
            group = (group << 8) | (i < count ? static_cast<unsigned char>(bytes[at + i]) : 0U);
        out += kAlphabet[(group >> 18) & 0x3F];
        out += kAlphabet[(group >> 12) & 0x3F];
        out += count > 1 ? kAlphabet[(group >> 6) & 0x3F] : '=';
        out += count > 2 ? kAlphabet[group & 0x3F] : '=';
    }
    return out;
}

bool is_pasteable(std::string_view text) {
    return text.find('\0') == std::string_view::npos && core::is_valid_utf8(text);
}

std::string size_limit(std::size_t bytes) {
    return human_size(bytes);
}

#ifdef _WIN32

// Pano başka süreçte kısa süreli kilitli olabilir: birkaç deneme, sonra vazgeç.
bool open_clipboard() {
    constexpr int kAttempts = 5;
    constexpr DWORD kRetryMs = 20;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (OpenClipboard(nullptr))
            return true;
        Sleep(kRetryMs);
    }
    return false;
}

struct ClipboardLock {
    bool held = open_clipboard();
    ~ClipboardLock() {
        if (held)
            CloseClipboard();
    }
};

bool win32_copy(std::string_view text) {
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide <= 0 && !text.empty())
        return false;
    const ClipboardLock lock;
    if (!lock.held || !EmptyClipboard())
        return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide) + 1) * sizeof(wchar_t));
    if (memory == nullptr)
        return false;
    auto* target = static_cast<wchar_t*>(GlobalLock(memory));
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), target, wide);
    target[wide] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);  // sahiplik yalnız başarıda sisteme geçer
        return false;
    }
    return true;
}

SystemClipboard::Pasted win32_paste(std::size_t max_bytes) {
    using Status = SystemClipboard::PasteStatus;
    const ClipboardLock lock;
    if (!lock.held || !IsClipboardFormatAvailable(CF_UNICODETEXT))
        return {Status::Unavailable};
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const auto* wide = data == nullptr ? nullptr : static_cast<const wchar_t*>(GlobalLock(data));
    if (wide == nullptr)
        return {Status::Unavailable};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string text(size > 0 ? static_cast<std::size_t>(size) - 1 : 0, '\0');
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
    GlobalUnlock(data);
    if (text.size() > max_bytes)
        return {Status::TooLarge};
    return {text.empty() ? Status::Unavailable : Status::Text, std::move(text)};
}

#else

struct Tools {
    std::vector<std::string> copy;
    std::vector<std::string> paste;
};

bool on_path(std::string_view program) {
    return core::find_program(program).has_value();
}

// Oturum türü ortamdan gelir: Wayland oturumunda X11 aracı XWayland'in ayrı panosuna yazardı. Araç yalnız o
// oturumda aranır; hiçbiri yoksa nullopt (kopyada OSC 52).
std::optional<Tools> tools_for_session() {
#ifdef __APPLE__
    if (on_path("pbcopy"))
        return Tools{{"pbcopy"}, {"pbpaste"}};
#endif
    if (core::get_env_utf8("WAYLAND_DISPLAY") && (on_path("wl-copy") || on_path("wl-paste")))
        return Tools{{"wl-copy"}, {"wl-paste", "--no-newline"}};
    if (core::get_env_utf8("DISPLAY")) {
        if (on_path("xclip"))
            return Tools{{"xclip", "-selection", "clipboard", "-in"},
                         {"xclip", "-selection", "clipboard", "-out"}};
        if (on_path("xsel"))
            return Tools{{"xsel", "--clipboard", "--input"}, {"xsel", "--clipboard", "--output"}};
    }
    return std::nullopt;
}

bool tool_copy(const std::vector<std::string>& argv, std::string_view text,
               std::chrono::milliseconds timeout) {
    auto child = ChildProcess::start(argv, false);
    if (!child)
        return false;
    const auto deadline = ChildProcess::Clock::now() + timeout;
    const bool written = child->write_all(text, deadline);
    child->close_stdin();
    return written && child->wait_until(deadline) == 0;  // bitmediyse yıkıcı öldürür
}

SystemClipboard::Pasted tool_paste(const std::vector<std::string>& argv, std::chrono::milliseconds timeout,
                                   std::size_t max_bytes) {
    using Status = SystemClipboard::PasteStatus;
    auto child = ChildProcess::start(argv, true);
    if (!child)
        return {Status::Unavailable};
    child->close_stdin();
    const auto deadline = ChildProcess::Clock::now() + timeout;
    std::string text;
    while (true) {
        const ChildProcess::ReadStatus read = child->read_some(text, deadline);
        if (text.size() > max_bytes)
            return {Status::TooLarge};
        if (read == ChildProcess::ReadStatus::Eof)
            break;
        if (read != ChildProcess::ReadStatus::Data)
            return {Status::Unavailable};
    }
    if (child->wait_until(deadline) != 0 || text.empty())
        return {Status::Unavailable};
    return {Status::Text, std::move(text)};
}

#endif

}  // namespace

void SystemClipboard::write_to_terminal(std::string_view sequence) {
    std::cout << sequence << std::flush;
}

SystemClipboard::SystemClipboard(TerminalWriter terminal, std::chrono::milliseconds timeout,
                                 std::size_t max_paste_bytes)
    : terminal_(std::move(terminal)), timeout_(timeout), max_paste_bytes_(max_paste_bytes) {}

SystemClipboard::CopyResult SystemClipboard::copy(std::string_view text) {
    if (text.find('\0') != std::string_view::npos)
        return CopyResult::Binary;
#ifdef _WIN32
    if (win32_copy(text)) {
        stale_ = false;
        return CopyResult::Tool;
    }
    stale_ = true;
#else
    if (const auto tools = tools_for_session()) {
        if (tool_copy(tools->copy, text, timeout_)) {
            stale_ = false;
            return CopyResult::Tool;
        }
        stale_ = true;
    }
#endif
    if (text.size() > kOsc52MaxBytes)
        return CopyResult::TooLarge;
    terminal_(std::string(kOsc52Prefix) + base64(text) + std::string(kOsc52Suffix));
    return CopyResult::Terminal;
}

SystemClipboard::Pasted SystemClipboard::paste() {
#ifdef _WIN32
    Pasted pasted = win32_paste(max_paste_bytes_);
#else
    const auto tools = tools_for_session();
    if (!tools)
        return {PasteStatus::NoTool};
    Pasted pasted = tool_paste(tools->paste, timeout_, max_paste_bytes_);
#endif
    if (pasted.status == PasteStatus::Text && !is_pasteable(pasted.text))
        return {PasteStatus::Binary};
    return pasted;
}

std::string copy_message(SystemClipboard::CopyResult result, std::size_t bytes) {
    const std::string size = human_size(bytes);
    switch (result) {
    case SystemClipboard::CopyResult::Tool:
        return core::tr("tui.clipboard_copied", {{"size", size}});
    case SystemClipboard::CopyResult::Terminal:
        return core::tr("tui.clipboard_copied_terminal", {{"size", size}});
    case SystemClipboard::CopyResult::TooLarge:
        return core::tr("tui.clipboard_too_large",
                        {{"size", size}, {"limit", size_limit(SystemClipboard::kOsc52MaxBytes)}});
    case SystemClipboard::CopyResult::Binary:
        break;
    }
    return core::tr("tui.clipboard_binary");
}

PasteChoice choose_paste(SystemClipboard& clipboard, const std::string& internal) {
    if (clipboard.stale())
        return {internal, {}};
    using Status = SystemClipboard::PasteStatus;
    SystemClipboard::Pasted pasted = clipboard.paste();
    switch (pasted.status) {
    case Status::Text:
        return {std::move(pasted.text), {}};
    case Status::NoTool:
        return {internal, core::tr(internal.empty() ? "tui.paste_no_tool" : "tui.paste_internal")};
    case Status::Unavailable:
        return {internal, internal.empty() ? core::tr("tui.paste_unavailable") : std::string{}};
    case Status::Binary:
        return {{}, core::tr("tui.paste_binary")};
    case Status::TooLarge:
        return {{}, core::tr("tui.paste_too_large", {{"limit", size_limit(clipboard.max_paste_bytes())}})};
    }
    return {internal, {}};
}

}  // namespace noteapp::tui
