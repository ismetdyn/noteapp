// Senkron oplog üzerinde (G5): iki klon, gerçek günlük dosyaları, gerçek git (yerel bare uzak). Her cihazın
// kendi NOTEAPP_HOME'u ve graph.db'si vardır. Senaryo eşlemesi: tasks/g5-test-map.md.
#include "core/file_stat.hpp"
#include "core/migration.hpp"
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "graph/oplog.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "sync/git_runtime.hpp"
#include "sync/note_status.hpp"
#include "sync/sync_service.hpp"
#include "sync/text_merge.hpp"
#include "sync_fixture.hpp"

#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using noteapp::core::NoteStore;
using noteapp::core::SyncState;
using noteapp::sync::SyncResult;
using namespace noteapp::sync_test;
namespace core = noteapp::core;
namespace graph = noteapp::graph;

namespace {

core::NotePath np(const std::string& path) {
    return *core::NotePath::parse(path);
}

std::vector<std::string> paths_of(const Device& device) {
    std::vector<std::string> paths;
    for (const core::NoteInfo& info : device.store().list())
        paths.push_back(info.path.string());
    std::ranges::sort(paths);
    return paths;
}

std::vector<std::string> trash_of(const Device& device) {
    std::vector<std::string> paths;
    const auto items = device.store().list_trash();
    REQUIRE(items.has_value());
    for (const core::TrashItem& item : *items)
        paths.push_back(item.path.string());
    std::ranges::sort(paths);
    return paths;
}

SyncResult must_sync(const Device& device) {
    auto result = device.sync();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    return std::move(*result);
}

void exchange(const Device& a, const Device& b) {
    must_sync(a);
    must_sync(b);
    must_sync(a);
    must_sync(b);
}

bool is_copy(const std::string& path) {
    return path.find(" (conflict ") != std::string::npos;
}

std::set<std::string> head_files(const fs::path& repo_dir, const std::string& ref = "HEAD") {
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

const std::string kExact = std::string("\xEF\xBB\xBFsatır\r\nnul:\0son\xFF\n", 21);

}  // namespace

TEST_CASE("acceptance 1: a note from A reaches B and B's own note goes back to A, nothing lost") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("klasör/ilk not.md", "A'nın notu\n");
    const SyncResult first = must_sync(a);
    CHECK(first.committed == 1);
    CHECK(first.pushed);

    b.put("b.md", "B'nin notu\n");
    SyncResult second = must_sync(b);
    CHECK(second.pulled == 1);
    CHECK(second.committed == 1);
    CHECK(second.pushed);
    CHECK(b.body("klasör/ilk not.md") == "A'nın notu\n");

    const SyncResult third = must_sync(a);
    CHECK(third.pulled == 1);
    CHECK_FALSE(third.pushed);
    CHECK(a.body("b.md") == "B'nin notu\n");
    CHECK(paths_of(a) == paths_of(b));
}

TEST_CASE("only carriers are committed: other files in the repository stay untouched") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "gizli içerik\n");
    must_sync(a);
    const auto files = head_files(a.notes);
    std::size_t logs = 0;
    for (const std::string& file : files)
        logs += file.starts_with("graph/") && file.ends_with(".oplog") ? 1 : 0;
    CHECK(logs == 1);
    CHECK(files.size() == logs + 2);
    CHECK(files.contains(".gitignore"));
    CHECK(files.contains(".gitattributes"));
    CHECK(read_file(a.notes / ".gitattributes").find("*.oplog -text -merge") != std::string::npos);
    CHECK_FALSE(fs::exists(a.notes / "not.md"));

    // Depodaki yabancı dosya sahnelenmez, silinmez, değiştirilmez.
    write_file(a.notes / ".yabanci", "dokunma\n");
    a.put("iki.md", "x\n");
    must_sync(a);
    CHECK(read_file(a.notes / ".yabanci") == "dokunma\n");
    CHECK_FALSE(head_files(a.notes).contains(".yabanci"));
}

TEST_CASE("acceptance 2: changes to different notes merge both ways without copies") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("a.md", "a0\n");
    a.put("b.md", "b0\n");
    exchange(a, b);
    a.put("a.md", "a1\n");
    b.put("b.md", "b1\n");
    exchange(a, b);
    for (const Device* device : {&a, &b}) {
        CHECK(paths_of(*device) == std::vector<std::string>{"a.md", "b.md"});
        CHECK(device->body("a.md") == "a1\n");
        CHECK(device->body("b.md") == "b1\n");
    }
}

TEST_CASE("acceptance 3: the same note changed on both sides keeps both bytes, one copy, on both devices") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("not.md", "ortak\n");
    exchange(a, b);
    const std::string on_a = kExact + "a";
    const std::string on_b = kExact + "b";
    a.put("not.md", on_a);
    b.put("not.md", on_b);
    must_sync(a);
    const SyncResult merged = must_sync(b);
    CHECK(merged.conflict_copies.size() == 1);
    must_sync(a);
    must_sync(b);
    must_sync(a);

    const auto paths = paths_of(a);
    CHECK(paths_of(b) == paths);
    REQUIRE(paths.size() == 2);
    CHECK(std::ranges::find(paths, "not.md") != paths.end());
    CHECK(std::ranges::count_if(paths, is_copy) == 1);
    std::set<std::string> bodies;
    for (const std::string& path : paths) {
        CHECK(a.body(path) == b.body(path));
        bodies.insert(a.body(path));
    }
    CHECK(bodies == std::set<std::string>{on_a, on_b});
}

TEST_CASE("acceptance 4: delete against edit keeps the edit, whichever side syncs first") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("not.md", "ortak\n");
    exchange(a, b);

    SUBCASE("purge on A, edit on B") {
        REQUIRE(a.store().remove(np("not.md")).has_value());
        b.put("not.md", "düzenlendi\n");
    }
    SUBCASE("trash on A, edit on B") {
        REQUIRE(a.store().trash(np("not.md")).has_value());
        b.put("not.md", "düzenlendi\n");
    }
    exchange(a, b);
    for (const Device* device : {&a, &b}) {
        CHECK(paths_of(*device) == std::vector<std::string>{"not.md"});
        CHECK(device->body("not.md") == "düzenlendi\n");
    }
}

TEST_CASE("a delete that saw the edit removes the note on the other device too") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("not.md", "x\n");
    a.put("kalan.md", "y\n");
    exchange(a, b);
    REQUIRE(a.store().remove(np("not.md")).has_value());
    exchange(a, b);
    CHECK(paths_of(b) == std::vector<std::string>{"kalan.md"});
}

