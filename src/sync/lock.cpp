#include "sync/lock.hpp"

#include "core/atomic_file.hpp"
#include "core/log.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace noteapp::sync {

namespace fs = std::filesystem;

namespace {

long long current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

bool process_alive(long long pid) {
    if (pid <= 0)
        return false;  // POSIX'te kill(0) süreç grubuna gider; geçersiz PID canlı sayılmamalı
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;  // var ama başka kullanıcının: canlı
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// İlk satır PID; okunamazsa 0 (bilinmiyor).
long long owner_pid(const std::string& content) {
    long long pid = 0;
    std::from_chars(content.data(), content.data() + content.size(), pid);
    return pid;
}

core::Error io_error(const fs::path& file, const std::error_code& ec) {
    const std::u8string path = file.generic_u8string();
    return {core::Error::Code::Io,
            "error.io",
            {{"path", std::string(path.begin(), path.end())}, {"detail", ec.message()}}};
}

}  // namespace

std::expected<SyncLock, core::Error> SyncLock::acquire(const fs::path& file,
                                                       std::chrono::minutes stale_after) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec)
        return std::unexpected(io_error(file.parent_path(), ec));

    // Belirteç: aynı süreçte aynı saniyede alınan iki kilit de ayırt edilsin (sahiplik denetimi).
    const std::string content = std::format(
        "{}\n{:%FT%TZ}\n{:08x}\n", current_pid(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()), std::random_device{}());
    // İkinci tur yalnız bayat kilit silindikten sonra; yine varsa başkası araya girmiştir.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const core::FileWriteResult written = core::write_new_file(file, content);
        if (!written.already_exists && !written.ec)
            return SyncLock(file, content);
        if (!written.already_exists) {
            fs::remove(file, ec);  // yarım yazılmış kendi dosyamız
            return std::unexpected(io_error(file, written.ec));
        }

        // Yaş dosya zamanından: yazılırken okunan yarım içerik de `stale_after` boyunca meşgul sayılır.
        const auto modified = fs::last_write_time(file, ec);
        if (ec == std::errc::no_such_file_or_directory)
            continue;  // sahibi tam şimdi bıraktı
        const std::string existing = read_text(file);
        const long long pid = owner_pid(existing);
        const bool fresh = !ec && fs::file_time_type::clock::now() - modified < stale_after;
        if (attempt > 0 || (fresh && (pid == 0 || process_alive(pid))))
            return std::unexpected(
                core::Error{core::Error::Code::Io, "sync.busy", {{"pid", std::to_string(pid)}}});

        // ponytail: iki süreç aynı bayat kilidi aynı anda görürse biri diğerinin yeni kilidini silebilir;
        // pencere milisaniyeler, libgit2'nin kendi index/ref kilitleri ikinci yazanı yine durdurur.
        core::log_warn("sync: stale lock taken over (pid " + std::to_string(pid) + ")");
        fs::remove(file, ec);
    }
    return std::unexpected(core::Error{core::Error::Code::Io, "sync.busy", {{"pid", "0"}}});
}

SyncLock::SyncLock(SyncLock&& other) noexcept
    : file_(std::move(other.file_)), content_(std::move(other.content_)) {
    other.file_.clear();
}

SyncLock::~SyncLock() {
    // Yalnız kendi kilidimiz silinir: bayat sayılıp devralındıysa artık başkasınındır.
    if (file_.empty() || read_text(file_) != content_)
        return;
    std::error_code ec;
    if (!fs::remove(file_, ec) && ec)
        core::log_warn("sync: lock could not be released: " + ec.message());
}

}  // namespace noteapp::sync
