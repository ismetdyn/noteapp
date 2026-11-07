// G4b: düşmanca ya da sadece büyük veride depo durmaz; sınır aşımı raporlanır, kilitlenme ya da kayıp olmaz.
#include "core/config.hpp"
#include "core/file_stat.hpp"
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "support/note_import.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <string>
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

graph::Graph open_raw(const fs::path& dir) {
    fs::create_directories(dir);
    auto opened = graph::Graph::open(dir / "graph.db");
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

graph::NodeId raw_note(graph::WriteTxn& txn, const std::string& name, const std::string& type,
                       std::string body) {
    const graph::NodeId id = graph::new_node_id();
    REQUIRE(txn.put_node(id).has_value());
    REQUIRE(txn.add_label(id, "Note").has_value());
    REQUIRE(txn.set_property(id, "name", graph::Value{name}).has_value());
    REQUIRE(txn.set_property(id, "type", graph::Value{type}).has_value());
    REQUIRE(txn.set_property(id, "body", graph::Value{graph::Bytes{std::move(body)}}).has_value());
    return id;
}

#ifndef _WIN32
unsigned mode_of(const fs::path& path) {
    return static_cast<unsigned>(fs::status(path).permissions()) & 0777U;
}
#endif

}  // namespace

// İşlem başına sınır not sayısına değil işleme bağlıdır: yoksa bağlantılı 1000 notun içe aktarılması kalıcı
// olarak kilitlerdi.
TEST_CASE(
    "importing 1000 notes with 70 links each finishes; folder removal and empty_trash also batch by ops") {
    const TempDir data;
    std::vector<noteapp::test::ImportedNote> notes;
    for (int n = 0; n < 1000; ++n) {
        std::string body;
        for (int link = 1; link <= 70; ++link)
            body += "[[n" + std::to_string((n + link) % 1000) + "]] ";
        notes.push_back(noteapp::test::ImportedNote{np("d/n" + std::to_string(n) + ".md"), body});
    }
    {
        NoteStore importer(data.path());
        const auto report = noteapp::test::import_notes(importer, notes);
        REQUIRE_MESSAGE(report.has_value(),
                        (report ? "" : report.error().message_key + " " + report.error().args.back().second));
        CHECK(report->imported == 1000);
        REQUIRE(noteapp::test::relink_notes(importer)
                    .has_value());  // hedef, kaynaktan sonra içe alınmış olabilir
    }
    {
        auto raw = open_raw(data.path());
        const auto links = raw.query("MATCH (a:Note)-[:LINKS_TO]->(b:Note) RETURN count(*) AS c");
        REQUIRE(links.has_value());
        CHECK(std::get<std::int64_t>(links->rows.at(0).at(0).v) == 70000);
    }
    NoteStore store(data.path());
    SUBCASE("remove_folder force") {
        const auto removed = store.remove_folder("d", true);
        REQUIRE_MESSAGE(removed.has_value(), (removed ? "" : removed.error().args.back().second));
        CHECK(store.list().empty());
    }
    SUBCASE("empty_trash") {
        for (const NoteInfo& info : store.list())
            REQUIRE(store.trash(info.path).has_value());
        const auto emptied = store.empty_trash();
        REQUIRE_MESSAGE(emptied.has_value(), (emptied ? "" : emptied.error().args.back().second));
        CHECK(store.list_trash()->empty());
    }
}

namespace {

void check_deep_folder_chain(int depth) {
    const TempDir dir;
    {
        auto raw = open_raw(dir.path());
        std::optional<graph::NodeId> parent;
        graph::NodeId last;
        for (int done = 0; done < depth;) {
            auto txn = raw.write();
            REQUIRE(txn.has_value());
            for (int i = 0; i < 5000 && done < depth; ++i, ++done) {
                const graph::NodeId id = graph::new_node_id();
                REQUIRE(txn->put_node(id).has_value());
                REQUIRE(txn->add_label(id, "Folder").has_value());
                REQUIRE(txn->set_property(id, "name", graph::Value{std::string("d")}).has_value());
                if (parent)
                    REQUIRE(txn->put_edge(graph::new_edge_id(), id, "CHILD_OF", *parent).has_value());
                parent = id;
                last = id;
            }
            REQUIRE(txn->commit().has_value());
        }
        auto txn = raw.write();
        REQUIRE(txn.has_value());
        const graph::NodeId deep = raw_note(*txn, "derin", "md", "x");
        REQUIRE(txn->put_edge(graph::new_edge_id(), deep, "IN_FOLDER", last).has_value());
        raw_note(*txn, "sig", "md", "y");
        REQUIRE(txn->commit().has_value());
    }
    NoteStore store(dir.path());
    const auto notes = store.list();
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().path.string() == "sig.md");
    const auto lost = store.unreachable();
    REQUIRE(lost.size() == 1);
    CHECK(lost.front().name == "derin.md");
}

}  // namespace

