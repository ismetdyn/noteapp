// Sorgu argümanları (T55a): saf ayrıştırıcı ve apply.
#include "core/i18n.hpp"
#include "search/query.hpp"

#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace noteapp;
using namespace noteapp::search;
namespace chr = std::chrono;
namespace fs = std::filesystem;

namespace {

using Indices = std::vector<std::size_t>;

ParsedQuery parsed(std::string_view query) {
    auto result = parse_query(query);
    REQUIRE_MESSAGE(result.has_value(), core::format_error(result.error().error));
    return *result;
}

// Hatalı değerin konumu: sorgunun içinden kesilen metin.
std::string located(std::string_view query) {
    const auto result = parse_query(query);
    REQUIRE_FALSE(result.has_value());
    return std::string(query.substr(result.error().begin, result.error().end - result.error().begin));
}

fs::file_time_type local_time(int year, int month, int day, int hour, int minute = 0) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    return chr::file_clock::from_sys(chr::system_clock::from_time_t(std::mktime(&tm)));
}

core::NoteMeta meta(std::string_view path, std::uintmax_t size = 0, fs::file_time_type updated = {}) {
    return core::NoteMeta{.path = *core::NotePath::parse(path), .size = size, .updated_at = updated};
}

Indices filtered(std::string_view query, std::span<const core::NoteMeta> notes,
                 chr::system_clock::time_point now = chr::system_clock::now()) {
    const ParsedQuery q = parsed(query);
    return apply(q.filters, q.sort, notes, now);
}

}  // namespace

TEST_CASE("parse_query separates arguments from search text") {
    const ParsedQuery q = parsed("plan sort:-size type:md   toplantı in:work/");
    CHECK(q.text == "plan toplantı");
    REQUIRE(q.filters.size() == 2);
    CHECK(q.filters[0].kind == Filter::Kind::Type);
    CHECK(q.filters[0].text == "md");
    CHECK(q.filters[1].kind == Filter::Kind::Folder);
    CHECK(q.filters[1].text == "work/");
    REQUIRE(q.sort.has_value());
    CHECK(q.sort->field == SortField::Size);
    CHECK(q.sort->descending);

    CHECK(parsed("").text.empty());
    CHECK(parsed("  sort:name  ").text.empty());
}

TEST_CASE("parse_query reads every sort field and direction; the last sort wins") {
    REQUIRE(parsed("sort:name").sort.has_value());
    CHECK(parsed("sort:name").sort->field == SortField::Name);
    REQUIRE(parsed("sort:name").sort.has_value());
    CHECK_FALSE(parsed("sort:name").sort->descending);
    REQUIRE(parsed("sort:updated").sort.has_value());
    CHECK(parsed("sort:updated").sort->field == SortField::Updated);
    REQUIRE(parsed("sort:-created").sort.has_value());
    CHECK(parsed("sort:-created").sort->field == SortField::Created);
    REQUIRE(parsed("sort:-created").sort.has_value());
    CHECK(parsed("sort:-created").sort->descending);
    REQUIRE(parsed("sort:name sort:-size").sort.has_value());
    CHECK(parsed("sort:name sort:-size").sort->field == SortField::Size);
}

TEST_CASE("parse_query keeps tokens with an unknown key as plain text") {
    CHECK(parsed("foo:bar").text == "foo:bar");
    CHECK(parsed("http://x").text == "http://x");
    CHECK(parsed("saat:14:30 not").text == "saat:14:30 not");
    CHECK(parsed(":deger").text == ":deger");
    CHECK(parsed("foo:bar").filters.empty());
}

