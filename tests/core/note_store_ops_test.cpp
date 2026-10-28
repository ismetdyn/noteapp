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

// Reddin gerekçesi (InvalidPath ise reason, değilse mesaj anahtarı); başarıda "ok". REQUIRE yerine: sonraki
// "hiçbir şey silinmedi/taşınmadı" denetimleri işlem yanlışlıkla başarılı olduğunda da koşsun.
template <typename T> std::string refusal(const std::expected<T, Error>& result) {
    if (result)
        return "ok";
    return result.error().code == Error::Code::InvalidPath ? arg(result.error(), "reason")
                                                           : result.error().message_key;
}

// Dizin bağı: Windows'ta yetki istemeyen junction, POSIX'te sembolik bağ (note_store_test.cpp ile aynı).
bool make_dir_link(const fs::path& target, const fs::path& link) {
#ifdef _WIN32
    const std::wstring command = L"mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >NUL";
    return _wsystem(command.c_str()) == 0;
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

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

// T16d: Windows'ta junction ile (MinGW libstdc++ onu bağ saymaz), POSIX'te sembolik bağla koşar.
TEST_CASE("move and remove reject paths through a directory link to outside the root") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    put(outside / "secret.md", "gizli");
    put(root / "a.md", "a");
    REQUIRE(make_dir_link(outside, root / "link"));
    NoteStore store(root);

    CHECK(refusal(store.move(np("link/secret.md"), np("stolen.md"))) == "link");
    CHECK(refusal(store.move(np("a.md"), np("link/a.md"))) == "link");
    CHECK(refusal(store.remove(np("link/secret.md"))) == "link");

    CHECK(slurp(outside / "secret.md") == "gizli");
    CHECK_FALSE(fs::exists(outside / "a.md"));
    CHECK_FALSE(fs::exists(root / "stolen.md"));
    CHECK(slurp(root / "a.md") == "a");
}

TEST_CASE("remove_folder never deletes through a directory link") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    const fs::path keep_only = tmp.path() / "keep-only";  // force'suz sayımda "boş" görünen dış klasör
    put(outside / "secret.md", "gizli");
    put(keep_only / "sub" / ".keep", "");
    put(root / "a" / "x.md", "x");
    fs::create_directories(root / "b");
    REQUIRE(make_dir_link(outside, root / "link"));
    REQUIRE(make_dir_link(outside, root / "a" / "link"));
    REQUIRE(make_dir_link(keep_only, root / "b" / "link"));
    NoteStore store(root);

    SUBCASE("the link itself, with force") {
        CHECK(refusal(store.remove_folder("link", true)) == "link");
    }
    SUBCASE("a folder that contains a link, with force") {
        CHECK(refusal(store.remove_folder("a", true)) == "contains_link");
    }
    SUBCASE("a folder whose only entry is a link to .keep-only folders, without force") {
        CHECK(refusal(store.remove_folder("b", false)) == "contains_link");
    }

    // Ne kök dışı hedef ne de kök içindeki hiçbir şey silinmiş olmalı.
    CHECK(slurp(outside / "secret.md") == "gizli");
    CHECK(fs::exists(keep_only / "sub" / ".keep"));
    CHECK(slurp(root / "a" / "x.md") == "x");
    CHECK(fs::exists(root / "link"));
    CHECK(fs::exists(root / "a" / "link"));
    CHECK(fs::exists(root / "b" / "link"));
}

TEST_CASE("rename_folder refuses to move a link or to move a folder into a link") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    put(outside / "secret.md", "gizli");
    put(root / "a" / "x.md", "x");
    REQUIRE(make_dir_link(outside, root / "link"));
    NoteStore store(root);

    CHECK(refusal(store.rename_folder("link", "moved")) == "link");
    CHECK_FALSE(fs::exists(root / "moved"));
    CHECK(refusal(store.rename_folder("a", "link/a")) == "link");
    CHECK(slurp(root / "a" / "x.md") == "x");
    CHECK_FALSE(fs::exists(outside / "a"));
    CHECK(slurp(outside / "secret.md") == "gizli");
}

TEST_CASE("create_folder refuses a link and a folder below a link") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    fs::create_directories(outside);
    fs::create_directories(root);
    REQUIRE(make_dir_link(outside, root / "link"));
    NoteStore store(root);

    CHECK(refusal(store.create_folder("link")) == "link");
    CHECK(refusal(store.create_folder("link/alt")) == "link");
    CHECK(fs::is_empty(outside));
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
