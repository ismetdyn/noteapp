#include "core/config.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
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
#include <string>

namespace fs = std::filesystem;
using noteapp::core::Error;
using noteapp::sync::SyncLock;
using noteapp::sync::SyncResult;
using noteapp::sync::SyncService;
using namespace noteapp::sync_test;

namespace {

constexpr std::chrono::minutes kStaleAfter{10};  // tuning.sync_stale_lock_minutes varsayılanı

std::expected<bool, Error> rejected() {
    return std::unexpected(Error{Error::Code::Io, "sync.push_rejected", {{"branch", "main"}}});
}

}  // namespace

TEST_CASE("resolve_author: config, then ~/.gitconfig, then XDG git config, then user@host") {
    const TempDir root;
    const EnvGuard home("HOME", root.path() / "home");
    const EnvGuard xdg("XDG_CONFIG_HOME", root.path() / "xdg");
    noteapp::core::SyncConfig sync = noteapp::core::parse_config("").config.sync;

    const noteapp::sync::Author fallback = noteapp::sync::resolve_author(sync);
    CHECK_FALSE(fallback.name.empty());
    CHECK(fallback.email.starts_with(fallback.name + "@"));
    CHECK(fallback.email.size() > fallback.name.size() + 1);

    write_file(root.path() / "xdg" / "git" / "config",
               "[user]\n\tname = Xdg Kişi\n\temail = xdg@example.invalid\n");
    noteapp::sync::Author author = noteapp::sync::resolve_author(sync);
    CHECK(author.name == "Xdg Kişi");
    CHECK(author.email == "xdg@example.invalid");

    // Alan alan: ~/.gitconfig'teki ad kazanır, eksik e-posta XDG'den gelir.
    write_file(root.path() / "home" / ".gitconfig", "[user]\n\tname = Ayşe Ev\n");
    author = noteapp::sync::resolve_author(sync);
    CHECK(author.name == "Ayşe Ev");
    CHECK(author.email == "xdg@example.invalid");

    sync.author_name = "Config Kişi";
    sync.author_email = "config@example.invalid";
    author = noteapp::sync::resolve_author(sync);
    CHECK(author.name == "Config Kişi");
    CHECK(author.email == "config@example.invalid");
}

TEST_CASE("SyncLock: a live, fresh lock makes the next acquirer busy; release removes the file") {
    const TempDir root;
    const fs::path file = root.path() / "state" / "sync.lock";
    {
        const auto first = SyncLock::acquire(file, kStaleAfter);
        REQUIRE_MESSAGE(first.has_value(), describe(first));
        CHECK(fs::exists(file));
        const auto second = SyncLock::acquire(file, kStaleAfter);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().message_key == "sync.busy");
    }
    CHECK_FALSE(fs::exists(file));
    CHECK(SyncLock::acquire(file, kStaleAfter).has_value());
}

TEST_CASE("SyncLock: a lock whose owner process is dead is taken over") {
    const TempDir root;
    const fs::path file = root.path() / "sync.lock";
    write_file(file, "999999999\n2026-10-28T00:00:00Z\n");  // böyle bir PID yok
    const auto lock = SyncLock::acquire(file, kStaleAfter);
    REQUIRE_MESSAGE(lock.has_value(), describe(lock));
    CHECK(read_file(file) != "999999999\n2026-10-28T00:00:00Z\n");
}

TEST_CASE("SyncLock: a lock whose owner lives is never taken over, however old it is") {
    const TempDir root;
    const fs::path file = root.path() / "sync.lock";
    const auto running = SyncLock::acquire(file, kStaleAfter);  // bu süreç: canlı sahip
    REQUIRE(running.has_value());
    fs::last_write_time(file, fs::file_time_type::clock::now() - kStaleAfter - std::chrono::minutes(1));
    const auto second = SyncLock::acquire(file, kStaleAfter);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().message_key == "sync.busy");
}

TEST_CASE("SyncLock: a garbled lock is busy while young and stale once older than the stale age") {
    const TempDir root;
    const fs::path file = root.path() / "sync.lock";
    write_file(file, "bozuk içerik");  // ör. sahibi yazarken: PID okunamıyor
    const auto young = SyncLock::acquire(file, kStaleAfter);
    REQUIRE_FALSE(young.has_value());
    CHECK(young.error().message_key == "sync.busy");
    fs::last_write_time(file, fs::file_time_type::clock::now() - kStaleAfter - std::chrono::minutes(1));
    const auto old = SyncLock::acquire(file, kStaleAfter);
    REQUIRE_MESSAGE(old.has_value(), describe(old));
    CHECK(old->took_over());
}

TEST_CASE("SyncLock: the previous owner of a taken-over lock does not delete the new owner's lock") {
    const TempDir root;
    const fs::path file = root.path() / "sync.lock";
    std::optional<std::expected<SyncLock, Error>> previous(SyncLock::acquire(file, kStaleAfter));
    REQUIRE(previous->has_value());
    write_file(file, "999999999\n2026-10-28T00:00:00Z\n");  // dosyada artık ölü bir sahip var
    const auto taken = SyncLock::acquire(file, kStaleAfter);
    REQUIRE(taken.has_value());
    previous.reset();  // eski nesnenin yıkıcısı: kilit artık onun değil
    CHECK(fs::exists(file));
    CHECK(SyncLock::acquire(file, kStaleAfter).error().message_key == "sync.busy");
}

