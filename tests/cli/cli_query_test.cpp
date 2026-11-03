// note query: tablo, --json, --write, --param, çıkış kodları (SPEC-cli "Çıkış kodları"). Depo <veri
// dizini>/graph.db.
#include "graph/graph.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <vector>

using namespace noteapp::test;

namespace {

fs::path graph_db(const Sandbox& box) {
    return box.data() / "graph.db";
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

void write_ok(const Sandbox& box, const std::vector<std::string>& args) {
    std::vector<std::string> full = {"query", "--write"};
    full.insert(full.end(), args.begin(), args.end());
    const auto result = box.run(full);
    INFO(result.err);
    REQUIRE(result.exit_code == 0);
}

}  // namespace

TEST_CASE("query_without_database_is_a_clear_user_error_and_creates_nothing") {
    const Sandbox box;
    const auto result = box.run({"query", "MATCH (n) RETURN n"});
    CHECK(result.exit_code == 1);
    CHECK(contains(result.err, "--write"));
    CHECK(contains(result.err, "graph.db"));
    CHECK_FALSE(fs::exists(graph_db(box)));
}

TEST_CASE("query_write_creates_the_database_and_reads_back_as_a_table") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Note {name: 'plan', size: 3}), (:Note {name: 'ödev', size: 12})"});
    CHECK(fs::exists(graph_db(box)));

    const auto result = box.run({"query", "MATCH (n:Note) RETURN n.name AS name, n.size ORDER BY n.size"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(result.out == "name | n.size\n"
                        "plan | 3\n"
                        "ödev | 12\n");
    // Yazma sonucu da gösterilir; satırsız yazma sessizdir.
    const auto returned = box.run({"query", "--write", "CREATE (n:X {v: 1}) RETURN n.v"});
    CHECK(returned.exit_code == 0);
    CHECK(returned.out == "n.v\n1\n");
    const auto silent = box.run({"query", "--write", "CREATE (:X)"});
    CHECK(silent.exit_code == 0);
    CHECK(silent.out.empty());
}

// Tablo insan içindir ve her zaman temizlenir (denetim baytı gibi); betik ham değeri --json ile alır.
TEST_CASE("query_table_shows_bidi_characters_as_a_marker_and_json_keeps_them") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Note {name: 'gpj\xE2\x80\xAE"
                   "exe'})"});
    const auto table = box.run({"query", "MATCH (n:Note) RETURN n.name"});
    CHECK(table.exit_code == 0);
    CHECK(table.out == "n.name\ngpj\xEF\xBF\xBD"
                       "exe\n");
    const auto json = box.run({"query", "--json", "MATCH (n:Note) RETURN n.name"});
    CHECK(json.exit_code == 0);
    CHECK_FALSE(contains(json.out, "\xEF\xBF\xBD"));
}

TEST_CASE("query_is_read_only_without_write_flag") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Note {name: 'a'})"});
    for (const char* text : {"CREATE (:Note)", "MATCH (n) DELETE n", "MATCH (n) SET n.x = 1", "MERGE (:M)"}) {
        const auto result = box.run({"query", text});
        INFO(text);
        CHECK(result.exit_code == 2);
        CHECK(contains(result.err, "--write"));
        CHECK(contains(result.err, "1"));
    }
    const auto count = box.run({"query", "--json", "MATCH (n) RETURN count(*)"});
    CHECK(contains(count.out, "[[1]]"));
}

TEST_CASE("query_exit_codes_follow_spec_cli") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Note)"});
    // Kullanım hatası: 2.
    CHECK(box.run({"query"}).exit_code == 2);
    CHECK(box.run({"query", "RETURN 1", "RETURN 2"}).exit_code == 2);
    CHECK(box.run({"query", "--bogus", "RETURN 1"}).exit_code == 2);
    CHECK(box.run({"query", "--param"}).exit_code == 2);
    CHECK(box.run({"query", "--param", "noequals", "RETURN 1"}).exit_code == 2);
    CHECK(box.run({"query", "--param", "=1", "RETURN 1"}).exit_code == 2);
    CHECK(box.run({"query", "--param", "9x=1", "RETURN 1"}).exit_code == 2);
    const auto syntax = box.run({"query", "MATCH (n RETURN n"});
    CHECK(syntax.exit_code == 2);
    CHECK(contains(syntax.err, "1"));
    CHECK(syntax.out.empty());
    const auto unsupported = box.run({"query", "LOAD CSV FROM 'file:///etc/passwd' AS x RETURN x"});
    CHECK(unsupported.exit_code == 2);
    CHECK(contains(unsupported.err, "LOAD"));
    CHECK_FALSE(contains(unsupported.out + unsupported.err, "root:"));
    CHECK(box.run({"query", "RETURN $missing"}).exit_code == 2);
    // Kullanıcı kaynaklı: 1.
    CHECK(box.run({"query", "RETURN 1 / 0"}).exit_code == 1);
    CHECK(box.run({"query", "RETURN (-9223372036854775807 - 1) / -1"}).exit_code == 1);
