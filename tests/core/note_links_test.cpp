// T78: bağlantı paneli için NoteStore::links ve RELATED_TO yazmaları. Yollar komşuluk gezinmesiyle çözülür;
// çöp ve ulaşılamayan not listeye girmez, çözülemeyen [[hedef]] ayrı listededir.
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

Texts texts(const std::vector<NotePath>& paths) {
    Texts out;
    for (const NotePath& path : paths)
        out.push_back(path.string());
    return out;
}

void exchange_log(NoteStore& from, NoteStore& to, const fs::path& logs) {
    REQUIRE(export_note_log(from, logs).has_value());
    const auto name = own_log_name(from);
    REQUIRE(name.has_value());
    const std::vector<fs::path> files{logs / *name};
    REQUIRE(ingest_note_logs(to, files).has_value());
}

// Yalnız LINKS_TO, "kaynak>hedef" biçiminde, sıralı.
Texts link_edges(const NoteStore& store) {
    const auto edges = store.edges();
    REQUIRE(edges.has_value());
    Texts out;
    for (const NoteEdge& edge : *edges)
        if (edge.kind == EdgeKind::LinksTo)
            out.push_back(edge.from.string() + ">" + edge.to.string());
    return out;
}

void write_body(NoteStore& store, const std::string& path, const std::string& body) {
    const auto current = store.read(np(path));
    REQUIRE(current.has_value());
    REQUIRE(store.write(np(path), body, current->revision).has_value());
}

std::uint64_t last_seq(NoteStore& store) {
    const auto seq = own_last_seq(store);
    REQUIRE(seq.has_value());
    return *seq;
}

}  // namespace

TEST_CASE("links returns outgoing and incoming LINKS_TO as sorted paths") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "work/b.md", "b\n"));
    REQUIRE(seed_note(store, "c.md", "c\n"));
    REQUIRE(seed_note(store, "a.md", "[[work/b.md]] [[c]] [[c.md]]\n"));
    REQUIRE(seed_note(store, "d.md", "[[a]]\n"));

    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(texts(links->outgoing) == Texts{"c.md", "work/b.md"});
    CHECK(texts(links->incoming) == Texts{"d.md"});
    CHECK(links->related.empty());
    CHECK(links->missing.empty());
    CHECK_FALSE(links->empty());
}

TEST_CASE("links of an unknown note is NotFound, of a note without links empty") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "text\n"));
    CHECK(store.links(np("yok.md")).error().code == Error::Code::NotFound);
    const auto links = store.links(np("x.md"));
    REQUIRE(links.has_value());
    CHECK(links->empty());
}

TEST_CASE("links lists an unresolved target as missing, not as outgoing") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[hedef]] [[yok|takma ad]] [[yok#bölüm]]\n"));
    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(links->outgoing.empty());
    CHECK(links->missing == Texts{"hedef", "yok"});
    CHECK_FALSE(links->empty());
}

TEST_CASE("resolve_link follows the write rule: path, path without type, file name, bare name") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "work/b.md", "b\n"));
    REQUIRE(seed_note(store, "c.md", "c\n"));
    for (const char* target : {"work/b.md", "work/b", "b.md", "b"})
        CHECK_MESSAGE(store.resolve_link(target) == np("work/b.md"), target);
    // Takma ad, başlık ve kenar boşluğu panelle aynı kuralla atılır.
    CHECK(store.resolve_link(" c | takma ad ") == np("c.md"));
    CHECK(store.resolve_link("c#bölüm") == np("c.md"));
    CHECK_FALSE(store.resolve_link("yok").has_value());
    CHECK_FALSE(store.resolve_link("").has_value());
    CHECK_FALSE(store.resolve_link("c\nd").has_value());

    REQUIRE(store.trash(np("c.md")).has_value());
    CHECK_FALSE(store.resolve_link("c").has_value());
}

TEST_CASE("a self link is neither outgoing nor missing") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[a]]\n"));
    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(links->empty());
}

TEST_CASE("a binary note reports no missing targets") {
    const TempDir dir;
    NoteStore store(dir.path());
    std::string bytes = "[[hedef]]";
    bytes += '\0';
    REQUIRE(seed_note(store, "a.md", bytes));
    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(links->empty());
}

