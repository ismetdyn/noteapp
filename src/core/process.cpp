#include "core/process.hpp"

#include "core/log.hpp"
#include "core/process_win.hpp"
#include "core/unique_handle.hpp"

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
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
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

// Bekleme hatası pratikte olmaz (geçersiz tutamaç); ayrı katalog metni yerine genel G/Ç hatası.
Error wait_failed(long long pid, unsigned long code) {
    return Error{Error::Code::Io,
                 "error.io",
                 {{"path", "pid " + std::to_string(pid)}, {"detail", std::to_string(code)}}};
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

// Mevcut ortam + geçersiz kılmalar (nullopt: silinir); değişken adları Windows'ta büyük/küçük harfe
// duyarsızdır.
std::wstring build_environment_block(const EnvOverrides& overrides) {
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; block != nullptr && *p != L'\0'; p += std::wcslen(p) + 1)
        entries.emplace_back(p);
    if (block != nullptr)
        FreeEnvironmentStringsW(block);

    for (const auto& [key, value] : overrides) {
        const std::wstring wide_key = to_wide(key);
        std::erase_if(entries, [&](const std::wstring& entry) {
            const std::size_t eq = entry.find(L'=', 1);  // "=C:=..." gibi gizli girdiler 1'den başlar
            return eq == wide_key.size() && _wcsnicmp(entry.c_str(), wide_key.c_str(), eq) == 0;
        });
        if (value)
            entries.push_back(wide_key + L"=" + to_wide(*value));
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

struct Command {
    std::wstring application;
    std::wstring command_line;
};

// cmd.exe PATH'ten değil sistem dizininden gelir: PATH'in önündeki sahte cmd.exe betiği çalıştıran kabuğun
// yerine geçemez.
std::optional<fs::path> system_cmd() {
    std::wstring dir(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(dir.data(), static_cast<UINT>(dir.size()));
    if (length == 0 || length >= dir.size())
        return std::nullopt;
    dir.resize(length);
    return fs::path(dir) / L"cmd.exe";
}

std::expected<Command, Error> prepare_command(const std::vector<std::string>& argv) {
    const std::string& program_name = argv.front();
    const auto program = find_program(program_name);
    if (!program)
        return std::unexpected(not_found(program_name));

    Command command{program->wstring(), {}};
    if (is_batch_file(*program)) {
        std::vector<std::string> checked(argv.begin() + 1, argv.end());
        checked.push_back(to_utf8(*program));
        for (const std::string& text : checked)
            if (has_cmd_metacharacter(text))
                return std::unexpected(
                    Error{Error::Code::InvalidPath, "error.process_unsafe_argument", {{"argument", text}}});
        const auto cmd = system_cmd();
        if (!cmd)
            return std::unexpected(not_found("cmd.exe"));
        // /s ile dıştaki tırnak çifti soyulur; içte program ve argümanlar ayrı ayrı tırnaklıdır.
        command.command_line = quote_arg(cmd->wstring()) + L" /d /s /c \"" + quote_arg(command.application);
        for (std::size_t i = 1; i < argv.size(); ++i)
            command.command_line += L" " + quote_arg(to_wide(argv[i]));
        command.command_line += L"\"";
        command.application = cmd->wstring();
    } else {
        command.command_line = quote_arg(to_wide(program_name));
        for (std::size_t i = 1; i < argv.size(); ++i)
            command.command_line += L" " + quote_arg(to_wide(argv[i]));
    }
    return command;
}

// Yalnız çocuğun ağacını toplayan, adsız iş nesnesi; son tutamaç kapanırsa (ör. uygulama çökerse)
// üyeleri öldürür. Başarısızlıkta boş.
UniqueHandle create_kill_on_close_job() {
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return nullptr;
    return job;
}

#else

// posix_spawn'ın istediği NULL sonlu dizi; işaretçiler `strings`in ömrüne bağlıdır.
std::vector<char*> c_strings(const std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (const std::string& s : strings)
        out.push_back(const_cast<char*>(s.c_str()));
    out.push_back(nullptr);
    return out;
}

// Mevcut ortam + geçersiz kılmalar (nullopt: silinir).
std::vector<std::string> build_environment(const EnvOverrides& overrides) {
    std::vector<std::string> entries;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
        entries.emplace_back(*e);
    for (const auto& [key, value] : overrides) {
        const std::string prefix = key + "=";
        std::erase_if(entries, [&](const std::string& entry) { return entry.starts_with(prefix); });
        if (value)
            entries.push_back(prefix + *value);
    }
    return entries;
}

// `actions`/`attributes` boş olabilir (devralınan stdio, varsayılan öznitelikler).
std::expected<pid_t, Error> spawn_posix(const std::vector<std::string>& argv,
                                        const EnvOverrides& env_overrides,
                                        const posix_spawn_file_actions_t* actions,
                                        const posix_spawnattr_t* attributes) {
    // posix_spawnp ikinci bir PATH araması yapardı (boş girdi = çalışma dizini); find_program'ın bulduğu yol
    // aynen başlatılır, argv[0] verilen ad kalır.
    const auto program = find_program(argv.front());
    if (!program)
        return std::unexpected(not_found(argv.front()));
    const std::vector<std::string> env_strings = build_environment(env_overrides);
    std::vector<char*> args = c_strings(argv);
    std::vector<char*> envp = c_strings(env_strings);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, program->c_str(), actions, attributes, args.data(), envp.data());
    if (rc == ENOENT)
        return std::unexpected(not_found(argv.front()));
    if (rc != 0)
        return std::unexpected(spawn_failed(argv.front(), std::to_string(rc)));
    return pid;
}

int exit_status(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);  // sinyalle ölüm: kabuk geleneği
}

#endif

}  // namespace