// Özyineleme 60.000 derinlikte yığını taşırırdı: açılışta çöker, graph.db silinse de günlükten geri gelir.
TEST_CASE("a very deep folder chain neither crashes nor hides shallow notes; notes past the path limit are "
          "reported") {
    check_deep_folder_chain(60000);
}

// Kurulumu uzun: elle `--no-skip --test-case='a 200k*'`.
TEST_CASE("a 200k CHILD_OF chain neither crashes nor hides shallow notes" * doctest::skip()) {
    check_deep_folder_chain(200000);
}

// L3: kopya sayacının tavanı (1000) yok. Sayaç ad başına sürer, alınmış ad sonlu olduğundan arama biter.
TEST_CASE("many notes on one name all take linear suffixes; none is hidden") {
    const TempDir dir;
    constexpr int kCount = 3000;
    {
        auto raw = open_raw(dir.path());
        auto txn = raw.write();
        REQUIRE(txn.has_value());
        for (int i = 0; i < kCount; ++i)
            raw_note(*txn, "ayni", "md", std::to_string(i));
        raw_note(*txn, "ayni-5", "md", "önceden var");  // sayaç alınmış adı atlar
        REQUIRE(txn->commit().has_value());
    }
    NoteStore store(dir.path());
    CHECK(store.list().size() == kCount + 1);
    CHECK(store.unreachable().empty());
    CHECK(store.read(np("ayni-" + std::to_string(kCount + 1) + ".md")).has_value());
    CHECK(store.read(np("ayni-5.md"))->text == "önceden var");
    // Yeni not da sayaçla aynı kuralla yer bulur.
    const auto created = store.create(NewNote{"", "ayni", "md", "yeni"});
    REQUIRE(created.has_value());
    CHECK(created->string() == "ayni-" + std::to_string(kCount + 2) + ".md");
}

namespace {

struct Heads {
    TempDir logs;
    TempDir a_dir;
    TempDir b_dir;
    graph::Graph a = open_raw(a_dir.path());
    graph::Graph b = open_raw(b_dir.path());
};

std::vector<fs::path> export_log(graph::Graph& source, const fs::path& dir) {
    REQUIRE(source.export_log(dir).has_value());
    std::vector<fs::path> logs;
    for (const auto& entry : fs::directory_iterator(dir))
        logs.push_back(entry.path());
    return logs;
}

}  // namespace

