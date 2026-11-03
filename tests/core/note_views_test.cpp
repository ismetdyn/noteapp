// T81: kaydedilmiş Cypher görünümleri (:View {name, query, created_at, updated_at}). Görünüm salt-okunurdur:
// yazan sorgu kayıtta ve çalıştırmada reddedilir; senkronla gelen kötü niyetli görünüm hiçbir şey
// değiştirmez. İki cihazın aynı görünümü aynı düğümdür, eşzamanlı düzenleme mevcut kuralla (en büyük head)
// yakınsar.
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_sync.hpp"
#include "core/note_views.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <doctest/doctest.h>
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

Texts view_names(const NoteStore& store) {
    const auto views = list_views(store);
    REQUIRE(views.has_value());
    Texts names;
    for (const SavedView& view : *views)
        names.push_back(view.name);
    return names;
}

Texts paths_of(const std::vector<NotePath>& notes) {
    Texts out;
    for (const NotePath& note : notes)
        out.push_back(note.string());
    return out;
}

Texts notes_of(const NoteStore& store, std::string_view view) {
    const auto notes = view_notes(store, view);
    INFO((notes ? std::string() : notes.error().message_key));
    REQUIRE(notes.has_value());
    return paths_of(*notes);
}

// Ayrı bağlantı: başka bir cihazdan senkronla gelmiş gibi doğrudan grafa yazar.
void write_raw(const fs::path& directory, const std::string& cypher) {
    auto raw = graph::Graph::open(directory / "graph.db");
    REQUIRE(raw.has_value());
    const auto written = raw->query(cypher, {}, graph::QueryMode::ReadWrite);
    REQUIRE(written.has_value());
}

std::int64_t count_raw(const fs::path& directory, const std::string& cypher) {
    auto raw = graph::Graph::open(directory / "graph.db");
    REQUIRE(raw.has_value());
    const auto result = raw->query(cypher);
    REQUIRE(result.has_value());
    REQUIRE(result->rows.size() == 1);
    return std::get<std::int64_t>(result->rows.front().front().v);
}

void exchange_log(NoteStore& from, NoteStore& to, const fs::path& logs) {
    REQUIRE(export_note_log(from, logs).has_value());
    const auto name = own_log_name(from);
    REQUIRE(name.has_value());
    const std::vector<fs::path> files{logs / *name};
    REQUIRE(ingest_note_logs(to, files).has_value());
}

constexpr std::string_view kTaggedWork = "MATCH (n:Note)-[:TAGGED]->(:Tag {name: 'work'}) RETURN n";

}  // namespace

TEST_CASE("views: save, list, find, overwrite with the flag, remove") {
    const TempDir dir;
    NoteStore store(dir.path());
    const auto first = std::chrono::system_clock::time_point{std::chrono::seconds(1000)};
    const auto later = first + std::chrono::seconds(60);
    REQUIRE(save_view(store, "Work", kTaggedWork, false, first).has_value());
    REQUIRE(save_view(store, "all", "MATCH (n:Note) RETURN n", false, first).has_value());
    CHECK(view_names(store) == Texts{"all", "Work"});  // katlanmış ada göre

    auto found = find_view(store, "work");  // eşleşme katlamalı
    REQUIRE(found.has_value());
    CHECK(found->name == "Work");
    CHECK(found->query == kTaggedWork);
    CHECK(found->created_at == first);
    CHECK(found->updated_at == first);

    const auto refused = save_view(store, "WORK", "MATCH (n) RETURN n", false, later);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Error::Code::AlreadyExists);
    CHECK(find_view(store, "work")->query == kTaggedWork);

    REQUIRE(save_view(store, "work", "MATCH (n:Note) RETURN n.name", true, later).has_value());
    found = find_view(store, "Work");
    REQUIRE(found.has_value());
    CHECK(found->query == "MATCH (n:Note) RETURN n.name");
    CHECK(found->created_at == first);
    CHECK(found->updated_at == later);
    CHECK(view_names(store).size() == 2);

    REQUIRE(remove_view(store, "WORK").has_value());
    CHECK(view_names(store) == Texts{"all"});
    const auto gone = find_view(store, "work");
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == Error::Code::NotFound);
    CHECK(remove_view(store, "work").error().code == Error::Code::NotFound);
}

