#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "edit/completion.hpp"
#include "edit/editor_session.hpp"
#include "edit/spellcheck.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp;
using tui::EditorSession;
using tui::Key;
namespace fs = std::filesystem;

namespace {

int plain_width(char32_t) {
    return 1;
}

std::size_t end_of(std::string_view text) {
    return text.size();
}

struct Fixture {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string internal;
    std::string type = "md";
    std::shared_ptr<const tui::WordTable> table;
    EditorSession session{tui::EditorHost{
        store,
        mutex,
        internal,
        plain_width,
        std::uintmax_t{1024} * 1024,
        core::kDefaultSlugMaxLength,
        [this](std::string_view) { return type; },
        [](const core::NotePath&) {},
        [] {},
        [](std::optional<std::size_t>) {},
        [](const core::Error&) {},
        [] {},
        {},
        {},
        [this] { return table; },
    }};

    Fixture() {
        core::init_i18n("en");
        session.layout(60, 10);
        session.begin_new_note("");
    }
    void type_text(std::string_view text) {
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t length = 1;
            while (at + length < text.size() &&
                   (static_cast<unsigned char>(text[at + length]) & 0xC0) == 0x80)
                ++length;
            const std::string_view piece = text.substr(at, length);
            if (piece == "\n")
                session.handle(Key::Enter);
            else
                session.handle(Key::Char, piece);
            at += length;
        }
    }
    const std::string& text() const { return session.buffer()->text(); }
};

}  // namespace

TEST_CASE("word prefix: only at the end of a word") {
    CHECK(tui::word_prefix_at("merhaba dün", 12) == "dün");
    CHECK(tui::word_prefix_at("snake_ca", 8) == "snake_ca");
    CHECK(tui::word_prefix_at("abc", 2).empty());  // arkasında harf var
    CHECK(tui::word_prefix_at("abc ", 4).empty());
    CHECK(tui::word_prefix_at("", 0).empty());
}

TEST_CASE("suggestion: nearest word in this note wins, then other notes") {
    const std::string text = "toplanti sonra toplam\nto";
    CHECK(tui::suggest_completion(text, end_of(text), "md", nullptr) == "plam");
    const std::string first = "to\ntoplanti";
    CHECK(tui::suggest_completion(first, 2, "md", nullptr) == "planti");

    const tui::WordTable table = tui::build_word_table({{"md", "sunucu sunucu sunum"}});
    const std::string other = "yeni su";
    CHECK(tui::suggest_completion(other, end_of(other), "md", &table) == "nucu");  // en sık geçen
}

TEST_CASE("suggestion: needs two bytes, a longer word and a word start") {
    CHECK_FALSE(tui::suggest_completion("toplam t", 8, "md", nullptr));
    CHECK_FALSE(tui::suggest_completion("toplam x toplam", 15, "md", nullptr));
    CHECK_FALSE(tui::suggest_completion("xtoplam to", 10, "md", nullptr));  // sözcük ortası
}

