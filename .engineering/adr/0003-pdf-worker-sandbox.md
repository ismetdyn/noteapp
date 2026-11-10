# ADR 0003 — PDF işçisi kum havuzu

- Durum: kabul edildi (2026-11-11); [ADR 0002](0002-pdf-worker-boundary.md)'nin yerini aldı
- Kapsam: `src/pdfview/engine/sandbox.{hpp,cpp}` (Linux), `src/pdfview/engine/worker.cpp` (çağrı yeri),
  `src/core/child_process.cpp` (`start_confined`, Linux), `src/core/process.cpp` (Windows kısıtlama),
  `resources/defaults.toml` `[limits.pdf] worker_memory`; tasarım ve Win11 elle doğrulama adımları SPEC-core.md
  "PDF yardımcı süreci"; plan `tasks/plan-sbx.md`

## Bağlam

ADR 0002 işçiyi askıya ve çöküşe karşı bir sınır yaptı, yetki sınırı yapmadı: MuPDF'te kod çalıştırmayı başaran
bir PDF kullanıcının yapabildiği her şeyi yapabilirdi (`~/.ssh` okuma, ebeveynin `/proc/<pid>/environ`'u, not
deposu, ağ). Sandbox ertelenmişti ve senkronla gelen PDF'in otomatik açılmasından önce yapılacaktı. Tehdit
modeli: senkronla gelen kötü niyetli PDF işçide kod çalıştırır. İşçi şunlara erişememeli: kullanıcı dosyaları, ağ ve
D-Bus, başka süreçler (ebeveyn belleği, ptrace, sinyal, fork/exec), devralınan fd/tutamaçlar ve ortam (token,
AppImage değişkenleri). Kapsam dışı: işçinin bayt akışında yalan söylemesi (ADR 0002 çözüyor), CPU ve süre (ebeveyn
tutuyor).

## Karar

Bütün kısıtlar ebeveynin ilk bayt yazmasından önce kurulur. Biri kurulamazsa PDF açılmaz (fail-closed).

**Linux x86_64**
- Ebeveyn (`start_confined`): boş ortam; `RLIMIT_CORE = 0`; `RLIMIT_DATA = worker_memory` (0 sınırsız). Biri
  konamazsa çocuk öldürülür, başlatma başarısız.
- İşçi (`enter_sandbox`): `close_range(3, ~0U, 0)` (ENOSYS'te `RLIMIT_NOFILE`'a dek `close`); stdin/stdout'a sabit
  tampon; `PR_SET_NO_NEW_PRIVS`; seccomp-bpf filtresi `SECCOMP_FILTER_FLAG_TSYNC` ile.
- Filtre: mimari x86_64 değilse ya da x32 ise ve listede olmayan her çağrı için `SECCOMP_RET_KILL_PROCESS`
  (ebeveyn `exit_code=159` loglar). İzin listesi 16 girdidir: `read write exit_group brk munmap madvise
  clock_nanosleep restart_syscall getppid rt_sigprocmask rt_sigaction set_robust_list rseq` ile koşullu `mmap`
  (PROT_EXEC, MAP_SHARED, MAP_GROWSDOWN yok), `mprotect` (PROT_EXEC yok), `clone` (yalnız CLONE_THREAD). `clone3`
  ENOSYS döner (glibc `clone`a düşer). Liste `strace -ff` ile ölçüldü: Fedora glibc 2.44, AppImage araç zinciriyle
  derlenen `note` Ubuntu glibc 2.39 ve 2.43 üzerinde. Her girdinin gerekçesi `sandbox.cpp`'dedir; gerekçesi
  yazılamayan çağrı eklenmez.
- Kurulamazsa (başka mimari dahil) işçi `kSandbox = 6` ile çıkar.

**Windows**
- Kendi belirtecimizin `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)` kopyası, BUILTIN\Administrators deny-only,
  düşük bütünlük (S-1-16-4096), `CreateProcessAsUserW`, askıda başlar.
- İş nesnesi (sürdürmeden önce): `KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS = 1`, `JOB_MEMORY = worker_memory`, bütün
  `JOB_OBJECT_UILIMIT_*`.
