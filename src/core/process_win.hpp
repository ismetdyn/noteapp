#pragma once

// Windows'ta süreç başlatmanın ortak parçaları: core::spawn_detached ve core::ChildProcess aynı yolu kullanır
// (program arama, .bat/.cmd reddi, yalnız verilen tutamaçların devri, iş nesnesi). Yalnız .cpp'ler içerir.
#ifdef _WIN32

#include "core/error.hpp"
#include "core/process.hpp"
#include "core/unique_handle.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace noteapp::core {

struct SpawnedProcess {
    UniqueHandle process;
    UniqueHandle job;  // KILL_ON_JOB_CLOSE iş nesnesi; çocuk atanamadıysa boş (uyarı loglanır)
    unsigned long pid = 0;
};

// Güvenilmez girdiyi işleyen çocuğun sınırları (PDF-SBX, core::ChildProcess::start_confined). spawn_confined çocuğu
// kendi belirtecimizin en büyük ayrıcalıkları atılmış, düşük bütünlüklü (S-1-16-4096) kopyasıyla ve süreç oluşturma
// azaltımlarıyla (PROHIBIT_DYNAMIC_CODE, IMAGE_LOAD_NO_REMOTE, IMAGE_LOAD_NO_LOW_LABEL, EXTENSION_POINT_DISABLE) başlatır.
// Ortamı yalnız SystemRoot ve NoDefaultCurrentDirectoryInExePath=1'dir (ebeveynin ortamı ve host_environment_restores
// geçmez). İş nesnesi tek süreçle (torun yok), `memory_bytes` sıfır değilse o kadar
// ayrılmış bellekle ve bütün UI sınırlarıyla kurulur. Biri kurulamazsa askıdaki çocuk öldürülür, başlatma başarısız
// olur (fail-closed).
struct SpawnConfinement {
    std::uint64_t memory_bytes = 0;  // 0: sınırsız
};

// Devredilebilir boş aygıt (NUL) tutamacı; hatada boş.
UniqueHandle open_null_device();

// Kabuksuz başlatır: program find_program ile bulunur, .bat/.cmd sistem cmd.exe'siyle çalışır ve
// metakarakterli argüman reddedilir (Error{InvalidPath, "error.process_unsafe_argument"}). Çocuğa yalnız
// verilen standart tutamaçlar devredilir; hepsi devredilebilir olmalıdır, aynı tutamaç birden çok kez
// verilebilir. Çocuk askıda başlar, iş nesnesine atanır, sonra sürdürülür: torunlar da işe katılır.
// Ortam devralınır, üstüne NoDefaultCurrentDirectoryInExePath=1 ve `env_overrides` biner. `working_directory` boş değilse çocuk orada başlar (program
// yolu önce mutlak yapılır). Hatalar run_and_wait ile aynı.
std::expected<SpawnedProcess, Error> spawn_with_stdio(const std::vector<std::string>& argv,
                                                      const EnvOverrides& env_overrides, HANDLE stdin_handle,
                                                      HANDLE stdout_handle, HANDLE stderr_handle,
                                                      DWORD creation_flags,
                                                      const std::filesystem::path& working_directory = {});

// spawn_with_stdio gibi, ama SpawnConfinement sınırlarıyla, konsolsuz (DETACHED_PROCESS) ve çalışma dizini
// değiştirmeden. Sınırlardan biri kurulamazsa çocuk öldürülür, hata döner ve neden loglanır.
std::expected<SpawnedProcess, Error> spawn_confined(const std::vector<std::string>& argv, HANDLE stdin_handle,
                                                    HANDLE stdout_handle, HANDLE stderr_handle,
                                                    const SpawnConfinement& confinement);

}  // namespace noteapp::core

#endif
