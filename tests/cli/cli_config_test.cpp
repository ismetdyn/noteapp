// note config path|list|get|set (T17) ve note help KOMUT.
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::test;

TEST_CASE("config path prints the config file location") {
    const Sandbox box;
    const auto result = box.run({"config", "path"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.size() > std::string("config.toml\n").size());
    CHECK(result.out.ends_with("config.toml\n"));
}

TEST_CASE("config list prints key = value lines with dotted sync keys and joined lists") {
    const Sandbox box;
    const auto result = box.run({"config", "list"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find("language = tr\n") != std::string::npos);
    CHECK(result.out.find("default_type = md\n") != std::string::npos);
    CHECK(result.out.find("types = md, txt, csv, cpp, java, py, json, sh, env, properties, yaml, toml, ini, "
                          "xml, html, css, js, ts, sql, c, go, rs, dockerfile, makefile\n") !=
          std::string::npos);
    CHECK(result.out.find("sync.enabled = false\n") != std::string::npos);
    CHECK(result.out.find("sync.idle_seconds = 30\n") != std::string::npos);
    CHECK(result.out.find("tuning") == std::string::npos);
    CHECK(result.out.find("token") == std::string::npos);
}

TEST_CASE("config set language en switches the messages of the next command (SPEC-cli acceptance 9)") {
    const Sandbox box;
    CHECK(box.run({"mv", "yok.md", "b.md"}).err.find("Bulunamadı") != std::string::npos);

    const auto set = box.run({"config", "set", "language", "en"});
    CHECK(set.exit_code == 0);
    CHECK(set.out == "language = en\n");

    CHECK(box.run({"mv", "yok.md", "b.md"}).err.find("Not found") != std::string::npos);
    CHECK(box.run({"config", "get", "language"}).out == "en\n");
}

TEST_CASE("config set keeps the other keys (notes_dir) intact") {
    const Sandbox box;
    CHECK(box.run({"config", "set", "default_type", "txt"}).exit_code == 0);

    const auto saved = box.run({"hello"});
    CHECK(saved.exit_code == 0);
    CHECK(box.exists("hello.txt"));
}

TEST_CASE("config set and get round-trip scalars, bools, ints and lists") {
    const Sandbox box;
    CHECK(box.run({"config", "set", "sync.idle_seconds", "45"}).exit_code == 0);
    CHECK(box.run({"config", "set", "sync.enabled", "true"}).exit_code == 0);
    CHECK(box.run({"config", "set", "editor", "code --wait"}).exit_code == 0);
    CHECK(box.run({"config", "set", "types", "md, py ,rs"}).exit_code == 0);

    CHECK(box.run({"config", "get", "sync.idle_seconds"}).out == "45\n");
    CHECK(box.run({"config", "get", "sync.enabled"}).out == "true\n");
    CHECK(box.run({"config", "get", "editor"}).out == "code --wait\n");
    CHECK(box.run({"config", "get", "types"}).out == "md, py, rs\n");

    const auto list = box.run({"config", "list"});
    CHECK(list.out.find("types = md, py, rs\n") != std::string::npos);
    CHECK(list.out.find("sync.idle_seconds = 45\n") != std::string::npos);
}

TEST_CASE("config set with an invalid value or unknown key exits 2 and changes nothing") {
    const Sandbox box;

    const auto bad_language = box.run({"config", "set", "language", "de"});
    CHECK(bad_language.exit_code == 2);
    CHECK(bad_language.err.find("language") != std::string::npos);
    CHECK(box.run({"config", "set", "sync.idle_seconds", "0"}).exit_code == 2);
    CHECK(box.run({"config", "set", "sync.enabled", "yes"}).exit_code == 2);
    CHECK(box.run({"config", "set", "types", "md,a.b"}).exit_code == 2);

    const auto unknown = box.run({"config", "set", "nope", "1"});
    CHECK(unknown.exit_code == 2);
    CHECK(unknown.err.find("nope") != std::string::npos);
    CHECK(box.run({"config", "get", "nope"}).exit_code == 2);
    CHECK(box.run({"config", "set", "tuning.autosave_ms", "5"}).exit_code == 2);

    CHECK(box.run({"config", "get", "language"}).out == "tr\n");
    CHECK(box.run({"config", "set", "language"}).exit_code == 2);
    CHECK(box.run({"config", "get"}).exit_code == 2);
    CHECK(box.run({"config"}).exit_code == 2);
    CHECK(box.run({"config", "bogus"}).exit_code == 2);
}

TEST_CASE("config rejects tokens with a hint to note sync login") {
    const Sandbox box;
    for (const char* verb : {"set", "get"}) {
        CAPTURE(verb);
        const auto result = box.run(
            verb == std::string("set") ? std::vector<std::string>{"config", verb, "sync.token", "gizli-deger"}
                                       : std::vector<std::string>{"config", verb, "sync.token"});
        CHECK(result.exit_code == 2);
        CHECK(result.err.find("sync login") != std::string::npos);
        CHECK(result.err.find("gizli-deger") == std::string::npos);
    }
    CHECK(box.run({"config", "list"}).out.find("gizli-deger") == std::string::npos);
}

TEST_CASE("config set refuses to overwrite a config file with a syntax error") {
    const Sandbox box;
    const auto path_out = box.run({"config", "path"}).out;
    const fs::path file = fs::path(std::u8string(path_out.begin(), path_out.end() - 1));
    {
        std::ofstream broken(file, std::ios::binary);
        broken << "this is [not toml";
    }

    const auto result = box.run({"config", "set", "language", "en"});
    CHECK(result.exit_code != 0);
    CHECK(slurp(file) == "this is [not toml");
}

TEST_CASE("help COMMAND prints that command's line, not the general help") {
    const Sandbox box;
    const auto find = box.run({"help", "find"});
    CHECK(find.exit_code == 0);
    CHECK(find.out.find("note find") != std::string::npos);
    CHECK(find.out.find("note config") == std::string::npos);

    const auto config = box.run({"help", "config"});
    CHECK(config.out.find("note config") != std::string::npos);
    CHECK(config.out.find("note find") == std::string::npos);

    const auto general = box.run({"help"});
    CHECK(general.out.find("note find") != std::string::npos);
    CHECK(general.out.find("note config") != std::string::npos);

    const auto unknown = box.run({"help", "bogus"});
    CHECK(unknown.exit_code == 2);
}

namespace {

// config.toml'un sonuna satır ekler (kum havuzunun yazdığı dosyanın yolu `config path` ile alınır).
void append_to_config(const Sandbox& box, const std::string& extra) {
    std::string path = box.run({"config", "path"}).out;
    path.pop_back();  // '\n'
    std::ofstream(std::filesystem::path(std::u8string(path.begin(), path.end())),
                  std::ios::binary | std::ios::app)
        << extra;
}

}  // namespace

TEST_CASE("an unknown config key is warned on stderr while stdout, exit code and the note are unchanged") {
    const Sandbox box;
    append_to_config(box, "bogus_key = 1\n");

    const auto result = box.run({"hello"});

    CHECK(result.exit_code == 0);
    CHECK(result.err.find("bogus_key") != std::string::npos);
    CHECK(box.exists("hello.md"));
    const auto value = box.run({"config", "get", "language"});
    CHECK(value.exit_code == 0);
    CHECK(value.out == "tr\n");  // stdout yalnız değer: uyarı stderr'de
    CHECK(value.err.find("bogus_key") != std::string::npos);
}

TEST_CASE("a config syntax error is announced on stderr instead of silently using defaults") {
    const Sandbox box;
    append_to_config(box, "this is = = not toml\n");

    const auto result = box.run({"config", "list"});

    CHECK(result.exit_code == 0);
    // Config okunamadığı için dil varsayılana (auto) düşer: metin makinenin diline bağlıdır.
    const bool announced = result.err.find("sözdizimi hatası") != std::string::npos ||
                           result.err.find("Syntax error") != std::string::npos;
    CHECK(announced);
    CHECK(result.out.find("language = ") != std::string::npos);
}

TEST_CASE("sync --background stays silent even with config warnings") {
    const Sandbox box;
    append_to_config(box, "bogus_key = 1\n");

    const auto result = box.run({"sync", "--background"});

    CHECK(result.out.empty());
    CHECK(result.err.empty());
}
