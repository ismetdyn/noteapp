// NoteStore'un graph üzerindeki davranışı (G4): bayt koruma, revizyon, deleted_at, klasör düğümleri,
// [[bağlantı]], eşzamanlı taşıma okuma kuralı. Bir cihaz = bir dizin + bir graph.db; iki bağlantı aynı
// dizinde iki süreç gibidir.
#include "core/config.hpp"
#include "core/file_stat.hpp"
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "core/paths.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp::core;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;
namespace graph = noteapp::graph;

namespace {

NotePath np(const std::string& text) {
    return *NotePath::parse(text);
}

std::string exact_bytes() {
    std::string bytes = "\xEF\xBB\xBF"
                        "ilk\r\nikinci\n";
    bytes += '\0';
    bytes += "\xFF\xFE geçersiz \x80";
    return bytes;
}

std::vector<std::string> paths_of(const NoteStore& store) {
    std::vector<std::string> paths;
    for (const NoteInfo& info : store.list())
        paths.push_back(info.path.string());
    std::ranges::sort(paths);
    return paths;
}

// İki cihaz: gerçek günlük dosyalarıyla el ele. Git G5'te; burada günlük dizini paylaşılır.
struct Device {
    TempDir data;
    NoteStore store{data.path()};
};

struct Pair {
    TempDir logs;
    Device a;
    Device b;

    void exchange_once(Device& from, Device& to) {
        REQUIRE(export_note_log(from.store, logs.path()).has_value());
        const auto name = own_log_name(from.store);
        REQUIRE(name.has_value());
        const std::vector<fs::path> files{logs.path() / *name};
        const auto ingested = ingest_note_logs(to.store, files);
        REQUIRE(ingested.has_value());
    }
    void sync() {
        exchange_once(a, b);
        exchange_once(b, a);
        exchange_once(a, b);
    }
};

}  // namespace

TEST_CASE("notes live in graph.db and leave no file tree") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(store.create(NewNote{"a/b", "plan", "md", "x"}).has_value());
    CHECK(fs::exists(dir.path() / "graph.db"));
    CHECK_FALSE(fs::exists(dir.path() / "a"));
    CHECK_FALSE(fs::exists(dir.path() / ".keep"));
}

TEST_CASE("a store never creates the database just to read") {
    const TempDir dir;
    const NoteStore store(dir.path());
    CHECK(store.list().empty());
    CHECK(store.folders().empty());
    CHECK_FALSE(store.health().has_value());
    CHECK_FALSE(fs::exists(dir.path() / "graph.db"));
}

TEST_CASE("bodies round-trip byte for byte: CRLF, BOM, NUL, invalid UTF-8, large") {
    const TempDir dir;
    NoteStore store(dir.path());
    const std::string bytes = exact_bytes();
    const auto created = store.create_exact(NewNote{"", "ham", "bin", bytes});
    REQUIRE(created.has_value());
    const auto read = store.read(created->path);
    REQUIRE(read.has_value());
    CHECK(read->text == bytes);

    const std::string large = bytes + std::string(10U * 1024U * 1024U, 'x') + bytes;
    const auto written = store.write(created->path, large, read->revision);
    REQUIRE(written.has_value());
    CHECK(store.read(created->path)->text == large);
    CHECK(store.list().front().size == large.size());
    CHECK(looks_binary(read->text));
}

TEST_CASE("a second connection's write trips the revision check and changes nothing") {
    const TempDir dir;
    NoteStore mine(dir.path());
    NoteStore other(dir.path());
    const auto created = mine.create_exact(NewNote{"", "n", "md", "ilk"});
    REQUIRE(created.has_value());
    const auto seen = mine.read(created->path);
    const auto theirs = other.read(created->path);
    REQUIRE(theirs.has_value());
    REQUIRE(other.write(created->path, "onların", theirs->revision).has_value());

    const auto mine_write = mine.write(created->path, "benim", seen->revision);
    REQUIRE_FALSE(mine_write.has_value());
    CHECK(mine_write.error().code == Error::Code::ModifiedExternally);
    CHECK(mine.read(created->path)->text == "onların");

    // Taze revizyonla yazmak mümkündür: çakışma diyaloğunun "yine de kaydet" yolu.
    const auto fresh = mine.read(created->path);
    REQUIRE(mine.write(created->path, "benim", fresh->revision).has_value());
    CHECK(other.read(created->path)->text == "benim");
}

