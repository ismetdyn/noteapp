// Geçiş ve dışa aktarma uçtan uca (G6): gerçek `note` ikilisi, eski düzen klasörü.
#include "core/migration.hpp"
#include "core/note_store.hpp"
#include "graph/graph.hpp"
#include "support/cli_sandbox.hpp"
#include "support/note_seed.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

void seed_old_layout(const Sandbox& box) {
    write_plain(box.notes() / "plan.md", "plan\n");
    write_plain(box.notes() / "work" / "crlf.txt", "a\r\nb\r\n");
    write_plain(box.notes() / "work" / "ham.bin", exact_bytes());
    write_plain(box.notes() / "work" / "alt" / "uzantisiz", "uzantisiz\n");
    write_plain(box.notes() / "bos" / ".keep", "");
    write_plain(box.notes() / ".trash" / "eski.md", "çöp\n");
    write_plain(box.notes() / ".git" / "config", "[core]\n");  // not değil: gizli
    write_plain(box.notes() / ".gizli", "not değil");
}

}  // namespace

TEST_CASE("old-layout notes and no graph: every write is refused with a clear message, reads say why") {
    const Sandbox box("en");
    seed_old_layout(box);

    const auto add = box.run({"x"});
    CHECK(add.exit_code != 0);
    CHECK(add.err.find("note graph migrate") != std::string::npos);
    CHECK_FALSE(box.has_any_note());

    const auto list = box.run({"ls"});
    CHECK(list.err.find("note graph migrate") != std::string::npos);
    CHECK(tree_of(box.notes()).at("plan.md") == "plan\n");
}

TEST_CASE("migrate imports byte for byte, backs up first, keeps old files and tells how to remove them") {
    const Sandbox box("en");
    seed_old_layout(box);
    const auto before = tree_of(box.notes());

    const auto migrated = box.run({"graph", "migrate"});
    REQUIRE_MESSAGE(migrated.exit_code == 0, migrated.err);
    CHECK(migrated.out.find("Migrated 5 notes") != std::string::npos);
    CHECK(migrated.out.find("NOT deleted") != std::string::npos);
    CHECK(tree_of(box.notes()) == before);  // eski veriye dokunulmadı

    CHECK(box.body("work/ham.bin") == exact_bytes());
    CHECK(box.body("work/crlf.txt") == "a\r\nb\r\n");
    CHECK(box.body("work/alt/uzantisiz") == "uzantisiz\n");
    CHECK(note_in_trash(box.data(), "eski.md"));
    CHECK(folder_exists(box.data(), "bos"));

    const noteapp::core::NoteStore store(box.data());
    for (const noteapp::core::NoteMeta& meta : store.list_meta())
        CHECK(meta.origin == "unknown");

    // Yedek, baytlarıyla.
    bool found_backup = false;
    for (const auto& entry : fs::directory_iterator(box.data()))
        if (entry.path().filename().string().starts_with("migration-backup-")) {
            found_backup = true;
            CHECK(slurp(entry.path() / "notes" / "work" / "ham.bin") == exact_bytes());
            CHECK(slurp(entry.path() / "trash" / "eski.md") == "çöp\n");
        }
    CHECK(found_backup);

    // Kapı açıldı.
    const auto add = box.run({"-n", "yeni", "metin"});
    CHECK_MESSAGE(add.exit_code == 0, add.err);
}

TEST_CASE("migrate twice changes nothing: same notes, no new operations") {
    const Sandbox box("en");
    seed_old_layout(box);
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    const auto first = box.run({"graph", "export", "--format", "jsonl"});
    REQUIRE(first.exit_code == 0);
    const auto again = box.run({"graph", "migrate"});
    REQUIRE_MESSAGE(again.exit_code == 0, again.err);
    CHECK(again.out.find("Migrated 0 notes (5 already there)") != std::string::npos);
    CHECK(box.run({"graph", "export", "--format", "jsonl"}).out == first.out);
}

// L4b: çerçeve not boyutunu sınırlamaz; eskiden atlanan 60 MiB üstü dosya artık bayt bayt içe alınır.
TEST_CASE("a file over 60 MiB, larger than one frame, is migrated byte for byte") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    const fs::path huge = box.notes() / "dev.bin";
    // resize_file MinGW + Wine'da çöküyor (mingw-crt checkfreespace): gerçekten yazılır.
    const std::string bytes(std::size_t{65} * 1024U * 1024U, 'x');
    write_plain(huge, bytes);

    const auto migrated = box.run({"graph", "migrate"});
    CHECK_MESSAGE(migrated.exit_code == 0, migrated.err);
    CHECK(fs::file_size(huge) == bytes.size());
    CHECK(box.body("iyi.md") == "iyi\n");
    CHECK((box.body("dev.bin") == bytes));  // çift parantez: 65 MiB değer iletiye basılmasın
    CHECK(box.run({"-n", "n", "metin"}).exit_code == 0);
}