TEST_CASE("views: an empty store has no views; unknown names are NotFound") {
    const TempDir dir;
    const NoteStore store(dir.path());
    CHECK(view_names(store).empty());
    CHECK(find_view(store, "x").error().code == Error::Code::NotFound);
    CHECK(run_view(store, "x").error().code == Error::Code::NotFound);
    CHECK(view_notes(store, "x").error().code == Error::Code::NotFound);
}

TEST_CASE("views: names follow the folder name rules and take no slash") {
    const TempDir dir;
    NoteStore store(dir.path());
    for (const std::string_view bad : {"", "a/b", ".hidden", "..", "tab\there", "trailing "}) {
        CAPTURE(std::string(bad));
        const auto saved = save_view(store, bad, "MATCH (n) RETURN n", false);
        REQUIRE_FALSE(saved.has_value());
        CHECK(saved.error().code == Error::Code::InvalidPath);
    }
    CHECK(save_view(store, "iş notları", "MATCH (n) RETURN n", false).has_value());
}

TEST_CASE("views: a query with a write clause, a parameter or bad syntax is rejected at save") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    const std::uint64_t before = store.data_version();
    for (const std::string_view writes :
         {"CREATE (n:X)", "MATCH (n:Note) SET n.x = 1", "MATCH (n) DETACH DELETE n", "MERGE (n:X {k: 1})",
          "MATCH (n:Note) REMOVE n.name", "MATCH (n) WITH n CREATE (m) RETURN n, m"}) {
        CAPTURE(std::string(writes));
        const auto saved = save_view(store, "bad", writes, false);
        REQUIRE_FALSE(saved.has_value());
        CHECK(saved.error().code == Error::Code::InvalidQuery);
        CHECK(saved.error().message_key == "error.view_writes");
    }
    const auto param = save_view(store, "bad", "MATCH (n) WHERE n.name = $x RETURN n", false);
    REQUIRE_FALSE(param.has_value());
    CHECK(param.error().code == Error::Code::InvalidQuery);
    CHECK(param.error().message_key == "error.view_param");
    const auto syntax = save_view(store, "bad", "MATCH (n RETURN n", false);
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().code == Error::Code::InvalidQuery);
    CHECK(view_names(store).empty());
    CHECK(store.data_version() == before);
    CHECK(store.list().size() == 1);
}

TEST_CASE("views: a synced view with a write clause is rejected at run and mutates nothing") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "#work a\n"));
    REQUIRE(seed_note(store, "b.md", "b\n"));
    // Kayıt denetimini atlayan görünüm: başka cihaz ya da elle yazılmış Cypher.
    write_raw(dir.path(), "CREATE (:View {name: 'evil', query: 'MATCH (n:Note) DETACH DELETE n'})");
    write_raw(dir.path(),
              "CREATE (:View {name: 'sneaky', query: 'MATCH (n:Note) SET n.name = \\'x\\' RETURN n'})");
    const std::int64_t nodes = count_raw(dir.path(), "MATCH (n) RETURN count(n)");
    CHECK(view_names(store) == Texts{"evil", "sneaky"});

    for (const std::string_view name : {"evil", "sneaky"}) {
        CAPTURE(std::string(name));
        const auto ran = run_view(store, name);
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().code == Error::Code::InvalidQuery);
        CHECK(ran.error().message_key == "error.view_writes");
        const auto filtered = view_notes(store, name);
        REQUIRE_FALSE(filtered.has_value());
        CHECK(filtered.error().message_key == "error.view_writes");
    }
    CHECK(count_raw(dir.path(), "MATCH (n) RETURN count(n)") == nodes);
    CHECK(count_raw(dir.path(), "MATCH (n:Note) WHERE n.name = 'x' RETURN count(n)") == 0);
    CHECK(store.list().size() == 2);
}

