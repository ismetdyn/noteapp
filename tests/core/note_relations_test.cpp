// F1: adlı ilişki (SPEC-graph "Adlı ilişkiler"). RELATED_TO kenarı `name` ve `directed` taşır; kimlik
// uçlardan ve ad anahtarından türer. Birleşim iki cihazın gerçek günlük dosyalarıyla sınanır.
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace noteapp::core;
using noteapp::test::seed_note;
using noteapp::test::TempDir;
namespace fs = std::filesystem;
namespace graph = noteapp::graph;

namespace {

NotePath np(const std::string& text) {
    return *NotePath::parse(text);
}

using Dir = RelationDirection;

// Satırlar kimliksiz: karşılaştırma yol, ad ve yönledir (kimlik relation_of ile alınır).
std::vector<RelatedNote> related(const NoteStore& store, const std::string& path) {
    const auto links = store.links(np(path));
    REQUIRE(links.has_value());
    std::vector<RelatedNote> out = links->related;
    for (RelatedNote& row : out)
        row.id = {};
    return out;
}

// `from` notunun panelindeki, `to`ya giden ve gösterilen adı `name` olan ilk ilişkinin kimliği.
RelationId relation_of(const NoteStore& store, const std::string& from, const std::string& to,
                       const std::string& name) {
    const auto links = store.links(np(from));
    REQUIRE(links.has_value());
    const auto row = std::ranges::find_if(links->related, [&](const RelatedNote& candidate) {
        return candidate.path == np(to) && candidate.name == name;
    });
    REQUIRE(row != links->related.end());
    return row->id;
}

std::vector<NoteEdge> relation_edges(const NoteStore& store) {
    const auto edges = store.edges();
    REQUIRE(edges.has_value());
    std::vector<NoteEdge> out;
    std::ranges::copy_if(*edges, std::back_inserter(out),
                         [](const NoteEdge& edge) { return edge.kind == EdgeKind::RelatedTo; });
    for (NoteEdge& edge : out)
        edge.relation = {};
    return out;
}

NoteEdge relation(const std::string& from, const std::string& to, const std::string& name, bool directed) {
    return NoteEdge{np(from), np(to), EdgeKind::RelatedTo, name, directed};
}

std::vector<RelationName> names(const NoteStore& store) {
    const auto out = store.relation_names();
    REQUIRE(out.has_value());
    REQUIRE(*out != nullptr);
    return **out;
}

Error::Code add_error(NoteStore& store, const std::string& from, const std::string& to, RelationSpec spec) {
    const auto added = store.add_relation(np(from), np(to), spec);
    REQUIRE_FALSE(added.has_value());
    return added.error().code;
}

// İki cihaz: gerçek günlük dosyalarıyla. Notlar a'da kurulur, b'ye gider: iki cihaz aynı düğümleri görür.
struct Devices {
    TempDir logs;
    TempDir data_a;
    TempDir data_b;
    NoteStore a{data_a.path()};
    NoteStore b{data_b.path()};