#ifdef _WIN32

UniqueHandle open_null_device() {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    return own_win_handle<UniqueHandle>(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                                    OPEN_EXISTING, 0, nullptr));
}

std::expected<SpawnedProcess, Error> spawn_with_stdio(const std::vector<std::string>& argv,
                                                      const EnvOverrides& env_overrides, HANDLE stdin_handle,
                                                      HANDLE stdout_handle, HANDLE stderr_handle,
                                                      DWORD creation_flags,
                                                      const fs::path& working_directory) {
    const std::string& program_name = argv.front();
    const auto command = prepare_command(argv);
    if (!command)
        return std::unexpected(command.error());
    // Çalışma dizini değişince göreli program yolu başka yeri gösterirdi.
    std::error_code absolute_error;
    const std::wstring application = working_directory.empty()
                                         ? command->application
                                         : fs::absolute(command->application, absolute_error).wstring();
    std::wstring command_line = command->command_line;
    std::wstring environment;
    if (!env_overrides.empty())
        environment = build_environment_block(env_overrides);

    // Yalnız standart tutamaçlar devredilir (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): bInheritHandles=TRUE tek başına
    // ebeveynin devredilebilir her tutamacını (ör. stdout'un yönlendirildiği dosya) çocuğa verir. Liste aynı
    // tutamacı iki kez kabul etmez.
    std::vector<HANDLE> inherited;  // liste CreateProcessW'e kadar bu belleği okur
    for (HANDLE handle : {stdin_handle, stdout_handle, stderr_handle})
        if (std::ranges::find(inherited, handle) == inherited.end())
            inherited.push_back(handle);
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<char> list_buffer(list_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    BOOL ok = InitializeProcThreadAttributeList(attributes, 1, 0, &list_size);
    if (ok)
        ok = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                       inherited.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_handle;
    startup.StartupInfo.hStdOutput = stdout_handle;
    startup.StartupInfo.hStdError = stderr_handle;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    if (ok)
        ok = CreateProcessW(
            application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
            creation_flags | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
            env_overrides.empty() ? nullptr : environment.data(),
            working_directory.empty() ? nullptr : working_directory.c_str(), &startup.StartupInfo, &info);
    const DWORD spawn_error = GetLastError();
    if (list_size > 0)
        DeleteProcThreadAttributeList(attributes);
    if (!ok)
        return std::unexpected(spawn_failed(program_name, std::to_string(spawn_error)));
    // Çocuk askıda başlar: işe atanmadan tek komut çalışmasın ki torunlar da işe katılsın. Çağıran zaten
    // bir işin içindeyse (CI/IDE) iç içe iş Windows 8+'ta çalışır; olmazsa doğrudan çocuğa düşülür.
    UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);
    UniqueHandle job = create_kill_on_close_job();
    if (!job || !AssignProcessToJobObject(job.get(), process.get())) {
        log_warn("process: job object unavailable (error " + std::to_string(GetLastError()) +
                 "); terminate will only kill the direct child");
        job.reset();
    }
    ResumeThread(thread.get());
    return SpawnedProcess{std::move(process), std::move(job), info.dwProcessId};
}

#endif

EnvOverrides without_secrets(EnvOverrides overrides) {
    for (const char* name : {"NOTEAPP_GIT_TOKEN", "NOTEAPP_GIT_TOKEN_HOST"})
        overrides.emplace_back(name, std::nullopt);
    return overrides;
}