TEST_CASE("parse_query allows quoted values and keeps quoted text for the content search") {
    const ParsedQuery q = parsed("\"tam ifade\" kelime in:\"iş notları/\" origin:\"Ev PC\"");
    CHECK(q.text == "\"tam ifade\" kelime");
    REQUIRE(q.filters.size() == 2);
    CHECK(q.filters[0].text == "is notlari/");
    CHECK(q.filters[1].text == "ev pc");

    // Tamamı tırnaklı sözcük anahtar değildir.
    const ParsedQuery literal = parsed("\"sort:name\"");
    CHECK(literal.text == "\"sort:name\"");
    CHECK_FALSE(literal.sort.has_value());
    // Kapanmamış tırnak sonuna kadar sürer (parse_terms kuralı).
    CHECK(parsed("in:\"a b").filters[0].text == "a b/");
    CHECK(parsed("\"a b").text == "\"a b");
}

TEST_CASE("parse_query folds keys and values with the search fold rules") {
    REQUIRE(parsed("SORT:-SİZE").sort.has_value());
    CHECK(parsed("SORT:-SİZE").sort->field == SortField::Size);
    REQUIRE(parsed("sort:-sıze").sort.has_value());
    CHECK(parsed("sort:-sıze").sort->descending);
    CHECK(parsed("TYPE:MD").filters[0].text == "md");
    CHECK(parsed("İN:İŞ/").filters[0].text == "is/");
    CHECK(parsed("ORİGİN:WINDOWS").filters[0].text == "windows");
    CHECK(parsed("SYNC:MODIFIED").filters[0].sync == core::SyncState::Modified);
}

TEST_CASE("parse_query reads size bounds in bytes with binary units") {
    const auto size = [](std::string_view query) { return parsed(query).filters.at(0); };
    CHECK(size("size:>1mb").compare == Filter::Compare::Greater);
    CHECK(size("size:>1mb").bytes == 1024 * 1024);
    CHECK(size("size:<10kb").compare == Filter::Compare::Less);
    CHECK(size("size:<10kb").bytes == 10 * 1024);
    CHECK(size("size:>2GB").bytes == 2ULL * 1024 * 1024 * 1024);
    CHECK(size("size:<100b").bytes == 100);
    CHECK(size("size:<100").bytes == 100);
}

TEST_CASE("parse_query reads updated and created ages and dates") {
    const Filter age = parsed("updated:7d").filters.at(0);
    CHECK(age.kind == Filter::Kind::Updated);
    CHECK(age.compare == Filter::Compare::Within);
    CHECK(age.age == chr::days(7));

    const Filter after = parsed("created:>2026-10-01").filters.at(0);
    CHECK(after.kind == Filter::Kind::Created);
    CHECK(after.compare == Filter::Compare::Greater);
    CHECK(after.day == chr::sys_days(chr::year(2026) / 10 / 1));

    CHECK(parsed("updated:<2026-02-28").filters.at(0).compare == Filter::Compare::Less);
}

TEST_CASE("parse_query reads every sync state") {
    CHECK(parsed("sync:synced").filters[0].sync == core::SyncState::Synced);
    CHECK(parsed("sync:modified").filters[0].sync == core::SyncState::Modified);
    CHECK(parsed("sync:new").filters[0].sync == core::SyncState::New);
    CHECK(parsed("sync:conflict_copy").filters[0].sync == core::SyncState::ConflictCopy);
    CHECK(parsed("sync:sync_off").filters[0].sync == core::SyncState::SyncOff);
}

TEST_CASE("parse_query reports an invalid value at its location instead of treating it as text") {
    CHECK(located("plan sort:foo x") == "foo");
    CHECK(located("sort:-") == "-");
    CHECK(located("sort:+size") == "+size");
    CHECK(located("size:1mb") == "1mb");
    CHECK(located("size:>abc") == ">abc");
    CHECK(located("size:>1tb") == ">1tb");
    CHECK(located("size:>1.5mb") == ">1.5mb");
    CHECK(located("size:>") == ">");
    CHECK(located("size:>99999999999999gb") == ">99999999999999gb");
    CHECK(located("updated:7x") == "7x");
    CHECK(located("updated:d") == "d");
    CHECK(located("updated:0d") == "0d");
    CHECK(located("updated:999999999d") == "999999999d");
    CHECK(located("updated:2026-10-01") == "2026-10-01");
    CHECK(located("updated:>2026-13-01") == ">2026-13-01");
    CHECK(located("updated:>2026-02-30") == ">2026-02-30");
    CHECK(located("created:>26-10-01") == ">26-10-01");
    CHECK(located("created:>9999-01-01") == ">9999-01-01");
    CHECK(located("sync:bogus") == "bogus");
    CHECK(located("in:../gizli") == "../gizli");
    CHECK(located("in:.trash/") == ".trash/");
    CHECK(located("type:a.b") == "a.b");
    CHECK(located("x in:\"..\"") == "\"..\"");

    // Boş değer: yeri anahtarın kendisi.
    CHECK(located("a sort:") == "sort:");
    CHECK(located("type:") == "type:");
    CHECK(located("origin:\"\"") == "origin:\"\"");
}