    Devices() {
        REQUIRE(seed_note(a, "x.md", "x\n"));
        REQUIRE(seed_note(a, "y.md", "y\n"));
        send(a, b);
    }
    LogIngest send(NoteStore& from, NoteStore& to) {
        REQUIRE(export_note_log(from, logs.path()).has_value());
        const auto name = own_log_name(from);
        REQUIRE(name.has_value());
        const std::vector<fs::path> files{logs.path() / *name};
        auto ingested = ingest_note_logs(to, files);
        REQUIRE(ingested.has_value());
        return std::move(*ingested);
    }
};

// Görünür satırlar paralel kenarı teke indirir: kenar sayısı motordan okunur.
std::int64_t stored_relations(const fs::path& data) {
    auto raw = graph::Graph::open(data / "graph.db");
    REQUIRE(raw.has_value());
    const auto counted = raw->query("MATCH ()-[r:RELATED_TO]->() RETURN count(r)");
    REQUIRE(counted.has_value());
    return std::get<std::int64_t>(counted->rows[0][0].v);
}

bool reports_edge_name_conflict(const LogIngest& ingest) {
    return std::ranges::any_of(ingest.report.conflicts, [](const graph::MergeConflict& conflict) {
        return std::holds_alternative<graph::EdgeId>(conflict.owner) && conflict.key == "name";
    });
}

// Notun düğüm kimliği, motordan (Note düğümünün adı yolun dosya adıdır).
graph::NodeId note_node(graph::Graph& raw, const std::string& name) {
    auto txn = raw.read();
    REQUIRE(txn.has_value());
    auto cursor = txn->nodes_with_label("Note");
    REQUIRE(cursor.has_value());
    while (true) {
        auto next = cursor->next();
        REQUIRE(next.has_value());
        REQUIRE(next->has_value());
        const auto value = txn->property(**next, "name");
        REQUIRE(value.has_value());
        if (const auto* text = std::get_if<std::string>(&*value); text != nullptr && *text == name)
            return **next;
    }
}

std::string raw_id(const graph::NodeId& id) {
    return std::string(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
}

// Motor üzerinden ilişkilerin `name`ini doğrudan yazar (senkronla gelen bozuk değerin yerine).
void set_raw_name(const fs::path& data, const graph::Params& params) {
    auto raw = graph::Graph::open(data / "graph.db");
    REQUIRE(raw.has_value());
    REQUIRE(raw->query("MATCH ()-[r:RELATED_TO]->() SET r.name = $name", params, graph::QueryMode::ReadWrite)
                .has_value());
}

// Eşzamanlı iki yazma farklı HLC alsın: LWW kazananı belirli olsun.
void later() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

}  // namespace

TEST_CASE("a named directed relation round-trips through links, edges and a reopened store") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "a\n"));
        REQUIRE(seed_note(store, "b.md", "b\n"));
        REQUIRE(store.add_relation(np("b.md"), np("a.md"), {"  kullanır ", true}).has_value());
    }
    const NoteStore store(dir.path());
    CHECK(related(store, "b.md") == std::vector{RelatedNote{np("a.md"), "kullanır", Dir::Outgoing}});
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "kullanır", Dir::Incoming}});
    // Yönlü kenarda from kaynaktır, yol sırası değil.
    CHECK(relation_edges(store) == std::vector{relation("b.md", "a.md", "kullanır", true)});
}

TEST_CASE("an unnamed relation is undirected, its edge sorted by path, and listed once from both sides") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("b.md"), np("a.md")).has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected}});
    CHECK(related(store, "b.md") == std::vector{RelatedNote{np("a.md"), "", Dir::Undirected}});
    CHECK(relation_edges(store) == std::vector{relation("a.md", "b.md", "", false)});
}

TEST_CASE("names match with Turkish folding: one relation per pair and name key, whatever the direction") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"İlgi", false}).has_value());
    CHECK(add_error(store, "a.md", "b.md", {"ilgi", false}) == Error::Code::AlreadyExists);
    CHECK(add_error(store, "a.md", "b.md", {"ILGI", false}) == Error::Code::AlreadyExists);
    CHECK(add_error(store, "b.md", "a.md", {"ilgi", true}) == Error::Code::AlreadyExists);
    CHECK(add_error(store, "a.md", "b.md", {" İLGİ ", true}) == Error::Code::AlreadyExists);
    CHECK(related(store, "a.md").size() == 1);
}

TEST_CASE("a pair holds several differently named relations but only one unnamed") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    REQUIRE(store.add_relation(np("b.md"), np("a.md"), {"cites", false}).has_value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md")).has_value());
    CHECK(add_error(store, "b.md", "a.md", {"", false}) == Error::Code::AlreadyExists);
    CHECK(add_error(store, "b.md", "a.md", {"   ", true}) == Error::Code::AlreadyExists);
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected},
                                                RelatedNote{np("b.md"), "cites", Dir::Undirected},
                                                RelatedNote{np("b.md"), "uses", Dir::Outgoing}});
    CHECK(relation_edges(store).size() == 3);
}

