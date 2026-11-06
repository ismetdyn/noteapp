// T81: `note view save|ls|run|rm|show` ve `note ls` içinde `view:`. Görünüm salt-okunurdur: yazan sorgu
// kayıtta ve çalıştırmada reddedilir, grafa doğrudan yazılmış kötü niyetli görünüm hiçbir şey değiştirmez.
#include "graph/graph.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace noteapp::test;

namespace {

constexpr std::string_view kWork = "MATCH (n:Note)-[:TAGGED]->(:Tag {name: 'work'}) RETURN n.name, n";

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

void seed(const Sandbox& box) {
    box.put("plan.md", "#work plan\n", 300);
    box.put("ev.md", "#home\n", 200);
    box.put("iş/rapor.md", "#work rapor\n", 100);
}

void save_ok(const Sandbox& box, const std::vector<std::string>& args) {
    std::vector<std::string> full = {"view", "save"};
    full.insert(full.end(), args.begin(), args.end());
    const auto result = box.run(full);
    INFO(result.err);
    REQUIRE(result.exit_code == 0);
}

}  // namespace

TEST_CASE("view save, ls, show, run and rm") {
    const Sandbox box("en");
    seed(box);
    save_ok(box, {"Work", std::string(kWork)});
    save_ok(box, {"all", "MATCH (n:Note) RETURN count(n) AS notes"});

    const auto listed = box.run({"view", "ls"});
    INFO(listed.err);
    CHECK(listed.exit_code == 0);
    CHECK(lines_of(listed.out) == std::vector<std::string>{"all\tMATCH (n:Note) RETURN count(n) AS notes",
                                                           "Work\t" + std::string(kWork)});

    const auto shown = box.run({"view", "show", "work"});
    CHECK(shown.exit_code == 0);
    CHECK(shown.out == std::string(kWork) + "\n");

    // Çıktı `note query` ile bayt bayt aynıdır.
    const auto ran = box.run({"view", "run", "all"});
    const auto queried = box.run({"query", "MATCH (n:Note) RETURN count(n) AS notes"});
    CHECK(ran.exit_code == 0);
    CHECK(ran.out == "notes\n3\n");
    CHECK(ran.out == queried.out);
    const auto ran_json = box.run({"view", "run", "WORK", "--json"});
    const auto queried_json = box.run({"query", "--json", std::string(kWork)});
    CHECK(ran_json.exit_code == 0);
    CHECK(ran_json.out == queried_json.out);
    CHECK(ran_json.out.starts_with("{\"columns\":[\"n.name\",\"n\"],\"rows\":["));

    const auto removed = box.run({"view", "rm", "WORK"});
    CHECK(removed.exit_code == 0);
    CHECK(lines_of(box.run({"view", "ls"}).out) ==
          std::vector<std::string>{"all\tMATCH (n:Note) RETURN count(n) AS notes"});
    CHECK(box.run({"view", "run", "work"}).exit_code == 1);
    CHECK(box.run({"view", "show", "work"}).exit_code == 1);
    CHECK(box.run({"view", "rm", "work"}).exit_code == 1);
}

TEST_CASE("view ls --json prints name, query and timestamps; an empty list is []") {
    const Sandbox box;
    CHECK(box.run({"view", "ls", "--json"}).out == "[]\n");
    CHECK(box.run({"view", "ls"}).out.empty());
    save_ok(box, {"a", "MATCH (n) RETURN n"});
    const auto listed = box.run({"view", "ls", "--json"});
    CHECK(listed.exit_code == 0);
    CHECK(listed.out.starts_with("[{\"name\":\"a\",\"query\":\"MATCH (n) RETURN n\",\"created_at\":"));
    CHECK(contains(listed.out, ",\"updated_at\":"));
    CHECK(listed.out.ends_with("}]\n"));
}

TEST_CASE("view save does not overwrite without --force") {
    const Sandbox box("en");
    save_ok(box, {"a", "MATCH (n) RETURN n"});
    const auto refused = box.run({"view", "save", "A", "MATCH (n:Note) RETURN n"});
    CHECK(refused.exit_code == 1);
    CHECK(contains(refused.err, "--force"));
    CHECK(box.run({"view", "show", "a"}).out == "MATCH (n) RETURN n\n");
    save_ok(box, {"A", "MATCH (n:Note) RETURN n", "--force"});
    CHECK(box.run({"view", "show", "a"}).out == "MATCH (n:Note) RETURN n\n");
}

