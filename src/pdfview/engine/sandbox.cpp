#include "pdfview/engine/sandbox.hpp"

#if defined(__linux__) && defined(__x86_64__)
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#endif

namespace noteapp::pdfview {

#if defined(__linux__) && defined(__x86_64__)

namespace {

// İzin listesi ölçüldü (strace -ff, NO_NEW_PRIVS sonrası; 2026-11-10): Fedora 45 glibc 2.44 üzerinde pdfview, app,
// tui, gui ve cli PDF testleri; AppImage araç zinciriyle (Ubuntu 24.04, g++-14) derlenmiş `note` hem Ubuntu 24.04
// (glibc 2.39) hem Ubuntu 26.04 host'ta (glibc 2.43) gerçek PDF'lerle (açma, metin, yapılı metin, 2x çizim,
// SIGSTOP/SIGCONT). Ölçülmeyen çağrı yoktur; gerekçesi yazılamayan çağrı eklenmez. Saat okuması (vDSO yedeği)
// ölçüldü: işçi clock_gettime/gettimeofday/time çağırmıyor. futex, exit ve rt_sigreturn yok: iş parçacığı
// beklenmez, bitmez ve sinyal işleyicisi çalışmaz.
constexpr std::array kAllowed = {
    SYS_read,             // stdin'den istek çerçeveleri
    SYS_write,            // stdout'a cevap
    SYS_exit_group,       // süreç sonu (main dönüşü, watch_parent'ın _exit'i)
    SYS_brk,              // malloc'un ana yığını
    SYS_munmap,           // büyük ayırmaların ve önbelleğin geri verilmesi
    SYS_madvise,          // glibc 2.4x iş parçacığı yığını koruma sayfası (MADV_GUARD_INSTALL)
    SYS_clock_nanosleep,  // watch_parent'ın saniyelik beklemesi (sleep_for)
    SYS_restart_syscall,  // durdurulup sürdürülen (SIGSTOP/SIGCONT) beklemenin çekirdekçe sürdürülmesi
    SYS_getppid,          // watch_parent: ebeveyn öldü mü
    SYS_rt_sigprocmask,   // pthread_create'in sinyal maskesi (yeni iş parçacığı başlarken)
    SYS_rt_sigaction,     // ilk pthread_create'te glibc'nin kendi iç sinyal işleyicisi (SIGRT_1)
    SYS_set_robust_list,  // yeni iş parçacığının glibc kaydı
    SYS_rseq,             // yeni iş parçacığının glibc kaydı
};

constexpr std::size_t kStdioBuffer = 64 * 1024;
// glibc tamponsuz akışta ilk okuma/yazmada tampon boyu için fstat (x86_64'te yol alan newfstatat) çağırır; tampon
// önceden verilince çağırmaz. newfstatat izin listesinde olamaz: herhangi bir yolun varlığını sızdırırdı.
char stdin_buffer[kStdioBuffer];
char stdout_buffer[kStdioBuffer];

// seccomp_data.args[i]'nin alt 32 biti (küçük-sonlu).
constexpr std::uint32_t arg_low(std::size_t index) {
    return static_cast<std::uint32_t>(offsetof(seccomp_data, args) + index * sizeof(std::uint64_t));
}

std::vector<sock_filter> build_filter() {
    std::vector<sock_filter> program;
    const auto stmt = [&](std::uint16_t code, std::uint32_t k) { program.push_back(BPF_STMT(code, k)); };
    const auto jump = [&](std::uint16_t code, std::uint32_t k, std::uint8_t jt, std::uint8_t jf) {
        program.push_back(BPF_JUMP(code, k, jt, jf));
    };
    constexpr std::uint16_t kLoad = BPF_LD | BPF_W | BPF_ABS;
    constexpr std::uint16_t kIfEqual = BPF_JMP | BPF_JEQ | BPF_K;
    constexpr std::uint16_t kIfAnyBit = BPF_JMP | BPF_JSET | BPF_K;
    constexpr std::uint16_t kReturn = BPF_RET | BPF_K;

    // Başka mimarinin giriş noktası (int 0x80 ile i386) aynı numaralarla başka çağrılar demektir; x32 de.
    stmt(kLoad, offsetof(seccomp_data, arch));
    jump(kIfEqual, AUDIT_ARCH_X86_64, 1, 0);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    stmt(kLoad, offsetof(seccomp_data, nr));
    jump(BPF_JMP | BPF_JGE | BPF_K, __X32_SYSCALL_BIT, 0, 1);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    for (const long nr : kAllowed) {
        jump(kIfEqual, static_cast<std::uint32_t>(nr), 0, 1);
        stmt(kReturn, SECCOMP_RET_ALLOW);
    }
    // Bellek eşleme ve koruma yalnız çalıştırılamaz sayfa için: yeni makine kodu yüklenemez. mmap ayrıca paylaşılan
    // ya da aşağı büyüyen olamaz: o eşlemeler RLIMIT_DATA'ya sayılmaz, worker_memory'yi atlatırdı (ölçüldü: glibc ve
    // MuPDF yalnız MAP_PRIVATE|MAP_ANONYMOUS[|MAP_STACK] kullanır; MAP_SHARED_VALIDATE de MAP_SHARED bitini taşır).
    jump(kIfEqual, SYS_mmap, 0, 6);
    stmt(kLoad, arg_low(2));
    jump(kIfAnyBit, PROT_EXEC, 3, 0);
    stmt(kLoad, arg_low(3));
    jump(kIfAnyBit, MAP_SHARED | MAP_GROWSDOWN, 1, 0);
    stmt(kReturn, SECCOMP_RET_ALLOW);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    jump(kIfEqual, SYS_mprotect, 0, 4);
    stmt(kLoad, arg_low(2));
    jump(kIfAnyBit, PROT_EXEC, 1, 0);
    stmt(kReturn, SECCOMP_RET_ALLOW);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    // clone yalnız iş parçacığı için (watch_parent): fork, yeni süreç ve ad alanı yok.
    jump(kIfEqual, SYS_clone, 0, 4);
    stmt(kLoad, arg_low(0));
    jump(kIfAnyBit, CLONE_THREAD, 0, 1);
    stmt(kReturn, SECCOMP_RET_ALLOW);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    // clone3'ün bayrakları bellekteki yapıdadır, BPF okuyamaz; ENOSYS'te glibc pthread_create clone'a düşer.
    jump(kIfEqual, SYS_clone3, 0, 1);
    stmt(kReturn, SECCOMP_RET_ERRNO | ENOSYS);
    stmt(kReturn, SECCOMP_RET_KILL_PROCESS);
    return program;
}

// close_range (Linux 5.9) yoksa RLIMIT_NOFILE'a dek tek tek; sınır yoksa kapatılacak aralık bilinmez.
bool close_inherited_fds() {
    if (close_range(3, ~0U, 0) == 0)
        return true;
    if (errno != ENOSYS)
        return false;
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) != 0 || files.rlim_cur == RLIM_INFINITY || files.rlim_cur > INT_MAX)
        return false;
    for (rlim_t fd = 3; fd < files.rlim_cur; ++fd)
        close(static_cast<int>(fd));
    return true;
}

}  // namespace

bool enter_sandbox() {
    if (!close_inherited_fds())
        return false;
    if (std::setvbuf(stdin, stdin_buffer, _IOFBF, kStdioBuffer) != 0 ||
        std::setvbuf(stdout, stdout_buffer, _IOFBF, kStdioBuffer) != 0)
        return false;
    // Filtre programı ayırma gerektirir: kurulmadan önce hazırlanır.
    std::vector<sock_filter> program = build_filter();
    const sock_fprog filter{static_cast<unsigned short>(program.size()), program.data()};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
    // TSYNC: filtre (ve NO_NEW_PRIVS) süreçteki her iş parçacığına konur; bugün tek iş parçacığı var ama sonradan
    // erken açılan biri filtresiz kalmasın. seccomp(2): başarı 0; eşitlenemeyen iş parçacığı varsa onun TID'i (> 0),
    // başka hatada -1. Yalnız 0 başarıdır.
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter) == 0;
}

#elif defined(__linux__)

bool enter_sandbox() {
    return false;  // izin listesi yalnız x86_64 için ölçüldü: başka mimaride PDF açılmaz (fail-closed)
}

#else

bool enter_sandbox() {
    return true;
}

#endif

}  // namespace noteapp::pdfview
