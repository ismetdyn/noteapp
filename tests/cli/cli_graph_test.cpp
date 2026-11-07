// Dışa aktarma uçtan uca (G6): gerçek `note` ikilisi.
#include "core/note_store.hpp"
#include "graph/graph.hpp"
#include "support/cli_sandbox.hpp"
#include "support/note_seed.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace noteapp::test;
namespace fs = std::filesystem;

namespace {

void write_plain(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << bytes;
    out.flush();
    REQUIRE_MESSAGE(out.good(), "cannot write test file: ", noteapp::test::utf8(file));
}

// Dizindeki düz dosyalar: göreli yol → bayt.
std::map<std::string, std::string> tree_of(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
        if (entry.is_regular_file())
            files[entry.path().lexically_relative(dir).generic_string()] = slurp(entry.path());
    return files;
}

std::string exact_bytes() {
    std::string bytes = "\xEF\xBB\xBF"
                        "bir\r\niki\n";
    bytes += '\0';
    bytes += "\xFF\xFE son \x80";
    return bytes;
}

// Uzantısız ve ikili not, boş klasör ve çöpte bir not.
void seed_store(const Sandbox& box) {
    box.put("plan.md", "plan\n");
    box.put("work/crlf.txt", "a\r\nb\r\n");
    box.put("work/ham.bin", exact_bytes());
    box.put("work/alt/uzantisiz", "uzantisiz\n");
    box.put("eski.md", "çöp\n");
    noteapp::core::NoteStore store(box.data());
    REQUIRE(store.create_folder("bos").has_value());
    REQUIRE(store.trash(noteapp::core::NotePath::parse("eski.md").value()).has_value());
}

}  // namespace

TEST_CASE("note export --all output is byte-identical to the originals") {
    const Sandbox box("en");
    seed_store(box);
    const TempDir out;
    const fs::path target = out.path() / "cikti";
    const auto exported = box.run({"export", "--all", utf8(target)});
    REQUIRE_MESSAGE(exported.exit_code == 0, exported.err);

    const std::map<std::string, std::string> expected{{"plan.md", "plan\n"},
                                                      {"work/crlf.txt", "a\r\nb\r\n"},
                                                      {"work/ham.bin", exact_bytes()},
                                                      {"work/alt/uzantisiz", "uzantisiz\n"},
                                                      {".trash/eski.md", "çöp\n"}};
    CHECK(tree_of(target) == expected);
    CHECK(fs::is_directory(target / "bos"));
}

TEST_CASE("note export --all never overwrites and never goes through a link") {
    const Sandbox box("en");
    box.put("var.md", "yeni");
    box.put("bagli/not.md", "içeride");
    const TempDir out;
    const TempDir outside;
    const fs::path target = out.path() / "cikti";
    write_plain(target / "var.md", "eski");
    REQUIRE(make_dir_link(outside.path(), target / "bagli"));

    const auto exported = box.run({"export", "--all", utf8(target)});
    CHECK(exported.exit_code == 1);
    CHECK(exported.err.find("var.md") != std::string::npos);
    CHECK(exported.err.find("bagli") != std::string::npos);
    CHECK(slurp(target / "var.md") == "eski");
    CHECK(fs::is_empty(outside.path()));
}

TEST_CASE("a hostile name from a remote device cannot escape note export --all") {
    const Sandbox box("en");
    box.put("gercek.md", "x");
    {
        auto raw = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(raw.has_value());
        auto txn = raw->write();
        REQUIRE(txn.has_value());
        for (const char* name : {"../../kacis", ".ssh/id", "CON", "a/../../b"}) {
            const auto id = noteapp::graph::new_node_id();
            REQUIRE(txn->put_node(id).has_value());
            REQUIRE(txn->add_label(id, "Note").has_value());
            REQUIRE(txn->set_property(id, "name", noteapp::graph::Value{std::string(name)}).has_value());
            REQUIRE(txn->set_property(id, "type", noteapp::graph::Value{std::string("md")}).has_value());
            REQUIRE(txn->set_property(id, "body", noteapp::graph::Value{noteapp::graph::Bytes{"kötü"}})
                        .has_value());
        }
        REQUIRE(txn->commit().has_value());
    }
    const TempDir out;
    const fs::path target = out.path() / "a" / "b" / "cikti";
    const auto exported = box.run({"export", "--all", utf8(target)});
    CHECK(exported.exit_code == 1);  // ulaşılamayan notlar raporlanır
    const auto tree = tree_of(out.path());
    CHECK(tree.size() == 1);
    CHECK(tree.contains("a/b/cikti/gercek.md"));
}

