// T92b: kullanıcının kendi işlemi türetilmiş iş ya da çerçeve başına işlem sınırı yüzünden reddedilmez.
// Sınır test dikişiyle küçültülür (txn_ops_limit); bütçeyi aşan iş sonraki işlemlere bölünür, her işlem
// atomiktir, süreç arada ölürse sonraki açılış tamamlar.
#include "core/config.hpp"
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_sync.hpp"
#include "core/note_views.hpp"
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

using Texts = std::vector<std::string>;

constexpr std::size_t kSmallLimit = 16;  // bütçe 8: bir not oluşturma bütçeyi doldurur
constexpr int kSources = 30;

NotePath np(const std::string& text) {
    return *NotePath::parse(text);
}

Texts link_edges(const NoteStore& store) {
    const auto edges = store.edges();
    REQUIRE(edges.has_value());
    Texts out;
    for (const NoteEdge& edge : *edges)
        if (edge.kind == EdgeKind::LinksTo)
            out.push_back(edge.from.string() + ">" + edge.to.string());
    return out;
}

std::size_t edges_into(const NoteStore& store, const std::string& target) {
    std::size_t count = 0;
    for (const std::string& edge : link_edges(store))
        count += edge.ends_with(">" + target) ? 1 : 0;
    return count;
}

// Ayrı ham bağlantı: NoteStore açmak sonraki açılışın onarımını başlatırdı.
std::int64_t raw_links_into_hub(const fs::path& directory) {
    auto raw = graph::Graph::open(directory / "graph.db");
    REQUIRE(raw.has_value());
    auto result = raw->query("MATCH (s:Note)-[:LINKS_TO]->(h:Note {name: 'hub'}) RETURN count(s)");
    REQUIRE(result.has_value());
    const auto* count = std::get_if<std::int64_t>(&result->rows.at(0).at(0).v);
    REQUIRE(count != nullptr);
    return *count;
}

void seed_sources(NoteStore& store, const std::string& link, const std::string& folder = "src") {
    for (int i = 0; i < kSources; ++i)
        REQUIRE(seed_note(store, folder + "/s" + std::to_string(i) + ".md", "[[" + link + "]]\n"));
}

}  // namespace

TEST_CASE("creating, moving, restoring or renaming onto a name linked beyond one transaction succeeds") {
    const TempDir dir;
    NoteStore store(dir.path());
    SUBCASE("create") {
        seed_sources(store, "hub");
        store.impl().txn_ops_limit = kSmallLimit;
        REQUIRE(seed_note(store, "hub.md", "h\n"));
        CHECK(edges_into(store, "hub.md") == kSources);
    }
    SUBCASE("move") {
        REQUIRE(seed_note(store, "draft.md", "d\n"));
        seed_sources(store, "hub");
        store.impl().txn_ops_limit = kSmallLimit;
        REQUIRE(store.move(np("draft.md"), np("hub.md")).has_value());
        CHECK(edges_into(store, "hub.md") == kSources);
    }
    SUBCASE("restore") {
        REQUIRE(seed_note(store, "hub.md", "h\n"));
        REQUIRE(store.trash(np("hub.md")).has_value());
        seed_sources(store, "hub");
        store.impl().txn_ops_limit = kSmallLimit;
        REQUIRE(store.restore(np("hub.md")).has_value());
        CHECK(edges_into(store, "hub.md") == kSources);
    }
    SUBCASE("folder rename") {
        REQUIRE(seed_note(store, "old/hub.md", "h\n"));
        seed_sources(store, "new/hub");
        store.impl().txn_ops_limit = kSmallLimit;
        REQUIRE(store.rename_folder("old", "new").has_value());
        CHECK(edges_into(store, "new/hub.md") == kSources);
    }
}

TEST_CASE("derived edges left after a crash between follow-up transactions are completed on the next open") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        seed_sources(store, "hub");
        store.impl().txn_ops_limit = kSmallLimit;
        store.impl().follow_up_allowed = [] { return false; };
        REQUIRE(seed_note(store, "hub.md", "h\n"));  // kullanıcının işlemi commit edildi
        CHECK(edges_into(store, "hub.md") < kSources);
    }
    const NoteStore reopened(dir.path());
    CHECK(edges_into(reopened, "hub.md") == kSources);
    CHECK(fs::exists(dir.path() / "links-indexed"));
}

TEST_CASE("deleting a note with more links than one transaction holds succeeds") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "f/hub.md", "#etiket [[src/s0]]\n"));
    seed_sources(store, "hub");
    REQUIRE(store.add_relation(np("f/hub.md"), np("src/s1.md")).has_value());
    REQUIRE(edges_into(store, "f/hub.md") == kSources);
    store.impl().txn_ops_limit = kSmallLimit;
    SUBCASE("remove") {
        REQUIRE(store.remove(np("f/hub.md")).has_value());
    }
    SUBCASE("purge") {
        REQUIRE(store.trash(np("f/hub.md")).has_value());
        REQUIRE(store.purge(np("f/hub.md")).has_value());
    }
    SUBCASE("empty trash") {
        REQUIRE(store.trash(np("f/hub.md")).has_value());
        REQUIRE(store.empty_trash().has_value());
    }
    SUBCASE("remove folder") {
        REQUIRE(store.remove_folder("f", true).has_value());
        CHECK(store.folders() == Texts{"src"});
    }
    CHECK(store.list_trash()->empty());
    CHECK_FALSE(store.read(np("f/hub.md")).has_value());
    CHECK(link_edges(store).empty());
    CHECK(store.links(np("src/s3.md"))->missing == Texts{"hub"});
    CHECK(store.links(np("src/s1.md"))->related.empty());
    CHECK(store.tags().empty());
}