// Silinmiş not atıldığı yolla listede görünür ama adı gizli: kopyanın adı NotePath'ten geçemezdi, tüm ingest
// düşerdi.
TEST_CASE("a conflict on a note whose name cannot make a path gets a safe copy and ingest goes on") {
    Heads h;
    graph::NodeId note;
    {
        auto txn = h.a.write();
        REQUIRE(txn.has_value());
        note = raw_note(*txn, "gecerli", "md", "ilk");
        REQUIRE(txn->commit().has_value());
    }
    REQUIRE(h.b.ingest_logs(export_log(h.a, h.logs.path())).has_value());
    const auto write_body = [&](graph::Graph& device, const std::string& body) {
        auto txn = device.write();
        REQUIRE(txn.has_value());
        REQUIRE(txn->set_property(note, "body", graph::Value{graph::Bytes{body}}).has_value());
        REQUIRE(txn->commit().has_value());
    };
    write_body(h.a, "a sürümü");
    write_body(h.b, "b sürümü");
    REQUIRE(h.a.export_log(h.logs.path()).has_value());
    REQUIRE(h.b.export_log(h.logs.path()).has_value());
    std::vector<fs::path> logs;
    for (const auto& entry : fs::directory_iterator(h.logs.path()))
        logs.push_back(entry.path());
    REQUIRE(h.a.ingest_logs(logs).has_value());
    {  // motorla ingest edildi, çözülmedi; ad gizli, çöp yolu geçerli
        auto txn = h.a.write();
        REQUIRE(txn.has_value());
        REQUIRE(txn->set_property(note, "name", graph::Value{std::string(".x")}).has_value());
        REQUIRE(txn->set_property(note, "deleted_at", graph::Value{graph::Timestamp{1000}}).has_value());
        REQUIRE(txn->set_property(note, "trashed_from", graph::Value{std::string("ok.md")}).has_value());
        const graph::NodeId other = raw_note(*txn, "diger", "md", "kalir");
        static_cast<void>(other);
        REQUIRE(txn->commit().has_value());
    }
    NoteStore store(h.a_dir.path());
    REQUIRE(store.list_trash()->size() == 1);
    const std::vector<fs::path> none;
    const auto ingested = ingest_note_logs(store, none);
    REQUIRE_MESSAGE(ingested.has_value(), (ingested ? "" : ingested.error().message_key));
    CHECK(ingested->conflict_copies.size() == 1);
    CHECK(ingested->unresolved.empty());
    const auto copy = store.read(ingested->conflict_copies.front());
    REQUIRE(copy.has_value());
    CHECK((copy->text == "a sürümü" || copy->text == "b sürümü"));
}

namespace {

std::string deep_folder(std::size_t depth) {
    std::string path = "d";
    for (std::size_t i = 1; i < depth; ++i)
        path += "/d";
    return path;
}

// Tam `bytes` uzunluğunda geçerli klasör yolu: bileşenler ad sınırının (255) altında.
std::string long_folder(std::size_t bytes) {
    constexpr std::size_t kComponent = 200;
    std::string path;
    while (bytes - path.size() > kComponent + 1)
        path += std::string(kComponent, 'k') + "/";
    return path + std::string(bytes - path.size(), 'k');
}

bool has_reason(const Error& error, std::string_view reason) {
    return std::ranges::any_of(
        error.args, [&](const auto& arg) { return arg.first == "reason" && arg.second == reason; });
}

void check_refused(const std::expected<NotePath, Error>& created) {
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == Error::Code::InvalidPath);
    CHECK(has_reason(created.error(), "invalid_folder"));
}

void check_nothing_invisible(const NoteStore& store) {
    CHECK(store.list().empty());
    CHECK(store.folders().empty());
    CHECK(store.unreachable().empty());
}

}  // namespace

// L3: klasör derinliğinin tavanı (128) yok; yalnız yolun baytı (limits.core.folder_path_bytes) korunur.
TEST_CASE("a note in a folder deeper than 128 levels is saved, moved and renamed like any other") {
    const TempDir dir;
    NoteStore store(dir.path());
    const auto deep = store.create(NewNote{deep_folder(1000), "n", "md", "metin"});
    REQUIRE(deep.has_value());
    CHECK(store.read(*deep)->text == "metin");
    REQUIRE(seed_note(store, "m.md", "taşınan"));
    REQUIRE(store.move(np("m.md"), np(deep_folder(1500) + "/m.md")).has_value());
    REQUIRE(store.rename_folder("d", "x/d").has_value());
    CHECK(store.read(np("x/" + deep_folder(1000) + "/n.md"))->text == "metin");
    CHECK(store.read(np("x/" + deep_folder(1500) + "/m.md"))->text == "taşınan");
    CHECK(store.unreachable().empty());
}

// Görüntü bu klasörleri çözemez: başarı dönen kayıt görünmez, sonraki kayıt NotFound alıp yeni görünmez düğüm
// üretirdi.
TEST_CASE("a note under a folder path over the byte limit is refused and nothing invisible is created") {
    const TempDir dir;
    NoteStore store(dir.path());
    check_refused(store.create(NewNote{long_folder(4097), "n", "md", "metin"}));
    check_nothing_invisible(store);

    // Sınır klasör yolundadır: tam sınırdaki klasörün notu görünür, toplam yol 4096'yı aşsa da.
    const auto at_limit = store.create(NewNote{long_folder(4096), "n", "md", "metin"});
    REQUIRE(at_limit.has_value());
    CHECK(store.read(*at_limit)->text == "metin");
    CHECK(store.unreachable().empty());
}

