#include "sync/git_runtime.hpp"

#include <git2.h>
#include <stdexcept>
#include <string>

namespace noteapp::sync {

namespace {

// git_error_last'in arabelleği libgit2'nin iş parçacığı durumundadır: shutdown'dan önce kopyalanır.
std::runtime_error git_failure(const char* what) {
    const git_error* error = git_error_last();
    return std::runtime_error(std::string(what) + (error != nullptr ? error->message : ""));
}

// libgit2'nin arama yolları süreç genelidir ve git_libgit2_opts kilitsiz yazar; son shutdown onları da
// sıfırlar. Her GitRuntime'da yeniden yazmak, UI'nin AutoSync'i ile SyncStatusWorker aynı anda kurulunca
// başka iş parçacığının okuduğu global arabelleğe yazıyordu (TSan: git_sysdir_set). Bu yüzden yalıtım bir
// kez, ilk kurulumda yapılır (magic static: diğer iş parçacıkları bitmesini bekler) ve burada alınan
// referans süreç sonuna dek bırakılmaz: sayaç sıfıra inmez, ayar kaybolmaz, yeniden yazılması gerekmez.
void isolate_git_config_once() {
    static const bool isolated = [] {
        if (git_libgit2_init() < 0)
            throw git_failure("libgit2 başlatılamadı: ");
        // Boş arama yolu: o seviyede hiçbir config dosyası bulunmaz (NULL ise varsayılana dönerdi).
        for (const int level : {GIT_CONFIG_LEVEL_PROGRAMDATA, GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_XDG,
                                GIT_CONFIG_LEVEL_GLOBAL}) {
            if (git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, "") < 0) {
                std::runtime_error error = git_failure("git config yalıtılamadı: ");
                git_libgit2_shutdown();
                throw error;
            }
        }
        return true;
    }();
    static_cast<void>(isolated);
}

}  // namespace

// Kurucu değer dönemez; yarım başlatılmış libgit2 ile devam etmek sonraki her çağrıyı bozacağından
// fırlatılır. Yalıtım başarısızsa magic static bir sonraki kurulumda yeniden dener.
GitRuntime::GitRuntime() {
    isolate_git_config_once();
    if (git_libgit2_init() < 0)
        throw git_failure("libgit2 başlatılamadı: ");
}

GitRuntime::~GitRuntime() {
    git_libgit2_shutdown();
}

}  // namespace noteapp::sync
