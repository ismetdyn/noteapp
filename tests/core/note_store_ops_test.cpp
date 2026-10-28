// NoteStore taşı/sil/klasör işlemleri (T14). Silme yolu 🔴: hiçbir testte "yanlış şey silindi" olmamalı.
#include "core/note_store.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::core;
using noteapp::test::slurp;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

NotePath np(std::string_view s) {
    return *NotePath::parse(s);
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
    put(tmp.path() / "a.md", "içerik");
    NoteStore store(tmp.path());

    const auto moved = store.move(np("a.md"), np("work/2026/b.md"));
    REQUIRE(moved.has_value());
    CHECK(moved->string() == "work/2026/b.md");
    CHECK_FALSE(fs::exists(tmp.path() / "a.md"));
    CHECK(slurp(tmp.path() / "work" / "2026" / "b.md") == "içerik");
}

TEST_CASE("move with a changed extension changes the note type") {
    TempDir tmp;
    put(tmp.path() / "a.md", "class A {}");
    NoteStore store(tmp.path());

    REQUIRE(store.move(np("a.md"), np("a.java")).has_value());
    CHECK(listed_paths(store) == Paths{"a.java"});
}

TEST_CASE("move never overwrites an existing target") {
    TempDir tmp;
    put(tmp.path() / "a.md", "kaynak");
    put(tmp.path() / "b.md", "hedef");
    NoteStore store(tmp.path());

    const auto moved = store.move(np("a.md"), np("b.md"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().code == Error::Code::AlreadyExists);
    CHECK(arg(moved.error(), "path") == "b.md");
    CHECK(slurp(tmp.path() / "a.md") == "kaynak");
    CHECK(slurp(tmp.path() / "b.md") == "hedef");
}

TEST_CASE("move of a missing source is NotFound and creates no folders") {
    TempDir tmp;
    NoteStore store(tmp.path());

    const auto moved = store.move(np("yok.md"), np("yeni/yok.md"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().code == Error::Code::NotFound);
    CHECK_FALSE(fs::exists(tmp.path() / "yeni"));
}

TEST_CASE("move refuses a directory as source") {
    TempDir tmp;
    put(tmp.path() / "d" / "x.md", "x");
    NoteStore store(tmp.path());

    const auto moved = store.move(np("d"), np("e"));
    REQUIRE_FALSE(moved.has_value());
    CHECK(fs::exists(tmp.path() / "d" / "x.md"));
}

TEST_CASE("move and remove reject paths that escape the root through a symlink") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    put(outside / "secret.md", "gizli");
    put(root / "a.md", "a");
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "link", ec);
    if (ec) {
        MESSAGE("dizin sembolik bağı oluşturulamadı, atlandı: " << ec.message());
        return;
    }
    NoteStore store(root);

    const auto from_outside = store.move(np("link/secret.md"), np("stolen.md"));
    REQUIRE_FALSE(from_outside.has_value());
    CHECK(from_outside.error().code == Error::Code::InvalidPath);
    const auto to_outside = store.move(np("a.md"), np("link/a.md"));
    REQUIRE_FALSE(to_outside.has_value());
    CHECK(to_outside.error().code == Error::Code::InvalidPath);
    const auto removed = store.remove(np("link/secret.md"));
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error().code == Error::Code::InvalidPath);
    const auto folder_removed = store.remove_folder("link", true);
    REQUIRE_FALSE(folder_removed.has_value());
    CHECK(folder_removed.error().code == Error::Code::InvalidPath);

    CHECK(slurp(outside / "secret.md") == "gizli");
    CHECK(slurp(root / "a.md") == "a");
}

TEST_CASE("remove deletes exactly the named note") {
    TempDir tmp;
    put(tmp.path() / "a.md", "a");
    put(tmp.path() / "a-2.md", "a2");
    put(tmp.path() / "d" / "a.md", "da");
    NoteStore store(tmp.path());

    REQUIRE(store.remove(np("a.md")).has_value());
    CHECK(listed_paths(store) == Paths{"a-2.md", "d/a.md"});
}