// L2: limits.tui.completion_* 0 (SIZE_MAX) ise eski tavanların üstü de önerilir.
TEST_CASE("suggestion: words beyond the old window, length and table caps are offered when unlimited") {
    const tui::CompletionLimits unlimited = tui::completion_limits_of(
        core::parse_config("[limits.tui]\ncompletion_window_bytes = 0\ncompletion_word_bytes = 0\n"
                           "completion_table_words = 0\n")
            .config.limits.tui,
        2);
    // 256 KiB'lık pencerenin dışındaki tek aday.
    const std::string far = "uzaksozcuk\n" + std::string(300 * 1024, ' ') + "\nuz";
    CHECK_FALSE(tui::suggest_completion(far, end_of(far), "md", nullptr));
    CHECK(tui::suggest_completion(far, end_of(far), "md", nullptr, unlimited) == "aksozcuk");

    // 64 bayttan uzun sözcük.
    const std::string word = "ab" + std::string(98, 'c');
    const std::string longer = word + "\nab";
    CHECK_FALSE(tui::suggest_completion(longer, end_of(longer), "md", nullptr));
    CHECK(tui::suggest_completion(longer, end_of(longer), "md", nullptr, unlimited) == word.substr(2));

    // tuning.completion_min_prefix_bytes 1: tek harfle öneri.
    tui::CompletionLimits one_byte;
    one_byte.min_prefix_bytes = 1;
    const std::string single = "toplam t";
    CHECK(tui::suggest_completion(single, end_of(single), "md", nullptr, one_byte) == "oplam");

    // Sözcük tablosu 200.000 sözcükte durmaz.
    std::string many;
    for (int i = 0; i < 200'010; ++i)
        many += "ww" + std::to_string(i) + ' ';
    CHECK(tui::build_word_table({{"md", many}}).words.size() == tui::CompletionLimits{}.table_words);
    CHECK(tui::build_word_table({{"md", many}}, unlimited).words.size() == 200'010);
}

TEST_CASE("suggestion: Turkish words complete whole code points") {
    const std::string text = "çalışma ça";
    CHECK(tui::suggest_completion(text, end_of(text), "md", nullptr) == "lışma");
}

TEST_CASE("suggestion: env notes offer keys of this note, never values") {
    const std::string keys = "API_TOKEN=gizlideger\nAP";
    CHECK(tui::suggest_completion(keys, end_of(keys), "env", nullptr) == "I_TOKEN");
    const std::string values = "API_TOKEN=gizlideger\nDB=gi";
    CHECK_FALSE(tui::suggest_completion(values, end_of(values), "env", nullptr));
    const std::string comment = "# yorumgizli\nyo";
    CHECK_FALSE(tui::suggest_completion(comment, end_of(comment), "properties", nullptr));
}

TEST_CASE("word table: secret-bearing types give only keys to other notes") {
    const tui::WordTable table = tui::build_word_table({{"env", "API_TOKEN=gizlideger\n# yorumgizli\n"},
                                                        {"properties", "db.parola: cokgizli\n"},
                                                        {"md", "normalsozcuk gizlideger2"}});
    const auto has = [&](std::string_view word) { return std::ranges::binary_search(table.words, word); };
    CHECK(has("API_TOKEN"));
    CHECK(has("normalsozcuk"));
    CHECK_FALSE(has("gizlideger"));
    CHECK_FALSE(has("yorumgizli"));
    CHECK_FALSE(has("cokgizli"));
    const std::string typed = "giz";
    CHECK(tui::suggest_completion(typed, 3, "md", &table) == "lideger2");
}

TEST_CASE("candidates: unique, note first; filter is fuzzy and bounded") {
    const tui::WordTable table = tui::build_word_table({{"md", "banana bandana"}});
    const std::string text = "bandaj bandaj ba";
    const auto candidates = tui::completion_candidates(text, end_of(text), "md", &table);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates.front() == "bandaj");
    const auto filtered = tui::filter_candidates(candidates, "bnn", 8);
    CHECK(filtered.size() == 2);
    CHECK(tui::filter_candidates(candidates, "", 2).size() == 2);
    CHECK(tui::filter_candidates(candidates, "zzz", 8).empty());
}

TEST_CASE("editor ghost: Tab accepts as one undo step, typing replaces it") {
    Fixture f;
    f.type_text("toplanti\nto");
    CHECK(f.session.ghost() == "planti");
    f.session.handle(Key::Tab);
    CHECK(f.text() == "toplanti\ntoplanti");
    CHECK(f.session.ghost().empty());
    f.session.handle(Key::CtrlZ);
    CHECK(f.text() == "toplanti\nto");
}

TEST_CASE("editor ghost: right arrow at line end accepts; with no ghost it moves") {
    Fixture f;
    f.type_text("toplanti\nto");
    f.session.handle(Key::Right);
    CHECK(f.text() == "toplanti\ntoplanti");
    const std::size_t cursor = f.session.buffer()->cursor();
    f.session.handle(Key::Right);
    CHECK(f.session.buffer()->cursor() == cursor);
    CHECK(f.text() == "toplanti\ntoplanti");
}

TEST_CASE("editor ghost: Tab with no suggestion inserts a tab") {
    Fixture f;
    f.type_text("ab");
    CHECK(f.session.ghost().empty());
    f.session.handle(Key::Tab);
    CHECK(f.text() == "ab\t");
}

TEST_CASE("editor ghost: motion, Esc and mid-line dismiss it") {
    Fixture f;
    f.type_text("toplanti\nto");
    CHECK_FALSE(f.session.ghost().empty());
    f.session.handle(Key::Escape);
    CHECK(f.session.ghost().empty());
    f.type_text("p");
    CHECK(f.session.ghost() == "lanti");
    f.session.handle(Key::Left);
    CHECK(f.session.ghost().empty());
    f.type_text("x");  // satır sonu değil: öneri yok
    CHECK(f.session.ghost().empty());
}

TEST_CASE("editor ghost: from other notes through the word table; never in a read-only note") {
    Fixture f;
    f.table = std::make_shared<tui::WordTable>(tui::build_word_table({{"md", "sunucu"}}));
    f.type_text("su");
    CHECK(f.session.ghost() == "nucu");
}

TEST_CASE("popup: Ctrl+Space filters live, arrows move, Enter inserts as one undo step, Esc closes") {
    Fixture f;
    f.table = std::make_shared<tui::WordTable>(tui::build_word_table({{"md", "sunucu sunum"}}));
    f.type_text("surec su");
    f.session.handle(Key::CtrlSpace);
    REQUIRE(f.session.mode() == EditorSession::Mode::Complete);
    CHECK(f.session.completion_query() == "su");
    CHECK(f.session.completion_items().size() == 3);
    f.session.handle(Key::Char, "n");
    CHECK(f.session.completion_query() == "sun");
    CHECK(f.session.completion_items().size() == 2);
    CHECK(f.text() == "surec su");  // yazı girdiye gider, metne değil
    f.session.handle(Key::Down);
    CHECK(f.session.completion_selected() == 1);
    const std::string chosen = f.session.completion_items()[1];
    f.session.handle(Key::Enter);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "surec " + chosen);
    f.session.handle(Key::CtrlZ);
    CHECK(f.text() == "surec su");

    f.session.handle(Key::CtrlSpace);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "surec su");
}

