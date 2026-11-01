#include "core/file_stat.hpp"
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <thread>

using namespace noteapp::core;
using noteapp::test::make_dir_link;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

const auto kNow = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 365 * 56);

}  // namespace

TEST_CASE("missing root lists nothing and is not created by the constructor") {
    TempDir tmp;
    NoteStore store(tmp.path() / "notes");
    CHECK(store.list().empty());
    CHECK(store.folders().empty());
    CHECK_FALSE(fs::exists(tmp.path() / "notes"));
}

TEST_CASE("create writes a note and creates root and folders as needed") {
    TempDir tmp;
    NoteStore store(tmp.path() / "notes");
    auto created = store.create({"a/b", "plan", "md", "merhaba"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "a/b/plan.md");
    CHECK(body(store, "a/b/plan.md") == "merhaba");
    CHECK(store.folders() == std::vector<std::string>{"a", "a/b"});
}

TEST_CASE("create puts a root note when folder is empty") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "plan", "txt", "x"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "plan.txt");
    CHECK(store.folders().empty());
}

TEST_CASE("create rejects invalid folder, name and type") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto bad_folder = store.create({"../x", "plan", "md", ""}, kNow);
    REQUIRE_FALSE(bad_folder.has_value());
    CHECK(bad_folder.error().code == Error::Code::InvalidPath);
    CHECK(arg(bad_folder.error(), "reason") == "dot_dot");

    for (const char* type : {"", ".md", "m.d"}) {
        CAPTURE(type);
        auto bad_type = store.create({"", "plan", type, ""}, kNow);
        REQUIRE_FALSE(bad_type.has_value());
        CHECK(bad_type.error().code == Error::Code::InvalidPath);
    }
    auto bad_name = store.create({"", "a/b", "md", ""}, kNow);
    REQUIRE_FALSE(bad_name.has_value());
    CHECK(bad_name.error().code == Error::Code::InvalidPath);
    auto reserved = store.create({"", "con", "md", ""}, kNow);
    REQUIRE_FALSE(reserved.has_value());
    CHECK(arg(reserved.error(), "reason") == "reserved_name");
    CHECK(store.list().empty());
}

TEST_CASE("create derives the name from the first non-empty line") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "", "md", "\n  \n# Toplantı Notları: Q4!\nikinci satır"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "toplanti-notlari-q4.md");
}

TEST_CASE("create truncates the slug to the configured maximum length") {
    TempDir tmp;
    NoteStore store(tmp.path(), 10);
    auto created = store.create({"", "", "md", "abcde fghij klmno"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "abcde.md");  // tire sınırında kesilir
}

TEST_CASE("create falls back to a timestamped name for empty content") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "", "md", ""}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string().starts_with("note-"));
}

TEST_CASE("create never overwrites: collisions get -2, -3") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto first = store.create({"", "plan", "md", "bir"}, kNow);
    auto second = store.create({"", "plan", "md", "iki"}, kNow);
    auto third = store.create({"", "plan", "md", "üç"}, kNow);
    REQUIRE((first && second && third));
    CHECK(first->string() == "plan.md");
    CHECK(second->string() == "plan-2.md");
    CHECK(third->string() == "plan-3.md");
    CHECK(body(store, "plan.md") == "bir");
    CHECK(body(store, "plan-2.md") == "iki");
}

TEST_CASE("create normalizes CRLF to LF") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "plan", "md", "a\r\nb\r\n"}, kNow);
    REQUIRE(created.has_value());
    CHECK(body(store, "plan.md") == "a\nb\n");
}

TEST_CASE("create_exact keeps CRLF and a BOM byte for byte and returns the note's revision") {
    TempDir tmp;
    NoteStore store(tmp.path());
    const std::string bytes = "\xEF\xBB\xBF"
                              "a\r\nb\nc\r\n";
    auto created = store.create_exact({"klasör", "plan", "md", bytes}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->path.string() == "klasör/plan.md");
    CHECK(body(store, "klasör/plan.md") == bytes);
    CHECK(created->revision == store.read(created->path)->revision);
    CHECK(store.write(created->path, "yeni", created->revision).has_value());
}

