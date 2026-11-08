// F3: harita sorgu çubuğunun ortak akışı (app::MapQuery) ve haritadaki vurgu (MapModel::set_highlight). GUI ve
// TUI yalnız bunları çağırır: okuma hemen, yazma önizleme + onay + uygulama, hata konumlu, tablo ve vurgu.
#include "app/map_query.hpp"
#include "app/note_map.hpp"
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_views.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <expected>
#include <future>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace app = noteapp::app;
namespace core = noteapp::core;
using app::MapQuery;
using app::QueryCell;
using noteapp::test::seed_note;
using noteapp::test::TempDir;

namespace {

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

using Kind = QueryCell::Kind;

struct Store {
    TempDir dir;
    core::NoteStore store{dir.path()};
    core::RelationId uses;
    Store() {
        REQUIRE(seed_note(store, "a.md", "see [[b]] #proj\n"));
        REQUIRE(seed_note(store, "b.md", "plain\n"));
        REQUIRE(seed_note(store, "c.md", "other\n"));
        const auto relation = store.add_relation(np("a.md"), np("c.md"), {"uses", false});
        REQUIRE(relation.has_value());
        uses = *relation;
    }
    std::size_t count(std::string_view cypher) {
        const auto done = core::run_user_query(store, cypher, core::QueryAction::Read, {});
        REQUIRE(done.has_value());
        return done->result.rows.size();
    }
};

const app::QueryOutcome& ran(MapQuery& query, core::NoteStore& store, std::string_view text) {
    INFO(std::string(text));
    const MapQuery::Step step = query.run(store, text);
    INFO(query.error());
    REQUIRE(step == MapQuery::Step::Ran);
    REQUIRE(query.outcome().has_value());
    return *query.outcome();
}

}  // namespace

// İnceleme: sorgu tablosu zaman damgasını denetçiyle aynı yardımcıyla yazar (core::timestamp_text): uç değer
// taşmaz, ham milisaniye olur.
TEST_CASE("map query: timestamp cells use the shared text, extreme values stay raw") {
    core::UserQuery query;
    query.result.columns = {"t"};
    for (const std::int64_t ms : {std::int64_t{0}, std::numeric_limits<std::int64_t>::min()})
        query.result.rows.push_back({noteapp::graph::QueryValue(noteapp::graph::Timestamp{ms})});
    const app::QueryOutcome outcome = app::query_outcome(query, false);
    REQUIRE(outcome.table.rows.size() == 2);
    CHECK(outcome.table.rows[0][0].text == "1970-01-01 00:00:00.000");
    CHECK(outcome.table.rows[1][0].text ==
          "<time:" + std::to_string(std::numeric_limits<std::int64_t>::min()) + ">");
}