TEST_CASE("a query error formats with key, value and reason in both languages") {
    const auto error = parse_query("size:abc");
    REQUIRE_FALSE(error.has_value());
    core::init_i18n("en");
    const std::string en = core::format_error(error.error().error);
    CHECK(en.find("size") != std::string::npos);
    CHECK(en.find("abc") != std::string::npos);
    CHECK(en.find("error.") == std::string::npos);
    CHECK(en.find("reason.") == std::string::npos);
    core::init_i18n("tr");
    const std::string tr = core::format_error(error.error().error);
    CHECK(tr.find("abc") != std::string::npos);
    CHECK(tr != en);
    core::init_i18n("en");
}

TEST_CASE("apply filters by type with case folding and only the last extension") {
    const std::vector notes{meta("a.md"), meta("b.MD"), meta("c.txt"), meta("d"), meta("e.md.txt")};
    CHECK(filtered("type:md", notes) == Indices{0, 1});
    CHECK(filtered("TYPE:txt", notes) == Indices{2, 4});
}

TEST_CASE("apply filters by folder prefix on whole components with folding") {
    const std::vector notes{meta("work/a.md"), meta("work/sub/b.md"), meta("workshop/c.md"), meta("d.md"),
                            meta("İş/e.md")};
    CHECK(filtered("in:work/", notes) == Indices{0, 1});
    CHECK(filtered("in:work", notes) == Indices{0, 1});
    CHECK(filtered("in:work/sub", notes) == Indices{1});
    CHECK(filtered("in:is", notes) == Indices{4});
    CHECK(filtered("in:WORKSHOP/", notes) == Indices{2});
}

TEST_CASE("apply filters by size with strict bounds; repeated bounds form a range") {
    const std::vector notes{meta("a", 1024), meta("b", 1025), meta("c", 1023), meta("d", 5000)};
    CHECK(filtered("size:>1kb", notes) == Indices{1, 3});
    CHECK(filtered("size:<1kb", notes) == Indices{2});
    CHECK(filtered("size:>1kb size:<2kb", notes) == Indices{1});
}

TEST_CASE("apply filters updated by age against the given clock") {
    const auto now = chr::sys_days(chr::year(2026) / 11 / 1) + chr::hours(12);
    const auto at = [&](chr::hours ago) { return chr::file_clock::from_sys(now - ago); };
    const std::vector notes{meta("yeni", 0, at(chr::hours(1))), meta("sinir", 0, at(chr::days(7))),
                            meta("eski", 0, at(chr::days(8)))};
    CHECK(filtered("updated:7d", notes, now) == Indices{0, 1});
    CHECK(filtered("updated:1d", notes, now) == Indices{0});
}

TEST_CASE("apply treats dates as whole local days: > is after the day, < is before it") {
    const std::vector notes{meta("onceki", 0, local_time(2026, 9, 30, 23, 30)),
                            meta("gun-basi", 0, local_time(2026, 10, 1, 0, 30)),
                            meta("gun-sonu", 0, local_time(2026, 10, 1, 23, 30)),
                            meta("sonraki", 0, local_time(2026, 10, 2, 0, 30))};
    CHECK(filtered("updated:>2026-10-01", notes) == Indices{3});
    CHECK(filtered("updated:<2026-10-01", notes) == Indices{0});
    CHECK(filtered("updated:>2026-09-30 updated:<2026-10-02", notes) == Indices{1, 2});
}