TEST_CASE("create_exact never overwrites: collisions get -2 and the first note is untouched") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create_exact({"", "plan", "md", "bir\r\n"}, kNow).has_value());
    auto second = store.create_exact({"", "plan", "md", "iki\r\n"}, kNow);
    REQUIRE(second.has_value());
    CHECK(second->path.string() == "plan-2.md");
    CHECK(body(store, "plan.md") == "bir\r\n");
    CHECK(body(store, "plan-2.md") == "iki\r\n");
}

TEST_CASE("create_exact validates like create and writes nothing when it refuses") {
    TempDir tmp;
    NoteStore store(tmp.path() / "yeni");
    CHECK(store.create_exact({"../x", "plan", "md", ""}, kNow).error().code == Error::Code::InvalidPath);
    CHECK(store.create_exact({"", "a/b", "md", ""}, kNow).error().code == Error::Code::InvalidPath);
    CHECK_FALSE(fs::exists(tmp.path() / "yeni"));
}

TEST_CASE("read returns NotFound for a missing note") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto result = store.read(np("yok.md"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(result.error().message_key == "error.not_found");
    CHECK(arg(result.error(), "path") == "yok.md");
}

TEST_CASE("read returns Io when the path is a folder") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create_folder("d.md").has_value());
    auto result = store.read(np("d.md"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::Io);
    CHECK(result.error().message_key == "error.io");
    CHECK(arg(result.error(), "path") == "d.md");
    CHECK_FALSE(arg(result.error(), "detail").empty());
}

TEST_CASE("write round-trips a UTF-8 BOM and CRLF byte-exactly") {
    TempDir tmp;
    const std::string original = "\xEF\xBB\xBF"
                                 "satır bir\r\nsatır iki\r\n";
    NoteStore store(tmp.path());
    put(store, "bom.md", original);
    auto read = store.read(np("bom.md"));
    REQUIRE(read.has_value());
    CHECK(read->text == original);
    auto written = store.write(np("bom.md"), read->text + "x", read->revision);
    REQUIRE(written.has_value());
    CHECK(body(store, "bom.md") == original + "x");
}

TEST_CASE("write replaces the content and returns the new revision") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "n.md", "eski");
    auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    auto written = store.write(np("n.md"), "yeni içerik", read->revision);
    REQUIRE(written.has_value());
    CHECK(body(store, "n.md") == "yeni içerik");
    CHECK(*written == store.read(np("n.md"))->revision);
    // Dönen revizyonla ardışık kayıt dış değişiklik sayılmaz.
    CHECK(store.write(np("n.md"), "üçüncü", *written).has_value());
    CHECK(body(store, "n.md") == "üçüncü");
}

TEST_CASE("write refuses when the note changed in another process") {
    TempDir tmp;
    NoteStore store(tmp.path());
    NoteStore other(tmp.path());
    put(store, "n.md", "eski");
    auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    REQUIRE(other.write(np("n.md"), "dış", other.read(np("n.md"))->revision).has_value());
    auto written = store.write(np("n.md"), "yeni", read->revision);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::ModifiedExternally);
    CHECK(body(store, "n.md") == "dış");
}

TEST_CASE("a same-size edit within the same millisecond window still trips the revision") {
    TempDir tmp;
    NoteStore store(tmp.path());
    NoteStore other(tmp.path());
    put(store, "n.md", "abcd");
    const auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    REQUIRE(other.write(np("n.md"), "wxyz", other.read(np("n.md"))->revision).has_value());  // aynı boyut
    const auto written = store.write(np("n.md"), "benim", read->revision);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::ModifiedExternally);
    CHECK(body(store, "n.md") == "wxyz");
}

TEST_CASE("list(), read() and write() report the same revision for the same note") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "n.md", "eski");
    const auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    CHECK(store.list().front().revision == read->revision);
    CHECK(store.list().front().mtime == read->mtime);
    const auto written = store.write(np("n.md"), "yeni", read->revision);
    REQUIRE(written.has_value());
    CHECK(store.list().front().revision == *written);
    CHECK(store.read(np("n.md"))->revision == *written);
}