TEST_CASE("a renamed relation keeps its edge and frees the old name for a new relation") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    REQUIRE(store.rename_relation(relation_of(store, "b.md", "a.md", "uses"), "needs").has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "needs", Dir::Outgoing}});

    // Yeni "uses"un türetilmiş kimliği yeniden adlandırılan kenarınkiyle aynıdır: başka kimlik alır.
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "needs", Dir::Outgoing},
                                                RelatedNote{np("b.md"), "uses", Dir::Outgoing}});
    CHECK(relation_edges(store).size() == 2);
    // Aynı adlı ilişkiyi yeniden adlandırma zinciri de çakışmaz.
    REQUIRE(store.rename_relation(relation_of(store, "a.md", "b.md", "uses"), "wants").has_value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    CHECK(relation_edges(store).size() == 3);
}

TEST_CASE("rename_relation refuses a missing relation, a taken name and a bad name; empty makes it unnamed") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", false}).has_value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
    const RelationId uses = relation_of(store, "a.md", "b.md", "uses");
    CHECK(store.rename_relation(RelationId{}, "x").error().code == Error::Code::NotFound);
    CHECK(store.rename_relation(uses, "Cites").error().code == Error::Code::AlreadyExists);
    CHECK(store.rename_relation(uses, "a\nb").error().code == Error::Code::InvalidPath);
    // Yalnız yazımı değişen ad (aynı anahtar) kabul edilir.
    REQUIRE(store.rename_relation(uses, "Uses").has_value());
    REQUIRE(store.rename_relation(relation_of(store, "a.md", "b.md", "cites"), "").has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected},
                                                RelatedNote{np("b.md"), "Uses", Dir::Undirected}});
}

TEST_CASE("relation names reject control characters, newlines, tabs and invalid UTF-8; edges are trimmed") {
    for (const std::string& bad :
         {std::string("a\nb"), std::string("a\tb"), std::string("x\x01y"), std::string("x\x7Fy"),
          std::string("x\xC2\x85y"), std::string("\xFF"), std::string("a\r\nb")}) {
        CAPTURE(bad);
        const auto checked = relation_name(bad);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().code == Error::Code::InvalidPath);
        CHECK(checked.error().message_key == "error.invalid_relation_name");
    }
    CHECK(relation_name("  uses \n") == "uses");
    CHECK(relation_name(" \t ") == "");
    CHECK(relation_name("çok uzun ad: 🙂") == "çok uzun ad: 🙂");

    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    CHECK(add_error(store, "a.md", "b.md", {"a\nb", false}) == Error::Code::InvalidPath);
    CHECK(add_error(store, "a.md", "b.md", {"a\tb", true}) == Error::Code::InvalidPath);
    CHECK(related(store, "a.md").empty());
}

TEST_CASE("relation_names counts named relations between live notes, most used first") {
    const TempDir dir;
    NoteStore store(dir.path());
    for (const char* path : {"a.md", "b.md", "c.md", "d.md"})
        REQUIRE(seed_note(store, path, "x\n"));
    CHECK(names(store).empty());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    REQUIRE(store.add_relation(np("a.md"), np("c.md"), {"Uses", true}).has_value());
    REQUIRE(store.add_relation(np("b.md"), np("c.md"), {"uses", false}).has_value());
    REQUIRE(store.add_relation(np("a.md"), np("d.md"), {"cites", false}).has_value());
    REQUIRE(store.add_relation(np("b.md"), np("d.md"), {"alıntı", true}).has_value());
    REQUIRE(store.add_relation(np("c.md"), np("d.md")).has_value());  // adsız sayılmaz

    const std::vector<RelationName> want{
        {"uses", "uses", 3, 2}, {"alıntı", "alıntı", 1, 1}, {"cites", "cites", 1, 0}};
    CHECK(names(store) == want);
    CHECK(names(store).front().default_directed());
    CHECK_FALSE(want.back().default_directed());

    // Görüntü değişince önbellek düşer: yeni ilişki ve çöpe atılan uç hemen görünür.
    REQUIRE(store.add_relation(np("c.md"), np("d.md"), {"cites", false}).has_value());
    CHECK(names(store)[1] == RelationName{"cites", "cites", 2, 0});
    REQUIRE(store.trash(np("a.md")).has_value());
    CHECK(names(store) == std::vector<RelationName>{
                              {"alıntı", "alıntı", 1, 1}, {"cites", "cites", 1, 0}, {"uses", "uses", 1, 0}});
}