TEST_CASE("writing identical bytes adds no operation and keeps the revision") {
    const TempDir dir;
    const TempDir logs;
    NoteStore store(dir.path());
    const auto created = store.create_exact(NewNote{"", "n", "md", "aynı"});
    REQUIRE(created.has_value());
    REQUIRE(export_note_log(store, logs.path()).has_value());
    const auto log = logs.path() / *own_log_name(store);
    const auto before = fs::file_size(log);

    const auto same = store.write(created->path, "aynı", created->revision);
    REQUIRE(same.has_value());
    CHECK(*same == created->revision);
    REQUIRE(export_note_log(store, logs.path()).has_value());
    CHECK(fs::file_size(log) == before);

    const auto changed = store.write(created->path, "farklı", created->revision);
    REQUIRE(changed.has_value());
    CHECK(*changed != created->revision);
    REQUIRE(export_note_log(store, logs.path()).has_value());
    CHECK(fs::file_size(log) > before);
}

TEST_CASE("created_at and origin are real values; move keeps them and the body") {
    const TempDir dir;
    NoteStore store(dir.path());
    const auto when = std::chrono::system_clock::now() - std::chrono::hours(3);
    REQUIRE(store.create(NewNote{"", "n", "md", "x"}, when).has_value());
    const auto metas = store.list_meta();
    REQUIRE(metas.size() == 1);
    REQUIRE(metas[0].created_at.has_value());
    const auto created_sys = std::chrono::file_clock::to_sys(*metas[0].created_at);
    CHECK(std::chrono::abs(created_sys - when) < std::chrono::seconds(1));
    REQUIRE(metas[0].origin.has_value());
#if defined(_WIN32)
    CHECK(*metas[0].origin == "windows");
#elif defined(__APPLE__)
    CHECK(*metas[0].origin == "macos");
    REQUIRE(metas[0].origin.has_value());
#else
    CHECK(*metas[0].origin == "linux");
#endif
    REQUIRE(store.move(np("n.md"), np("alt/m.txt")).has_value());
    const auto moved = store.list_meta();
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].path.string() == "alt/m.txt");
    CHECK(moved[0].created_at == metas[0].created_at);
    CHECK(moved[0].updated_at == metas[0].updated_at);
    CHECK(store.read(np("alt/m.txt"))->text == "x");
}

TEST_CASE("folder rename is one write: notes below follow without being touched") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(store.create(NewNote{"a/b", "n", "md", "x"}).has_value());
    const auto revision = store.list().front().revision;
    REQUIRE(store.rename_folder("a", "c").has_value());
    CHECK(paths_of(store) == std::vector<std::string>{"c/b/n.md"});
    CHECK(store.list().front().revision == revision);
    CHECK(store.folders() == std::vector<std::string>{"c", "c/b"});
}

TEST_CASE("a recreated folder name after a rename and after a delete is a working folder") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(store.create_folder("work").has_value());
    REQUIRE(store.rename_folder("work", "job").has_value());
    REQUIRE(store.create_folder("work").has_value());
    CHECK(store.folders() == std::vector<std::string>{"job", "work"});
    REQUIRE(store.remove_folder("work", false).has_value());
    REQUIRE(store.create_folder("work").has_value());
    REQUIRE(store.create(NewNote{"work", "n", "md", "x"}).has_value());
    CHECK(paths_of(store) == std::vector<std::string>{"work/n.md"});
}

TEST_CASE("empty folders are nodes and travel to another device") {
    Pair pair;
    REQUIRE(pair.a.store.create_folder("boş/alt").has_value());
    pair.sync();
    CHECK(pair.b.store.folders() == std::vector<std::string>{"boş", "boş/alt"});
}

TEST_CASE("trash keeps exact bytes and clears from the live list; restore brings them back") {
    const TempDir dir;
    NoteStore store(dir.path());
    const std::string bytes = exact_bytes();
    REQUIRE(seed_note(store, "work/n.bin", bytes));
    const auto item = store.trash(np("work/n.bin"));
    REQUIRE(item.has_value());
    CHECK(item->path.string() == "work/n.bin");
    CHECK(item->size == bytes.size());
    CHECK(store.list().empty());
    CHECK(store.folders() == std::vector<std::string>{"work"});
    CHECK(store.read(np("work/n.bin")).error().code == Error::Code::NotFound);
    REQUIRE(store.remove_folder("work", false).has_value());  // çöp klasör sayımına girmez
    const auto restored = store.restore(item->path);
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "work/n.bin");
    CHECK(store.read(*restored)->text == bytes);
    CHECK(store.list_trash()->empty());
}