TEST_CASE("stat_file reports kind and size and does not follow symlinks") {
    TempDir tmp;
    std::ofstream(tmp.path() / "f.md", std::ios::binary) << "abc";
    const auto file = stat_file(tmp.path() / "f.md");
    REQUIRE(file.has_value());
    CHECK((file->regular && !file->directory && !file->symlink));
    CHECK(file->size == 3);
    const auto dir = stat_file(tmp.path());
    REQUIRE(dir.has_value());
    CHECK((dir->directory && !dir->regular && !dir->symlink));
    CHECK_FALSE(stat_file(tmp.path() / "yok.md").has_value());

    fs::create_directories(tmp.path() / "target");
    if (!make_dir_link(tmp.path() / "target", tmp.path() / "link")) {
        MESSAGE("dizin bağı oluşturulamadı, bağ denetimi atlandı");
        return;
    }
    const auto link = stat_file(tmp.path() / "link");
    REQUIRE(link.has_value());
    CHECK((link->symlink && !link->regular && !link->directory));
}

TEST_CASE("write to a missing note is NotFound") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto written = store.write(np("yok.md"), "x", Revision{});
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::NotFound);
}

TEST_CASE("handles Turkish file and folder names") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"klasör", "toplantı", "md", "içerik ğüşiöç"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "klasör/toplantı.md");
    auto read = store.read(*created);
    REQUIRE(read.has_value());
    CHECK(read->text == "içerik ğüşiöç");
    REQUIRE(store.write(*created, "yeni", read->revision).has_value());
    auto listed = store.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].path.string() == "klasör/toplantı.md");
    CHECK(store.folders() == std::vector<std::string>{"klasör"});
}

TEST_CASE("list is recursive and sorts by update time descending") {
    TempDir tmp;
    NoteStore store(tmp.path());
    const auto now = std::chrono::system_clock::now();
    REQUIRE(store.create({"", "old", "md", "o"}, now - 2h).has_value());
    REQUIRE(store.create({"sub", "new", "md", "n"}, now).has_value());
    REQUIRE(store.create({"", "mid", "md", "m"}, now - 1h).has_value());
    auto listed = store.list();
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].path.string() == "sub/new.md");
    CHECK(listed[1].path.string() == "mid.md");
    CHECK(listed[2].path.string() == "old.md");
    CHECK(listed[0].size == 1);
}

TEST_CASE("list breaks time ties by path ascending") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create({"", "b", "md", "1"}, kNow).has_value());
    REQUIRE(store.create({"", "a", "md", "1"}, kNow).has_value());
    auto listed = store.list();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].path.string() == "a.md");
    CHECK(listed[1].path.string() == "b.md");
}

TEST_CASE("looks_binary: a NUL byte within the first 8 KB marks binary content") {
    CHECK_FALSE(looks_binary(""));
    CHECK_FALSE(looks_binary("düz metin"));
    CHECK(looks_binary(std::string("ab\0cd", 5)));
    CHECK(looks_binary(std::string(8191, 'x') + '\0'));
    CHECK_FALSE(looks_binary(std::string(8192, 'x') + '\0'));  // 8 KB'tan sonra: ikili sayılmaz
}

TEST_CASE("list includes binary notes") {
    TempDir tmp;
    NoteStore store(tmp.path());
    put(store, "text.md", "düz metin");
    put(store, "bin.dat", std::string("ab\0cd", 5));
    CHECK(store.list().size() == 2);
}

TEST_CASE("folders lists nested folders sorted") {
    TempDir tmp;
    NoteStore store(tmp.path());
    REQUIRE(store.create_folder("b/c").has_value());
    REQUIRE(store.create_folder("a").has_value());
    put(store, "file.md", "x");
    CHECK(store.folders() == std::vector<std::string>{"a", "b", "b/c"});
}