TEST_CASE("add/add on one path: both notes stay, the second takes -2 the same way everywhere") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("not.md", "a\n");
    b.put("not.md", "b\n");
    exchange(a, b);
    CHECK(paths_of(a) == std::vector<std::string>{"not-2.md", "not.md"});
    CHECK(paths_of(b) == paths_of(a));
    CHECK(a.body("not.md") == b.body("not.md"));
    std::set<std::string> bodies{a.body("not.md"), a.body("not-2.md")};
    CHECK(bodies == std::set<std::string>{"a\n", "b\n"});
}

TEST_CASE("several conflicts in one sync each get a copy; other changes merge alongside; Turkish names") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("klasör/özet ç.md", "0\n");
    a.put("iki.md", "0\n");
    a.put("üç.md", "0\n");
    exchange(a, b);
    a.put("klasör/özet ç.md", "a1\n");
    b.put("klasör/özet ç.md", "b1\n");
    a.put("iki.md", "a1\n");
    b.put("iki.md", "b1\n");
    b.put("üç.md", "b1\n");
    exchange(a, b);
    const auto paths = paths_of(a);
    CHECK(paths_of(b) == paths);
    CHECK(paths.size() == 5);
    CHECK(std::ranges::count_if(paths, is_copy) == 2);
    CHECK(a.body("üç.md") == "b1\n");
    const auto copy_in_folder = std::ranges::find_if(paths, [](const std::string& path) {
        return path.starts_with("klasör/özet ç (conflict ") && path.ends_with(".md");
    });
    CHECK(copy_in_folder != paths.end());
}

TEST_CASE("a conflict copy's name fits 255 bytes and is cut on a UTF-8 boundary") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    std::string stem;
    while (stem.size() + 2 + 3 <= 250)
        stem += "ç";
    const std::string file = "x" + stem + ".md";
    a.put(file, "0\n");
    exchange(a, b);
    a.put(file, "a\n");
    b.put(file, "b\n");
    exchange(a, b);
    const auto paths = paths_of(a);
    CHECK(paths_of(b) == paths);
    REQUIRE(paths.size() == 2);
    const auto copy = std::ranges::find_if(paths, is_copy);
    REQUIRE(copy != paths.end());
    CHECK(copy->size() <= 255);
    CHECK(noteapp::core::is_valid_utf8(*copy));
}

TEST_CASE("acceptance 5: the remote advancing between fetch and push is merged and pushed on the retry") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("a.md", "a\n");
    exchange(a, b);
    a.put("a2.md", "a2\n");
    b.put("b2.md", "b2\n");
    bool raced = false;
    noteapp::sync::SyncTestHooks hooks;
    hooks.before_push = [&] {
        if (std::exchange(raced, true))
            return;
        const auto pushed = b.sync();  // uzak ilerler: A'nın push'u non-fast-forward olur
        REQUIRE_MESSAGE(pushed.has_value(), describe(pushed));
    };
    const auto result = a.sync(hooks);
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(raced);
    CHECK(result->pushed);
    CHECK(a.body("b2.md") == "b2\n");
    must_sync(b);
    CHECK(b.body("a2.md") == "a2\n");
    CHECK(ref_id(a.notes, "HEAD") == ref_id(fx.remote(), "refs/heads/main"));
}

// Windows: libgit2 yerel taşıması aynı paketi ikinci kez göndermede salt-okunur .idx üzerine yazamıyor
// (Access denied, Wine'da aralıklı); yalnız dosya yolu uzak depoda olur, ret akışı burada sınanamaz.
#ifndef _WIN32
TEST_CASE("a push rejected twice is reported, the commit stays and the next sync delivers it") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("bir.md", "1\n");
    must_sync(a);
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");
    const fs::path remote_lock = fx.remote() / "refs" / "heads" / "main.lock";
    write_file(remote_lock, "");
    a.put("iki.md", "2\n");
    const auto refused = a.sync();
    REQUIRE_FALSE(refused.has_value());
    CAPTURE(describe(refused));
    CHECK(refused.error().message_key == "sync.push_rejected");
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
    CHECK(a.body("iki.md") == "2\n");
    const auto status = a.status();
    REQUIRE(status.last_error.has_value());
    CHECK(status.last_error->message_key == "sync.push_rejected");

    fs::remove(remote_lock);
    const SyncResult retried = must_sync(a);
    CHECK(retried.committed == 1);  // henüz gönderilmemişti
    CHECK(retried.pushed);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(a.notes, "HEAD"));
}
#endif

TEST_CASE("unrelated histories are sync.unrelated_histories and nothing is merged") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("a.md", "A\n");
    must_sync(a);
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");
    const Device b(fx, "B");
    commit_entries(b.notes, {{"kendi-koku", "ayrı geçmiş\n"}});
    b.put("b.md", "B\n");
    const auto refused = b.sync();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message_key == "sync.unrelated_histories");
    CHECK(paths_of(b) == std::vector<std::string>{"b.md"});
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
}

TEST_CASE("sync with a clean clone of the remote: every device joins through enable") {
    const SyncFixture fx;
    const Device a(fx, "A", false);
    const Device b(fx, "B", false);
    a.put("ilk not.md", "A'nın notu\n");
    const auto enabled_a = a.enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(enabled_a.has_value(), describe(enabled_a));
    const auto enabled_b = b.enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(enabled_b.has_value(), describe(enabled_b));
    must_sync(b);
    CHECK(b.body("ilk not.md") == "A'nın notu\n");
    b.put("klasör/B notu.md", "B'nin notu\n");
    must_sync(b);
    a.put("ilk not.md", "A'nın notu, düzenlendi\n");
    const SyncResult from_a = must_sync(a);
    CHECK(from_a.pulled == 1);
    CHECK(a.body("klasör/B notu.md") == "B'nin notu\n");
    const SyncResult back = must_sync(b);
    CHECK(b.body("ilk not.md") == "A'nın notu, düzenlendi\n");
    CHECK(back.conflict_copies.empty());
    CHECK(ref_id(a.notes, "HEAD") == ref_id(b.notes, "HEAD"));
}