TEST_CASE("create_folder and move refuse an over-limit folder; the moved note stays where it was") {
    const TempDir dir;
    NoteStore store(dir.path());
    const auto folder = store.create_folder(long_folder(4097));
    REQUIRE_FALSE(folder.has_value());
    CHECK(has_reason(folder.error(), "invalid_folder"));
    check_nothing_invisible(store);

    REQUIRE(seed_note(store, "n.md", "metin"));
    check_refused(store.move(np("n.md"), np(long_folder(4097) + "/n.md")));
    CHECK(store.read(np("n.md"))->text == "metin");
    CHECK(store.folders().empty());
    CHECK(store.unreachable().empty());
}

TEST_CASE("a folder rename that pushes a descendant over the byte limit is refused and changes nothing") {
    const TempDir dir;
    NoteStore store(dir.path());
    const std::string long_note = "t/" + long_folder(4094) + "/m.md";
    REQUIRE(seed_note(store, long_note, "uzun"));
    REQUIRE(store.unreachable().empty());

    const auto renamed = store.rename_folder("t", "tt");
    REQUIRE_FALSE(renamed.has_value());
    CHECK(has_reason(renamed.error(), "invalid_folder"));
    CHECK(store.read(np(long_note))->text == "uzun");
    CHECK(store.unreachable().empty());
    CHECK_FALSE(std::ranges::contains(store.folders(), std::string("tt")));

    // Aynı uzunlukta ad değişikliği sınırı aşmaz.
    REQUIRE(store.rename_folder("t", "u").has_value());
    CHECK(store.read(np("u/" + long_folder(4094) + "/m.md"))->text == "uzun");
}

// L3: klasör yolunun bayt sınırı config'tedir (limits.core.folder_path_bytes); 0 sınırsızdır.
TEST_CASE("limits.core.folder_path_bytes sets the folder path limit and 0 lifts it") {
    const TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    SUBCASE("smaller") {
        const LoadedConfig loaded = parse_config("[limits.core]\nfolder_path_bytes = 100\n");
        REQUIRE(loaded.warnings.empty());
        NoteStore store = NoteStore::for_config(loaded.config);
        check_refused(store.create(NewNote{long_folder(101), "n", "md", "metin"}));
        check_nothing_invisible(store);
        REQUIRE(store.create(NewNote{long_folder(100), "n", "md", "metin"}).has_value());
    }
    SUBCASE("unlimited") {
        const LoadedConfig loaded = parse_config("[limits.core]\nfolder_path_bytes = 0\n");
        REQUIRE(loaded.warnings.empty());
        NoteStore store = NoteStore::for_config(loaded.config);
        const auto created = store.create(NewNote{long_folder(20000), "n", "md", "metin"});
        REQUIRE(created.has_value());
        CHECK(store.read(*created)->text == "metin");
        REQUIRE(store.rename_folder(created->string().substr(0, 200), "yeni").has_value());
        CHECK(store.unreachable().empty());
    }
}

// Yapılandırmasız depo (geçiş, test) defaults.toml ile aynı sınırı kullanır: tek olgu.
TEST_CASE("an unconfigured store uses the folder path limit from defaults.toml") {
    CHECK(parse_config("").config.limits.core.folder_path_bytes.value == kDefaultFolderPathBytes);
    CHECK(parse_config("").config.limits.graph.relation_name_bytes.value == kDefaultRelationNameBytes);
}

namespace {

// Üç cihazın eşzamanlı gövdeleri aynı nottadır; hepsi birleşince iki kaybeden olur.
struct ThreeHeads {
    TempDir logs;
    TempDir dirs[3];
    graph::Graph devices[3] = {open_raw(dirs[0].path()), open_raw(dirs[1].path()), open_raw(dirs[2].path())};
    std::vector<fs::path> all_logs;