TEST_CASE("an edit concurrent with a trash wins: the note stays live on both devices") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "n.md", "ilk"));
    pair.sync();
    REQUIRE(pair.a.store.trash(np("n.md")).has_value());
    const auto content = pair.b.store.read(np("n.md"));
    REQUIRE(content.has_value());
    REQUIRE(pair.b.store.write(np("n.md"), "düzenlendi", content->revision).has_value());
    pair.sync();
    for (const NoteStore* store : {&pair.a.store, &pair.b.store}) {
        CHECK(paths_of(*store) == std::vector<std::string>{"n.md"});
        CHECK(store->read(np("n.md"))->text == "düzenlendi");
        CHECK(store->list_trash()->empty());
    }
}

TEST_CASE("a trash that saw the edit wins") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "n.md", "ilk"));
    pair.sync();
    const auto content = pair.b.store.read(np("n.md"));
    REQUIRE(pair.b.store.write(np("n.md"), "düzenlendi", content->revision).has_value());
    pair.sync();
    REQUIRE(pair.a.store.trash(np("n.md")).has_value());
    pair.sync();
    CHECK(pair.b.store.list().empty());
    CHECK(pair.b.store.list_trash()->size() == 1);
}

TEST_CASE("concurrent moves give one deterministic path everywhere") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "n.md", "x"));
    pair.sync();
    REQUIRE(pair.a.store.move(np("n.md"), np("a/n.md")).has_value());
    REQUIRE(pair.b.store.move(np("n.md"), np("b/n.md")).has_value());
    pair.sync();
    const auto on_a = paths_of(pair.a.store);
    REQUIRE(on_a.size() == 1);
    CHECK(paths_of(pair.b.store) == on_a);
    CHECK((on_a[0] == "a/n.md" || on_a[0] == "b/n.md"));
    CHECK(pair.b.store.read(np(on_a[0]))->text == "x");
    // Sonraki taşıma iki kenarı da temizler.
    REQUIRE(pair.b.store.move(np(on_a[0]), np("c/n.md")).has_value());
    pair.sync();
    CHECK(paths_of(pair.a.store) == std::vector<std::string>{"c/n.md"});
}

TEST_CASE("a CHILD_OF cycle from concurrent folder moves resolves the same way everywhere") {
    Pair pair;
    REQUIRE(pair.a.store.create_folder("p").has_value());
    REQUIRE(pair.a.store.create_folder("q").has_value());
    REQUIRE(seed_note(pair.a.store, "p/n.md", "x"));
    pair.sync();
    REQUIRE(pair.a.store.rename_folder("p", "q/p").has_value());
    REQUIRE(pair.b.store.rename_folder("q", "p/q").has_value());
    pair.sync();
    const auto folders = pair.a.store.folders();
    CHECK(pair.b.store.folders() == folders);
    CHECK(folders.size() == 2);
    const auto notes = paths_of(pair.a.store);
    CHECK(paths_of(pair.b.store) == notes);
    REQUIRE(notes.size() == 1);
    CHECK(pair.b.store.read(np(notes[0]))->text == "x");
}

TEST_CASE("two notes created under one path both stay, the second takes -2 on every device") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "n.md", "a"));
    REQUIRE(seed_note(pair.b.store, "n.md", "b"));
    pair.sync();
    const auto paths = paths_of(pair.a.store);
    CHECK(paths == std::vector<std::string>{"n-2.md", "n.md"});
    CHECK(paths_of(pair.b.store) == paths);
    CHECK(pair.a.store.read(np("n.md"))->text == pair.b.store.read(np("n.md"))->text);
    CHECK(pair.a.store.read(np("n-2.md"))->text == pair.b.store.read(np("n-2.md"))->text);
}

TEST_CASE("the same folder created on two devices is one node") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "ortak/a.md", "a"));
    REQUIRE(seed_note(pair.b.store, "ortak/b.md", "b"));
    pair.sync();
    CHECK(pair.a.store.folders() == std::vector<std::string>{"ortak"});
    CHECK(paths_of(pair.b.store) == std::vector<std::string>{"ortak/a.md", "ortak/b.md"});
    REQUIRE(pair.a.store.rename_folder("ortak", "yeni").has_value());
    pair.sync();
    CHECK(paths_of(pair.b.store) == std::vector<std::string>{"yeni/a.md", "yeni/b.md"});
}