#ifndef _WIN32  // cmd /c komutu 8191 karakterle sınırlı: run_process bu argümanı Windows'ta taşıyamaz
    const std::string huge = "RETURN '" + std::string(70000, 'x') + "'";
    CHECK(box.run({"query", huge}).exit_code == 1);
#endif
    // Başarı.
    CHECK(box.run({"query", "RETURN 1"}).exit_code == 0);
    CHECK(box.run({"query", "--", "RETURN 1"}).exit_code == 0);
}

TEST_CASE("query_foreign_file_is_an_internal_error") {
    const Sandbox box;
    fs::create_directories(graph_db(box).parent_path());
    std::ofstream(graph_db(box), std::ios::binary) << std::string(8192, 'q');
    const auto result = box.run({"query", "RETURN 1"});
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.err.empty());
}

TEST_CASE("query_parameters_are_typed_and_cannot_inject") {
    const Sandbox box;
    write_ok(box, {"CREATE (:P {i: 7, s: 'x', b: true})"});
    const auto run_one = [&](const std::string& param, const std::string& text) {
        return box.run({"query", "--json", "--param", param, text});
    };
    CHECK(contains(run_one("v=7", "MATCH (n:P) WHERE n.i = $v RETURN n.s").out, "[[\"x\"]]"));
    CHECK(contains(run_one("v=7.5", "RETURN $v").out, "[[7.5]]"));
    CHECK(contains(run_one("v=true", "MATCH (n:P) WHERE n.b = $v RETURN n.s").out, "[[\"x\"]]"));
    CHECK(contains(run_one("v=null", "RETURN $v").out, "[[null]]"));
    CHECK(contains(run_one("v=hello", "RETURN $v").out, "[[\"hello\"]]"));
    // Tırnaklı değer her zaman metindir; sonraki = değere dahildir.
#ifndef _WIN32  // run_process argümanda `"` taşımaz (Windows: cmd /c)
    CHECK(contains(run_one("v=\"12\"", "RETURN $v").out, "[[\"12\"]]"));
#endif
    CHECK(contains(run_one("v=a=b", "RETURN $v").out, "[[\"a=b\"]]"));
    // Değer sorgu metnine karışmaz.
    CHECK(contains(run_one("v=x'}) DETACH DELETE n //", "MATCH (n:P) WHERE n.s = $v RETURN count(*)").out,
                   "[[0]]"));
    CHECK(contains(box.run({"query", "--json", "MATCH (n:P) RETURN count(*)"}).out, "[[1]]"));
    // Birden çok parametre.
    const auto both = box.run({"query", "--json", "--param", "a=1", "--param", "b=2", "RETURN $a + $b"});
    CHECK(contains(both.out, "[[3]]"));
}