TEST_CASE("trash, restore, purge and empty_trash travel with exact bytes and counts") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("d/not.md", kExact);
    a.put("kalan.md", "k\n");
    exchange(a, b);

    REQUIRE(a.store().trash(np("d/not.md")).has_value());
    const SyncResult thrown = must_sync(a);
    CHECK(thrown.committed == 1);
    CHECK(thrown.pushed);
    const SyncResult arrived = must_sync(b);
    CHECK(arrived.pulled == 1);
    CHECK(paths_of(b) == std::vector<std::string>{"kalan.md"});
    CHECK(trash_of(b) == std::vector<std::string>{"d/not.md"});

    const auto restored = b.store().restore(np("d/not.md"));
    REQUIRE(restored.has_value());
    CHECK(must_sync(b).committed == 1);
    CHECK(must_sync(a).pulled == 1);
    CHECK(a.body("d/not.md") == kExact);
    CHECK(trash_of(a).empty());

    REQUIRE(a.store().trash(np("d/not.md")).has_value());
    REQUIRE(a.store().purge(np("d/not.md")).has_value());
    const SyncResult purged = must_sync(a);
    CHECK(purged.committed == 0);  // kalıcı silme not sayılmaz, ama gönderilir
    CHECK(purged.pushed);
    must_sync(b);
    CHECK(paths_of(b) == std::vector<std::string>{"kalan.md"});

    REQUIRE(b.store().trash(np("kalan.md")).has_value());
    REQUIRE(b.store().empty_trash().has_value());
    exchange(a, b);
    CHECK(paths_of(a).empty());
    CHECK(trash_of(a).empty());
}

TEST_CASE("the same version trashed on both sides is one trash item, no copy") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("not.md", "x\n");
    exchange(a, b);
    REQUIRE(a.store().trash(np("not.md")).has_value());
    REQUIRE(b.store().trash(np("not.md")).has_value());
    exchange(a, b);
    CHECK(trash_of(a) == std::vector<std::string>{"not.md"});
    CHECK(trash_of(b) == trash_of(a));
    CHECK(paths_of(a).empty());
}

TEST_CASE("empty folders travel to the other device; a folder rename reaches notes inside it") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    REQUIRE(a.store().create_folder("boş/alt").has_value());
    a.put("iş/not.md", "x\n");
    exchange(a, b);
    CHECK(b.store().folders() == std::vector<std::string>{"boş", "boş/alt", "iş"});
    REQUIRE(b.store().rename_folder("iş", "yeni").has_value());
    exchange(a, b);
    CHECK(paths_of(a) == std::vector<std::string>{"yeni/not.md"});
}

TEST_CASE("a hostile name from the remote never becomes a path and the rest of the log still applies") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    {
        const EnvGuard home("NOTEAPP_HOME", a.home);
        auto raw = graph::Graph::open(a.home / "data" / "graph.db");
        if (!raw) {  // ilk açılış
            fs::create_directories(a.home / "data");
            raw = graph::Graph::open(a.home / "data" / "graph.db");
        }
        REQUIRE(raw.has_value());
        auto txn = raw->write();
        REQUIRE(txn.has_value());
        for (const char* name : {"../../dışarı", "CON", ".git", "a/b"}) {
            const graph::NodeId id = graph::new_node_id();
            REQUIRE(txn->put_node(id).has_value());
            REQUIRE(txn->add_label(id, "Note").has_value());
            REQUIRE(txn->set_property(id, "name", graph::Value{std::string(name)}).has_value());
            REQUIRE(txn->set_property(id, "type", graph::Value{std::string("md")}).has_value());
            REQUIRE(txn->set_property(id, "body", graph::Value{graph::Bytes{"x"}}).has_value());
        }
        REQUIRE(txn->commit().has_value());
    }
    a.put("gerçek.md", "kalır\n");
    must_sync(a);
    must_sync(b);
    CHECK(paths_of(b) == std::vector<std::string>{"gerçek.md"});
    CHECK(b.body("gerçek.md") == "kalır\n");
    REQUIRE(core::export_all_notes(b.store(), b.home / "out").has_value());
    CHECK_FALSE(fs::exists(b.home / "dışarı.md"));
}

TEST_CASE(
    "a damaged remote log is reported in the status, stops only that device and other notes keep flowing") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    const Device c(fx, "C");
    a.put("a.md", "a\n");
    exchange(a, b);
    c.put("c.md", "c\n");
    must_sync(c);
    must_sync(b);

    const auto a_name = core::own_log_name(*std::make_unique<NoteStore>(a.store()));
    REQUIRE(a_name.has_value());
    {  // Tam uzunlukta ama özeti tutmayan çerçeve: A'nın günlüğü orada durmalı.
        a.put("a2.md", "a2\n");
        must_sync(a);
        must_sync(b);
        const fs::path log = b.notes / "graph" / *a_name;
        std::string bytes = read_file(log);
        REQUIRE(bytes.size() > 40);
        std::string damaged = bytes;
        damaged.back() = static_cast<char>(damaged.back() ^ 0x55);
        a.put("a3.md", "a3\n");  // A'nın dosyası yerelde sağlam ilerler
        must_sync(a);
        commit_entries(fx.remote(), {});
    }
    // Yerel kopyayı bozarız: B'nin ingest'i kendi çalışma ağacındaki dosyayı okur.
    const fs::path log = b.notes / "graph" / *a_name;
    const std::string good = read_file(log);
    std::string damaged = good;
    damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x55);
    write_file(log, damaged);
    const EnvGuard home("NOTEAPP_HOME", b.home);
    NoteStore store = b.store();
    const std::vector<fs::path> logs{log};
    const auto ingested = core::ingest_note_logs(store, logs);
    REQUIRE(ingested.has_value());
    CHECK_FALSE(ingested->report.stalled.empty());
    CHECK(b.body("a.md") == "a\n");
    CHECK(b.body("c.md") == "c\n");
}

TEST_CASE("sync-status.toml surfaces stalled logs, partial tails and heads; note sync status can read them") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("a.md", "a\n");
    exchange(a, b);

    // Uzağa yarım kuyruklu ve yabancı cihazlı günlük gelir: git taşır, B raporlar, senkron başarılıdır.
    const std::string foreign_device(32, 'a');
    const graph::detail::DeviceId device_id{
        std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA},
        std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA},
        std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}};
    const std::string header = graph::detail::encode_log_header(device_id);
    {
        const Repository repo = open_repository(b.notes);
        (void)repo;
    }
    write_file(a.notes / "graph" / graph::detail::log_file_name(device_id),
               header + std::string("\x40\x00", 2));
    {
        const Repository repo = open_repository(a.notes);
        git_index* index = nullptr;
        REQUIRE_GIT(git_repository_index(&index, repo.get()));
        const std::string relative = "graph/" + graph::detail::log_file_name(device_id);
        REQUIRE_GIT(git_index_add_bypath(index, relative.c_str()));
        REQUIRE_GIT(git_index_write(index));
        git_index_free(index);
    }
    must_sync(a);
    must_sync(b);
    const auto status = b.status();
    CHECK(status.last_success.has_value());
    const auto partial =
        std::ranges::find(status.log_issues, std::string("sync.log_partial_tail"), &core::Error::message_key);
    REQUIRE(partial != status.log_issues.end());
    CHECK_FALSE(status.heads.empty());
    CHECK(read_file(b.home / "state" / "sync-status.toml").find("log_partial_tail") != std::string::npos);
    // Git'te bozuk dosya olsa da diğer not geçer.
    a.put("sonra.md", "x\n");
    must_sync(a);
    must_sync(b);
    CHECK(b.body("sonra.md") == "x\n");
}

