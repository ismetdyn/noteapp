// F3: kullanıcının Cypher sorgusu NoteStore üzerinden (harita sorgu çubuğu). Okuma; yazmanın sınıflandırılması;
// türetilmiş verinin (gövde, yol, bağlantı, etiket, klasör, görünüm) ham Cypher'a kapalı olması; kullanıcı
// verisinin (özellik, adlı ilişki, kendi düğümleri) yazılması; önizleme; sınırlar.
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_sync.hpp"
#include "core/note_views.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace noteapp::core;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;
namespace graph = noteapp::graph;

namespace {

using Texts = std::vector<std::string>;

NotePath np(const std::string& text) {
    return *NotePath::parse(text);
}

std::string arg_of(const Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args)
        if (key == name)
            return value;
    return {};
}

std::string describe(const Error& error) {
    std::string out = error.message_key;
    for (const auto& [key, value] : error.args)
        out += " " + key + "=" + value;
    return out;
}

UserQuery must_run(NoteStore& store, std::string_view cypher, QueryAction action = QueryAction::Read,
                   const graph::QueryLimits& limits = {}) {
    auto done = run_user_query(store, cypher, action, limits);
    INFO(std::string(cypher));
    REQUIRE_MESSAGE(done.has_value(), (done ? std::string() : describe(done.error())));
    return std::move(*done);
}

// Sonucun ilk sütunundaki öğelerin adları (düğümde key, kenarda "from>to").
Texts element_keys(const UserQuery& query) {
    Texts out;
    for (const auto& row : query.result.rows) {
        const graph::QueryValue& cell = row.front();
        if (const auto* node = std::get_if<graph::NodeId>(&cell.v)) {
            const auto found = query.nodes.find(*node);
            out.push_back(found == query.nodes.end() ? "?" : found->second.key);
        } else if (const auto* edge = std::get_if<graph::EdgeId>(&cell.v)) {
            const auto found = query.edges.find(*edge);
            out.push_back(found == query.edges.end() ? "?" : found->second.from + ">" + found->second.to);
        }
    }
    std::ranges::sort(out);
    return out;
}

Texts note_paths(const NoteStore& store) {
    Texts out;
    for (const NoteInfo& info : store.list())
        out.push_back(info.path.string());
    std::ranges::sort(out);
    return out;
}

// Notların türetilmiş durumu: yollar, gövdeler, bağlantılar ve etiketler.
Texts derived_state(const NoteStore& store) {
    Texts out = note_paths(store);
    for (const std::string& path : note_paths(store)) {
        const auto body = store.read(np(path));
        REQUIRE(body.has_value());
        out.push_back(path + ":" + body->text);
        const auto links = store.links(np(path));
        REQUIRE(links.has_value());
        for (const NotePath& target : links->outgoing)
            out.push_back(path + "->" + target.string());
        for (const std::string& tag : links->tags)
            out.push_back(path + "#" + tag);
    }
    for (const TagCount& tag : store.tags())
        out.push_back("tag:" + tag.name + "=" + std::to_string(tag.count));
    return out;
}

struct Fixture {
    TempDir dir;
    NoteStore store{dir.path()};
    Fixture() {
        REQUIRE(seed_note(store, "a.md", "see [[b]] #proj\n"));
        REQUIRE(seed_note(store, "b.md", "plain\n"));
        REQUIRE(seed_note(store, "work/c.md", "in a folder\n"));
        REQUIRE(save_view(store, "notes", "MATCH (n:Note) RETURN n", false).has_value());
    }
};

}  // namespace

TEST_CASE("user query: a read names nodes and edges the way the app does") {
    Fixture f;
    UserQuery notes = must_run(f.store, "MATCH (n:Note) RETURN n");
    CHECK(element_keys(notes) == Texts{"a.md", "b.md", "work/c.md"});
    CHECK(notes.changes == QueryChanges{});
    for (const auto& [id, element] : notes.nodes)
        CHECK(element.kind == QueryElement::Kind::Note);

    const UserQuery links = must_run(f.store, "MATCH ()-[r:LINKS_TO]->() RETURN r");
    CHECK(element_keys(links) == Texts{"a.md>b.md"});
    REQUIRE(links.edges.size() == 1);
    CHECK(links.edges.begin()->second.kind == QueryElement::Kind::Link);

    const UserQuery tagged = must_run(f.store, "MATCH (:Note)-[t:TAGGED]->(g:Tag) RETURN t, g");
    CHECK(element_keys(tagged) == Texts{"a.md>proj"});
    REQUIRE(tagged.nodes.size() == 1);
    CHECK(tagged.nodes.begin()->second == QueryElement{QueryElement::Kind::Tag, "proj"});

    // Liste içindeki düğümler de adlanır; düğüm olmayan sütun adlanmaz.
    const UserQuery listed = must_run(f.store, "MATCH (n:Note) RETURN collect(n) AS all, count(n) AS total");
    CHECK(listed.nodes.size() == 3);
    CHECK(listed.result.columns == Texts{"all", "total"});
}