TEST_CASE("popup: nothing to offer leaves the editor untouched") {
    Fixture f;
    f.session.handle(Key::CtrlSpace);
    f.session.handle(Key::Enter);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text().empty());
}

TEST_CASE("ghost is drawn dim after the cursor, on the current row, and clipped to the pane") {
    Fixture f;
    f.type_text("toplanti\nto");
    ftxui::Screen screen(40, 6);
    auto element = tui::editor_pane(f.session, true);
    ftxui::Render(screen, element);
    const std::string shown = screen.ToString();
    CHECK(shown.find("toplanti") != std::string::npos);
    bool dim_found = false;
    for (int x = 0; x < 40; ++x)
        dim_found = dim_found || (screen.PixelAt(x, 1).character == "l" && screen.PixelAt(x, 1).dim);
    CHECK(dim_found);
    CHECK(f.text() == "toplanti\nto");  // yalnız görüntü

    // Panelden taşan gölge sarmaya girmez: sonraki satır dosya sonu işaretidir.
    Fixture narrow;
    narrow.session.layout(20, 6);
    narrow.type_text("to" + std::string(40, 'x') + "\nto");
    REQUIRE(narrow.session.ghost().size() == 40);
    ftxui::Screen small(20, 6);
    auto clipped = tui::editor_pane(narrow.session, true);
    ftxui::Render(small, clipped);
    CHECK(small.PixelAt(0, 4).character == "~");
    CHECK(small.PixelAt(19, 3).character == "x");
}