// L4b'den önce 61 MiB'lik dosya da dışarıda kalıyordu; artık yalnız depolama sınırını (1 GB) aşan dosya
// `too_large`dır, testte üretilemez. Dışarıda kalan dosya geçersiz addır: Windows'ta `nul` aygıttır, böyle
// bir dosya oluşamaz; bu yüzden test yalnız POSIX'te koşar.
#ifndef _WIN32
TEST_CASE("files the migration left out stay visible until resolved: status and export --all exit 1") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    constexpr const char* kLeftOutSummary = "NOT migrated (1,";
    write_plain(box.notes() / "ham" / "nul.bin", "x");
    const TempDir out;

    const auto clean_before = box.run({"graph", "status"});
    CHECK(clean_before.exit_code == 1);  // henüz geçirilmedi: "kalan yok" denemez
    CHECK(clean_before.err.find("not migrated yet") != std::string::npos);

    const auto migrated = box.run({"graph", "migrate"});
    CHECK_MESSAGE(migrated.exit_code == 0, migrated.err);
    CHECK(migrated.err.find(kLeftOutSummary) != std::string::npos);
    CHECK(migrated.err.find("do not delete") != std::string::npos);

    const auto listed = [&](const RunResult& result) {
        return result.err.find("nul.bin") != std::string::npos &&
               result.err.find("NOT in any export") != std::string::npos;
    };
    const auto exported = box.run({"export", "--all", utf8(out.path() / "a")});
    CHECK(exported.exit_code == 1);
    CHECK(listed(exported));
    CHECK(slurp(out.path() / "a" / "iyi.md") == "iyi\n");  // sağlam notlar yine çıkar
    CHECK_FALSE(fs::exists(out.path() / "a" / "ham" / "nul.bin"));

    const auto accepted = box.run({"export", "--all", utf8(out.path() / "b"), "--ignore-skipped"});
    CHECK(accepted.exit_code == 0);
    CHECK(listed(accepted));  // kabul de adları gizlemez

    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(listed(status));
    const auto ignored = box.run({"graph", "status", "--ignore-skipped"});
    CHECK(ignored.exit_code == 0);
    CHECK(listed(ignored));
    // Liste yazıldıysa "hiçbir şey kalmadı" yalan olur: dosyalar hâlâ veritabanında değil.
    CHECK(ignored.out.find("Nothing was left out") == std::string::npos);
    CHECK(ignored.out.find("NOT in the database") != std::string::npos);
    CHECK(box.run({"graph", "status", "--yok"}).exit_code == 2);
    CHECK(box.run({"export", "--pdf", "iyi", "--ignore-skipped"}).exit_code == 2);

    // Yeni süreçte de bilir; sorun çözülüp geçiş yinelenince liste boşalır.
    fs::remove_all(box.notes() / "ham");
    const auto again = box.run({"graph", "migrate"});
    CHECK_MESSAGE(again.exit_code == 0, again.err);
    CHECK(again.err.find("NOT migrated") == std::string::npos);
    const auto clean = box.run({"graph", "status"});
    CHECK_MESSAGE(clean.exit_code == 0, clean.err);
    CHECK(clean.out.find("Nothing was left out") != std::string::npos);
    CHECK(box.run({"export", "--all", utf8(out.path() / "c")}).exit_code == 0);
}
#endif

TEST_CASE("a damaged list of left-out files fails loudly instead of saying nothing was skipped") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.data() / "migration-skipped", "bozuk satir\n");
    CHECK(box.run({"graph", "status"}).exit_code == 3);
    const TempDir out;
    CHECK(box.run({"export", "--all", utf8(out.path() / "a"), "--ignore-skipped"}).exit_code == 3);
}

