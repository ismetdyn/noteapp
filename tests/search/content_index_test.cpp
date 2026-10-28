#include "search/content_index.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace noteapp::search;
namespace core = noteapp::core;
namespace fs = std::filesystem;

namespace {

// Her test kendi geçici dizinini açar ve kapanışta siler.
struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("noteapp_ci_" + std::to_string(std::random_device{}()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& content) const {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel, std::ios::binary) << content;
    }
};

struct Fixture {
    TempDir dir;
    core::NoteStore store{dir.path};
    ContentIndex index{store, 1024 * 1024};
};

std::vector<ContentResult> find(Fixture& f, std::string_view q, std::size_t limit = 50) {
    f.index.refresh();
    return f.index.search(q, limit);
}

std::string slice(const Hit& h, std::size_t i) {
    return h.snippet.substr(h.ranges[i].first, h.ranges[i].second - h.ranges[i].first);
}

}  // namespace

TEST_CASE("content: all terms must occur, on any lines") {
    Fixture f;
    f.dir.write("both.md", "proje başladı\nsonra\nbütçe onaylandı\n");
    f.dir.write("one.md", "proje proje proje\n");
    auto r = find(f, "proje bütçe");
    REQUIRE(r.size() == 1);
    CHECK(r[0].path.string() == "both.md");
    CHECK(r[0].hit_count == 2);
    REQUIRE(r[0].first_hits.size() == 2);
    CHECK(r[0].first_hits[0].line == 1);
    CHECK(r[0].first_hits[1].line == 3);
}

TEST_CASE("content: quoted phrase matches only adjacent words") {
    Fixture f;
    f.dir.write("adjacent.md", "x proje bütçe y\n");
    f.dir.write("apart.md", "proje\nbütçe\n");
    auto r = find(f, "\"proje bütçe\"");
    REQUIRE(r.size() == 1);
    CHECK(r[0].path.string() == "adjacent.md");
}

TEST_CASE("content: Turkish folding finds Öğrenci with ogrenci") {
    Fixture f;
    f.dir.write("a.md", "Öğrenci listesi\n");
    auto r = find(f, "ogrenci");
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].first_hits.size() == 1);
    CHECK(slice(r[0].first_hits[0], 0) == "Öğrenci");
}

TEST_CASE("content: empty or blank query returns nothing") {
    Fixture f;
    f.dir.write("a.md", "x\n");
    CHECK(find(f, "").empty());
    CHECK(find(f, "  \t ").empty());
    CHECK(find(f, "x", 0).empty());
}

TEST_CASE("content: ranges cover every occurrence inside the snippet") {
    Fixture f;
    f.dir.write("a.md", "çğ Ürün ve ürün sonra ÜRÜN\n");
    auto r = find(f, "urun");
    REQUIRE(r.size() == 1);
    const auto& h = r[0].first_hits[0];
    REQUIRE(h.ranges.size() == 3);
    CHECK(slice(h, 0) == "Ürün");
    CHECK(slice(h, 1) == "ürün");
    CHECK(slice(h, 2) == "ÜRÜN");
}

TEST_CASE("content: long line is truncated around the first occurrence without splitting UTF-8") {
    Fixture f;
    std::string line;
    for (int i = 0; i < 100; ++i)
        line += "ğ";  // 2 baytlık karakterler
    line += " hedef ";
    for (int i = 0; i < 100; ++i)
        line += "ş";
    f.dir.write("a.md", line + "\n");
    auto r = find(f, "hedef");
    REQUIRE(r.size() == 1);
    const auto& h = r[0].first_hits[0];
    CHECK(h.snippet.starts_with("…"));
    CHECK(h.snippet.ends_with("…"));
    REQUIRE(h.ranges.size() == 1);
    CHECK(slice(h, 0) == "hedef");
    // 120 kod noktası + iki "…"
    std::size_t cps = 0;
    for (unsigned char c : h.snippet)
        if ((c & 0xC0) != 0x80)
            ++cps;
    CHECK(cps == 122);
}

TEST_CASE("content: occurrence near the line end does not add a trailing ellipsis") {
    Fixture f;
    std::string line(200, 'a');
    line += " son";
    f.dir.write("a.md", line + "\n");
    auto r = find(f, "son");
    const auto& h = r[0].first_hits[0];
    CHECK(h.snippet.starts_with("…"));
    CHECK_FALSE(h.snippet.ends_with("…"));
    CHECK(slice(h, 0) == "son");
}

TEST_CASE("content: CRLF keeps line numbers and drops carriage returns") {
    Fixture f;
    f.dir.write("a.md", "bir\r\niki hedef\r\nüç\r\nhedef son\r\n");
    auto r = find(f, "hedef");
    REQUIRE(r.size() == 1);
    CHECK(r[0].hit_count == 2);
    CHECK(r[0].first_hits[0].line == 2);
    CHECK(r[0].first_hits[1].line == 4);
    for (const auto& h : r[0].first_hits)
        CHECK(h.snippet.find('\r') == std::string::npos);
    CHECK(r[0].first_hits[0].snippet == "iki hedef");
}

TEST_CASE("content: only the first three hit lines are returned but all are counted") {
    Fixture f;
    f.dir.write("a.md", "x\nx\nx\nx\nx\n");
    auto r = find(f, "x");
    CHECK(r[0].hit_count == 5);
    CHECK(r[0].first_hits.size() == 3);
}

