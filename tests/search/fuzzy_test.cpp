#include "search/fuzzy.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp::search;
namespace core = noteapp::core;

namespace {

using Positions = std::vector<std::size_t>;

core::NoteInfo note(const std::string& path, int mtime_ticks = 0) {
    return core::NoteInfo{*core::NotePath::parse(path),
                          std::filesystem::file_time_type{std::chrono::seconds{mtime_ticks}}, 0};
}

double score_of(std::string_view q, std::string_view c) {
    auto m = fuzzy_match(q, c);
    REQUIRE(m.has_value());
    return m->score;
}

}  // namespace

TEST_CASE("fuzzy: subsequence matches with word-boundary positions") {
    auto m = fuzzy_match("wmn", "work/meeting-notes.md");
    REQUIRE(m.has_value());
    CHECK(m->positions == Positions{0, 5, 13});
}

// fzf/fzy davranışı: kelime başlarına düşen kısaltma eşleşmesi, kelime içindeki sıkı eşleşmeden önce gelir.
TEST_CASE("fuzzy: word-start acronym ranks above mid-word match like fzf") {
    CHECK(score_of("mtg", "my-long-text-generator.md") > score_of("mtg", "meeting.md"));
}

TEST_CASE("fuzzy: match after slash beats mid-word match") {
    CHECK(score_of("n", "x/n.md") > score_of("n", "xan.md"));
    CHECK(score_of("n", "x/n.md") > score_of("n", "x-n.md"));  // '/' en yüksek bonus
    CHECK(score_of("n", "x-n.md") > score_of("n", "xan.md"));
}

TEST_CASE("fuzzy: camelCase boundary and consecutive run get bonuses") {
    CHECK(score_of("fb", "fooBar.md") > score_of("fb", "foobar.md"));
    CHECK(score_of("abc", "abc.md") > score_of("abc", "a-b-c.md"));
}

TEST_CASE("fuzzy: non-subsequence returns nullopt") {
    CHECK_FALSE(fuzzy_match("xyz", "work/meeting-notes.md").has_value());
    CHECK_FALSE(fuzzy_match("ab", "ba").has_value());
    CHECK_FALSE(fuzzy_match("a", "").has_value());
}

TEST_CASE("fuzzy: Turkish folding matches and highlights original characters") {
    auto a = fuzzy_match("ogrenci", "Öğrenci.md");
    REQUIRE(a.has_value());
    CHECK(a->positions == Positions{0, 2, 4, 5, 6, 7, 8});  // Ö ve ğ iki baytlık

    auto b = fuzzy_match("ISIK", "ışık.txt");
    REQUIRE(b.has_value());
    CHECK(b->positions == Positions{0, 2, 4, 6});

    auto c = fuzzy_match("şehir", "SEHIR.md");
    REQUIRE(c.has_value());
    CHECK(c->positions == Positions{0, 1, 2, 3, 4});

    auto d = fuzzy_match("istanbul", "İSTANBUL.md");
    REQUIRE(d.has_value());
    CHECK(d->positions.front() == 0);
    CHECK(d->positions[1] == 2);  // İ iki bayt
}

TEST_CASE("fuzzy: empty query matches everything with score 0 and no positions") {
    auto m = fuzzy_match("", "anything.md");
    REQUIRE(m.has_value());
    CHECK(m->score == 0.0);
    CHECK(m->positions.empty());
}

TEST_CASE("fuzzy: exact match and very long candidate") {
    auto exact = fuzzy_match("abc", "ABC");
    REQUIRE(exact.has_value());
    CHECK(exact->positions == Positions{0, 1, 2});
    CHECK(exact->score > score_of("abc", "abcd"));

    std::string longc(5000, 'a');
    auto m = fuzzy_match("aa", longc);
    REQUIRE(m.has_value());
    CHECK(m->score > 0.0);
    CHECK(m->positions.empty());
    CHECK_FALSE(fuzzy_match("b", longc).has_value());
}

TEST_CASE("rank_notes: orders by score, then shorter path, then newer mtime") {
    std::vector<core::NoteInfo> notes{
        note("my-long-text-generator.md"),  // 0
        note("meeting.md"),                 // 1
        note("zzz.md"),                     // 2: eşleşmez
    };
    auto r = rank_notes("mtg", notes);
    REQUIRE(r.size() == 2);
    CHECK(r[0].index == 0);
    CHECK(r[1].index == 1);

    // Aynı puan, aynı uzunluk -> mtime azalan
    std::vector<core::NoteInfo> ties{note("a/x.md", 1), note("b/x.md", 5), note("c/x.md", 3)};
    auto t = rank_notes("x", ties);
    REQUIRE(t.size() == 3);
    CHECK(t[0].index == 1);
    CHECK(t[1].index == 2);
    CHECK(t[2].index == 0);

    // Aynı puan, farklı uzunluk -> kısa yol önce
    std::vector<core::NoteInfo> lens{note("ab.md", 9), note("a.md", 1)};
    CHECK(rank_notes("md", lens)[0].index == 1);
}

TEST_CASE("rank_notes: empty query returns all notes newest first, regardless of input order") {
    std::vector<core::NoteInfo> notes{note("a.md", 1), note("b.md", 9), note("c.md", 5)};
    auto r = rank_notes("", notes);
    REQUIRE(r.size() == 3);
    CHECK(r[0].index == 1);
    CHECK(r[1].index == 2);
    CHECK(r[2].index == 0);
    CHECK(r[0].match.positions.empty());
    CHECK(rank_notes("", {}).empty());
}

// Boş sorguda yol uzunluğu sırayı etkilememeli (T29'da bulundu): daha yeni ama daha uzun yol önce gelir.
TEST_CASE("rank_notes: empty query ignores path length, newer longer path comes first") {
    std::vector<core::NoteInfo> notes{note("beta.md", 1), note("gamma-notes.md", 5)};
    auto r = rank_notes("", notes);
    REQUIRE(r.size() == 2);
    CHECK(r[0].index == 1);
    CHECK(r[1].index == 0);
}

TEST_CASE("rank_notes: empty query breaks mtime ties by path") {
    std::vector<core::NoteInfo> notes{note("zeta.md", 3), note("alpha-long.md", 3)};
    auto r = rank_notes("", notes);
    REQUIRE(r.size() == 2);
    CHECK(r[0].index == 1);
}

TEST_CASE("rank_notes: 5000 realistic paths in <= 20 ms (Release only)") {
    static const char* const kFolders[] = {"work",    "personal",       "ideas",
                                           "journal", "projects/alpha", "projects/beta"};
    static const char* const kWords[] = {"meeting", "notes",  "shopping", "list", "design",
                                         "review",  "budget", "travel",   "plan", "retro",
                                         "idea",    "draft",  "summary",  "todo"};
    std::vector<core::NoteInfo> notes;
    notes.reserve(5000);
    unsigned state = 12345;  // deterministik LCG; testte rastgelelik istemiyoruz
    auto next = [&state](unsigned mod) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % mod;
    };
    for (int i = 0; i < 5000; ++i) {
        std::string p = std::string(kFolders[next(6)]) + "/" + kWords[next(14)] + "-" + kWords[next(14)] +
                        "-" + std::to_string(i) + ".md";
        notes.push_back(note(p, static_cast<int>(next(100000))));
    }

    auto t0 = std::chrono::steady_clock::now();
    auto r = rank_notes("wmn", notes);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("rank_notes(5000 paths) = " << ms << " ms, " << r.size() << " matches");
    CHECK_FALSE(r.empty());
#ifdef NDEBUG
    CHECK(ms <= 20.0);
#endif
}
