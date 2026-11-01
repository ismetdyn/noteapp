// Çöp kutusu (T55a; G4'ten beri Note.deleted_at). Silme yolu 🔴: hiçbir testte "yanlış şey silindi/ezildi"
// olmamalı.
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp::core;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

void put(NoteStore& store, std::string_view path, std::string_view bytes) {
    REQUIRE(seed_note(store, path, bytes));
}

NotePath np(std::string_view s) {
    return *NotePath::parse(s);
}

std::string body(const NoteStore& store, std::string_view path) {
    const auto content = store.read(np(path));
    return content ? content->text : std::string("<yok>");
}

std::vector<std::string> trash_paths(const NoteStore& store) {
    std::vector<std::string> paths;
    const auto items = store.list_trash();
    REQUIRE(items.has_value());
    for (const TrashItem& item : *items)
        paths.push_back(item.path.string());
    std::ranges::sort(paths);
    return paths;
}

using Paths = std::vector<std::string>;

}  // namespace

TEST_CASE("trash keeps the note's folder and exact bytes") {
    TempDir tmp;
    const std::string bytes = std::string("\xEF\xBB\xBFsatır\r\nnul:\0son", 19);
    NoteStore store(tmp.path());
    put(store, "work/plan.md", bytes);

    const auto item = store.trash(np("work/plan.md"));
    REQUIRE(item.has_value());
    CHECK(item->path.string() == "work/plan.md");
    CHECK(item->size == bytes.size());
    CHECK(store.read(np("work/plan.md")).error().code == Error::Code::NotFound);
    const auto restored = store.restore(item->path);
    REQUIRE(restored.has_value());
    CHECK(body(store, restored->string()) == bytes);
}

TEST_CASE("trash of a missing note or a folder is refused and changes nothing") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/x.md", "x");

    CHECK(store.trash(np("yok.md")).error().code == Error::Code::NotFound);
    CHECK_FALSE(store.trash(np("d")).has_value());
    CHECK(body(store, "d/x.md") == "x");
    CHECK(trash_paths(store).empty());
}

TEST_CASE("trash name collision uses a deterministic suffix and never overwrites") {
    TempDir tmp;
    NoteStore store(tmp.path());
    for (const std::string_view content : {"bir", "iki", "uc"}) {
        put(store, "d/a.tar.gz", content);
        REQUIRE(store.trash(np("d/a.tar.gz")).has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(trash_paths(store) == Paths{"d/a.tar-2.gz", "d/a.tar-3.gz", "d/a.tar.gz"});
    const auto restored = store.restore(np("d/a.tar-2.gz"));
    REQUIRE(restored.has_value());
    CHECK(body(store, restored->string()) == "iki");

    REQUIRE(store.create_exact({"", "uzantisiz", "txt", "1"}).has_value());
    REQUIRE(store.trash(np("uzantisiz.txt")).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(store.create_exact({"", "uzantisiz", "txt", "2"}).has_value());
    const auto second = store.trash(np("uzantisiz.txt"));
    REQUIRE(second.has_value());
    CHECK(second->path.string() == "uzantisiz-2.txt");
}

TEST_CASE("trash records the time of trashing") {
    TempDir tmp;
    NoteStore store(tmp.path());
    const auto long_ago = std::chrono::system_clock::now() - std::chrono::days(30);
    REQUIRE(store.create({"", "eski", "md", "içerik"}, long_ago).has_value());

    const auto before = fs::file_time_type::clock::now() - std::chrono::minutes(1);
    const auto item = store.trash(np("eski.md"));
    REQUIRE(item.has_value());
    CHECK(item->trashed_at >= before);

    const auto listed = store.list_trash();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().trashed_at >= before);
    CHECK(listed->front().size == std::string("içerik").size());
}

TEST_CASE("list_trash sorts newest first and is empty without any trash") {
    TempDir tmp;
    NoteStore store(tmp.path());
    CHECK(trash_paths(store).empty());

    put(store, "a.md", "a");
    put(store, "b.md", "b");
    REQUIRE(store.trash(np("a.md")).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(store.trash(np("b.md")).has_value());
    const auto items = store.list_trash();
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 2);
    CHECK(items->front().path.string() == "b.md");
}

TEST_CASE("trash is hidden from list, folders and folder deletion counts") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "work/a.md", "a");
    put(store, "work/b.md", "b");
    REQUIRE(store.trash(np("work/a.md")).has_value());
    REQUIRE(store.trash(np("work/b.md")).has_value());

    CHECK(store.list().empty());
    CHECK(store.folders() == Paths{"work"});
    // Çöpteki notlar klasörü dolu göstermez, silme de onlara dokunmaz.
    CHECK(store.remove_folder("work", false).has_value());
    CHECK(trash_paths(store) == Paths{"work/a.md", "work/b.md"});

    CHECK_FALSE(NotePath::parse(".trash").has_value());
    CHECK_FALSE(NotePath::parse(".trash/work/a.md").has_value());
    CHECK_FALSE(store.create_folder(".trash").has_value());
    CHECK_FALSE(store.remove_folder(".trash", true).has_value());
    CHECK_FALSE(store.rename_folder(".trash", "kurtulan").has_value());
    CHECK(trash_paths(store) == Paths{"work/a.md", "work/b.md"});
}