// Seçici her tuşta sorar: aynı görüntüde aynı paylaşılan sonuç döner, liste kopyalanmaz.
TEST_CASE("relation_names shares one result per snapshot and rebuilds it after a write") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", false}).has_value());
    const auto first = store.relation_names();
    const auto second = store.relation_names();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->get() == second->get());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
    const auto third = store.relation_names();
    REQUIRE(third.has_value());
    CHECK(third->get() != first->get());
    CHECK((*third)->size() == 2);
    CHECK((*first)->size() == 1);  // eski sonuç tutanında geçerli kalır
}

// Arama her tuşta kenar ister: aynı görüntüde aynı paylaşılan liste, kendi yazmamızdan sonra yenisi.
TEST_CASE("shared_edges shares one list per snapshot and rebuilds it after a write") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[b]]\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    const auto first = store.shared_edges();
    const auto second = store.shared_edges();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->get() == second->get());
    CHECK(**first == store.edges().value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"uses", false}).has_value());
    const auto third = store.shared_edges();
    REQUIRE(third.has_value());
    CHECK(third->get() != first->get());
    CHECK((*third)->size() == 2);
    CHECK((*first)->size() == 1);

    const TempDir empty;
    const NoteStore none(empty.path());
    CHECK(none.shared_edges().value()->empty());
}

// Senkronla gelen Tag düğümünün adı yazma kuralından geçmemiştir: ilişki adı gibi okunurken temizlenir.
TEST_CASE("a synced tag name is read sanitised, trimmed and capped like a relation name") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "#tagx\n"));
    }
    const auto set_tag = [&](const std::string& name) {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (t:Tag) SET t.name = $name, t.display = $name",
                           {{"name", graph::QueryValue(name)}}, graph::QueryMode::ReadWrite)
                    .has_value());
    };
    set_tag(" x\ny.md\x1b ");
    {
        const NoteStore store(dir.path());
        const std::string clean = "x\xEF\xBF\xBDy.md\xEF\xBF\xBD";
        REQUIRE(store.tags().size() == 1);
        CHECK(store.tags().front().name == clean);
        CHECK(store.tags().front().display == clean);
        REQUIRE(store.list().size() == 1);
        CHECK(store.list().front().tags == std::vector<std::string>{clean});
    }
    set_tag(std::string(kDefaultRelationNameBytes + 10, 'q'));
    const NoteStore store(dir.path());
    CHECK(store.tags().front().name.size() == kDefaultRelationNameBytes);
}

TEST_CASE("relation_names of a store without a database is empty and creates nothing") {
    const TempDir dir;
    const NoteStore store(dir.path());
    CHECK(names(store).empty());
    CHECK_FALSE(fs::exists(dir.path() / "graph.db"));
}

TEST_CASE("Cypher sees the relation name and direction as edge properties") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "a\n"));
        REQUIRE(seed_note(store, "b.md", "b\n"));
        REQUIRE(store.add_relation(np("b.md"), np("a.md"), {"uses", true}).has_value());
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
        REQUIRE(store.add_relation(np("a.md"), np("b.md")).has_value());
    }
    auto raw = graph::Graph::open(dir.path() / "graph.db");
    REQUIRE(raw.has_value());
    const auto result = raw->query(
        "MATCH (a:Note)-[r:RELATED_TO]->(b:Note) WHERE r.name = 'uses' RETURN a.name, b.name, r.directed");
    REQUIRE(result.has_value());
    REQUIRE(result->rows.size() == 1);
    CHECK(std::get<std::string>(result->rows[0][0].v) == "b");
    CHECK(std::get<std::string>(result->rows[0][1].v) == "a");
    CHECK(std::get<bool>(result->rows[0][2].v));
    // Adsız ve yönsüz ilişki özellik taşımaz (boş ad ya da false yazılmaz).
    const auto unnamed =
        raw->query("MATCH ()-[r:RELATED_TO]->() WHERE r.name IS NULL AND r.directed IS NULL RETURN count(r)");
    REQUIRE(unnamed.has_value());
    CHECK(std::get<std::int64_t>(unnamed->rows[0][0].v) == 1);
}

