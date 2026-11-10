// PDF işçisinin kum havuzu probu (PDF-SBX, tests/pdfview/sandbox_test.cpp). Test bunu core::ChildProcess::start_confined
// ile başlatır; prob pdfview::enter_sandbox()'tan sonra argv[1]'deki tek denemeyi yapar (işçinin yolu: önce kum havuzu,
// sonra iş). Çıkış kodu:
//   0  olumlu prob işini bitirdi ya da beklenen ret geldi (clone3 → ENOSYS, alloc_over başarısız, Windows'ta erişim
//      reddi)
//   1  yasak çağrı döndü: denetim yok (Linux'ta filtre varken süreç SIGSYS'le ölür, ebeveyn 128 + 31 = 159 görür)
//   2  bilinmeyen prob ya da hazırlık hatası
//   6  enter_sandbox() false (işçinin kSandbox'ı)
// Olumsuz problar zararsızdır: denetim yokken de dosya bırakmaz, ebeveyni durdurmaz, yabancı program çalıştırmaz
// (Windows'ta spawn_child yalnız kendini askıda başlatmayı dener; başlarsa çalışmadan öldürülür).
#include "pdfview/engine/sandbox.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>

#ifdef __linux__
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>
#endif

namespace {

constexpr int kControlMissing = 1;
constexpr int kSetupFailed = 2;
constexpr int kSandboxRefused = 6;
constexpr std::size_t kPage = 4096;
// sandbox_test.cpp'deki kProbeMemory (128 MiB) ile eşleşir: 64 MiB sığar, 256 MiB sığmaz.
constexpr std::size_t kFitting = std::size_t{64} << 20U;
constexpr std::size_t kOver = std::size_t{256} << 20U;

#ifdef __linux__

int open_read() {
    (void)open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    return kControlMissing;
}

int open_write() {
    (void)open("/tmp", O_TMPFILE | O_WRONLY | O_CLOEXEC, 0600);  // adsız dosya: filtre yokken de iz kalmaz
    return kControlMissing;
}

int stat_path() {
    struct stat info{};
    (void)stat("/", &info);
    return kControlMissing;
}

int socket_inet() {
    (void)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    return kControlMissing;
}

int socket_unix() {
    (void)socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    return kControlMissing;
}

int fork_child() {
    if (fork() == 0)
        _exit(0);
    return kControlMissing;
}

int vfork_child() {
    if (vfork() == 0)
        _exit(0);
    return kControlMissing;
}

int exec_program() {
    std::array<char*, 2> argv{const_cast<char*>("noteapp-probe"), nullptr};
    std::array<char*, 1> envp{nullptr};
    (void)execve("/nonexistent/noteapp-probe", argv.data(), envp.data());  // ENOENT: filtre yokken de çalışmaz
    return kControlMissing;
}

int ptrace_parent() {
    (void)ptrace(PTRACE_PEEKDATA, getppid(), nullptr, nullptr);  // bağlanmaz: filtre yokken ESRCH
    return kControlMissing;
}

int read_parent_memory() {
    char byte = 0;
    iovec local{&byte, 1};
    iovec remote{&byte, 1};
    (void)process_vm_readv(getppid(), &local, 1, &remote, 1, 0);
    return kControlMissing;
}

int signal_parent() {
    (void)kill(getppid(), 0);  // sinyal 0: yalnız izin denetimi
    return kControlMissing;
}

int mmap_exec() {
    (void)mmap(nullptr, kPage, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return kControlMissing;
}

// Paylaşılan ve aşağı büyüyen anonim eşleme RLIMIT_DATA'ya sayılmaz: worker_memory'yi atlatırdı.
int mmap_shared() {
    (void)mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return kControlMissing;
}

int mmap_growsdown() {
    (void)mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
    return kControlMissing;
}

// enter_sandbox()'tan önce açılmış iş parçacığı (early_thread) filtre kurulduktan sonra yasak çağrı yapar: filtre
// yalnız kuran iş parçacığına konsaydı (TSYNC yok) çağrı dönerdi.
std::atomic<bool> early_go{false};
std::atomic<bool> early_done{false};

void start_early_thread() {
    std::thread([] {
        while (!early_go)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        early_done = true;
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }).detach();
}

int early_thread() {
    early_go = true;
    for (int waited = 0; waited < 5000 && !early_done; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return early_done ? kControlMissing : kSetupFailed;
}

int mprotect_exec() {
    void* page = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return kSetupFailed;
    (void)mprotect(page, kPage, PROT_READ | PROT_EXEC);
    return kControlMissing;
}

int prlimit_raise() {
    const rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
    (void)setrlimit(RLIMIT_DATA, &unlimited);  // glibc: prlimit64
    return kControlMissing;
}

// i386 time (13) x86_64'te izinli rt_sigaction'ın numarasıdır: mimari denetimi yoksa 32 bit giriş noktası izin
// listesini atlatırdı (i386 11 = execve, x86_64 11 = izinli munmap). time(NULL) zararsızdır.
int i386_entry() {
    long result = 13;
    // Çekirdek 64 bit süreçte int 0x80 dönüşünde r8-r11'i sıfırlar.
    __asm__ volatile("int $0x80" : "+a"(result) : "b"(0) : "r8", "r9", "r10", "r11", "memory");
    return kControlMissing;
}

int clone3_enosys() {
    errno = 0;
    const long result = syscall(SYS_clone3, nullptr, 0);
    return result == -1 && errno == ENOSYS ? 0 : kControlMissing;
}

#endif

int pipe_io() {
    std::array<char, 256> buffer{};
    std::size_t total = 0;
    while (const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), stdin)) {
        if (std::fwrite(buffer.data(), 1, got, stdout) != got)
            return kSetupFailed;
        total += got;
    }
    return total > 0 && std::fflush(stdout) == 0 ? 0 : kSetupFailed;
}

#ifdef __linux__

// İşçinin watch_parent'ı gibi: ayrılmış iş parçacığı uyur, getppid sorar ve hiç bitmez. join (futex) ve iş parçacığı
// sonu (exit, std::thread durumunun serbest bırakılması) işçide yoktur, burada da yok. Süreç exit_group'la biter.
int run_thread() {
    static std::atomic<pid_t> seen{0};
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        seen = getppid();
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }).detach();
    for (int waited = 0; waited < 5000 && seen == 0; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return seen > 0 ? 0 : kSetupFailed;
}

