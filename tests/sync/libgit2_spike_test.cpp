#include "sync/git_runtime.hpp"

#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <git2.h>
#include <iterator>
#include <memory>
#include <random>
#include <string>

namespace fs = std::filesystem;

// Başarısızlıkta libgit2'nin kendi açıklaması görünsün; yalnız dönüş kodu teşhis için yetersiz. std::string'e
// sarılır: doctest const olmayan char*'ı metin değil bool olarak yazar.
#define REQUIRE_GIT(expr) REQUIRE_MESSAGE((expr) >= 0, std::string(git_error_last()->message))

namespace {

// REQUIRE yarıda kesse de tutamaçlar kapanır; Windows'ta açık tutamaç geçici dizinin silinmesini engeller.
template <auto Free> struct GitFree {
    template <class T> void operator()(T* ptr) const { Free(ptr); }
};
using Repository = std::unique_ptr<git_repository, GitFree<git_repository_free>>;
using Index = std::unique_ptr<git_index, GitFree<git_index_free>>;
using Tree = std::unique_ptr<git_tree, GitFree<git_tree_free>>;
using Signature = std::unique_ptr<git_signature, GitFree<git_signature_free>>;
using Remote = std::unique_ptr<git_remote, GitFree<git_remote_free>>;

class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() / ("noteapp-test-" + std::to_string(std::random_device{}()))) {
        fs::create_directories(path_);
    }
    ~TempDir() {
        // libgit2 nesne dosyalarını salt-okunur yazar ve Windows salt-okunur dosyayı silmez; önce yazılabilir
        // yapılır.
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(path_, ec)) {
            fs::permissions(entry.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        }
        fs::remove_all(path_, ec);
        if (ec) {
            MESSAGE("geçici dizin silinemedi: " << path_.string() << ": " << ec.message());
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// libgit2 Windows'ta da UTF-8 yol bekler; path::string()'in dar kodlaması platforma göre değişir.
std::string git_path(const fs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

// İkili kip: metin kipinde MinGW \n'yi \r\n'ye çevirir ve içerik karşılaştırması bozulur.
void write_file(const fs::path& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Kullanıcının git ayarları (ör. Git for Windows'un core.autocrlf=true'su) testi etkilemesin diye tüm config
// arama yolları ayar dosyası olmayan bir dizine yönlendirilir. Arama yolu süreç geneldir ve GitRuntime onu
// yalnız bir kez yazar: eski yol okunur, yıkımda geri yazılır; sonraki testler GitRuntime'ın yalıtımını
// görür.
class IsolatedGitConfig {
public:
    explicit IsolatedGitConfig(const fs::path& empty_dir) {
        for (std::size_t i = 0; i < kLevels.size(); ++i) {
            git_buf old = GIT_BUF_INIT;
            REQUIRE_GIT(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, kLevels[i], &old));
            saved_[i].assign(old.ptr, old.size);
            git_buf_dispose(&old);
        }
        for (const int level : kLevels)
            REQUIRE_GIT(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, git_path(empty_dir).c_str()));
    }
    ~IsolatedGitConfig() {
        for (std::size_t i = 0; i < kLevels.size(); ++i)
            CHECK(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, kLevels[i], saved_[i].c_str()) >= 0);
    }
    IsolatedGitConfig(const IsolatedGitConfig&) = delete;
    IsolatedGitConfig& operator=(const IsolatedGitConfig&) = delete;

private:
    static constexpr std::array<int, 4> kLevels = {GIT_CONFIG_LEVEL_PROGRAMDATA, GIT_CONFIG_LEVEL_SYSTEM,
                                                   GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_GLOBAL};
    std::array<std::string, kLevels.size()> saved_;
};

// Varsayılan dal kullanıcının init.defaultBranch ayarına bırakılmaz; senkron dalı `main`.
Repository init_repository(const fs::path& path, bool bare) {
    git_repository_init_options options;
    REQUIRE_GIT(git_repository_init_options_init(&options, GIT_REPOSITORY_INIT_OPTIONS_VERSION));
    options.flags = GIT_REPOSITORY_INIT_MKPATH;
    if (bare) {
        options.flags |= GIT_REPOSITORY_INIT_BARE;
    }
    options.initial_head = "main";
    git_repository* repo = nullptr;
    REQUIRE_GIT(git_repository_init_ext(&repo, git_path(path).c_str(), &options));
    return Repository(repo);
}

}  // namespace