TEST_CASE("own log first: a restored database catches up and never forks seq") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("eski.md", "eski\n");
    must_sync(a);
    const fs::path db = a.home / "data" / "graph.db";
    const fs::path backup = a.home / "graph.db.bak";
    {  // WAL boşaltılsın diye bağlantı kapanır; kopya geri yüklenmiş yedektir.
        NoteStore checkpoint = a.store();
        CHECK(checkpoint.list().size() == 1);
    }
    fs::copy_file(db, backup);
    for (const char* extra : {"-wal", "-shm"})
        if (fs::exists(a.home / "data" / (std::string("graph.db") + extra)))
            fs::copy_file(a.home / "data" / (std::string("graph.db") + extra),
                          a.home / (std::string("graph.db.bak") + extra));
    a.put("yeni.md", "yedekten sonra\n");
    must_sync(a);
    must_sync(b);
    CHECK(b.body("yeni.md") == "yedekten sonra\n");

    fs::remove(db);
    for (const char* extra : {"-wal", "-shm"})
        fs::remove(a.home / "data" / (std::string("graph.db") + extra));
    fs::copy_file(backup, db);
    for (const char* extra : {"-wal", "-shm"})
        if (fs::exists(a.home / (std::string("graph.db.bak") + extra)))
            fs::copy_file(a.home / (std::string("graph.db.bak") + extra),
                          a.home / "data" / (std::string("graph.db") + extra));

    const SyncResult caught_up = must_sync(a);
    CHECK(a.body("yeni.md") == "yedekten sonra\n");
    CHECK(caught_up.log_issues.empty());
    a.put("sonraki.md", "taze\n");
    must_sync(a);
    const SyncResult at_b = must_sync(b);
    CHECK(at_b.log_issues.empty());  // seq_digest_mismatch yok
    CHECK(b.body("sonraki.md") == "taze\n");
}

TEST_CASE("a lost graph.db comes back from the logs") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", kExact);
    must_sync(a);
    fs::remove_all(a.home / "data");
    must_sync(a);
    CHECK(a.body("not.md") == kExact);
}

TEST_CASE("a local log that does not match the database is set aside, kept and rewritten") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "x\n");
    must_sync(a);
    const auto name = core::own_log_name(*std::make_unique<NoteStore>(a.store()));
    REQUIRE(name.has_value());
    const fs::path log = a.notes / "graph" / *name;
    std::string bytes = read_file(log);
    bytes[bytes.size() - 2] = static_cast<char>(bytes[bytes.size() - 2] ^ 0x55);
    write_file(log, bytes);
    a.put("iki.md", "y\n");
    const SyncResult result = must_sync(a);
    const auto aside =
        std::ranges::find(result.log_issues, std::string("sync.log_set_aside"), &core::Error::message_key);
    CHECK(aside != result.log_issues.end());
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(a.notes / "graph"))
        kept = kept || utf8(entry.path().filename()).find(".aside-") != std::string::npos;
    CHECK(kept);
    for (const std::string& file : head_files(a.notes))
        CHECK(file.find(".aside-") == std::string::npos);
}

TEST_CASE("a log folder that is a link is not used") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "x\n");
    must_sync(a);
    const fs::path outside = fx.root() / "dışarı";
    fs::create_directories(outside);
    fs::rename(a.notes / "graph", a.notes / "graph.real");
    REQUIRE(noteapp::test::make_dir_link(outside, a.notes / "graph"));
    a.put("iki.md", "y\n");
    const auto refused = a.sync();
    REQUIRE_FALSE(refused.has_value());
    CHECK(fs::is_empty(outside));
}

TEST_CASE("git bytes stay exact: a global autocrlf changes nothing in a note body") {
    const SyncFixture fx;  // fikstür global core.autocrlf=true yazar
    const Device a(fx, "A");
    const Device b(fx, "B");
    a.put("crlf.md", "bir\r\niki\n");
    a.put("lf.md", "bir\niki\n");
    exchange(a, b);
    CHECK(b.body("crlf.md") == "bir\r\niki\n");
    CHECK(b.body("lf.md") == "bir\niki\n");
}

TEST_CASE("sync refuses while old notes wait for migration and leaves everything alone") {
    const SyncFixture fx;
    const Device a(fx, "A");
    write_file(a.notes / "eski.md", "eski\n");
    const auto refused = a.sync();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == core::Error::Code::MigrationRequired);
    CHECK(commit_count(a.notes) == 0);
    CHECK(read_file(a.notes / "eski.md") == "eski\n");
    const auto status = a.status();
    REQUIRE(status.last_error.has_value());
    CHECK(status.last_error->message_key == "error.migration_required");
}

TEST_CASE("note status: new, modified, synced, conflict_copy and sync_off from frames") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    core::Config config = a.config();
    const auto states = [&](const Device& device) {
        const EnvGuard home("NOTEAPP_HOME", device.home);
        core::Config current = device.config();
        current.sync.enabled = true;
        std::vector<core::NoteMeta> notes = device.store().list_meta();
        REQUIRE(noteapp::sync::fill_sync_status(current, notes).has_value());
        std::map<std::string, SyncState> map;
        for (const core::NoteMeta& note : notes)
            map[note.path.string()] = note.sync_status;
        return map;
    };

    a.put("bir.md", "1\n");
    a.put("iki.md", "2\n");
    CHECK(states(a).at("bir.md") == SyncState::New);  // henüz hiç gönderilmedi
    must_sync(a);
    CHECK(states(a).at("bir.md") == SyncState::Synced);
    a.put("bir.md", "1b\n");
    a.put("uc.md", "3\n");
    auto now = states(a);
    CHECK(now.at("bir.md") == SyncState::Modified);
    CHECK(now.at("iki.md") == SyncState::Synced);
    CHECK(now.at("uc.md") == SyncState::New);

    must_sync(a);
    must_sync(b);
    CHECK(states(b).at("bir.md") == SyncState::Synced);  // uzaktan geldi

    a.put("bir.md", "a son\n");
    b.put("bir.md", "b son\n");
    must_sync(a);
    must_sync(b);
    bool saw_copy = false;
    for (const auto& [path, state] : states(b))
        if (is_copy(path)) {
            CHECK(state == SyncState::ConflictCopy);
            saw_copy = true;
        }
    CHECK(saw_copy);

    config.sync.enabled = false;
    std::vector<core::NoteMeta> off = a.store().list_meta();
    REQUIRE(noteapp::sync::fill_sync_status(config, off).has_value());
    CHECK(off.front().sync_status == SyncState::SyncOff);

    core::Config not_a_repo = a.config();
    not_a_repo.sync.enabled = true;
    not_a_repo.notes_dir = fx.root() / "depo değil";
    std::vector<core::NoteMeta> outside = a.store().list_meta();
    REQUIRE(noteapp::sync::fill_sync_status(not_a_repo, outside).has_value());
    CHECK(outside.front().sync_status == SyncState::SyncOff);
}