std::optional<std::string> get_env_utf8(std::string_view name) {
#ifdef _WIN32
    // ANSI getenv Türkçe karakterli yolları bozar; geniş dizgeyle okunur. Değişken adları ASCII'dir.
    const std::wstring wide_name(name.begin(), name.end());
    const wchar_t* value = _wgetenv(wide_name.c_str());
    if (value == nullptr || *value == L'\0')
        return std::nullopt;
    return to_utf8(fs::path(std::wstring(value)));
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- üründe setenv/putenv yok, salt okuma yarışmaz
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

namespace {

// `absolute_only`: çalışma dizinine göre çözülecek hiçbir aday denenmez.
std::optional<fs::path> search_program(std::string_view name, bool absolute_only) {
    if (name.empty())
        return std::nullopt;
    const fs::path requested = path_from_utf8(name);
    if (absolute_only && requested.has_parent_path() && !requested.is_absolute())
        return std::nullopt;
    std::error_code ec;

#ifdef _WIN32
    std::vector<fs::path> dirs;
    if (requested.has_parent_path()) {
        dirs.push_back(requested.parent_path());
    } else {
        for (auto dir : split_list(get_env_utf8("PATH").value_or(""), ';')) {
            if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
                dir = dir.substr(1, dir.size() - 2);
            if (fs::path entry = path_from_utf8(dir); !absolute_only || entry.is_absolute())
                dirs.push_back(std::move(entry));
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
        const fs::path entry = path_from_utf8(dir);
        if (absolute_only && !entry.is_absolute())
            continue;
        if (fs::path candidate = entry / requested; executable(candidate))
            return candidate;
    }
    return std::nullopt;
#endif
}

}  // namespace

std::optional<fs::path> find_program(std::string_view name) {
    return search_program(name, false);
}

std::optional<fs::path> find_program_on_absolute_path(std::string_view name) {
    return search_program(name, true);
}

fs::path current_executable() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
            return {};
        if (size < buffer.size())
            return fs::path(buffer.substr(0, size));
        buffer.resize(buffer.size() * 2);  // uzun yol: kesildi, büyütüp yeniden dene
    }
#elif defined(__linux__)
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#else
    // ponytail: macOS/BSD'de kendi yolu yok; PATH'teki `note` (ön yüzler kendi dizinlerine bakar).
    return find_program("note").value_or(fs::path());
#endif
}

std::expected<int, Error> run_and_wait(const std::vector<std::string>& argv,
                                       const EnvOverrides& env_overrides, bool discard_output) {
    if (argv.empty())
        return std::unexpected(Error{Error::Code::InvalidPath, "error.process_empty_command", {}});
    const std::string& program_name = argv.front();

#ifdef _WIN32
    const auto command = prepare_command(argv);
    if (!command)
        return std::unexpected(command.error());
    std::wstring application = command->application;
    std::wstring command_line = command->command_line;

    std::wstring environment;
    if (!env_overrides.empty())
        environment = build_environment_block(env_overrides);

    UniqueHandle null_device;
    if (discard_output) {
        null_device = open_null_device();
        if (!null_device)
            return std::unexpected(spawn_failed(program_name, std::to_string(GetLastError())));
    }
    // Standart tutamaçlar açıkça devredilir: stdout bir boruya yönlendirilmişse de çocuk aynısını görür.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = discard_output ? null_device.get() : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = discard_output ? null_device.get() : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(
        application.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
        env_overrides.empty() ? nullptr : environment.data(), nullptr, &startup, &info);
    const DWORD spawn_error = GetLastError();
    null_device.reset();
    if (!started)
        return std::unexpected(spawn_failed(program_name, std::to_string(spawn_error)));
    const UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);

    WaitForSingleObject(process.get(), INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.get(), &exit_code);
    return static_cast<int>(exit_code);
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (discard_output) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
    const auto pid = spawn_posix(argv, env_overrides, &actions, nullptr);
    posix_spawn_file_actions_destroy(&actions);
    if (!pid)
        return std::unexpected(pid.error());

    int status = 0;
    while (waitpid(*pid, &status, 0) < 0) {
        if (errno != EINTR)
            return std::unexpected(spawn_failed(program_name, std::to_string(errno)));
    }
    return exit_status(status);
#endif
}

