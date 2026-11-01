#include "core/error.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "core/migration.hpp"
#include "core/paths.hpp"
#include "sync/lock.hpp"
#include "sync/sync_service.hpp"
#include "sync_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <git2.h>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using noteapp::core::Error;
using noteapp::sync::SyncService;
using namespace noteapp::sync_test;

namespace {

const std::string kGitignore = ".noteapp/\n.*.tmp\ngraph/*.aside-*\n";

std::optional<std::string> origin_url(const fs::path& dir) {
    const Repository repo = open_repository(dir);
    git_remote* remote = nullptr;
    if (git_remote_lookup(&remote, repo.get(), "origin") < 0)
        return std::nullopt;
    std::string url = git_remote_url(remote);
    git_remote_free(remote);
    return url;
}

// `ref`'in ağacındaki dosyalar, depo-göreli '/' ayırıcılı.
std::set<std::string> tree_files(const fs::path& repo_dir, const std::string& ref) {
    const Repository repo = open_repository(repo_dir);
    git_object* tree = nullptr;
    REQUIRE_GIT(git_revparse_single(&tree, repo.get(), (ref + "^{tree}").c_str()));
    std::set<std::string> files;
    REQUIRE_GIT(git_tree_walk(
        reinterpret_cast<git_tree*>(tree), GIT_TREEWALK_PRE,
        [](const char* root, const git_tree_entry* entry, void* payload) {
            if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB)
                static_cast<std::set<std::string>*>(payload)->insert(std::string(root) +
                                                                     git_tree_entry_name(entry));
            return 0;
        },
        &files));
    git_object_free(tree);
    return files;
}

std::set<std::string> entries(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir))
        names.insert(utf8(entry.path().filename()));
    return names;
}

// Uzak depoya, enable'dan bağımsız olarak (eski usul klon + senkron) bir not yollar.
void seed_remote(const SyncFixture& fx) {
    const Device seed(fx, "tohum");
    seed.put("klasör/ilk not.md", "# İlk\nçğıöşü\n");
    const auto synced = seed.sync();
    REQUIRE_MESSAGE(synced.has_value(), describe(synced));
}

std::expected<void, Error> enable(const fs::path& notes_dir, const std::string& remote,
                                  bool replace_origin = false) {
    return SyncService(SyncFixture::config_for(notes_dir)).enable(remote, replace_origin);
}

}  // namespace

TEST_CASE("enable row 1: a repository whose origin is the remote is left as it is") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A notları");
    write_file(a / "not.md", "x\n");
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK_FALSE(fs::exists(a / ".gitignore"));
    CHECK(commit_count(a) == 0);
    CHECK(origin_url(a) == utf8(fx.remote()));
    CHECK_FALSE(ref_id(fx.remote(), "refs/heads/main").has_value());
}

TEST_CASE("enable row 2: another or no origin is sync.remote_mismatch until the caller confirms") {
    const SyncFixture fx;
    const fs::path other = fx.root() / u8"başka uzak.git";
    init_repository(other, true);

    const fs::path a = fx.make_clone(u8"A notları");
    auto result = enable(a, utf8(other));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.remote_mismatch");
    CHECK(result.error().args ==
          noteapp::core::Args{{"current", utf8(fx.remote())}, {"remote", utf8(other)}});
    CHECK(origin_url(a) == utf8(fx.remote()));
    result = enable(a, utf8(other), true);
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(origin_url(a) == utf8(other));

    const fs::path b = fx.root() / u8"B notları";
    init_repository(b, false);
    result = enable(b, utf8(other));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.remote_mismatch");
    CHECK_FALSE(origin_url(b).has_value());
    result = enable(b, utf8(other), true);
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(origin_url(b) == utf8(other));
}