TEST_CASE(
    "graph export and import rebuild the same graph elsewhere, exact bytes, and repeat without writing") {
    const Sandbox box("en");
    box.put("work/ham.bin", exact_bytes());
    box.put("hedef.md", std::string(1000, 'x'));
    box.put("work/kaynak.md", "bak [[hedef]]\n");
    noteapp::core::NoteStore source(box.data());
    REQUIRE(source.create_folder("bos").has_value());
    const TempDir out;
    const fs::path dump = out.path() / "dump.jsonl";
    const auto exported = box.run({"graph", "export", "--format", "jsonl", "-o", utf8(dump)});
    REQUIRE_MESSAGE(exported.exit_code == 0, exported.err);

    const Sandbox other("en");
    const auto imported = other.run({"graph", "import", utf8(dump)});
    REQUIRE_MESSAGE(imported.exit_code == 0, imported.err);
    CHECK(other.body("work/ham.bin") == exact_bytes());
    CHECK(other.body("hedef.md") == std::string(1000, 'x'));
    CHECK(folder_exists(other.data(), "bos"));
    const auto links = other.run({"query", "--json", "MATCH (a:Note)-[:LINKS_TO]->(b:Note) RETURN b.name"});
    CHECK(links.out.find("hedef") != std::string::npos);

    const auto again = other.run({"graph", "export", "--format", "jsonl"});
    const auto second = other.run({"graph", "import", utf8(dump)});
    REQUIRE(second.exit_code == 0);
    CHECK(other.run({"graph", "export", "--format", "jsonl"}).out == again.out);
    CHECK(slurp(dump) == again.out);
}

// Değişiklik senkronla yayılır: sessiz üzerine yazma bütün cihazlardaki notu değiştirirdi.
TEST_CASE("graph import refuses to change an existing id without --overwrite, names it and keeps the note") {
    const Sandbox box("en");
    box.put("not.md", "ilk\n");
    const TempDir out;
    const fs::path dump = out.path() / "dump.jsonl";
    REQUIRE(box.run({"graph", "export", "--format", "jsonl", "-o", utf8(dump)}).exit_code == 0);
    {
        noteapp::core::NoteStore store(box.data());
        const auto path = *noteapp::core::NotePath::parse("not.md");
        const auto current = store.read(path);
        REQUIRE(current.has_value());
        REQUIRE(store.write(path, "degisti\n", current->revision).has_value());
    }

    const auto refused = box.run({"graph", "import", utf8(dump)});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("Refused") != std::string::npos);
    CHECK(box.body("not.md") == "degisti\n");

    const auto overwritten = box.run({"graph", "import", "--overwrite", utf8(dump)});
    CHECK_MESSAGE(overwritten.exit_code == 0, overwritten.err);
    CHECK(box.body("not.md") == "ilk\n");
    CHECK(box.run({"graph", "import", "--yok", utf8(dump)}).exit_code == 2);
}