TEST_CASE("view save rejects a write clause and saves nothing") {
    const Sandbox box("en");
    seed(box);
    for (const std::string query :
         {"MATCH (n:Note) DETACH DELETE n", "CREATE (:X)", "MATCH (n) SET n.k = 1", "MERGE (:X {k: 1})"}) {
        CAPTURE(query);
        const auto refused = box.run({"view", "save", "bad", query});
        CHECK(refused.exit_code == 2);
        CHECK(contains(refused.err, "read-only"));
    }
    CHECK(box.run({"view", "ls"}).out.empty());
    CHECK(box.run({"view", "save", "bad", "MATCH (n RETURN n"}).exit_code == 2);
    CHECK(box.run({"view", "save", "a/b", "MATCH (n) RETURN n"}).exit_code == 2);
    CHECK(lines_of(box.run({"ls"}).out).size() == 3);
}

TEST_CASE("a view with a write clause written straight into the graph is rejected at run, nothing changes") {
    const Sandbox box("en");
    seed(box);
    // Senkronla gelmiş gibi: kayıt denetimini atlayan görünüm. `query --write` View kuramaz (korunan etiket):
    // ayrı bağlantıdan, motora doğrudan yazılır.
    {
        auto raw = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("CREATE (:View {name: 'evil', query: 'MATCH (n:Note) DETACH DELETE n'})", {},
                           noteapp::graph::QueryMode::ReadWrite)
                    .has_value());
    }
    const auto before = box.run({"query", "MATCH (n) RETURN count(n)"}).out;

    const auto ran = box.run({"view", "run", "evil"});
    CHECK(ran.exit_code == 2);
    CHECK(ran.out.empty());
    CHECK(contains(ran.err, "read-only"));
    const auto filtered = box.run({"ls", "view:evil"});
    CHECK(filtered.exit_code == 2);
    CHECK(filtered.out.empty());
    CHECK(contains(filtered.err, "read-only"));

    CHECK(box.run({"query", "MATCH (n) RETURN count(n)"}).out == before);
    CHECK(lines_of(box.run({"ls"}).out).size() == 3);
    CHECK(box.run({"view", "show", "evil"}).out == "MATCH (n:Note) DETACH DELETE n\n");
}

TEST_CASE("ls takes view: and an unknown view or a view without notes is an argument error") {
    const Sandbox box("en");
    seed(box);
    save_ok(box, {"work", std::string(kWork)});
    save_ok(box, {"count", "MATCH (n:Note) RETURN count(n)"});
    CHECK(lines_of(box.run({"ls", "view:work"}).out) == std::vector<std::string>{"iş/rapor.md", "plan.md"});
    CHECK(lines_of(box.run({"ls", "rap view:WORK"}).out) == std::vector<std::string>{"iş/rapor.md"});

    const auto unknown = box.run({"ls", "view:nope"});
    CHECK(unknown.exit_code == 2);
    CHECK(unknown.out.empty());
    CHECK(contains(unknown.err, "nope"));
    const auto counted = box.run({"ls", "view:count"});
    CHECK(counted.exit_code == 2);
    CHECK(contains(counted.err, "count"));
}

TEST_CASE("view word is reserved, has help and a usage error") {
    const Sandbox box("en");
    const auto help = box.run({"help", "view"});
    CHECK(help.exit_code == 0);
    CHECK(contains(help.out, "note view save"));
    CHECK(contains(box.run({"help"}).out, "note view"));
    CHECK(box.run({"view"}).exit_code == 2);
    CHECK(box.run({"view", "bogus"}).exit_code == 2);
    CHECK(box.run({"view", "run"}).exit_code == 2);
    CHECK(box.run({"view", "ls", "--bogus"}).exit_code == 2);
    CHECK_FALSE(box.has_any_note());  // `view` metin olarak not eklemez
}

// L4: görünüm başka cihazdan gelmiş olabilir; kökeni izlenmediğinden her görünüm limits.view.* ile çalışır.
// limits.query.* yalnız `note query`'nindir: sınırsız kullanıcı profili görünümü gevşetmez.
TEST_CASE("views run under limits.view; the user's limits.query does not loosen them") {
    const Sandbox box("en");
    seed(box);
    save_ok(box, {"all", "MATCH (n:Note) RETURN n"});
    {
        std::ofstream config(box.config_file(), std::ios::binary | std::ios::app);
        config << "[limits.query]\nrows = 0\n[limits.view]\nrows = 2\n";
    }
    const auto ran = box.run({"view", "run", "all"});
    CHECK(ran.exit_code != 0);
    CHECK(contains(ran.err, "rows"));
    const auto listed = box.run({"ls", "view:all"});
    CHECK(listed.exit_code != 0);
    CHECK(contains(listed.err, "rows"));
    const auto queried = box.run({"query", "MATCH (n:Note) RETURN n.name"});
    INFO(queried.err);
    CHECK(queried.exit_code == 0);
    CHECK(lines_of(queried.out).size() == 4);  // başlık + 3 not
}