std::expected<ProcessHandle, Error> spawn_detached(const std::vector<std::string>& argv,
                                                   const EnvOverrides& env_overrides) {
    if (argv.empty())
        return std::unexpected(Error{Error::Code::InvalidPath, "error.process_empty_command", {}});

#ifdef _WIN32
    // POSIX dalı hatayı spawn_posix'ten hazır alır; adı yalnız Windows dalı kullanır (-Werror).
    const std::string& program_name = argv.front();
    const UniqueHandle null_device = open_null_device();
    if (!null_device)
        return std::unexpected(spawn_failed(program_name, std::to_string(GetLastError())));
    auto spawned = spawn_with_stdio(argv, env_overrides, null_device.get(), null_device.get(), null_device.get(),
                                    DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW);
    if (!spawned)
        return std::unexpected(spawned.error());
    return ProcessHandle(spawned->pid, spawned->process.release(), spawned->job.release());
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Yeni oturum: terminal kapanınca (SIGHUP) ya da ön plandaki Ctrl-C ile senkron yarıda ölmesin.
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // en azından ayrı süreç grubu
#endif
    // ponytail: CLOEXEC olmayan diğer fd'ler devredilir; gerekirse posix_spawn_file_actions_addclosefrom_np.
    const auto pid = spawn_posix(argv, env_overrides, &actions, &attributes);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (!pid)
        return std::unexpected(pid.error());
    return ProcessHandle(*pid, nullptr, nullptr);
#endif
}

std::expected<std::optional<int>, Error> wait_with_timeout(ProcessHandle& process,
                                                           std::chrono::milliseconds timeout) {
    if (process.exit_code_)
        return process.exit_code_;
#ifdef _WIN32
    const auto handle = static_cast<HANDLE>(process.handle_);
    const DWORD waited =
        WaitForSingleObject(handle, static_cast<DWORD>(std::max<long long>(timeout.count(), 0)));
    if (waited == WAIT_TIMEOUT)
        return std::optional<int>();
    DWORD exit_code = 1;
    if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(handle, &exit_code))
        return std::unexpected(wait_failed(process.pid_, GetLastError()));
    process.exit_code_ = static_cast<int>(exit_code);
#else
    // ponytail: waitpid'in zaman aşımı yok; 10 ms aralıkla yoklanır (senkron saniyeler sürer).
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(static_cast<pid_t>(process.pid_), &status, WNOHANG);
        if (done == static_cast<pid_t>(process.pid_)) {
            process.exit_code_ = exit_status(status);
            break;
        }
        if (done < 0 && errno != EINTR)
            return std::unexpected(wait_failed(process.pid_, errno));
        if (std::chrono::steady_clock::now() >= deadline)
            return std::optional<int>();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    return process.exit_code_;
}

void terminate(ProcessHandle& process) {
    if (process.exit_code_)
        return;
#ifdef _WIN32
    const auto handle = static_cast<HANDLE>(process.handle_);
    // Zaten bittiyse başarısız olur; bekleme yine de kodu toplar.
    if (process.job_ == nullptr || !TerminateJobObject(static_cast<HANDLE>(process.job_), 1))
        TerminateProcess(handle, 1);
    WaitForSingleObject(handle, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(handle, &exit_code);
    process.exit_code_ = static_cast<int>(exit_code);
#else
    // Çocuk SETSID/SETPGROUP ile kendi grubunun lideri: -pid torunları da hedefler.
    kill(-static_cast<pid_t>(process.pid_), SIGKILL);
    kill(static_cast<pid_t>(process.pid_), SIGKILL);  // grup kurulamadıysa en azından çocuk
    int status = 0;
    while (waitpid(static_cast<pid_t>(process.pid_), &status, 0) < 0 && errno == EINTR) {
    }
    process.exit_code_ = exit_status(status);
#endif
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : pid_(other.pid_), handle_(std::exchange(other.handle_, nullptr)),
      job_(std::exchange(other.job_, nullptr)), exit_code_(other.exit_code_) {}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
        ProcessHandle old(std::move(*this));  // eski tutamaç yıkıcısında kapanır
        pid_ = other.pid_;
        handle_ = std::exchange(other.handle_, nullptr);
        job_ = std::exchange(other.job_, nullptr);
        exit_code_ = other.exit_code_;
    }
    return *this;
}

ProcessHandle::~ProcessHandle() {
#ifdef _WIN32
    if (job_ != nullptr) {
        // Tutamacı bırakmak ayrık süreci öldürmemeli (CLI arka plan senkronunu başlatıp çıkar): kapatmadan
        // önce "kapanınca öldür" bayrağı kaldırılır; ağacı yalnız terminate() öldürür.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        SetInformationJobObject(static_cast<HANDLE>(job_), JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        CloseHandle(static_cast<HANDLE>(job_));
    }
    if (handle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(handle_));
#endif
}

}  // namespace noteapp::core
