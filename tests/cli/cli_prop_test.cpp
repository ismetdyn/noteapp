// F4: `note prop get|set|rm` ve `note relation prop …` (SPEC-cli "Özellikler"). Kural core'un (sorgu
// çubuğunun koruması), ileti app::Inspector'ın; burada argümanlar, çıktı (`--json`), çıkış kodları ve
// güvenilmez metnin temizliği gerçek `note` ikilisiyle.
#include "core/note_props.hpp"
#include "core/note_store.hpp"
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

RunResult run_ok(const Sandbox& box, const std::vector<std::string>& args) {
    auto result = box.run(args);
    INFO(result.err);
    REQUIRE(result.exit_code == 0);
    return result;
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

graph::Value stored(const Sandbox& box, const std::string& path, std::string_view key) {
    const auto props = core::note_properties(core::NoteStore(box.data()), np(path));
    REQUIRE(props.has_value());
    for (const core::Property& property : *props)
        if (property.key == key)
            return property.value;
    return graph::Value{};
}

}  // namespace

TEST_CASE("prop set, get and rm round-trip typed values on a note") {
    const Sandbox box("en");
    box.put("a.md", "a\n");
    CHECK(run_ok(box, {"prop", "set", "a.md", "status", "draft"}).out == "Set status = draft\n");
    run_ok(box, {"prop", "set", "a.md", "priority", "3"});
    run_ok(box, {"prop", "set", "a.md", "score", "-2.5"});  // eksi sayı değerdir, seçenek değil
    run_ok(box, {"prop", "set", "a.md", "done", "false"});
    run_ok(box, {"prop", "set", "a.md", "code", "42", "--type", "text"});
    run_ok(box, {"prop", "set", "a.md", "dash", "--", "-x"});
    CHECK(stored(box, "a.md", "priority") == graph::Value{std::int64_t{3}});
    CHECK(stored(box, "a.md", "score") == graph::Value{-2.5});
    CHECK(stored(box, "a.md", "done") == graph::Value{false});
    CHECK(stored(box, "a.md", "code") == graph::Value{std::string("42")});
    CHECK(stored(box, "a.md", "dash") == graph::Value{std::string("-x")});

    CHECK(run_ok(box, {"prop", "get", "a.md", "priority"}).out == "3\n");
    CHECK(run_ok(box, {"prop", "get", "a.md", "code", "--json"}).out == "\"42\"\n");
    CHECK(run_ok(box, {"prop", "get", "a.md", "done", "--json"}).out == "false\n");
    const std::vector<std::string> lines = lines_of(run_ok(box, {"prop", "get", "a.md"}).out);
    CHECK(std::ranges::find(lines, "status\ttext\tdraft") != lines.end());
    CHECK(std::ranges::find(lines, "priority\tnumber\t3") != lines.end());
    CHECK(std::ranges::find(lines, "type\ttext\tmd\tread-only") != lines.end());
    const std::string json = run_ok(box, {"prop", "get", "a.md", "--json"}).out;
    CHECK(json.starts_with("[{\"key\":"));
    CHECK(json.find("{\"key\":\"priority\",\"type\":\"number\",\"value\":3,\"read_only\":false}") !=
          std::string::npos);
    CHECK(json.find("{\"key\":\"type\",\"type\":\"text\",\"value\":\"md\",\"read_only\":true}") !=
          std::string::npos);

    CHECK(run_ok(box, {"prop", "rm", "a.md", "status"}).out == "Removed property status\n");
    CHECK(std::holds_alternative<std::monostate>(stored(box, "a.md", "status")));
}

