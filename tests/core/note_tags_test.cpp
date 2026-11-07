// T80: `#etiket`ler not metninden türer; (:Tag {name}) ve (:Note)-[:TAGGED]->(:Tag) gövdeyle aynı işlemde
// güncellenir, sahipsiz Tag silinir, senkronla iki cihaz aynı düğüme varır (SPEC-graph "Etiketler").
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "core/note_sync.hpp"
#include "graph/graph.hpp"
#include "support/note_import.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "sync/git_runtime.hpp"
#include "sync/text_merge.hpp"

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

// Ayrı bağlantı: başka bir süreç gibi okur.
Texts query_texts(const fs::path& directory, const std::string& cypher) {
    auto raw = graph::Graph::open(directory / "graph.db");
    REQUIRE(raw.has_value());
    auto result = raw->query(cypher);
    REQUIRE(result.has_value());
    Texts out;
    for (const auto& row : result->rows) {
        const auto* text = std::get_if<std::string>(&row.at(0).v);
        REQUIRE(text != nullptr);
        out.push_back(*text);
    }
    return out;
}

Texts tag_names(const fs::path& directory) {
    return query_texts(directory, "MATCH (t:Tag) RETURN t.name ORDER BY t.name");
}

Texts tagged(const fs::path& directory, const std::string& note) {
    return query_texts(directory, "MATCH (n:Note {name: '" + note +
                                      "'})-[:TAGGED]->(t:Tag) RETURN t.name ORDER BY t.name");
}

void write_body(NoteStore& store, const std::string& path, const std::string& body) {
    const auto current = store.read(np(path));
    REQUIRE(current.has_value());
    REQUIRE(store.write(np(path), body, current->revision).has_value());
}

void exchange_log(NoteStore& from, NoteStore& to, const fs::path& logs) {
    REQUIRE(export_note_log(from, logs).has_value());
    const auto name = own_log_name(from);
    REQUIRE(name.has_value());
    const std::vector<fs::path> files{logs / *name};
    REQUIRE(ingest_note_logs(to, files,
                             [](std::string_view base, std::string_view ours, std::string_view theirs) {
                                 return noteapp::sync::merge_text(base, ours, theirs, SIZE_MAX);
                             })
                .has_value());
}

}  // namespace

TEST_CASE("note_tags reads hashtags with Turkish letters, nesting and hyphens, first spelling wins") {
    CHECK(note_tags("a.md", "#Proje ve #çalışma, #proje/alt #a-b_c #PROJE\n") ==
          Texts{"a-b_c", "Proje", "proje/alt", "çalışma"});  // anahtarın bayt sırası
    CHECK(note_tags("a.md", "#Çalışma #çalışma") == Texts{"Çalışma"});
    CHECK(note_tags("a.md", "(#parantez) son #nokta. #bölü/") == Texts{"bölü", "nokta", "parantez"});
    CHECK(tag_key("Çalışma/ALT") == "çalışma/alt");
}

TEST_CASE("note_tags skips headings, code, URLs, wiki link anchors and numbers") {
    const std::string body = "# Başlık\n"
                             "## İkinci #gerçek\n"
                             "```\n#kod1\n```\n"
                             "~~~ python\n#kod2\n~~~\n"
                             "satır `#kod3` ve ``#kod4 ` #kod5`` sonra #dış\n"
                             "https://ornek.com/sayfa#bolum [[not#bölüm]] [[not #köşe]] a#b\n"
                             "konu #42 ve \\#kaçış &#351;\n";
    CHECK(note_tags("a.md", body) == Texts{"dış", "gerçek"});
}

// İnceleme bulgusu: kapanmayan uzun ` dizisinden sonra arama kapanışı atlamamalı.
TEST_CASE("note_tags keeps a code span closed after a longer backtick run inside it") {
    CHECK(note_tags("a.md", "`` #t ```y`` z").empty());
    CHECK(note_tags("a.md", "`` #t ```y`` #dış") == Texts{"dış"});
    CHECK(note_tags("a.md", "` #t ``x` #dış") == Texts{"dış"});
}