TEST_CASE("map query: a read fills a typed table with note paths and highlights what it returned") {
    Store s;
    MapQuery query;
    const app::QueryOutcome& links =
        ran(query, s.store, "MATCH (a:Note)-[r:LINKS_TO]->(b) RETURN a, r, b, a.name AS name, null AS gone");
    CHECK(links.table.columns == std::vector<std::string>{"a", "r", "b", "name", "gone"});
    REQUIRE(links.table.rows.size() == 1);
    const auto& row = links.table.rows.front();
    CHECK(row[0] == QueryCell{Kind::Note, "a.md", "a.md"});
    CHECK(row[1] == QueryCell{Kind::Edge, "a.md -[LINKS_TO]-> b.md"});
    CHECK(row[2] == QueryCell{Kind::Note, "b.md", "b.md"});
    CHECK(row[3] == QueryCell{Kind::Value, "a"});  // Note.name uzantısızdır (tip ayrı özellik)
    CHECK(row[4] == QueryCell{Kind::Null, "null"});
    CHECK(links.highlight.notes == std::set<std::string>{"a.md", "b.md"});
    CHECK(links.highlight.links == std::set<std::pair<std::string, std::string>>{{"a.md", "b.md"}});
    CHECK_FALSE(links.wrote);
    CHECK(links.summary == core::tr("map.query_read", {{"rows", "1"}, {"notes", "2"}}));

    const app::QueryOutcome& related = ran(query, s.store, "MATCH ()-[r:RELATED_TO]->() RETURN r");
    REQUIRE(related.table.rows.size() == 1);
    CHECK(related.table.rows.front().front() == QueryCell{Kind::Edge, "a.md -[uses]- c.md"});
    CHECK(related.highlight.relations == std::set<core::RelationId>{s.uses});
    CHECK(related.highlight.notes == std::set<std::string>{"a.md", "c.md"});  // döndürülen kenarın uçları

    const app::QueryOutcome& tagged = ran(query, s.store, "MATCH (n:Note)-[t:TAGGED]->(g) RETURN g, t");
    REQUIRE(tagged.table.rows.size() == 1);
    CHECK(tagged.table.rows.front()[0] == QueryCell{Kind::Tag, "#proj"});
    CHECK(tagged.table.rows.front()[1] == QueryCell{Kind::Edge, "a.md -[TAGGED]-> #proj"});
    CHECK(tagged.highlight.tags == std::set<std::string>{"proj"});
    CHECK(tagged.highlight.tagged == std::set<std::pair<std::string, std::string>>{{"a.md", "proj"}});

    const app::QueryOutcome& listed =
        ran(query, s.store, "MATCH (n:Note) WITH n ORDER BY n.name RETURN collect(n) AS notes, count(n) AS total");
    REQUIRE(listed.table.rows.size() == 1);
    CHECK(listed.table.rows.front()[0] == QueryCell{Kind::List, "[a.md, b.md, c.md]"});
    CHECK(listed.table.rows.front()[1] == QueryCell{Kind::Value, "3"});
    CHECK(listed.highlight.notes.size() == 3);

    // Kullanıcı düğümü etiketi ve kısa kimliğiyle.
    REQUIRE(core::run_user_query(s.store, "CREATE (:Person {name: 'Ali'})", core::QueryAction::Write, {}));
    const app::QueryOutcome& person = ran(query, s.store, "MATCH (p:Person) RETURN p");
    REQUIRE(person.table.rows.size() == 1);
    CHECK(person.table.rows.front().front().kind == Kind::Node);
    CHECK(person.table.rows.front().front().text.starts_with("(:Person "));
    CHECK(person.highlight == app::MapHighlight{});
}

TEST_CASE("map query: a write is previewed, asked, then applied; cancel and errors write nothing") {
    Store s;
    MapQuery query;
    const std::string_view write = "MATCH (n:Note) SET n.status = 'x'";
    CHECK(query.run(s.store, write) == MapQuery::Step::Confirm);
    CHECK(query.confirming());
    core::QueryChanges expected;
    expected.properties_set = 3;
    CHECK(query.confirm_text() == core::tr("map.query_confirm", {{"changes", app::query_changes_text(expected)}}));
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 0);

    query.cancel();
    CHECK_FALSE(query.confirming());
    CHECK(query.apply(s.store) == MapQuery::Step::Failed);  // onay yokken uygulanacak bir şey yok
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 0);

    REQUIRE(query.run(s.store, write) == MapQuery::Step::Confirm);
    CHECK(query.apply(s.store) == MapQuery::Step::Ran);
    CHECK_FALSE(query.confirming());
    REQUIRE(query.outcome().has_value());
    CHECK(query.outcome()->wrote);
    CHECK(query.outcome()->changes == expected);
    CHECK(query.outcome()->summary == core::tr("map.query_written", {{"changes", app::query_changes_text(expected)}}));
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 3);

    // Korunan veri önizlemede reddedilir: onay sorulmaz, önceki sonuç kalır, hata konumludur.
    CHECK(query.run(s.store, "MATCH (n:Note) SET n.body = 'gone'") == MapQuery::Step::Failed);
    CHECK_FALSE(query.confirming());
    CHECK(query.error().find("body") != std::string::npos);
    CHECK(query.outcome().has_value());
    CHECK(s.store.read(np("b.md"))->text == "plain\n");

    CHECK(query.run(s.store, "MATCH (n:Note RETURN n") == MapQuery::Step::Failed);
    CHECK_FALSE(query.error().empty());
    CHECK(query.outcome().has_value());
}

