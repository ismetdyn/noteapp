// `note import PATH...`: dosyayı ya da klasörü not olarak içe alır (app::import_paths). Çıkış 0 hepsi alındı,
// 1 atlanan ya da alınamayan var, 2 kullanım.
#include "core/json_text.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <string>

using namespace noteapp::test;

namespace {

fs::path write_file(const TempDir& dir, const std::string& name, const std::string& bytes) {
    const fs::path file = dir.path() / name;
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
    return file;
}

std::string json_string(const std::string& text) {
    std::string out;
    noteapp::core::append_json_string(out, text);
    return out;
}

}  // namespace

TEST_CASE("import adds a file as a note; show prints its exact bytes, CRLF and BOM included") {
    const Sandbox box;
    const TempDir files;
    const std::string bytes = "\xEF\xBB\xBFsatır bir\r\nsatır iki\r\n";
    const fs::path file = write_file(files, "notlar.txt", bytes);
    const auto result = box.run({"import", utf8(file)});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(result.out.find("notlar.txt") != std::string::npos);
    const auto shown = box.run({"show", "notlar.txt"});
    CHECK(shown.exit_code == 0);
    CHECK(shown.out == bytes);
    CHECK(slurp(file) == bytes);
}

TEST_CASE("import of a binary file keeps it, and show refuses to print it") {
    const Sandbox box;
    const TempDir files;
    const std::string bytes("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
    REQUIRE(box.run({"import", utf8(write_file(files, "logo.png", bytes))}).exit_code == 0);
    CHECK(box.body("logo.png") == bytes);
    CHECK(box.run({"show", "logo.png"}).exit_code == 1);
}

TEST_CASE("import of a folder keeps its tree under --folder") {
    const Sandbox box;
    const TempDir files;
    write_file(files, "proje/a.md", "a");
    write_file(files, "proje/alt/b.md", "b");
    write_file(files, "proje/.git/HEAD", "ref");
    const auto result = box.run({"import", utf8(files.path() / "proje"), "--folder", "arsiv"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(box.body("arsiv/proje/a.md") == "a");
    CHECK(box.body("arsiv/proje/alt/b.md") == "b");
    CHECK_FALSE(box.exists("arsiv/proje/.git/HEAD"));
}

TEST_CASE("import numbers a taken name and --overwrite replaces it") {
    const Sandbox box;
    const TempDir files;
    box.put("plan.md", "eski");
    const fs::path file = write_file(files, "plan.md", "yeni");
    CHECK(box.run({"import", utf8(file)}).exit_code == 0);
    CHECK(box.body("plan.md") == "eski");
    CHECK(box.body("plan-2.md") == "yeni");
    write_file(files, "plan.md", "en yeni");
    CHECK(box.run({"import", "--overwrite", utf8(file)}).exit_code == 0);
    CHECK(box.body("plan.md") == "en yeni");
    CHECK_FALSE(box.exists("plan-3.md"));
}

TEST_CASE("import --name names a single file") {
    const Sandbox box;
    const TempDir files;
    CHECK(box.run({"import", utf8(write_file(files, "x.txt", "x")), "--name", "kayit.md"}).exit_code == 0);
    CHECK(box.body("kayit.md") == "x");
}

TEST_CASE("import of a missing path exits 1 with a message; the rest is still imported") {
    const Sandbox box;
    const TempDir files;
    const fs::path missing = files.path() / "yok.md";
    const auto result = box.run({"import", utf8(missing), utf8(write_file(files, "var.md", "v"))});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find(utf8(missing)) != std::string::npos);
    CHECK(box.body("var.md") == "v");
}

TEST_CASE("import --json prints imported, skipped, failed and hidden") {
    const Sandbox box;
    const TempDir files;
    const fs::path file = write_file(files, "plan.md", "p");
    const auto ok = box.run({"import", "--json", utf8(file)});
    INFO(ok.err);
    CHECK(ok.exit_code == 0);
    CHECK(ok.out == "{\"imported\":[{\"source\":" + json_string(utf8(file)) +
                        ",\"note\":\"plan.md\",\"renamed\":false,\"overwritten\":false}],"
                        "\"skipped\":[],\"failed\":[],\"hidden\":0}\n");

    const fs::path missing = files.path() / "yok.md";
    const auto bad = box.run({"import", "--json", utf8(missing)});
    CHECK(bad.exit_code == 1);
    CHECK(bad.out.starts_with("{\"imported\":[],\"skipped\":[],\"failed\":[{\"source\":" +
                              json_string(utf8(missing)) + ",\"error\":"));
    CHECK(bad.out.ends_with("}],\"hidden\":0}\n"));
}

TEST_CASE("import usage errors exit 2 and import nothing") {
    const Sandbox box;
    const TempDir files;
    const std::string a = utf8(write_file(files, "a.md", "a"));
    const std::string b = utf8(write_file(files, "b.md", "b"));
    CHECK(box.run({"import"}).exit_code == 2);
    CHECK(box.run({"import", a, b, "--name", "x.md"}).exit_code == 2);
    CHECK(box.run({"import", a, "--bogus"}).exit_code == 2);
    CHECK(box.run({"import", a, "--folder"}).exit_code == 2);
    CHECK(box.run({"import", a, "--folder", "../dis"}).exit_code == 2);
    CHECK(box.run({"import", a, "--type", "a.b"}).exit_code == 2);
    CHECK(box.run({"import", a, "--type", "md "}).exit_code == 2);  // NotePath sondaki boşluğu reddeder
    CHECK_FALSE(box.has_any_note());
}

TEST_CASE("import is a reserved word with help") {
    const Sandbox box;
    const auto help = box.run({"help", "import"});
    CHECK(help.exit_code == 0);
    CHECK(help.out.find("note import") != std::string::npos);
    CHECK(box.run({"help"}).out.find("note import") != std::string::npos);
}
