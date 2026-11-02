#include "core/log.hpp"

#include "core/config.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
std::size_t g_max_bytes = 0;  // 0: henüz ayarlanmadı → gömülü varsayılan

unsigned long current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

// Mesajlarda dosya adı gibi dış veri geçer; `cat log.txt` terminale kaçış dizisi göndermesin. Satır sonu
// boşluk olur: kayıt tek satır kalır. C0, DEL, C1 ve geçersiz UTF-8 `\xNN` yazılır, bilgi kaybolmaz.
std::string log_safe(std::string_view message) {
    const bool valid = is_valid_utf8(message);
    std::string line;
    line.reserve(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        const bool c1 = byte == 0xC2 && i + 1 < message.size() &&
                        (static_cast<unsigned char>(message[i + 1]) & 0xE0) == 0x80;
        if (message[i] == '\n' || message[i] == '\r') {
            line += ' ';
        } else if (c1) {
            line += std::format("\\x{:02x}", static_cast<unsigned char>(message[++i]));
        } else if ((byte < 0x20 && message[i] != '\t') || byte == 0x7F || (!valid && byte >= 0x80)) {
            line += std::format("\\x{:02x}", byte);
        } else {
            line += message[i];
        }
    }
    return line;
}

// Çağıran g_mutex'i tutar. Başarısızlıklar bilerek yutulur: log, uygulamayı düşürmemeli.
void append_line(std::string_view level, std::string_view message) {
    try {
        if (g_max_bytes == 0) {
            g_max_bytes = parse_config("").config.tuning.log_max_bytes;  // tek kaynak: defaults.toml
            if (g_max_bytes == 0)
                g_max_bytes = SIZE_MAX;  // varsayılanlar kullanılamıyorsa dönüş yok, ama log da düşmez
        }

        const fs::path dir = state_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path file = dir / "log.txt";

        const auto size = fs::file_size(file, ec);
        if (!ec && size > g_max_bytes) {
            const fs::path rotated = dir / "log.1.txt";
            fs::remove(rotated, ec);  // Windows'ta rename var olan hedefin üstüne yazmaz
            fs::rename(file, rotated, ec);
        }

        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out)
            return;
        out << timestamp() << ' ' << level << ' ' << current_pid() << ' ' << log_safe(message) << '\n';
    } catch (...) {  // NOLINT(bugprone-empty-catch) -- log hatası yutulur; bkz. yukarıdaki yorum
    }
}

void write(std::string_view level, std::string_view message) {
    const std::scoped_lock lock(g_mutex);
    append_line(level, message);
}

}  // namespace

void log_info(std::string_view message) {
    write("info", message);
}
void log_warn(std::string_view message) {
    write("warn", message);
}
void log_error(std::string_view message) {
    write("error", message);
}

void init_log(std::size_t max_bytes) {
    const std::scoped_lock lock(g_mutex);
    g_max_bytes = max_bytes;
}

}  // namespace noteapp::core