TEST_CASE("set_relation_direction makes a relation directed, flips it and makes it undirected again") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"Uses", false}).has_value());

    REQUIRE(seed_note(store, "c.md", "c\n"));
    const auto uses = [&] { return relation_of(store, "a.md", "b.md", "Uses"); };

    REQUIRE(store.set_relation_direction(uses(), np("b.md")).has_value());
    CHECK(relation_edges(store) == std::vector{relation("b.md", "a.md", "Uses", true)});
    REQUIRE(store.set_relation_direction(uses(), np("a.md")).has_value());
    CHECK(relation_edges(store) == std::vector{relation("a.md", "b.md", "Uses", true)});
    REQUIRE(store.set_relation_direction(uses(), std::nullopt).has_value());
    CHECK(relation_edges(store) == std::vector{relation("a.md", "b.md", "Uses", false)});
    // Zaten istenen durumdaki ilişki hata vermez; olmayan kimlik NotFound, uç olmayan kaynak InvalidPath.
    REQUIRE(store.set_relation_direction(uses(), std::nullopt).has_value());
    CHECK(store.set_relation_direction(RelationId{}, np("a.md")).error().code == Error::Code::NotFound);
    CHECK(store.set_relation_direction(uses(), np("c.md")).error().code == Error::Code::InvalidPath);
    CHECK(relation_edges(store).size() == 1);
}

// İnceleme bulgusu (veri kaybı): kaldırma kimlikle tek ilişkiyi hedefler, çiftin öbür ilişkileri kalır.
TEST_CASE("remove_relation removes only the identified relation of a pair") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    const auto uses = store.add_relation(np("a.md"), np("b.md"), {"uses", true});
    REQUIRE(uses.has_value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"cites", false}).has_value());
    REQUIRE(store.add_relation(np("a.md"), np("b.md")).has_value());
    // add_relation'ın döndürdüğü kimlik panelin ve haritanın kimliğidir.
    CHECK(*uses == relation_of(store, "b.md", "a.md", "uses"));
    const auto edges = store.edges();
    REQUIRE(edges.has_value());
    CHECK(std::ranges::count(*edges, *uses, &NoteEdge::relation) == 1);

    CHECK(store.remove_relation(RelationId{}).error().code == Error::Code::NotFound);
    REQUIRE(store.remove_relation(*uses).has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected},
                                                RelatedNote{np("b.md"), "cites", Dir::Undirected}});
    CHECK(store.remove_relation(*uses).error().code == Error::Code::NotFound);  // artık yok
    REQUIRE(store.remove_relation(relation_of(store, "a.md", "b.md", "")).has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "cites", Dir::Undirected}});

    // Çöpteki uç: ilişki grafta durur ama hedeflenemez.
    const RelationId cites = relation_of(store, "a.md", "b.md", "cites");
    REQUIRE(store.trash(np("b.md")).has_value());
    CHECK(store.remove_relation(cites).error().code == Error::Code::NotFound);
}

// Cypher'la yazılmış bozuk ad (denetim karakteri, sayı) temizlenmiş gösterilir ve kimlikle hedeflenir.
TEST_CASE("a relation whose name was set by Cypher is shown sanitised and can be renamed and removed by id") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "a\n"));
        REQUIRE(seed_note(store, "b.md", "b\n"));
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"x", false}).has_value());
    }
    set_raw_name(dir.path(), {{"name", graph::QueryValue(std::string("a\x1b"))}});
    {
        NoteStore store(dir.path());
        CHECK(related(store, "a.md") ==
              std::vector{RelatedNote{np("b.md"), "a\xEF\xBF\xBD", Dir::Undirected}});
        REQUIRE(store.rename_relation(relation_of(store, "a.md", "b.md", "a\xEF\xBF\xBD"), "ok").has_value());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "ok", Dir::Undirected}});
    }
    set_raw_name(dir.path(), {{"name", graph::QueryValue(std::int64_t{1})}});
    {
        NoteStore store(dir.path());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected}});
        REQUIRE(store.remove_relation(relation_of(store, "a.md", "b.md", "")).has_value());
        CHECK(related(store, "a.md").empty());
    }
}

