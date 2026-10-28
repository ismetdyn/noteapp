#pragma once

// process/editor testlerinin ortak yardımcıları: ortam değişkeni koruyucusu, geçici dizin, dosya okuma.
#include "core/process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace noteapp::test {

// Değişkeni atar/siler; yıkıcıda eski değere döner (başarısız testte de).
class EnvGuard {
public:
    explicit EnvGuard(std::string name) : name_(std::move(name)), old_(core::get_env_utf8(name_)) {}
    EnvGuard(std::string name, const std::string& value) : EnvGuard(std::move(name)) { set(value); }
    ~EnvGuard() {
        if (old_)
            put(*old_);
        else
            put("");
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

    void set(const std::string& value) { put(value); }
    void unset() { put(""); }

private:
    // Boş değer "tanımsız" sayılır (get_env_utf8 ile aynı sözleşme).
    void put(const std::string& value) const {
#ifdef _WIN32
        // Geniş sürüm şart: ANSI _putenv_s UTF-8 baytlarını kod sayfası sanıp Türkçe değeri bozar.
        // Boş değer değişkeni siler.
        const std::wstring wide_name(name_.begin(), name_.end());
        _wputenv_s(wide_name.c_str(),
                   std::filesystem::path(std::u8string(value.begin(), value.end())).wstring().c_str());
#else
        if (value.empty())
            unsetenv(name_.c_str());
        else
            setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    std::optional<std::string> old_;
};

class TempDir {
public:
    explicit TempDir(std::u8string_view suffix = u8"") {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                (std::u8string(u8"noteapp-proc-") +
                 std::u8string(reinterpret_cast<const char8_t*>(std::to_string(rd()).c_str())) +
                 std::u8string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::string slurp(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

inline std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace noteapp::test