TEST_CASE("user query: writing is classified by parsing, without touching the store") {
    Fixture f;
    CHECK(query_writes("MATCH (n:Note) RETURN n", {}) == false);
    CHECK(query_writes("MATCH (n:Note) SET n.status = 'done'", {}) == true);
    CHECK(query_writes("CREATE (p:Person)", {}) == true);
    const auto broken = query_writes("MATCH (n:Note RETURN n", {});
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == Error::Code::InvalidQuery);
    CHECK_FALSE(arg_of(broken.error(), "line").empty());
    CHECK_FALSE(arg_of(broken.error(), "col").empty());

    // Okuma yazan sorguyu çalıştırmaz.
    const Texts before = derived_state(f.store);
    const auto refused = run_user_query(f.store, "MATCH (n:Note) SET n.status = 'x'", QueryAction::Read, {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::InvalidQuery);
    CHECK(refused.error().message_key == "error.query_writes");
    CHECK(must_run(f.store, "MATCH (n:Note) WHERE n.status = 'x' RETURN n").result.rows.empty());
    CHECK(derived_state(f.store) == before);
}

TEST_CASE("user query: derived note data is read-only to raw Cypher, with a clear located error") {
    Fixture f;
    const Texts before = derived_state(f.store);
    struct Case {
        std::string_view cypher;
        std::string_view key;
    };
    const Case cases[] = {
        {"MATCH (n:Note) SET n.body = 'x'", "error.query_protected_property"},
        {"MATCH (n:Note) SET n.name = 'z.md'", "error.query_protected_property"},
        {"MATCH (n:Note) SET n.link_targets = 'b'", "error.query_protected_property"},
        {"MATCH (n:Note) REMOVE n.updated_at", "error.query_protected_property"},
        {"MATCH (n:Note) SET n.deleted_at = 1", "error.query_protected_property"},
        {"CREATE (n:Note {name: 'fake.md'})", "error.query_protected_label"},
        {"MERGE (n:Note {name: 'fake.md'})", "error.query_protected_label"},
        {"MATCH (n:Note) REMOVE n:Note", "error.query_protected_label"},
        {"CREATE (p:Person) SET p:Tag", "error.query_protected_label"},
        {"MATCH (n:Note) DELETE n", "error.query_protected_node"},
        {"MATCH (n:Note) DETACH DELETE n", "error.query_protected_node"},
        {"MATCH (t:Tag) DETACH DELETE t", "error.query_protected_node"},
        {"MATCH (f:Folder) DETACH DELETE f", "error.query_protected_node"},
        {"MATCH (v:View) DETACH DELETE v", "error.query_protected_node"},
        {"MATCH (a:Note)-[:LINKS_TO]->(b) CREATE (b)-[:LINKS_TO]->(a)", "error.query_protected_edge"},
        {"MATCH ()-[r:LINKS_TO]->() DELETE r", "error.query_protected_edge"},
        {"MATCH ()-[r:TAGGED]->() SET r.weight = 1", "error.query_protected_edge"},
        {"MATCH ()-[r:IN_FOLDER]->() DELETE r", "error.query_protected_edge"},
        {"MATCH (n:Note) CREATE (n)-[:TAGGED]->(:Label)", "error.query_protected_edge"},
        {"MATCH (t:Tag) SET t.name = 'other'", "error.query_protected_property"},
        {"MATCH (f:Folder) SET f.name = 'elsewhere'", "error.query_protected_property"},
        {"MATCH (v:View) SET v.query = 'MATCH (n) DETACH DELETE n'", "error.query_protected_property"},
        // Kullanıcı düğümü de türetilmiş kenarı alamaz; DETACH onu silemez.
        {"CREATE (p:Person) WITH p MATCH (n:Note) CREATE (n)-[:IN_FOLDER]->(p)", "error.query_protected_edge"},
        // İlişki ham Cypher'la kurulmaz (lead kararı): türetilmiş kimlik, yönsüzde sıralı uçlar ve ad başına tek
        // ilişki NoteStore::add_relation'ın kuralıdır; ileti haritanın Bağla…'sını ve `note relation add`i söyler.
        {"MATCH (a:Note)-[:LINKS_TO]->(b) CREATE (a)-[:RELATED_TO {name: 'uses'}]->(b)",
         "error.query_relation_create"},
        // Eşleşen MERGE yazmaz (var olan ilişki kalır); eşleşmeyen kuracağı için reddedilir.
        {"MATCH (a:Note)-[:LINKS_TO]->(b) MERGE (a)-[:RELATED_TO {name: 'new'}]->(b)",
         "error.query_relation_create"},
        // Var olan ilişkinin adı yazma kuralından geçer; directed yalnız true.
        {"MATCH ()-[r:RELATED_TO]->() SET r.name = 'two\\nlines'", "error.query_relation_name"},
        {"MATCH ()-[r:RELATED_TO]->() SET r.name = 42", "error.query_relation_name"},
        {"MATCH ()-[r:RELATED_TO]->() SET r.directed = 'yes'", "error.query_relation_directed"},
    };
    REQUIRE(f.store.add_relation(np("a.md"), np("b.md"), {"rel", false}).has_value());
    for (const Case& rejected : cases) {
        INFO(std::string(rejected.cypher));
        for (const QueryAction action : {QueryAction::Preview, QueryAction::Write}) {
            const auto done = run_user_query(f.store, rejected.cypher, action, {});
            REQUIRE_FALSE(done.has_value());
            INFO(describe(done.error()));
            CHECK(done.error().code == Error::Code::InvalidQuery);
            CHECK(done.error().message_key == rejected.key);
            CHECK_FALSE(arg_of(done.error(), "line").empty());
        }
    }
    CHECK(derived_state(f.store) == before);
    CHECK(must_run(f.store, "MATCH (p:Person) RETURN p").result.rows.empty());
    CHECK(must_run(f.store, "MATCH ()-[r:RELATED_TO]->() WHERE r.name = 'rel' RETURN r").result.rows.size() == 1);
    CHECK(must_run(f.store, "MATCH ()-[r:RELATED_TO]->() RETURN r").result.rows.size() == 1);
    CHECK(find_view(f.store, "notes")->query == "MATCH (n:Note) RETURN n");
}

