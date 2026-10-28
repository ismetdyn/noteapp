// note sync … (T27) gerçek `note` ikilisiyle, yerel bare uzak depoya karşı (ağ yok).
#include "support/cli_sandbox.hpp"
#include "sync/auth.hpp"
#include "sync/git_runtime.hpp"
#include "sync/lock.hpp"
#include "sync/sync_fixture.hpp"  // bare depo ve commit sayımı (libgit2 yardımcıları)

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>

using namespace noteapp::test;
using namespace std::chrono_literals;

namespace {

// Boş, yerel bare uzak depo.
class BareRemote {
public:
    BareRemote() { noteapp::sync_test::init_repository(path(), true); }
    fs::path path() const { return dir_.path() / "remote.git"; }
    std::string arg() const { return utf8(path()); }
    int commits() const { return noteapp::sync_test::commit_count(path()); }

private:
    noteapp::sync::GitRuntime runtime_;
    TempDir dir_;
};

bool eventually(const std::function<bool()>& condition, std::chrono::seconds limit = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return condition();
}

fs::path state_dir(const Sandbox& box) {
    return box.notes().parent_path() / "state";
}

fs::path config_toml(const Sandbox& box) {
    return box.notes().parent_path() / "config" / "config.toml";
}

void append_config(const Sandbox& box, const std::string& text) {
    std::ofstream(config_toml(box), std::ios::binary | std::ios::app) << text;
}

// Arka plan senkronu bitti: kilit bırakıldı. Sonra kısa bir pay: süreç log'u kapatıp çıksın (geçici dizin
// silinebilsin).
void wait_background_done(const Sandbox& box) {
    CHECK(eventually([&] { return !fs::exists(state_dir(box) / "sync.lock"); }));
    std::this_thread::sleep_for(300ms);
}

}  // namespace

TEST_CASE("sync init sets up an empty remote and turns sync on; sync off/on toggle it") {
    const Sandbox box("en");
    const BareRemote remote;
    const auto init = box.run({"sync", "init", remote.arg()});
    CHECK_MESSAGE(init.exit_code == 0, init.err);
    CHECK(remote.commits() == 1);  // yalnız .gitignore'lu ilk commit
    CHECK(box.run({"config", "get", "sync.enabled"}).out == "true\n");
    CHECK(box.run({"config", "get", "sync.remote"}).out == remote.arg() + "\n");

    CHECK(box.run({"sync", "off"}).exit_code == 0);
    CHECK(box.run({"config", "get", "sync.enabled"}).out == "false\n");
    const auto on = box.run({"sync", "on"});
    CHECK_MESSAGE(on.exit_code == 0, on.err);
    CHECK(box.run({"config", "get", "sync.enabled"}).out == "true\n");
}

TEST_CASE("acceptance: quick add, mv and rm return at once; a detached background sync pushes each") {
    const Sandbox box("en");
    const BareRemote remote;
    REQUIRE(box.run({"sync", "init", remote.arg()}).exit_code == 0);

    const auto added = box.run({"merhaba senkron"});
    CHECK(added.exit_code == 0);
    CHECK(added.out == "Saved: merhaba-senkron.md (md, suggested)\n");  // senkronun çıktısı yok
    CHECK(added.err.empty());
    CHECK(eventually([&] { return remote.commits() == 2; }));
    wait_background_done(box);

    CHECK(box.run({"mv", "merhaba-senkron.md", "selam.md"}).exit_code == 0);
    CHECK(eventually([&] { return remote.commits() == 3; }));
    wait_background_done(box);

    CHECK(box.run({"rm", "selam.md", "-y"}).exit_code == 0);
    CHECK(eventually([&] { return remote.commits() == 4; }));
    wait_background_done(box);

    const auto status = box.run({"sync", "status"});
    CHECK(status.exit_code == 0);
    CHECK(status.out.find("Sync: on") != std::string::npos);
    CHECK(status.out.find(remote.arg()) != std::string::npos);
    CHECK(status.out.find("Last successful sync: never") == std::string::npos);
    CHECK(status.out.find("Last error") == std::string::npos);
}

TEST_CASE("--no-sync and sync off start no background sync") {
    const Sandbox box("en");
    const BareRemote remote;
    REQUIRE(box.run({"sync", "init", remote.arg()}).exit_code == 0);
    CHECK(box.run({"--no-sync", "birinci"}).exit_code == 0);
    CHECK(box.run({"mv", "birinci.md", "ikinci.md", "--no-sync"}).exit_code == 0);
    REQUIRE(box.run({"sync", "off"}).exit_code == 0);
    CHECK(box.run({"ucuncu"}).exit_code == 0);
    std::this_thread::sleep_for(1500ms);
    CHECK(remote.commits() == 1);
    CHECK_FALSE(fs::exists(state_dir(box) / "sync-status.toml"));
}

TEST_CASE("note sync runs in the foreground and prints a summary") {
    const Sandbox box("en");
    const BareRemote remote;
    REQUIRE(box.run({"sync", "init", remote.arg()}).exit_code == 0);
    std::ofstream(box.notes() / "elle.md", std::ios::binary) << "elle yazıldı\n";

    const auto result = box.run({"sync"});
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(result.out.find("1 committed") != std::string::npos);
    CHECK(result.out.find(", pushed,") != std::string::npos);
    CHECK(remote.commits() == 2);
}

