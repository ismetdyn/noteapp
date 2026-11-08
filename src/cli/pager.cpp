#include "cli/pager.hpp"

#include "cli/term.hpp"
#include "core/editor.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

namespace noteapp::cli {

#ifdef _WIN32

// GEÇİCİ: Windows'ta pager yok; popen/less eşdeğeri ve konsol kipi ayrı iş.
// BEDEL: uzun çıktı Windows konsolunda kayar, sayfalanmaz.
// NE ZAMAN KALDIRILIR: Windows için pager kararı verildiğinde (SPEC-cli "Pager", Faz 2).
int run_paged(const std::function<int()>& body) {
    return body();
}

#else

namespace {

constexpr std::string_view kDefaultPager = "less -FIRX";

// nullopt: sayfalama kapalı. `more`/`most` renkleri ve kısa çıktıda çıkmayı bilmez: less'e düşülür (bat gibi).
std::optional<std::string> pager_command() {
    std::optional<std::string> chosen = core::get_env_utf8("NOTEAPP_PAGER");
    if (!chosen)
        chosen = core::get_env_utf8("PAGER");
    if (!chosen)
        return std::string(kDefaultPager);
    const std::string program = chosen->substr(0, chosen->find(' '));
    if (program == "cat")
        return std::nullopt;
    if (program == "more" || program == "most")
        return std::string(kDefaultPager);
    return chosen;
}

// Sarılan uzun satırlar dahil terminal satırı; sığıyorsa pager açılmaz (sabit eşik yok, terminalin kendisi).
bool fits_terminal(std::string_view text) {
    const TermSize size = term_size();
    if (size.rows == 0)
        return true;  // yükseklik bilinmiyor: sayfalamak için gerekçe yok
    std::size_t rows = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const auto columns = static_cast<std::size_t>(std::ranges::count_if(
            text.substr(start, end - start), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        rows += size.columns == 0 ? 1 : std::max<std::size_t>(1, (columns + size.columns - 1) / size.columns);
        start = end + 1;
    }
    return rows < size.rows;  // istem satırına yer kalsın
}

// Pager açıkken SIGPIPE, SIGINT ve SIGQUIT yok sayılır, sonra eski işleyiciler geri gelir (git gibi). SIGPIPE:
// kullanıcı pager'ı erken kapatırsa (q) yazma EPIPE alır, sessizce biter. SIGINT/SIGQUIT: Ctrl-C ön plan
// grubunun tamamına gider; `note` ölseydi pager (less Ctrl-C'yi yok sayar) terminali okumaya devam eder, kabuk
// istemiyle aynı TTY'yi paylaşırdı. Pager sinyalleri kendi ele alır.
class IgnoreSignalsWhilePaging {
public:
    IgnoreSignalsWhilePaging()
        : pipe_(std::signal(SIGPIPE, SIG_IGN)), interrupt_(std::signal(SIGINT, SIG_IGN)),
          quit_(std::signal(SIGQUIT, SIG_IGN)) {}
    ~IgnoreSignalsWhilePaging() {
        std::signal(SIGPIPE, pipe_);
        std::signal(SIGINT, interrupt_);
        std::signal(SIGQUIT, quit_);
    }
    IgnoreSignalsWhilePaging(const IgnoreSignalsWhilePaging&) = delete;
    IgnoreSignalsWhilePaging& operator=(const IgnoreSignalsWhilePaging&) = delete;

private:
    void (*pipe_)(int);
    void (*interrupt_)(int);
    void (*quit_)(int);
};

// Pager başladıysa true. Ortam (LESS dahil) kullanıcınındır: üründe setenv yok (core/process.cpp sözleşmesi);
// varsayılan komut `less -FIRX` bayrakları zaten taşır.
// Kabuk yok: komut editör komutunun tırnak kuralıyla bölünür ve program find_program'ın bulduğu mutlak yolla
// başlar (kabuk PATH'in göreli girdilerinde, ör. ".", arardı).
bool write_to_pager(const std::string& command, std::string_view text) {
    const std::vector<std::string> argv = core::split_command_line(command);
    if (argv.empty() || !core::find_program(argv.front()))
        return false;
    std::cout.flush();
    const IgnoreSignalsWhilePaging ignore;
    return core::run_with_input(argv, text).has_value();
}

// Gövde std::cout'a yazarken tamponu yönlendirir; istisnada da geri alınır.
class CaptureStdout {
public:
    CaptureStdout() : saved_(std::cout.rdbuf(buffer_.rdbuf())) {}
    ~CaptureStdout() { std::cout.rdbuf(saved_); }
    CaptureStdout(const CaptureStdout&) = delete;
    CaptureStdout& operator=(const CaptureStdout&) = delete;

    std::string take() {
        std::cout.rdbuf(saved_);
        return std::move(buffer_).str();
    }

private:
    std::ostringstream buffer_;
    std::streambuf* saved_;
};

}  // namespace

int run_paged(const std::function<int()>& body) {
    if (!stream_is_tty(Stream::Out))
        return body();
    const std::optional<std::string> command = pager_command();
    if (!command)
        return body();
    CaptureStdout capture;
    const int code = body();
    const std::string text = capture.take();
    if (fits_terminal(text) || !write_to_pager(*command, text))
        std::cout << text;
    return code;
}

#endif

}  // namespace noteapp::cli