TEST_CASE("map query: saved views run read-only; the table shows only with a result; clear drops it") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    MapQuery query;
    CHECK_FALSE(query.table_shown());
    query.toggle_table();
    CHECK_FALSE(query.table_shown());  // sonuç yokken tablo yok
    CHECK(query.run_view(s.store, "Linked") == MapQuery::Step::Ran);
    CHECK(query.table_shown());
    CHECK(query.outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md"});
    CHECK(query.run_view(s.store, "missing") == MapQuery::Step::Failed);
    CHECK(query.outcome().has_value());
    // Çubuğa yazılan görünüm adı (TUI Tab tamamlaması) görünümü çalıştırır; tek sözcük Cypher değildir.
    REQUIRE(query.run(s.store, "MATCH (n:Note) WHERE n.name = 'c' RETURN n") == MapQuery::Step::Ran);
    CHECK(query.run(s.store, "  LINKED ") == MapQuery::Step::Ran);
    CHECK(query.outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md"});
    // Güvenlik incelemesi (Sev-3): görünüm yazılan metni gölgeleyebilir; özet görünümün çalıştığını ve kayıtlı
    // adını söyler (iki arayüz bu özeti gösterir).
    const std::string view_summary =
        core::tr("map.query_view", {{"name", "linked"}, {"rows", "1"}, {"notes", "2"}});
    CHECK(query.outcome()->summary == view_summary);
    REQUIRE(query.run_view(s.store, "linked") == MapQuery::Step::Ran);
    CHECK(query.outcome()->summary == view_summary);
    CHECK(query.run(s.store, "nothing") == MapQuery::Step::Failed);
    query.clear();
    CHECK_FALSE(query.outcome().has_value());
    CHECK(query.error().empty());
    CHECK_FALSE(query.table_shown());
}

// İnceleme (F3, Sev-2): senkronla gelen görünümün ADI yazan bir Cypher olabilir; TUI Tab onu satıra yazar. Ad önce
// görünüm olarak çözülür: salt-okunur ve görünüm profiliyle çalışır, kullanıcının yazması (ya da limits.query'li
// okuması) olmaz.
TEST_CASE("map query: a view named like a Cypher write runs as that read-only view under the view profile") {
    Store s;
    const std::string name = "MATCH (n) DETACH DELETE n";
    {
        auto raw = noteapp::graph::Graph::open(s.dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("CREATE (:View {name: $name, query: 'MATCH (n:Note) RETURN n'})",
                           {{"name", noteapp::graph::QueryValue(name)}}, noteapp::graph::QueryMode::ReadWrite));
    }
    noteapp::graph::QueryLimits tight;
    tight.max_rows = 1;  // kullanıcının profili; görünüm bunu kullanmaz
    MapQuery query(tight);
    CHECK(query.run(s.store, name) == MapQuery::Step::Ran);
    CHECK_FALSE(query.confirming());
    REQUIRE(query.outcome().has_value());
    CHECK_FALSE(query.outcome()->wrote);
    CHECK(query.outcome()->table.rows.size() == 3);
    CHECK(s.count("MATCH (n:Note) RETURN n") == 3);
}

// Güvenlik incelemesi: görünüm araması NotFound dışında bir nedenle düşerse (Busy, G/Ç) metin "görünüm değil"
// sayılıp Cypher'a düşmez: görünümün adı olabilirdi, yazma olarak çalışırdı. Hata gösterilir, hiçbir şey çalışmaz.
TEST_CASE("map query: a view lookup that fails for a reason other than not-found is shown, not run as Cypher") {
    Store s;
    const core::Error busy{core::Error::Code::Io, "error.io", {{"path", "graph.db"}, {"detail", "busy"}}};
    s.store.impl().view_lookup_fault = [busy] { return std::optional(busy); };
    MapQuery query;
    CHECK(query.run(s.store, "MATCH (n:Note) SET n.status = 'x'") == MapQuery::Step::Failed);
    CHECK_FALSE(query.confirming());
    CHECK(query.run(s.store, "MATCH (n:Note) RETURN n") == MapQuery::Step::Failed);
    CHECK(query.error() == core::format_error(busy));
    CHECK_FALSE(query.outcome().has_value());
    s.store.impl().view_lookup_fault = nullptr;
    CHECK(query.run(s.store, "MATCH (n:Note) RETURN n") == MapQuery::Step::Ran);
}

// İnceleme (Low): uygulanan yazmanın sonucu adlanamazsa da yazılmıştır: Ran, wrote (arayüz tazeler), özet
// adlandırma hatasını söyler.
TEST_CASE("map query: an applied write whose result cannot be named still counts as written") {
    Store s;
    MapQuery query;
    REQUIRE(query.run(s.store, "MATCH (n:Note) SET n.status = 'x'") == MapQuery::Step::Confirm);
    s.store.impl().query_naming_fault = [] {
        return std::optional(core::Error{core::Error::Code::Io, "error.io", {{"path", "graph.db"}, {"detail", "fault"}}});
    };
    CHECK(query.apply(s.store) == MapQuery::Step::Ran);
    REQUIRE(query.outcome().has_value());
    CHECK(query.outcome()->wrote);
    core::QueryChanges changes;
    changes.properties_set = 3;
    CHECK(query.outcome()->summary ==
          core::tr("map.query_written_unnamed",
                   {{"changes", app::query_changes_text(changes)},
                    {"error", core::format_error(core::Error{core::Error::Code::Io, "error.io",
                                                             {{"path", "graph.db"}, {"detail", "fault"}}})}}));
    s.store.impl().query_naming_fault = nullptr;
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 3);
}