TEST_CASE("graph import refuses labels, edge types and property keys the note view would not accept") {
    const Sandbox box("en");
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";
    const std::string header = "{\"k\":\"noteapp-graph\",\"v\":1}\n";
    const std::string id = "00000000000000000000000000000001";
    const std::string node = "{\"k\":\"node\",\"id\":\"" + id + "\",\"labels\":[],\"props\":{}}\n";
    SUBCASE("label") {
        write_plain(dump,
                    header + "{\"k\":\"node\",\"id\":\"" + id + "\",\"labels\":[\"../x\"],\"props\":{}}\n");
    }
    SUBCASE("key") {
        write_plain(dump, header + "{\"k\":\"node\",\"id\":\"" + id +
                              "\",\"labels\":[],\"props\":{\"a/b\":{\"s\":\"x\"}}}\n");
    }
    SUBCASE("edge type") {
        write_plain(dump, header + node +
                              "{\"k\":\"edge\",\"id\":\"00000000000000000000000000000002\",\"src\":\"" + id +
                              "\",\"type\":\"a/b\",\"dst\":\"" + id + "\",\"props\":{}}\n");
    }
    const auto refused = box.run({"graph", "import", utf8(dump)});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("Line ") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("graph export -o creates a private new file, follows no link and overwrites nothing") {
    const Sandbox box("en");
    box.put("not.md", "gizli\n");
    const TempDir out;
    const fs::path dump = out.path() / "dump.jsonl";
    REQUIRE(box.run({"graph", "export", "--format", "jsonl", "-o", utf8(dump)}).exit_code == 0);
    CHECK((fs::status(dump).permissions() & fs::perms::mask) ==
          (fs::perms::owner_read | fs::perms::owner_write));

    const std::string first = slurp(dump);
    CHECK(box.run({"graph", "export", "--format", "jsonl", "-o", utf8(dump)}).exit_code != 0);
    CHECK(slurp(dump) == first);

    const fs::path victim = out.path() / "kurban.txt";
    write_plain(victim, "dokunma");
    fs::create_symlink(victim, out.path() / "bag");
    CHECK(box.run({"graph", "export", "--format", "jsonl", "-o", utf8(out.path() / "bag")}).exit_code != 0);
    CHECK(slurp(victim) == "dokunma");
}

TEST_CASE("export --all writes private files in private new directories and leaves an existing root alone") {
    const Sandbox box("en");
    box.put("alt/not.md", "gizli\n");
    const TempDir out;
    const fs::path fresh = out.path() / "yeni" / "cikti";
    REQUIRE(box.run({"export", "--all", utf8(fresh)}).exit_code == 0);
    const auto mode = [](const fs::path& path) { return fs::status(path).permissions() & fs::perms::mask; };
    CHECK(mode(out.path() / "yeni") == fs::perms::owner_all);
    CHECK(mode(fresh) == fs::perms::owner_all);
    CHECK(mode(fresh / "alt") == fs::perms::owner_all);
    CHECK(mode(fresh / "alt" / "not.md") == (fs::perms::owner_read | fs::perms::owner_write));

    const fs::path existing = out.path() / "var";
    fs::create_directories(existing);
    fs::permissions(existing, static_cast<fs::perms>(0755), fs::perm_options::replace);
    REQUIRE(box.run({"export", "--all", utf8(existing)}).exit_code == 0);
    CHECK(mode(existing) == static_cast<fs::perms>(0755));
    CHECK(mode(existing / "alt") == fs::perms::owner_all);
}
#endif

TEST_CASE("graph import reports a bad line with its number and leaves the store usable") {
    const Sandbox box("en");
    const TempDir out;
    const fs::path dump = out.path() / "bad.jsonl";
    write_plain(
        dump,
        "{\"k\":\"noteapp-graph\",\"v\":1}\n{\"k\":\"node\",\"id\":\"zz\",\"labels\":[],\"props\":{}}\n");
    const auto refused = box.run({"graph", "import", utf8(dump)});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("Line 2") != std::string::npos);
    write_plain(dump, "bu json değil\n");
    CHECK(box.run({"graph", "import", utf8(dump)}).exit_code == 1);
    CHECK(box.run({"-n", "n", "metin"}).exit_code == 0);
}

TEST_CASE("graph usage errors exit 2") {
    const Sandbox box("en");
    CHECK(box.run({"graph"}).exit_code == 2);
    CHECK(box.run({"graph", "yok"}).exit_code == 2);
    CHECK(box.run({"graph", "export"}).exit_code == 2);
    CHECK(box.run({"graph", "import"}).exit_code == 2);
    CHECK(box.run({"export", "--all"}).exit_code == 2);
    CHECK(box.run({"export", "--all", "d", "--pdf", "x"}).exit_code == 2);
}

TEST_CASE("note export --all also writes the trash under .trash; the trash comes back byte for byte") {
    const Sandbox box("en");
    seed_store(box);
    const TempDir out;
    const fs::path target = out.path() / "cikti";
    const auto exported = box.run({"export", "--all", utf8(target)});
    REQUIRE_MESSAGE(exported.exit_code == 0, exported.err);
    CHECK(slurp(target / ".trash" / "eski.md") == "çöp\n");
}

TEST_CASE("note export --all lists unreachable notes, exits non-zero; graph export keeps their bytes") {
    const Sandbox box("en");
    box.put("gercek.md", "x");
    {
        auto raw = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(raw.has_value());
        auto txn = raw->write();
        REQUIRE(txn.has_value());
        const auto id = noteapp::graph::new_node_id();
        REQUIRE(txn->put_node(id).has_value());
        REQUIRE(txn->add_label(id, "Note").has_value());
        REQUIRE(txn->set_property(id, "name", noteapp::graph::Value{std::string("CON")}).has_value());
        REQUIRE(txn->set_property(id, "type", noteapp::graph::Value{std::string("md")}).has_value());
        REQUIRE(txn->set_property(id, "body", noteapp::graph::Value{noteapp::graph::Bytes{"kurtarilacak"}})
                    .has_value());
        REQUIRE(txn->commit().has_value());
    }
    const TempDir out;
    const auto exported = box.run({"export", "--all", utf8(out.path() / "cikti")});
    CHECK(exported.exit_code == 1);
    CHECK(exported.err.find("CON.md") != std::string::npos);
    CHECK(exported.err.find("note graph export") != std::string::npos);
    const auto dump = box.run({"graph", "export", "--format", "jsonl"});
    REQUIRE(dump.exit_code == 0);
    CHECK(dump.out.find("a3VydGFyaWxhY2Fr") != std::string::npos);  // base64("kurtarilacak")
}

