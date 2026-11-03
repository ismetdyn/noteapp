// T80: `note tags [--json]`, `note ls` içinde `tag:` ve TAGGED üzerinde Cypher.
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <sstream>
#include <string>
#include <vector>

using namespace noteapp::test;

namespace {

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

void seed(const Sandbox& box) {
    box.put("plan.md", "#Proje toplantı\n", 300);
    box.put("alt.md", "#proje/alt ayrıntı\n", 200);
    box.put("ev.md", "#çalışma #ev\n", 100);
    box.put("kod.py", "#include\n", 50);
}

}  // namespace

TEST_CASE("tags lists every live tag with its note count, sorted by name") {
    const Sandbox box;
    seed(box);
    const auto result = box.run({"tags"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(lines_of(result.out) ==
          std::vector<std::string>{"ev\t1", "Proje\t1", "proje/alt\t1", "çalışma\t1"});
}

TEST_CASE("tags --json prints name, display and count") {
    const Sandbox box;
    seed(box);
    box.put("ikinci.md", "#proje\n");
    const auto result = box.run({"tags", "--json"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(result.out == "[{\"name\":\"ev\",\"display\":\"ev\",\"count\":1},"
                        "{\"name\":\"proje\",\"display\":\"Proje\",\"count\":2},"
                        "{\"name\":\"proje/alt\",\"display\":\"proje/alt\",\"count\":1},"
                        "{\"name\":\"çalışma\",\"display\":\"çalışma\",\"count\":1}]\n");
}

TEST_CASE("tags without notes prints nothing, --json an empty list; an unknown flag is a usage error") {
    const Sandbox box;
    CHECK(box.run({"tags"}).out.empty());
    CHECK(box.run({"tags", "--json"}).out == "[]\n");
    CHECK(box.run({"tags", "--bogus"}).exit_code == 2);
}

TEST_CASE("ls takes query arguments: tag: with nesting, combined with text") {
    const Sandbox box;
    seed(box);
    CHECK(lines_of(box.run({"ls", "tag:proje"}).out) == std::vector<std::string>{"alt.md", "plan.md"});
    CHECK(lines_of(box.run({"ls", "tag:proje/alt"}).out) == std::vector<std::string>{"alt.md"});
    CHECK(lines_of(box.run({"ls", "plan tag:proje"}).out) == std::vector<std::string>{"plan.md"});
    CHECK(lines_of(box.run({"ls", "tag:calisma"}).out) == std::vector<std::string>{"ev.md"});
    const auto bad = box.run({"ls", "tag:#"});
    CHECK(bad.exit_code == 2);
    CHECK(bad.out.empty());
    CHECK(bad.err.find("tag") != std::string::npos);
}

TEST_CASE("Cypher matches notes through TAGGED") {
    const Sandbox box;
    seed(box);
    const auto result =
        box.run({"query", "MATCH (n:Note)-[:TAGGED]->(t:Tag {name:'proje'}) RETURN n.name ORDER BY n.name"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(result.out == "n.name\nplan\n");
}

TEST_CASE("tags word is reserved and help describes it") {
    const Sandbox box;
    const auto help = box.run({"help", "tags"});
    CHECK(help.exit_code == 0);
    CHECK(help.out.find("note tags") != std::string::npos);
    CHECK(box.run({"help"}).out.find("tags") != std::string::npos);
}
