#include "core/process.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <stdlib.h>  // _wgetenv, _wcsicmp
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

fs::path path_from_utf8(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::vector<std::string> split_list(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(separator, start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
            parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

Error not_found(const std::string& program) {
    return Error{Error::Code::NotFound, "error.process_not_found", {{"program", program}}};
}

Error spawn_failed(const std::string& program, const std::string& reason) {
    return Error{Error::Code::Io, "error.process_spawn_failed", {{"program", program}, {"reason", reason}}};
}

#ifdef _WIN32

// Yalnız Windows dalı geniş karakterli yolları UTF-8'e çevirir; POSIX'te tanımlamak -Werror'da kullanılmayan
// fonksiyon hatası verir.
std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::wstring to_wide(std::string_view utf8) {
    return path_from_utf8(utf8).wstring();
}

// CommandLineToArgvW / MSVCRT kurallarıyla tırnaklar: boşluk, tırnak ve tırnaktan önceki (ya da
// sondaki) ters eğik çizgiler doğru kaçışlanır.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');  // kapanış tırnağından önceki eğik çizgiler ikilenir
            break;
        }
        if (arg[i] == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out += arg[i];
    }
    return out + L'"';
}

bool is_launchable_extension(const std::wstring& ext) {
    for (const wchar_t* known : {L".com", L".exe", L".bat", L".cmd"})
        if (_wcsicmp(ext.c_str(), known) == 0)
            return true;
    return false;
}

bool is_batch_file(const fs::path& p) {
    const std::wstring ext = p.extension().wstring();
    return _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
}

// Mevcut ortam + geçersiz kılmalar; değişken adları Windows'ta büyük/küçük harfe duyarsızdır.
std::wstring build_environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; block != nullptr && *p != L'\0'; p += std::wcslen(p) + 1)
        entries.emplace_back(p);
    if (block != nullptr)
        FreeEnvironmentStringsW(block);

    for (const auto& [key, value] : overrides) {
        const std::wstring wide_key = to_wide(key);
        const auto same_key = [&](const std::wstring& entry) {
            const std::size_t eq = entry.find(L'=', 1);  // "=C:=..." gibi gizli girdiler 1'den başlar
            return eq == wide_key.size() && _wcsnicmp(entry.c_str(), wide_key.c_str(), eq) == 0;
        };
        const std::wstring entry = wide_key + L"=" + to_wide(value);
        if (auto it = std::find_if(entries.begin(), entries.end(), same_key); it != entries.end())
            *it = entry;
        else
            entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::wstring joined;
    for (const auto& entry : entries) {
        joined += entry;
        joined += L'\0';
    }
    joined += L'\0';
    return joined;
}

// .cmd/.bat yalnız cmd.exe üzerinden çalışır ve cmd.exe argümanı kendi kurallarıyla yeniden ayrıştırır:
// tırnak, %VAR%, !VAR!, ^ ve & | < > kabuk komutu enjekte etmeye yarar. Güvenli kaçışı olmadığı için
// bu karakterleri içeren argüman (ve yol) reddedilir.
bool has_cmd_metacharacter(const std::string& text) {
    return text.find_first_of("\"%!^&|<>") != std::string::npos;
}

#else

std::vector<std::string>
build_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> entries;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
        entries.emplace_back(*e);
    for (const auto& [key, value] : overrides) {
        const std::string prefix = key + "=";
        const auto same_key = [&](const std::string& entry) { return entry.starts_with(prefix); };
        if (auto it = std::find_if(entries.begin(), entries.end(), same_key); it != entries.end())
            *it = prefix + value;
        else
            entries.push_back(prefix + value);
    }
    return entries;
}

#endif

}  // namespace