// İnceleme (Sev-3): hücre metni bir kez, sonuç kurulurken temizlenir ve limits.app.query_cell_bytes'ta kesilir;
// arayüzler kare başına yeniden temizlemez, dev değer (gövde) tabloyu şişirmez.
TEST_CASE("map query: cells are sanitised once and cut at limits.app.query_cell_bytes") {
    Store s;
    CHECK(core::parse_config("").config.limits.app.query_cell_bytes.cap() == app::kDefaultQueryCellBytes);
    const std::string set = "MATCH (n:Note {name: 'a'}) SET n.title = 'ab\\u202Ecd\\nef', n.long = '" +
                            std::string(10000, 'x') + "', n.wide = '" + std::string(3000, 'x') + "ğğ'";
    REQUIRE(core::run_user_query(s.store, set, core::QueryAction::Write, {}));
    const std::string read = "MATCH (n:Note {name: 'a'}) RETURN n.title AS title, n.long AS long, n.wide AS wide";

    const std::string ellipsis = "…";
    MapQuery query({}, 3001);
    const auto& row = ran(query, s.store, read).table.rows.at(0);
    CHECK(row[0].text == "ab\xEF\xBF\xBD" "cd\xEF\xBF\xBD" "ef");
    CHECK(row[1].text.size() == 3001 + ellipsis.size());
    CHECK(row[1].text.ends_with(ellipsis));
    CHECK(row[2].text.size() == 3000 + ellipsis.size());  // kod noktası ortasında kesilmez
    CHECK(row[2].text.ends_with("x" + ellipsis));

    MapQuery unlimited({}, SIZE_MAX);
    CHECK(ran(unlimited, s.store, read).table.rows.at(0)[1].text.size() == 10000);
}

