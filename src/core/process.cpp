#include "core/process.hpp"

#include "core/log.hpp"
#include "core/process_win.hpp"
#include "core/unique_handle.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <limits>
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

// Mevcut ortam + NoDefaultCurrentDirectoryInExePath=1 + geçersiz kılmalar (nullopt: silinir); değişken adları
// Windows'ta büyük/küçük harfe duyarsızdır. CreateProcess ve cmd.exe (ör. .cmd editörün içinden) yalın adı önce
// çalışma dizininde arar: find_program'ın göreli PATH kuralı her çocukta da geçsin. `confined` ise mevcut ortam yerine
// yalnız SystemRoot (sistem DLL'leri ona dayanır) taşınır: token, kullanıcı yolları ve AppImage değişkenleri geçmez.
std::wstring build_environment_block(const EnvOverrides& overrides, bool confined) {
    std::vector<std::wstring> entries;
    EnvOverrides all;
    if (confined) {
        if (std::optional<std::string> root = get_env_utf8("SystemRoot"))
            all.emplace_back("SystemRoot", std::move(root));
    } else {
        wchar_t* block = GetEnvironmentStringsW();
        for (const wchar_t* p = block; block != nullptr && *p != L'\0'; p += std::wcslen(p) + 1)
            entries.emplace_back(p);
        if (block != nullptr)
            FreeEnvironmentStringsW(block);
        all = host_environment_restores();
    }
    all.emplace_back("NoDefaultCurrentDirectoryInExePath", "1");
    all.insert(all.end(), overrides.begin(), overrides.end());
    for (const auto& [key, value] : all) {
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
// üyeleri öldürür. `confinement` varsa SpawnConfinement'ın iş sınırları da konur. Başarısızlıkta boş.
UniqueHandle create_kill_on_close_job(const SpawnConfinement* confinement) {
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (confinement != nullptr) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (confinement->memory_bytes != 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.JobMemoryLimit = static_cast<SIZE_T>(
                std::min<std::uint64_t>(confinement->memory_bytes, std::numeric_limits<SIZE_T>::max()));
        }
    }
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return nullptr;
    if (confinement != nullptr) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                 JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                                 JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                 JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS;
        if (!SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
            return nullptr;
    }
    return job;
}

// Kısıtlı başlatmada başarısız adım ve o andaki GetLastError.
struct SpawnFailure {
    const char* step;
    DWORD error;
};

SpawnFailure last_failure(const char* step) {
    return {step, GetLastError()};
}

// Hizalı SID tamponu: SID'in alt yetkileri DWORD'dür.
struct SidBuffer {
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> bytes{};
    PSID get() { return bytes.data(); }
};

bool make_well_known_sid(WELL_KNOWN_SID_TYPE type, SidBuffer& sid) {
    DWORD size = static_cast<DWORD>(sid.bytes.size());
    return CreateWellKnownSid(type, nullptr, sid.get(), &size) != 0;
}

// SpawnConfinement belirteci: kendi birincil belirtecimizin DISABLE_MAX_PRIVILEGE'li kopyası, bütünlüğü düşük. Çağıranın
// kendi belirtecinin kısıtlı sürümü olduğundan CreateProcessAsUserW SE_ASSIGNPRIMARYTOKEN istemez (CreateRestrictedToken
// belgesi); bütünlüğü düşürmek ayrıcalık istemez. Yeni tutamaç eskisinin erişim haklarını taşır: DUPLICATE
// (CreateRestrictedToken), ADJUST_DEFAULT (TokenIntegrityLevel), QUERY + ASSIGN_PRIMARY (CreateProcessAsUserW). Devralınan
// boru tutamaçları açıkken erişim denetiminden geçmiştir: düşük bütünlüklü çocuk onları yine okur ve yazar.
// Yönetici olarak çalışan ebeveynin (UAC kapalı ya da "Yönetici olarak çalıştır") BUILTIN\Administrators grubu etkin
// kalsaydı işçi, yöneticinin okuyabildiği her kullanıcının dosyasını okurdu: grup yalnız reddetmek için (deny-only) kalır.
// Belirteçte yoksa CreateRestrictedToken onu yok sayar.
std::expected<UniqueHandle, SpawnFailure> create_low_integrity_token() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &raw))
        return std::unexpected(last_failure("open_token"));
    const UniqueHandle own(raw);
    SidBuffer administrators;
    if (!make_well_known_sid(WinBuiltinAdministratorsSid, administrators))
        return std::unexpected(last_failure("admin_sid"));
    SID_AND_ATTRIBUTES deny_only{administrators.get(), 0};  // Attributes yok sayılır
    if (!CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE, 1, &deny_only, 0, nullptr, 0, nullptr, &raw))
        return std::unexpected(last_failure("restricted_token"));
    UniqueHandle restricted(raw);
    SidBuffer low_label;
    if (!make_well_known_sid(WinLowLabelSid, low_label))
        return std::unexpected(last_failure("low_label_sid"));
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Sid = low_label.get();
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(restricted.get(), TokenIntegrityLevel, &label,
                             static_cast<DWORD>(sizeof(label)) + GetLengthSid(low_label.get())))
        return std::unexpected(last_failure("low_integrity"));
    return restricted;
}

