#include "sync/git_runtime.hpp"

#include <git2.h>
#include <stdexcept>
#include <string>

namespace noteapp::sync {

GitRuntime::GitRuntime() {
    // Kurucu değer dönemez; yarım başlatılmış libgit2 ile devam etmek sonraki her çağrıyı bozacağından
    // fırlatılır.
    if (git_libgit2_init() < 0) {
        throw std::runtime_error(std::string("libgit2 başlatılamadı: ") + git_error_last()->message);
    }
}

GitRuntime::~GitRuntime() {
    git_libgit2_shutdown();
}

}  // namespace noteapp::sync
