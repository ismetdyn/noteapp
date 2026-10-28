#pragma once

// Gerçek bir ikiliyi alt süreçte çalıştırıp stdout/stderr'i yakalar ve stdin besler (cli testleri).
// Kabuk yönlendirmesi + geçici dosya kullanır: ek süreç API'si gerekmez, Türkçe argümanlar geniş
// dizgeyle (_wsystem) geçer. Argümanlarda `"` ve `%` kullanılmaz (testler bunları içermez).
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace noteapp::test {

struct RunResult {
    int exit_code = -1;
    std::string out;
    std::string err;
    std::chrono::milliseconds elapsed{0};  // kabuk + süreç açılışı dahil
};

// `stdin_bytes` verilmezse stdin boş bir dosyadan gelir (TTY olmaz, EOF hemen görülür).
inline RunResult run_process(const fs::path& exe, const std::vector<std::string>& args,
                             const std::string& stdin_bytes = "") {
    const TempDir scratch;
    const fs::path in = scratch.path() / "stdin.bin";
    const fs::path out = scratch.path() / "stdout.bin";
    const fs::path err = scratch.path() / "stderr.bin";
    {
        std::ofstream f(in, std::ios::binary);
        f.write(stdin_bytes.data(), static_cast<std::streamsize>(stdin_bytes.size()));
    }

#ifdef _WIN32
    const auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    // cmd /c, ilk ve son tırnağı soyar; bu yüzden tüm komut bir kez daha tırnaklanır.
    std::string command = "\"" + quote(utf8(exe));
#else
    const auto quote = [](const std::string& s) {
        std::string q = "'";
        for (const char c : s)
            q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    };
    std::string command = quote(utf8(exe));
#endif
    for (const auto& a : args)
        command += " " + quote(a);
    command += " < " + quote(utf8(in)) + " > " + quote(utf8(out)) + " 2> " + quote(utf8(err));
#ifdef _WIN32
    command += "\"";
#endif

    const auto start = std::chrono::steady_clock::now();
#ifdef _WIN32
    const std::u8string u8(command.begin(), command.end());
    const int status = _wsystem(fs::path(u8).wstring().c_str());
    const int code = status;
#else
    const int status = std::system(command.c_str());
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    RunResult result;
    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    result.exit_code = code;
    result.out = slurp(out);
    result.err = slurp(err);
    return result;
}

}  // namespace noteapp::test