TEST_CASE("a hostile name from another device never becomes a path and is not listed") {
    const TempDir victim_dir;
    const TempDir logs;
    Device attacker;
    NoteStore victim(victim_dir.path());
    {
        auto opened = graph::Graph::open(attacker.data.path() / "graph.db");
        REQUIRE(opened.has_value());
        auto txn = opened->write();
        REQUIRE(txn.has_value());
        for (const char* name : {"../../etc/passwd", "CON", ".git", "a/b", "x\\y"}) {
            const graph::NodeId id = graph::new_node_id();
            REQUIRE(txn->put_node(id).has_value());
            REQUIRE(txn->add_label(id, "Note").has_value());
            REQUIRE(txn->set_property(id, "name", graph::Value{std::string(name)}).has_value());
            REQUIRE(txn->set_property(id, "type", graph::Value{std::string("md")}).has_value());
            REQUIRE(txn->set_property(id, "body", graph::Value{graph::Bytes{"ele geçirildi"}}).has_value());
        }
        REQUIRE(txn->commit().has_value());
    }
    REQUIRE(seed_note(victim, "yerel.md", "kalır"));
    REQUIRE(export_note_log(attacker.store, logs.path()).has_value());
    const std::vector<fs::path> files{logs.path() / *own_log_name(attacker.store)};
    REQUIRE(ingest_note_logs(victim, files).has_value());
    const auto paths = paths_of(victim);
    CHECK(std::ranges::find(paths, "yerel.md") != paths.end());
    for (const std::string& path : paths) {
        CHECK(path.find("..") == std::string::npos);
        CHECK_FALSE(path.starts_with(".git"));
    }
}

TEST_CASE("[[links]] become LINKS_TO edges in the same write and follow edits") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "work/target.md", "hedef"));
    REQUIRE(seed_note(store, "other.md", "öteki"));
    REQUIRE(seed_note(store, "source.md", "bak [[target]] ve [[work/target.md|takma]] ve [[yok]]"));
    const auto count_links = [&] {
        auto opened = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(opened.has_value());
        const auto result =
            opened->query("MATCH (a:Note)-[:LINKS_TO]->(b:Note) RETURN a.name, b.name ORDER BY b.name");
        REQUIRE(result.has_value());
        std::vector<std::string> links;
        for (const auto& row : result->rows)
            links.push_back(std::get<std::string>(row[0].v) + "->" + std::get<std::string>(row[1].v));
        return links;
    };
    CHECK(count_links() == std::vector<std::string>{"source->target"});
    const auto content = store.read(np("source.md"));
    REQUIRE(store.write(np("source.md"), "artık [[other]]", content->revision).has_value());
    CHECK(count_links() == std::vector<std::string>{"source->other"});
}

TEST_CASE("edit_in_temp_file: private file, saved through the conflict check, removed after") {
    const TempDir dir;
    const TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "n.md", exact_bytes()));
    fs::path used;
    const auto changed =
        store.edit_in_temp_file(np("n.md"), [&](const fs::path& file) -> std::expected<void, Error> {
            used = file;
            CHECK(noteapp::test::slurp(file) == exact_bytes());
#ifndef _WIN32
            CHECK(fs::status(file).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
#endif
            std::ofstream(file, std::ios::binary) << "yeni";
            return {};
        });
    REQUIRE(changed.has_value());
    CHECK(*changed);
    CHECK_FALSE(fs::exists(used));
    CHECK(store.read(np("n.md"))->text == "yeni");

    const auto untouched =
        store.edit_in_temp_file(np("n.md"), [](const fs::path&) -> std::expected<void, Error> { return {}; });
    REQUIRE(untouched.has_value());
    CHECK_FALSE(*untouched);
}

TEST_CASE("edit_in_temp_file keeps the temp file and names it when another writer got there first") {
    const TempDir dir;
    const TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    NoteStore store(dir.path());
    NoteStore other(dir.path());
    REQUIRE(seed_note(store, "n.md", "ilk"));
    fs::path used;
    const auto result =
        store.edit_in_temp_file(np("n.md"), [&](const fs::path& file) -> std::expected<void, Error> {
            used = file;
            const auto theirs = other.read(np("n.md"));
            REQUIRE(other.write(np("n.md"), "onların", theirs->revision).has_value());
            std::ofstream(file, std::ios::binary) << "benim";
            return {};
        });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::ModifiedExternally);
    CHECK(result.error().message_key == "error.edit_kept");
    CHECK(noteapp::test::slurp(used) == "benim");
    CHECK(store.read(np("n.md"))->text == "onların");
    fs::remove(used);
}

TEST_CASE("another connection's commit is seen by list without any write of ours") {
    const TempDir dir;
    NoteStore reader(dir.path());
    NoteStore writer(dir.path());
    REQUIRE(seed_note(writer, "a.md", "x"));
    CHECK(paths_of(reader) == std::vector<std::string>{"a.md"});
    REQUIRE(seed_note(writer, "b.md", "y"));
    CHECK(paths_of(reader) == std::vector<std::string>{"a.md", "b.md"});
}

