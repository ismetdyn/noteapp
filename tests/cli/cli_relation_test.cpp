// F1: `note relation add|rm|rename|ls` (SPEC-cli "İlişkiler"). Kural NoteStore'un ve app::map_relations'ın;
// burada argümanlar, çıktı biçimi, çıkış kodları ve güvenilmez adın temizliği gerçek `note` ikilisiyle.
#include "core/note_store.hpp"
#include "graph/graph.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <sstream>
#include <string>
#include <vector>

using namespace noteapp::test;
namespace core = noteapp::core;
namespace graph = noteapp::graph;

namespace {

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

void seed(const Sandbox& box) {
    box.put("a.md", "a\n");
    box.put("b.md", "b\n");
    box.put("c.md", "c\n");
}

std::vector<core::RelatedNote> related(const Sandbox& box, const std::string& path) {
    return core::NoteStore(box.data()).links(np(path)).value().related;
}

void run_ok(const Sandbox& box, const std::vector<std::string>& args) {
    const auto result = box.run(args);
    INFO(result.err);
    REQUIRE(result.exit_code == 0);
}

std::string hex_of(const core::RelationId& id) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::byte byte : id.bytes) {
        out += kDigits[std::to_integer<unsigned>(byte) >> 4U];
        out += kDigits[std::to_integer<unsigned>(byte) & 0xFU];
    }
    return out;
}

}  // namespace

TEST_CASE("relation add relates two notes, unnamed and undirected unless asked") {
    const Sandbox box("en");
    seed(box);
    const auto plain = box.run({"relation", "add", "a.md", "b.md"});
    INFO(plain.err);
    CHECK(plain.exit_code == 0);
    CHECK(plain.out == "Related: a.md and b.md\n");
    const auto named = box.run({"relation", "add", "a.md", "c.md", "--name", " uses ", "--directed"});
    INFO(named.err);
    CHECK(named.exit_code == 0);
    CHECK(named.out == "Related as 'uses': a.md and c.md\n");
    const auto rows = related(box, "a.md");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].path == np("b.md"));
    CHECK(rows[0].name.empty());
    CHECK(rows[0].direction == core::RelationDirection::Undirected);
    CHECK(rows[1].path == np("c.md"));
    CHECK(rows[1].name == "uses");
    CHECK(rows[1].direction == core::RelationDirection::Outgoing);
}

TEST_CASE("relation add refuses a duplicate, a missing note, itself and a bad name with the right exit code") {
    const Sandbox box("en");
    seed(box);
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "uses"});
    CHECK(box.run({"relation", "add", "b.md", "a.md", "--name", "USES"}).exit_code == 1);
    CHECK(box.run({"relation", "add", "a.md", "nope.md"}).exit_code == 1);
    CHECK(box.run({"relation", "add", "a.md", "a.md"}).exit_code == 2);
    CHECK(box.run({"relation", "add", "a.md", "b.md", "--name", "a\tb"}).exit_code == 2);
    CHECK(box.run({"relation", "add", "a.md"}).exit_code == 2);
    CHECK(box.run({"relation", "add", "a.md", "b.md", "--name"}).exit_code == 2);
    CHECK(box.run({"relation", "add", "a.md", "b.md", "--bogus"}).exit_code == 2);
    CHECK(box.run({"relation"}).exit_code == 2);
    CHECK(related(box, "a.md").size() == 1);
}

TEST_CASE("relation ls lists names with counts; ls NOTE lists its relations; --json for both") {
    const Sandbox box("en");
    seed(box);
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "uses", "--directed"});
    run_ok(box, {"relation", "add", "c.md", "a.md", "--name", "Uses", "--directed"});
    run_ok(box, {"relation", "add", "b.md", "c.md", "--name", "cites"});
    run_ok(box, {"relation", "add", "a.md", "b.md"});

    // Gösterim en çok kullanılan yazımdır, eşitlikte bayt sırası küçük olan ("Uses" < "uses").
    CHECK(lines_of(box.run({"relation", "ls"}).out) == std::vector<std::string>{"Uses\t2", "cites\t1"});
    CHECK(box.run({"relation", "ls", "--json"}).out ==
          "[{\"name\":\"uses\",\"display\":\"Uses\",\"count\":2,\"directed\":2},"
          "{\"name\":\"cites\",\"display\":\"cites\",\"count\":1,\"directed\":0}]\n");

    const auto rows = related(box, "a.md");
    REQUIRE(rows.size() == 3);
    const auto listed = box.run({"relation", "ls", "a.md"});
    INFO(listed.err);
    CHECK(listed.exit_code == 0);
    CHECK(lines_of(listed.out) == std::vector<std::string>{
                                      "b.md\t\tundirected\t" + hex_of(rows[0].id),
                                      "b.md\tuses\toutgoing\t" + hex_of(rows[1].id),
                                      "c.md\tUses\tincoming\t" + hex_of(rows[2].id),
                                  });
    CHECK(box.run({"relation", "ls", "a.md", "--json"}).out ==
          "[{\"path\":\"b.md\",\"name\":\"\",\"direction\":\"undirected\",\"id\":\"" + hex_of(rows[0].id) +
              "\"},{\"path\":\"b.md\",\"name\":\"uses\",\"direction\":\"outgoing\",\"id\":\"" +
              hex_of(rows[1].id) + "\"},{\"path\":\"c.md\",\"name\":\"Uses\",\"direction\":\"incoming\",\"id\":\"" +
              hex_of(rows[2].id) + "\"}]\n");
    CHECK(box.run({"relation", "ls", "nope.md"}).exit_code == 1);
    CHECK(box.run({"relation", "ls", "a.md", "b.md"}).exit_code == 2);
}