TEST_CASE("a crash while deleting a heavily linked note leaves it whole after the next open") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "hub.md", "h\n"));
        seed_sources(store, "hub");
        store.impl().txn_ops_limit = kSmallLimit;
        store.impl().follow_up_allowed = [] { return false; };
        CHECK_FALSE(store.remove(np("hub.md")).has_value());
        CHECK(raw_links_into_hub(dir.path()) < kSources);  // ilk işlemler commit edilmiş
    }
    const NoteStore reopened(dir.path());
    CHECK(reopened.read(np("hub.md")).has_value());
    CHECK(edges_into(reopened, "hub.md") == kSources);
}

TEST_CASE("importing notes and folders beyond one transaction succeeds") {
    const TempDir dir;
    NoteStore store(dir.path());
    store.impl().txn_ops_limit = 64;  // not başına ~29 işlem: bir not sığar, beşi sığmaz
    std::vector<ImportedNote> notes;
    for (int i = 0; i < 5; ++i) {
        const std::string n = std::to_string(i);
        notes.push_back(
            ImportedNote{np("n" + n + ".md"), "#a" + n + " #b" + n + " #c" + n + " #d" + n + "\n"});
    }
    const auto outcome = store.import_notes(notes);
    REQUIRE(outcome.has_value());
    CHECK(outcome->imported == 5);
    CHECK(store.list().size() == 5);
    CHECK(store.tags().size() == 20);

    std::vector<std::string> folders;
    for (int i = 0; i < 40; ++i)
        folders.push_back("f" + std::to_string(100 + i));
    REQUIRE(store.import_folders(folders).has_value());
    CHECK(store.folders().size() == 40);
}

TEST_CASE("copying a folder with more subfolders than one transaction holds succeeds") {
    const TempDir dir;
    NoteStore store(dir.path());
    for (int i = 0; i < 20; ++i)
        REQUIRE(store.create_folder("src/s" + std::to_string(10 + i)).has_value());
    REQUIRE(seed_note(store, "src/s10/n.md", "n\n"));
    store.impl().txn_ops_limit = kSmallLimit;
    REQUIRE(store.copy_folder("src", "dst").has_value());
    std::size_t copied = 0;
    for (const std::string& folder : store.folders())
        copied += folder.starts_with("dst/") ? 1 : 0;
    CHECK(copied == 20);
    CHECK(store.read(np("dst/s10/n.md")).has_value());
}

TEST_CASE("removing a view with more edges than one transaction holds succeeds") {
    const TempDir dir;
    NoteStore store(dir.path());
    for (int i = 0; i < kSources; ++i)
        REQUIRE(seed_note(store, "n" + std::to_string(i) + ".md", "n\n"));
    REQUIRE(save_view(store, "v", "MATCH (n:Note) RETURN n", false).has_value());
    {
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(
            raw->query("MATCH (v:View), (n:Note) CREATE (n)-[:SEES]->(v)", {}, graph::QueryMode::ReadWrite)
                .has_value());
    }
    NoteStore fresh(dir.path());
    fresh.impl().txn_ops_limit = kSmallLimit;
    REQUIRE(remove_view(fresh, "v").has_value());
    CHECK(find_view(fresh, "v").error().code == Error::Code::NotFound);
}

// Bütçe bir ayardır, sınır değil: [tuning] graph_batch_ops depoya gelir; küçük bütçe işi yalnız daha çok
// işleme böler.
TEST_CASE("the batch budget comes from [tuning] graph_batch_ops and only changes how work is split") {
    const TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    const LoadedConfig loaded = parse_config("[tuning]\ngraph_batch_ops = 4\n");
    REQUIRE(loaded.warnings.empty());
    NoteStore store = NoteStore::for_config(loaded.config);
    CHECK(store.impl().batch_ops == 4);
    seed_sources(store, "hub");
    REQUIRE(seed_note(store, "hub.md", "h\n"));
    CHECK(edges_into(store, "hub.md") == kSources);
}

