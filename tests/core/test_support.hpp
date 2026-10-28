#pragma once

// Çekirdek testlerinin ortak yardımcıları (ortam değişkeni kapsamı, geçici dizin).
#include <filesystem>
#include <optional>
#include <random>
#include <string>

#ifdef _WIN32
#include <stdlib.h>  // _wputenv_s
#endif

namespace noteapp::test {

namespace fs = std::filesystem;

// Ortam değişkenini ayarlar ve çıkışta eski değeri geri yükler. Windows'ta geniş dizgeyle yazılır:
// ANSI sürümü Türkçe karakterleri bozar (üretim kodu da _wgetenv ile okur).
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::optional<std::u8string>& value) : name_(std::move(name)) {
        if (const auto old = read(name_))
            old_ = *old;
        write(name_, value);
    }
    ~ScopedEnv() { write(name_, old_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    static std::optional<std::u8string> read(const std::string& name) {
#ifdef _WIN32
        const std::wstring wname(name.begin(), name.end());
        const wchar_t* v = _wgetenv(wname.c_str());
        if (v == nullptr)
            return std::nullopt;
        return fs::path(std::wstring(v)).u8string();
#else
        const char* v = std::getenv(name.c_str());
        if (v == nullptr)
            return std::nullopt;
        return std::u8string(v, v + std::char_traits<char>::length(v));
#endif
    }
    static void write(const std::string& name, const std::optional<std::u8string>& value) {
#ifdef _WIN32
        const std::wstring wname(name.begin(), name.end());
        _wputenv_s(wname.c_str(), value ? fs::path(*value).wstring().c_str() : L"");  // boş = silinir
#else
        if (value)
            setenv(name.c_str(), std::string(value->begin(), value->end()).c_str(), 1);
        else
            unsetenv(name.c_str());
#endif
    }

    std::string name_;
    std::optional<std::u8string> old_;
};

// Türkçe karakterli geçici kök: yol/ortam dönüşümlerindeki UTF-8 hatalarını yakalar.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / fs::path(u8"noteapp-İşğü-" + to_u8(std::to_string(rd())));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    static std::u8string to_u8(const std::string& s) { return std::u8string(s.begin(), s.end()); }
    fs::path path_;
};

}  // namespace noteapp::test