TEST_CASE("sync.remote_mismatch never echoes the credentials of a hand-written origin") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A notları");
    {
        const Repository repo = open_repository(a);
        REQUIRE_GIT(
            git_remote_set_url(repo.get(), "origin", "https://kisi:gizli@github.com/kisi/notlar.git"));
    }
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.remote_mismatch");
    CHECK(result.error().args == noteapp::core::Args{{"current", "https://github.com/kisi/notlar.git"},
                                                     {"remote", utf8(fx.remote())}});
}

TEST_CASE("sync_now checks the remote before touching anything: unknown SSH host, credentials in the URL") {
    const SyncFixture fx;
    const EnvGuard path("PATH", fs::path(NOTEAPP_FAKE_SSH_DIR));
    const fs::path a = fx.make_clone(u8"A notları");
    write_file(a / "not.md", "x\n");
    struct Row {
        const char* url;
        const char* key;
    };
    for (const Row row :
         {Row{"git@unknown.example:kisi/notlar.git", "sync.ssh_host_unknown"},
          Row{"https://kisi:gizli@github.com/kisi/notlar.git", "sync.remote_has_credentials"}}) {
        CAPTURE(row.url);
        {
            const Repository repo = open_repository(a);
            REQUIRE_GIT(git_remote_set_url(repo.get(), "origin", row.url));
        }
        const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == row.key);
        CHECK(commit_count(a) == 0);  // commit dahil hiçbir şeye dokunulmadı
    }
}

TEST_CASE("after replacing origin with an empty remote the next sync pushes the whole history there") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A notları");
    put_note("not.md", "x\n");
    SyncService sync_a(SyncFixture::config_for(a));
    REQUIRE(sync_a.sync_now().has_value());
    const fs::path other = fx.root() / u8"yeni uzak.git";
    init_repository(other, true);

    const auto replaced = sync_a.enable(utf8(other), true);
    REQUIRE_MESSAGE(replaced.has_value(), describe(replaced));
    const auto synced = sync_a.sync_now();
    REQUIRE_MESSAGE(synced.has_value(), describe(synced));
    CHECK(synced->pushed);
    CHECK(ref_id(other, "refs/heads/main") == ref_id(a, "refs/heads/main"));
}

TEST_CASE(
    "enable row 3: an empty notes folder (hidden entries allowed, or none at all) gets the remote's notes") {
    const SyncFixture fx;
    seed_remote(fx);
    const fs::path b = fx.root() / u8"B notları";
    write_file(b / ".noteapp" / "durum", "gizli\n");  // gizli girdi klasörü "dolu" yapmaz
    const fs::path c = fx.root() / u8"henüz yok" / u8"C notları";

    for (const fs::path& dir : {b, c}) {
        CAPTURE(utf8(dir));
        const auto result = enable(dir, utf8(fx.remote()));
        REQUIRE_MESSAGE(result.has_value(), describe(result));
        CHECK(fs::is_directory(dir / "graph"));
        CHECK(ref_id(dir, "HEAD") == ref_id(fx.remote(), "refs/heads/main"));
        CHECK(origin_url(dir) == utf8(fx.remote()));
        CHECK(read_file(dir / ".gitignore") == kGitignore);
    }
    CHECK(read_file(b / ".noteapp" / "durum") == "gizli\n");
}