// T92: hedef kaynaktan sonra oluşunca kenar o yazmada kurulur; kaynağın yeniden kaydı beklenmez.
TEST_CASE("a target created after the source gets the edge in the same write") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[later]]\n"));
    REQUIRE(seed_note(store, "work/b.md", "[[work/later.md]] [[later.md|takma]]\n"));
    CHECK(link_edges(store).empty());
    REQUIRE(seed_note(store, "work/later.md", "x\n"));
    CHECK(link_edges(store) == Texts{"a.md>work/later.md", "work/b.md>work/later.md"});
    const auto links = store.links(np("work/later.md"));
    REQUIRE(links.has_value());
    CHECK(texts(links->incoming) == Texts{"a.md", "work/b.md"});
    CHECK(store.links(np("a.md"))->missing.empty());
}

TEST_CASE("moving or renaming a note onto a linked name adds the edge") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[x]]\n"));
    REQUIRE(seed_note(store, "b.md", "[[arsiv/y.md]]\n"));
    REQUIRE(seed_note(store, "draft.md", "d\n"));
    REQUIRE(seed_note(store, "y.md", "y\n"));
    REQUIRE(store.move(np("draft.md"), np("notes/x.md")).has_value());
    REQUIRE(store.move(np("y.md"), np("arsiv/y.md")).has_value());
    CHECK(link_edges(store) == Texts{"a.md>notes/x.md", "b.md>arsiv/y.md"});
}

TEST_CASE("renaming a folder adds the edges of links that now resolve by path") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[yeni/alt/x]]\n"));
    REQUIRE(seed_note(store, "eski/alt/x.md", "x\n"));
    REQUIRE(store.rename_folder("eski", "yeni").has_value());
    CHECK(link_edges(store) == Texts{"a.md>yeni/alt/x.md"});
}

TEST_CASE("a target restored from the trash gets the edges of sources saved while it was away") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "t.md", "t\n"));
    REQUIRE(store.trash(np("t.md")).has_value());
    REQUIRE(seed_note(store, "a.md", "[[t]]\n"));
    CHECK(link_edges(store).empty());
    REQUIRE(store.restore(np("t.md")).has_value());
    CHECK(link_edges(store) == Texts{"a.md>t.md"});
}

TEST_CASE("a source restored from the trash links targets created while it was away") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "[[t]]\n"));
    REQUIRE(store.trash(np("a.md")).has_value());
    REQUIRE(seed_note(store, "t.md", "t\n"));
    REQUIRE(store.restore(np("a.md")).has_value());
    CHECK(link_edges(store) == Texts{"a.md>t.md"});
}

TEST_CASE("deleting a target drops its edges, creating it again brings them back") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "1\n"));
    REQUIRE(seed_note(store, "a.md", "[[x]]\n"));
    REQUIRE(link_edges(store) == Texts{"a.md>x.md"});
    REQUIRE(store.remove(np("x.md")).has_value());
    CHECK(link_edges(store).empty());
    CHECK(store.links(np("a.md"))->missing == Texts{"x"});
    REQUIRE(seed_note(store, "x.md", "2\n"));
    CHECK(link_edges(store) == Texts{"a.md>x.md"});

    REQUIRE(store.trash(np("x.md")).has_value());
    REQUIRE(store.purge(np("x.md")).has_value());
    CHECK(link_edges(store).empty());
    REQUIRE(seed_note(store, "x.md", "3\n"));
    CHECK(link_edges(store) == Texts{"a.md>x.md"});
}

// İki cihaz kaynağı ve hedefi ayrı kurar; her teslim sırasında herkes aynı kenara varır. Türetilmiş kenar
// yeniden alışverişte işlem yazmaz: cihazlar arasında gidip gelmez.
TEST_CASE("source and target created on different devices converge to one edge without ping-pong") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    const TempDir data_c;
    const TempDir data_d;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    NoteStore c(data_c.path());
    NoteStore d(data_d.path());
    REQUIRE(seed_note(a, "src.md", "[[hedef]]\n"));
    REQUIRE(seed_note(b, "hedef.md", "h\n"));
    exchange_log(a, c, logs.path());
    exchange_log(b, c, logs.path());
    exchange_log(b, d, logs.path());
    exchange_log(a, d, logs.path());
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());

    const Texts want{"src.md>hedef.md"};
    CHECK(link_edges(a) == want);
    CHECK(link_edges(b) == want);
    CHECK(link_edges(c) == want);
    CHECK(link_edges(d) == want);

    const std::uint64_t seq_a = last_seq(a);
    const std::uint64_t seq_b = last_seq(b);
    const std::uint64_t seq_c = last_seq(c);
    const std::uint64_t seq_d = last_seq(d);
    for (int round = 0; round < 2; ++round) {
        exchange_log(a, b, logs.path());
        exchange_log(b, a, logs.path());
        for (NoteStore* other : {&c, &d}) {
            exchange_log(a, *other, logs.path());
            exchange_log(b, *other, logs.path());
        }
        exchange_log(c, a, logs.path());
        exchange_log(d, b, logs.path());
    }
    CHECK(last_seq(a) == seq_a);
    CHECK(last_seq(b) == seq_b);
    CHECK(last_seq(c) == seq_c);
    CHECK(last_seq(d) == seq_d);
    CHECK(link_edges(a) == want);
    CHECK(link_edges(b) == want);
}