TEST_CASE("restore returns the note to its folder with exact bytes and recreates the folder") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a/b/n.md", "bayt\r\n");
    REQUIRE(store.trash(np("a/b/n.md")).has_value());
    REQUIRE(store.remove_folder("a", true).has_value());

    const auto restored = store.restore(np("a/b/n.md"));
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "a/b/n.md");
    CHECK(body(store, "a/b/n.md") == "bayt\r\n");
    CHECK(trash_paths(store).empty());
    CHECK(store.folders() == Paths{"a", "a/b"});
}

TEST_CASE("restore with a live name collision takes -2 and never overwrites") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "n.md", "eski");
    REQUIRE(store.trash(np("n.md")).has_value());
    put(store, "n.md", "yeni");

    const auto restored = store.restore(np("n.md"));
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "n-2.md");
    CHECK(body(store, "n.md") == "yeni");
    CHECK(body(store, "n-2.md") == "eski");
}

TEST_CASE("restore and purge of a missing item report NotFound") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "n.md", "canlı");
    CHECK(store.restore(np("n.md")).error().code == Error::Code::NotFound);
    CHECK(store.purge(np("n.md")).error().code == Error::Code::NotFound);
    CHECK(body(store, "n.md") == "canlı");  // aynı adlı canlı nota dokunulmaz
}

TEST_CASE("purge deletes only the given trash item") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/a.md", "a");
    put(store, "d/b.md", "b");
    REQUIRE(store.trash(np("d/a.md")).has_value());
    REQUIRE(store.trash(np("d/b.md")).has_value());
    put(store, "d/a.md", "canlı");

    REQUIRE(store.purge(np("d/a.md")).has_value());
    CHECK(trash_paths(store) == Paths{"d/b.md"});
    CHECK(body(store, "d/a.md") == "canlı");
}

TEST_CASE("empty_trash removes every trash item, keeps live notes and can repeat") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/a.md", "a");
    put(store, "kalan.md", "kalan");
    REQUIRE(store.trash(np("d/a.md")).has_value());

    REQUIRE(store.empty_trash().has_value());
    CHECK(trash_paths(store).empty());
    CHECK(body(store, "kalan.md") == "kalan");
    CHECK(store.empty_trash().has_value());
}

TEST_CASE("list_meta gives size, times and origin") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/a.md", "abc");
    const auto listed = store.list();
    const auto metas = store.list_meta();
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].path == listed[0].path);
    CHECK(metas[0].size == 3);
    CHECK(metas[0].updated_at == listed[0].mtime);
    CHECK(metas[0].created_at.has_value());
    CHECK(metas[0].origin.has_value());
    CHECK(metas[0].sync_status == SyncState::SyncOff);
}