- Ortam yalnız `SystemRoot` ve `NoDefaultCurrentDirectoryInExePath=1`.
- Süreç azaltımları: `PROHIBIT_DYNAMIC_CODE`, `IMAGE_LOAD_NO_REMOTE`, `IMAGE_LOAD_NO_LOW_LABEL`,
  `EXTENSION_POINT_DISABLE`.
- Adımlardan biri başarısızsa askıdaki çocuk öldürülür, başlatma başarısız olur (fail-closed).

**Kullanıcı kararları (2026-11-10)**
1. Linux fail-closed.
2. Landlock şimdi yok: izin listesinde yol alan çağrı yok.
3. Windows'ta düşük bütünlük + iş nesnesi şimdi, AppContainer geniş dağıtımdan önce.
4. Win11 elle doğrulamasını kullanıcı yapar (ctest `pdfview_win11`, etiket `manual-win11`; komutlar SPEC-core'da).
5. `worker_memory = "1GiB"`.

## Sonuçlar

- Linux'ta MuPDF'te kod çalıştıran PDF dosya açamaz, ağa çıkamaz, süreç oluşturamaz, ebeveyne sinyal ya da ptrace
  gönderemez; yeni çalıştırılabilir bellek eşleyemez. İddia Linux x86_64 ve ölçülen glibc'ler içindir.
- Windows'ta iddia daha zayıftır, bkz. aşağısı.

## Kalan riskler

- **Windows'ta okuma ve ağ açık.** Düşük bütünlük yalnız yazmayı keser; işçi kullanıcının dosyalarını okuyabilir ve
  ağa çıkabilir. Kapatan AppContainer'dır (ertelendi).
- **win32k kilidi yok.** Ayrı masaüstü ya da win32k kapatma yok (`note.exe` shell32/ole32 üzerinden user32/gdi32
  yükler). Ele geçirilmiş işçi ön plana çıkıp kullanıcıyı kandırabilir (ör. sahte token isteği).
- **Aynı kullanıcının düşük bütünlüklü süreçleri** birbirini açabilir; işçi `LocalLow` ve `%TEMP%\Low`'a yazabilir.
- **İç içe iş nesnesi.** UI sınırlı iş iç içe olamaz: uygulama zaten bir iş nesnesi içinde başlatıldıysa atama
  başarısız olur ve işçi reddedilir (PDF açılmaz, `step=job`). Olası yedek: `CREATE_BREAKAWAY_FROM_JOB`; Win11
  denemesi gösterirse eklenir.
- **Host glibc.** AppImage host'un glibc'sini kullanır; listede olmayan bir çağrı işçiyi 159 ile öldürür, PDF görünür
  biçimde açılmaz. Ölçülmeyen glibc sürümü bu riski taşır.
- **futex izinli değil.** Çekişen iş parçacığı eklemek (işçide bugün yalnız `watch_parent` var) listeyi yeniden
  ölçmeyi gerektirir.
- **Sanitizer derlemeleri ölçülmedi.** ASan/LSan'da bellek sınırı konmaz (gölge bellek `RLIMIT_DATA`'ya sayılır) ve
  işçi `_exit` ile çıkar; o derlemelerde filtre doğrulanmadı.
- **Doğrulanmadı (Windows): senkron token'ı.** Token Windows Kimlik Bilgisi Yöneticisi'nde (`src/sync/auth.cpp:92`);
  düşük bütünlüklü işçinin `CredReadW` ile okuyup okuyamadığı Win11'de ölçülmedi. Okuyabiliyorsa işçi token'a ulaşır.
- **Doğrulanmadı (Windows): çökme dökümü.** Linux'taki `RLIMIT_CORE 0` karşılığı yok (WER dışlaması yok); LocalDumps ya
  da WER yüklemesi açıksa çöken işçinin dökümü PDF baytlarını taşıyabilir.
- Hâlâ geçerli: MuPDF güncel tutulmalı; Linux x86_64 ve Windows dışında (diğer POSIX'ler, x86_64 olmayan Linux) PDF
  açılmaz.

## Yeniden değerlendirme koşulu

- MuPDF `openat` (yol alan çağrı) isterse Landlock zorunlu olur.
- Geniş dağıtımdan önce Windows için AppContainer.
- win32k kilidi için ayrı işçi exe'si.
- Win11 elle doğrulaması iç içe iş nesnesi reddi gösterirse `CREATE_BREAKAWAY_FROM_JOB` yedeği.
