#pragma once

#include "core/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

using EnvOverrides = std::vector<std::pair<std::string, std::string>>;

// Boş değer "tanımsız" sayılır. Windows'ta geniş API ile okunup UTF-8'e çevrilir.
std::optional<std::string> get_env_utf8(std::string_view name);

// Programı PATH'te arar (Windows: PATHEXT'teki .com/.exe/.bat/.cmd uzantılarıyla; çalışma dizinine
// bakılmaz). Ad dizin içeriyorsa yalnız o dizine bakılır. Bulunamazsa nullopt.
std::optional<std::filesystem::path> find_program(std::string_view name);

// Çalışan ikilinin mutlak yolu (Windows GetModuleFileNameW, Linux /proc/self/exe); bulunamazsa boş.
// TUI/GUI `note`'u kendi dizininde, CLI arka plan senkronu için kendini bununla bulur.
std::filesystem::path current_executable();

// Kabuksuz başlatır; çocuk stdin/stdout/stderr'i devralır (editör terminali kullanır) ve çıkış kodu
// beklenir. `env_overrides` mevcut ortamın üstüne biner. `discard_output`: çocuğun stdout/stderr'i
// boş aygıta gider (ör. ssh-keygen'in bulduğu satır TUI ekranını bozmasın). Program bulunamazsa
// Error{NotFound, "error.process_not_found", {{"program", ...}}}; başlatma hatası Error{Io,
// "error.process_spawn_failed"}.
std::expected<int, Error> run_and_wait(const std::vector<std::string>& argv,
                                       const EnvOverrides& env_overrides = {}, bool discard_output = false);

class ProcessHandle;

// Beklemeden, ayrık başlatır: stdin/stdout/stderr boş aygıttır, başka hiçbir tutamaç devredilmez,
// konsol/terminal oturumu ayrıdır (Windows: DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
// POSIX: setsid). Çağıran çıkınca çocuk sürmeye devam eder. Hatalar run_and_wait ile aynı.
std::expected<ProcessHandle, Error> spawn_detached(const std::vector<std::string>& argv,
                                                   const EnvOverrides& env_overrides = {});

// En çok `timeout` bekler: çocuk bittiyse çıkış kodu (sinyalle ölümde 128 + sinyal), sürüyorsa nullopt.
// Bitmiş süreç için tekrar çağrılabilir (aynı kodu döner).
std::expected<std::optional<int>, Error> wait_with_timeout(ProcessHandle& process,
                                                           std::chrono::milliseconds timeout);

// Zorla sonlandırır ve bitmesini bekler; bitmiş süreçte etkisiz. Çocuğun tüm süreç ağacı ölür
// (torun ssh dahil): Windows'ta iş nesnesi (TerminateJobObject), POSIX'te süreç grubu (kill(-pid, SIGKILL)).
// Windows'ta çocuk bir işe atanamazsa (uyarı loglanır) yalnız doğrudan çocuk öldürülür.
void terminate(ProcessHandle& process);

// spawn_detached'in döndürdüğü süreç. Taşınabilir; yıkıcı yalnız tutamacı kapatır, süreci
// sonlandırmaz (ayrık süreç kendi başına sürer).
class ProcessHandle {
public:
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ~ProcessHandle();

    long long pid() const { return pid_; }

private:
    ProcessHandle(long long pid, void* handle, void* job) : pid_(pid), handle_(handle), job_(job) {}
    friend std::expected<ProcessHandle, Error> spawn_detached(const std::vector<std::string>&,
                                                              const EnvOverrides&);
    friend std::expected<std::optional<int>, Error> wait_with_timeout(ProcessHandle&,
                                                                      std::chrono::milliseconds);
    friend void terminate(ProcessHandle&);

    long long pid_ = 0;
    void* handle_ = nullptr;        // Windows: süreç HANDLE'ı
    void* job_ = nullptr;           // Windows: süreç ağacını toplayan iş nesnesi (atanamadıysa null)
    std::optional<int> exit_code_;  // bitti ve (POSIX'te) toplandı; PID artık başkasının olabilir
};

}  // namespace noteapp::core