TEST_CASE("status covers the current notes_dir only; --clear-skipped forgets the list, not the files") {
    const Sandbox box("en");
    const TempDir outside;
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(make_dir_link(outside.path(), box.notes() / "bag"));
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    const fs::path list = box.data() / "migration-skipped";
    // Ad öneki paylaşan kardeş dizin de başka köktür.
    const std::string sibling = utf8(fs::weakly_canonical(box.notes())) + "-eski/kardes.bin";
    write_plain(list, slurp(list) + "too_large\t/baska/kok/dev.bin\ntoo_large\t" + sibling + "\n");

    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(status.err.find("bag") != std::string::npos);
    CHECK(status.err.find("dev.bin") == std::string::npos);
    CHECK(status.err.find("kardes.bin") == std::string::npos);
    CHECK(status.err.find("(2)") != std::string::npos);
    CHECK(status.err.find("--clear-skipped") != std::string::npos);

    CHECK(box.run({"graph", "status", "--clear-skipped", "--ignore-skipped"}).exit_code == 2);
    CHECK(fs::exists(list));

    const auto cleared = box.run({"graph", "status", "--clear-skipped"});
    CHECK_MESSAGE(cleared.exit_code == 0, cleared.err);
    CHECK(cleared.err.find("dev.bin") != std::string::npos);  // unutulmadan önce adları yazılır
    CHECK(cleared.out.find("(3)") != std::string::npos);
    CHECK_FALSE(fs::exists(list));
    CHECK(fs::exists(box.notes() / "bag"));  // özgünlere dokunulmadı
    CHECK(slurp(box.notes() / "iyi.md") == "iyi\n");
    CHECK(box.run({"graph", "status"}).exit_code == 0);
    CHECK(box.run({"graph", "status", "--clear-skipped"}).exit_code == 0);  // tekrar etkisiz

    // Geçiş yinelenince dosyalar yine listelenir: temizlemek dosyayı geçirilmiş saymaz.
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    CHECK(box.run({"graph", "status"}).exit_code == 1);
}

TEST_CASE("another notes_dir's records alone do not fail status") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.data() / "migration-skipped", "too_large\t/baska/kok/dev.bin\n");
    const auto status = box.run({"graph", "status"});
    CHECK_MESSAGE(status.exit_code == 0, status.err);
    CHECK(status.err.find("(1)") != std::string::npos);
    CHECK(box.run({"help", "graph"}).out.find("--clear-skipped") != std::string::npos);
}

TEST_CASE("link in the old tree is never followed and is reported") {
    const Sandbox box("en");
    const TempDir outside;
    write_plain(outside.path() / "gizli.md", "gizli");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(make_dir_link(outside.path(), box.notes() / "bag"));

    const auto migrated = box.run({"graph", "migrate"});
    REQUIRE_MESSAGE(migrated.exit_code == 0, migrated.err);
    CHECK(migrated.err.find("bag") != std::string::npos);
    CHECK_FALSE(box.exists("bag/gizli.md"));
    CHECK(box.body("iyi.md") == "iyi\n");
}

TEST_CASE("note export --all output is byte-identical to the originals") {
    const Sandbox box("en");
    seed_old_layout(box);
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    const TempDir out;
    const fs::path target = out.path() / "cikti";
    const auto exported = box.run({"export", "--all", utf8(target)});
    REQUIRE_MESSAGE(exported.exit_code == 0, exported.err);

    auto expected = tree_of(box.notes());
    expected.erase(".git/config");
    expected.erase(".gizli");
    expected.erase("bos/.keep");
    CHECK(tree_of(target) == expected);
    CHECK(fs::is_directory(target / "bos"));
}

TEST_CASE(
    "old trash in a subfolder creates no live folder: the export tree is exact, restore brings it back") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    write_plain(box.notes() / ".trash" / "eski" / "alt" / "cop.md", "cop\n");
    const auto migrated = box.run({"graph", "migrate"});
    REQUIRE_MESSAGE(migrated.exit_code == 0, migrated.err);
    CHECK_FALSE(folder_exists(box.data(), "eski"));
    CHECK(note_in_trash(box.data(), "eski/alt/cop.md"));

    const TempDir out;
    const auto exported = box.run({"export", "--all", utf8(out.path() / "c")});
    REQUIRE_MESSAGE(exported.exit_code == 0, exported.err);
    std::set<std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(out.path() / "c"))
        entries.insert(entry.path().lexically_relative(out.path() / "c").generic_string());
    const std::set<std::string> exact{"iyi.md", ".trash", ".trash/eski", ".trash/eski/alt",
                                      ".trash/eski/alt/cop.md"};
    CHECK(entries == exact);
    CHECK(slurp(out.path() / "c" / ".trash" / "eski" / "alt" / "cop.md") == "cop\n");

    noteapp::core::NoteStore store(box.data());
    const auto item = noteapp::core::NotePath::parse("eski/alt/cop.md");
    REQUIRE(item.has_value());
    const auto restored = store.restore(*item);
    REQUIRE(restored.has_value());
    CHECK(restored->string() == "eski/alt/cop.md");
    CHECK(store.read(*restored)->text == "cop\n");
    CHECK(folder_exists(box.data(), "eski/alt"));
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

