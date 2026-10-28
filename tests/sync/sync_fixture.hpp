#pragma once

// sync testlerinin ortak dünyası: izole HOME/XDG/NOTEAPP_HOME, bare uzak depo R ve klonlar, libgit2
// yardımcıları. Fonksiyonlar `inline` ve adlı ad alanında: bir test dosyası kullanmadığında -Werror'lu
// "kullanılmayan fonksiyon" uyarısı çıkmasın.
#include "core/config.hpp"
#include "core/error.hpp"
#include "core/process.hpp"
#include "sync/git_runtime.hpp"

#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <fstream>
#include <git2.h>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <stdlib.h>  // _wputenv_s
#endif

// Başarısızlıkta libgit2'nin kendi açıklaması görünsün; std::string'e sarılır (doctest char*'ı bool yazar).
#define REQUIRE_GIT(expr) REQUIRE_MESSAGE((expr) >= 0, std::string(git_error_last()->message))

namespace noteapp::sync_test {

namespace fs = std::filesystem;
using core::Error;

// Hata anahtarı ve argümanları; başarısız REQUIRE'da neyin ters gittiği görünsün.
template <class T> std::string describe(const std::expected<T, Error>& result) {
    if (result)
        return "ok";
    std::string text = result.error().message_key;
    for (const auto& [name, value] : result.error().args)
        text += " " + name + "=" + value;
    return text;
}

template <auto Free> struct GitFree {
    template <class T> void operator()(T* ptr) const { Free(ptr); }
};
using Repository = std::unique_ptr<git_repository, GitFree<git_repository_free>>;
using Commit = std::unique_ptr<git_commit, GitFree<git_commit_free>>;
using Revwalk = std::unique_ptr<git_revwalk, GitFree<git_revwalk_free>>;

inline std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// İkili kip: metin kipinde MinGW \n'yi \r\n'ye çevirir; satır sonu testleri bayt bayt karşılaştırır.
inline void write_file(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Ortam değişkenini ayarlar (nullopt: siler), yıkıcıda eskisine döner. Windows'ta geniş sürüm: ANSI
// _putenv_s Türkçe karakterli yolu bozar.
class EnvGuard {
public:
    EnvGuard(std::string name, const std::optional<fs::path>& value)
        : name_(std::move(name)), old_(core::get_env_utf8(name_)) {
        set(value);
    }
    ~EnvGuard() {
        set(old_ ? std::optional<fs::path>(fs::path(std::u8string(old_->begin(), old_->end())))
                 : std::nullopt);
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    void set(const std::optional<fs::path>& value) const {
#ifdef _WIN32
        const std::wstring wide_name(name_.begin(), name_.end());
        _wputenv_s(wide_name.c_str(), value ? value->wstring().c_str() : L"");
#else
        if (value)
            setenv(name_.c_str(), value->c_str(), 1);
        else
            unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    std::optional<std::string> old_;
};

// Türkçe karakterli geçici kök: libgit2'ye giden yolların UTF-8 dönüşümü de sınanır.
class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() /
                fs::path(u8"noteapp-sync-İşğü-" + to_u8(std::to_string(std::random_device{}())))) {
        fs::create_directories(path_);
    }
    ~TempDir() {
        // libgit2 nesne dosyalarını salt-okunur yazar; Windows salt-okunur dosyayı silmez.
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(path_, ec))
            fs::permissions(entry.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    static std::u8string to_u8(const std::string& text) { return {text.begin(), text.end()}; }
    fs::path path_;
};

inline Repository open_repository(const fs::path& dir) {
    git_repository* raw = nullptr;
    REQUIRE_GIT(git_repository_open(&raw, utf8(dir).c_str()));
    return Repository(raw);
}

// Varsayılan dal kullanıcının init.defaultBranch ayarına bırakılmaz; senkron dalı `main`.
inline Repository init_repository(const fs::path& dir, bool bare) {
    git_repository_init_options options;
    REQUIRE_GIT(git_repository_init_options_init(&options, GIT_REPOSITORY_INIT_OPTIONS_VERSION));
    options.flags = GIT_REPOSITORY_INIT_MKPATH;
    if (bare)
        options.flags |= GIT_REPOSITORY_INIT_BARE;
    options.initial_head = "main";
    git_repository* raw = nullptr;
    REQUIRE_GIT(git_repository_init_ext(&raw, utf8(dir).c_str(), &options));
    return Repository(raw);
}

inline std::optional<std::string> ref_id(const fs::path& repo_dir, const char* name) {
    const Repository repo = open_repository(repo_dir);
    git_oid id;
    if (git_reference_name_to_id(&id, repo.get(), name) < 0)
        return std::nullopt;
    return std::string(git_oid_tostr_s(&id));
}

inline int commit_count(const fs::path& repo_dir) {
    const Repository repo = open_repository(repo_dir);
    git_revwalk* raw = nullptr;
    REQUIRE_GIT(git_revwalk_new(&raw, repo.get()));
    const Revwalk walk(raw);
    if (git_revwalk_push_head(walk.get()) < 0)
        return 0;  // doğmamış dal
    int count = 0;
    git_oid id;
    while (git_revwalk_next(&id, walk.get()) == 0)
        ++count;
    return count;
}

struct CommitInfo {
    std::string message;
    std::string author_name;
    std::string author_email;
    unsigned parents;
};

inline CommitInfo head_commit(const fs::path& repo_dir) {
    const Repository repo = open_repository(repo_dir);
    git_oid id;
    REQUIRE_GIT(git_reference_name_to_id(&id, repo.get(), "HEAD"));
    git_commit* raw = nullptr;
    REQUIRE_GIT(git_commit_lookup(&raw, repo.get(), &id));
    const Commit commit(raw);
    const git_signature* author = git_commit_author(commit.get());
    return {git_commit_message(commit.get()), author->name, author->email,
            git_commit_parentcount(commit.get())};
}

// Her test kendi dünyasında: HOME/USERPROFILE/XDG/NOTEAPP_HOME geçici dizine yönlenir (kilit ve durum
// dosyası oraya düşer), global git config bilerek düşmanca (autocrlf=true), bare uzak depo R hazır.
class SyncFixture {
public:
    SyncFixture() {
        write_file(home_ / ".gitconfig", "[core]\n\tautocrlf = true\n");
        init_repository(remote_, true);
    }

    const fs::path& root() const { return root_.path(); }
    const fs::path& remote() const { return remote_; }

    // `origin` = R olan, commit'i olmayan (doğmamış `main`) not deposu.
    fs::path make_clone(std::u8string_view name) const {
        const fs::path dir = root_.path() / name;
        const Repository repo = init_repository(dir, false);
        git_remote* remote = nullptr;
        REQUIRE_GIT(git_remote_create(&remote, repo.get(), "origin", utf8(remote_).c_str()));
        git_remote_free(remote);
        return dir;
    }

    static core::Config config_for(const fs::path& notes_dir) {
        core::Config config = core::parse_config("").config;
        config.notes_dir = notes_dir;
        config.sync.branch = "main";
        config.sync.author_name = "Test Yazar";
        config.sync.author_email = "test@noteapp.invalid";
        return config;
    }

private:
    TempDir root_;
    fs::path home_ = root_.path() / "home";
    EnvGuard env_home_{"HOME", home_};
    EnvGuard env_profile_{"USERPROFILE", home_};
    EnvGuard env_xdg_{"XDG_CONFIG_HOME", root_.path() / "xdg"};
    EnvGuard env_app_{"NOTEAPP_HOME", root_.path() / "app"};
    // Ortamdan sonra kurulur: libgit2 config arama yollarını init sırasında tahmin eder.
    sync::GitRuntime runtime_;
    fs::path remote_ = root_.path() / u8"uzak depo.git";
};

}  // namespace noteapp::sync_test