// --- Birleşim ---

TEST_CASE("concurrent renames converge to the same winner on both devices and are reported") {
    Devices d;
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", false}).has_value());
    d.send(d.a, d.b);
    REQUIRE(d.a.rename_relation(relation_of(d.a, "x.md", "y.md", "uses"), "needs").has_value());
    later();
    REQUIRE(d.b.rename_relation(relation_of(d.b, "x.md", "y.md", "uses"), "wants").has_value());

    const LogIngest at_b = d.send(d.a, d.b);
    const LogIngest at_a = d.send(d.b, d.a);
    CHECK(reports_edge_name_conflict(at_b));
    CHECK(reports_edge_name_conflict(at_a));
    const std::vector want{RelatedNote{np("y.md"), "wants", Dir::Undirected}};
    CHECK(related(d.a, "x.md") == want);
    CHECK(related(d.b, "x.md") == want);
}

TEST_CASE("a rename concurrent with a removal keeps the relation and its direction on both devices") {
    Devices d;
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", true}).has_value());
    d.send(d.a, d.b);
    REQUIRE(d.a.remove_relation(relation_of(d.a, "x.md", "y.md", "uses")).has_value());
    REQUIRE(d.b.rename_relation(relation_of(d.b, "x.md", "y.md", "uses"), "needs").has_value());
    d.send(d.a, d.b);
    d.send(d.b, d.a);
    const std::vector want{RelatedNote{np("y.md"), "needs", Dir::Outgoing}};
    CHECK(related(d.a, "x.md") == want);
    CHECK(related(d.b, "x.md") == want);
}

TEST_CASE("the same name created on two devices is one relation") {
    Devices d;
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", false}).has_value());
    later();
    REQUIRE(d.b.add_relation(np("y.md"), np("x.md"), {"Uses", false}).has_value());
    d.send(d.a, d.b);
    d.send(d.b, d.a);
    // Yazım LWW: büyük HLC'li b'ninki.
    const std::vector want{RelatedNote{np("y.md"), "Uses", Dir::Undirected}};
    CHECK(related(d.a, "x.md") == want);
    CHECK(related(d.b, "x.md") == want);
    CHECK(relation_edges(d.a).size() == 1);
    CHECK(relation_edges(d.b).size() == 1);
}

TEST_CASE("an unnamed relation made as x to y on one device and y to x on another is one edge") {
    Devices d;
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md")).has_value());
    REQUIRE(d.b.add_relation(np("y.md"), np("x.md")).has_value());
    d.send(d.a, d.b);
    d.send(d.b, d.a);
    const std::vector want{relation("x.md", "y.md", "", false)};
    CHECK(relation_edges(d.a) == want);
    CHECK(relation_edges(d.b) == want);
    CHECK(related(d.a, "y.md").size() == 1);
    CHECK(stored_relations(d.data_a.path()) == 1);
    CHECK(stored_relations(d.data_b.path()) == 1);
}

// Tanım: aynı ad iki cihazda ters yönde kurulursa iki kenar da kalır (iki niyet de korunur, veri düşmez); iki
// cihaz aynı iki ilişkiyi görür. set_relation_direction ikisini teke indirir, bu da öbür cihaza gider.
TEST_CASE(
    "one name made directed in opposite directions on two devices keeps both, until a direction is set") {
    Devices d;
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", true}).has_value());
    REQUIRE(d.b.add_relation(np("y.md"), np("x.md"), {"uses", true}).has_value());
    d.send(d.a, d.b);
    d.send(d.b, d.a);
    const std::vector both{RelatedNote{np("y.md"), "uses", Dir::Outgoing},
                           RelatedNote{np("y.md"), "uses", Dir::Incoming}};
    CHECK(related(d.a, "x.md") == both);
    CHECK(related(d.b, "x.md") == both);
    CHECK(names(d.a) == std::vector<RelationName>{{"uses", "uses", 2, 2}});

    REQUIRE(d.b.set_relation_direction(relation_of(d.b, "x.md", "y.md", "uses"), np("x.md")).has_value());
    d.send(d.b, d.a);
    const std::vector one{RelatedNote{np("y.md"), "uses", Dir::Outgoing}};
    CHECK(related(d.a, "x.md") == one);
    CHECK(related(d.b, "x.md") == one);
}