#endif

// Dokunulmuş ayırma: sayfalar gerçekten bağlanır.
bool touched_alloc(std::size_t size) {
    auto* block = static_cast<char*>(std::malloc(size));
    if (block == nullptr)
        return false;
    std::memset(block, 1, size);
    const bool ok = block[size - 1] == 1;
    std::free(block);
    return ok;
}

int alloc_fitting() {
    return touched_alloc(kFitting) ? 0 : kSetupFailed;
}

int alloc_over() {
    return touched_alloc(kOver) ? kControlMissing : 0;
}

#ifdef _WIN32

std::string utf8(std::wstring_view wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

// Ortam bloğunun her girdisi bir satır ("AD=değer", UTF-8); test adları karşılaştırır.
int env_dump() {
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr)
        return kSetupFailed;
    std::string out;
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1)
        out += utf8(entry) + "\n";
    FreeEnvironmentStringsW(block);
    return std::fwrite(out.data(), 1, out.size(), stdout) == out.size() && std::fflush(stdout) == 0
               ? 0
               : kSetupFailed;
}

bool write_line(const std::string& line) {
    return std::fwrite(line.data(), 1, line.size(), stdout) == line.size() && std::fputs("\n", stdout) >= 0 &&
           std::fflush(stdout) == 0;
}

// Kendini askıda başlatmayı dener; başlarsa tek komut çalıştırmadan öldürülür. Yalnız iş nesnesinin süreç sınırı
// (ERROR_NOT_ENOUGH_QUOTA: sınır aşımında işin sonlandırma kodu, JOBOBJECT_BASIC_LIMIT_INFORMATION) beklenen rettir;
// başka bir hata kodu yazılır ve hazırlık hatası sayılır: denetimi başka bir şey sanmayalım.
int spawn_child() {
    std::wstring self(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size())
        return kSetupFailed;
    self.resize(length);
    std::wstring command_line = L"\"" + self + L"\" spawned";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(self.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        nullptr, &startup, &info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_ENOUGH_QUOTA)
            return 0;
        write_line("spawn_child: CreateProcessW error " + std::to_string(error));
        return kSetupFailed;
    }
    TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return kControlMissing;
}

// Yönetici olarak (UAC kapalı ya da "Yönetici olarak çalıştır") başlayan ebeveynin belirtecinde BUILTIN\Administrators
// etkindir; kısıtlı belirteçte yalnız reddetmek için (deny-only) kalmalı. Grup yoksa (yönetici olmayan kullanıcı) prob
// ayırt edemez: bunu yazar ve 0 döner.
int admin_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return kSetupFailed;
    DWORD size = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const bool read = size > 0 && GetTokenInformation(token, TokenGroups, buffer.data(), size, &size);
    CloseHandle(token);
    alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> admins{};
    DWORD admins_size = static_cast<DWORD>(admins.size());
    if (!read || !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins.data(), &admins_size))
        return kSetupFailed;
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
    const SID_AND_ATTRIBUTES* group = groups->Groups;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if (!EqualSid(group[i].Sid, admins.data()))
            continue;
        const DWORD attributes = group[i].Attributes;
        if (!write_line("administrators: attributes " + std::to_string(attributes)))
            return kSetupFailed;
        const bool deny_only = (attributes & SE_GROUP_USE_FOR_DENY_ONLY) != 0 && (attributes & SE_GROUP_ENABLED) == 0;
        return deny_only ? 0 : kControlMissing;
    }
    return write_line("administrators: absent") ? 0 : kSetupFailed;
}

