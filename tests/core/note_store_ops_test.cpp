// NoteStore taşı/sil/klasör işlemleri (T14). Silme yolu 🔴: hiçbir testte "yanlış şey silindi" olmamalı.
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "support/note_import.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <span>
#include <string>
#include <utility>
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

std::string arg(const Error& e, std::string_view key) {
    for (const auto& [k, v] : e.args) {
        if (k == key)
            return v;
    }
    return {};
}

std::vector<std::string> listed_paths(const NoteStore& store) {
    std::vector<std::string> paths;
    for (const auto& info : store.list())
        paths.push_back(info.path.string());
    std::sort(paths.begin(), paths.end());
    return paths;
}

using Paths = std::vector<std::string>;

}  // namespace

TEST_CASE("move renames a note, creates target folders and keeps the content") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a.md", "içerik");

    const auto moved = store.move(np("a.md"), np("work/2026/b.md"));
    REQUIRE(moved.has_value());
    CHECK(moved->string() == "work/2026/b.md");
    CHECK(listed_paths(store) == Paths{"work/2026/b.md"});
    CHECK(body(store, "work/2026/b.md") == "içerik");
}

TEST_CASE("move with a changed extension changes the note type") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a.md", "class A {}");

    REQUIRE(store.move(np("a.md"), np("a.java")).has_value());
    CHECK(listed_paths(store) == Paths{"a.java"});
}

TEST_CASE("move never overwrites an existing target") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a.md", "kaynak");
    put(store, "b.md", "hedef");

    const auto moved = store.move(np("a.md"), np("b.md"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().code == Error::Code::AlreadyExists);
    CHECK(arg(moved.error(), "path") == "b.md");
    CHECK(body(store, "a.md") == "kaynak");
    CHECK(body(store, "b.md") == "hedef");
}

TEST_CASE("move of a missing source is NotFound and creates no folders") {
    TempDir tmp;
    NoteStore store(tmp.path());

    const auto moved = store.move(np("yok.md"), np("yeni/yok.md"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().code == Error::Code::NotFound);
    CHECK(store.folders().empty());
}

TEST_CASE("move refuses a folder as source") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/x.md", "x");

    const auto moved = store.move(np("d"), np("e"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(listed_paths(store) == Paths{"d/x.md"});
}

TEST_CASE("a note can't get a path below another note") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a.md", "a");
    CHECK(store.create({"a.md", "b", "md", "x"}).error().code == Error::Code::Io);
    CHECK(store.create_folder("a.md").error().code == Error::Code::AlreadyExists);
    CHECK(store.create_folder("a.md/alt").error().code == Error::Code::Io);
    CHECK(listed_paths(store) == Paths{"a.md"});
}

TEST_CASE("remove deletes exactly the named note") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a.md", "a");
    put(store, "a-2.md", "a2");
    put(store, "d/a.md", "da");

    REQUIRE(store.remove(np("a.md")).has_value());
    CHECK(listed_paths(store) == Paths{"a-2.md", "d/a.md"});
}

TEST_CASE("remove of a missing note is NotFound; a folder is not removed as a note") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/x.md", "x");

    const auto missing = store.remove(np("yok.md"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
    CHECK_FALSE(store.remove(np("d")).has_value());
    CHECK(listed_paths(store) == Paths{"d/x.md"});
}

TEST_CASE("create_folder makes empty folders; repeating it is harmless") {
    TempDir tmp;
    NoteStore store(tmp.path());

    REQUIRE(store.create_folder("bos/alt").has_value());
    CHECK(store.list().empty());
    CHECK(store.folders() == std::vector<std::string>{"bos", "bos/alt"});
    CHECK(store.create_folder("bos/alt").has_value());  // yinelenen çağrı zararsız
    CHECK(store.folders() == std::vector<std::string>{"bos", "bos/alt"});
}

TEST_CASE("folder operations validate the folder name") {
    TempDir tmp;
    NoteStore store(tmp.path());

    for (const char* bad : {"../x", "a/../b", "", "/abs", ".gizli"}) {
        CAPTURE(bad);
        const auto created = store.create_folder(bad);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().code == Error::Code::InvalidPath);
        const auto removed = store.remove_folder(bad, true);
        REQUIRE_FALSE(removed.has_value());
        CHECK(removed.error().code == Error::Code::InvalidPath);
    }
    put(store, "a/x.md", "x");
    const auto renamed = store.rename_folder("a", "../kacis");
    REQUIRE_FALSE(renamed.has_value());
    CHECK(renamed.error().code == Error::Code::InvalidPath);
    CHECK(store.folders() == std::vector<std::string>{"a"});
}