TEST_CASE("graph import: oversized line, huge array, bad base64, edge without a node are refused with a line "
          "number") {
    const Sandbox box("en");
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";
    const std::string header = "{\"k\":\"noteapp-graph\",\"v\":1}\n";
    const std::string node =
        "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[],\"props\":{}}\n";

    SUBCASE("line over the limit") {
        std::string big = header;
        big.append(std::size_t{129} * 1024U * 1024U, ' ');
        write_plain(dump, big + "\n");
    }
    SUBCASE("array with millions of items") {
        std::string big = header + "{\"k\":\"node\",\"labels\":[";
        for (int i = 0; i < 4000000; ++i)
            big += "1,";
        write_plain(dump, big + "1]}\n");
    }
    SUBCASE("bad base64") {
        write_plain(dump, header + "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[]"
                                   ",\"props\":{\"x\":{\"b\":\"@@@@\"}}}\n");
    }
    SUBCASE("edge to a missing node") {
        write_plain(dump, header + node +
                              "{\"k\":\"edge\",\"id\":\"00000000000000000000000000000002\",\"src\":"
                              "\"00000000000000000000000000000001\",\"type\":\"T\",\"dst\":"
                              "\"00000000000000000000000000000009\",\"props\":{}}\n");
    }
    const auto refused = box.run({"graph", "import", utf8(dump)});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("Line ") != std::string::npos);
}

// L6: satır başına JSON değer sınırı kullanıcının kendi dışa aktarımını reddedebilir (çok özellikli düğüm);
// sınır limits.graph.import_line_values'tadır, 0 = sınırsız.
TEST_CASE("graph import: limits.graph.import_line_values caps the JSON values per line and 0 lifts the cap") {
    const auto line_with_labels = [](int count) {
        std::string line = "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[";
        for (int i = 0; i < count; ++i)
            line += (i == 0 ? "\"L" : ",\"L") + std::to_string(i) + "\"";
        return "{\"k\":\"noteapp-graph\",\"v\":1}\n" + line + "],\"props\":{}}\n";
    };
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";

    SUBCASE("a small cap refuses a line over it") {
        const Sandbox box("en");
        std::ofstream(box.config_file(), std::ios::app) << "[limits.graph]\nimport_line_values = 10\n";
        write_plain(dump, line_with_labels(20));
        const auto refused = box.run({"graph", "import", utf8(dump)});
        CHECK(refused.err.find("config") == std::string::npos);  // anahtar tanınır, uyarı yok
        CHECK(refused.exit_code == 1);
        CHECK(refused.err.find("Line ") != std::string::npos);
    }
    SUBCASE("0 takes a line over the default of 100000 values") {
        const Sandbox box("en");
        std::ofstream(box.config_file(), std::ios::app) << "[limits.graph]\nimport_line_values = 0\n";
        constexpr int kOverDefault = 110000;
        write_plain(dump, line_with_labels(kOverDefault));
        const auto imported = box.run({"graph", "import", utf8(dump)});
        INFO(imported.err);
        CHECK(imported.exit_code == 0);
        CHECK(box.run({"query", "MATCH (n:L109999) RETURN count(n)"}).out == "count(n)\n1\n");
    }
}