// Yalnız SpawnConfinement'lı çocukta; MuPDF ve işçi JIT ya da çalışma zamanı kod üretmez.
constexpr DWORD64 kConfinedMitigations = PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON |
                                         PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                                         PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
                                         PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;

// PROC_THREAD_ATTRIBUTE_LIST'in sahibi. Eklenen değerlerin belleği CreateProcess*'e kadar yaşamalıdır.
class AttributeList {
public:
    explicit AttributeList(DWORD count) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        buffer_.resize(size);
        initialized_ = size > 0 && InitializeProcThreadAttributeList(get(), count, 0, &size);
    }
    ~AttributeList() {
        if (initialized_)
            DeleteProcThreadAttributeList(get());
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    bool add(DWORD_PTR attribute, void* value, SIZE_T size) {
        return initialized_ && UpdateProcThreadAttribute(get(), 0, attribute, value, size, nullptr, nullptr);
    }
    LPPROC_THREAD_ATTRIBUTE_LIST get() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer_.data()); }

private:
    std::vector<char> buffer_;
    bool initialized_ = false;
};

// HANDLE_LIST aynı tutamacı iki kez kabul etmez.
std::vector<HANDLE> distinct_handles(const std::array<HANDLE, 3>& handles) {
    std::vector<HANDLE> distinct;
    for (HANDLE handle : handles)
        if (std::ranges::find(distinct, handle) == distinct.end())
            distinct.push_back(handle);
    return distinct;
}

// Askıdaki çocuğu işe atar. Çağıran zaten bir işin içindeyse iç içe iş Windows 8+'ta çalışır; UI sınırlı iş iç içe
// olamaz (Nested Jobs belgesi), kısıtlı çocukta bu da hatadır. Kısıtsız çocukta atama olmazsa uyarı loglanır ve doğrudan
// çocuğa düşülür (boş iş).
std::expected<UniqueHandle, SpawnFailure> attach_job(HANDLE process, const SpawnConfinement* confinement) {
    UniqueHandle job = create_kill_on_close_job(confinement);
    if (job && AssignProcessToJobObject(job.get(), process))
        return job;
    const SpawnFailure failure = last_failure("job");
    if (confinement != nullptr)
        return std::unexpected(failure);
    log_warn("process: job object unavailable (error " + std::to_string(failure.error) +
             "); terminate will only kill the direct child");
    return UniqueHandle{};
}

struct SpawnRequest {
    const std::vector<std::string>& argv;
    const EnvOverrides& env_overrides;
    std::array<HANDLE, 3> stdio;  // stdin, stdout, stderr
    DWORD creation_flags;
    const fs::path& working_directory;
    const SpawnConfinement* confinement;  // null: kısıtsız
};

