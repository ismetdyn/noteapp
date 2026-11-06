// İlişki araması: etiket, ilişki adı ve bağlı notun adı aramaya girer; `rel:` ve `linked:` süzgeçleri.
#include "search/relation_search.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <numeric>
#include <string>
#include <vector>

using namespace noteapp;
using namespace noteapp::search;
using Kind = RelationReason::Kind;
using core::EdgeKind;
using core::RelationDirection;

namespace {

core::NotePath np(std::string_view path) {
    return core::NotePath::parse(path).value();
}

core::NoteMeta meta(std::string_view path, std::vector<std::string> tags = {}, int minutes = 0) {
    return core::NoteMeta{.path = np(path),
                          .updated_at = std::filesystem::file_time_type{} + std::chrono::minutes(minutes),
                          .tags = std::move(tags)};
}

core::NoteEdge link(std::string_view from, std::string_view to) {
    return core::NoteEdge{.from = np(from), .to = np(to)};
}

core::NoteEdge related(std::string_view from, std::string_view to, std::string name, bool directed = false) {
    return core::NoteEdge{.from = np(from),
                          .to = np(to),
                          .kind = EdgeKind::RelatedTo,
                          .name = std::move(name),
                          .directed = directed};
}

std::vector<std::size_t> all(std::span<const core::NoteMeta> notes) {
    std::vector<std::size_t> indices(notes.size());
    std::iota(indices.begin(), indices.end(), 0);
    return indices;
}

std::vector<RelationHit> hits(std::string_view text, const std::vector<core::NoteMeta>& notes,
                              const std::vector<core::NoteEdge>& edges,
                              std::optional<std::size_t> depth = 1) {
    return relation_hits(text, notes, all(notes), GraphInput{edges, depth});
}

std::vector<std::string> paths(const std::vector<RelationHit>& found,
                               const std::vector<core::NoteMeta>& notes) {
    std::vector<std::string> out;
    for (const RelationHit& hit : found)
        out.push_back(notes[hit.index].path.string());
    return out;
}

std::vector<std::string> passing(std::string_view query, const std::vector<core::NoteMeta>& notes,
                                 const std::vector<core::NoteEdge>& edges) {
    auto parsed = parse_query(query);
    REQUIRE(parsed.has_value());
    bind_graph_filters(parsed->filters, GraphInput{edges});
    std::vector<std::string> out;
    for (const std::size_t i : apply(parsed->filters, std::nullopt, notes))
        out.push_back(notes[i].path.string());
    return out;
}

}  // namespace

TEST_CASE("a term found in a tag matches the note with a tag reason") {
    const std::vector notes{meta("a.md", {"proje/alt"}), meta("b.md", {"ev"})};
    const auto found = hits("Proj", notes, {});
    REQUIRE(found.size() == 1);
    CHECK(found[0].index == 0);
    CHECK(found[0].reason.kind == Kind::Tag);
    CHECK(found[0].reason.label == "proje/alt");
}

TEST_CASE("every term must sit in the same tag; Turkish letters fold") {
    const std::vector notes{meta("a.md", {"bütçe-planı"}), meta("b.md", {"bütçe", "plan"})};
    CHECK(paths(hits("butce plani", notes, {}), notes) == std::vector<std::string>{"a.md"});
}