// Uzakta kurulumun yazdığı .gitignore var; klasörde yalnız gizli, izlenmeyen bir .gitignore kalmış (ör. eski
// bir kurulumdan): klasör boş sayılır ama SAFE checkout o dosyanın üzerine yazamazdı.
TEST_CASE("enable row 3 with a leftover .gitignore: the clone keeps the remote's and adds the local lines") {
    const SyncFixture fx;
    const Device a(fx, "A notları", false);
    a.put("not.md", "A\n");
    REQUIRE(a.enable(utf8(fx.remote())).has_value());  // uzak: .gitignore + .gitattributes + günlük
    const fs::path b = fx.root() / u8"B notları";
    write_file(b / ".gitignore", "*.bak\r\n.noteapp/\n");

    const auto result = enable(b, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(fs::is_directory(b / "graph"));
    CHECK(read_file(b / ".gitignore") == kGitignore + "*.bak\n");
    CHECK(ref_id(b, "HEAD") == ref_id(fx.remote(), "refs/heads/main"));
}

TEST_CASE("a leftover hidden file the clone would overwrite fails the enable; it and .gitignore stay as they "
          "were") {
    const SyncFixture fx;
    commit_entries(fx.remote(), {{".ayar", "uzak ayar\n"}});
    const fs::path b = fx.root() / u8"B notları";
    write_file(b / ".ayar", "yerel ayar\n");
    write_file(b / ".gitignore", "*.bak\n");

    const auto result = enable(b, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(read_file(b / ".ayar") == "yerel ayar\n");
    CHECK(read_file(b / ".gitignore") == "*.bak\n");
    CHECK_FALSE(fs::exists(b / ".git"));
}

TEST_CASE("enable row 3, empty remote: init, a first commit with only the carrier setup, push") {
    const SyncFixture fx;
    const fs::path b = fx.root() / u8"B notları";
    const auto result = enable(b, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(commit_count(b) == 1);
    CHECK(tree_files(fx.remote(), "refs/heads/main") ==
          std::set<std::string>{".gitattributes", ".gitignore"});
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(b, "HEAD"));
    CHECK(read_file(b / ".gitignore") == kGitignore);
}

TEST_CASE("enable row 4: notes and an empty remote: init, carriers, this device's log, push") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    put_note("ilk not.md", "bir\n");
    put_note("klasör/ikinci.md", "iki\r\n");
    write_file(a / ".noteapp" / "index", "dahili\n");  // .gitignore: .noteapp/
    write_file(a / u8".ilk not.md.tmp", "yarım\n");    // .gitignore: .*.tmp

    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(commit_count(a) == 1);
    const auto files = tree_files(a, "HEAD");
    CHECK(files.size() == 3);
    CHECK(files.contains(".gitignore"));
    CHECK(files.contains(".gitattributes"));
    CHECK(std::ranges::count_if(files, [](const std::string& f) { return f.starts_with("graph/"); }) == 1);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(a, "HEAD"));
    CHECK(read_file(a / ".gitignore") == kGitignore);
}

TEST_CASE("enable with old files waiting for migration sets up the repository but commits no plain file") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / "eski.md", "eski\n");
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(tree_files(a, "HEAD") == std::set<std::string>{".gitattributes", ".gitignore"});
    CHECK(read_file(a / "eski.md") == "eski\n");
    CHECK(SyncService(SyncFixture::config_for(a)).sync_now().error().code ==
          noteapp::core::Error::Code::MigrationRequired);
}

TEST_CASE("enable row 5: notes on both sides is sync.both_non_empty and nothing changes") {
    const SyncFixture fx;
    seed_remote(fx);
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");
    const fs::path c = fx.root() / u8"C notları";
    write_file(c / "yerel.md", "yerel\n");

    const auto result = enable(c, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.both_non_empty");
    CHECK(entries(c) == std::set<std::string>{"yerel.md"});  // .git ve .gitignore kalmadı
    CHECK(read_file(c / "yerel.md") == "yerel\n");
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
}

TEST_CASE("a non-hidden empty folder makes the notes folder non-empty") {
    const SyncFixture fx;
    seed_remote(fx);
    const fs::path c = fx.root() / u8"C notları";
    fs::create_directories(c / u8"boş klasör");
    const auto result = enable(c, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.both_non_empty");
    CHECK(entries(c) == std::set<std::string>{"boş klasör"});
}

TEST_CASE("an existing .gitignore keeps its content and only gets the missing lines") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    put_note("not.md", "x\n");
    write_file(a / ".gitignore", "*.bak\r\n.noteapp/");  // CRLF, son satırda satır sonu yok
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(read_file(a / ".gitignore") == "*.bak\r\n.noteapp/\n.*.tmp\ngraph/*.aside-*\n");
    CHECK(tree_files(a, "HEAD").contains(".gitattributes"));
    CHECK_FALSE(tree_files(a, "HEAD").contains("not.md"));
}

TEST_CASE("a remote without sync.branch is sync.remote_branch_missing and nothing is created") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / "not.md", "x\n");
    noteapp::core::Config master = SyncFixture::config_for(a);
    master.sync.branch = "master";
    const auto seeded = SyncService(master).enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(seeded.has_value(), describe(seeded));
    REQUIRE(ref_id(fx.remote(), "refs/heads/master").has_value());

    const fs::path b = fx.root() / u8"B notları";
    fs::create_directories(b);
    const auto result = enable(b, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.remote_branch_missing");
    CHECK(result.error().args == noteapp::core::Args{{"branch", "main"}});
    CHECK(entries(b).empty());
}

