#pragma once

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Pano araçları ve hunspell için stdin/stdout borulu, kabuksuz çocuk süreç. Kendi süreç grubundadır; yıkıcı
// bitmemişse grubu öldürür ve toplar: askıda çocuk bırakılmaz. Ortamdan AppImage değişkenleri (SSL_CERT_*,
// LD_*) ve senkron token'ı düşer, `extra_env` üstüne biner. Windows'ta yoktur (start nullopt döner): pano
// Win32 API'sini kullanır, yazım denetimi kapalıdır.
class ChildProcess {
public:
    using Clock = std::chrono::steady_clock;
    enum class ReadStatus { Data, Eof, Timeout, Failed };

    // `capture_stdout` false ise stdout boş aygıttır: wl-copy ve xclip arka plana çatallanıp seçimi tutar,
    // devralınan boru hiç EOF vermezdi. Program yoksa ya da başlatılamazsa nullopt.
    static std::optional<ChildProcess> start(const std::vector<std::string>& argv, bool capture_stdout,
                                             const std::vector<std::string>& extra_env = {});

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

    long long pid_ = -1;  // bitip toplanınca -1
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
};

}  // namespace noteapp::tui