TEST_CASE("map query: the changes text lists only non-zero counters without plurals") {
    CHECK(app::query_changes_text({}) == core::tr("map.changes_none"));
    core::QueryChanges changes;
    changes.nodes_created = 2;
    changes.properties_set = 3;
    CHECK(app::query_changes_text(changes) == core::tr("map.changes_nodes_created", {{"count", "2"}}) + ", " +
                                                  core::tr("map.changes_properties_set", {{"count", "3"}}));
}

namespace {

core::NoteMeta meta(const std::string& path) {
    return core::NoteMeta{.path = np(path)};
}

core::NoteEdge link(const std::string& from, const std::string& to) {
    return {np(from), np(to)};
}

}  // namespace

TEST_CASE("map query: the highlight lights its notes and edges, brings result notes into scope, and clears") {
    const std::vector<core::NoteMeta> notes{meta("a.md"), meta("b.md"), meta("c.md"), meta("h.md")};
    core::NoteEdge related{np("a.md"), np("c.md"), core::EdgeKind::RelatedTo, "uses"};
    related.relation.bytes[0] = std::byte{7};
    const std::vector<core::NoteEdge> edges{link("a.md", "b.md"), related};
    app::MapModel model;
    model.set_options(app::MapOptions{.depth = 1});
    const auto update = [&] {
        model.update([&]() -> std::expected<std::vector<core::NoteEdge>, core::Error> { return edges; }, notes,
                     np("a.md"));
    };
    const auto keys = [&] {
        std::set<std::string> out;
        for (const app::MapNode& node : model.map().nodes)
            out.insert(node.key);
        return out;
    };
    const auto index_of = [&](const std::string& key) {
        const auto& nodes = model.map().nodes;
        return static_cast<std::size_t>(std::ranges::find(nodes, key, &app::MapNode::key) - nodes.begin());
    };
    update();
    CHECK(keys() == std::set<std::string>{"a.md", "b.md", "c.md"});
    CHECK(model.highlighted_nodes().empty());

    app::MapHighlight highlight;
    highlight.notes = {"a.md", "b.md", "h.md"};
    highlight.links = {{"a.md", "b.md"}};
    model.set_highlight(highlight);
    update();
    // h.md derinliğin dışında ve bağsız: sonuçta olduğu için kapsama girer.
    CHECK(keys() == std::set<std::string>{"a.md", "b.md", "c.md", "h.md"});
    const auto& lit = model.highlighted_nodes();
    REQUIRE(lit.size() == model.map().nodes.size());
    CHECK(lit[index_of("a.md")] == 1);
    CHECK(lit[index_of("b.md")] == 1);
    CHECK(lit[index_of("c.md")] == 0);
    CHECK(lit[index_of("h.md")] == 1);
    const auto& lit_edges = model.highlighted_edges();
    REQUIRE(lit_edges.size() == model.map().edges.size());
    for (std::size_t e = 0; e < model.map().edges.size(); ++e) {
        const app::MapEdge& edge = model.map().edges[e];
        CHECK(lit_edges[e] == (edge.kind == app::MapEdgeKind::Link ? 2 : 0));
    }

    // Kenar kendisi sonuçta olmasa da iki ucu sonuçtaysa yanar (1); ilişki kimliğiyle eşleşir (2).
    highlight = {};
    highlight.notes = {"a.md", "b.md", "c.md"};
    highlight.relations = {related.relation};
    model.set_highlight(highlight);
    update();
    for (std::size_t e = 0; e < model.map().edges.size(); ++e) {
        const app::MapEdge& edge = model.map().edges[e];
        CHECK(model.highlighted_edges()[e] == (edge.kind == app::MapEdgeKind::Related ? 2 : 1));
    }

    model.set_highlight(std::nullopt);
    update();
    CHECK(keys() == std::set<std::string>{"a.md", "b.md", "c.md"});
    CHECK(model.highlighted_nodes().empty());
    CHECK(model.highlighted_edges().empty());
}

