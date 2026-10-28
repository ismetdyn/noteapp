#include "core/note_store.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <random>

using namespace noteapp::core;
namespace fs = std::filesystem;

namespace {

// Her test kendi geçici kökünü alır; çıkışta (başarısız testte de) silinir.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("noteapp-test-" + std::to_string(rd()) + std::to_string(rd()));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void put(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
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
    CHECK(slurp(tmp.path() / "notes" / "a" / "b" / "plan.md") == "merhaba");
}

TEST_CASE("create puts a root note when folder is empty") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "plan", "txt", "x"}, kNow);
    REQUIRE(created.has_value());
    CHECK(created->string() == "plan.txt");
    CHECK(fs::exists(tmp.path() / "plan.txt"));
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
    CHECK(slurp(tmp.path() / "plan.md") == "bir");
    CHECK(slurp(tmp.path() / "plan-2.md") == "iki");
}

TEST_CASE("create normalizes CRLF to LF") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto created = store.create({"", "plan", "md", "a\r\nb\r\n"}, kNow);
    REQUIRE(created.has_value());
    CHECK(slurp(tmp.path() / "plan.md") == "a\nb\n");
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

TEST_CASE("read returns Io when the path is a directory") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "d.md");
    NoteStore store(tmp.path());
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
    put(tmp.path() / "bom.md", original);
    NoteStore store(tmp.path());
    auto read = store.read(np("bom.md"));
    REQUIRE(read.has_value());
    CHECK(read->text == original);
    auto written = store.write(np("bom.md"), read->text, read->mtime);
    REQUIRE(written.has_value());
    CHECK(slurp(tmp.path() / "bom.md") == original);
}

TEST_CASE("write replaces the content and returns the new mtime") {
    TempDir tmp;
    put(tmp.path() / "n.md", "eski");
    NoteStore store(tmp.path());
    auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    auto written = store.write(np("n.md"), "yeni içerik", read->mtime);
    REQUIRE(written.has_value());
    CHECK(slurp(tmp.path() / "n.md") == "yeni içerik");
    CHECK(*written == fs::last_write_time(tmp.path() / "n.md"));
    // Dönen mtime ile ardışık kayıt dış değişiklik sayılmaz.
    CHECK(store.write(np("n.md"), "üçüncü", *written).has_value());
    CHECK(slurp(tmp.path() / "n.md") == "üçüncü");
}

TEST_CASE("write refuses when the file changed externally") {
    TempDir tmp;
    put(tmp.path() / "n.md", "eski");
    NoteStore store(tmp.path());
    auto read = store.read(np("n.md"));
    REQUIRE(read.has_value());
    fs::last_write_time(tmp.path() / "n.md", read->mtime + std::chrono::seconds(5));
    auto written = store.write(np("n.md"), "yeni", read->mtime);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::ModifiedExternally);
    CHECK(slurp(tmp.path() / "n.md") == "eski");
}

TEST_CASE("write to a missing note is NotFound") {
    TempDir tmp;
    NoteStore store(tmp.path());
    auto written = store.write(np("yok.md"), "x", fs::file_time_type{});
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::NotFound);
}

TEST_CASE("failed write leaves the original intact and no .tmp behind") {
    TempDir tmp;
    // Hedef bir dizin: geçici dosya yazılır ama üzerine yer değiştirme başarısız olur.
    fs::create_directories(tmp.path() / "d.md");
    put(tmp.path() / "d.md" / "icerik.txt", "korunmalı");
    NoteStore store(tmp.path());
    auto written = store.write(np("d.md"), "x", fs::last_write_time(tmp.path() / "d.md"));
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::Io);
    CHECK(fs::is_directory(tmp.path() / "d.md"));
    CHECK(slurp(tmp.path() / "d.md" / "icerik.txt") == "korunmalı");
    for (const auto& entry : fs::directory_iterator(tmp.path())) {
        CAPTURE(entry.path().string());
        CHECK(entry.path().extension() != ".tmp");
    }
}

TEST_CASE("successful write leaves no .tmp behind") {
    TempDir tmp;
    put(tmp.path() / "n.md", "eski");
    NoteStore store(tmp.path());
    REQUIRE(store.write(np("n.md"), "yeni", fs::last_write_time(tmp.path() / "n.md")).has_value());
    int files = 0;
    for (const auto& entry : fs::directory_iterator(tmp.path())) {
        (void)entry;
        ++files;
    }
    CHECK(files == 1);
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
    REQUIRE(store.write(*created, "yeni", read->mtime).has_value());
    auto listed = store.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].path.string() == "klasör/toplantı.md");
    CHECK(store.folders() == std::vector<std::string>{"klasör"});
}

