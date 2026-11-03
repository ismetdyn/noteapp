#pragma once

// Süreç testlerinin ortak yoklamaları: PID hâlâ yaşıyor mu, koşul süre dolmadan doğrulandı mı. Windows başlığını
// yalnız süreç testleri içersin diye test_support.hpp'den ayrıdır.
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace noteapp::test {

#ifdef __linux__
// Ölmüş ama henüz biçilmemiş (zombi) süreç: ebeveyni ölen torun PID 1'e devredilir; Docker'da PID 1 bir kabuk
// olduğundan onu hemen biçmeyebilir. Zombi çalışmaz, kill(pid, 0) yine de başarılı döner.
inline bool is_zombie(long long pid) {
    std::FILE* stat = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
    if (stat == nullptr)
        return false;
    char buffer[512] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, stat);
    std::fclose(stat);
    // Biçim: "pid (komut) DURUM ..."; komut adı parantez içerebileceğinden son ')' sonrası okunur.
    const std::string line(buffer, read);
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
}
#endif

// PID hâlâ çalışan bir süreç mi? (Windows: çıkış kodu STILL_ACTIVE; POSIX: kill(pid, 0), zombi ölü sayılır.)
inline bool process_alive(long long pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    const bool exists = kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#ifdef __linux__
    return exists && !is_zombie(pid);
#else
    return exists;
#endif
#endif
}

// Başarısız testin bıraktığı süreç (ör. 10 dakika uyuyan torun) koşuyu kirletmesin.
inline void kill_process(long long pid) {
#ifdef _WIN32
    if (HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
        TerminateProcess(handle, 1);
        CloseHandle(handle);
    }
#else
    kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

inline bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

}  // namespace noteapp::test