TEST_CASE("libgit2 commits, pushes to a local bare repo and clones it back") {
    const noteapp::sync::GitRuntime runtime;
    const TempDir root;
    const IsolatedGitConfig isolated(root.path() / "no-config");

    const fs::path work_dir = root.path() / "work";
    const fs::path bare_dir = root.path() / "remote.git";
    const fs::path clone_dir = root.path() / "clone";
    const std::string content = "# Spike\nMerhaba çğıİöşü\n";

    const Repository repo = init_repository(work_dir, false);
    write_file(work_dir / "hello.md", content);

    git_index* index_raw = nullptr;
    REQUIRE_GIT(git_repository_index(&index_raw, repo.get()));
    const Index index(index_raw);
    REQUIRE_GIT(git_index_add_bypath(index.get(), "hello.md"));
    REQUIRE_GIT(git_index_write(index.get()));
    git_oid tree_id;
    REQUIRE_GIT(git_index_write_tree(&tree_id, index.get()));
    git_tree* tree_raw = nullptr;
    REQUIRE_GIT(git_tree_lookup(&tree_raw, repo.get(), &tree_id));
    const Tree tree(tree_raw);

    // Açık imza: kullanıcının user.name/user.email ayarına ve saate bağımlılık yok.
    git_signature* signature_raw = nullptr;
    REQUIRE_GIT(git_signature_new(&signature_raw, "noteapp spike", "spike@noteapp.invalid", 1761609600, 0));
    const Signature signature(signature_raw);
    git_oid commit_id;
    REQUIRE_GIT(git_commit_create_v(&commit_id, repo.get(), "HEAD", signature.get(), signature.get(), nullptr,
                                    "Add spike note", tree.get(), 0));

    init_repository(bare_dir, true);
    git_remote* remote_raw = nullptr;
    REQUIRE_GIT(git_remote_create(&remote_raw, repo.get(), "origin", git_path(bare_dir).c_str()));
    const Remote remote(remote_raw);
    char refspec[] = "refs/heads/main:refs/heads/main";
    char* refspecs_raw[] = {refspec};
    const git_strarray refspecs{refspecs_raw, 1};
    REQUIRE_GIT(git_remote_push(remote.get(), &refspecs, nullptr));

    git_repository* clone_raw = nullptr;
    REQUIRE_GIT(git_clone(&clone_raw, git_path(bare_dir).c_str(), git_path(clone_dir).c_str(), nullptr));
    const Repository clone(clone_raw);
    git_oid clone_head;
    REQUIRE_GIT(git_reference_name_to_id(&clone_head, clone.get(), "HEAD"));
    CHECK(git_oid_equal(&clone_head, &commit_id));
    CHECK(read_file(clone_dir / "hello.md") == content);
}

// Elle (ağ + GitHub SSH anahtarı gerekir): exec SSH arka ucu sistemdeki ssh ile uzak ref'leri
// listeleyebiliyor mu. Önce bir kez `ssh -T git@github.com` ile host anahtarı onaylanmalı: aksi halde Git
// Bash ssh'ı "Host key verification failed" ile düşer, Win32-OpenSSH ise konsolda onay bekleyip asılı kalır.
// Uzak, bir repoya bağlı olmalı: libgit2 1.9.7'de exec SSH core.sshCommand için remote->repo'yu okur;
// git_remote_create_detached ile oluşan reposuz uzakta NULL'a erişip çöker (0xC0000005, denendi).
TEST_CASE("ssh exec transport lists refs of a GitHub repository (manual)" * doctest::skip()) {
    const noteapp::sync::GitRuntime runtime;
    const TempDir root;
    // Kullanıcının url.<base>.insteadOf ayarı git@ adresini sessizce HTTPS'e çevirip SSH'ı atlatmasın.
    const IsolatedGitConfig isolated(root.path() / "no-config");
    const Repository repo = init_repository(root.path() / "repo", true);

    git_remote* remote_raw = nullptr;
    REQUIRE_GIT(git_remote_create_anonymous(&remote_raw, repo.get(), "git@github.com:libgit2/libgit2.git"));
    const Remote remote(remote_raw);
    REQUIRE_GIT(git_remote_connect(remote.get(), GIT_DIRECTION_FETCH, nullptr, nullptr, nullptr));

    const git_remote_head** heads = nullptr;
    size_t count = 0;
    REQUIRE_GIT(git_remote_ls(&heads, &count, remote.get()));
    REQUIRE(count > 0);
    MESSAGE("ilk ref: " << std::string(heads[0]->name) << ", toplam " << count);
}
