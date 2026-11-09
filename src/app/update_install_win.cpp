// Windows zip kurulumunun gerçek işlemleri: sistemin tar.exe'si (bsdtar; `..` ve mutlak yolu varsayılan
// olarak reddeder), MoveFileExW ve bağ izlemeyen silme. Çekirdek (allowlist, geri alma)
// update_install.cpp'dedir. Yalnız Windows'ta derlenir (src/app/CMakeLists.txt).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app/update_install.hpp"
#include "core/process.hpp"
#include "core/process_win.hpp"
#include "core/unique_handle.hpp"

#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::app {

namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

constexpr DWORD kReadChunk = 4096;
// Bekleme adımı: iptal ve süre aşımına tepki süresi, kapasite değil.
constexpr DWORD kPollMilliseconds = 100;

core::Error install_error(std::string key, std::vector<std::pair<std::string, std::string>> args = {}) {
    return core::Error{core::Error::Code::Io, std::move(key), std::move(args)};
}

std::string to_utf8(const fs::path& path) {
    const std::u8string u = path.u8string();
    return {u.begin(), u.end()};
}

// PATH'teki ya da çalışma dizinindeki tar.exe yerine geçemesin: sistem dizininden mutlak yol.
std::expected<fs::path, core::Error> system_tar() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    const fs::path tar = length == 0 || length >= buffer.size()
                             ? fs::path()
                             : fs::path(std::wstring(buffer.data(), length)) / L"tar.exe";
    std::error_code ec;
    if (tar.empty() || !fs::is_regular_file(tar, ec))
        return std::unexpected(
            core::Error{core::Error::Code::NotFound, "error.process_not_found", {{"program", "tar.exe"}}});
    return tar;
}

core::Error spawn_failed(const std::string& program) {
    return install_error("error.process_spawn_failed",
                         {{"program", program}, {"reason", std::to_string(GetLastError())}});
}

core::Error extract_failed(unsigned long exit_code) {
    return install_error("update.extract_failed", {{"code", std::to_string(exit_code)}});
}

struct Captured {
    DWORD exit_code = 1;
    std::string output;
};

// Süreç ağacını öldürür ve bitmesini bekler.
void kill(const core::SpawnedProcess& child) {
    if (child.job)
        TerminateJobObject(child.job.get(), 1);
    else
        TerminateProcess(child.process.get(), 1);
    WaitForSingleObject(child.process.get(), INFINITE);
}