    explicit ThreeHeads(const std::vector<std::string>& bodies) {
        graph::NodeId note;
        {
            auto txn = devices[0].write();
            REQUIRE(txn.has_value());
            note = raw_note(*txn, "not", "md", "ilk");
            REQUIRE(txn->commit().has_value());
        }
        REQUIRE(devices[0].export_log(logs.path()).has_value());
        std::vector<fs::path> first;
        for (const auto& entry : fs::directory_iterator(logs.path()))
            first.push_back(entry.path());
        for (std::size_t i = 1; i < bodies.size(); ++i)
            REQUIRE(devices[i].ingest_logs(first).has_value());
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            auto txn = devices[i].write();
            REQUIRE(txn.has_value());
            REQUIRE(txn->set_property(note, "body", graph::Value{graph::Bytes{bodies[i]}}).has_value());
            REQUIRE(txn->commit().has_value());
            REQUIRE(devices[i].export_log(logs.path()).has_value());
        }
        for (const auto& entry : fs::directory_iterator(logs.path()))
            all_logs.push_back(entry.path());
    }
};

}  // namespace

TEST_CASE("two large losers of one conflict each get their own transaction: 40 MiB bodies, nothing lost") {
    const std::vector<std::string> bodies{std::string(40U << 20U, 'a'), std::string(40U << 20U, 'b'),
                                          std::string(40U << 20U, 'c')};
    ThreeHeads h(bodies);
    const TempDir target;
    NoteStore store(target.path());
    const auto ingested = ingest_note_logs(store, h.all_logs);
    REQUIRE_MESSAGE(ingested.has_value(), (ingested ? "" : ingested.error().message_key));
    CHECK(ingested->conflict_copies.size() == 2);
    CHECK(ingested->unresolved.empty());
    std::vector<std::size_t> sizes;
    for (const NoteInfo& info : store.list())
        sizes.push_back(info.size);
    CHECK(std::ranges::count(sizes, std::size_t{40U << 20U}) == 3);
}

// L4b: çerçeve kopyanın sınırı değildir. Eskiden gövdesi çerçeveye ancak sığan çakışmanın kopyası sığmıyor,
// head'ler yerinde kalıyordu; artık kopya kurulur, ikinci ingest yeni iş üretmez.
TEST_CASE("a conflict whose copy is larger than one frame is resolved with a copy") {
    const std::size_t near_limit = (std::size_t{64} << 20U) - 300;
    const std::vector<std::string> bodies{std::string(near_limit, 'a'), std::string(near_limit, 'b')};
    ThreeHeads h(bodies);
    const TempDir target;
    NoteStore store(target.path());
    const auto ingested = ingest_note_logs(store, h.all_logs);
    REQUIRE_MESSAGE(ingested.has_value(), (ingested ? "" : ingested.error().message_key));
    CHECK(ingested->unresolved.empty());
    CHECK(ingested->conflict_copies.size() == 1);
    CHECK(store.list().size() == 2);
    const auto again = ingest_note_logs(store, h.all_logs);
    REQUIRE(again.has_value());
    CHECK(again->unresolved.empty());
    CHECK(again->conflict_copies.empty());
}

#ifndef _WIN32
TEST_CASE(
    "the database, its WAL and SHM and the data directory are private, also when they already existed") {
    const TempDir root;
    const fs::path dir = root.path() / "veri";
    {
        NoteStore store(dir);
        REQUIRE(seed_note(store, "n.md", "gizli"));
        CHECK(mode_of(dir) == 0700U);
        CHECK(mode_of(dir / "graph.db") == 0600U);
        CHECK(mode_of(dir / "graph.db-wal") == 0600U);
        CHECK(mode_of(dir / "graph.db-shm") == 0600U);
    }
    // Eski sürümün umask'la bıraktığı dosyalar açılışta daralır.
    for (const char* name : {"graph.db", "graph.db-wal", "graph.db-shm"})
        if (fs::exists(dir / name))
            fs::permissions(dir / name, static_cast<fs::perms>(0644), fs::perm_options::replace);
    fs::permissions(dir, static_cast<fs::perms>(0755), fs::perm_options::replace);
    NoteStore again(dir);
    REQUIRE(seed_note(again, "m.md", "x"));
    CHECK(mode_of(dir) == 0700U);
    for (const char* name : {"graph.db", "graph.db-wal", "graph.db-shm"})
        if (fs::exists(dir / name))
            CHECK_MESSAGE(mode_of(dir / name) == 0600U, name);
}
#endif