TEST_CASE("note_tags skips Markdown anchor links and fences indented under list items") {
    CHECK(note_tags("a.md", "[bölüm](#section) ve (#parantez)") == Texts{"parantez"});
    CHECK(note_tags("a.md", "- madde\n    ```c\n    #include <x.h>\n    ```\n#sonra") == Texts{"sonra"});
    CHECK(note_tags("a.md", "1. adım\n\t~~~\n\t#kod\n\t~~~\n").empty());
}

TEST_CASE("note_tags reads prose notes only: code types and binary notes have no tags") {
    CHECK(note_tags("a.txt", "#metin") == Texts{"metin"});
    CHECK(note_tags("a", "#uzantısız") == Texts{"uzantısız"});
    CHECK(note_tags("a.markdown", "#md") == Texts{"md"});
    CHECK(note_tags("main.c", "#include <stdio.h>\n").empty());
    CHECK(note_tags("a.py", "#todo").empty());
    CHECK(note_tags("a.md", std::string("#ikili\0x", 9)).empty());
}

// MAP-R: haritadan etiket kaldırma aynı tarayıcıyı kullanır; kod, bağlantı ve başka etiket yerinde kalır.
// Düzyazıdaki etiketin sözcüğü kalır, yalnız `#` gider (lead kararı); yalnız etiketlerden oluşan satırda
// etiket bir boşlukla silinir, satır boşalırsa satır da.
TEST_CASE("remove_note_tag keeps the word in prose, cuts it from tag-only lines, leaves code alone") {
    CHECK(remove_note_tag("a.md", "I love #cats and dogs\n", "cats") == "I love cats and dogs\n");
    CHECK(remove_note_tag("a.md", "a #Proje b\n", "proje") == "a Proje b\n");
    CHECK(remove_note_tag("a.md", "#proje b\n", "proje") == "proje b\n");
    CHECK(remove_note_tag("a.md", "x\n#proje\r\ny\n", "proje") == "x\ny\n");
    CHECK(remove_note_tag("a.md", "#proje #proje\n", "proje") == "");
    CHECK(remove_note_tag("a.md", "#proje #iş\n", "proje") == "#iş\n");
    CHECK(remove_note_tag("a.md", "#proje/ #proje/alt\n", "proje") == "#proje/alt\n");
    CHECK(remove_note_tag("a.md", "#a#b\n", "a") == "a#b\n");  // ikinci `#` etiket değildir
    CHECK(note_tags("a.md", "a#b\n").empty());
    CHECK(remove_note_tag("a.md", "(#proje) `#proje` [[n#proje]]\n", "proje") ==
          "(proje) `#proje` [[n#proje]]\n");
    CHECK(remove_note_tag("a.md", "```\n#proje\n```\n#proje", "proje") == "```\n#proje\n```\n");
    CHECK(remove_note_tag("a.py", "#proje\n", "proje") == "#proje\n");
    CHECK(remove_note_tag("a.md", "#iş\n", "proje") == "#iş\n");
    CHECK(is_prose_note("a.md"));
    CHECK(is_prose_note("a"));
    CHECK_FALSE(is_prose_note("a.py"));
}

// L3: etiket sayısı ve uzunluğu sınırsızdır; eski tavanların (256 etiket, 128 bayt) üstü sessizce düşmez.
TEST_CASE("note_tags keeps more than 256 tags and tags longer than 128 bytes") {
    std::string body;
    for (int i = 0; i < 300; ++i)
        body += "#e" + std::to_string(i) + " ";
    CHECK(note_tags("a.md", body).size() == 300);
    const std::string long_tag(200, 'u');
    CHECK(note_tags("a.md", "#" + long_tag) == Texts{long_tag});
}

