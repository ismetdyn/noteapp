// Sonlandırılmış (AutoSync zaman aşımı, çökme) bir senkronun artıkları: SPEC-sync karar 3. Sonraki senkron
// bayat kilidi devralınca libgit2 kilitlerini temizler; yarım merge'i yalnız index çakışmasızsa kapatır.
#include "core/config.hpp"
#include "core/paths.hpp"
#include "sync/sync_service.hpp"
#include "sync_fixture.hpp"

#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <git2.h>
#include <memory>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using noteapp::sync::SyncService;
using namespace noteapp::sync_test;

namespace {

// Öldürülmüş bir senkronun kilidi: sahibi olan PID yok.
void leave_dead_sync_lock() {
    write_file(noteapp::core::state_dir() / "sync.lock", "999999999\n2026-10-28T00:00:00Z\n00000000\n");
}

const char* const kGitLocks[] = {"index.lock", "HEAD.lock", "refs/heads/main.lock"};

void leave_git_locks(const fs::path& repo) {
    for (const char* lock : kGitLocks)
        write_file(repo / ".git" / lock, "");
}

// İlk senkronu yapılmış klon: HEAD ve uzak dal aynı commit'te.
fs::path synced_clone(const SyncFixture& fx) {
    const fs::path a = fx.make_clone(u8"A");
    write_file(a / "not.md", "ortak\n");
    const auto first = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_MESSAGE(first.has_value(), describe(first));
    return a;
}

}  // namespace

TEST_CASE("after a killed sync the next one removes the libgit2 locks it left behind and syncs") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    write_file(a / "not.md", "x\n");
    leave_git_locks(a);
    leave_dead_sync_lock();

    const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(result->pushed);
    for (const char* lock : kGitLocks)
        CHECK_MESSAGE(!fs::exists(a / ".git" / lock), lock);
    CHECK(commit_count(fx.remote()) == 1);
}

TEST_CASE("libgit2 locks are left alone when no stale sync lock was taken over") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    write_file(a / "not.md", "x\n");
    write_file(a / ".git" / "index.lock", "");  // ör. kullanıcının o an çalışan git komutu

    const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_FALSE(result.has_value());
    CHECK(fs::exists(a / ".git" / "index.lock"));
}

TEST_CASE("a merge a killed sync left without conflicts in the index is closed and the sync goes on") {
    const SyncFixture fx;
    const fs::path a = synced_clone(fx);
    write_file(a / ".git" / "MERGE_HEAD", *ref_id(a, "HEAD") + "\n");
    write_file(a / "yeni.md", "yeni\n");
    leave_dead_sync_lock();

    const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK_FALSE(fs::exists(a / ".git" / "MERGE_HEAD"));
    CHECK(result->committed == 1);
    CHECK(read_file(a / "yeni.md") == "yeni\n");
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(a, "HEAD"));
}

TEST_CASE("a merge with unresolved conflicts is never closed: sync.repo_busy_state, nothing touched") {
    const SyncFixture fx;
    const fs::path a = synced_clone(fx);
    {
        const Repository repo = open_repository(a);
        git_index* raw = nullptr;
        REQUIRE_GIT(git_repository_index(&raw, repo.get()));
        std::unique_ptr<git_index, GitFree<git_index_free>> index(raw);
        git_index_entry ours{}, theirs{};
        for (auto [entry, text] : {std::pair{&ours, "bizim\n"}, std::pair{&theirs, "onların\n"}}) {
            REQUIRE_GIT(git_blob_create_from_buffer(&entry->id, repo.get(), text, std::strlen(text)));
            entry->path = "not.md";
            entry->mode = GIT_FILEMODE_BLOB;
        }
        REQUIRE_GIT(git_index_conflict_add(index.get(), nullptr, &ours, &theirs));
        REQUIRE_GIT(git_index_write(index.get()));
    }
    write_file(a / ".git" / "MERGE_HEAD", *ref_id(a, "HEAD") + "\n");
    leave_dead_sync_lock();
    const auto head = ref_id(a, "HEAD");

    const auto result = SyncService(SyncFixture::config_for(a)).sync_now();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.repo_busy_state");
    CHECK(fs::exists(a / ".git" / "MERGE_HEAD"));
    CHECK(ref_id(a, "HEAD") == head);
    CHECK(read_file(a / "not.md") == "ortak\n");
}