// SD11b: kayıtlı görünüm TUI ve GUI'de de kaydedilir; kural CLI `note view save` ile aynı (core::save_view).
TEST_CASE("map query: only a shown read can be saved as a view, under the core name rules") {
    Store s;
    MapQuery query;
    const auto none = query.save_view(s.store, "mine");
    REQUIRE_FALSE(none);
    CHECK(none.error().message_key == "error.view_needs_query");

    const std::string text = "MATCH (n:Note) WHERE n.name = 'b' RETURN n";
    ran(query, s.store, text);
    CHECK(query.read_query() == text);
    CHECK_FALSE(query.save_view(s.store, " mine"));  // baş boşluk reddedilir, kırpılmaz
    CHECK_FALSE(query.save_view(s.store, "a/b"));
    REQUIRE(query.save_view(s.store, "mine"));
    CHECK(core::find_view(s.store, "mine")->query == text);
    const auto again = query.save_view(s.store, "MINE");  // ezilmez
    REQUIRE_FALSE(again);
    CHECK(again.error().code == core::Error::Code::AlreadyExists);

    // Görünümün ya da yazmanın sonucu kaydedilecek sorgu taşımaz; temizlenen sonuç da.
    REQUIRE(query.run_view(s.store, "mine") == MapQuery::Step::Ran);
    CHECK(query.read_query().empty());
    ran(query, s.store, text);
    query.clear();
    CHECK(query.read_query().empty());
}

// Kayıtlı görünümü açılışta geri getirme (UiPrefs::map.view): etkin görünüm çalışan, gösterilen okumanın
// kaydedildiği addır; yazılan sorgu ve clear() onu düşürür. Açılış yalnız görünümü çalıştırır.
TEST_CASE("map query: the active view follows run, save and clear and is what the state file keeps") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    MapQuery query;
    CHECK(query.view().empty());
    REQUIRE(query.run(s.store, "LINKED") == MapQuery::Step::Ran);
    CHECK(query.view() == "linked");  // kayıtlı yazım
    CHECK(query.run_view(s.store, "missing") == MapQuery::Step::Failed);
    CHECK(query.view() == "linked");  // önceki sonuç kalır
    ran(query, s.store, "MATCH (n:Note) WHERE n.name = 'c' RETURN n");
    CHECK(query.view().empty());
    REQUIRE(query.save_view(s.store, "mine"));
    CHECK(query.view() == "mine");
    CHECK_FALSE(query.save_view(s.store, "mine"));  // aynı ad ezilmez: etkin görünüm aynı kalır
    CHECK(query.view() == "mine");
    query.clear();
    CHECK(query.view().empty());
}

