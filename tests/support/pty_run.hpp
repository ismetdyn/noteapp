#pragma once

// Bir ikiliyi sahte terminalde (pty) çalıştırır: isatty dalları (temizlik, renk, ipucu, pager) böyle sınanır.
// Yalnız Linux (posix_openpt). Çocuğun ortamı belirlenimcidir: TERM=xterm-256color, renk/pager değişkenleri
// silinir; test istediğini `env` ile ekler ya da siler.
#ifdef __linux__

#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace noteapp::test {

struct PtyOptions {
    // nullopt: değişken silinir.
    std::vector<std::pair<std::string, std::optional<std::string>>> env = {};
    std::string input = {};          // terminale yazılan tuşlar (ör. onay yanıtı "y\n")
    std::string input_after = {};    // doluysa girdi, terminalde bu metin görününce yazılır (ör. Ctrl-C zamanlaması)
    bool stderr_to_file = false;     // stderr terminal değil, dosya: yalnız stdout TTY
    unsigned short rows = 24;
    unsigned short columns = 80;
};

struct PtyResult {
    std::string terminal;  // terminalde görünen her şey (stdout, stderr_to_file değilse stderr, girdinin ekosu)
    std::string err;       // stderr_to_file iken stderr
    int exit_code = -1;
};

// Okuma, çocuk bitip terminal kapanınca (EIO) ya da bu süre boyunca çıktı gelmezse biter (asılmaya karşı).
inline constexpr int kPtyIdleTimeoutMs = 10000;

inline PtyResult run_in_pty(const std::string& exe, const std::vector<std::string>& args,
                            const PtyOptions& options = {}) {
    const TempDir scratch;
    const std::string err_file = utf8(scratch.path() / "stderr.bin");
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    REQUIRE(grantpt(master) == 0);
    REQUIRE(unlockpt(master) == 0);
    const std::string slave = ptsname(master);
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        setsid();
        const int fd = open(slave.c_str(), O_RDWR);
        winsize size{};
        size.ws_row = options.rows;
        size.ws_col = options.columns;
        ioctl(fd, TIOCSWINSZ, &size);
        dup2(fd, 0);
        dup2(fd, 1);
        if (options.stderr_to_file)
            dup2(open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), 2);
        else
            dup2(fd, 2);
        setenv("TERM", "xterm-256color", 1);
        for (const char* name : {"NO_COLOR", "NOTEAPP_COLOR", "NOTEAPP_PAGER", "PAGER", "LESS", "COLUMNS", "LINES"})
            unsetenv(name);
        for (const auto& [name, value] : options.env) {
            if (value)
                setenv(name.c_str(), value->c_str(), 1);
            else
                unsetenv(name.c_str());
        }
        std::vector<char*> argv;
        std::string program = exe;
        argv.push_back(program.data());
        std::vector<std::string> copy = args;
        for (std::string& arg : copy)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        _exit(127);
    }
    PtyResult result;
    bool input_written = options.input.empty();
    const auto write_input = [&] {
        REQUIRE(write(master, options.input.data(), options.input.size()) ==
                static_cast<ssize_t>(options.input.size()));
        input_written = true;
    };
    if (!input_written && options.input_after.empty())
        write_input();
    char buffer[4096];
    while (true) {
        pollfd ready{master, POLLIN, 0};
        if (poll(&ready, 1, kPtyIdleTimeoutMs) <= 0)
            break;
        const ssize_t got = read(master, buffer, sizeof buffer);
        if (got <= 0)
            break;
        result.terminal.append(buffer, static_cast<std::size_t>(got));
        if (!input_written && result.terminal.find(options.input_after) != std::string::npos)
            write_input();
    }
    close(master);
    int status = 0;
    waitpid(child, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.err = slurp(err_file);
    return result;
}

}  // namespace noteapp::test

#endif  // __linux__