#ifndef _WIN32  // run_process argümanda `"` taşımaz (Windows: cmd /c)
TEST_CASE("query_json_is_properly_escaped_and_bytes_are_base64") {
    const Sandbox box;
    write_ok(box, {R"(CREATE (n:J {t: 'a\"b\\c\nd\u0001e\u001b[31m', u: 'ğ'}))"});
    const auto result =
        box.run({"query", "--json", "MATCH (n:J) RETURN n.t AS t, n.u AS u, labels(n) AS l, n.absent AS a"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(result.out == "{\"columns\":[\"t\",\"u\",\"l\",\"a\"],\"rows\":[[\"a\\\"b\\\\c\\nd\\u0001e\\u001b["
                        "31m\",\"ğ\",[\"J\"],null]]}\n");

    const auto ids = box.run({"query", "--json", "CREATE (n:K)-[r:T]->(m) RETURN n, r", "--write"});
    CHECK(contains(ids.out, "{\"$node\":\""));
    CHECK(contains(ids.out, "{\"$edge\":\""));
}
#endif

TEST_CASE("query_bytes_stay_exact_in_json_and_opaque_in_the_table") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Z)"});
    {
        // Baytlar CLI'dan girilemez: bu değerler yalnız kitaplık API'siyle yazılır.
        auto graph = noteapp::graph::Graph::open(graph_db(box));
        REQUIRE(graph.has_value());
        auto txn = graph->write();
        REQUIRE(txn.has_value());
        const auto id = noteapp::graph::new_node_id();
        REQUIRE(txn->put_node(id).has_value());
        REQUIRE(txn->add_label(id, "B").has_value());
        const std::string raw("\xFF\x00\r\n\x1B", 5);
        REQUIRE(txn->set_property(id, "two", noteapp::graph::Bytes{raw.substr(0, 2)}).has_value());
        REQUIRE(txn->set_property(id, "three", noteapp::graph::Bytes{raw.substr(0, 3)}).has_value());
        REQUIRE(txn->set_property(id, "five", noteapp::graph::Bytes{raw}).has_value());
        REQUIRE(txn->set_property(id, "at", noteapp::graph::Timestamp{1000}).has_value());
        REQUIRE(txn->commit().has_value());
    }
    const std::string text = "MATCH (n:B) RETURN n.two, n.three, n.five, n.at";
    const auto json = box.run({"query", "--json", text});
    INFO(json.err);
    CHECK(json.exit_code == 0);
    CHECK(contains(json.out, "[[{\"$bytes\":\"/wA=\"},{\"$bytes\":\"/wAN\"},{\"$bytes\":\"/wANChs=\"},"
                             "{\"$timestamp\":1000}]]"));
    const auto table = box.run({"query", text});
    CHECK(table.exit_code == 0);
    CHECK(contains(table.out, "<bytes:5>"));
    CHECK_FALSE(contains(table.out, "\x1b"));

    const auto number = box.run({"query", "--json", "RETURN 1.5, -9223372036854775808, true, 'x'"});
    CHECK(contains(number.out, "[[1.5,-9223372036854775808,true,\"x\"]]"));
}

TEST_CASE("query_table_is_sanitized_for_terminals") {
    const Sandbox box;
    write_ok(box, {"CREATE (:T {evil: 'a\u001b]52;c;xx\u0007b', multi: 'x\ny\tz\r'})"});
    const auto result = box.run({"query", "MATCH (n:T) RETURN n.evil, n.multi, labels(n)"});
    CHECK(result.exit_code == 0);
    // OSC 52 ve BEL geçmez; hücre tek satırda kalır.
    CHECK_FALSE(contains(result.out, "\x1b"));
    CHECK_FALSE(contains(result.out, "\x07"));
    CHECK_FALSE(contains(result.out, "\r"));
    CHECK(contains(result.out, "x\\ny\\tz"));
    CHECK(contains(result.out, "[T]"));
    // Hata iletisindeki kullanıcı metni de temizlenir.
    const auto error = box.run({"query", "RETURN evil\x1b(1)"});
    CHECK(error.exit_code == 2);
    CHECK_FALSE(contains(error.err, "\x1b"));
}

TEST_CASE("query_word_is_reserved_and_help_describes_it") {
    const Sandbox box;
    const auto help = box.run({"help", "query"});
    CHECK(help.exit_code == 0);
    CHECK(contains(help.out, "--write"));
    CHECK(contains(box.run({"--help"}).out, "note query"));
    // Aynı metinde not eklemek hâlâ mümkün.
    const auto added = box.run({"--", "query"});
    CHECK(added.exit_code == 0);
    CHECK(box.has_any_note());
}

// Güvenlik incelemesi bulgu 7: inf ve nan sessizce sonlu olmayan sayıya dönüyordu.
TEST_CASE("query_parameter_inf_and_nan_are_plain_text") {
    const Sandbox box;
    write_ok(box, {"CREATE (:Z)"});
    for (const char* text :
         {"inf", "-inf", "Inf", "INF", "infinity", "Infinity", "nan", "NaN", "NAN", "-nan"}) {
        const auto result = box.run({"query", "--json", "--param", std::string("v=") + text, "RETURN $v"});
        INFO(text);
        CHECK(result.exit_code == 0);
        CHECK(contains(result.out, "[[\"" + std::string(text) + "\"]]"));
    }
    // Sonlu sayılar değişmez; aralık dışı da metindir.
    CHECK(contains(box.run({"query", "--json", "--param", "v=1e3", "RETURN $v"}).out, "[[1000]]"));
    CHECK(contains(box.run({"query", "--json", "--param", "v=1e999", "RETURN $v"}).out, "[[\"1e999\"]]"));
}