// --- İnceleme bulguları: kötü niyetli ya da bozuk senkron verisi ---



TEST_CASE("relation names also reject bidi and zero-width characters and names over the byte limit") {
    for (const std::string& hidden : {std::string("a\xE2\x80\xAE"
                                                  "b"),
                                      std::string("a\xE2\x80\x8B"
                                                  "b"),
                                      std::string("\xEF\xBB\xBF"
                                                  "a")}) {
        CAPTURE(hidden);
        const auto checked = relation_name(hidden);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().args == std::vector<std::pair<std::string, std::string>>{
                                          {"name", hidden}, {"reason", "hidden_control"}});
    }
    CHECK(relation_name("a\xE2\x80\x8D"
                        "b") == "a\xE2\x80\x8D"
                                "b");  // ZWJ meşrudur (emoji dizileri)
    const std::string longest(kDefaultRelationNameBytes, 'a');
    CHECK(relation_name(longest) == longest);
    const auto too_long = relation_name(longest + "a");
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().args.back() == std::pair<std::string, std::string>{"reason", "too_long"});
    CHECK(relation_name(longest + "a", SIZE_MAX) == longest + "a");  // sınırsız

    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    CHECK(add_error(store, "a.md", "b.md",
                    {"a\xE2\x80\xAE"
                     "b",
                     false}) == Error::Code::InvalidPath);
    CHECK(add_error(store, "a.md", "b.md", {longest + "a", false}) == Error::Code::InvalidPath);
}

// Senkronla gelen ad yazma kuralından geçmemiş olabilir: okunurken kırpılır, bozuk karakter U+FFFD olur,
// sınırdan uzunu kesilir. Anahtar bu biçimden türer: " uses" ile "uses" aynı ilişki adıdır.
TEST_CASE("a synced name is read trimmed and sanitised, and its key keeps one relation per name") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "a\n"));
        REQUIRE(seed_note(store, "b.md", "b\n"));
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"x", false}).has_value());
    }
    set_raw_name(dir.path(), {{"name", graph::QueryValue(std::string(" uses\t"))}});
    {
        NoteStore store(dir.path());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "uses", Dir::Undirected}});
        CHECK(add_error(store, "b.md", "a.md", {"USES", false}) == Error::Code::AlreadyExists);
    }
    set_raw_name(dir.path(), {{"name", graph::QueryValue(std::string("a\x1b\xE2\x80\xAE"
                                                                     ""
                                                                     "b"))}});
    {
        const NoteStore store(dir.path());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"),
                                                                "a\xEF\xBF\xBD"
                                                                "\xEF\xBF\xBD"
                                                                "b",
                                                                Dir::Undirected}});
    }
    set_raw_name(dir.path(), {{"name", graph::QueryValue(std::int64_t{1})}});
    {
        NoteStore store(dir.path());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), "", Dir::Undirected}});
        CHECK(add_error(store, "a.md", "b.md", {}) == Error::Code::AlreadyExists);  // adsız en çok bir tane
    }
    // Sınırdan uzun ad grafta olduğu gibi kalır; gösterim ve anahtar sınıra (kod noktası sınırında) kesilir.
    const std::string head(kDefaultRelationNameBytes - 1, 'a');
    set_raw_name(dir.path(), {{"name", graph::QueryValue(head + "çok uzun")}});
    {
        NoteStore store(dir.path());
        CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), head, Dir::Undirected}});
        CHECK(add_error(store, "a.md", "b.md", {head, false}) == Error::Code::AlreadyExists);
    }
}