std::optional<std::string> get_env_utf8(std::string_view name) {
#ifdef _WIN32
    // ANSI getenv Türkçe karakterli yolları bozar; geniş dizgeyle okunur. Değişken adları ASCII'dir.
    const std::wstring wide_name(name.begin(), name.end());
    const wchar_t* value = _wgetenv(wide_name.c_str());
    if (value == nullptr || *value == L'\0')
        return std::nullopt;
    return to_utf8(fs::path(std::wstring(value)));
#else
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

std::optional<fs::path> find_program(std::string_view name) {
    if (name.empty())
        return std::nullopt;
    const fs::path requested = path_from_utf8(name);
    std::error_code ec;

#ifdef _WIN32
    std::vector<fs::path> dirs;
    if (requested.has_parent_path()) {
        dirs.push_back(requested.parent_path());
    } else {
        for (auto dir : split_list(get_env_utf8("PATH").value_or(""), ';')) {
            if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
                dir = dir.substr(1, dir.size() - 2);
            dirs.push_back(path_from_utf8(dir));
        }
    }
    // PATHEXT'ten yalnız CreateProcess'in başlatabildikleri (.vbs/.js gibileri değil) alınır.
    std::vector<std::wstring> extensions;
    for (const auto& ext : split_list(get_env_utf8("PATHEXT").value_or(".COM;.EXE;.BAT;.CMD"), ';')) {
        const std::wstring wide_ext = to_wide(ext);
        if (is_launchable_extension(wide_ext))
            extensions.push_back(wide_ext);
    }
    const fs::path leaf = requested.filename();
    for (const auto& dir : dirs) {
        if (is_launchable_extension(leaf.extension().wstring()) && fs::is_regular_file(dir / leaf, ec))
            return dir / leaf;
        for (const auto& ext : extensions) {
            fs::path candidate = dir / leaf;
            candidate += ext;
            if (fs::is_regular_file(candidate, ec))
                return candidate;
        }
    }
    return std::nullopt;
#else
    const auto executable = [&](const fs::path& p) {
        return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
    };
    if (requested.has_parent_path())
        return executable(requested) ? std::optional<fs::path>(requested) : std::nullopt;
    for (const auto& dir : split_list(get_env_utf8("PATH").value_or(""), ':')) {
        if (const fs::path candidate = path_from_utf8(dir) / requested; executable(candidate))
            return candidate;
    }
    return std::nullopt;
#endif
}

std::expected<int, Error>
run_and_wait(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& env_overrides) {
    if (argv.empty())
        return std::unexpected(Error{Error::Code::InvalidPath, "error.process_empty_command", {}});
    const std::string& program_name = argv.front();

#ifdef _WIN32
    const auto program = find_program(program_name);
    if (!program)
        return std::unexpected(not_found(program_name));

    std::wstring application = program->wstring();
    std::wstring command_line;
    if (is_batch_file(*program)) {
        std::vector<std::string> checked(argv.begin() + 1, argv.end());
        checked.push_back(to_utf8(*program));
        for (const std::string& text : checked)
            if (has_cmd_metacharacter(text))
                return std::unexpected(
                    Error{Error::Code::InvalidPath, "error.process_unsafe_argument", {{"argument", text}}});
        const auto cmd = find_program("cmd.exe");
        if (!cmd)
            return std::unexpected(not_found("cmd.exe"));
        // /s ile dıştaki tırnak çifti soyulur; içte program ve argümanlar ayrı ayrı tırnaklıdır.
        command_line = quote_arg(cmd->wstring()) + L" /d /s /c \"" + quote_arg(application);
        for (std::size_t i = 1; i < argv.size(); ++i)
            command_line += L" " + quote_arg(to_wide(argv[i]));
        command_line += L"\"";
        application = cmd->wstring();
    } else {
        command_line = quote_arg(to_wide(program_name));
        for (std::size_t i = 1; i < argv.size(); ++i)
            command_line += L" " + quote_arg(to_wide(argv[i]));
    }

    std::wstring environment;
    if (!env_overrides.empty())
        environment = build_environment_block(env_overrides);

    // Standart tutamaçlar açıkça devredilir: stdout bir boruya yönlendirilmişse de çocuk aynısını görür.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT, env_overrides.empty() ? nullptr : environment.data(),
                        nullptr, &startup, &info))
        return std::unexpected(spawn_failed(program_name, std::to_string(GetLastError())));

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    return static_cast<int>(exit_code);
#else
    const std::vector<std::string> env_strings = build_environment(env_overrides);
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& e : env_strings)
        envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, program_name.c_str(), nullptr, nullptr, args.data(), envp.data());
    if (rc == ENOENT)
        return std::unexpected(not_found(program_name));
    if (rc != 0)
        return std::unexpected(spawn_failed(program_name, std::to_string(rc)));

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return std::unexpected(spawn_failed(program_name, std::to_string(errno)));
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);  // sinyalle ölüm: kabuk geleneği
#endif
}

}  // namespace noteapp::core