TEST_CASE("a failed enable leaves no .git and no .gitignore of its own; the notes stay") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / u8"not ç.md", "içerik\n");

    SUBCASE("the local remote does not exist (refused before anything is created)") {
        const auto result = enable(a, utf8(fx.root() / u8"olmayan uzak.git"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.remote_invalid");
    }
    SUBCASE("the remote is not a repository (fails after the repository was created)") {
        fs::create_directories(fx.root() / u8"boş klasör.git");
        const auto result = enable(a, utf8(fx.root() / u8"boş klasör.git"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.git_failed");
    }
    SUBCASE("the first push is rejected") {
        // Uzak dalın kilidi tutulursa push ref bazında reddedilir (commit zaten yapılmıştır).
        write_file(fx.remote() / "refs" / "heads" / "main.lock", "");
        const auto result = enable(a, utf8(fx.remote()));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.push_rejected");
        CHECK_FALSE(ref_id(fx.remote(), "refs/heads/main").has_value());
    }
    SUBCASE("the SSH host key is unknown (checked before anything is created)") {
        const EnvGuard path("PATH", fs::path(NOTEAPP_FAKE_SSH_DIR));
        const auto result = enable(a, "git@unknown.example:kisi/notlar.git");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.ssh_host_unknown");
    }
    SUBCASE("ssh-keygen is not available") {
        const TempDir empty;
        const EnvGuard path("PATH", empty.path());
        const auto result = enable(a, "ssh://git@known.example/notlar.git");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.ssh_unavailable");
    }
    CHECK(entries(a) == std::set<std::string>{"not ç.md"});
    CHECK(read_file(a / u8"not ç.md") == "içerik\n");
}

TEST_CASE("an https remote with credentials in it is refused, nothing is created and it is not echoed") {
    const SyncFixture fx;
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / "not.md", "x\n");
    for (const std::string url : {"https://kisi:gizli-parola@github.com/kisi/notlar.git",
                                  "https://gizli-parola@github.com/kisi/notlar.git"}) {
        const auto result = enable(a, url);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.remote_has_credentials");
        CHECK(noteapp::core::format_error(result.error()).find("gizli-parola") == std::string::npos);
        CHECK(result.error().args.empty());
    }
    for (const std::string url : {"", "ssh://git@-oProxyCommand=calc/x.git", "-oProxyCommand=calc:x.git"}) {
        CAPTURE(url);
        const auto result = enable(a, url);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.remote_invalid");
    }
    CHECK(entries(a) == std::set<std::string>{"not.md"});
}

TEST_CASE("enable holds the sync lock: while a sync runs it is sync.busy and creates nothing") {
    const SyncFixture fx;
    const auto lock =
        noteapp::sync::SyncLock::acquire(noteapp::core::state_dir() / "sync.lock", std::chrono::minutes(10));
    REQUIRE(lock.has_value());
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / "not.md", "x\n");
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.busy");
    CHECK(entries(a) == std::set<std::string>{"not.md"});
}

// Uzak depodaki bir bağ (120000) notes_dir dışını gösterebilir: gerçek bağ olarak çıkarılsaydı
// `.gitignore -> ../gizli.txt`'nin hedefi okunup yeniden commit'lenirdi (sızıntı).
namespace {

bool is_plain_file(const fs::path& path) {
    const auto stat = noteapp::core::stat_file(path);
    return stat && stat->regular && !stat->symlink;
}

std::string secret_outside(const SyncFixture& fx) {
    const std::string secret = "GIZLI-ANAHTAR-" + std::to_string(std::random_device{}());
    write_file(fx.root() / "gizli.txt", secret);
    return secret;
}

}  // namespace

TEST_CASE("remote symlinks are checked out as plain files holding the link text; nothing outside is read") {
    const SyncFixture fx;
    const std::string secret = secret_outside(fx);
    commit_entries(fx.remote(), {{"not.md", "not\n"},
                                 {".gitignore", "../gizli.txt", GIT_FILEMODE_LINK},
                                 {"bag.md", "../gizli.txt", GIT_FILEMODE_LINK}});
    const fs::path b = fx.root() / u8"B notları";
    SyncService sync_b(SyncFixture::config_for(b));
    const auto enabled = sync_b.enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(enabled.has_value(), describe(enabled));
    // Uzakta düz dosya var (bag.md, not.md): önce geçiş; bağ metni not olur, hedefi okunmaz.
    const auto migrated = noteapp::core::migrate_legacy_notes(b, noteapp::core::data_dir());
    REQUIRE_MESSAGE(migrated.has_value(), describe(migrated));
    put_note("yeni.md", "yeni\n");
    const auto synced = sync_b.sync_now();
    REQUIRE_MESSAGE(synced.has_value(), describe(synced));

    CHECK(config_bool(b, "core.symlinks") == false);
    for (const char* name : {".gitignore", "bag.md"}) {
        CAPTURE(name);
        CHECK(is_plain_file(b / name));
        CHECK(read_file(b / name).starts_with("../gizli.txt"));
    }
    CHECK(read_file(fx.root() / "gizli.txt") == secret);
    for (const fs::path& repo : {b, fx.remote()}) {
        CAPTURE(utf8(repo));
        CHECK_FALSE(objects_contain(repo, secret));
    }
}

TEST_CASE("a repository opened for sync gets core.symlinks=false even if it was created with links on") {
    const SyncFixture fx;
    secret_outside(fx);
    const fs::path a = fx.make_clone(u8"A notları");
    {
        const Repository repo = open_repository(a);
        git_config* config = nullptr;
        REQUIRE_GIT(git_repository_config(&config, repo.get()));
        REQUIRE_GIT(git_config_set_bool(config, "core.symlinks", 1));  // ör. Linux'ta git ile kurulmuş
        git_config_free(config);
    }
    put_note("not.md", "x\n");
    const auto first = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_MESSAGE(first.has_value(), describe(first));
    commit_entries(fx.remote(), {{"bag.md", "../gizli.txt", GIT_FILEMODE_LINK}});

    const auto pulled = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_MESSAGE(pulled.has_value(), describe(pulled));
    CHECK(config_bool(a, "core.symlinks") == false);
    CHECK(is_plain_file(a / "bag.md"));
    CHECK(read_file(a / "bag.md") == "../gizli.txt");
}

TEST_CASE("a .gitignore that is a link is neither read nor replaced") {
    const SyncFixture fx;
    const std::string secret = secret_outside(fx);
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / "not.md", "x\n");
    std::error_code ec;
    fs::create_symlink(fx.root() / "gizli.txt", a / ".gitignore", ec);
    if (ec) {  // Windows'ta geliştirici kipi kapalıyken (MinGW libstdc++ de desteklemeyebilir)
        MESSAGE("symbolic links cannot be created here, skipped: " << ec.message());
        return;
    }
    const auto result = enable(a, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.gitignore_not_regular");
    CHECK(read_file(fx.root() / "gizli.txt") == secret);
    CHECK_FALSE(is_plain_file(a / ".gitignore"));  // bağ yerinde, üzerine yazılmadı
    CHECK_FALSE(fs::exists(a / ".git"));
}

// Bu işletim sisteminde yazılamayan uzak yol: genel git hatası yerine ne yapılacağını söyleyen hata. Yalnız
// Windows: aux.md Linux'ta geçerli bir addır.
#ifdef _WIN32
TEST_CASE("a clone of a remote path invalid on this OS is sync.remote_invalid_paths and names the path") {
    const SyncFixture fx;
    commit_entries(fx.remote(), {{"aux.md", "aygıt adı\n"}, {"not.md", "not\n"}});
    const fs::path b = fx.root() / u8"B notları";
    fs::create_directories(b);
    const auto result = enable(b, utf8(fx.remote()));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.remote_invalid_paths");
    CHECK(result.error().args == noteapp::core::Args{{"path", "aux.md"}});
    CHECK(entries(b).empty());
}

TEST_CASE("a sync pulling a path invalid on this OS is sync.remote_invalid_paths; nothing local is lost") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A notları");
    put_note("not.md", "x\n");
    SyncService sync_a(SyncFixture::config_for(a));
    REQUIRE(sync_a.sync_now().has_value());
    commit_entries(fx.remote(), {{"aux.md", "aygıt adı\n"}});

    SUBCASE("fast-forward") {
        const auto head = ref_id(a, "HEAD");
        const auto result = sync_a.sync_now();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.remote_invalid_paths");
        CHECK(ref_id(a, "HEAD") == head);
    }
    SUBCASE("merge") {
        put_note("yerel.md", "yerel\n");
        const auto result = sync_a.sync_now();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.remote_invalid_paths");
        CHECK(note_body_here("yerel.md") == "yerel\n");
        CHECK(head_commit(a).parents == 1);
    }
    CHECK(git_repository_state(open_repository(a).get()) == GIT_REPOSITORY_STATE_NONE);
    CHECK(note_body_here("not.md") == "x\n");
    CHECK_FALSE(fs::exists(a / "aux.md"));
}

