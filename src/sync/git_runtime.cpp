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
    // Boş arama yolu: o seviyede hiçbir config dosyası bulunmaz (NULL ise varsayılana dönerdi).
    for (const int level : {GIT_CONFIG_LEVEL_PROGRAMDATA, GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_XDG,
                            GIT_CONFIG_LEVEL_GLOBAL}) {
        if (git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, "") < 0) {
            git_libgit2_shutdown();
            throw std::runtime_error(std::string("git config yalıtılamadı: ") + git_error_last()->message);
        }
    }
}

GitRuntime::~GitRuntime() {
    git_libgit2_shutdown();
}

}  // namespace noteapp::sync
