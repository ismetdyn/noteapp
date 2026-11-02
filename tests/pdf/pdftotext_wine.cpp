// Çapraz derlemede pdf_tests'in pdftotext'i: Fedora'da pdftotext.exe paketi yok, gerçek okuyucu host'unki.
// Wine Unix programını beklenebilir süreç olarak başlatmaz: CreateProcess hemen döner, çıkış kodu kaybolur,
// stderr bağlanmaz, boru da erken EOF verir. Bu yüzden sh betiği sonucu dosyalara yazar (pdftotext_wine.sh);
// burada beklenir ve stdout, stderr, çıkış kodu olduğu gibi aktarılır.
// shellapi.h windows.h türlerine ihtiyaç duyar; include sıralayıcısı onları yer değiştirmesin.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

constexpr DWORD kPollMilliseconds = 10;
constexpr DWORD kTimeoutMilliseconds = 60000;

// Wine'a özgü kernel32 dışa aktarımı: Windows yolunu host yoluna çevirir.
using WineGetUnixFileName = char*(__cdecl*)(const wchar_t*);

std::wstring widen_utf8(const char* text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    wide.pop_back();
    return wide;
}

std::string read_and_delete(const std::wstring& path) {
    std::string content;
    {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    DeleteFileW(path.c_str());
    return content;
}

void write_all(DWORD std_handle, const std::string& content) {
    DWORD written = 0;
    WriteFile(GetStdHandle(std_handle), content.data(), static_cast<DWORD>(content.size()), &written,
              nullptr);
}

}  // namespace

int main() {
    const auto to_unix = reinterpret_cast<WineGetUnixFileName>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name")));
    if (to_unix == nullptr) {
        std::fputs("pdftotext_wine: not running under Wine\n", stderr);
        return 1;
    }

    wchar_t temp_dir[MAX_PATH];
    wchar_t temp_file[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) == 0 || GetTempFileNameW(temp_dir, L"pdf", 0, temp_file) == 0) {
        std::fputs("pdftotext_wine: cannot create a temporary file\n", stderr);
        return 1;
    }
    const std::wstring base = temp_file;

    std::wstring command = L"\"/bin/sh\" \"" NOTEAPP_PDFTOTEXT_WINE_SCRIPT L"\" \"" +
                           widen_utf8(to_unix(base.c_str())) + L"\" \"" NOTEAPP_HOST_PDFTOTEXT L"\"";
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool is_windows_path = arg.size() > 2 && arg[1] == L':';
        command += L" \"" + (is_windows_path ? widen_utf8(to_unix(arg.c_str())) : arg) + L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        std::fprintf(stderr, "pdftotext_wine: cannot start the script, error %lu\n", GetLastError());
        return 1;
    }

    const std::wstring done = base + L".done";
    DWORD waited = 0;
    while (GetFileAttributesW(done.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (waited >= kTimeoutMilliseconds) {
            std::fputs("pdftotext_wine: timed out waiting for pdftotext\n", stderr);
            return 1;
        }
        Sleep(kPollMilliseconds);
        waited += kPollMilliseconds;
    }

    write_all(STD_OUTPUT_HANDLE, read_and_delete(base + L".out"));
    write_all(STD_ERROR_HANDLE, read_and_delete(base + L".err"));
    const int exit_code = std::stoi(read_and_delete(done));
    DeleteFileW(base.c_str());
    return exit_code;
}