// Lead kararı 2 (tasks/plan-limits.md): tek satır çerçeve tavanından (65.536 işlem) çok işlem üretse de
// reddedilmez. L4b: satır tek işlemdir (ya hepsi ya hiç), çerçeveleri yazan böler; yeniden içe aktarma işlem
// yazmaz (idempotent).
TEST_CASE("graph import takes a line with more operations than one frame holds as one transaction") {
    const Sandbox box("en");
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";
    // Etiket satırda tek JSON değeridir (satır başına 100.000 değer): 70.000 etiket + put_node varsayılan
    // çerçeve sınırını (65.536) aşar.
    constexpr int kLabels = 70000;
    std::string line = "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[";
    for (int i = 0; i < kLabels; ++i)
        line += (i == 0 ? "\"L" : ",\"L") + std::to_string(i) + "\"";
    write_plain(dump, "{\"k\":\"noteapp-graph\",\"v\":1}\n" + line + "],\"props\":{\"x\":{\"i\":\"7\"}}}\n");

    const auto imported = box.run({"graph", "import", utf8(dump)});
    INFO(imported.err);
    REQUIRE(imported.exit_code == 0);
    const auto count = box.run({"query", "MATCH (n:L69999) RETURN n.x"});
    CHECK(count.out == "n.x\n7\n");
    const auto seq_after = [&box] {
        auto graph = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(graph.has_value());
        return *graph->last_seq();
    };
    const std::uint64_t first = seq_after();
    CHECK(first == 1);  // satır bölünmez: tek işlem

    const auto again = box.run({"graph", "import", utf8(dump)});
    INFO(again.err);
    CHECK(again.exit_code == 0);
    CHECK(seq_after() == first);
}

// Lead bulgusu: make_room satır ortasında commit ediyordu; çerçeveden büyük değer sonra reddedilince yarım düğüm
// kalıyor, yineleme onu reddediyordu. Satır tek işlemdir; değer çerçeveden büyük olabilir.
TEST_CASE("graph import: a line with a value larger than one frame lands whole and a rerun changes nothing") {
    const Sandbox box("en");
    std::ofstream(box.config_file(), std::ios::app) << "[limits.graph]\nframe_bytes = \"64KiB\"\n";
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";
    const std::string text(200U * 1024U, 'a');
    write_plain(dump, "{\"k\":\"noteapp-graph\",\"v\":1}\n"
                      "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[\"Big\"],"
                      "\"props\":{\"a\":{\"i\":\"1\"},\"t\":{\"s\":\"" +
                          text + "\"},\"z\":{\"i\":\"2\"}}}\n");
    const auto seq_after = [&box] {
        auto graph = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(graph.has_value());
        return *graph->last_seq();
    };

    const auto imported = box.run({"graph", "import", utf8(dump)});
    INFO(imported.err);
    REQUIRE(imported.exit_code == 0);
    CHECK(box.run({"query", "MATCH (n:Big) RETURN n.z"}).out == "n.z\n2\n");
    CHECK(box.run({"query", "MATCH (n:Big) WHERE n.t STARTS WITH 'aaa' RETURN n.a"}).out == "n.a\n1\n");
    CHECK(seq_after() == 1);

    const auto again = box.run({"graph", "import", utf8(dump)});
    INFO(again.err);
    CHECK(again.exit_code == 0);
    CHECK(seq_after() == 1);
}

TEST_CASE("graph import accepts null in an unknown field and still refuses it in a known one") {
    const Sandbox box("en");
    const TempDir out;
    const fs::path dump = out.path() / "d.jsonl";
    const std::string header = "{\"k\":\"noteapp-graph\",\"v\":1}\n";
    write_plain(dump, header + "{\"k\":\"node\",\"id\":\"00000000000000000000000000000001\",\"labels\":[],"
                               "\"props\":{},\"extra\":null}\n");
    CHECK(box.run({"graph", "import", utf8(dump)}).exit_code == 0);

    write_plain(dump, header + "{\"k\":\"node\",\"id\":\"00000000000000000000000000000002\",\"labels\":null,"
                               "\"props\":{}}\n");
    const auto refused = box.run({"graph", "import", utf8(dump)});
    CHECK(refused.exit_code == 1);
    CHECK(refused.err.find("Line 2") != std::string::npos);
}

TEST_CASE("the status file cannot put escape sequences on the terminal") {
    const Sandbox box("en");
    const fs::path status = box.notes().parent_path() / "state" / "sync-status.toml";
    write_plain(status,
                "pushed_seq = 0\n[last_error]\nkey = \"sync.push_rejected\"\n[last_error.args]\nbranch = "
                "\"main\"\ndetail = \"\\u001b]52;c;ZXZpbA==\\u0007kirli\"\n"
                "[[log_issues]]\nkey = \"sync.push_rejected\"\n[log_issues.args]\nbranch = \"main\"\ndetail "
                "= \"\\u001b[31mkirli2\"\n");
    const auto shown = box.run({"sync", "status"});
    CHECK(shown.out.find("kirli") != std::string::npos);
    CHECK(shown.out.find('\x1B') == std::string::npos);
    CHECK(shown.out.find('\x07') == std::string::npos);
}