TEST_CASE("a sync error exits 4; --background prints nothing, on success or error") {
    const Sandbox box("en");
    const auto failed = box.run({"sync"});  // notes_dir bir git deposu değil
    CHECK(failed.exit_code == 4);
    CHECK(failed.err.find("Sync is not set up") != std::string::npos);

    append_config(box, "[sync]\nenabled = true\n");
    const auto background = box.run({"sync", "--background"});
    CHECK(background.exit_code == 4);
    CHECK(background.out.empty());
    CHECK(background.err.empty());
    CHECK(box.run({"sync", "status"}).out.find("Sync is not set up") != std::string::npos);

    const BareRemote remote;
    REQUIRE(box.run({"sync", "init", remote.arg()}).exit_code == 0);
    std::ofstream(box.notes() / "not.md", std::ios::binary) << "x\n";
    const auto ok = box.run({"sync", "--background"});
    CHECK(ok.exit_code == 0);
    CHECK(ok.out.empty());
    CHECK(ok.err.empty());
    CHECK(remote.commits() == 2);
}

TEST_CASE("--background waits for a sync that holds the lock instead of dropping its changes") {
    const Sandbox box("en");
    const BareRemote remote;
    REQUIRE(box.run({"sync", "init", remote.arg()}).exit_code == 0);
    std::ofstream(box.notes() / "sonra.md", std::ios::binary) << "kilit bırakılınca\n";

    // Bu süreç, notu görmemiş başka bir senkron gibi kilidi tutar ve biraz sonra bırakır.
    std::optional held(
        noteapp::sync::SyncLock::acquire(state_dir(box) / "sync.lock", std::chrono::minutes(10)));
    REQUIRE(held->has_value());
    std::thread release([&] {
        std::this_thread::sleep_for(1500ms);
        held.reset();
    });
    const auto background = box.run({"sync", "--background"});
    release.join();
    CHECK(background.exit_code == 0);
    CHECK(background.elapsed >= 1400ms);
    CHECK(remote.commits() == 2);
}

TEST_CASE("sync init against another origin: refused without a terminal, --replace replaces it") {
    const Sandbox box("en");
    const BareRemote first;
    const BareRemote second;
    REQUIRE(box.run({"sync", "init", first.arg()}).exit_code == 0);

    const auto refused = box.run({"sync", "init", second.arg()});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("--replace") != std::string::npos);
    CHECK(box.run({"config", "get", "sync.remote"}).out == first.arg() + "\n");

    const auto replaced = box.run({"sync", "init", second.arg(), "--replace"});
    CHECK_MESSAGE(replaced.exit_code == 0, replaced.err);
    CHECK(box.run({"config", "get", "sync.remote"}).out == second.arg() + "\n");
    CHECK(box.run({"sync"}).exit_code == 0);
    CHECK(second.commits() == 1);  // ilk geçmiş yeni uzağa gitti
}

TEST_CASE("sync status: off by default; a remote is shown without the credentials in it") {
    const Sandbox box("en");
    const auto plain = box.run({"sync", "status"});
    CHECK(plain.exit_code == 0);
    CHECK(plain.out.find("Sync: off") != std::string::npos);
    CHECK(plain.out.find("Last successful sync: never") != std::string::npos);

    append_config(box, "[sync]\nremote = \"https://kisi:gizli-token@example.com/notlar.git\"\n");
    const auto status = box.run({"sync", "status"});
    CHECK(status.out.find("https://example.com/notlar.git") != std::string::npos);
    CHECK(status.out.find("gizli-token") == std::string::npos);
}

TEST_CASE("sync on without a remote points to sync init") {
    const Sandbox box("en");
    const auto result = box.run({"sync", "on"});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find("note sync init") != std::string::npos);
    CHECK(box.run({"config", "get", "sync.enabled"}).out == "false\n");
}

TEST_CASE("sync usage errors exit 2") {
    const Sandbox box("en");
    CHECK(box.run({"sync", "bogus"}).exit_code == 2);
    CHECK(box.run({"sync", "init"}).exit_code == 2);
    CHECK(box.run({"sync", "status", "fazla"}).exit_code == 2);
    CHECK(box.run({"sync", "login", "a.example", "b.example"}).exit_code == 2);
}

TEST_CASE("sync login: no host and no remote is a usage error; an empty token is refused") {
    const Sandbox box("en");
    CHECK(box.run({"sync", "login"}, "token\n").exit_code == 2);
    const auto empty = box.run({"sync", "login", "noteapp-test.invalid"}, "\n");
    CHECK(empty.exit_code == 4);
    CHECK(empty.err.find("No access token") != std::string::npos);
}

// Gerçek OS kasasına yazar (Windows Credential Manager / Secret Service): elle koşulur.
TEST_CASE("acceptance 8: sync login stores a piped token without echoing it; logout removes it (manual)" *
          doctest::skip()) {
    const Sandbox box("en");
    const std::string host = "noteapp-test.invalid";
    const std::string token = "ghp_noteappClitestToken123";
    const auto login = box.run({"sync", "login", host}, token + "\n");
    CHECK_MESSAGE(login.exit_code == 0, login.err);
    CHECK(noteapp::sync::has_token(host));
    for (const std::string& text :
         {login.out, login.err, slurp(config_toml(box)), slurp(state_dir(box) / "log.txt")})
        CHECK(text.find(token) == std::string::npos);

    CHECK(box.run({"sync", "logout", host}).exit_code == 0);
    CHECK_FALSE(noteapp::sync::has_token(host));
}