TEST_CASE("apply excludes unknown created and origin values from their filters") {
    std::vector notes{meta("bilinmeyen"), meta("eski"), meta("yeni")};
    notes[1].created_at = local_time(2020, 1, 1, 12);
    notes[1].origin = "Windows";
    notes[2].created_at = local_time(2026, 10, 15, 12);
    notes[2].origin = "linux";
    CHECK(filtered("created:>2026-10-01", notes) == Indices{2});
    CHECK(filtered("created:<2026-10-01", notes) == Indices{1});
    const auto now = chr::sys_days(chr::year(2026) / 10 / 20);
    CHECK(filtered("created:30d", notes, now) == Indices{2});
    CHECK(filtered("origin:WİNDOWS", notes) == Indices{1});
    CHECK(filtered("origin:mac", notes).empty());
}

TEST_CASE("apply filters by sync state") {
    std::vector notes{meta("a"), meta("b"), meta("c")};
    notes[0].sync_status = core::SyncState::Modified;
    notes[1].sync_status = core::SyncState::Synced;
    notes[2].sync_status = core::SyncState::Modified;
    CHECK(filtered("sync:modified", notes) == Indices{0, 2});
    CHECK(filtered("sync:new", notes).empty());
}

TEST_CASE("apply requires every filter and keeps input order without a sort") {
    const std::vector notes{meta("work/z.md", 10), meta("work/a.md", 10), meta("work/b.txt", 10),
                            meta("x/c.md", 10)};
    CHECK(filtered("type:md in:work/", notes) == Indices{0, 1});
    CHECK(filtered("type:md type:txt", notes).empty());
    CHECK(filtered("", notes) == Indices{0, 1, 2, 3});
}

TEST_CASE("apply sorts by name with folding, both directions") {
    const std::vector notes{meta("z/çilek.md"), meta("a/Zeytin.md"), meta("m/armut.md")};
    CHECK(filtered("sort:name", notes) == Indices{2, 0, 1});
    CHECK(filtered("sort:-name", notes) == Indices{1, 0, 2});
}

TEST_CASE("apply sorts by size and updated and is stable") {
    const auto t = [](int hours) { return fs::file_time_type{} + chr::hours(hours); };
    const std::vector notes{meta("a", 2, t(3)), meta("b", 1, t(1)), meta("c", 2, t(3)), meta("d", 3, t(2))};
    CHECK(filtered("sort:size", notes) == Indices{1, 0, 2, 3});
    CHECK(filtered("sort:-size", notes) == Indices{3, 0, 2, 1});  // eşit boyutta gelen sıra korunur
    CHECK(filtered("sort:updated", notes) == Indices{1, 3, 0, 2});
    CHECK(filtered("sort:-updated", notes) == Indices{0, 2, 3, 1});
}

TEST_CASE("apply sorts unknown created values last in both directions") {
    std::vector notes{meta("bilinmeyen1"), meta("eski"), meta("bilinmeyen2"), meta("yeni")};
    notes[1].created_at = fs::file_time_type{} + chr::hours(1);
    notes[3].created_at = fs::file_time_type{} + chr::hours(2);
    CHECK(filtered("sort:created", notes) == Indices{1, 3, 0, 2});
    CHECK(filtered("sort:-created", notes) == Indices{3, 1, 0, 2});
}

namespace {

core::NoteMeta tagged_meta(std::string_view path, std::vector<std::string> tags) {
    return core::NoteMeta{.path = *core::NotePath::parse(path), .tags = std::move(tags)};
}

}  // namespace