// Askıda başlatır; `token` yalnız kısıtlı başlatmada dolu. Yalnız standart tutamaçlar devredilir
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): bInheritHandles=TRUE tek başına ebeveynin devredilebilir her tutamacını (ör.
// stdout'un yönlendirildiği dosya) çocuğa verir.
std::expected<PROCESS_INFORMATION, SpawnFailure> create_suspended(const SpawnRequest& request, const Command& command,
                                                                  HANDLE token) {
    const bool confined = request.confinement != nullptr;
    // Çalışma dizini değişince göreli program yolu başka yeri gösterirdi.
    std::error_code absolute_error;
    const std::wstring application = request.working_directory.empty()
                                         ? command.application
                                         : fs::absolute(command.application, absolute_error).wstring();
    std::wstring command_line = command.command_line;
    std::wstring environment = build_environment_block(request.env_overrides, confined);
    std::vector<HANDLE> inherited = distinct_handles(request.stdio);
    DWORD64 mitigations = kConfinedMitigations;
    AttributeList attributes(confined ? 2 : 1);
    if (!attributes.add(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE)) ||
        (confined && !attributes.add(PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations, sizeof(mitigations))))
        return std::unexpected(last_failure("attributes"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = request.stdio[0];
    startup.StartupInfo.hStdOutput = request.stdio[1];
    startup.StartupInfo.hStdError = request.stdio[2];
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION info{};
    const DWORD flags =
        request.creation_flags | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    const wchar_t* directory = request.working_directory.empty() ? nullptr : request.working_directory.c_str();
    const BOOL created =
        confined ? CreateProcessAsUserW(token, application.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                                        environment.data(), directory, &startup.StartupInfo, &info)
                 : CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                                  environment.data(), directory, &startup.StartupInfo, &info);
    if (!created)
        return std::unexpected(last_failure("create_process"));
    return info;
}

// spawn_with_stdio ve spawn_confined'ın ortak yolu: askıda başlatır, işe atar, sonra sürdürür.
std::expected<SpawnedProcess, Error> spawn_resumed_in_job(const SpawnRequest& request) {
    const std::string& program_name = request.argv.front();
    const auto command = prepare_command(request.argv);
    if (!command)
        return std::unexpected(command.error());
    const bool confined = request.confinement != nullptr;
    // Kısıtlı başlatmanın her adımı fail-closed ve loglanır (sır yok: adım ve Windows hata kodu); kısıtsız başlatmanın
    // hata nedeni eskisi gibi yalnız koddur.
    const auto fail = [&](const SpawnFailure& failure) {
        const std::string error = std::to_string(failure.error);
        if (!confined)
            return std::unexpected(spawn_failed(program_name, error));
        log_warn(std::string("event=sandboxed_spawn_failed step=") + failure.step + " error=" + error);
        return std::unexpected(spawn_failed(program_name, std::string(failure.step) + " " + error));
    };
    UniqueHandle token;
    if (confined) {
        auto low = create_low_integrity_token();
        if (!low)
            return fail(low.error());
        token = std::move(*low);
    }
    auto created = create_suspended(request, *command, token.get());
    if (!created)
        return fail(created.error());
    // Çocuk askıda başlar: işe atanmadan tek komut çalışmasın ki torunlar da işe katılsın. Atanamaz ya da sürdürülemezse
    // tek komut çalıştırmadan ölür.
    UniqueHandle process(created->hProcess);
    const UniqueHandle thread(created->hThread);
    auto job = attach_job(process.get(), request.confinement);
    if (!job) {
        TerminateProcess(process.get(), 1);
        return fail(job.error());
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const SpawnFailure failure = last_failure("resume");
        TerminateProcess(process.get(), 1);
        return fail(failure);
    }
    return SpawnedProcess{std::move(process), std::move(*job), created->dwProcessId};
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

// Mevcut ortam + host'a dönüşler + geçersiz kılmalar (nullopt: silinir).
std::vector<std::string> build_environment(const EnvOverrides& overrides) {
    std::vector<std::string> entries;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
        entries.emplace_back(*e);
    EnvOverrides all = host_environment_restores();
    all.insert(all.end(), overrides.begin(), overrides.end());
    for (const auto& [key, value] : all) {
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

std::expected<int, Error> wait_for_exit(pid_t pid, const std::string& program_name) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return std::unexpected(spawn_failed(program_name, std::to_string(errno)));
    }
    return exit_status(status);
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
    return spawn_resumed_in_job(
        {argv, env_overrides, {stdin_handle, stdout_handle, stderr_handle}, creation_flags, working_directory, nullptr});
}

std::expected<SpawnedProcess, Error> spawn_confined(const std::vector<std::string>& argv, HANDLE stdin_handle,
                                                    HANDLE stdout_handle, HANDLE stderr_handle,
                                                    const SpawnConfinement& confinement) {
    const EnvOverrides no_overrides;
    const fs::path same_directory;
    // Konsol yok: DETACHED_PROCESS'li çocuğa conhost açılmaz, tek süreç sınırına takılacak yardımcı süreç de olmaz.
    return spawn_resumed_in_job({argv, no_overrides, {stdin_handle, stdout_handle, stderr_handle}, DETACHED_PROCESS,
                                 same_directory, &confinement});
}

#endif

EnvOverrides without_secrets(EnvOverrides overrides) {
    for (const char* name : {"NOTEAPP_GIT_TOKEN", "NOTEAPP_GIT_TOKEN_HOST"})
        overrides.emplace_back(name, std::nullopt);
    return overrides;
}

// İşaretler packaging/linux/AppRun'la sözleşmedir. Boş değer tanımsız sayılır (get_env_utf8).
EnvOverrides host_environment_restores() {
    constexpr std::string_view kSaved = "NOTEAPP_HOST_GIO_MODULE_DIR";
    constexpr std::string_view kWasUnset = "NOTEAPP_HOST_GIO_MODULE_DIR_UNSET";
    std::optional<std::string> saved = get_env_utf8(kSaved);
    if (!saved && !get_env_utf8(kWasUnset))
        return {};
    return {{"GIO_MODULE_DIR", std::move(saved)}, {std::string(kSaved), std::nullopt},
            {std::string(kWasUnset), std::nullopt}};
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

// Göreli PATH girdisi ("." , boş girdi, node_modules/.bin) her zaman atlanır: çalışma dizinine (klonlanmış bir
// depoya) konmuş sahte git/xclip/micro aranan aracın yerine geçmesin. `absolute_only`: göreli dizinli ad da reddedilir.
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
            if (fs::path entry = path_from_utf8(dir); entry.is_absolute())
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
        if (!entry.is_absolute())
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

fs::path note_executable_beside_self() {
#ifdef _WIN32
    constexpr const char* kName = "note.exe";
#else
    constexpr const char* kName = "note";
#endif
    return current_executable().parent_path() / kName;
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

    std::wstring environment = build_environment_block(env_overrides, false);

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
        environment.data(), nullptr, &startup, &info);
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
    return wait_for_exit(*pid, program_name);
#endif
}

#ifndef _WIN32
std::expected<int, Error> run_with_input(const std::vector<std::string>& argv, std::string_view input) {
    if (argv.empty())
        return std::unexpected(Error{Error::Code::InvalidPath, "error.process_empty_command", {}});
    std::array<int, 2> ends{};
    if (pipe2(ends.data(), O_CLOEXEC) != 0)
        return std::unexpected(spawn_failed(argv.front(), std::to_string(errno)));
    UniqueFd read_end(ends[0]);
    UniqueFd write_end(ends[1]);
    // dup2 hedefi (stdin) CLOEXEC taşımaz; iki uç da CLOEXEC: yazma ucu çocuğa geçmez, EOF görünür.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, read_end.get(), STDIN_FILENO);
    const auto pid = spawn_posix(argv, without_secrets(), &actions, nullptr);  // pager token'ı görmez
    posix_spawn_file_actions_destroy(&actions);
    read_end.reset();
    if (!pid)
        return std::unexpected(pid.error());
    while (!input.empty()) {
        const ssize_t written = ::write(write_end.get(), input.data(), input.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;  // EPIPE: çocuk okumayı bıraktı
        input.remove_prefix(static_cast<std::size_t>(written));
    }
    write_end.reset();  // EOF: pager girdinin bittiğini görür
    return wait_for_exit(*pid, argv.front());
}
#endif

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