TEST_CASE("a term found in a relation name matches both ends, with the direction seen from each") {
    const std::vector notes{meta("a.md"), meta("b.md"), meta("c.md")};
    const auto found = hits("use", notes, {related("a.md", "b.md", "Uses", true), link("a.md", "c.md")});
    REQUIRE(paths(found, notes) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(found[0].reason.kind == Kind::Relation);
    CHECK(found[0].reason.label == "Uses");
    CHECK(found[0].reason.other == "b.md");
    CHECK(found[0].reason.direction == RelationDirection::Outgoing);
    CHECK(found[1].reason.other == "a.md");
    CHECK(found[1].reason.direction == RelationDirection::Incoming);
}

TEST_CASE(
    "a neighbour whose path matches finds the note one hop away; depth widens it, 0 hops is a name hit") {
    // a -> budget <- b ; c - a (iki adım)
    const std::vector notes{meta("a.md"), meta("b.md"), meta("c.md"), meta("budget.md")};
    const std::vector edges{link("a.md", "budget.md"), related("b.md", "budget.md", "cites"),
                            related("c.md", "a.md", "")};

    const auto one = hits("budget", notes, edges);
    REQUIRE(paths(one, notes) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(one[0].reason.kind == Kind::Linked);
    CHECK(one[0].reason.other == "budget.md");
    CHECK(one[0].reason.hops == 1);
    CHECK(one[0].reason.label.empty());  // [[bağlantı]]
    CHECK(one[1].reason.label == "cites");

    const auto two = hits("budget", notes, edges, 2);
    REQUIRE(paths(two, notes) == std::vector<std::string>{"a.md", "b.md", "c.md"});
    CHECK(two[2].reason.hops == 2);
    CHECK(two[2].reason.other == "budget.md");
    CHECK(two[2].reason.label.empty());  // uzak komşuda kenar adı gösterilmez

    CHECK(hits("budget", notes, edges, SIZE_MAX).size() == 3);
    CHECK(hits("budget", notes, edges, std::nullopt).empty());  // kapalı
}

TEST_CASE("the strongest reason wins and results rank by kind, hops, then newest") {
    const std::vector notes{meta("old.md", {}, 1), meta("new.md", {}, 9), meta("tagged.md", {"plan"}),
                            meta("named.md"), meta("plan.md")};
    const std::vector edges{link("old.md", "plan.md"), link("new.md", "plan.md"),
                            link("tagged.md", "plan.md"), related("named.md", "plan.md", "planlar")};
    const auto found = hits("plan", notes, edges);
    // plan.md'nin adı eşleşir (o ad eşleşmesidir, Linked olmaz) ama "planlar" ilişkisinin ucudur.
    REQUIRE(paths(found, notes) ==
            std::vector<std::string>{"tagged.md", "named.md", "plan.md", "new.md", "old.md"});
    CHECK(found[0].reason.kind == Kind::Tag);
    CHECK(found[1].reason.kind == Kind::Relation);
    CHECK(found[2].reason.kind == Kind::Relation);
    CHECK(found[3].reason.kind == Kind::Linked);
}

TEST_CASE("only allowed notes are returned, but any note can be the matching neighbour") {
    const std::vector notes{meta("a.md"), meta("b.md"), meta("budget.md")};
    const std::vector edges{link("a.md", "budget.md"), link("b.md", "budget.md")};
    const std::vector<std::size_t> allowed{1};
    const auto found = relation_hits("budget", notes, allowed, GraphInput{edges, 1});
    REQUIRE(found.size() == 1);
    CHECK(found[0].index == 1);
}

TEST_CASE("no terms, no hits") {
    const std::vector notes{meta("a.md", {"x"})};
    CHECK(hits("  ", notes, {}).empty());
}

TEST_CASE("rel: keeps the notes on either end of a relation with that name") {
    const std::vector notes{meta("a.md"), meta("b.md"), meta("c.md"), meta("d.md")};
    const std::vector edges{related("a.md", "b.md", "Uses", true), related("c.md", "d.md", "used by"),
                            link("a.md", "c.md")};
    CHECK(passing("rel:uses", notes, edges) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(passing("rel:\"used by\"", notes, edges) == std::vector<std::string>{"c.md", "d.md"});
    CHECK(passing("rel:none", notes, edges).empty());
}

TEST_CASE("linked: keeps the neighbours of a note named by path, path without extension or file name") {
    const std::vector notes{meta("a.md"), meta("work/b.md"), meta("c.md"), meta("d.md")};
    const std::vector edges{link("a.md", "work/b.md"), related("work/b.md", "c.md", "x"),
                            link("c.md", "d.md")};
    const std::vector<std::string> neighbours{"a.md", "c.md"};
    CHECK(passing("linked:work/b.md", notes, edges) == neighbours);
    CHECK(passing("linked:work/b", notes, edges) == neighbours);
    CHECK(passing("linked:B.md", notes, edges) == neighbours);
    CHECK(passing("linked:b", notes, edges) == neighbours);
    CHECK(passing("linked:zzz", notes, edges).empty());
}

TEST_CASE("unbound graph filters pass nothing; the keys are known to the parser") {
    const std::vector notes{meta("a.md")};
    const auto parsed = parse_query("rel:uses linked:a plan");
    REQUIRE(parsed.has_value());
    CHECK(parsed->text == "plan");
    CHECK(has_graph_filter(parsed->filters));
    CHECK(apply(parsed->filters, std::nullopt, notes).empty());
    CHECK_FALSE(has_graph_filter(parse_query("tag:x plan")->filters));
    const auto keys = query_key_names();
    CHECK(std::ranges::find(keys, "rel") != keys.end());
    CHECK(std::ranges::find(keys, "linked") != keys.end());
}

TEST_CASE("a prebuilt relation index gives the same hits as the edge list") {
    const std::vector notes{meta("a.md"), meta("b.md"), meta("budget.md")};
    const std::vector edges{link("a.md", "budget.md"), related("b.md", "a.md", "cites")};
    const RelationGraph index(edges);
    const auto plain = relation_hits("budget", notes, all(notes), GraphInput{edges, 2});
    const auto indexed = relation_hits("budget", notes, all(notes), GraphInput{edges, 2, &index});
    REQUIRE(paths(indexed, notes) == paths(plain, notes));
    for (std::size_t i = 0; i < plain.size(); ++i)
        CHECK(indexed[i].reason == plain[i].reason);
}

TEST_CASE("prop: binds from the property source without edges; values match folded and whole") {
    const std::vector notes{meta("a.md"), meta("b.md"), meta("c.md")};
    const PropertyLookup lookup = [](std::string_view key) -> std::shared_ptr<const core::PropertyNotes> {
        if (key != "status")
            return std::make_shared<const core::PropertyNotes>();
        return std::make_shared<const core::PropertyNotes>(
            core::PropertyNotes{{np("a.md"), "İptal"}, {np("b.md"), "open"}});
    };
    const auto passing_props = [&](std::string_view query, const PropertyLookup& source) {
        auto parsed = parse_query(query);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(has_graph_filter(parsed->filters));  // `prop:` kenar istemez
        GraphInput graph;
        graph.properties = source;
        bind_graph_filters(parsed->filters, graph);
        std::vector<std::string> out;
        for (const std::size_t i : apply(parsed->filters, std::nullopt, notes))
            out.push_back(notes[i].path.string());
        return out;
    };
    CHECK(passing_props("prop:status", lookup) == std::vector<std::string>{"a.md", "b.md"});
    CHECK(passing_props("prop:status=iptal", lookup) == std::vector<std::string>{"a.md"});
    CHECK(passing_props("prop:status=OPEN", lookup) == std::vector<std::string>{"b.md"});
    CHECK(passing_props("prop:status=op", lookup).empty());
    CHECK(passing_props("prop:other", lookup).empty());
    CHECK(passing_props("prop:status", {}).empty());  // kaynak yok: `view:` kuralı
    CHECK(passing_props("prop:status", [](std::string_view) { return nullptr; }).empty());
    const auto bad = parse_query("prop:=open");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().error.args.back().second == "must_be_property");
    const auto keys = query_key_names();
    CHECK(std::ranges::find(keys, "prop") != keys.end());
}