TEST_CASE("a graph written before late edges existed gets them on first open") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "a.md", "[[x]]\n"));
        REQUIRE(seed_note(store, "x.md", "x\n"));
        REQUIRE(seed_note(store, "b.md", "[[x]]\n"));
    }
    {
        // Önceki sürümün grafı: sonradan oluşan hedefin kenarı yok, türetilmiş hedef listesi yok.
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (a:Note {name: 'a'})-[r:LINKS_TO]->() DELETE r", {},
                           graph::QueryMode::ReadWrite)
                    .has_value());
        REQUIRE(
            raw->query("MATCH (n:Note) REMOVE n.link_targets", {}, graph::QueryMode::ReadWrite).has_value());
    }
    fs::remove(dir.path() / "links-indexed");

    const NoteStore reopened(dir.path());
    CHECK(link_edges(reopened) == Texts{"a.md>x.md", "b.md>x.md"});
    CHECK(fs::exists(dir.path() / "links-indexed"));
}

TEST_CASE("renaming a target keeps the edge, leaves the text stale and reports the old name as missing") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "x\n"));
    const std::string body = "[[x]]\n";
    REQUIRE(seed_note(store, "a.md", body));
    REQUIRE(store.move(np("x.md"), np("y.md")).has_value());

    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(texts(links->outgoing) == Texts{"y.md"});
    CHECK(links->missing == Texts{"x"});
    CHECK(texts(store.links(np("y.md"))->incoming) == Texts{"a.md"});
    // Gövde yeniden yazılmaz.
    CHECK(store.read(np("a.md"))->text == body);

    // Kaynak yeniden kaydedilince kenar metne uyar: yalnız eksik kalır.
    const auto current = store.read(np("a.md"));
    REQUIRE(store.write(np("a.md"), "[[x]]\nek\n", current->revision).has_value());
    const auto saved = store.links(np("a.md"));
    REQUIRE(saved.has_value());
    CHECK(saved->outgoing.empty());
    CHECK(saved->missing == Texts{"x"});
}

TEST_CASE("trashed notes leave every list and come back on restore") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "u.md", "u\n"));
    REQUIRE(seed_note(store, "a.md", "[[u]] [[t]]\n"));
    REQUIRE(seed_note(store, "t.md", "[[a]]\n"));
    REQUIRE(store.add_relation(np("a.md"), np("u.md")).has_value());
    CHECK(texts(store.links(np("a.md"))->incoming) == Texts{"t.md"});

    REQUIRE(store.trash(np("t.md")).has_value());
    REQUIRE(store.trash(np("u.md")).has_value());
    const auto links = store.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(links->outgoing.empty());
    CHECK(links->incoming.empty());
    CHECK(links->related.empty());
    CHECK(links->missing == Texts{"t", "u"});
    CHECK(store.links(np("t.md")).error().code == Error::Code::NotFound);

    REQUIRE(store.restore(np("u.md")).has_value());
    CHECK(texts(store.links(np("a.md"))->outgoing) == Texts{"u.md"});
    CHECK(texts(store.links(np("a.md"))->related) == Texts{"u.md"});
}

TEST_CASE("an edge to an unreachable or non-note node is not listed") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "text\n"));
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (a:Note) WHERE a.name = 'a' CREATE (a)-[:RELATED_TO]->(:Note {name: ''})",
                           {}, graph::QueryMode::ReadWrite)
                    .has_value());
        REQUIRE(raw->query("MATCH (a:Note) WHERE a.name = 'a' CREATE (a)-[:LINKS_TO]->(:Folder {name: 'f'})",
                           {}, graph::QueryMode::ReadWrite)
                    .has_value());
    }
    NoteStore fresh(dir.path());
    REQUIRE(fresh.unreachable().size() == 1);
    const auto links = fresh.links(np("a.md"));
    REQUIRE(links.has_value());
    CHECK(links->empty());
}

