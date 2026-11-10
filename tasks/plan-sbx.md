# PDF-SBX — PDF işçisi sandbox planı

Kaynak: cpp-architect planı (2026-11-10), ADR 0002 "Yeniden değerlendirme". Kullanıcı kararları (2026-11-10, hepsi kabul):
1. Linux fail-closed: sandbox kurulamazsa işçi tek bayt okumadan `kSandbox = 6` ile çıkar, PDF açılmaz.
2. Landlock şimdi yok (izin listesinde yol alan çağrı yok). strace MuPDF'in `openat` istediğini gösterirse zorunlu olur; koşul ADR 0003'te.
3. Windows: düşük bütünlük (Low IL) + job nesnesi şimdi; AppContainer geniş dağıtımdan önce (okuma ve ağ Windows'ta açık kalır).
4. Win11 elle doğrulamasını kullanıcı yapar (`manual-win11` ctest etiketi, SPEC'te komutuyla).
5. `[limits.pdf] worker_memory = "1GiB"` (Linux `RLIMIT_DATA`, Windows job bellek sınırı; 0 = sınırsız; `context_memory`den büyük).

## Tehdit modeli
Senkronla gelen kötü niyetli PDF, MuPDF'te işçi içinde kod çalıştırabilir. İşçi erişemez: kullanıcı dosyaları (`~/.ssh`, not deposu, config, log), ağ ve D-Bus (libsecret), başka süreçler (ebeveyn belleği, `/proc/<ppid>/environ`, ptrace, sinyal, fork/exec), devralınan fd/handle ve ortam (token, AppImage değişkenleri). Kapsam dışı: işçinin bayt akışında yalan söylemesi (ADR 0002 çözüyor), CPU/zaman (ebeveyn süreyi tutuyor).

## Gerçekler
- PDF baytları stdin borusundan (`src/pdfview/pdf_document.cpp:384`, `fz_open_memory` `src/pdfview/engine/pdf_engine.cpp:101`); işçi dosya açmaz, fontlar gömülü (`cmake/MuPDF.cmake:90-104`).
- İşçi döngüsü `src/pdfview/engine/worker.cpp:42-54,114-215`; `watch_parent` iş parçacığı ilk Open'da (`:135`).
- Spawn/ortam: `src/core/child_process.cpp:34-43,262-277,347-397`; Windows `src/core/process.cpp:121-142,208-217,295-357`.
- AppImage imajı Ubuntu 24.04 (glibc 2.39, kernel başlıkları 6.8).
- KI-1 `core::host_environment_restores()` (fix/gui-desktop-integration) çocuk ortamını kuran iki yere eklendi; confined spawn ortamı sıfırdan kurar, onunla çakışmaz.

## T1 — Linux + ortak sözleşme (~8 saat)
Scope: `src/pdfview/engine/**`, `src/pdfview/CMakeLists.txt`, `src/pdfview/pdf_document.cpp`, `src/core/child_process.*`, `src/core/limits.hpp`, `src/core/config.cpp`, `resources/defaults.toml`, `tests/pdfview/**`, `tests/support/pdf_sandbox_probe.cpp`, `tests/core/**`, `SPEC-core.md`
- Yeni `src/pdfview/engine/sandbox.{hpp,cpp}`: `bool enter_sandbox()`. Yeni bağımlılık yok (ham `<linux/seccomp.h>`, `<linux/filter.h>`, `<linux/audit.h>`).
- `run_pdf_worker` (`worker.cpp:207`) içinde terminal denetiminden sonra, `serve()`den önce sırayla:
  1. `close_range(3, ~0U, 0)`; ENOSYS → `RLIMIT_NOFILE`'a kadar `close` döngüsü.
  2. `setvbuf(stdin/stdout, static 64 KiB, _IOFBF)` (glibc ilk okumada `newfstatat` çağırmasın).
  3. `prctl(PR_SET_NO_NEW_PRIVS, 1)`.
  4. `syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)` (tek iş parçacığı varken; TSYNC gerekmez).
  5. Herhangi bir adım başarısız → `kSandbox = 6` (`worker.cpp:30`) ile çık.
- Filtre: arch ≠ `AUDIT_ARCH_X86_64` ya da x32 biti → KILL; varsayılan `SECCOMP_RET_KILL_PROCESS` (ebeveyn `exit_code=159` loglar, `pdf_document.cpp:150`). Aday izin listesi (strace ile ÖLÇÜLECEK, tahmin edilmez): `read write close exit exit_group rt_sigreturn rt_sigprocmask restart_syscall brk munmap mremap madvise`, `mmap`/`mprotect` yalnız `!(prot & PROT_EXEC)`, `futex clock_nanosleep nanosleep clock_gettime getppid set_robust_list rseq`, `clone` yalnız `CLONE_THREAD`, `clone3` → `ERRNO(ENOSYS)`.
- Ölçüm: Fedora konteynerinde `--cap-add=SYS_PTRACE` ile `strace -ff` altında `pdfview_tests` + `tests/app` ve `tests/tui` PDF testleri; `prctl(PR_SET_NO_NEW_PRIVS` sonrasındaki çağrılar. Aynısı Ubuntu host'ta AppImage ile (farklı glibc). Gerekçesi yazılamayan çağrı listeye girmez.
- Ebeveyn: `ChildProcess::start_confined(argv, std::uint64_t memory_bytes)` (`child_process.hpp:27`); ortam boş başlar (ebeveynde kurulur); `posix_spawn` sonrası `prlimit(pid, RLIMIT_DATA, {m, m})` (`m != 0`), başarısızsa çocuk öldürülür, `nullopt`. `pdf_document.cpp:373` → `start_confined`. Windows dalı bu görevde yalnız ortam süzmesi.
- Config: `defaults.toml` `[limits.pdf] worker_memory = "1GiB"`, `limits.hpp:200`, `config.cpp:615`, defaults eşitlik testi.
- ASan/LSan: sanitizer derlemesinde işçi `_exit` ile çıkar; bellek testi ASan'da etiketle dışlanır (doğrulanacak).

## T2 — Windows (~6 saat) — Depends: T1
Scope: `src/core/process.cpp`, `src/core/process_win.hpp`, `src/core/child_process.cpp` (Windows dalı), `tests/support/pdf_sandbox_probe.cpp`, `tests/pdfview/**`
- `spawn_with_stdio`'ya opsiyonel `const SpawnConfinement*`: `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` → `SetTokenInformation(TokenIntegrityLevel, S-1-16-4096)` → `CreateProcessAsUserW`; STARTUPINFOEX + `CREATE_SUSPENDED` korunur.
- Job (`create_kill_on_close_job` `:208`, `ResumeThread`'den önce): `KILL_ON_JOB_CLOSE | ACTIVE_PROCESS(1) | JOB_MEMORY`, tüm `UILIMIT_*`; atama başarısız → `nullopt` (confined olmayan çocuklarda bugünkü uyarı davranışı kalır).
- Ortam sıfırdan: yalnız `SystemRoot` (+ `NoDefaultCurrentDirectoryInExePath`). `HANDLE_LIST` zaten var.
- Mitigation (sabitler mingw `winbase.h`'de grep'lenerek): `PROHIBIT_DYNAMIC_CODE`, `IMAGE_LOAD_NO_REMOTE`, `IMAGE_LOAD_NO_LOW_MANDATORY_LABEL`, `EXTENSION_POINT_DISABLE`. win32k kapatma yalnız `objdump -p note.exe` user32/gdi32 göstermezse; gösterirse ayrı işçi exe backlog.

## T3 — Belgeler + ADR 0003 + security-reviewer (~2 saat) — Depends: T1, T2
Scope: `.engineering/adr/**`, `SPEC-core.md`, `SPEC-cli.md`, `README.md`, `tasks/todo.md`
- ADR 0003 "PDF işçisi kum havuzu" (platform başına iddia, Windows'ta açık kalan okuma/ağ, Landlock koşulu); ADR 0002 durumu "yerini 0003 aldı". SPEC-core "PDF yardımcı süreci" yeniden yazılır.

## Testler (kontrol kaldırılınca düşmeli)
- Probe `tests/support/pdf_sandbox_probe.cpp` (`tests/pdfview/CMakeLists.txt:13` yanına), `start_confined` + `enter_sandbox()` sonrası argv'deki tek probe. Negatif (159/SIGSYS): `open_read /etc/passwd`, `open_write`, `stat_path`, `socket_inet`, `socket_unix`, `fork`, `vfork`, `execve`, `ptrace_ppid`, `process_vm_readv_ppid`, `kill_ppid`, `mmap_exec`, `mprotect_exec`, `prlimit_raise`; `clone3` → ENOSYS. Pozitif (exit 0): `pipe_io`, `thread`, `alloc_64MiB`; `alloc_over` (64 MiB sınırda 128 MiB dokunulmuş ayırma başarısız).
- Gerçek işçi (`pdf_document_test.cpp`): CLOEXEC'siz fd + `NOTEAPP_SBX_CANARY` env → `PdfDocument::open` → işçinin `/proc/<pid>/status` `Seccomp: 2`, `NoNewPrivs: 1`; `fd` yalnız {0,1,2}; `environ` boş; `limits` `Max data size` = config.
- Windows probe: `env_dump`, `spawn_child` (başarısız), `integrity` (Low), `write_localappdata` (ACCESS_DENIED); Wine'da uygulanmayan `manual-win11` etiketi.
- Fonksiyonel: filtre açıkken mevcut pdfview/app/tui PDF testleri yeşil.

## Riskler
- Orta: AppImage host glibc'sini kullanır; listede olmayan çağrı → işçi 159 ile ölür, PDF açılmaz (görünür). Fedora + Ubuntu strace birleşimi + duman testi.
- Orta: ASan/LSan ve RLIMIT_DATA çakışması (yukarıda).
- Düşük: sandbox kurulamazsa kullanıcı `pdf_invalid` görür; ayrım yalnız logda.