TEST_CASE("rename_folder moves the notes; an existing target is AlreadyExists") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "a/x.md", "x");
    put(store, "a/sub/y.md", "y");
    put(store, "b/z.md", "z");

    REQUIRE(store.rename_folder("a", "c/d").has_value());
    CHECK(listed_paths(store) == Paths{"b/z.md", "c/d/sub/y.md", "c/d/x.md"});

    const auto clash = store.rename_folder("c/d", "b");
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().code == Error::Code::AlreadyExists);
    CHECK(body(store, "b/z.md") == "z");
    CHECK(body(store, "c/d/x.md") == "x");

    const auto missing = store.rename_folder("yok", "yeni");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);

    const auto into_itself = store.rename_folder("c", "c/d/e");
    REQUIRE_FALSE(into_itself.has_value());
    CHECK(arg(into_itself.error(), "reason") == "into_itself");
}

TEST_CASE("remove_folder without force refuses a folder with notes and deletes nothing") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "d/x.md", "x");
    put(store, "d/sub/y.md", "y");

    const auto refused = store.remove_folder("d", false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::NotEmpty);
    CHECK(arg(refused.error(), "count") == "2");
    CHECK(arg(refused.error(), "path") == "d");
    CHECK(listed_paths(store) == Paths{"d/sub/y.md", "d/x.md"});
}

TEST_CASE("remove_folder removes an empty folder without force and a full one with force") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create_folder("bos").has_value());
    REQUIRE(store.remove_folder("bos", false).has_value());
    CHECK(store.folders().empty());

    put(store, "d/x.md", "x");
    REQUIRE(store.create_folder("d/sub").has_value());
    put(store, "dd/komsu.md", "önek benzer klasör kalır");
    put(store, "kalan.md", "k");
    REQUIRE(store.remove_folder("d", true).has_value());
    CHECK(store.folders() == std::vector<std::string>{"dd"});
    CHECK(listed_paths(store) == Paths{"dd/komsu.md", "kalan.md"});

    const auto missing = store.remove_folder("yok", true);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
}

TEST_CASE("copy_folder copies the subtree, empty folders and exact bytes, in one write step") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    noteapp::test::upsert_note(dir.path(), "work/a.md", std::string_view("A\r\n\0B", 5));
    noteapp::test::upsert_note(dir.path(), "work/sub/b.txt", "B");
    REQUIRE(noteapp::test::import_notes(
        store, std::vector{noteapp::test::ImportedNote{NotePath::parse("work/Makefile").value(), "M"}}));
    REQUIRE(store.create_folder("work/empty"));
    REQUIRE(store.create_folder("dest"));
    REQUIRE(store.copy_folder("work", "dest/kopya"));
    CHECK(noteapp::test::note_body(dir.path(), "dest/kopya/a.md") == std::string("A\r\n\0B", 5));
    CHECK(noteapp::test::note_body(dir.path(), "dest/kopya/sub/b.txt") == "B");
    CHECK(noteapp::test::note_body(dir.path(), "dest/kopya/Makefile") == "M");
    const auto folders = store.folders();
    CHECK(std::ranges::find(folders, "dest/kopya/empty") != folders.end());
    CHECK(noteapp::test::note_body(dir.path(), "work/a.md") == std::string("A\r\n\0B", 5));
}

TEST_CASE("copy_folder refuses a missing source, a taken target and copying into itself, writing nothing") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    noteapp::test::upsert_note(dir.path(), "work/a.md", "A");
    noteapp::test::upsert_note(dir.path(), "taken.md", "T");
    const auto folders_before = store.folders();
    using Code = Error::Code;
    const auto code_of = [&](std::string_view from, std::string_view to) {
        const auto result = store.copy_folder(from, to);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    CHECK(code_of("nope", "x") == Code::NotFound);
    CHECK(code_of("work", "taken.md") == Code::AlreadyExists);
    CHECK(code_of("work", "work") == Code::InvalidPath);
    CHECK(code_of("work", "work/in/deep") == Code::InvalidPath);
    CHECK(store.folders() == folders_before);
    CHECK(store.list().size() == 2);
}