TEST_CASE("is_conflict_copy_name follows the copy naming rule on the file name only") {
    using noteapp::sync::is_conflict_copy_name;
    CHECK(is_conflict_copy_name("not (conflict 2026-10-28 1430).md"));
    CHECK(is_conflict_copy_name("klasör/not (conflict 2026-10-28 1430) 2.md"));
    CHECK(is_conflict_copy_name("not (conflict 2026-10-28 1430)-2.md"));
    CHECK(is_conflict_copy_name("yapilacaklar (conflict 2026-10-28 1430)"));
    CHECK_FALSE(is_conflict_copy_name("not.md"));
    CHECK_FALSE(is_conflict_copy_name("not (conflict 2026-10-28).md"));
    CHECK_FALSE(is_conflict_copy_name("x (conflict 2026-10-28 1430)/not.md"));
}

TEST_CASE("two-clone migration: both devices migrate old files, identical bytes become one note") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B", false);
    // Eski sürümün deposu: düz dosyalar git'te, iki cihazda aynı.
    write_file(a.notes / "ortak.md", std::string("ortak\r\n\0x", 9));
    write_file(a.notes / "work" / "a.md", "a\n");
    fs::create_directories(a.notes / "bos");
    write_file(a.notes / "bos" / ".keep", "");
    write_file(a.notes / ".trash" / "cop.md", "çöp\n");
    {
        const Repository repo = open_repository(a.notes);
        git_index* index = nullptr;
        REQUIRE_GIT(git_repository_index(&index, repo.get()));
        REQUIRE_GIT(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr));
        REQUIRE_GIT(git_index_write(index));
        git_oid tree_id;
        REQUIRE_GIT(git_index_write_tree(&tree_id, index));
        git_index_free(index);
        git_tree* tree = nullptr;
        REQUIRE_GIT(git_tree_lookup(&tree, repo.get(), &tree_id));
        git_signature* who = nullptr;
        REQUIRE_GIT(git_signature_now(&who, "Eski", "eski@noteapp.invalid"));
        git_oid commit_id;
        REQUIRE_GIT(git_commit_create(&commit_id, repo.get(), "HEAD", who, who, nullptr, "eski surum", tree,
                                      0, nullptr));
        git_signature_free(who);
        git_tree_free(tree);
        git_remote* origin = nullptr;
        REQUIRE_GIT(git_remote_lookup(&origin, repo.get(), "origin"));
        char refspec[] = "refs/heads/main:refs/heads/main";
        char* refspecs[] = {refspec};
        const git_strarray list{refspecs, 1};
        REQUIRE_GIT(git_remote_push(origin, &list, nullptr));
        git_remote_free(origin);
    }
    // B eski dosyalarla klonlar: git yalnız taşır.
    {
        git_repository* raw = nullptr;
        REQUIRE_GIT(git_clone(&raw, utf8(fx.remote()).c_str(), utf8(b.notes).c_str(), nullptr));
        git_repository_free(raw);
    }
    CHECK(read_file(b.notes / "work" / "a.md") == "a\n");

    // Geçişten önce yazma ve senkron reddedilir.
    REQUIRE_FALSE(a.sync().has_value());
    {
        const EnvGuard home("NOTEAPP_HOME", a.home);
        NoteStore gated = NoteStore::for_config(a.config());
        CHECK(gated.create(core::NewNote{"", "x", "md", "y"}).error().code ==
              core::Error::Code::MigrationRequired);
    }

    // A geçer ve yollar; B henüz geçmemişken A'nın notları git'le gelir.
    const auto migrated_a = core::migrate_legacy_notes(a.notes, a.home / "data");
    REQUIRE_MESSAGE(migrated_a.has_value(), describe(migrated_a));
    CHECK(migrated_a->complete);
    CHECK(migrated_a->imported == 3);
    CHECK(fs::exists(a.notes / "ortak.md"));  // eski dosya silinmez
    must_sync(a);

    const auto migrated_b = core::migrate_legacy_notes(b.notes, b.home / "data");
    REQUIRE_MESSAGE(migrated_b.has_value(), describe(migrated_b));
    CHECK(migrated_b->complete);
    must_sync(b);
    must_sync(a);

    for (const Device* device : {&a, &b}) {
        CHECK(paths_of(*device) == std::vector<std::string>{"ortak.md", "work/a.md"});
        CHECK(device->body("ortak.md") == std::string("ortak\r\n\0x", 9));
        CHECK(trash_of(*device) == std::vector<std::string>{"cop.md"});
        const auto folders = device->store().folders();
        CHECK(std::ranges::find(folders, "bos") != folders.end());
        for (const core::NoteMeta& meta : device->store().list_meta())
            CHECK(meta.origin == "unknown");
    }
    // Eski dosyalar her iki depoda duruyor, senkron onlara dokunmaz.
    CHECK(read_file(b.notes / "work" / "a.md") == "a\n");

    // A'nın geçiş sonrası düzenlemesi, B'nin eski dosyasıyla çakışmaz.
    a.put("work/a.md", "a düzenlendi\n");
    exchange(a, b);
    CHECK(b.body("work/a.md") == "a düzenlendi\n");
    CHECK(paths_of(b) == std::vector<std::string>{"ortak.md", "work/a.md"});
}