TEST_CASE("a saved note with more than 256 tags has every TAGGED edge") {
    const TempDir dir;
    NoteStore store(dir.path());
    std::string body;
    for (int i = 0; i < 300; ++i)
        body += "#e" + std::to_string(i) + " ";
    REQUIRE(seed_note(store, "a.md", body));
    CHECK(tagged(dir.path(), "a").size() == 300);
}

TEST_CASE("saving a note writes Tag nodes and TAGGED edges that Cypher can match") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "#Proje #proje/alt\n"));
    REQUIRE(seed_note(store, "y.md", "#proje\n"));
    CHECK(tag_names(dir.path()) == Texts{"proje", "proje/alt"});
    CHECK(query_texts(dir.path(), "MATCH (n:Note)-[:TAGGED]->(t:Tag {name:'proje'}) RETURN n.name ORDER BY "
                                  "n.name") == Texts{"x", "y"});
    CHECK(query_texts(dir.path(), "MATCH (t:Tag {name:'proje'}) RETURN t.display") == Texts{"Proje"});
}

TEST_CASE("editing replaces TAGGED edges and an orphan Tag node is removed") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "#a #b\n"));
    write_body(store, "x.md", "#a #c\n");
    CHECK(tagged(dir.path(), "x") == Texts{"a", "c"});
    CHECK(tag_names(dir.path()) == Texts{"a", "c"});
    write_body(store, "x.md", "etiketsiz\n");
    CHECK(tagged(dir.path(), "x").empty());
    CHECK(tag_names(dir.path()).empty());
}

TEST_CASE("remove and purge drop the edges and orphan tags; a trashed note keeps its tags out of counts") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "#ortak #x\n"));
    REQUIRE(seed_note(store, "y.md", "#ortak #y\n"));
    REQUIRE(seed_note(store, "z.md", "#z\n"));

    REQUIRE(store.remove(np("x.md")).has_value());
    CHECK(tag_names(dir.path()) == Texts{"ortak", "y", "z"});

    REQUIRE(store.trash(np("y.md")).has_value());
    CHECK(tag_names(dir.path()) == Texts{"ortak", "y", "z"});
    const auto counts = store.tags();
    REQUIRE(counts.size() == 1);
    CHECK(counts[0].name == "z");
    CHECK(counts[0].count == 1);
    REQUIRE(store.restore(np("y.md")).has_value());
    CHECK(store.tags().size() == 3);

    REQUIRE(store.trash(np("y.md")).has_value());
    REQUIRE(store.purge(np("y.md")).has_value());
    CHECK(tag_names(dir.path()) == Texts{"z"});
}

TEST_CASE("list_meta, links and tags() expose a note's tags") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "#Proje #b\n"));
    REQUIRE(seed_note(store, "y.md", "#proje\n"));
    for (const NoteMeta& meta : store.list_meta()) {
        if (meta.path.string() == "x.md")
            CHECK(meta.tags == Texts{"b", "proje"});
        else
            CHECK(meta.tags == Texts{"proje"});
    }
    const auto links = store.links(np("x.md"));
    REQUIRE(links.has_value());
    CHECK(links->tags == Texts{"b", "Proje"});
    CHECK_FALSE(links->empty());

    const auto counts = store.tags();
    REQUIRE(counts.size() == 2);
    CHECK(counts[0].name == "b");
    CHECK(counts[0].count == 1);
    CHECK(counts[1].name == "proje");
    CHECK(counts[1].display == "Proje");
    CHECK(counts[1].count == 2);
}

TEST_CASE("changing a note's type to code drops its tags, back to prose restores them") {
    const TempDir dir;
    NoteStore store(dir.path());
    REQUIRE(seed_note(store, "x.md", "#a\n"));
    REQUIRE(store.move(np("x.md"), np("x.py")).has_value());
    CHECK(tag_names(dir.path()).empty());
    REQUIRE(store.move(np("x.py"), np("x.txt")).has_value());
    CHECK(tag_names(dir.path()) == Texts{"a"});
}