TEST_CASE("add_relation shows on both notes, is removable from either side and survives a rename") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));

    REQUIRE(store.add_relation(np("a.md"), np("b.md")).has_value());
    CHECK(texts(store.links(np("a.md"))->related) == Texts{"b.md"});
    CHECK(texts(store.links(np("b.md"))->related) == Texts{"a.md"});
    CHECK(store.links(np("a.md"))->outgoing.empty());

    REQUIRE(store.move(np("b.md"), np("c.md")).has_value());
    CHECK(texts(store.links(np("a.md"))->related) == Texts{"c.md"});

    REQUIRE(store.remove_relation(np("c.md"), np("a.md")).has_value());
    CHECK(store.links(np("a.md"))->empty());
    CHECK(store.links(np("c.md"))->empty());

    // Aynı türetilmiş kenar yeniden kurulabilir.
    REQUIRE(store.add_relation(np("c.md"), np("a.md")).has_value());
    CHECK(texts(store.links(np("a.md"))->related) == Texts{"c.md"});
}

TEST_CASE("add_relation rejects a missing note, a self relation and a duplicate in either direction") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    CHECK(store.add_relation(np("a.md"), np("yok.md")).error().code == Error::Code::NotFound);
    CHECK(store.add_relation(np("yok.md"), np("a.md")).error().code == Error::Code::NotFound);
    CHECK(store.add_relation(np("a.md"), np("a.md")).error().code == Error::Code::InvalidPath);
    REQUIRE(store.add_relation(np("a.md"), np("b.md")).has_value());
    CHECK(store.add_relation(np("a.md"), np("b.md")).error().code == Error::Code::AlreadyExists);
    CHECK(store.add_relation(np("b.md"), np("a.md")).error().code == Error::Code::AlreadyExists);
    CHECK(store.remove_relation(np("a.md"), np("yok.md")).error().code == Error::Code::NotFound);
}

TEST_CASE("remove_relation of an unrelated pair is NotFound and leaves LINKS_TO alone") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "b.md", "b\n"));
    REQUIRE(seed_note(store, "a.md", "[[b]]\n"));
    CHECK(store.remove_relation(np("a.md"), np("b.md")).error().code == Error::Code::NotFound);
    CHECK(store.links(np("a.md"))->outgoing.size() == 1);
}

TEST_CASE("a relation reaches a second device through the log, its removal too") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "x\n"));
    REQUIRE(seed_note(a, "y.md", "y\n"));
    REQUIRE(a.add_relation(np("x.md"), np("y.md")).has_value());
    exchange_log(a, b, logs.path());
    CHECK(texts(b.links(np("x.md"))->related) == Texts{"y.md"});

    REQUIRE(b.remove_relation(np("y.md"), np("x.md")).has_value());
    exchange_log(b, a, logs.path());
    CHECK(a.links(np("x.md"))->related.empty());
}

// İnceleme bulgusu: çakışmada kazanan gövde yeniden yazılırken link_targets kaybedenin değerinde kalıyordu.
// A çevrimdışıyken B'de oluşan hedef kazanan gövdenin bağlantısını alır.
TEST_CASE("the winning body of a conflict keeps its link targets: a target created later is linked") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "n.md", "[[x]]\n"));
    exchange_log(a, b, logs.path());
    write_body(a, "n.md", "plain A\n");  // link_targets silinir
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_body(b, "n.md", "[[x]] B\n");  // hedefler aynı: link_targets yazılmaz; B'nin HLC'si büyük, kazanır
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());
    REQUIRE(b.read(np("n.md"))->text == "[[x]] B\n");
    REQUIRE(seed_note(b, "x.md", "x\n"));
    const Texts edges = link_edges(b);
    CHECK(std::ranges::find(edges, "n.md>x.md") != edges.end());
}

TEST_CASE("a conflict copy gets the links and link targets of its body like any note") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "x\n"));
    REQUIRE(seed_note(a, "n.md", "base\n"));
    exchange_log(a, b, logs.path());
    write_body(a, "n.md", "[[x]] A\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_body(b, "n.md", "B\n");
    exchange_log(a, b, logs.path());
    REQUIRE(b.read(np("n.md"))->text == "B\n");
    const Texts edges = link_edges(b);
    CAPTURE(edges);
    CHECK(std::ranges::count_if(edges, [](const std::string& edge) {
              return edge.starts_with("n (conflict") && edge.ends_with(">x.md");
          }) == 1);
    // Sonradan oluşan hedef kopyanın metninden de bulunur (link_targets).
    REQUIRE(seed_note(b, "y.md", "y\n"));
    REQUIRE(b.move(np("x.md"), np("z.md")).has_value());
    REQUIRE(b.move(np("y.md"), np("x.md")).has_value());
    CHECK(std::ranges::count_if(link_edges(b), [](const std::string& edge) {
              return edge.starts_with("n (conflict") && edge.ends_with(">x.md");
          }) == 1);
}