TEST_CASE("copy_folder splits a large copy across frames and copies every note once") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    constexpr int kNotes = 40;
    std::vector<noteapp::test::ImportedNote> notes;
    const std::string big(2U * 1024U * 1024U, 'x');  // 80 MiB toplam: tek çerçeveye sığmaz
    for (int i = 0; i < kNotes; ++i)
        notes.push_back(noteapp::test::ImportedNote{
            NotePath::parse("work/n" + std::to_string(i) + ".md").value(), big + std::to_string(i)});
    for (const noteapp::test::ImportedNote& note : notes)
        REQUIRE(noteapp::test::import_notes(store, std::span(&note, 1)));
    REQUIRE(store.copy_folder("work", "copy"));
    CHECK(store.list().size() == 2 * kNotes);
    CHECK(noteapp::test::note_body(dir.path(), "copy/n39.md") == big + "39");
}

namespace {

// Her biri kendi çerçevesine düşecek kadar büyük dört not: work/n0..n3.
constexpr std::size_t kCopyNoteBytes = 20U * 1024U * 1024U;
constexpr int kCopyNotes = 4;

void seed_large_notes(NoteStore& store) {
    for (int i = 0; i < kCopyNotes; ++i) {
        const noteapp::test::ImportedNote note{np("work/n" + std::to_string(i) + ".md"),
                                               std::string(kCopyNoteBytes, 'x') + std::to_string(i)};
        REQUIRE(noteapp::test::import_notes(store, std::span(&note, 1)));
    }
}

std::vector<std::string> paths_under(const NoteStore& store, std::string_view prefix) {
    std::vector<std::string> paths;
    for (const NoteInfo& info : store.list())
        if (info.path.string().starts_with(prefix))
            paths.push_back(info.path.string());
    std::ranges::sort(paths);
    return paths;
}

std::vector<std::string> all_copies() {
    return {"copy/n0.md", "copy/n1.md", "copy/n2.md", "copy/n3.md"};
}

}  // namespace

TEST_CASE("copy_folder closes the frame before a note that would overflow it: 31 MiB then 40 MiB") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    const std::string first(31U * 1024U * 1024U, 'a');
    const std::string second(40U * 1024U * 1024U, 'b');
    const std::vector<noteapp::test::ImportedNote> notes{{np("work/a.md"), first}, {np("work/b.md"), second}};
    for (const noteapp::test::ImportedNote& note : notes)
        REQUIRE(noteapp::test::import_notes(store, std::span(&note, 1)));
    const auto copied = store.copy_folder("work", "copy");
    REQUIRE_MESSAGE(copied.has_value(), (copied ? std::string() : format_error(copied.error())));
    CHECK(noteapp::test::note_body(dir.path(), "copy/a.md") == first);
    CHECK(noteapp::test::note_body(dir.path(), "copy/b.md") == second);
}

TEST_CASE("copy_folder resumes by path when a writer changes the source between frames") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    seed_large_notes(store);
    bool fired = false;
    std::function<void()> write;
    store.impl().after_copy_batch = [&] {
        if (!std::exchange(fired, true))
            write();
    };
    SUBCASE("a copied source note is removed: the next one is not skipped") {
        write = [&] { REQUIRE(store.remove(np("work/n0.md"))); };
    }
    SUBCASE("a note appears before the cursor: nothing is copied twice") {
        write = [&] { put(store, "work/a.md", "A"); };
    }
    REQUIRE(store.copy_folder("work", "copy"));
    CHECK(fired);
    CHECK(paths_under(store, "copy/") == all_copies());
}

TEST_CASE("copy_folder failing after a committed frame reports a partial copy with its count") {
    const noteapp::test::TempDir dir;
    NoteStore store(dir.path());
    seed_large_notes(store);
    bool fired = false;
    store.impl().after_copy_batch = [&] {
        if (!std::exchange(fired, true))
            put(store, "copy/n3.md", "someone else");
    };
    const auto copied = store.copy_folder("work", "copy");
    REQUIRE_FALSE(copied.has_value());
    CHECK(copied.error().code == Error::Code::PartialCopy);  // AlreadyExists olsa çağıran yeni adla yinelerdi
    const auto& args = copied.error().args;
    const auto count = std::ranges::find_if(args, [](const auto& arg) { return arg.first == "count"; });
    REQUIRE(count != args.end());
    CHECK(count->second == "3");
    CHECK(paths_under(store, "copy/") == all_copies());  // yarım kopya yerinde; n3 başkasının
    CHECK(noteapp::test::note_body(dir.path(), "copy/n3.md") == "someone else");
    CHECK(paths_under(store, "work/").size() == kCopyNotes);
}
