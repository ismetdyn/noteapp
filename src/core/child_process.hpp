#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

// Pano araçları, hunspell, dil sunucuları ve PDF işçisi için stdin/stdout borulu, kabuksuz çocuk süreç. Kendi süreç grubundadır
// (Windows: kapanınca öldüren iş nesnesi); yıkıcı bitmemişse ağacı öldürür ve toplar: askıda çocuk bırakılmaz.
// Ortamdan AppImage değişkenleri (SSL_CERT_*, LD_*) ve senkron token'ı düşer, `extra_env` ("AD=değer") üstüne
// biner. Windows'ta program PATHEXT ile bulunur, .bat/.cmd cmd.exe ile çalışır ve metakarakterli argüman
// reddedilir (core::spawn_with_stdio); konsol penceresi açılmaz, stderr boş aygıttır.
class ChildProcess {
public:
    using Clock = std::chrono::steady_clock;
    enum class ReadStatus { Data, Eof, Timeout, Failed };

    // `capture_stdout` false ise stdout boş aygıttır: wl-copy ve xclip arka plana çatallanıp seçimi tutar,
    // devralınan boru hiç EOF vermezdi. Program yoksa ya da başlatılamazsa nullopt.
    // `working_directory` boş değilse çocuk orada başlar (POSIX: posix_spawn chdir eylemi, Windows:
    // lpCurrentDirectory); dizin yoksa başlatma başarısız olur.
    static std::optional<ChildProcess> start(const std::vector<std::string>& argv, bool capture_stdout,
                                             const std::vector<std::string>& extra_env = {},
                                             const std::filesystem::path& working_directory = {});

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess();

    // Hepsi yazılana dek; çocuk okumazsa `deadline`da ya da durma isteğinde false. Ölü çocuğa yazmak süreci
    // öldürmez (SIGPIPE yalnız bu çağrıda engellenir).
    bool write_all(std::string_view data, Clock::time_point deadline, const std::stop_token& stop = {});
    void close_stdin();
    // Gelen baytları `out`a ekler; `deadline`a dek ilk veriyi bekler.
    ReadStatus read_some(std::string& out, Clock::time_point deadline);
    // Çıkış kodu; `deadline`a dek bitmediyse nullopt.
    std::optional<int> wait_until(Clock::time_point deadline);
    void kill();

private:
    ChildProcess() = default;
    void close_fds();
    void take(ChildProcess& other) noexcept;  // `other`ın tutamaçlarını alır, onu boşaltır

#ifdef _WIN32
    // HANDLE'lar; windows.h bu başlığa sızmasın diye void*. Boru uçları bindirmeli (overlapped) açılır.
    void* process_ = nullptr;  // bitip toplanınca null
    void* job_ = nullptr;      // çocuk işe atanamadıysa null
    void* stdin_pipe_ = nullptr;
    void* stdout_pipe_ = nullptr;
#else
    long long pid_ = -1;  // bitip toplanınca -1
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
#endif
};

}  // namespace noteapp::core