TEST_CASE("prop refuses reserved keys, bad keys, bad values and missing targets with the right exit code") {
    const Sandbox box("en");
    box.put("a.md", "a\n");
    const auto reserved = box.run({"prop", "set", "a.md", "type", "txt"});
    CHECK(reserved.exit_code == 2);
    CHECK(reserved.err.find("'type' is managed by the app") != std::string::npos);
    CHECK(box.run({"prop", "rm", "a.md", "name"}).exit_code == 2);
    CHECK(box.run({"prop", "set", "a.md", "", "x"}).exit_code == 2);
    const auto bad_number = box.run({"prop", "set", "a.md", "n", "abc", "--type", "number"});
    CHECK(bad_number.exit_code == 2);
    CHECK(bad_number.err.find("not a number") != std::string::npos);
    CHECK(box.run({"prop", "set", "a.md", "n", "1", "--type", "date"}).exit_code == 2);
    CHECK(box.run({"prop", "set", "a.md", "n", "1", "--type"}).exit_code == 2);
    CHECK(box.run({"prop", "set", "nope.md", "n", "1"}).exit_code == 1);
    CHECK(box.run({"prop", "get", "nope.md"}).exit_code == 1);
    CHECK(box.run({"prop", "get", "a.md", "missing"}).exit_code == 1);
    CHECK(box.run({"prop", "rm", "a.md", "missing"}).exit_code == 1);
    CHECK(box.run({"prop", "set", "a.md", "n"}).exit_code == 2);
    CHECK(box.run({"prop", "get", "a.md", "--type", "text"}).exit_code == 2);
    CHECK(box.run({"prop", "frob", "a.md"}).exit_code == 2);
    CHECK(box.run({"prop"}).exit_code == 2);
    CHECK(box.run({"prop", "set", "a.md", "n", "1", "--bogus"}).exit_code == 2);
    CHECK(box.body("a.md") == "a\n");
}

TEST_CASE("relation prop works on a relation by id; its name and direction stay read-only") {
    const Sandbox box("en");
    box.put("a.md", "a\n");
    box.put("b.md", "b\n");
    run_ok(box, {"relation", "add", "a.md", "b.md", "--name", "uses", "--directed"});
    const auto links = core::NoteStore(box.data()).links(np("a.md"));
    REQUIRE(links.has_value());
    const std::string id = hex_of(links->related.front().id);

    CHECK(run_ok(box, {"relation", "prop", "set", id, "weight", "3"}).out == "Set weight = 3\n");
    CHECK(run_ok(box, {"relation", "prop", "get", id, "weight"}).out == "3\n");
    const std::string json = run_ok(box, {"relation", "prop", "get", id, "--json"}).out;
    CHECK(json.find("{\"key\":\"name\",\"type\":\"text\",\"value\":\"uses\",\"read_only\":true}") !=
          std::string::npos);
    CHECK(json.find("{\"key\":\"weight\",\"type\":\"number\",\"value\":3,\"read_only\":false}") !=
          std::string::npos);
    CHECK(box.run({"relation", "prop", "set", id, "name", "x"}).exit_code == 2);
    CHECK(box.run({"relation", "prop", "rm", id, "directed"}).exit_code == 2);
    CHECK(box.run({"relation", "prop", "get", "zz"}).exit_code == 2);
    CHECK(box.run({"relation", "prop", "get", std::string(32, '0')}).exit_code == 1);
    CHECK(box.run({"relation", "prop", "frob", id}).exit_code == 2);
    run_ok(box, {"relation", "prop", "rm", id, "weight"});
    CHECK(box.run({"relation", "prop", "get", id, "weight"}).exit_code == 1);
}

TEST_CASE("prop output strips terminal control from synced keys and values; ls filters by prop:") {
    const Sandbox box("en");
    box.put("a.md", "a\n");
    box.put("b.md", "b\n");
    {
        core::NoteStore store(box.data());
        REQUIRE(
            core::set_note_property(store, np("a.md"), "k\x1b]0;x\x07", graph::Value{std::string("v\x1b[2J")})
                .has_value());
        REQUIRE(core::set_note_property(store, np("b.md"), "status", graph::Value{std::string("Done")})
                    .has_value());
    }
    const std::string out = run_ok(box, {"prop", "get", "a.md"}).out;
    CHECK(out.find('\x1b') == std::string::npos);
    CHECK(out.find('\x07') == std::string::npos);
    CHECK(run_ok(box, {"ls", "prop:status=done"}).out == "b.md\n");
    CHECK(run_ok(box, {"ls", "prop:status"}).out == "b.md\n");
}
