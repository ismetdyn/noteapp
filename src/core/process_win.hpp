#pragma once

// Windows'ta süreç başlatmanın ortak parçaları: core::spawn_detached ve core::ChildProcess aynı yolu kullanır
// (program arama, .bat/.cmd reddi, yalnız verilen tutamaçların devri, iş nesnesi). Yalnız .cpp'ler içerir.
#ifdef _WIN32

#include "core/error.hpp"
#include "core/process.hpp"
#include "core/unique_handle.hpp"

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

}  // namespace noteapp::core

#endif
