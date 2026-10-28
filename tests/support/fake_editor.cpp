// Testlerde gerçek editör yerine çalışan yardımcı. Son argüman dosya yoludur.
//   FAKE_EDITOR_CONTENT   -> dosyaya ikili olarak yazılır
//   FAKE_EDITOR_ARGS_OUT  -> alınan tüm argümanlar (satır başına bir, UTF-8) bu dosyaya yazılır
//   FAKE_EDITOR_EXIT      -> çıkış kodu (varsayılan 0)
//   FAKE_EDITOR_STDOUT    -> bu metin + satır sonu stdout'a ve stderr'e yazılır (çıktı devralma testleri)
//   FAKE_EDITOR_PID_OUT   -> sürecin PID'i bu dosyaya yazılır (sonlandırma testleri)
//   FAKE_EDITOR_SLEEP_MS  -> çıkmadan önce bu kadar milisaniye uyur (zaman aşımı testleri)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
// windows.h'den sonra gelmeli
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path path_from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

#ifdef _WIN32
std::string to_utf8(const std::wstring& w) {
    const std::u8string u = fs::path(w).u8string();
    return std::string(u.begin(), u.end());
}
#endif

std::optional<std::string> env(const char* name) {
#ifdef _WIN32
    const std::string narrow(name);
    const std::wstring wide_name(narrow.begin(), narrow.end());
    const wchar_t* v = _wgetenv(wide_name.c_str());
    if (v == nullptr)
        return std::nullopt;
    return to_utf8(v);
#else
    const char* v = std::getenv(name);
    if (v == nullptr)
        return std::nullopt;
    return std::string(v);
#endif
}

// Windows'ta main'in argv'si ANSI'dir; Türkçe yolları bozmamak için komut satırı geniş okunur.
std::vector<std::string> args([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; i < count; ++i)
        out.push_back(to_utf8(wide[i]));
    LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
#endif
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> received = args(argc, argv);
    if (const auto out_path = env("FAKE_EDITOR_ARGS_OUT")) {
        std::ofstream out(path_from_utf8(*out_path), std::ios::binary);
        for (std::size_t i = 1; i < received.size(); ++i)
            out << received[i] << '\n';
    }
    if (const auto content = env("FAKE_EDITOR_CONTENT"); content && received.size() > 1) {
        std::ofstream out(path_from_utf8(received.back()), std::ios::binary);
        out.write(content->data(), static_cast<std::streamsize>(content->size()));
    }
    if (const auto text = env("FAKE_EDITOR_STDOUT")) {
        std::printf("%s\n", text->c_str());
        std::fprintf(stderr, "%s\n", text->c_str());
        std::fflush(stdout);
    }
    if (const auto pid_path = env("FAKE_EDITOR_PID_OUT")) {
#ifdef _WIN32
        const long long pid = GetCurrentProcessId();
#else
        const long long pid = getpid();
#endif
        std::ofstream(path_from_utf8(*pid_path), std::ios::binary) << pid;
    }
    if (const auto sleep_ms = env("FAKE_EDITOR_SLEEP_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoll(sleep_ms->c_str())));
    if (const auto code = env("FAKE_EDITOR_EXIT"))
        return std::atoi(code->c_str());
    return 0;
}