TEST_CASE(
    "migration after the other device edited: a stale old file does not become a copy, a changed one does") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B", false);
    write_file(a.notes / "ayni.md", "taban\n");
    write_file(a.notes / "degisik.md", "taban\n");
    REQUIRE(core::migrate_legacy_notes(a.notes, a.home / "data").has_value());
    must_sync(a);
    a.put("ayni.md", "A düzenledi\n");
    must_sync(a);

    // B: aynı taban + B'nin gönderilmemiş eski dosya düzenlemesi; önce günlükleri alır, sonra geçer.
    {
        git_repository* raw = nullptr;
        REQUIRE_GIT(git_clone(&raw, utf8(fx.remote()).c_str(), utf8(b.notes).c_str(), nullptr));
        git_repository_free(raw);
    }
    write_file(b.notes / "ayni.md", "taban\n");
    write_file(b.notes / "degisik.md", "B'nin eski düzenlemesi\n");
    {
        NoteStore store = b.store();
        std::vector<fs::path> logs;
        for (const auto& entry : fs::directory_iterator(b.notes / "graph"))
            logs.push_back(entry.path());
        REQUIRE(core::ingest_note_logs(store, logs).has_value());
    }
    const auto migrated = core::migrate_legacy_notes(b.notes, b.home / "data");
    REQUIRE_MESSAGE(migrated.has_value(), describe(migrated));
    CHECK(migrated->complete);
    CHECK(migrated->conflict_copies.size() == 1);
    CHECK(b.body("ayni.md") == "A düzenledi\n");  // bayat dosya A'yı ezmedi, kopya da doğurmadı
    CHECK(b.body("degisik.md") == "taban\n");
    CHECK(b.body(migrated->conflict_copies.front().string()) == "B'nin eski düzenlemesi\n");
}

// Depoda kalan düz dosyaya git'le dokunan olursa birleştirme hâlâ kayıpsızdır: yerel sürüm yerinde, uzak
// sürüm kopyada.
TEST_CASE("old files changed on both sides are merged by git without loss and the sync goes on") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "x\n");
    must_sync(a);
    commit_entries(fx.remote(), {{".eski", "ortak\n"}});
    must_sync(a);
    REQUIRE(read_file(a.notes / ".eski") == "ortak\n");
    commit_entries(fx.remote(), {{".eski", "uzak\n"}});
    write_file(a.notes / ".eski", "yerel\n");
    {
        const Repository repo = open_repository(a.notes);
        git_index* index = nullptr;
        REQUIRE_GIT(git_repository_index(&index, repo.get()));
        REQUIRE_GIT(git_index_add_bypath(index, ".eski"));
        REQUIRE_GIT(git_index_write(index));
        git_index_free(index);
    }
    a.put("iki.md", "y\n");  // yerel commit'i tetikler: staged .eski de girer
    const SyncResult merged = must_sync(a);
    CHECK(merged.pushed);
    CHECK(read_file(a.notes / ".eski") == "yerel\n");
    bool copy_found = false;
    for (const auto& entry : fs::directory_iterator(a.notes))
        if (utf8(entry.path().filename()).starts_with(".eski (conflict "))
            copy_found = read_file(entry.path()) == "uzak\n";
    CHECK(copy_found);
    CHECK(head_commit(a.notes).parents == 2);
}

// graph.db kaybolunca cihaz yeni kimlik alır: eski cihazın gönderilen numarası bu cihazın yazmalarını
// "gönderildi" gösteremez.
TEST_CASE("after a lost graph.db the new device identity starts from nothing pushed") {
    const SyncFixture fx;
    const Device a(fx, "A");
    for (const char* name : {"bir.md", "iki.md", "uc.md", "dort.md"})
        a.put(name, "x\n");
    must_sync(a);
    fs::remove_all(a.home / "data");
    must_sync(a);
    a.put("yeni.md", "gönderilmedi\n");
    const EnvGuard home("NOTEAPP_HOME", a.home);
    core::Config config = a.config();
    config.sync.enabled = true;
    std::vector<core::NoteMeta> notes = a.store().list_meta();
    REQUIRE(noteapp::sync::fill_sync_status(config, notes).has_value());
    const auto fresh = std::ranges::find_if(
        notes, [](const core::NoteMeta& note) { return note.path.string() == "yeni.md"; });
    REQUIRE(fresh != notes.end());
    CHECK(fresh->sync_status == SyncState::New);
}

namespace {

void clone_into(const SyncFixture& fx, const Device& device) {
    git_repository* raw = nullptr;
    REQUIRE_GIT(git_clone(&raw, utf8(fx.remote()).c_str(), utf8(device.notes).c_str(), nullptr));
    git_repository_free(raw);
}

// Eski sürüm: düz dosyalar git'te, uzakta.
void old_version_commits(const Device& device) {
    const Repository repo = open_repository(device.notes);
    git_index* index = nullptr;
    REQUIRE_GIT(git_repository_index(&index, repo.get()));
    REQUIRE_GIT(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr));
    REQUIRE_GIT(git_index_write(index));
    git_oid tree_id;
    REQUIRE_GIT(git_index_write_tree(&tree_id, index));
    git_index_free(index);
    git_tree* tree = nullptr;
    REQUIRE_GIT(git_tree_lookup(&tree, repo.get(), &tree_id));
    git_signature* who = nullptr;
    REQUIRE_GIT(git_signature_now(&who, "Eski", "eski@noteapp.invalid"));
    git_oid commit_id;
    REQUIRE_GIT(
        git_commit_create(&commit_id, repo.get(), "HEAD", who, who, nullptr, "eski surum", tree, 0, nullptr));
    git_signature_free(who);
    git_tree_free(tree);
    git_remote* origin = nullptr;
    REQUIRE_GIT(git_remote_lookup(&origin, repo.get(), "origin"));
    char refspec[] = "refs/heads/main:refs/heads/main";
    char* refspecs[] = {refspec};
    const git_strarray list{refspecs, 1};
    REQUIRE_GIT(git_remote_push(origin, &list, nullptr));
    git_remote_free(origin);
}

}  // namespace

// Gerçek sıra: B senkrondan önce geçmek zorunda (kapı), A'nın düzenlemesini görmeden eski metni içe aktarır.
// B'nin saati daha geç olsa da bayat içerik gerçek düzenlemeyi geçemez.
TEST_CASE(
    "real migration order: A migrates, edits, syncs; B migrates its old text and syncs; A's edit stays") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B", false);
    write_file(a.notes / "not.md", "eski metin\n");
    write_file(a.notes / "dokunulmayan.md", "aynı\n");
    old_version_commits(a);
    clone_into(fx, b);

    REQUIRE(core::migrate_legacy_notes(a.notes, a.home / "data").has_value());
    a.put("not.md", "A'nın düzenlemesi\n");
    must_sync(a);

    std::this_thread::sleep_for(
        std::chrono::milliseconds(20));  // B'nin saati A'nın düzenlemesinden kesin sonra
    CHECK(b.sync().error().code == core::Error::Code::MigrationRequired);
    REQUIRE(core::migrate_legacy_notes(b.notes, b.home / "data").has_value());
    must_sync(b);
    must_sync(a);
    must_sync(b);

    for (const Device* device : {&a, &b}) {
        CHECK(device->body("not.md") == "A'nın düzenlemesi\n");
        CHECK(paths_of(*device) == std::vector<std::string>{"dokunulmayan.md", "not.md"});
    }
}