// İnceleme: aynı sorguda silinen ilişkiye özellik yazmak yetim özellik bırakmaz. Motor sahibi olmayan yazmayı
// reddeder, işlem geri alınır: ilişki adıyla yerinde kalır; adsız ilişki aynı kimlikle yeniden kurulsa da eski
// yazma görünmez.
TEST_CASE("user query: deleting a relation then setting its name in one query leaves no orphan property") {
    Fixture f;
    REQUIRE(f.store.add_relation(np("a.md"), np("b.md"), {}).has_value());
    const auto done = run_user_query(f.store, "MATCH ()-[r:RELATED_TO]->() DELETE r SET r.name = 'ghost'",
                                     QueryAction::Write, {});
    REQUIRE_FALSE(done.has_value());
    CHECK(done.error().code == Error::Code::InvalidQuery);
    auto edges = f.store.edges();
    REQUIRE(edges.has_value());
    REQUIRE(std::ranges::count(*edges, EdgeKind::RelatedTo, &NoteEdge::kind) == 1);
    CHECK(std::ranges::find(*edges, EdgeKind::RelatedTo, &NoteEdge::kind)->name.empty());

    must_run(f.store, "MATCH ()-[r:RELATED_TO]->() DELETE r", QueryAction::Write);  // silme serbesttir
    REQUIRE(f.store.add_relation(np("a.md"), np("b.md"), {}).has_value());           // aynı türetilmiş kimlik
    edges = f.store.edges();
    REQUIRE(edges.has_value());
    REQUIRE(std::ranges::count(*edges, EdgeKind::RelatedTo, &NoteEdge::kind) == 1);
    CHECK(std::ranges::find(*edges, EdgeKind::RelatedTo, &NoteEdge::kind)->name.empty());
}