// Kabuksuz çalıştırır (stdin ve stderr boş aygıt); `capture` ise stdout `limits.tool_output_bytes`a dek
// toplanır. Boru ancak bekleme adımlarında boşaltılır: çocuk dolu boruda takılsa da iptal ve süre aşımı
// çalışır.
std::expected<Captured, core::Error> run_tool(const std::vector<std::string>& argv, bool capture,
                                              const core::UpdateLimits& limits, const std::stop_token& stop) {
    const std::string& program = argv.front();
    core::UniqueHandle read_end;
    core::UniqueHandle write_end;
    if (capture) {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read_raw = nullptr;
        HANDLE write_raw = nullptr;
        if (!CreatePipe(&read_raw, &write_raw, &inheritable, 0))
            return std::unexpected(spawn_failed(program));
        read_end.reset(read_raw);
        write_end.reset(write_raw);
        SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);  // yalnız yazma ucu çocuğa geçer
    }
    const core::UniqueHandle null_device = core::open_null_device();
    if (!null_device)
        return std::unexpected(spawn_failed(program));
    auto child = core::spawn_with_stdio(argv, core::without_secrets(), null_device.get(),
                                        capture ? write_end.get() : null_device.get(), null_device.get(),
                                        CREATE_NO_WINDOW);
    write_end.reset();  // çocuk bitince boru EOF görsün
    if (!child)
        return std::unexpected(std::move(child.error()));

    Captured captured;
    const auto drain = [&]() -> bool {  // false: sınır aşıldı
        DWORD available = 0;
        std::array<char, kReadChunk> buffer{};
        while (capture && PeekNamedPipe(read_end.get(), nullptr, 0, nullptr, &available, nullptr) &&
               available > 0) {
            DWORD got = 0;
            if (!ReadFile(read_end.get(), buffer.data(), std::min(available, kReadChunk), &got, nullptr) ||
                got == 0)
                break;
            if (!limits.tool_output_bytes.allows(captured.output.size() + got))
                return false;
            captured.output.append(buffer.data(), got);
        }
        return true;
    };
    const auto deadline = steady_clock::now() + std::chrono::milliseconds(limits.install_timeout.value);
    for (;;) {
        const DWORD waited = WaitForSingleObject(child->process.get(), kPollMilliseconds);
        if (waited == WAIT_FAILED) {  // geçersiz tutamaç: döngü dönmesin, hata ver
            const DWORD error = GetLastError();
            kill(*child);
            return std::unexpected(install_error(
                "error.io", {{"path", "pid " + std::to_string(child->pid)}, {"detail", std::to_string(error)}}));
        }
        if (!drain()) {
            kill(*child);
            return std::unexpected(install_error(
                "update.output_too_large", {{"program", program}, {"limit", limits.tool_output_bytes.text}}));
        }
        if (waited == WAIT_OBJECT_0)
            break;
        if (stop.stop_requested()) {
            kill(*child);
            return std::unexpected(install_error("update.install_cancelled"));
        }
        if (!limits.install_timeout.unlimited() && steady_clock::now() >= deadline) {
            kill(*child);
            return std::unexpected(install_error(
                "update.install_timeout", {{"program", program}, {"limit", limits.install_timeout.text}}));
        }
    }
    GetExitCodeProcess(child->process.get(), &captured.exit_code);
    return captured;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (!line.empty())
            lines.emplace_back(line);
    }
    return lines;
}

std::expected<void, core::Error> move_path(const fs::path& from, const fs::path& to) {
    // MOVEFILE_REPLACE_EXISTING yok: hedef varsa düşer. Çalışan exe'nin adı değişebilir, silinemez.
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        return {};
    return std::unexpected(install_error(
        "update.replace_failed",
        {{"from", to_utf8(from)}, {"to", to_utf8(to)}, {"code", std::to_string(GetLastError())}}));
}

}  // namespace

ZipInstallOps windows_zip_ops(const core::UpdateLimits& limits) {
    ZipInstallOps ops;
    ops.list = [limits](const fs::path& zip,
                        std::stop_token stop) -> std::expected<std::vector<std::string>, core::Error> {
        const auto tar = system_tar();
        if (!tar)
            return std::unexpected(tar.error());
        const auto listed = run_tool({to_utf8(*tar), "-tf", to_utf8(zip)}, true, limits, stop);
        if (!listed)
            return std::unexpected(listed.error());
        if (listed->exit_code != 0)
            return std::unexpected(extract_failed(listed->exit_code));
        return split_lines(listed->output);
    };
    ops.extract = [limits](const fs::path& zip, const fs::path& destination,
                           std::stop_token stop) -> std::expected<void, core::Error> {
        const auto tar = system_tar();
        if (!tar)
            return std::unexpected(tar.error());
        const auto ran =
            run_tool({to_utf8(*tar), "-xf", to_utf8(zip), "-C", to_utf8(destination)}, false, limits, stop);
        if (!ran)
            return std::unexpected(ran.error());
        if (ran->exit_code != 0)
            return std::unexpected(extract_failed(ran->exit_code));
        return {};
    };
    ops.version_output = [limits](const fs::path& exe,
                                  std::stop_token stop) -> std::expected<std::string, core::Error> {
        auto ran = run_tool({to_utf8(exe), "--version"}, true, limits, stop);
        if (!ran)
            return std::unexpected(ran.error());
        if (ran->exit_code != 0)
            return std::string();  // sürüm uyuşmazlığı olarak raporlanır
        return std::move(ran->output);
    };
    ops.rename = move_path;
    ops.remove = remove_tree;
    return ops;
}

}  // namespace noteapp::app