TEST_CASE("remove of a missing note is NotFound; a folder is not removed as a note") {
    TempDir tmp;
    put(tmp.path() / "d" / "x.md", "x");
    NoteStore store(tmp.path());

    const auto missing = store.remove(np("yok.md"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
    CHECK_FALSE(store.remove(np("d")).has_value());
    CHECK(fs::exists(tmp.path() / "d" / "x.md"));
}

TEST_CASE("create_folder writes a hidden .keep; list and folders hide the file itself") {
    TempDir tmp;
    NoteStore store(tmp.path());

    REQUIRE(store.create_folder("bos/alt").has_value());
    CHECK(fs::exists(tmp.path() / "bos" / "alt" / ".keep"));
    CHECK(store.list().empty());
    CHECK(store.folders() == std::vector<std::string>{"bos", "bos/alt"});
    CHECK(store.create_folder("bos/alt").has_value());  // yinelenen çağrı zararsız
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
    put(tmp.path() / "a" / "x.md", "x");
    const auto renamed = store.rename_folder("a", "../kacis");
    REQUIRE_FALSE(renamed.has_value());
    CHECK(renamed.error().code == Error::Code::InvalidPath);
    CHECK_FALSE(fs::exists(tmp.path().parent_path() / "kacis"));
}

TEST_CASE("rename_folder moves the notes; an existing target is AlreadyExists") {
    TempDir tmp;
    put(tmp.path() / "a" / "x.md", "x");
    put(tmp.path() / "a" / "sub" / "y.md", "y");
    put(tmp.path() / "b" / "z.md", "z");
    NoteStore store(tmp.path());

    REQUIRE(store.rename_folder("a", "c/d").has_value());
    CHECK(listed_paths(store) == Paths{"b/z.md", "c/d/sub/y.md", "c/d/x.md"});

    const auto clash = store.rename_folder("c/d", "b");
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().code == Error::Code::AlreadyExists);
    CHECK(slurp(tmp.path() / "b" / "z.md") == "z");
    CHECK(fs::exists(tmp.path() / "c" / "d" / "x.md"));

    const auto missing = store.rename_folder("yok", "yeni");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
}

TEST_CASE("remove_folder without force refuses a folder with notes and deletes nothing") {
    TempDir tmp;
    put(tmp.path() / "d" / "x.md", "x");
    put(tmp.path() / "d" / "sub" / "y.md", "y");
    NoteStore store(tmp.path());

    const auto refused = store.remove_folder("d", false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::NotEmpty);
    CHECK(arg(refused.error(), "count") == "2");
    CHECK(arg(refused.error(), "path") == "d");
    CHECK(listed_paths(store) == Paths{"d/sub/y.md", "d/x.md"});
}

TEST_CASE("remove_folder without force also refuses unknown hidden files") {
    TempDir tmp;
    put(tmp.path() / "d" / ".keep", "");
    put(tmp.path() / "d" / ".gizli", "veri");
    NoteStore store(tmp.path());

    const auto refused = store.remove_folder("d", false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::NotEmpty);
    CHECK(slurp(tmp.path() / "d" / ".gizli") == "veri");
}

TEST_CASE("remove_folder removes an empty .keep folder without force and a full one with force") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create_folder("bos").has_value());
    REQUIRE(store.remove_folder("bos", false).has_value());
    CHECK_FALSE(fs::exists(tmp.path() / "bos"));

    put(tmp.path() / "d" / "x.md", "x");
    put(tmp.path() / "d" / "sub" / ".keep", "");
    put(tmp.path() / "kalan.md", "k");
    REQUIRE(store.remove_folder("d", true).has_value());
    CHECK_FALSE(fs::exists(tmp.path() / "d"));
    CHECK(listed_paths(store) == Paths{"kalan.md"});

    const auto missing = store.remove_folder("yok", true);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
}

TEST_CASE("the binary-flag cache follows move, rename_folder and remove without rereading") {
    TempDir tmp;
    put(tmp.path() / "a.md", "metin");
    put(tmp.path() / "d" / "b.dat", std::string("x\0y", 3));
    put(tmp.path() / "d" / "c.md", "c");
    NoteStore store(tmp.path());
    REQUIRE(store.list().size() == 3);
    const auto reads = store.head_read_count();

    REQUIRE(store.move(np("a.md"), np("e.md")).has_value());
    REQUIRE(store.rename_folder("d", "f").has_value());
    REQUIRE(store.remove(np("f/c.md")).has_value());
    const auto after = store.list();
    REQUIRE(after.size() == 2);
    CHECK(store.head_read_count() == reads);
    for (const auto& info : after)
        CHECK(info.binary == (info.path.string() == "f/b.dat"));
}