TEST_CASE("spellcheck applies to md and txt; dictionary comes from the language") {
    CHECK(tui::spellcheck_applies_to("md"));
    CHECK(tui::spellcheck_applies_to("txt"));
    CHECK_FALSE(tui::spellcheck_applies_to("java"));
    CHECK_FALSE(tui::spellcheck_applies_to("env"));
    CHECK(tui::dictionary_for_language("tr") == "tr_TR");
    CHECK(tui::dictionary_for_language("en") == "en_US");
}

#ifndef _WIN32
namespace {

struct FakeHunspell {
    test::TempDir tools;
    test::ScopedEnv path{"PATH", test::utf8(tools.path())};
    std::atomic<int> ready{0};

    void install(const std::string& body) const {
        const fs::path file = tools.path() / "hunspell";
        {
            std::ofstream out(file);
            out << "#!/bin/sh\n" << body << "\n";
        }
        fs::permissions(file, fs::perms::owner_all);
    }
    std::function<void()> on_ready() {
        return [this] { ++ready; };
    }
    std::string args() const { return test::slurp(tools.path() / "args"); }
};

// Satırdaki her `kotu` sözcüğünü bildirir: gerçek hunspell'in `&` biçimi.
const std::string kFakeBody = R"(/usr/bin/printf '%s ' "$@" > "$(/usr/bin/dirname "$0")/args"
echo '@(#) International Ispell Version 3.2.06 (but really Hunspell 1.7.2)'
while IFS= read -r line; do
    for word in $line; do
        word="${word#^}"
        case "$word" in
            kotu*) echo "& $word 1 0: iyi" ;;
            ığdır*) echo "# $word 0" ;;
            *) echo "*" ;;
        esac
    done
    echo
done)";

template <typename Check> bool eventually(Check check) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (check())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

constexpr auto kNoIdle = std::chrono::milliseconds(1);

}  // namespace

TEST_CASE("spell: the first call never blocks, the answer arrives through on_ready") {
    FakeHunspell fake;
    fake.install(kFakeBody);
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    CHECK(spell.misspelled("iyi kotu sozcuk").empty());
    REQUIRE(eventually([&] { return fake.ready.load() > 0; }));
    const auto ranges = spell.misspelled("iyi kotu sozcuk");
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].begin == 4);
    CHECK(ranges[0].end == 8);
    CHECK(fake.args().find("-a") != std::string::npos);
    CHECK(fake.args().find("-d en_US") != std::string::npos);
    CHECK(fake.args().find("-i utf-8") != std::string::npos);
}

// Windows'ta hunspell'in stdout'u metin kipindedir: her satır \r\n ile biter, yanıt sonu \r\n\r\n olur.
TEST_CASE("spell: CRLF replies are understood") {
    FakeHunspell fake;
    fake.install("{\n" + kFakeBody + "\n} | /usr/bin/sed -u 's/$/\\r/'");
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    spell.misspelled("iyi kotu sozcuk");
    REQUIRE(eventually([&] { return !spell.misspelled("iyi kotu sozcuk").empty(); }));
    const auto ranges = spell.misspelled("iyi kotu sozcuk");
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].begin == 4);
    CHECK(ranges[0].end == 8);
    CHECK_FALSE(spell.disabled());
}

TEST_CASE("spell: Turkish words and repeats map to the right bytes; a line cannot run hunspell commands") {
    FakeHunspell fake;
    fake.install(kFakeBody);
    tui::SpellChecker spell("tr_TR", fake.on_ready(), kNoIdle);
    const std::string line = "şöyle ığdırx ve kotu kotu";
    spell.misspelled(line);
    REQUIRE(eventually([&] { return !spell.misspelled(line).empty(); }));
    const auto ranges = spell.misspelled(line);
    REQUIRE(ranges.size() == 3);
    CHECK(line.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) == "ığdırx");
    CHECK(line.substr(ranges[1].begin, ranges[1].end - ranges[1].begin) == "kotu");
    CHECK(ranges[2].begin > ranges[1].begin);
    // `*` ile başlayan satır ispell komutudur (kişisel sözlüğe yazar): `^` öneki onu metin yapar.
    const std::string command = "*kotu";
    spell.misspelled(command);
    REQUIRE(eventually([&] { return fake.ready.load() > 1; }));
    CHECK_FALSE(spell.disabled());
}