TEST_CASE("retry_rejected_push retries a rejected push exactly once") {
    int calls = 0;
    const auto result = noteapp::sync::retry_rejected_push([&]() -> std::expected<bool, Error> {
        ++calls;
        return calls == 1 ? rejected() : true;
    });
    CHECK(calls == 2);
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(*result);
}

TEST_CASE("retry_rejected_push gives up with sync.push_rejected after the second rejection") {
    int calls = 0;
    const auto result = noteapp::sync::retry_rejected_push([&] {
        ++calls;
        return rejected();
    });
    CHECK(calls == 2);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.push_rejected");
}

TEST_CASE("retry_rejected_push does not retry other errors") {
    int calls = 0;
    const auto result = noteapp::sync::retry_rejected_push([&]() -> std::expected<bool, Error> {
        ++calls;
        return std::unexpected(Error{Error::Code::Io, "sync.git_failed", {{"detail", "x"}}});
    });
    CHECK(calls == 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.git_failed");
}

TEST_CASE("nothing to do: the first sync sets up the carriers, later ones commit and push nothing") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    SyncService sync_a(SyncFixture::config_for(a));

    auto result = sync_a.sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(result->committed == 0);
    CHECK(result->pulled == 0);
    CHECK(commit_count(a) == 1);  // .gitignore + .gitattributes

    put_note("not.md", "içerik\n");
    REQUIRE(sync_a.sync_now().has_value());
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");
    REQUIRE(remote_head.has_value());

    result = sync_a.sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(result->committed == 0);
    CHECK(result->pulled == 0);
    CHECK_FALSE(result->pushed);
    CHECK(commit_count(a) == 2);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
}

TEST_CASE("status file records the last success and keeps it when a later sync fails") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    put_note("not.md", "x\n");
    SyncService sync_a(SyncFixture::config_for(a));
    CHECK_FALSE(sync_a.status().last_success.has_value());
    CHECK_FALSE(sync_a.status().last_error.has_value());

    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    REQUIRE(sync_a.sync_now().has_value());
    CHECK(fs::exists(noteapp::core::state_dir() / "sync-status.toml"));
    auto status = sync_a.status();
    REQUIRE(status.last_success.has_value());
    CHECK(*status.last_success >= before);
    CHECK(*status.last_success <= std::chrono::system_clock::now());
    CHECK_FALSE(status.last_error.has_value());

    {
        const Repository repo = open_repository(a);
        REQUIRE_GIT(git_remote_delete(repo.get(), "origin"));
    }
    const auto result = sync_a.sync_now();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.not_initialized");

    const auto previous_success = status.last_success;
    status = sync_a.status();
    CHECK(status.last_success == previous_success);
    REQUIRE(status.last_error.has_value());
    CHECK(status.last_error->message_key == "sync.not_initialized");
    CHECK(status.last_error->args == result.error().args);
}

TEST_CASE("a notes folder that is not a git repository is sync.not_initialized") {
    const SyncFixture fx;
    const TempDir plain;
    const auto result = SyncService(SyncFixture::config_for(plain.path())).sync_now();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.not_initialized");
}

TEST_CASE("acceptance 6: sync_now is sync.busy while another live process holds the lock") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    put_note("not.md", "x\n");
    SyncService sync_a(SyncFixture::config_for(a));
    {
        const auto held = SyncLock::acquire(noteapp::core::state_dir() / "sync.lock", kStaleAfter);
        REQUIRE(held.has_value());
        const auto result = sync_a.sync_now();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message_key == "sync.busy");
        CHECK(commit_count(a) == 0);                          // kilitsiz hiçbir şeye dokunulmaz
        CHECK_FALSE(sync_a.status().last_error.has_value());  // durum, kilidi tutan sürecindir
    }
    const auto result = sync_a.sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(result->pushed);
}

TEST_CASE("sync_now takes the stale age of a garbled lock from tuning.sync_stale_lock_minutes") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    put_note("not.md", "x\n");
    const fs::path file = noteapp::core::state_dir() / "sync.lock";
    write_file(file, "bozuk içerik");
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(2));

    const auto busy = SyncService(SyncFixture::config_for(a)).sync_now();  // varsayılan 10 dk: taze
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error().message_key == "sync.busy");

    noteapp::core::Config config = SyncFixture::config_for(a);
    config.tuning.sync_stale_lock_minutes = 1;
    const auto result = SyncService(config).sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(result->pushed);
}

TEST_CASE("HEAD on another branch than sync.branch is refused without touching anything") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    {
        const Repository repo = open_repository(a);
        REQUIRE_GIT(git_repository_set_head(repo.get(), "refs/heads/baska"));
    }
    put_note("not.md", "x\n");
    const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.branch_mismatch");
    CHECK(commit_count(a) == 0);
}

TEST_CASE("every sync message key exists in the catalog") {
    noteapp::core::init_i18n("en");
    for (const char* key :
         {"sync.commit_message", "sync.merge_message", "sync.not_initialized", "sync.busy",
          "sync.repo_busy_state", "sync.unrelated_histories", "sync.push_rejected", "sync.branch_mismatch",
          "sync.git_failed", "sync.timeout", "sync.gitignore_not_regular", "sync.remote_invalid_paths"})
        CHECK_MESSAGE(noteapp::core::tr(key) != key, key);
}