TEST_CASE("user query: user data is writable, counted, and the store sees new relations at once") {
    Fixture f;
    const Texts before = derived_state(f.store);

    const UserQuery status = must_run(f.store, "MATCH (n:Note) SET n.status = 'done'", QueryAction::Write);
    CHECK(status.changes.properties_set == 3);
    CHECK(status.changes.total() == 3);
    CHECK(must_run(f.store, "MATCH (n:Note) WHERE n.status = 'done' RETURN n").result.rows.size() == 3);

    // Var olan ilişkinin adı ve yönü yazılır: harita ve bağlantı paneli hemen görür (görüntü yenilendi).
    REQUIRE(f.store.add_relation(np("a.md"), np("b.md"), {"rel", true}).has_value());
    const UserQuery related = must_run(
        f.store, "MATCH ()-[r:RELATED_TO]->() SET r.name = ' uses ', r.directed = true RETURN r", QueryAction::Write);
    CHECK(related.changes.properties_set == 2);
    REQUIRE(related.edges.size() == 1);
    const QueryElement& relation = related.edges.begin()->second;
    CHECK(relation.kind == QueryElement::Kind::Relation);
    CHECK(relation.from == "a.md");
    CHECK(relation.to == "b.md");
    CHECK(relation.directed);
    const auto edges = f.store.edges();
    REQUIRE(edges.has_value());
    CHECK(std::ranges::count_if(*edges, [](const NoteEdge& edge) {
              return edge.kind == EdgeKind::RelatedTo && edge.name == "uses" && edge.directed;
          }) == 1);
    must_run(f.store, "MATCH ()-[r:RELATED_TO]->() SET r.name = 'needs', r.directed = null", QueryAction::Write);
    const auto names = f.store.relation_names();
    REQUIRE(names.has_value());
    REQUIRE((*names)->size() == 1);
    CHECK((*names)->front().key == "needs");
    CHECK((*names)->front().directed == 0);

    // Kendi düğümleri ve kenarları; nota kullanıcı etiketi; DETACH yalnız kullanıcı kenarlarını siler.
    const UserQuery person =
        must_run(f.store, "CREATE (p:Person {name: 'Ali'}) WITH p MATCH (n:Note) CREATE (n)-[:MENTIONS]->(p)",
                 QueryAction::Write);
    CHECK(person.changes.nodes_created == 1);
    CHECK(person.changes.labels_added == 1);
    CHECK(person.changes.relationships_created == 3);
    must_run(f.store, "MATCH (n:Note) SET n:Important", QueryAction::Write);
    const UserQuery gone = must_run(f.store, "MATCH (p:Person) DETACH DELETE p", QueryAction::Write);
    CHECK(gone.changes.nodes_deleted == 1);
    CHECK(gone.changes.relationships_deleted == 3);
    must_run(f.store, "MATCH (n:Note) REMOVE n:Important, n.status", QueryAction::Write);
    must_run(f.store, "MATCH (t:Tag) SET t.color = 'red'", QueryAction::Write);
    // İlişki silinebilir.
    CHECK(must_run(f.store, "MATCH ()-[r:RELATED_TO]->() DELETE r", QueryAction::Write)
              .changes.relationships_deleted == 1);

    // Türetilmiş veri hiç değişmedi.
    CHECK(derived_state(f.store) == before);
}

TEST_CASE("user query: a preview runs the write, counts it and leaves the store unchanged") {
    Fixture f;
    const UserQuery preview =
        must_run(f.store, "MATCH (n:Note) SET n.status = 'x' CREATE (p:Person) RETURN p", QueryAction::Preview);
    CHECK(preview.changes.properties_set == 3);
    CHECK(preview.changes.nodes_created == 3);
    CHECK(preview.changes.labels_added == 3);
    CHECK(preview.result.rows.size() == 3);
    CHECK(must_run(f.store, "MATCH (n:Note) WHERE n.status = 'x' RETURN n").result.rows.empty());
    CHECK(must_run(f.store, "MATCH (p:Person) RETURN p").result.rows.empty());
}

TEST_CASE("user query: the user's limits apply and an empty store reads empty without a file") {
    Fixture f;
    graph::QueryLimits limits;
    limits.max_rows = 1;
    const auto capped = run_user_query(f.store, "MATCH (n:Note) RETURN n", QueryAction::Read, limits);
    REQUIRE_FALSE(capped.has_value());
    CHECK(capped.error().code == Error::Code::InvalidQuery);
    CHECK(capped.error().message_key == "graph.limit_exceeded");
    CHECK(arg_of(capped.error(), "limit") == "rows");
    CHECK(must_run(f.store, "MATCH (n:Note) RETURN n", QueryAction::Read, {}).result.rows.size() == 3);

    const TempDir empty;
    NoteStore fresh(empty.path());
    CHECK(must_run(fresh, "MATCH (n) RETURN n").result.rows.empty());
    CHECK_FALSE(fs::exists(empty.path() / "graph.db"));
    CHECK(must_run(fresh, "CREATE (p:Person)", QueryAction::Write).changes.nodes_created == 1);
    CHECK(must_run(fresh, "MATCH (p:Person) RETURN p").result.rows.size() == 1);
}