// İnceleme bulgusu (veri kaybı): temizlenmiş gösterimi aynı ama saklı adı farklı iki senkron ilişki ayrı
// ilişkidir. Yön değiştirme yalnız saklı adı aynı olan kopyayı teke indirir, öbürünü silmez.
TEST_CASE("set_relation_direction keeps a relation whose stored name differs but reads the same") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "a\n"));
        REQUIRE(seed_note(store, "b.md", "b\n"));
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"p", false}).has_value());
        REQUIRE(store.add_relation(np("a.md"), np("b.md"), {"q", false}).has_value());
    }
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        const std::string rename = "MATCH ()-[r:RELATED_TO]->() WHERE r.name = $old SET r.name = $new";
        // Yön karakteri ile sıfır genişlikli boşluk: ikisi de U+FFFD okunur, anahtar aynı.
        REQUIRE(raw->query(rename,
                           {{"old", graph::QueryValue(std::string("p"))},
                            {"new", graph::QueryValue(std::string("a\xE2\x80\xAE"
                                                                  "b"))}},
                           graph::QueryMode::ReadWrite)
                    .has_value());
        REQUIRE(raw->query(rename,
                           {{"old", graph::QueryValue(std::string("q"))},
                            {"new", graph::QueryValue(std::string("a\xE2\x80\x8B"
                                                                  "b"))}},
                           graph::QueryMode::ReadWrite)
                    .has_value());
    }
    NoteStore store(dir.path());
    const std::string shown = "a\xEF\xBF\xBD"
                              "b";
    REQUIRE(related(store, "a.md").size() == 2);
    REQUIRE(store.set_relation_direction(relation_of(store, "a.md", "b.md", shown), np("a.md")).has_value());
    CHECK(related(store, "a.md") == std::vector{RelatedNote{np("b.md"), shown, Dir::Undirected},
                                                RelatedNote{np("b.md"), shown, Dir::Outgoing}});
}

// Kimlik gaspı: senkronla gelen bir kenar, ilişkinin türetilmiş kimliğini başka uçlarla taşıyabilir. Yeni
// ilişki o kimliği atlar (tuzlanır), kurulamaz kalmaz.
TEST_CASE("a synced edge squatting the derived id of a relation does not block the relation") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "x.md", "x\n"));
        REQUIRE(seed_note(store, "y.md", "y\n"));
        REQUIRE(seed_note(store, "z.md", "z\n"));
    }
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        const graph::NodeId x = note_node(*raw, "x");
        const graph::NodeId y = note_node(*raw, "y");
        const graph::NodeId z = note_node(*raw, "z");
        const std::string canonical = raw_id(x) + raw_id(y) + "uses";
        const graph::EdgeId derived =
            graph::derive_edge_id("RELATED_TO", std::as_bytes(std::span(canonical.data(), canonical.size())));
        auto txn = raw->write();
        REQUIRE(txn.has_value());
        REQUIRE(txn->put_edge(derived, x, "SQUAT", z).has_value());
        REQUIRE(txn->commit().has_value());
    }
    NoteStore store(dir.path());
    REQUIRE(store.add_relation(np("x.md"), np("y.md"), {"uses", true}).has_value());
    CHECK(related(store, "x.md") == std::vector{RelatedNote{np("y.md"), "uses", Dir::Outgoing}});
}

// Tuzlu kimlik aynı durumu gören iki cihazda aynıdır: yeniden adlandırmalardan sonra iki cihazın eşzamanlı
// aynı adlı yeni ilişkisi tek kenardır. Uzun tuz zinciri (çok yeniden adlandırma) doğrusal kalır.
TEST_CASE("a salted relation id converges on two devices after many renames") {
    Devices d;
    for (int round = 0; round < 40; ++round) {
        REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", false}).has_value());
        REQUIRE(d.a.rename_relation(relation_of(d.a, "x.md", "y.md", "uses"), "uses " + std::to_string(round))
                    .has_value());
    }
    d.send(d.a, d.b);
    REQUIRE(d.a.add_relation(np("x.md"), np("y.md"), {"uses", false}).has_value());
    later();
    REQUIRE(d.b.add_relation(np("y.md"), np("x.md"), {"Uses", false}).has_value());
    d.send(d.a, d.b);
    d.send(d.b, d.a);
    CHECK(stored_relations(d.data_a.path()) == 41);
    CHECK(stored_relations(d.data_b.path()) == 41);
    CHECK(names(d.a).front() == RelationName{"uses", "Uses", 1, 0});
    CHECK(names(d.b).front() == RelationName{"uses", "Uses", 1, 0});
}