// İnceleme bulgusu (T92c): kenar sayısı bütçenin hemen altında ya da üstündeyken yarım silme klasör kenarını
// da siliyordu; not köke düşüyor, ikinci geçiş onu bulamıyordu. Sınırlar: E = B, E = B-1 ve bir etiket,
// E = B+1 (B = bütçe = 8).
TEST_CASE("a split delete keeps the note in its folder until the last pass, at every budget boundary") {
    struct Case {
        int links;
        bool tag;
    };
    for (const Case c : {Case{7, false}, Case{5, true}, Case{8, false}}) {
        for (const std::string op : {"remove", "purge", "remove_folder"}) {
            CAPTURE(c.links);
            CAPTURE(c.tag);
            CAPTURE(op);
            const TempDir dir;
            NoteStore store(dir.path());
            REQUIRE(seed_note(store, "f/n.md", c.tag ? "#etiket\n" : "n\n"));
            for (int i = 0; i < c.links; ++i)
                REQUIRE(seed_note(store, "src/s" + std::to_string(i) + ".md", "[[n]]\n"));
            if (op == "purge")
                REQUIRE(store.trash(np("f/n.md")).has_value());
            store.impl().txn_ops_limit = kSmallLimit;
            if (op == "remove")
                CHECK(store.remove(np("f/n.md")).has_value());
            else if (op == "purge")
                CHECK(store.purge(np("f/n.md")).has_value());
            else
                CHECK(store.remove_folder("f", true).has_value());
            CHECK_FALSE(store.read(np("f/n.md")).has_value());
            CHECK_FALSE(store.read(np("n.md")).has_value());
            CHECK(store.list_trash()->empty());
            CHECK(store.list().size() == static_cast<std::size_t>(c.links));
            CHECK(link_edges(store).empty());
        }
    }
}

// Sonraki geçişler silinecek notu yoldan değil ilk geçişte bulunan kimlikten sürdürür: arada başka süreç
// notu taşıyıp yerine başka not koyarsa yanlış not silinmez.
TEST_CASE("a split delete continues on the note it started with, not on whatever takes its path") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "hub.md", "h\n"));
    REQUIRE(seed_note(store, "other.md", "başka\n"));
    seed_sources(store, "hub");
    store.impl().txn_ops_limit = kSmallLimit;
    bool swapped = false;
    store.impl().follow_up_allowed = [&] {
        if (!swapped) {
            swapped = true;
            NoteStore elsewhere(dir.path());  // başka süreç: hub'ı taşır, yolunu başka nota verir
            REQUIRE(elsewhere.move(np("hub.md"), np("moved.md")).has_value());
            REQUIRE(elsewhere.move(np("other.md"), np("hub.md")).has_value());
        }
        return true;
    };
    REQUIRE(store.remove(np("hub.md")).has_value());
    CHECK(swapped);
    CHECK_FALSE(store.read(np("moved.md")).has_value());
    REQUIRE(store.read(np("hub.md")).has_value());
    CHECK(store.read(np("hub.md"))->text == "başka\n");
}

// İnceleme bulgusu (T92d): çöp IN_FOLDER kenarını tutar; klasörün yalnız klasör kenarları kaldığında tam
// silme sınırsızdı, çöpü bütçeden büyük klasörün silinmesi her denemede düşüyordu. Çöpteki not yine atıldığı
// yolu bilir, geri alma klasörü kurar (SPEC-graph "Geçiş", çöp).
TEST_CASE("removing a folder whose trashed notes exceed one transaction succeeds; restore rebuilds it") {
    const TempDir dir;
    NoteStore store(dir.path());
    constexpr int kTrashed = 20;
    for (int i = 0; i < kTrashed; ++i) {
        const std::string path = "f/n" + std::to_string(i) + ".md";
        REQUIRE(seed_note(store, path, "n\n"));
        REQUIRE(store.trash(np(path)).has_value());
    }
    store.impl().txn_ops_limit = kSmallLimit;
    REQUIRE(store.remove_folder("f", true).has_value());
    CHECK(store.folders().empty());
    const auto trash = store.list_trash();
    REQUIRE(trash.has_value());
    CHECK(trash->size() == static_cast<std::size_t>(kTrashed));
    const auto restored = store.restore(np("f/n3.md"));
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "f/n3.md");
    CHECK(store.folders() == Texts{"f"});
    CHECK(store.read(np("f/n3.md"))->text == "n\n");
}

// İnceleme bulgusu (T92e): iç klasörün yarım geçişi bütçeyi doldurmazsa döngü aynı işlemde üst klasörü bütün
// siliyor, iç klasörün CHILD_OF'u kopuyor, iç klasör kökte başıboş kalıyordu. 7 çöp notu: yarım geçiş 7
// işlem, bütçe 8.
TEST_CASE("removing nested folders leaves no stray subfolder when an inner folder needs a split delete") {
    const TempDir dir;
    NoteStore store(dir.path());
    for (int i = 0; i < 7; ++i) {
        const std::string path = "outer/inner/n" + std::to_string(i) + ".md";
        REQUIRE(seed_note(store, path, "n\n"));
        REQUIRE(store.trash(np(path)).has_value());
    }
    store.impl().txn_ops_limit = kSmallLimit;
    REQUIRE(store.remove_folder("outer", true).has_value());
    CHECK(store.folders().empty());
    const auto restored = store.restore(np("outer/inner/n2.md"));
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "outer/inner/n2.md");
    CHECK(store.folders() == Texts{"outer", "outer/inner"});
}
