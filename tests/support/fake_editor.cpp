// Testlerde gerçek editör yerine çalışan yardımcı. Son argüman dosya yoludur.
//   FAKE_EDITOR_CONTENT   -> dosyaya ikili olarak yazılır
//   FAKE_EDITOR_ARGS_OUT  -> alınan tüm argümanlar (satır başına bir, UTF-8) bu dosyaya yazılır
//   FAKE_EDITOR_EXIT      -> çıkış kodu (varsayılan 0)
//   FAKE_EDITOR_STDOUT    -> bu metin + satır sonu stdout'a ve stderr'e yazılır (çıktı devralma testleri)
//   FAKE_EDITOR_PID_OUT   -> sürecin PID'i bu dosyaya yazılır (sonlandırma testleri)
//   FAKE_EDITOR_SLEEP_MS  -> çıkmadan önce bu kadar milisaniye uyur (zaman aşımı testleri)
//   FAKE_EDITOR_ENV_OUT   -> tüm ortam (satır başına AD=değer, UTF-8) bu dosyaya yazılır
//   FAKE_EDITOR_GRANDCHILD_PID_OUT -> kendini uzun uyuyan bir torun olarak başlatır, torunun PID'i bu
//                            dosyaya yazılır (süreç ağacı sonlandırma testleri; ssh torununu taklit eder)
//   FAKE_EDITOR_ECHO_STDIN -> stdin'i EOF'a dek gelen parçalar hâlinde stdout'a yazar (borulu çocuk testleri)
//   FAKE_EDITOR_FLOOD_BYTES -> stdin'i okumadan stdout'a bu kadar bayt yazar (dolu boru testleri)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>
// windows.h'den sonra gelmeli
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
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

// Kendini torun olarak yeniden başlatır (uzun uyur, PID'ini `pid_out`'a yazar). Torun bir daha torun
// başlatmasın diye değişken çocuğa geçmeden silinir.
void spawn_grandchild(const std::string& pid_out, const std::string& self) {
#ifdef _WIN32
    _putenv_s("FAKE_EDITOR_GRANDCHILD_PID_OUT", "");
    _putenv_s("FAKE_EDITOR_PID_OUT", pid_out.c_str());
    _putenv_s("FAKE_EDITOR_SLEEP_MS", "600000");
    std::wstring command_line = L"\"" + fs::path(std::u8string(self.begin(), self.end())).wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                       &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
#else
    unsetenv("FAKE_EDITOR_GRANDCHILD_PID_OUT");
    setenv("FAKE_EDITOR_PID_OUT", pid_out.c_str(), 1);
    setenv("FAKE_EDITOR_SLEEP_MS", "600000", 1);
    char* child_argv[] = {const_cast<char*>(self.c_str()), nullptr};
    pid_t pid = 0;
    posix_spawn(&pid, self.c_str(), nullptr, nullptr, child_argv, environ);
#endif
}

#ifdef _WIN32
// Metin kipi \n'yi \r\n'ye çevirirdi: baytlar birebir geçmeli. Yalnız borulu kiplerde çağrılır.
void binary_stdio() {
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
}
int read_fd(int fd, char* data, unsigned size) {
    return _read(fd, data, size);
}
int write_fd(int fd, const char* data, unsigned size) {
    return _write(fd, data, size);
}
#else
void binary_stdio() {}
int read_fd(int fd, char* data, unsigned size) {
    return static_cast<int>(read(fd, data, size));
}
int write_fd(int fd, const char* data, unsigned size) {
    return static_cast<int>(write(fd, data, size));
}
#endif

bool write_fully(const char* data, unsigned size) {
    while (size > 0) {
        const int written = write_fd(1, data, size);
        if (written <= 0)
            return false;
        data += written;
        size -= static_cast<unsigned>(written);
    }
    return true;
}

// stdio tamponu yerine doğrudan tanımlayıcı: gelen her parça hemen geri gider, yanıtı bekleyen çağıran takılmaz.
void echo_stdin() {
    binary_stdio();
    char chunk[4096];
    for (int count = 0; (count = read_fd(0, chunk, sizeof(chunk))) > 0;)
        if (!write_fully(chunk, static_cast<unsigned>(count)))
            return;
}

void flood_stdout(long long bytes) {
    binary_stdio();
    char chunk[65536];
    std::fill(std::begin(chunk), std::end(chunk), 'x');
    while (bytes > 0) {
        const auto size = static_cast<unsigned>(std::min<long long>(bytes, sizeof(chunk)));
        if (!write_fully(chunk, size))
            return;
        bytes -= size;
    }
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
    if (const auto env_out = env("FAKE_EDITOR_ENV_OUT")) {
        std::ofstream out(path_from_utf8(*env_out), std::ios::binary);
#ifdef _WIN32
        wchar_t* block = GetEnvironmentStringsW();
        for (const wchar_t* p = block; p != nullptr && *p != L'\0'; p += std::wcslen(p) + 1)
            out << to_utf8(p) << '\n';
        FreeEnvironmentStringsW(block);
#else
        for (char** e = environ; e != nullptr && *e != nullptr; ++e)
            out << *e << '\n';
#endif
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
    if (const auto grandchild_out = env("FAKE_EDITOR_GRANDCHILD_PID_OUT"))
        spawn_grandchild(*grandchild_out, received.front());
    if (env("FAKE_EDITOR_ECHO_STDIN"))
        echo_stdin();
    if (const auto bytes = env("FAKE_EDITOR_FLOOD_BYTES"))
        flood_stdout(std::atoll(bytes->c_str()));
    if (const auto sleep_ms = env("FAKE_EDITOR_SLEEP_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoll(sleep_ms->c_str())));
    if (const auto code = env("FAKE_EDITOR_EXIT"))
        return std::atoi(code->c_str());
    return 0;
}