TEST_CASE("note export --all also writes the trash under .trash; old trash comes back byte for byte") {
    const Sandbox box("en");
    seed_old_layout(box);
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
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
    REQUIRE(box.run({"query", "--write", "--param",
                     "d=etiket\xC2\x9D"
                     "52;c;ZXZpbA==\xC2\x9C",
                     "MATCH (t:Tag) SET t.display = $d"})
                .exit_code == 0);
    REQUIRE(box.run({"tags"}).out.find("\xC2\x9D") != std::string::npos);  // boru: etiket bayt bayt

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

#ifdef __linux__
#include <grp.h>
#include <sys/stat.h>

namespace {

struct Unprivileged {
    int exit_code = -1;
    std::string err;
};

// chmod 000 kökte işe yaramaz (konteyner): dosya gerçekten okunamasın diye süreç yetkisiz koşar.
Unprivileged run_unprivileged(const Sandbox& box, const std::vector<std::string>& args) {
    constexpr uid_t kNobody = 65534;
    const fs::path home = box.notes().parent_path();
    if (geteuid() == 0) {
        REQUIRE(chown(home.c_str(), kNobody, kNobody) == 0);
        for (const auto& entry : fs::recursive_directory_iterator(home))
            REQUIRE(lchown(entry.path().c_str(), kNobody, kNobody) == 0);
    }
    const fs::path err_file = fs::temp_directory_path() / ("noteapp-err-" + std::to_string(getpid()));
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        if (geteuid() == 0 && (setgroups(0, nullptr) != 0 || setgid(kNobody) != 0 || setuid(kNobody) != 0))
            _exit(126);
        const int fd = open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        dup2(fd, 2);
        std::vector<char*> argv;
        std::string exe = NOTEAPP_NOTE_EXE;
        argv.push_back(exe.data());
        std::vector<std::string> copy = args;
        for (std::string& arg : copy)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    Unprivileged result{WIFEXITED(status) ? WEXITSTATUS(status) : -1, slurp(err_file)};
    fs::remove(err_file);
    return result;
}

}  // namespace

TEST_CASE(
    "an unreadable file stops migration by default; --skip-failed names it, keeps it and opens the gate") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    const fs::path bad = box.notes() / "okunamaz.md";
    write_plain(bad, "x");
    fs::permissions(bad, fs::perms::none, fs::perm_options::replace);

    const auto failed = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(failed.exit_code == 1, failed.err);
    CHECK(failed.err.find("NOT migrated: okunamaz.md") != std::string::npos);
    CHECK(run_unprivileged(box, {"-n", "n", "metin"}).exit_code != 0);  // kapı kapalı

    const auto skipped = run_unprivileged(box, {"graph", "migrate", "--skip-failed"});
    CHECK_MESSAGE(skipped.exit_code == 0, skipped.err);
    CHECK(skipped.err.find("NOT migrated: okunamaz.md") != std::string::npos);
    CHECK(fs::exists(bad));
    CHECK(run_unprivileged(box, {"-n", "n", "metin"}).exit_code == 0);
    const auto status = run_unprivileged(box, {"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(status.err.find("okunamaz.md") != std::string::npos);
    CHECK(box.run({"graph", "migrate", "--yok"}).exit_code == 2);
}

// L3: derinlik sınırı yok; geçişi durduran klasör yolunun baytıdır (limits.core.folder_path_bytes). Sınır
// küçültülür: OS'un yol sınırı varsayılanı (4096) aşan dosyayı zaten oluşturtmaz.
TEST_CASE(
    "a file whose folder path is over the store limit stops migration; --skip-failed names it, keeps it, "
    "opens the gate") {
    const Sandbox box("en");
    REQUIRE(box.run({"config", "set", "limits.core.folder_path_bytes", "100"}).exit_code == 0);
    const std::string deep =
        std::string(60, 'k') + "/" + std::string(60, 'l') + "/derin.md";  // klasör 121 bayt
    write_plain(box.notes() / deep, "derin\n");
    write_plain(box.notes() / "iyi.md", "iyi\n");

    const auto failed = box.run({"graph", "migrate"});
    CHECK_MESSAGE(failed.exit_code == 1, failed.err);
    CHECK(failed.err.find("NOT migrated: " + deep) != std::string::npos);
    CHECK(failed.err.find("folder path is too long") != std::string::npos);
    CHECK(failed.err.find("NOT migrated: iyi.md") == std::string::npos);

    const auto skipped = box.run({"graph", "migrate", "--skip-failed"});
    CHECK_MESSAGE(skipped.exit_code == 0, skipped.err);
    CHECK(fs::exists(box.notes() / deep));
    CHECK(box.run({"-n", "n", "metin"}).exit_code == 0);
}

TEST_CASE("an unreadable file left in place: fixing it and migrating again finishes the import") {
    const Sandbox box("en");
    const fs::path bad = box.notes() / "okunamaz.md";
    write_plain(bad, "x\n");
    fs::permissions(bad, fs::perms::none, fs::perm_options::replace);
    CHECK(run_unprivileged(box, {"graph", "migrate"}).exit_code == 1);
    fs::permissions(bad, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto done = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(done.exit_code == 0, done.err);
    CHECK(box.body("okunamaz.md") == "x\n");
}

namespace {

// Test düşse de izin geri gelir: yetkisiz koşuda TempDir kilitli dizini silemezdi.
class ScopedMode {
public:
    ScopedMode(fs::path path, fs::perms mode) : path_(std::move(path)) {
        fs::permissions(path_, mode, fs::perm_options::replace);
    }
    ScopedMode(const ScopedMode&) = delete;
    ScopedMode& operator=(const ScopedMode&) = delete;
    ~ScopedMode() { restore(); }
    void restore() {
        std::error_code ignored;
        fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace, ignored);
    }

private:
    fs::path path_;
};

}  // namespace

TEST_CASE(
    "an unreadable folder is never silent: migrate, status and export --all name it until it is fixed") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    write_plain(box.notes() / "kilitli" / "degerli.md", "degerli\n");
    ScopedMode locked(box.notes() / "kilitli", fs::perms::none);
    const std::string named = "/kilitli (this folder cannot be read";  // kayıtlı liste tam yol yazar
    const std::string relative_name = "NOT migrated: kilitli (this folder cannot be read";

    const auto failed = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(failed.exit_code == 1, failed.err);
    CHECK(failed.err.find(relative_name) != std::string::npos);
    CHECK(run_unprivileged(box, {"-n", "n", "metin"}).exit_code != 0);  // kapı kapalı

    const auto skipped = run_unprivileged(box, {"graph", "migrate", "--skip-failed"});
    CHECK_MESSAGE(skipped.exit_code == 0, skipped.err);
    CHECK(skipped.err.find(relative_name) != std::string::npos);

    const auto status = run_unprivileged(box, {"graph", "status"});
    CHECK_MESSAGE(status.exit_code == 1, status.err);
    CHECK(status.err.find(named) != std::string::npos);
    const fs::path out = box.notes().parent_path() / "cikti";
    const auto exported = run_unprivileged(box, {"export", "--all", utf8(out)});
    CHECK_MESSAGE(exported.exit_code == 1, exported.err);
    CHECK(exported.err.find(named) != std::string::npos);

    locked.restore();
    const auto done = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(done.exit_code == 0, done.err);
    const auto clean = run_unprivileged(box, {"graph", "status"});
    CHECK_MESSAGE(clean.exit_code == 0, clean.err);
    CHECK(box.body("kilitli/degerli.md") == "degerli\n");
}

TEST_CASE(
    "a tree holding only an unreadable folder still counts as old notes: status does not say all clear") {
    const Sandbox box("en");
    write_plain(box.notes() / "kilitli" / "degerli.md", "degerli\n");
    const ScopedMode locked(box.notes() / "kilitli", fs::perms::none);
    const auto status = run_unprivileged(box, {"graph", "status"});
    CHECK_MESSAGE(status.exit_code == 1, status.err);
    CHECK(status.err.find("not migrated yet") != std::string::npos);
}

TEST_CASE("a folder that lists but cannot be entered names each entry; the unknown type is not guessed") {
    const Sandbox box("en");
    write_plain(box.notes() / "yalniz-liste" / "degerli.md", "degerli\n");
    const ScopedMode listed(box.notes() / "yalniz-liste", fs::perms::owner_read);
    const auto failed = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(failed.exit_code == 1, failed.err);
    CHECK(failed.err.find("NOT migrated: yalniz-liste/degerli.md (it cannot be read)") != std::string::npos);
}

TEST_CASE("unreadable old trash is reported under its real .trash path") {
    const Sandbox box("en");
    write_plain(box.notes() / ".trash" / "kilitli" / "cop.md", "cop\n");
    write_plain(box.notes() / ".trash" / "okunamaz.md", "cop\n");
    fs::permissions(box.notes() / ".trash" / "okunamaz.md", fs::perms::none, fs::perm_options::replace);
    {
        const ScopedMode folder(box.notes() / ".trash" / "kilitli", fs::perms::none);
        const auto inner = run_unprivileged(box, {"graph", "migrate"});
        CHECK_MESSAGE(inner.exit_code == 1, inner.err);
        CHECK(inner.err.find("NOT migrated: .trash/kilitli (this folder") != std::string::npos);
        CHECK(inner.err.find("NOT migrated: .trash/okunamaz.md (it cannot be read)") != std::string::npos);
    }
    const ScopedMode whole(box.notes() / ".trash", fs::perms::none);
    const auto root = run_unprivileged(box, {"graph", "migrate"});
    CHECK_MESSAGE(root.exit_code == 1, root.err);
    CHECK(root.err.find("NOT migrated: .trash (this folder") != std::string::npos);
}
#endif

namespace {

std::size_t backup_count(const fs::path& data) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(data))
        if (entry.path().filename().string().starts_with("migration-backup-"))
            ++count;
    return count;
}

}  // namespace