TEST_CASE("relation rm takes the only relation, or one chosen by --name or --id; several need a choice") {
    const Sandbox box("en");
    seed(box);
    run_ok(box, {"relation", "add", "a.md", "b.md"});
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "uses"});
    run_ok(box, {"relation", "add", "a.md", "c.md", "--name", "cites"});

    const auto several = box.run({"relation", "rm", "a.md", "b.md"});
    CHECK(several.exit_code == 1);
    CHECK(several.err.find("--name") != std::string::npos);
    CHECK(related(box, "a.md").size() == 3);

    const auto by_name = box.run({"relation", "rm", "b.md", "a.md", "--name", "USES"});
    INFO(by_name.err);
    CHECK(by_name.exit_code == 0);
    CHECK(by_name.out == "Relation 'uses' removed: b.md and a.md\n");

    const auto rows = related(box, "a.md");
    REQUIRE(rows.size() == 2);
    const auto by_id = box.run({"relation", "rm", "a.md", "b.md", "--id", hex_of(rows[0].id)});
    INFO(by_id.err);
    CHECK(by_id.exit_code == 0);
    CHECK(by_id.out == "Relation removed: a.md and b.md\n");

    const auto only = box.run({"relation", "rm", "c.md", "a.md"});
    INFO(only.err);
    CHECK(only.exit_code == 0);
    CHECK(only.out == "Relation 'cites' removed: c.md and a.md\n");
    CHECK(related(box, "a.md").empty());

    CHECK(box.run({"relation", "rm", "a.md", "b.md"}).exit_code == 1);
    CHECK(box.run({"relation", "rm", "a.md", "b.md", "--id", "xyz"}).exit_code == 2);
    CHECK(box.run({"relation", "rm", "a.md", "b.md", "--name", "x", "--id", hex_of(rows[0].id)}).exit_code ==
          2);
}

TEST_CASE("relation rename changes the name of the relation called OLD; an empty name means unnamed") {
    const Sandbox box("en");
    seed(box);
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "uses", "--directed"});
    const auto renamed = box.run({"relation", "rename", "b.md", "a.md", "USES", "cites"});
    INFO(renamed.err);
    CHECK(renamed.exit_code == 0);
    CHECK(renamed.out == "Relation renamed to 'cites': b.md and a.md\n");
    auto rows = related(box, "a.md");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "cites");
    CHECK(rows[0].direction == core::RelationDirection::Outgoing);

    run_ok(box, {"relation", "rename", "a.md", "b.md", "cites", ""});
    CHECK(related(box, "a.md")[0].name.empty());
    run_ok(box, {"relation", "rename", "a.md", "b.md", "", "again"});
    CHECK(related(box, "a.md")[0].name == "again");
    CHECK(box.run({"relation", "rename", "a.md", "b.md", "nope", "x"}).exit_code == 1);
    CHECK(box.run({"relation", "rename", "a.md", "b.md", "again"}).exit_code == 2);
    CHECK(box.run({"relation", "rename", "a.md", "b.md", "again", "a\nb"}).exit_code == 2);
}

TEST_CASE("relation names from sync are printed without control or hidden characters; JSON escapes") {
    const Sandbox box("en");
    seed(box);
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "x"});
    {
        auto raw = graph::Graph::open(box.data() / "graph.db");
        REQUIRE(raw.has_value());
        const std::string hostile = "a\x1b[31m\xE2\x80\xAE" "b\"";
        REQUIRE(raw->query("MATCH ()-[r:RELATED_TO]->() SET r.name = $name",
                           {{"name", graph::QueryValue(hostile)}}, graph::QueryMode::ReadWrite)
                    .has_value());
    }
    for (const auto& args : {std::vector<std::string>{"relation", "ls"},
                             std::vector<std::string>{"relation", "ls", "a.md"}}) {
        const auto result = box.run(args);
        INFO(result.err);
        CHECK(result.exit_code == 0);
        CHECK(result.out.find('\x1b') == std::string::npos);
        CHECK(result.out.find("\xE2\x80\xAE") == std::string::npos);
        CHECK(result.out.find("a\xEF\xBF\xBD[31m\xEF\xBF\xBD" "b\"") != std::string::npos);
    }
    const auto json = box.run({"relation", "ls", "a.md", "--json"});
    CHECK(json.out.find("\"name\":\"a\xEF\xBF\xBD[31m\xEF\xBF\xBD" "b\\\"\"") != std::string::npos);
}

TEST_CASE("relation help names every subcommand; completion offers the verbs and options") {
    const Sandbox box("en");
    const auto help = box.run({"help", "relation"});
    INFO(help.err);
    CHECK(help.exit_code == 0);
    for (const char* word : {"add", "rm", "rename", "ls", "--name", "--directed", "--id", "--json"})
        CHECK_MESSAGE(help.out.find(word) != std::string::npos, word);
    const auto bash = box.run({"completion", "bash"});
    CHECK(bash.out.find("relation") != std::string::npos);
    CHECK(bash.out.find("--directed") != std::string::npos);
}