TEST_CASE("tag: matches the tag and its nested tags, folded, combinable with text and other arguments") {
    const std::vector<core::NoteMeta> notes = {
        tagged_meta("a.md", {"proje"}),
        tagged_meta("b.md", {"proje/alt"}),
        tagged_meta("c.txt", {"projeler"}),
        tagged_meta("d.md", {"proje/alt/derin", "çalışma"}),
        tagged_meta("e.md", {}),
    };
    CHECK(filtered("tag:proje", notes) == Indices{0, 1, 3});
    CHECK(filtered("tag:proje/", notes) == Indices{0, 1, 3});
    CHECK(filtered("tag:#Proje", notes) == Indices{0, 1, 3});
    CHECK(filtered("tag:proje/alt", notes) == Indices{1, 3});
    CHECK(filtered("tag:calisma", notes) == Indices{3});
    CHECK(filtered("tag:ÇALIŞMA", notes) == Indices{3});
    CHECK(filtered("tag:proje type:md", notes) == Indices{0, 1, 3});
    CHECK(filtered("tag:proje tag:çalışma", notes) == Indices{3});
    const ParsedQuery q = parsed("plan tag:proje toplantı");
    CHECK(q.text == "plan toplantı");
    REQUIRE(q.filters.size() == 1);
    CHECK(q.filters[0].kind == Filter::Kind::Tag);
}

TEST_CASE("tag: without a name is an error at its value") {
    CHECK(located("x tag:#") == "#");
    CHECK(located("x tag:/") == "/");
    CHECK(located("tag:") == "tag:");
}

namespace {

// Depo yerine sabit görünümler: `work` iki not döndürür, `bad` sorgu hatası verir, kalanı yok.
ViewLookup fake_views(int& calls) {
    return [&calls](std::string_view name) -> std::expected<std::vector<core::NotePath>, core::Error> {
        ++calls;
        if (name == "work" || name == "Work")
            return std::vector{*core::NotePath::parse("a.md"), *core::NotePath::parse("x/c.md")};
        if (name == "bad")
            return std::unexpected(core::Error{core::Error::Code::InvalidQuery, "error.view_writes", {}});
        return std::unexpected(core::Error{core::Error::Code::NotFound, "error.view_not_found", {}});
    };
}

}  // namespace

TEST_CASE("view: keeps the notes the view returns, combinable with text and other arguments") {
    const std::vector<core::NoteMeta> notes = {tagged_meta("a.md", {}), tagged_meta("b.md", {"proje"}),
                                               tagged_meta("x/c.md", {"proje"})};
    int calls = 0;
    const ViewLookup views = fake_views(calls);
    const auto query = parse_query("plan view:work tag:proje", views);
    REQUIRE(query.has_value());
    CHECK(calls == 1);
    CHECK(query->text == "plan");
    REQUIRE(query->filters.size() == 2);
    CHECK(query->filters[0].kind == Filter::Kind::View);
    CHECK(apply(query->filters, std::nullopt, notes) == Indices{2});
    const auto quoted = parse_query("view:\"Work\"", views);
    REQUIRE(quoted.has_value());
    CHECK(apply(quoted->filters, std::nullopt, notes) == Indices{0, 2});
}

TEST_CASE("view: errors from the lookup sit at the value; without a lookup it matches nothing") {
    int calls = 0;
    const ViewLookup views = fake_views(calls);
    const auto unknown = parse_query("x view:nope", views);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().error.message_key == "error.view_not_found");
    CHECK(std::string_view("x view:nope").substr(unknown.error().begin) == "nope");
    const auto writes = parse_query("view:bad", views);
    REQUIRE_FALSE(writes.has_value());
    CHECK(writes.error().error.code == core::Error::Code::InvalidQuery);
    CHECK(located("view:") == "view:");
    const auto slashed = parse_query("view:a/b", views);  // ad kuralı depodan önce: sorulmaz
    REQUIRE_FALSE(slashed.has_value());
    CHECK(std::string_view("view:a/b").substr(slashed.error().begin) == "a/b");
    CHECK(calls == 2);

    // Sözdizimi denetimi (girdi boyama) depoya sormaz; süzgeç bağlanmamışsa hiçbir not geçmez.
    const ParsedQuery plain = parsed("view:anything");
    REQUIRE(plain.filters.size() == 1);
    CHECK(apply(plain.filters, std::nullopt, std::vector{tagged_meta("a.md", {})}).empty());
}