// Kendi belirtecinin bütünlük düzeyi düşük (S-1-16-4096) ya da daha azsa 0.
int integrity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return kSetupFailed;
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const bool read = size > 0 && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size);
    CloseHandle(token);
    if (!read)
        return kSetupFailed;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1U);
    return rid <= SECURITY_MANDATORY_LOW_RID ? 0 : kControlMissing;
}

// stdin'deki UTF-8 dizine (testin %LOCALAPPDATA%'sı) dosya açmayı dener; açılırsa kapanınca silinir. Düşük bütünlükteki
// süreç orta etiketli kullanıcı dizinine yazamaz: ERROR_ACCESS_DENIED.
int write_localappdata() {
    std::string dir;
    std::array<char, 256> chunk{};
    while (const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stdin))
        dir.append(chunk.data(), got);
    if (dir.empty())
        return kSetupFailed;
    const int size = MultiByteToWideChar(CP_UTF8, 0, dir.data(), static_cast<int>(dir.size()), nullptr, 0);
    std::wstring path(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.data(), static_cast<int>(dir.size()), path.data(), size);
    path += L"\\noteapp-sbx-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return kControlMissing;
    }
    return GetLastError() == ERROR_ACCESS_DENIED ? 0 : kSetupFailed;
}

// PROHIBIT_DYNAMIC_CODE (ACG): yürütülebilir sayfa ayrılamaz.
int dynamic_code() {
    void* page = VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (page == nullptr)
        return 0;
    VirtualFree(page, 0, MEM_RELEASE);
    return kControlMissing;
}

#else

// enter_sandbox()'tan önce: seccomp() EPERM dönsün diye kendi filtresini kurar (NO_NEW_PRIVS ister).
bool refuse_seccomp() {
    std::array<sock_filter, 4> program{{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_seccomp, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    }};
    const sock_fprog filter{static_cast<unsigned short>(program.size()), program.data()};
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) == 0;
}

int refused() {
    return kControlMissing;  // enter_sandbox() buraya gelmeden false dönmeliydi
}

#endif

}  // namespace

#ifdef _WIN32

// Sınırlar ebeveynin başlatmasındadır (core::ChildProcess::start_confined); enter_sandbox() işçideki gibi çağrılır.
int main(int argc, char** argv) {
    const std::map<std::string_view, int (*)()> probes{
        {"env_dump", env_dump},
        {"spawn_child", spawn_child},
        {"admin_sid", admin_sid},
        {"integrity", integrity},
        {"write_localappdata", write_localappdata},
        {"dynamic_code", dynamic_code},
        {"pipe_io", pipe_io},
        {"alloc_64MiB", alloc_fitting},
        {"alloc_over", alloc_over},
    };
    if (argc != 2)
        return kSetupFailed;
    const auto probe = probes.find(argv[1]);
    if (probe == probes.end())
        return kSetupFailed;
    if (!noteapp::pdfview::enter_sandbox())
        return kSandboxRefused;
    return probe->second();
}

#else

int main(int argc, char** argv) {
    const std::map<std::string_view, int (*)()> probes{
        {"open_read", open_read},
        {"open_write", open_write},
        {"stat_path", stat_path},
        {"socket_inet", socket_inet},
        {"socket_unix", socket_unix},
        {"fork", fork_child},
        {"vfork", vfork_child},
        {"execve", exec_program},
        {"ptrace_ppid", ptrace_parent},
        {"process_vm_readv_ppid", read_parent_memory},
        {"kill_ppid", signal_parent},
        {"mmap_exec", mmap_exec},
        {"mprotect_exec", mprotect_exec},
        {"mmap_shared", mmap_shared},
        {"mmap_growsdown", mmap_growsdown},
        {"early_thread", early_thread},
        {"prlimit_raise", prlimit_raise},
        {"i386_entry", i386_entry},
        {"clone3", clone3_enosys},
        {"pipe_io", pipe_io},
        {"thread", run_thread},
        {"alloc_64MiB", alloc_fitting},
        {"alloc_over", alloc_over},
        {"refused", refused},
    };
    if (argc != 2)
        return kSetupFailed;
    const auto probe = probes.find(argv[1]);
    if (probe == probes.end())
        return kSetupFailed;
    // Olumsuz problar SIGSYS'le ölür: her test koşusu host'un çekirdek dökümü işleyicisini (apport, systemd-coredump)
    // tetiklemesin.
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        return kSetupFailed;
    if (probe->first == "refused" && !refuse_seccomp())
        return kSetupFailed;
    if (probe->first == "early_thread")
        start_early_thread();
    if (!noteapp::pdfview::enter_sandbox())
        return kSandboxRefused;
    return probe->second();
}
#endif