#ifdef __linux__
#include "support/pty_run.hpp"

namespace {

// stdout ve stderr gerçek terminal: isatty dalı böyle sınanır.
std::string run_in_pty(const std::vector<std::string>& args) {
    return noteapp::test::run_in_pty(NOTEAPP_NOTE_EXE, args).terminal;
}

}  // namespace

TEST_CASE("names from sync are sanitized on a terminal and stay byte-exact in a pipe") {
    const Sandbox box("en");
    const std::string evil = "kotu\xC2\x9D"
                             "52;c;ZXZpbA==\xC2\x9C.md";
    // Etiketin gösterim adı graph'tan gelir (senkronla başka cihazdan): C1 (OSC/ST) taşıyan ad `note tags`ta
    // temizlenmeli. Ayrıştırıcı içerikten böyle etiket üretmez; düğüm doğrudan yazılır.
    box.put(evil, "#etiket içerik\n");
    {
        // `query --write` Tag.display'e yazamaz (türetilmiş, korunan): başka cihazdan gelmiş gibi doğrudan motora.
        auto raw = noteapp::graph::Graph::open(box.data() / "graph.db");
        REQUIRE(raw.has_value());
        REQUIRE(raw->query("MATCH (t:Tag) SET t.display = $d",
                           {{"d", noteapp::graph::QueryValue(std::string("etiket\xC2\x9D"
                                                                         "52;c;ZXZpbA==\xC2\x9C"))}},
                           noteapp::graph::QueryMode::ReadWrite)
                    .has_value());
    }
    // Etiket adı çekirdekte okunurken temizlenir (ilişki adı kuralı): boruda da C1 yok, U+FFFD var. Not adı
    // ise yoldur, boruda bayt bayt kalır (aşağıda).
    const std::string piped_tags = box.run({"tags"}).out;
    REQUIRE(piped_tags.find("\xC2\x9D") == std::string::npos);
    REQUIRE(piped_tags.find("etiket\xEF\xBF\xBD"
                            "52;c;ZXZpbA==\xEF\xBF\xBD") != std::string::npos);

    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"ls"}, std::vector<std::string>{"show", "kotu"}, std::vector<std::string>{"ls", "-l"},
          std::vector<std::string>{"tags"}}) {
        CAPTURE(args.back());
        const std::string terminal = run_in_pty(args);
        CHECK(terminal.find("52;c;ZXZpbA==") != std::string::npos);
        CHECK(terminal.find("\xC2\x9D") == std::string::npos);
        CHECK(terminal.find("\xC2\x9C") == std::string::npos);
    }
    CHECK(box.run({"ls"}).out.find(evil) != std::string::npos);     // boru: bayt bayt
    CHECK(box.run({"ls", "-l"}).out.find(evil) != std::string::npos);
}

TEST_CASE("bidi and zero-width characters show as U+FFFD on a terminal and stay byte-exact in a pipe") {
    const Sandbox box("en");
    const std::string rlo = "\xE2\x80\xAE";
    const std::string name = "gpj" + rlo + "exe.md";
    const std::string body = "kelime a" + rlo +
                             "b\xE2\x80\x8B"
                             "c \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\n";
    box.put(name, body);
    const std::string marker = "\xEF\xBF\xBD";

    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"ls"}, std::vector<std::string>{"show", "gpj"},
          std::vector<std::string>{"find", "kelime"}}) {
        CAPTURE(args.front());
        const std::string terminal = run_in_pty(args);
        CHECK(terminal.find(rlo) == std::string::npos);
        CHECK(terminal.find("\xE2\x80\x8B") == std::string::npos);
        CHECK(terminal.find(marker) != std::string::npos);
    }
    CHECK(run_in_pty({"ls"}).find("gpj" + marker + "exe.md") != std::string::npos);
    const std::string shown = run_in_pty({"show", "gpj"});
    CHECK(shown.find("a" + marker + "b" + marker + "c") != std::string::npos);
    CHECK(shown.find("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9") != std::string::npos);  // ZWJ kalır

    // Boru: betikler ham içeriği alır.
    CHECK(box.run({"ls"}).out.find(name) != std::string::npos);
    CHECK(box.run({"show", "gpj"}).out == body);
    const auto found = box.run({"find", "kelime"});
    CHECK(found.out.find(name) != std::string::npos);
    CHECK(found.out.find("a" + rlo + "b\xE2\x80\x8B") != std::string::npos);
    CHECK(found.out.find(marker) == std::string::npos);
}
#endif