TEST_CASE(
    "real migration order: B's old file really differs: A's edit stays and B's text survives as a copy") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B", false);
    write_file(a.notes / "not.md", "eski metin\n");
    old_version_commits(a);
    clone_into(fx, b);
    write_file(b.notes / "not.md", "B'nin göndermediği düzenleme\n");

    REQUIRE(core::migrate_legacy_notes(a.notes, a.home / "data").has_value());
    a.put("not.md", "A'nın düzenlemesi\n");
    must_sync(a);
    REQUIRE(core::migrate_legacy_notes(b.notes, b.home / "data").has_value());
    must_sync(b);
    must_sync(a);
    must_sync(b);

    for (const Device* device : {&a, &b}) {
        CHECK(device->body("not.md") == "A'nın düzenlemesi\n");
        const auto paths = paths_of(*device);
        REQUIRE(paths.size() == 2);
        const auto copy = std::ranges::find_if(paths, is_copy);
        REQUIRE(copy != paths.end());
        CHECK(device->body(*copy) == "B'nin göndermediği düzenleme\n");
    }
}

// Yeni kurulum eski düzen değildir: uzaktan gelen tek dosya yazmayı kilitleyemez, not da sayılmaz.
TEST_CASE("a plain file arriving from the remote does not trigger migration on a device that had none") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("gercek.md", "x\n");
    must_sync(a);
    const Device b(fx, "B", false);
    REQUIRE(b.enable(utf8(fx.remote())).has_value());
    must_sync(b);
    commit_entries(fx.remote(), {{"zararli.md", "uzaktan\n"}});
    must_sync(b);  // dosya şimdi çalışma ağacında
    CHECK(read_file(b.notes / "zararli.md") == "uzaktan\n");
    b.put("yerel.md", "y\n");
    const auto again = b.sync();
    REQUIRE_MESSAGE(again.has_value(), describe(again));
    {
        const EnvGuard home("NOTEAPP_HOME", b.home);
        NoteStore gated = NoteStore::for_config(b.config());
        CHECK_FALSE(gated.health().has_value());
        CHECK(gated.create(core::NewNote{"", "n", "md", "x"}).has_value());
    }
    CHECK_FALSE(b.store().read(np("zararli.md")).has_value());
}

TEST_CASE("a clone over an empty folder with remote plain files is not old layout either") {
    const SyncFixture fx;
    commit_entries(fx.remote(), {{"eski.md", "eski\n"}});
    const Device b(fx, "B", false);
    REQUIRE(b.enable(utf8(fx.remote())).has_value());
    const auto synced = b.sync();
    REQUIRE_MESSAGE(synced.has_value(), describe(synced));
}

TEST_CASE("a remote commit replacing the graph folder with a file is reported clearly and notes are kept") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "x\n");
    must_sync(a);
    commit_entries(fx.remote(), {{"graph", "artık dosya\n"}});
    const auto first = a.sync();
    if (first.has_value()) {
        const auto second = a.sync();
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().message_key == "sync.log_dir_not_regular");
    } else {
        CHECK(first.error().message_key == "sync.log_dir_not_regular");
    }
    CHECK(a.body("not.md") == "x\n");
    const auto status = a.status();
    REQUIRE(status.last_error.has_value());
    CHECK(status.last_error->message_key == "sync.log_dir_not_regular");
}

TEST_CASE("an error text with invalid UTF-8 does not corrupt the status file or reset pushed_seq") {
    const SyncFixture fx;
    const Device a(fx, "A");
    a.put("not.md", "x\n");
    must_sync(a);
    const std::uint64_t pushed = a.status().pushed_seq;
    REQUIRE(pushed > 0);
    {
        const EnvGuard home("NOTEAPP_HOME", a.home);
        noteapp::sync::SyncService(a.config())
            .record_failure(core::Error{core::Error::Code::Io,
                                        "sync.push_rejected",
                                        {{"branch", "main"}, {"detail", "kötü\xFF\xFE\x1B[31m"}}});
    }
    const auto status = a.status();
    CHECK(status.pushed_seq == pushed);
    CHECK(status.last_success.has_value());
    REQUIRE(status.last_error.has_value());
    CHECK(status.last_error->message_key == "sync.push_rejected");
}

// ---- satır düzeyi birleştirme

TEST_CASE("non-overlapping edits merge into one note on both devices, CRLF and BOM byte-exact, no copy") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    const std::string lines = "\xEF\xBB\xBF"
                              "bir\r\niki\nüç\r\ndört\nbeş\r\nalti\nyedi\r\n";
    a.put("not.md", lines);
    exchange(a, b);
    a.put("not.md", "\xEF\xBB\xBF"
                    "BIR\r\niki\nüç\r\ndört\nbeş\r\nalti\nyedi\r\n");
    b.put("not.md", "\xEF\xBB\xBF"
                    "bir\r\niki\nüç\r\ndört\nbeş\r\nalti\nYEDI\r\n");
    exchange(a, b);
    const std::string merged = "\xEF\xBB\xBF"
                               "BIR\r\niki\nüç\r\ndört\nbeş\r\nalti\nYEDI\r\n";
    for (const Device* device : {&a, &b}) {
        CHECK(device->body("not.md") == merged);
        CHECK(paths_of(*device) == std::vector<std::string>{"not.md"});
    }
}

TEST_CASE("overlapping edits, binary and invalid UTF-8 fall back to the copy with both bytes") {
    const SyncFixture fx;
    const Device a(fx, "A");
    const Device b(fx, "B");
    std::string base = "bir\niki\nüç\n";
    std::string on_a = "BİR\niki\nüç\n";
    std::string on_b = "1\niki\nüç\n";
    SUBCASE("same line") {}
    SUBCASE("binary") {
        base += std::string("\0x\n", 3);
        on_a = "A\n" + base;
        on_b = base + "B\n";
    }
    SUBCASE("invalid UTF-8") {
        base += "\xFF\n";
        on_a = "A\n" + base;
        on_b = base + "B\n";
    }
    a.put("not.md", base);
    exchange(a, b);
    a.put("not.md", on_a);
    b.put("not.md", on_b);
    exchange(a, b);
    const auto paths = paths_of(a);
    CHECK(paths_of(b) == paths);
    REQUIRE(paths.size() == 2);
    std::set<std::string> bodies;
    for (const std::string& path : paths) {
        CHECK(a.body(path) == b.body(path));
        bodies.insert(a.body(path));
    }
    CHECK(bodies == std::set<std::string>{on_a, on_b});
}