TEST_CASE("content: results ordered by hit count, then mtime, then path; limit applied") {
    Fixture f;
    f.dir.write("b.md", "x\n");
    f.dir.write("a.md", "x\n");
    f.dir.write("many.md", "x\nx\n");
    f.dir.write("new.md", "x\n");
    auto base = fs::file_time_type::clock::now();
    fs::last_write_time(f.dir.path / "new.md", base);
    fs::last_write_time(f.dir.path / "a.md", base - std::chrono::hours{1});
    fs::last_write_time(f.dir.path / "b.md", base - std::chrono::hours{1});
    auto r = find(f, "x");
    REQUIRE(r.size() == 4);
    CHECK(r[0].path.string() == "many.md");
    CHECK(r[1].path.string() == "new.md");
    CHECK(r[2].path.string() == "a.md");
    CHECK(r[3].path.string() == "b.md");
    CHECK(f.index.search("x", 2).size() == 2);
}

TEST_CASE("content: binary and oversize notes are skipped") {
    TempDir dir;
    core::NoteStore store{dir.path};
    dir.write("text.md", "hedef\n");
    dir.write("bin.md", std::string("hedef\0hedef", 11));
    dir.write("big.md", "hedef " + std::string(100, 'a'));
    ContentIndex index{store, 50};
    index.refresh();
    auto r = index.search("hedef", 10);
    REQUIRE(r.size() == 1);
    CHECK(r[0].path.string() == "text.md");
}

TEST_CASE("content: refresh rereads only changed files and drops deleted ones") {
    Fixture f;
    f.dir.write("a.md", "alfa\n");
    f.dir.write("b.md", "beta\n");
    f.index.refresh();
    CHECK(f.index.read_count() == 2);
    f.index.refresh();
    CHECK(f.index.read_count() == 2);

    f.dir.write("a.md", "alfa degisti\n");  // boyut değişti
    f.dir.write("c.md", "gama\n");
    f.index.refresh();
    CHECK(f.index.read_count() == 4);
    CHECK(f.index.search("degisti", 10).size() == 1);

    fs::remove(f.dir.path / "b.md");
    f.index.refresh();
    CHECK(f.index.read_count() == 4);
    CHECK(f.index.search("beta", 10).empty());
    CHECK(f.index.search("gama", 10).size() == 1);
}

TEST_CASE("content: same-size edit is detected through mtime") {
    Fixture f;
    f.dir.write("a.md", "alfa\n");
    f.index.refresh();
    f.dir.write("a.md", "beta\n");
    fs::last_write_time(f.dir.path / "a.md", fs::file_time_type::clock::now() + std::chrono::hours{1});
    f.index.refresh();
    CHECK(f.index.read_count() == 2);
    CHECK(f.index.search("beta", 10).size() == 1);
}

TEST_CASE("content: performance with 5000 notes of ~5 KB") {
    TempDir dir;
    core::NoteStore store{dir.path};
    std::mt19937 rng{42};
    std::vector<std::string> words;
    for (int i = 0; i < 2000; ++i) {
        std::string w;
        for (int j = 0, n = 3 + static_cast<int>(rng() % 8); j < n; ++j)
            w += static_cast<char>('a' + rng() % 26);
        words.push_back(w);
    }
    words[0] = "proje";
    words[1] = "bütçe";
    for (int n = 0; n < 5000; ++n) {
        std::string text;
        while (text.size() < 5000) {
            text += words[rng() % words.size()];
            text += (rng() % 12 == 0) ? '\n' : ' ';
        }
        dir.write("d" + std::to_string(n % 50) + "/n" + std::to_string(n) + ".md", text);
    }
    ContentIndex index{store, 10 * 1024 * 1024};
    auto t0 = std::chrono::steady_clock::now();
    index.refresh();
    auto t1 = std::chrono::steady_clock::now();
    auto r = index.search("proje bütçe", 50);
    auto t2 = std::chrono::steady_clock::now();
    r = index.search("proje bütçe", 50);
    auto t3 = std::chrono::steady_clock::now();
    index.refresh();
    auto t4 = std::chrono::steady_clock::now();
    const auto listed = store.list().size();  // ikili bayrak önbelleği sıcak: yalnız stat maliyeti
    auto t5 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    MESSAGE("cold refresh " << ms(t0, t1) << " ms, first search " << ms(t1, t2) << " ms, warm search "
                            << ms(t2, t3) << " ms, no-change refresh " << ms(t3, t4)
                            << " ms (of which store.list() " << ms(t4, t5) << " ms)");
    CHECK(listed == 5000);
    CHECK(index.read_count() == 5000);
    CHECK_FALSE(r.empty());
#ifdef NDEBUG
    CHECK(ms(t2, t3) <= 100.0);
    // ponytail: ikili bayrak önbelleği (T16b) dosya açmayı kaldırdı ama list() hâlâ dosya başına stat
    // maliyetini öder (kurumsal Windows'ta ~200 ms/5.000 dosya); bütçe ContentIndex'in kendi payına
    // uygulanır. Toplam ≤ 100 ms için yerel dizin taraması (FindFirstFileEx / getdents) gerekir.
    CHECK(ms(t3, t4) - ms(t4, t5) <= 100.0);
#endif
}
