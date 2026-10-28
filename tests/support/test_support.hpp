#pragma once

// Tüm test modüllerinin ortak yardımcıları: ortam değişkeni koruyucusu, geçici dizin, dosya okuma.
// core'a bağımlı değildir (cli/tui testleri de kullanır).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <stdlib.h>  // _wputenv_s
#endif

namespace noteapp::test {

namespace fs = std::filesystem;

// Ortam değişkenini atar/siler; yıkıcıda eski değere döner (başarısız testte de). Boş değer "tanımsız"
// sayılır (core::get_env_utf8 ile aynı sözleşme). Windows'ta geniş dizgeyle yazılır: ANSI sürümü UTF-8
// baytlarını kod sayfası sanıp Türkçe değeri bozar (üretim kodu da geniş API ile okur).
class ScopedEnv {
public:
    // Yalnız eski değeri saklar; değeri sonra set()/unset() ile değiştirmek için.
    explicit ScopedEnv(std::string name) : name_(std::move(name)), old_(read(name_)) {}
    ScopedEnv(std::string name, const std::optional<std::u8string>& value) : ScopedEnv(std::move(name)) {
        put(value.value_or(u8""));
    }
    ScopedEnv(std::string name, const std::string& value) : ScopedEnv(std::move(name)) { set(value); }
    ~ScopedEnv() { put(old_.value_or(u8"")); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    void set(const std::string& value) { put(std::u8string(value.begin(), value.end())); }
    void unset() { put(u8""); }

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
    void put(const std::u8string& value) const {
#ifdef _WIN32
        const std::wstring wname(name_.begin(), name_.end());
        _wputenv_s(wname.c_str(), fs::path(value).wstring().c_str());  // boş = silinir
#else
        if (value.empty())
            unsetenv(name_.c_str());
        else
            setenv(name_.c_str(), std::string(value.begin(), value.end()).c_str(), 1);
#endif
    }

    std::string name_;
    std::optional<std::u8string> old_;
};

// Rastgele adlı geçici dizin; çıkışta (başarısız testte de) silinir.
class TempDir {
public:
    // Varsayılan: dizini oluşturur. `suffix`, adın sonuna eklenir (boşluk/Türkçe yol testleri için).
    explicit TempDir(std::u8string_view suffix = u8"") {
        path_ = fs::temp_directory_path() /
                (std::u8string(u8"noteapp-test-") + random_part() + std::u8string(suffix));
        fs::create_directories(path_);
    }

    // Türkçe karakterli kök, OLUŞTURULMAZ: yol/ortam dönüşümlerindeki UTF-8 hatalarını yakalar; dizinin
    // henüz var olmadığı durumu (ilk yazımda oluşturma) sınayan testler bunu ister.
    struct TurkishUncreated {};
    explicit TempDir(TurkishUncreated) {
        path_ = fs::temp_directory_path() / (std::u8string(u8"noteapp-İşğü-") + random_part());
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    static std::u8string random_part() {
        std::random_device rd;
        const std::string n = std::to_string(rd());
        return std::u8string(n.begin(), n.end());
    }
    fs::path path_;
};

inline constexpr TempDir::TurkishUncreated kTurkishUncreated{};

inline std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

inline std::string utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace noteapp::test