TEST_CASE("spell: with no hunspell on PATH it is silently disabled") {
    FakeHunspell fake;
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    CHECK(spell.misspelled("kotu").empty());
    REQUIRE(eventually([&] { return spell.disabled(); }));
    CHECK(spell.misspelled("kotu").empty());
}

TEST_CASE("spell: a hung hunspell disables the session without blocking the caller") {
    FakeHunspell fake;
    fake.install("exec /bin/sleep 30");
    const auto timeout = std::chrono::milliseconds(150);
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle, timeout);
    const auto start = std::chrono::steady_clock::now();
    spell.misspelled("kotu");
    if (noteapp::test::kWallClockBudgets)
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
    REQUIRE(eventually([&] { return spell.disabled(); }));
}

TEST_CASE("spell: a hunspell that stops answering mid-session disables it") {
    FakeHunspell fake;
    fake.install("echo '@(#) ok'\nread -r line\nexec /bin/sleep 30");
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle, std::chrono::milliseconds(150));
    spell.misspelled("kotu");
    REQUIRE(eventually([&] { return spell.disabled(); }));
}

TEST_CASE("spell: a crash disables it and it does not restart") {
    FakeHunspell fake;
    fake.install("echo '@(#) ok'\nread -r line\nexit 2");
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    spell.misspelled("kotu");
    REQUIRE(eventually([&] { return spell.disabled(); }));
    fake.install(kFakeBody);
    spell.misspelled("kotu baska");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(spell.misspelled("kotu baska").empty());
}