// Açılış (UiPrefs::map.view): görünüm işçide yalnız kayıtlı görünüm olarak çalışır (core::run_saved_view);
// sonucu restored() uygular. Kalıcı hata (yok, yazan) adı düşürür; geçici hata adı korur (inceleme L2).
TEST_CASE("map query: a restored view shows its result; a missing or writing view drops its name, a passing "
          "error keeps it") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    {
        auto raw = noteapp::graph::Graph::open(s.dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("CREATE (:View {name: 'writer', query: \"MATCH (n:Note) SET n.status = 'x'\"})",
                           {}, noteapp::graph::QueryMode::ReadWrite));
    }
    using Kind = MapQuery::Restored::Kind;
    MapQuery query;
    CHECK(query.restored(core::run_saved_view(s.store, "linked")).kind == Kind::Stale);  // beklenmeyen sonuç
    CHECK_FALSE(query.outcome().has_value());

    query.begin_restore("linked");
    CHECK(query.view() == "linked");  // sonuç gelene dek de kaydedilen ad
    CHECK(query.restored(core::run_saved_view(s.store, "linked")).kind == Kind::Ran);
    REQUIRE(query.outcome().has_value());
    CHECK(query.outcome()->highlight.notes == std::set<std::string>{"a.md", "b.md"});
    CHECK(query.view() == "linked");

    // Arada yazılan sorgu kazanır: geç gelen sonuç onu ezmez.
    query.begin_restore("linked");
    ran(query, s.store, "MATCH (n:Note) WHERE n.name = 'c' RETURN n");
    CHECK(query.restored(core::run_saved_view(s.store, "linked")).kind == Kind::Stale);
    CHECK(query.outcome()->highlight.notes == std::set<std::string>{"c.md"});
    CHECK(query.view().empty());

    for (const std::string name : {"writer", "MATCH (n:Note) SET n.status = 'x'"}) {
        INFO(name);
        MapQuery fresh;
        fresh.begin_restore(name);
        const MapQuery::Restored failed = fresh.restored(core::run_saved_view(s.store, name));
        CHECK(failed.kind == Kind::Failed);
        CHECK(failed.notice.find(name) != std::string::npos);
        CHECK_FALSE(fresh.outcome().has_value());
        CHECK(fresh.error().empty());  // ileti bir kez, arayüzün bildirimiyle
        CHECK(fresh.view().empty());   // kalıcı: ad düşer
        CHECK_FALSE(fresh.confirming());
    }
    CHECK(s.count("MATCH (n:Note) WHERE n.status = 'x' RETURN n") == 0);

    MapQuery busy;
    busy.begin_restore("linked");
    const core::Error io{core::Error::Code::Io, "error.io", {{"path", "graph.db"}, {"detail", "busy"}}};
    CHECK(busy.restored(std::unexpected(io)).kind == Kind::Failed);
    CHECK(busy.view() == "linked");  // geçici: sonraki açılış yeniden dener
    CHECK_FALSE(busy.outcome().has_value());
}

TEST_CASE("map query: the restore worker runs the saved view off the calling thread and delivers it") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    std::promise<app::ViewRestore::Result> delivered;
    auto result = delivered.get_future();
    {
        app::ViewRestore worker;
        worker.start(s.store, "linked",
                     [&delivered](app::ViewRestore::Result done) { delivered.set_value(std::move(done)); });
    }  // yıkıcı işi bekler
    REQUIRE(result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const app::ViewRestore::Result done = result.get();
    REQUIRE(done.has_value());
    CHECK(done->result.rows.size() == 1);
}

// İnceleme (eşzamanlılık, düşük): işçideki istisna süreci düşürmez, hata olarak teslim edilir (geçici: ad
// kalır).
TEST_CASE("map query: an exception in the restore worker is delivered as an error") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    s.store.impl().view_lookup_fault = []() -> std::optional<core::Error> {
        throw std::runtime_error("boom");
    };
    std::promise<app::ViewRestore::Result> delivered;
    auto result = delivered.get_future();
    {
        app::ViewRestore worker;
        worker.start(s.store, "linked",
                     [&delivered](app::ViewRestore::Result done) { delivered.set_value(std::move(done)); });
    }
    const app::ViewRestore::Result done = result.get();
    REQUIRE_FALSE(done.has_value());
    CHECK(done.error().code == core::Error::Code::Io);
    MapQuery query;
    query.begin_restore("linked");
    CHECK(query.restored(done).kind == MapQuery::Restored::Kind::Failed);
    CHECK(query.view() == "linked");
}

// notes_dir değişince arayüz clear() çağırır: eski depoda yolda olan sonuç yeni depoya uygulanmaz.
TEST_CASE("map query: a restore result arriving after clear is stale") {
    Store s;
    REQUIRE(core::save_view(s.store, "linked", "MATCH (a:Note)-[:LINKS_TO]->(b) RETURN a, b", false));
    MapQuery query;
    query.begin_restore("linked");
    query.clear();
    CHECK(query.restored(core::run_saved_view(s.store, "linked")).kind == MapQuery::Restored::Kind::Stale);
    CHECK_FALSE(query.outcome().has_value());
    CHECK(query.view().empty());
}
