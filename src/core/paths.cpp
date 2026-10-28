#include "core/paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#ifdef _WIN32
#include <stdlib.h>  // _wgetenv
#endif

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

fs::path path_from_utf8(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

// Boş değer "tanımsız" sayılır. Windows'ta ANSI getenv Türkçe karakterli profil yollarını bozar;
// bu yüzden geniş dizgeyle okunup UTF-8'e çevrilir. Değişken adları ASCII'dir.
std::optional<std::string> get_env(std::string_view name) {
#ifdef _WIN32
    const std::wstring wide_name(name.begin(), name.end());
    const wchar_t* value = _wgetenv(wide_name.c_str());
    if (value == nullptr || *value == L'\0')
        return std::nullopt;
    const std::u8string utf8 = fs::path(std::wstring(value)).u8string();
    return std::string(utf8.begin(), utf8.end());
#else
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

std::optional<fs::path> env_path(std::string_view name) {
    if (auto value = get_env(name))
        return path_from_utf8(*value);
    return std::nullopt;
}

// Tanımsız değişken olduğu gibi bırakılır: kullanıcı yazım hatasını yolda görür, sessizce boşalmaz.
std::string expand_variables(std::string_view text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        std::size_t end = 0;  // değişken ifadesinin bittiği yer (dışlayıcı)
        std::string_view name;
        if (c == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const std::size_t close = text.find('}', i + 2);
            if (close != std::string_view::npos) {
                name = text.substr(i + 2, close - i - 2);
                end = close + 1;
            }
        } else if (c == '$') {
            end = i + 1;
            while (end < text.size() && is_name_char(text[end]))
                ++end;
            name = text.substr(i + 1, end - i - 1);
        }
#ifdef _WIN32
        else if (c == '%') {
            const std::size_t close = text.find('%', i + 1);
            if (close != std::string_view::npos) {
                name = text.substr(i + 1, close - i - 1);
                end = close + 1;
            }
        }
#endif
        const bool valid_name = !name.empty() && std::all_of(name.begin(), name.end(), is_name_char);
        if (end == 0 || !valid_name) {
            out += c;
            ++i;
            continue;
        }
        if (const auto value = get_env(name))
            out += *value;
        else
            out += text.substr(i, end - i);
        i = end;
    }
    return out;
}

fs::path app_dir_under(const fs::path& base) {
    return base / "noteapp";
}

}  // namespace

fs::path home_dir() {
#ifdef _WIN32
    constexpr std::string_view kHomeVariable = "USERPROFILE";
#else
    constexpr std::string_view kHomeVariable = "HOME";
#endif
    if (auto home = env_path(kHomeVariable))
        return *home;
    return fs::path(".");  // ev dizini bilinmiyor: çalışma dizinine düşmek çökmekten iyi
}

fs::path config_dir() {
    if (auto root = env_path("NOTEAPP_HOME"))
        return *root / "config";
#ifdef _WIN32
    return app_dir_under(env_path("APPDATA").value_or(home_dir() / "AppData" / "Roaming"));
#elif defined(__APPLE__)
    return app_dir_under(home_dir() / "Library" / "Application Support");
#else
    return app_dir_under(env_path("XDG_CONFIG_HOME").value_or(home_dir() / ".config"));
#endif
}

fs::path state_dir() {
    if (auto root = env_path("NOTEAPP_HOME"))
        return *root / "state";
#ifdef _WIN32
    return app_dir_under(env_path("LOCALAPPDATA").value_or(home_dir() / "AppData" / "Local"));
#elif defined(__APPLE__)
    return app_dir_under(home_dir() / "Library" / "Application Support");
#else
    return app_dir_under(env_path("XDG_STATE_HOME").value_or(home_dir() / ".local" / "state"));
#endif
}

fs::path config_file() {
    return config_dir() / "config.toml";
}

fs::path expand_user_path(std::string_view text) {
    const bool has_tilde =
        !text.empty() && text.front() == '~' && (text.size() == 1 || is_separator(text[1]));
    if (!has_tilde)
        return path_from_utf8(expand_variables(text));
    text.remove_prefix(1);
    // Baştaki ayırıcılar atılır: mutlak görünen kalan, `/` operatörüyle ev dizinini ezerdi.
    while (!text.empty() && is_separator(text.front()))
        text.remove_prefix(1);
    if (text.empty())
        return home_dir();
    return home_dir() / path_from_utf8(expand_variables(text));
}

}  // namespace noteapp::core