TEST_CASE("two devices adding different tags to one note end with both tags on both devices") {
    const noteapp::sync::GitRuntime runtime;  // merge_text libgit2'nin ayırıcısını ister
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n"));
    exchange_log(a, b, logs.path());

    write_body(a, "x.md", "0 #a\n1\n2\n3\n4\n5\n6\n7\n8\n9\n");
    write_body(b, "x.md", "0\n1\n2\n3\n4\n5\n6\n7\n8\n9 #b\n");
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());
    exchange_log(a, b, logs.path());

    CHECK(a.read(np("x.md"))->text == "0 #a\n1\n2\n3\n4\n5\n6\n7\n8\n9 #b\n");
    CHECK(a.list().size() == 1);  // kopya değil, birleşim
    CHECK(tagged(data_a.path(), "x") == Texts{"a", "b"});
    CHECK(tagged(data_b.path(), "x") == Texts{"a", "b"});
    CHECK(tag_names(data_a.path()) == Texts{"a", "b"});
    CHECK(tag_names(data_b.path()) == Texts{"a", "b"});
}

TEST_CASE("a tag removed as orphan on one device survives a concurrent use on the other") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    REQUIRE(seed_note(a, "x.md", "#ortak\n"));
    REQUIRE(seed_note(a, "y.md", "boş\n"));
    exchange_log(a, b, logs.path());

    write_body(a, "x.md", "artık yok\n");  // a'da Tag ortak sahipsiz kalır, silinir
    write_body(b, "y.md", "#ortak\n");     // b aynı anda kullanır
    exchange_log(a, b, logs.path());
    exchange_log(b, a, logs.path());

    CHECK(tagged(data_a.path(), "y") == Texts{"ortak"});
    CHECK(tagged(data_b.path(), "y") == Texts{"ortak"});
    CHECK(tagged(data_a.path(), "x").empty());
}

// İnceleme bulgusu: sahipsiz silme ile eşzamanlı kullanım her cihazda ve her teslim sırasında aynı Tag'i
// (etiket, ad, gösterim) bırakmalı; çıplak düğüm etiketi görünmez kılardı.
TEST_CASE("a concurrent orphan delete and reuse leave the same tags everywhere, in any delivery order") {
    const auto counts = [](const NoteStore& store) {
        Texts out;
        for (const TagCount& tag : store.tags())
            out.push_back(tag.name + "|" + tag.display + "|" + std::to_string(tag.count));
        return out;
    };
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    const TempDir data_c;
    const TempDir data_d;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    NoteStore c(data_c.path());
    NoteStore d(data_d.path());
    REQUIRE(seed_note(a, "x.md", "#Ortak\n"));
    REQUIRE(seed_note(a, "y.md", "boş\n"));
    for (NoteStore* other : {&b, &c, &d})  // herkes ortak tabanı görür: sonraki teslim sırası serbest
        exchange_log(a, *other, logs.path());

    write_body(a, "x.md", "artık yok\n");  // a'da Tag sahipsiz kalır, silinir
    write_body(b, "y.md", "#ortak\n");     // b aynı anda kullanır
    exchange_log(a, c, logs.path());
    exchange_log(b, c, logs.path());
    exchange_log(b, d, logs.path());
    exchange_log(a, d, logs.path());
    exchange_log(b, a, logs.path());
    exchange_log(a, b, logs.path());

    const Texts want{"ortak|Ortak|1"};
    CHECK(counts(a) == want);
    CHECK(counts(b) == want);
    CHECK(counts(c) == want);
    CHECK(counts(d) == want);
}