TEST_CASE("views: run returns the query result; view_notes keeps live notes from nodes and path cells") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "work/a.md", "#work a\n"));
    REQUIRE(seed_note(store, "b.md", "#work b\n"));
    REQUIRE(seed_note(store, "c.md", "c\n"));
    REQUIRE(save_view(store, "work", kTaggedWork, false).has_value());

    const auto ran = run_view(store, "work");
    REQUIRE(ran.has_value());
    CHECK(ran->columns == Texts{"n"});
    CHECK(ran->rows.size() == 2);
    CHECK(notes_of(store, "work") == Texts{"b.md", "work/a.md"});

    // Metin hücresi canlı notun yoluysa o nottur: Note.name yalnız ad olduğundan kökteki uzantısız ad tutmaz.
    REQUIRE(
        save_view(store, "names", "MATCH (n:Note) RETURN 'c.md' AS path, n.name LIMIT 1", false).has_value());
    CHECK(notes_of(store, "names") == Texts{"c.md"});

    REQUIRE(store.trash(np("b.md")).has_value());
    CHECK(notes_of(store, "work") == Texts{"work/a.md"});  // görüntü değişti: sonuç tazelenir

    REQUIRE(save_view(store, "none", "MATCH (n:Nothing) RETURN n", false).has_value());
    CHECK(notes_of(store, "none").empty());

    REQUIRE(save_view(store, "count", "MATCH (n:Note) RETURN count(n) AS total", false).has_value());
    const auto counted = view_notes(store, "count");
    REQUIRE_FALSE(counted.has_value());
    CHECK(counted.error().code == Error::Code::InvalidQuery);
    CHECK(counted.error().message_key == "error.view_no_notes");
    CHECK(run_view(store, "count").has_value());  // run tabloyu verir; yalnız süzgeç not ister
}

TEST_CASE("views: a view saved on one device reaches the other; concurrent edits converge") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "#work x\n"));
    REQUIRE(save_view(a, "Work", kTaggedWork, false).has_value());
    exchange_log(a, b, logs.path());
    CHECK(view_names(b) == Texts{"Work"});
    CHECK(notes_of(b, "work") == Texts{"x.md"});

    REQUIRE(save_view(a, "work", "MATCH (n:Note) RETURN n.name", true).has_value());
    REQUIRE(save_view(b, "WORK", "MATCH (n:Note) RETURN n", true).has_value());
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());

    const auto on_a = find_view(a, "work");
    const auto on_b = find_view(b, "work");
    REQUIRE(on_a.has_value());
    REQUIRE(on_b.has_value());
    CHECK(on_a->query == on_b->query);
    CHECK(on_a->name == on_b->name);
    CHECK((on_a->query == "MATCH (n:Note) RETURN n.name" || on_a->query == "MATCH (n:Note) RETURN n"));
    CHECK(view_names(a).size() == 1);
    CHECK(view_names(b).size() == 1);

    // Aynı adı iki cihaz birbirini görmeden kursa da tek görünüm olur (kimlik anahtardan türer).
    REQUIRE(save_view(a, "fresh", "MATCH (n) RETURN n", false).has_value());
    REQUIRE(save_view(b, "Fresh", "MATCH (n:Note) RETURN n", false).has_value());
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());
    CHECK(view_names(a) == view_names(b));
    CHECK(view_names(a).size() == 2);
    CHECK(find_view(a, "fresh")->query == find_view(b, "fresh")->query);
}

// Güvenlik incelemesi: senkronla gelen bozuk görünüm (süre sınırı, not döndürmeyen sonuç) her tuşta UI'yi
// NoteStore kilidi altında saniyelerce tutmasın. Hata da görüntü başına saklanır; görüntü değişince yeniden
// çalışır.
TEST_CASE("views: a failing view runs the engine once per snapshot, like a successful one") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "a.md", "a\n"));
    REQUIRE(save_view(store, "count", "MATCH (n:Note) RETURN count(n)", false).has_value());
    REQUIRE(save_view(store, "all", "MATCH (n:Note) RETURN n", false).has_value());
    int runs = 0;
    store.impl().before_view_run = [&runs] { ++runs; };

    for (int keystroke = 0; keystroke < 3; ++keystroke) {
        const auto failed = view_notes(store, "count");
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message_key == "error.view_no_notes");
    }
    CHECK(runs == 1);
    CHECK(view_notes(store, "all").has_value());
    CHECK(view_notes(store, "ALL").has_value());
    CHECK(runs == 2);

    REQUIRE(seed_note(store, "b.md", "b\n"));  // yeni görüntü: ikisi de yeniden çalışır
    CHECK_FALSE(view_notes(store, "count").has_value());
    CHECK(notes_of(store, "all") == Texts{"a.md", "b.md"});
    CHECK(runs == 4);
}