TEST_CASE("list is recursive, skips hidden entries and sorts by mtime descending") {
    TempDir tmp;
    put(tmp.path() / "old.md", "o");
    put(tmp.path() / "sub" / "new.md", "n");
    put(tmp.path() / "mid.md", "m");
    put(tmp.path() / ".hidden.md", "h");
    put(tmp.path() / ".git" / "config.md", "g");
    put(tmp.path() / "sub" / ".keep", "");
    const auto base = fs::last_write_time(tmp.path() / "old.md");
    fs::last_write_time(tmp.path() / "old.md", base - std::chrono::hours(2));
    fs::last_write_time(tmp.path() / "mid.md", base - std::chrono::hours(1));
    fs::last_write_time(tmp.path() / "sub" / "new.md", base);
    NoteStore store(tmp.path());
    auto listed = store.list();
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].path.string() == "sub/new.md");
    CHECK(listed[1].path.string() == "mid.md");
    CHECK(listed[2].path.string() == "old.md");
    CHECK(listed[0].size == 1);
}

TEST_CASE("list breaks mtime ties by path ascending") {
    TempDir tmp;
    put(tmp.path() / "b.md", "1");
    put(tmp.path() / "a.md", "1");
    const auto t = fs::last_write_time(tmp.path() / "a.md");
    fs::last_write_time(tmp.path() / "b.md", t);
    NoteStore store(tmp.path());
    auto listed = store.list();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].path.string() == "a.md");
    CHECK(listed[1].path.string() == "b.md");
}

TEST_CASE("list flags binary files by a NUL byte in the first 8 KB") {
    TempDir tmp;
    put(tmp.path() / "text.md", "düz metin");
    put(tmp.path() / "bin.dat", std::string("ab\0cd", 5));
    put(tmp.path() / "late.md", std::string(9000, 'x') + '\0');  // 8 KB'tan sonra: ikili sayılmaz
    NoteStore store(tmp.path());
    for (const auto& info : store.list()) {
        CAPTURE(info.path.string());
        CHECK(info.binary == (info.path.string() == "bin.dat"));
    }
}

TEST_CASE("list caches the binary flag: unchanged files are not reread") {
    TempDir tmp;
    put(tmp.path() / "a.md", "metin");
    put(tmp.path() / "b.dat", std::string("x\0y", 3));
    NoteStore store(tmp.path());

    CHECK(store.list().size() == 2);
    CHECK(store.head_read_count() == 2);
    CHECK(store.list().size() == 2);
    CHECK(store.head_read_count() == 2);  // değişmeyen dosyaların başı yeniden okunmadı

    put(tmp.path() / "c.md", "yeni");
    CHECK(store.list().size() == 3);
    CHECK(store.head_read_count() == 3);  // yalnız yeni dosya okundu
}

TEST_CASE("list refreshes the cached binary flag when a file changes (text to binary and back)") {
    TempDir tmp;
    const fs::path file = tmp.path() / "n.md";
    put(file, "abcd");
    NoteStore store(tmp.path());
    REQUIRE_FALSE(store.list().front().binary);

    put(file, std::string("ab\0d", 4));  // aynı boyut: yalnız mtime farkı yakalar
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    CHECK(store.list().front().binary);

    put(file, "abcd");
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(10));
    CHECK_FALSE(store.list().front().binary);
}

TEST_CASE("list skips files whose names fail NotePath validation") {
    TempDir tmp;
    put(tmp.path() / "ok.md", "x");
    put(tmp.path() / "trailing.", "x");  // Windows kırpar; Unix'te yol doğrulamasında elenir
    NoteStore store(tmp.path());
    auto listed = store.list();
    REQUIRE_FALSE(listed.empty());
    for (const auto& info : listed) {
        CHECK(NotePath::parse(info.path.string()).has_value());
    }
}

TEST_CASE("folders lists nested non-hidden directories sorted") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "b" / "c");
    fs::create_directories(tmp.path() / "a");
    fs::create_directories(tmp.path() / ".git" / "objects");
    fs::create_directories(tmp.path() / "a" / ".cache");
    put(tmp.path() / "file.md", "x");
    NoteStore store(tmp.path());
    CHECK(store.folders() == std::vector<std::string>{"a", "b", "b/c"});
}

TEST_CASE("symlinks pointing outside the root are rejected") {
    TempDir tmp;
    const fs::path root = tmp.path() / "notes";
    const fs::path outside = tmp.path() / "outside";
    put(outside / "secret.md", "gizli");
    fs::create_directories(root);
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "link", ec);
    if (ec) {
        MESSAGE("dizin sembolik bağı oluşturulamadı, atlandı: " << ec.message());
        return;
    }
    NoteStore store(root);

    auto read = store.read(np("link/secret.md"));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == Error::Code::InvalidPath);
    CHECK(arg(read.error(), "reason") == "outside_root");

    auto written = store.write(np("link/secret.md"), "x", fs::last_write_time(outside / "secret.md"));
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == Error::Code::InvalidPath);
    CHECK(slurp(outside / "secret.md") == "gizli");

    auto created = store.create({"link", "yeni", "md", "x"}, kNow);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == Error::Code::InvalidPath);
    CHECK_FALSE(fs::exists(outside / "yeni.md"));

    CHECK(store.list().empty());  // bağ listelenmez, içine inilmez
}