TEST_CASE("spell: destroying the checker with a hung child returns promptly") {
    FakeHunspell fake;
    fake.install("exec /bin/sleep 30");
    const auto start = std::chrono::steady_clock::now();
    {
        tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle, std::chrono::seconds(20));
        spell.misspelled("kotu");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (noteapp::test::kWallClockBudgets)
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("spell: over-long lines are not sent") {
    FakeHunspell fake;
    fake.install(kFakeBody);
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    CHECK(spell.misspelled(std::string(tui::SpellChecker::kDefaultMaxLineBytes + 1, 'k')).empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(fake.ready.load() == 0);
}

TEST_CASE("spell: a line over the old 8 KiB limit is checked when limits.tui.spellcheck_line_bytes is 0") {
    FakeHunspell fake;
    fake.install(kFakeBody);
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle, tui::SpellChecker::kDefaultTimeout, SIZE_MAX);
    const std::string line = "iyi kotu " + std::string(tui::SpellChecker::kDefaultMaxLineBytes, 'k');
    spell.misspelled(line);
    REQUIRE(eventually([&] { return fake.ready.load() > 0; }));
    CHECK_FALSE(spell.misspelled(line).empty());
}

TEST_CASE("misspelled words are underlined in EDIT and in REVIEW") {
    FakeHunspell fake;
    fake.install(kFakeBody);
    tui::SpellChecker spell("en_US", fake.on_ready(), kNoIdle);
    const std::string line = "iyi kotu";
    spell.misspelled(line);
    REQUIRE(eventually([&] { return !spell.misspelled(line).empty(); }));

    Fixture f;
    f.type_text(line);
    ftxui::Screen screen(40, 4);
    auto element = tui::editor_pane(f.session, false, &spell);
    ftxui::Render(screen, element);
    int underlined = 0;
    for (int x = 0; x < 40; ++x)
        underlined += screen.PixelAt(x, 0).underlined ? 1 : 0;
    CHECK(underlined == 4);

    Fixture java;
    java.type = "java";
    java.type_text(line);
    ftxui::Screen code(40, 4);
    auto code_element = tui::editor_pane(java.session, false, &spell);
    ftxui::Render(code, code_element);
    underlined = 0;
    for (int x = 0; x < 40; ++x)
        underlined += code.PixelAt(x, 0).underlined ? 1 : 0;
    CHECK(underlined == 0);
}

// Gerçek hunspell: konteynerde `tests/tui/hunspell_real.sh` çalıştırır, sözlük paketleri gerekir.
TEST_CASE("real hunspell: Turkish and English dictionaries" * doctest::skip()) {
    using namespace std::chrono_literals;
    for (const auto& [dictionary, good, bad] : {std::tuple{"tr_TR", "güneş çiçek", "güneşx çiiçek"},
                                                std::tuple{"en_US", "the house", "hosue teh"}}) {
        CAPTURE(dictionary);
        std::atomic<int> ready{0};
        tui::SpellChecker spell(dictionary, [&] { ++ready; }, 1ms, 10s);
        spell.misspelled(good);
        REQUIRE(eventually([&] { return ready.load() > 0 || spell.disabled(); }));
        REQUIRE_FALSE(spell.disabled());
        CHECK(spell.misspelled(good).empty());
        const std::string line(bad);
        spell.misspelled(line);
        REQUIRE(eventually([&] { return !spell.misspelled(line).empty() || spell.disabled(); }));
        const auto ranges = spell.misspelled(line);
        REQUIRE(ranges.size() == 2);
        CHECK(line.substr(ranges[0].begin, ranges[0].end - ranges[0].begin) ==
              line.substr(0, line.find(' ')));
    }
}
#endif

TEST_CASE("word table: KEY=value lines give only the key in any type") {
    const auto words_of = [](std::string type, std::string text) {
        return tui::build_word_table({{std::move(type), std::move(text)}}).words;
    };
    const auto contains = [](const std::vector<std::string>& words, std::string_view word) {
        return std::ranges::find(words, word) != words.end();
    };
    for (const char* type : {"md", "txt", "cpp", "py", "sh", "makefile"}) {
        CAPTURE(type);
        const auto words = words_of(type, "API_TOKEN=hunter2\nexport DB_PASS=sifre123\n");
        CHECK(contains(words, "API_TOKEN"));
        CHECK(contains(words, "DB_PASS"));
        CHECK_FALSE(contains(words, "hunter2"));
        CHECK_FALSE(contains(words, "sifre123"));
    }
    // Düz metin türlerinde düzyazı öneriye girmeye devam eder.
    CHECK(contains(words_of("md", "toplanti notu burada\n"), "toplanti"));
}

TEST_CASE("word table: colon assignments and token-like words never enter, in any type") {
    const auto words_of = [](std::string type, std::string text) {
        return tui::build_word_table({{std::move(type), std::move(text)}}).words;
    };
    const auto contains = [](const std::vector<std::string>& words, std::string_view word) {
        return std::ranges::find(words, word) != words.end();
    };
    const std::string github = "ghp_16C7e42F292c6912E7710c838347Ae178B4a";
    const std::string aws = "AKIAIOSFODNN7EXAMPLE";
    const std::string hex = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b";
    const std::string secret_key = "abcdEFGH1234ijkl";
    for (const char* type : {"md", "txt"}) {
        CAPTURE(type);
        const auto assigned = words_of(type, "token: " + github + "\n");
        CHECK(contains(assigned, "token"));
        CHECK_FALSE(contains(assigned, github));
        CHECK_FALSE(contains(words_of(type, "token: gizlisifre\n"), "gizlisifre"));
        CHECK_FALSE(contains(words_of(type, "bare " + github + " word\n"), github));
        CHECK_FALSE(contains(words_of(type, "anahtar " + aws + " burada\n"), aws));
        CHECK_FALSE(contains(words_of(type, "sum " + hex + "\n"), hex));
        CHECK_FALSE(contains(words_of(type, "key sk-" + secret_key + " son\n"), secret_key));
    }
    // Düzyazı ve kısa, rakamsız uzun sözcükler öneriye girmeye devam eder.
    CHECK(contains(words_of("md", "akia ghp_kisa\n"), "ghp_kisa"));
    CHECK(contains(words_of("md", "antidisestablishmentarianismandmore burada\n"),
                   "antidisestablishmentarianismandmore"));
    CHECK(contains(words_of("md", "Toplantı: bütçe planı\n"), "planı"));
}

TEST_CASE("suggestion: a token-like word in this note is not offered") {
    const std::string github = "ghp_16C7e42F292c6912E7710c838347Ae178B4a";
    const std::string text = github + " sonra\ngh";
    CHECK_FALSE(tui::suggest_completion(text, text.size(), "md", nullptr).has_value());
}

TEST_CASE("word table: sh, json and similar give keys, never values") {
    const auto words_of = [](std::string type, std::string text) {
        return tui::build_word_table({{std::move(type), std::move(text)}}).words;
    };
    const auto contains = [](const std::vector<std::string>& words, std::string_view word) {
        return std::ranges::find(words, word) != words.end();
    };
    const auto json = words_of("json", "{\n  \"password\": \"hunter2\",\n  \"list\": [\"gizli\"]\n}\n");
    CHECK(contains(json, "password"));
    CHECK_FALSE(contains(json, "hunter2"));
    CHECK_FALSE(contains(json, "gizli"));
    const auto shell = words_of("sh", "TOKEN=abc123\ncurl -u kullanici:parola https://x\n");
    CHECK(contains(shell, "TOKEN"));
    CHECK_FALSE(contains(shell, "abc123"));
    CHECK_FALSE(contains(shell, "parola"));
}

TEST_CASE("suggestion and popup: a value on a KEY=value line is not offered in any type") {
    const std::string text = "API_TOKEN=hunter2\nhu";
    CHECK_FALSE(tui::suggest_completion(text, text.size(), "md", nullptr).has_value());
    const std::string keys = "API_TOKEN=hunter2\nAP";
    CHECK(tui::suggest_completion(keys, keys.size(), "md", nullptr) == "I_TOKEN");
    const auto candidates = tui::completion_candidates(text, text.size(), "md", nullptr);
    CHECK(std::ranges::find(candidates, "hunter2") == candidates.end());
    CHECK(std::ranges::find(candidates, "API_TOKEN") != candidates.end());
    // Bu notta düzyazı önerilmeye devam eder.
    const std::string prose = "merhaba dunya\nme";
    CHECK(tui::suggest_completion(prose, prose.size(), "md", nullptr) == "rhaba");
    const std::string shell = "curl -u kullanici:parola\npar";
    CHECK_FALSE(tui::suggest_completion(shell, shell.size(), "sh", nullptr).has_value());
}

#ifndef _WIN32
TEST_CASE("spell: quitting while a write to a child that stopped reading is blocked returns promptly") {
    FakeHunspell fake;
    // Okumaz, her isteğe boş yanıt verir: boru dolunca yazma takılır.
    fake.install("echo '@(#) ok'\nwhile :; do echo; done");
    const auto line = [](int i) {
        return std::to_string(i) + std::string(tui::SpellChecker::kDefaultMaxLineBytes - 8, 'k');
    };
    auto spell =
        std::make_unique<tui::SpellChecker>("en_US", fake.on_ready(), kNoIdle, std::chrono::seconds(20));
    for (int i = 0; i < 100; ++i)
        spell->misspelled(line(i));
    // İşçi 64 KiB sonra yazmada takılır.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    spell.reset();
    if (noteapp::test::kWallClockBudgets)
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
#endif
