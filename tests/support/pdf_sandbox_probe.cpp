// PDF işçisinin kum havuzu probu (PDF-SBX, tests/pdfview/sandbox_test.cpp). Test bunu core::ChildProcess::start_confined
// ile başlatır; prob pdfview::enter_sandbox()'tan sonra argv[1]'deki tek denemeyi yapar (işçinin yolu: önce kum havuzu,
// sonra iş). Çıkış kodu:
//   0  olumlu prob işini bitirdi ya da beklenen ret geldi (clone3 → ENOSYS, alloc_over başarısız)
//   1  yasak çağrı döndü: denetim yok (filtre varken süreç SIGSYS'le ölür, ebeveyn 128 + 31 = 159 görür)
//   2  bilinmeyen prob ya da hazırlık hatası
//   6  enter_sandbox() false (işçinin kSandbox'ı)
// Olumsuz problar zararsızdır: filtre yokken de dosya oluşturmaz, ebeveyni durdurmaz, başka program çalıştırmaz.
#include "pdfview/engine/sandbox.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <map>
#include <string_view>
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

namespace {

constexpr int kControlMissing = 1;
constexpr int kSetupFailed = 2;
constexpr int kSandboxRefused = 6;
constexpr std::size_t kPage = 4096;
// sandbox_test.cpp'deki kProbeMemory (128 MiB) ile eşleşir: 64 MiB sığar, 256 MiB sığmaz.
constexpr std::size_t kFitting = std::size_t{64} << 20U;
constexpr std::size_t kOver = std::size_t{256} << 20U;

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

}  // namespace

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