// İnceleme (Low): commit edilmiş yazmanın sonucu adlanamazsa hata dönmek "yazılmadı" sanılırdı ve arayüz
// tazelenmezdi. Yazma başarıdır, adlandırma hatası ayrıca raporlanır; okumada hata kalır.
TEST_CASE("user query: a committed write whose naming fails is still reported as written") {
    Fixture f;
    f.store.impl().query_naming_fault = [] {
        return std::optional(Error{Error::Code::Io, "error.io", {{"path", "graph.db"}, {"detail", "fault"}}});
    };
    const auto written =
        run_user_query(f.store, "MATCH (n:Note) SET n.status = 'x' RETURN n", QueryAction::Write, {});
    REQUIRE(written.has_value());
    CHECK(written->changes.properties_set == 3);
    REQUIRE(written->naming_error.has_value());
    CHECK(written->naming_error->message_key == "error.io");
    CHECK_FALSE(run_user_query(f.store, "MATCH (n:Note) RETURN n", QueryAction::Read, {}).has_value());
    f.store.impl().query_naming_fault = nullptr;
    CHECK(must_run(f.store, "MATCH (n:Note) WHERE n.status = 'x' RETURN n").result.rows.size() == 3);
}

// İnceleme: adlandırma (not yolu, kenar uçları) sorgudan sonra UI iş parçacığında koşar; süre bütçesine
// (limits.query.max_duration) sayılır. 0 ms'de motor küçük sorguyu saate bakmadan bitirir (her 1024 adımda bakar),
// adlandırma ise süreyi aşmış bulur: sonuç kesilmiş değil LimitExceeded'dır.
TEST_CASE("user query: naming the result counts against the query's duration budget") {
    Fixture f;
    graph::QueryLimits instant;
    instant.max_duration = std::chrono::milliseconds(0);
    const auto late = run_user_query(f.store, "MATCH (n:Note) RETURN n", QueryAction::Read, instant);
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().code == Error::Code::InvalidQuery);
    CHECK(late.error().message_key == "graph.limit_exceeded");
    CHECK(arg_of(late.error(), "limit") == "duration");
    CHECK(must_run(f.store, "MATCH (n:Note) RETURN n").result.rows.size() == 3);  // varsayılan 5 sn
}

// Birleşim: sorgunun yazması öbür yazmalar gibi günlüğe girer ve öbür cihaza gelir; orada harita ilişkiyi görür.
TEST_CASE("user query: a write syncs to the other device like any other write") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "see [[y]]\n"));
    REQUIRE(seed_note(a, "y.md", "y\n"));
    const auto exchange = [&](NoteStore& from, NoteStore& to) {
        REQUIRE(export_note_log(from, logs.path()).has_value());
        const auto name = own_log_name(from);
        REQUIRE(name.has_value());
        const std::vector<fs::path> files{logs.path() / *name};
        REQUIRE(ingest_note_logs(to, files).has_value());
    };
    REQUIRE(a.add_relation(np("x.md"), np("y.md"), {}).has_value());
    exchange(a, b);
    must_run(a, "MATCH (x:Note)-[:LINKS_TO]->(y) SET x.status = 'done'", QueryAction::Write);
    must_run(a, "MATCH ()-[r:RELATED_TO]->() SET r.name = 'uses'", QueryAction::Write);
    exchange(a, b);
    CHECK(must_run(b, "MATCH (n:Note) WHERE n.status = 'done' RETURN n").result.rows.size() == 1);
    const auto edges = b.edges();
    REQUIRE(edges.has_value());
    CHECK(std::ranges::count_if(*edges, [](const NoteEdge& edge) {
              return edge.kind == EdgeKind::RelatedTo && edge.name == "uses";
          }) == 1);
}

TEST_CASE("user query: a saved view runs read-only and names its notes") {
    Fixture f;
    const auto view = run_saved_view(f.store, "Notes");
    REQUIRE(view.has_value());
    CHECK(element_keys(*view) == Texts{"a.md", "b.md", "work/c.md"});
    const auto missing = run_saved_view(f.store, "nothing");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);
}