namespace {

// Gerçek günlükler, rastgele teslim sırası: git'siz, çünkü sıra denetlenir. Her cihaz çözer, birleşen de yeni
// çerçevedir.
struct Mesh {
    TempDir logs;
    std::vector<std::unique_ptr<TempDir>> dirs;
    std::vector<NoteStore> stores;

    explicit Mesh(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            dirs.push_back(std::make_unique<TempDir>());
            stores.emplace_back(dirs.back()->path());
        }
    }
    fs::path log_of(std::size_t i) {
        REQUIRE(core::export_note_log(stores[i], logs.path()).has_value());
        return logs.path() / *core::own_log_name(stores[i]);
    }
    void deliver(std::size_t from, std::size_t to) {
        const std::vector<fs::path> files{log_of(from)};
        const auto got = core::ingest_note_logs(stores[to], files, noteapp::sync::merge_text);
        REQUIRE_MESSAGE(got.has_value(), describe(got));
    }
    void shuffle_rounds(std::mt19937& rng, int rounds) {
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        for (std::size_t i = 0; i < stores.size(); ++i)
            for (std::size_t j = 0; j < stores.size(); ++j)
                if (i != j)
                    pairs.emplace_back(i, j);
        for (int round = 0; round < rounds; ++round) {
            std::ranges::shuffle(pairs, rng);
            for (const auto& [from, to] : pairs)
                deliver(from, to);
        }
    }
    void edit(std::size_t i, const std::string& body) {
        const auto path = np("not.md");
        const auto current = stores[i].read(path);
        REQUIRE(current.has_value());
        REQUIRE(stores[i].write(path, body, current->revision).has_value());
    }
};

const char* const kTen = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n";

}  // namespace

TEST_CASE("same merge on both devices at once is byte-identical and settles, in any delivery order") {
    for (const unsigned seed : {1U, 2U, 3U, 4U, 5U}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Mesh mesh(2);
        REQUIRE(noteapp::test::seed_note(mesh.stores[0], "not.md", kTen));
        mesh.shuffle_rounds(rng, 2);
        mesh.edit(0, "A\n1\n2\n3\n4\n5\n6\n7\n8\n9\n");
        mesh.edit(1, "0\n1\n2\n3\n4\n5\n6\n7\n8\nB\n");
        // İkisi de birbirini aynı anda alır ve aynı sonucu yazar.
        mesh.deliver(0, 1);
        mesh.deliver(1, 0);
        const std::string merged = "A\n1\n2\n3\n4\n5\n6\n7\n8\nB\n";
        CHECK(mesh.stores[0].read(np("not.md"))->text == merged);
        CHECK(mesh.stores[1].read(np("not.md"))->text == merged);
        mesh.shuffle_rounds(rng, 2);
        const auto settled = core::own_last_seq(mesh.stores[0]);
        mesh.shuffle_rounds(rng, 2);
        CHECK(*core::own_last_seq(mesh.stores[0]) == *settled);  // birleştirme kendini beslemez
        for (NoteStore& store : mesh.stores) {
            CHECK(store.list().size() == 1);
            CHECK(store.read(np("not.md"))->text == merged);
        }
    }
}

TEST_CASE("three devices edit different lines and converge on one note in random delivery order") {
    for (const unsigned seed : {11U, 12U, 13U, 14U, 15U, 16U}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Mesh mesh(3);
        REQUIRE(noteapp::test::seed_note(mesh.stores[0], "not.md", kTen));
        mesh.shuffle_rounds(rng, 2);
        mesh.edit(0, "A\n1\n2\n3\n4\n5\n6\n7\n8\n9\n");
        mesh.edit(1, "0\n1\n2\n3\nB\n5\n6\n7\n8\n9\n");
        mesh.edit(2, "0\n1\n2\n3\n4\n5\n6\n7\n8\nC\n");
        mesh.shuffle_rounds(rng, 4);
        const std::string merged = "A\n1\n2\n3\nB\n5\n6\n7\n8\nC\n";
        for (NoteStore& store : mesh.stores) {
            CHECK(store.read(np("not.md"))->text == merged);
            CHECK(store.list().size() == 1);
        }
    }
}

TEST_CASE("no common ancestor: two separately migrated different files stay as note and copy") {
    Mesh mesh(2);
    const core::ImportedNote on_a{np("not.md"), "a\nortak\n"};
    const core::ImportedNote on_b{np("not.md"), "ortak\nb\n"};
    REQUIRE(mesh.stores[0].import_notes(std::span(&on_a, 1)).has_value());
    REQUIRE(mesh.stores[1].import_notes(std::span(&on_b, 1)).has_value());
    std::mt19937 rng(7);
    mesh.shuffle_rounds(rng, 3);
    CHECK(mesh.stores[0].list().size() == 2);
    CHECK(mesh.stores[1].list().size() == 2);
}

// git_merge_file yazma kilidi altında koşar: büyük girdi birleşmez, çakışma kopyasına düşer.
TEST_CASE("merge_text: an input over the size limit gives nothing, one at the limit still merges") {
    using noteapp::sync::merge_text;
    const noteapp::sync::GitRuntime runtime;  // libgit2'nin ayırıcısı: test tek başına da koşabilsin
    constexpr std::size_t kLimit = std::size_t{4} * 1024U * 1024U;
    const auto versions = [](std::size_t total) {
        std::string middle;
        for (int line = 0; middle.size() + 5 < total; ++line)
            middle += "satir " + std::to_string(line) + "\n";
        middle.resize(total - 5);
        middle += '\n';
        return std::array{"a\n" + middle + "e\n", "A\n" + middle + "e\n", "a\n" + middle + "E\n"};
    };
    const auto at_limit = versions(kLimit);
    REQUIRE(at_limit[0].size() == kLimit);
    CHECK(merge_text(at_limit[0], at_limit[1], at_limit[2]).has_value());
    const auto over = versions(kLimit + 1);
    REQUIRE(over[0].size() == kLimit + 1);
    CHECK_FALSE(merge_text(over[0], over[1], over[2]).has_value());
}

TEST_CASE("merge_text: clean merge keeps bytes, overlap gives nothing") {
    using noteapp::sync::merge_text;
    const SyncFixture fx;
    CHECK(merge_text("a\r\nb\nc\nd\ne\n", "A\r\nb\nc\nd\ne\n", "a\r\nb\nc\nd\nE\n") == "A\r\nb\nc\nd\nE\n");
    CHECK_FALSE(merge_text("a\n", "b\n", "c\n").has_value());
    CHECK(merge_text("", "x\n", "x\n") == "x\n");
}