TEST_CASE("concurrent body writes become one copy with both bytes kept, and the devices converge") {
    Pair pair;
    REQUIRE(seed_note(pair.a.store, "n.md", "ilk"));
    pair.sync();
    const auto on_a = pair.a.store.read(np("n.md"));
    const auto on_b = pair.b.store.read(np("n.md"));
    const std::string a_bytes = std::string("a\r\n\0a", 5);
    const std::string b_bytes = std::string("b\xFF", 2);
    REQUIRE(pair.a.store.write(np("n.md"), a_bytes, on_a->revision).has_value());
    REQUIRE(pair.b.store.write(np("n.md"), b_bytes, on_b->revision).has_value());
    pair.sync();
    pair.sync();
    const auto paths = paths_of(pair.a.store);
    CHECK(paths_of(pair.b.store) == paths);
    REQUIRE(paths.size() == 2);
    std::vector<std::string> bodies;
    for (const std::string& path : paths) {
        CHECK(pair.a.store.read(np(path))->text == pair.b.store.read(np(path))->text);
        bodies.push_back(pair.a.store.read(np(path))->text);
    }
    std::ranges::sort(bodies);
    std::vector<std::string> expected{a_bytes, b_bytes};
    std::ranges::sort(expected);
    CHECK(bodies == expected);
    const auto is_copy = [](const std::string& path) {
        return path.find(" (conflict ") != std::string::npos;
    };
    CHECK(std::ranges::count_if(paths, is_copy) == 1);
    CHECK(std::ranges::find(paths, "n.md") != paths.end());
}

TEST_CASE("a local write first saves a losing head that is still unresolved") {
    // Önceki senkron ingest ile çözüm arasında öldüyse: head'ler durur, yerel yazma onları görüp ezmemeli.
    const TempDir logs;
    Device a;
    Device b;
    REQUIRE(seed_note(a.store, "n.md", "ilk"));
    REQUIRE(export_note_log(a.store, logs.path()).has_value());
    const std::vector<fs::path> a_log{logs.path() / *own_log_name(a.store)};
    REQUIRE(ingest_note_logs(b.store, a_log).has_value());
    const auto seen_b = b.store.read(np("n.md"));
    REQUIRE(b.store.write(np("n.md"), "b sürümü", seen_b->revision).has_value());
    const auto seen_a = a.store.read(np("n.md"));
    REQUIRE(a.store.write(np("n.md"), "a sürümü", seen_a->revision).has_value());
    REQUIRE(export_note_log(a.store, logs.path()).has_value());
    REQUIRE(export_note_log(b.store, logs.path()).has_value());
    {  // çözüm atlanır: motorla doğrudan ingest
        auto raw = graph::Graph::open(a.data.path() / "graph.db");
        REQUIRE(raw.has_value());
        const std::vector<fs::path> b_log{logs.path() / *own_log_name(b.store)};
        REQUIRE(raw->ingest_logs(b_log).has_value());
    }
    const auto current = a.store.read(np("n.md"));
    REQUIRE(a.store.write(np("n.md"), "son", current->revision).has_value());
    const auto notes = a.store.list();
    REQUIRE(notes.size() == 2);
    std::vector<std::string> bodies;
    for (const NoteInfo& info : notes)
        bodies.push_back(a.store.read(info.path)->text);
    std::ranges::sort(bodies);
    CHECK((bodies == std::vector<std::string>{"a sürümü", "son"} ||
           bodies == std::vector<std::string>{"b sürümü", "son"}));
}

#ifndef _WIN32
TEST_CASE("a skipped hostile name with control bytes reaches the log escaped") {
    const TempDir dir;
    const TempDir state;
    const noteapp::test::ScopedEnv home("NOTEAPP_HOME", noteapp::test::utf8(state.path()));
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        auto txn = raw->write();
        REQUIRE(txn.has_value());
        const graph::NodeId id = graph::new_node_id();
        REQUIRE(txn->put_node(id).has_value());
        REQUIRE(txn->add_label(id, "Note").has_value());
        REQUIRE(txn->set_property(id, "name", graph::Value{std::string("kotu\x1B[31m/x")}).has_value());
        REQUIRE(txn->set_property(id, "type", graph::Value{std::string("md")}).has_value());
        REQUIRE(txn->commit().has_value());
    }
    NoteStore store(dir.path());
    CHECK(store.list().empty());
    const std::string log = noteapp::test::slurp(state_dir() / "log.txt");
    CHECK(log.find("skipped") != std::string::npos);
    CHECK(log.find('\x1B') == std::string::npos);
}
#endif