// İnceleme bulgusu: etiketsiz sürümün gövde yazması (ya da etiketlerden önceki günlük) senkronla gelir. Alan
// cihaz, ister eski ister yeni (boş grafla işaret yazmış) olsun, gövdesi değişen notu etiketler.
TEST_CASE("a body from a device without tags is tagged by every device that ingests it") {
    const TempDir logs;
    const TempDir data_a;
    const TempDir data_b;
    const TempDir data_c;
    NoteStore a(data_a.path());
    NoteStore b(data_b.path());
    NoteStore c(data_c.path());
    REQUIRE(seed_note(a, "x.md", "düz\n"));
    exchange_log(a, b, logs.path());
    {
        // Eski sürüm: gövdeyi yazar, TAGGED kurmaz.
        auto raw = graph::Graph::open(data_a.path() / "graph.db");
        REQUIRE(raw.has_value());
        const graph::Params params{{"b", graph::Bytes{"#eski\n"}}};
        REQUIRE(raw->query("MATCH (n:Note {name: 'x'}) SET n.body = $b", params, graph::QueryMode::ReadWrite)
                    .has_value());
    }
    REQUIRE(tagged(data_a.path(), "x").empty());
    exchange_log(a, b, logs.path());
    CHECK(tagged(data_b.path(), "x") == Texts{"eski"});
    CHECK(c.list().empty());  // yeni cihaz: boş grafla açılır, işaret yazılır
    exchange_log(a, c, logs.path());
    CHECK(tagged(data_c.path(), "x") == Texts{"eski"});
}

TEST_CASE("a database written before tags existed gets them on first open") {
    const TempDir dir;
    {
        NoteStore store(dir.path());
        REQUIRE(seed_note(store, "x.md", "#eski\n"));
    }
    {
        // Önceki sürümün grafı: etiket yok, işaret dosyası yok.
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (t:Tag) DETACH DELETE t", {}, graph::QueryMode::ReadWrite).has_value());
    }
    fs::remove(dir.path() / "tags-indexed");
    REQUIRE(tag_names(dir.path()).empty());

    const NoteStore reopened(dir.path());
    const auto metas = reopened.list_meta();
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].tags == Texts{"eski"});
    CHECK(tagged(dir.path(), "x") == Texts{"eski"});
    CHECK(fs::exists(dir.path() / "tags-indexed"));
}

// İnceleme bulgusu: toplu yeniden türetme işlemler arasında başka yazar notları silse de hiçbir notu atlamaz.
TEST_CASE("relinking resumes by note id, a concurrent delete between batches skips no note") {
    const TempDir dir;
    constexpr int kNotes = 30;
    constexpr int kTagsPerNote = 256;  // not başına ~1280 işlem: ilk işlem 30.000'de kesilir
    {
        NoteStore seed(dir.path());
        for (int note = 0; note < kNotes; ++note) {
            std::string body;
            for (int tag = 0; tag < kTagsPerNote; ++tag)
                body += "#n" + std::to_string(note) + "t" + std::to_string(tag) + " ";
            REQUIRE(seed_note(seed, "n" + std::to_string(note) + ".md", body));
        }
        auto raw = graph::Graph::open(dir.path() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (t:Tag) DETACH DELETE t", {}, graph::QueryMode::ReadWrite).has_value());
    }
    NoteStore store(dir.path());
    int batches = 0;
    store.impl().after_rederive_batch = [&] {
        ++batches;
        NoteStore other(dir.path());  // ayrı bağlantı: işlenmiş notları siler, kalanların sırası kayar
        for (const std::string& name :
             query_texts(dir.path(), "MATCH (n:Note)-[:TAGGED]->(:Tag) RETURN DISTINCT n.name"))
            REQUIRE(other.remove(np(name + ".md")).has_value());
    };
    REQUIRE(noteapp::test::relink_notes(store).has_value());
    CHECK(batches >= 1);
    const auto metas = store.list_meta();
    REQUIRE_FALSE(metas.empty());
    for (const NoteMeta& meta : metas) {
        CAPTURE(meta.path.string());
        CHECK(meta.tags.size() == kTagsPerNote);
    }
}