// Checkout burada hata vermez: iki not tek dosyaya düşer, biri sessizce kaybolur ve sonraki commit onu ezer.
TEST_CASE("two remote names that differ only in case are refused on a case-insensitive disk") {
    struct Row {
        const char* upper;
        const char* lower;
    };
    for (const Row row : {Row{"Not.md", "not.md"}, Row{"Çalışma.md", "çalışma.md"}}) {
        CAPTURE(row.lower);
        const SyncFixture fx;
        const fs::path a = fx.make_clone(u8"A notları");
        put_note("yerel.md", "yerel\n");
        SyncService sync_a(SyncFixture::config_for(a));
        REQUIRE(sync_a.sync_now().has_value());
        commit_entries(fx.remote(), {{row.upper, "büyük\n"}, {row.lower, "küçük\n"}});

        const fs::path b = fx.root() / u8"B notları";
        fs::create_directories(b);
        const auto cloned = enable(b, utf8(fx.remote()));
        REQUIRE_FALSE(cloned.has_value());
        CHECK(cloned.error().message_key == "sync.remote_invalid_paths");
        CHECK(cloned.error().args == noteapp::core::Args{{"path", row.lower}});
        CHECK(entries(b).empty());

        const auto head = ref_id(a, "HEAD");
        const auto pulled = sync_a.sync_now();
        REQUIRE_FALSE(pulled.has_value());
        CHECK(pulled.error().message_key == "sync.remote_invalid_paths");
        CHECK(ref_id(a, "HEAD") == head);
        CHECK(entries(a) == std::set<std::string>{".git", ".gitattributes", ".gitignore", "graph"});
    }
}
#endif