TEST_CASE("a file dropped after migration: status lists it, exits 1 and hints; carriers and .git are not") {
    const Sandbox box("en");
    seed_old_layout(box);
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.notes() / "graph" / (std::string(32, 'a') + ".oplog"), "carrier");
    write_plain(box.notes() / ".git" / "HEAD", "ref\n");
    const auto clean = box.run({"graph", "status"});
    CHECK_MESSAGE(clean.exit_code == 0, clean.err);

    write_plain(box.notes() / "denee.txt", "yeni\n");
    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(status.err.find("denee.txt") != std::string::npos);
    CHECK(status.err.find("note graph migrate --new") != std::string::npos);
    CHECK(status.err.find("plan.md") == std::string::npos);  // geçmiş dosya listelenmez
    CHECK(status.err.find(".oplog") == std::string::npos);
    CHECK(status.err.find("HEAD") == std::string::npos);
    CHECK(status.out.find("Nothing was left out") == std::string::npos);
    CHECK_FALSE(box.exists("denee.txt"));  // status yalnız okur
}

TEST_CASE("status does not list an old file whose migrated note was edited or renamed afterwards") {
    const Sandbox box("en");
    write_plain(box.notes() / "plan.md", "plan\n");
    write_plain(box.notes() / "tasinan.md", "tasi\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    box.put("plan.md", "plan duzenlendi\n");
    REQUIRE(box.run({"mv", "tasinan.md", "yeni-ad.md"}).exit_code == 0);
    const auto status = box.run({"graph", "status"});
    CHECK_MESSAGE(status.exit_code == 0, status.err);
}

TEST_CASE("migrate --new imports only the new files, keeps them, reports each and is idempotent") {
    const Sandbox box("en");
    seed_old_layout(box);

    const auto premature = box.run({"graph", "migrate", "--new"});  // önce tam geçiş
    CHECK(premature.exit_code == 1);
    CHECK(premature.err.find("not migrated yet") != std::string::npos);
    CHECK_FALSE(box.exists("plan.md"));

    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    box.put("plan.md", "plan duzenlendi\n");
    write_plain(box.notes() / "denee.txt", "yeni\n");
    write_plain(box.notes() / "yeni-klasor" / "alt.md", "alt [[denee.txt]]\n");
    const auto before = tree_of(box.notes());

    const auto imported = box.run({"graph", "migrate", "--new"});
    REQUIRE_MESSAGE(imported.exit_code == 0, imported.err);
    CHECK(imported.out.find("denee.txt") != std::string::npos);
    CHECK(imported.out.find("yeni-klasor/alt.md") != std::string::npos);
    CHECK(imported.out.find("plan.md") == std::string::npos);
    CHECK(imported.out.find("NOT deleted") != std::string::npos);
    CHECK(tree_of(box.notes()) == before);  // dosyalar yerinde
    CHECK(box.body("denee.txt") == "yeni\n");
    CHECK(box.body("yeni-klasor/alt.md") == "alt [[denee.txt]]\n");
    CHECK(box.body("plan.md") == "plan duzenlendi\n");  // düzenleme ezilmez
    CHECK(box.run({"graph", "status"}).exit_code == 0);

    const auto first = box.run({"graph", "export", "--format", "jsonl"});
    const std::size_t backups = backup_count(box.data());
    const auto again = box.run({"graph", "migrate", "--new"});
    REQUIRE_MESSAGE(again.exit_code == 0, again.err);
    CHECK(again.out.find("denee.txt") == std::string::npos);
    CHECK(box.run({"graph", "export", "--format", "jsonl"}).out == first.out);
    CHECK(backup_count(box.data()) == backups);  // yeni dosya yoksa yedek de yok
    CHECK(box.run({"graph", "migrate", "--new", "--skip-failed"}).exit_code == 2);
}

TEST_CASE("an old file changed after migration becomes a conflict copy, the migrated note stays") {
    const Sandbox box("en");
    write_plain(box.notes() / "plan.md", "plan\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.notes() / "plan.md", "degisti\n");

    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(status.err.find("plan.md") != std::string::npos);

    const auto imported = box.run({"graph", "migrate", "--new"});
    REQUIRE_MESSAGE(imported.exit_code == 0, imported.err);
    CHECK(box.body("plan.md") == "plan\n");
    const auto notes_of = [&] { return noteapp::core::NoteStore(box.data()).list_meta(); };
    std::size_t copies = 0;
    for (const noteapp::core::NoteMeta& meta : notes_of())
        if (meta.path.string() != "plan.md") {
            ++copies;
            CHECK(box.body(meta.path.string()) == "degisti\n");
            CHECK(imported.out.find(meta.path.string()) != std::string::npos);
        }
    CHECK(copies == 1);
    CHECK(box.run({"graph", "status"}).exit_code == 0);
    CHECK(box.run({"graph", "migrate", "--new"}).exit_code == 0);
    CHECK(notes_of().size() == 2);
}

TEST_CASE("a note made in the app at the same path: same bytes is known, other bytes overwrite nothing") {
    const Sandbox box("en");
    write_plain(box.notes() / "plan.md", "plan\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    box.put("ayni.md", "ayni\n");
    box.put("farkli.md", "uygulamadaki\n");
    write_plain(box.notes() / "ayni.md", "ayni\n");
    const auto same = box.run({"graph", "status"});
    CHECK_MESSAGE(same.exit_code == 0, same.err);

    write_plain(box.notes() / "farkli.md", "dosyadaki\n");
    CHECK(box.run({"graph", "status"}).exit_code == 1);
    REQUIRE(box.run({"graph", "migrate", "--new"}).exit_code == 0);
    std::multiset<std::string> bodies;
    for (const noteapp::core::NoteMeta& meta : noteapp::core::NoteStore(box.data()).list_meta())
        bodies.insert(box.body(meta.path.string()));
    CHECK(bodies.count("uygulamadaki\n") == 1);
    CHECK(bodies.count("dosyadaki\n") == 1);
    CHECK(box.run({"graph", "status"}).exit_code == 0);
}

// L4b: 61 MiB artık içe alınır; dışarıda kalan yeni dosya geçersiz addır (Windows'ta `nul` oluşamaz).
#ifndef _WIN32
TEST_CASE("new files over the limits: status names them, --new skips and records them like migrate") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.notes() / "ham" / "nul.bin", "x");
    write_plain(box.notes() / "sonra.md", "sonra\n");

    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK(status.err.find("nul.bin") != std::string::npos);
    CHECK(status.err.find("sonra.md") != std::string::npos);

    const auto imported = box.run({"graph", "migrate", "--new"});
    CHECK_MESSAGE(imported.exit_code == 0, imported.err);
    CHECK(imported.err.find("nul.bin") != std::string::npos);
    CHECK(box.body("sonra.md") == "sonra\n");
    CHECK(fs::file_size(box.notes() / "ham" / "nul.bin") == 1);

    const auto recorded = box.run({"graph", "status"});  // kayıtlı: bir kez, eski yoldan
    CHECK(recorded.exit_code == 1);
    CHECK(recorded.err.find("NOT in any export") != std::string::npos);
    CHECK(recorded.err.find("--new") == std::string::npos);
    CHECK(box.run({"graph", "status", "--ignore-skipped"}).exit_code == 0);
}

// L2: status ve migrate --new deponun config'teki klasör yolu sınırını kullanır (eskiden hep 4096).
TEST_CASE("status and migrate --new use the configured folder path limit") {
    const Sandbox box("en");
    write_plain(box.notes() / "iyi.md", "iyi\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    REQUIRE(box.run({"config", "set", "limits.core.folder_path_bytes", "100"}).exit_code == 0);
    const std::string deep = std::string(60, 'k') + "/" + std::string(60, 'l') + "/derin.md";  // klasör 121 bayt
    write_plain(box.notes() / deep, "derin\n");

    const auto status = box.run({"graph", "status"});
    CHECK(status.exit_code == 1);
    CHECK_MESSAGE(status.err.find("folder path is too long") != std::string::npos, status.err);

    // migrate gibi: dosya içe alınmaz, yerinde kalır, bir kez ve nedeniyle raporlanır.
    const auto imported = box.run({"graph", "migrate", "--new"});
    CHECK_MESSAGE(imported.exit_code == 1, imported.err);
    CHECK(imported.err.find("folder path is too long") != std::string::npos);
    CHECK(imported.err.find("saved bytes differ") == std::string::npos);
    CHECK_FALSE(box.exists(deep));
    CHECK(fs::exists(box.notes() / deep));
}
#endif

TEST_CASE("the stray scan stops at its limits and says so, and sees the old trash too") {
    using noteapp::core::find_stray_files;
    const Sandbox box("en");
    write_plain(box.notes() / "a.md", "aaaa\n");
    write_plain(box.notes() / "b.md", "bbbb\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.notes() / ".trash" / "yeni.md", "cop\n");
    const noteapp::core::NoteStore store(box.data());

    const auto whole = find_stray_files(store, box.notes(), box.data());
    REQUIRE(whole.has_value());
    CHECK_FALSE(whole->truncated);
    REQUIRE(whole->files.size() == 1);
    CHECK(whole->files.front().path == ".trash/yeni.md");
    CHECK(whole->files.front().reason == "new_file");

    const auto by_entries = find_stray_files(store, box.notes(), box.data(), {.max_entries = 1});
    REQUIRE(by_entries.has_value());
    CHECK(by_entries->truncated);
    const auto by_bytes = find_stray_files(store, box.notes(), box.data(), {.max_bytes = 3});
    REQUIRE(by_bytes.has_value());
    CHECK(by_bytes->truncated);
    CHECK(by_bytes->files.empty());
    std::stop_source quitting;
    quitting.request_stop();
    const auto stopped = find_stray_files(store, box.notes(), box.data(), {.stop = quitting.get_token()});
    REQUIRE(stopped.has_value());
    CHECK(stopped->truncated);
    CHECK(stopped->files.empty());

    REQUIRE(box.run({"graph", "migrate", "--new"}).exit_code == 0);
    CHECK(note_in_trash(box.data(), "yeni.md"));
    CHECK(box.run({"graph", "status"}).exit_code == 0);
}

TEST_CASE("a migrated note deleted or purged in the app is not stray and --new does not bring it back") {
    using noteapp::core::NotePath;
    const Sandbox box("en");
    write_plain(box.notes() / "silinen.md", "sir\n");
    write_plain(box.notes() / "atilan.md", "gizli\n");
    write_plain(box.notes() / "kalan.md", "kalan\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    write_plain(box.notes() / "sonradan.md", "sonra\n");
    REQUIRE(box.run({"graph", "migrate", "--new"}).exit_code == 0);
    {
        noteapp::core::NoteStore store(box.data());
        REQUIRE(store.remove(NotePath::parse("silinen.md").value()).has_value());
        REQUIRE(store.trash(NotePath::parse("atilan.md").value()).has_value());
        REQUIRE(store.purge(NotePath::parse("atilan.md").value()).has_value());
        REQUIRE(store.remove(NotePath::parse("sonradan.md").value()).has_value());
    }
    const auto status = box.run({"graph", "status"});
    CHECK_MESSAGE(status.exit_code == 0, status.err);

    const auto again = box.run({"graph", "migrate", "--new"});
    CHECK_MESSAGE(again.exit_code == 0, again.err);
    CHECK_FALSE(box.exists("silinen.md"));
    CHECK_FALSE(box.exists("atilan.md"));
    CHECK_FALSE(box.exists("sonradan.md"));
    CHECK(noteapp::core::NoteStore(box.data()).list_meta().size() == 1);

    write_plain(box.notes() / "silinen.md", "sir, sonra degisti\n");  // içe alınandan farklı: yine görünür
    const auto edited = box.run({"graph", "status"});
    CHECK(edited.exit_code == 1);
    CHECK(edited.err.find("silinen.md") != std::string::npos);
#ifndef _WIN32
    CHECK((fs::status(box.data() / "migrated-digests").permissions() & fs::perms::mask) ==
          (fs::perms::owner_read | fs::perms::owner_write));
#endif
}

#ifndef _WIN32
TEST_CASE("migrated-notes-dirs is private, also when an older version left it readable") {
    const auto mode = [](const fs::path& path) { return fs::status(path).permissions() & fs::perms::mask; };
    const auto private_file = fs::perms::owner_read | fs::perms::owner_write;
    const Sandbox box("en");
    write_plain(box.notes() / "plan.md", "plan\n");
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    const fs::path marker = box.data() / "migrated-notes-dirs";
    CHECK(mode(marker) == private_file);

    fs::permissions(marker, private_file | fs::perms::group_read | fs::perms::others_read);
    REQUIRE(box.run({"graph", "migrate"}).exit_code == 0);
    CHECK(mode(marker) == private_file);
}
#endif
