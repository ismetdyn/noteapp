#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/lsp_workspace.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp;
using tui::WorkspaceMirror;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

core::NotePath note(std::string_view path) {
    auto parsed = core::NotePath::parse(path);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::string file_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Kök altındaki tüm normal dosyalar, göreli ve '/' ayırıcılı.
std::set<std::string> mirrored(const fs::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) {
            const std::u8string relative = it->path().lexically_relative(root).generic_u8string();
            out.emplace(relative.begin(), relative.end());
        }
    return out;
}

struct Workspace {
    test::TempDir dir;
    fs::path notes = dir.path() / "data";
    fs::path root = dir.path() / "state" / "lsp" / "workspace";
    core::NoteStore store{notes};

    WorkspaceMirror mirror(std::set<std::string> languages = {"python", "cpp"}) {
        return WorkspaceMirror(root, std::move(languages));
    }
};

}  // namespace

TEST_CASE("lsp language ids come from the note's extension or file name; Env and Properties never map") {
    const std::pair<const char*, const char*> expected[] = {
        {"a/b.py", "python"},
        {"b.pyi", "python"},
        {"x.c", "c"},
        {"x.h", "c"},
        {"x.cpp", "cpp"},
        {"x.HPP", "cpp"},
        {"x.cc", "cpp"},
        {"s.sh", "shellscript"},
        {"s.bash", "shellscript"},
        {"n.md", "markdown"},
        {"n.markdown", "markdown"},
        {"l.lua", "lua"},
        {"j.json", "json"},
        {"j.jsonc", "jsonc"},
        {"y.yaml", "yaml"},
        {"y.yml", "yaml"},
        {"t.toml", "toml"},
        {"a.js", "javascript"},
        {"a.mjs", "javascript"},
        {"a.cjs", "javascript"},
        {"a.jsx", "javascriptreact"},
        {"a.ts", "typescript"},
        {"a.mts", "typescript"},
        {"a.cts", "typescript"},
        {"a.tsx", "typescriptreact"},
        {"m.go", "go"},
        {"m.rs", "rust"},
        {"M.java", "java"},
        {"P.cs", "csharp"},
        {"t/page.html", "html"},
        {"page.htm", "html"},
        {"s.css", "css"},
        {"s.scss", "scss"},
        {"s.less", "less"},
        {"q.sql", "sql"},
        {"x.dockerfile", "dockerfile"},
        {"Dockerfile", "dockerfile"},
        {"app/Containerfile", "dockerfile"},
        {"x.makefile", "makefile"},
        {"Makefile", "makefile"},
        {"makefile", "makefile"},
        {"GNUmakefile", "makefile"},
        {"rules.mk", "makefile"},
        {"CMakeLists.txt", "cmake"},
        {"f.cmake", "cmake"},
        {"r.rb", "ruby"},
        {"p.php", "php"},
        {"k.kt", "kotlin"},
        {"k.kts", "kotlin"},
        {"s.swift", "swift"},
        {"z.zig", "zig"},
        {"x.xml", "xml"},
        {"c.ini", "ini"},
    };
    for (const auto& [path, id] : expected) {
        CAPTURE(path);
        CHECK(tui::lsp_language_for(note(path)) == id);
    }
    // Sır taşıyan tipler ve tanınmayanlar hiçbir sunucuya gitmez.
    for (const char* path : {"prod.env", "app.properties", "notes.txt", "data.csv", "x.unknown"}) {
        CAPTURE(path);
        CHECK(tui::lsp_language_for(note(path)).empty());
    }
}

TEST_CASE("the mirror writes only configured languages as private files and never secrets") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "src/main.py", "import util\n"));
    REQUIRE(test::seed_note(ws.store, "src/util.py", "def f(): pass\n"));
    REQUIRE(test::seed_note(ws.store, "notes/todo.md", "# todo\n"));
    REQUIRE(test::seed_note(ws.store, "app/prod.env", "TOKEN=secret\n"));
    REQUIRE(test::seed_note(ws.store, "app/db.properties", "password=secret\n"));
    REQUIRE(test::seed_note(ws.store, "big.py", std::string(tui::kMaxLspNoteBytes + 1, '#')));

    // Sır tiplerine sunucu tanımlansa bile (kullanıcının dil kimliği tahmini) yazılmaz.
    auto mirror = ws.mirror({"python", "markdown", "env", "dotenv", "properties", "ini", "plaintext"});
    mirror.sync(ws.store);

    CHECK(mirrored(ws.root) == std::set<std::string>{"notes/todo.md", "src/main.py", "src/util.py"});
    CHECK(file_text(ws.root / "src" / "main.py") == "import util\n");
#ifndef _WIN32
    CHECK(fs::status(ws.root).permissions() == fs::perms::owner_all);
    CHECK(fs::status(ws.root / "src").permissions() == fs::perms::owner_all);
    CHECK((fs::status(ws.root / "src" / "main.py").permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
#endif
}

// Senkronla gelen bir not sunucunun ayarını ezmemeli ya da çalıştırdığı kodu sağlamamalı (T59e).
TEST_CASE("the mirror never writes files that configure a server or supply its code") {
    Workspace ws;
    for (const char* path :
         {"rust-analyzer.toml", "crate/rust-analyzer.toml", "rust-toolchain.toml", "rust-project.json",
          "taplo.toml", "web/taplo.toml", "node_modules/typescript/lib/tsserver.js",
          "web/Node_Modules/typescript/package.json", "Cargo.toml", "src/main.rs", "tsconfig.json", "app.js",
          "node_modules.js"})
        REQUIRE(test::seed_note(ws.store, path, "x\n"));

    auto mirror = ws.mirror({"toml", "json", "javascript", "rust"});
    mirror.sync(ws.store);

    CHECK(mirrored(ws.root) ==
          std::set<std::string>{"Cargo.toml", "app.js", "node_modules.js", "src/main.rs", "tsconfig.json"});
    CHECK_FALSE(fs::exists(ws.root / "node_modules"));
}

// APFS ve NTFS Unicode harf katlar: ſ (U+017F) s'ye, ı (U+0131) I'ya, K (U+212A) k'ye denk düşer; tam
// genişlik harfler de ASCII'nin eşi sayılır. Bu adlar engellenen adın kendisiymiş gibi ele alınır.
TEST_CASE("names that a folding file system would match to a denied name count as denied") {
    for (const char* path :
         {"node_modules/x.js", "NODE_MODULES/x.js", "node_module\xC5\xBF/typescript/lib/tsserver.js",
          "ru\xC5\xBFt-analyzer.toml", "rust-toolcha\xC4\xB1n.toml", "rust-toolcha\xC4\xB0n.toml",
          "\xEF\xBD\x94\xEF\xBD\x81\xEF\xBD\x90\xEF\xBD\x8C\xEF\xBD\x8F.toml",   // ｔａｐｌｏ
          "a/\xEF\xBD\x8E\xEF\xBD\x8F\xEF\xBD\x84\xEF\xBD\x85_modules/b.js"}) {  // ｎｏｄｅ_modules
        CAPTURE(path);
        CHECK(tui::is_server_owned_path(note(path)));
    }
    for (const char* path :
         {"k\xC4\xB1lavuz.toml", "\xC4\xB0stanbul/a.js", "node_modules.js", "Cargo.toml"}) {
        CAPTURE(path);
        CHECK_FALSE(tui::is_server_owned_path(note(path)));
    }
}

// Katlama tablosunun bilmediği bir eşleşmeyi dosya sistemi yakalar: yazılan dosya ya da klasör, engellenen
// adla aynı girdiyse silinir ve atlanır. Linux'ta katlama yok; eşlik sahte bir karşılaştırıcıyla kurulur.
TEST_CASE("an entry the file system resolves to a denied name is removed and skipped") {
    Workspace ws;
    for (const char* path : {"weird/a.js", "odd.toml", "fine.toml"})
        REQUIRE(test::seed_note(ws.store, path, "x\n"));
    const auto same_entry = [](const fs::path& written, const fs::path& denied) {
        const auto name = [](const fs::path& p) { return p.filename().string(); };
        return written.parent_path() == denied.parent_path() &&
               ((name(written) == "weird" && name(denied) == "node_modules") ||
                (name(written) == "odd.toml" && name(denied) == "taplo.toml"));
    };
    WorkspaceMirror mirror(ws.root, {"toml", "javascript"}, same_entry);
    mirror.sync(ws.store);
    CHECK(mirrored(ws.root) == std::set<std::string>{"fine.toml"});
    CHECK_FALSE(fs::exists(ws.root / "weird"));
}

// T59g incelemesinin atlatma örnekleri: anahtar adına bakılmaz, metnin her yeri (ham ve JSON/yüzde kaçışları
// çözülmüş) ağ yolu için taranır. Karar işlevi platformdan bağımsız sınanır; kuralın yalnız Windows'ta
// uygulanması kBlockNetworkPaths'tedir.
TEST_CASE("network path references are found anywhere in the text, escaped or not") {
    std::string long_array = R"({"extends": [)";
    while (long_array.size() < 5000)
        long_array += R"("./a.json", )";
    long_array += R"("//h/x.json"]})";
    const std::string blocked[] = {
        "{\"$schema\"\n: \"//h/s.json\"}",
        "{/* yorum */ \"$schema\": \"//h/s.json\"}",
        "// yorum\n{\"$schema\": \"\\\\\\\\h\\\\s.json\"}",  // JSON'da \\h\s.json
        R"({"$schema": "\/\/h\/s.json"})",
        R"({"$schema": "\u002f\u002fh/s.json"})",
        R"({"$schema": "\u0066ile://h/s.json"})",
        R"({"extends": ["a\"b", "//h/x.json"]})",
        long_array,
        "#include \"//h/a.h\"\n",
        "#include \"\\\\h\\a.h\"\n",  // ham \\h\a.h: #include kaçış çözmez
        "import \"//h/m\"\n",
        "/// <reference path=\"//h/x.d.ts\"/>\n",
        R"({"include": ["//h/src/**"]})",
        R"({"compilerOptions": {"paths": {"@x/*": ["//h/x/*"]}}})",
        R"([{"command": "cc -I//h/s -c a.c"}])",
        R"({"$schema": "%5C%5Ch%5Cs.json"})",
        R"({"$schema": "%2F%2Fh/s.json"})",
        "see file://server/x",
        "see file:////server/share/x",
        "FILE://Server/x",
        "file:\\\\server\\x",
        "file:///\\\\server\\x",
        "//cdn.example.com/x.js",  // kabul edilen yanlış pozitif: protokolsüz URL
    };
    for (const std::string& text : blocked) {
        CAPTURE(text.substr(0, 120));
        CHECK(tui::references_network_path(text));
    }
    const char* allowed[] = {
        "https://x/y",
        R"({"$schema": "https://json.schemastore.org/package.json"})",
        "C:\\a\\b",
        R"({"path": "C:\\a\\b\\c.json"})",  // JSON'da C:\a\b\c.json
        "/abs/path/x.json",
        R"({"$schema": "file:///abs/s.json"})",
        R"({"$schema": "file://localhost/abs/x.json"})",
        "x = a // b\n",
        "half = n//2\n",
        "//TODO: fix\n",
        R"(const re = "\\d+\\s"; console.log("\\n");)",
        "profile://x",
    };
    for (const char* text : allowed) {
        CAPTURE(text);
        CHECK_FALSE(tui::references_network_path(text));
    }
}

TEST_CASE("with network paths blocked a note referencing one is not mirrored") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "remote.json", R"({"$schema": "//server/share/s.json"})"));
    REQUIRE(test::seed_note(ws.store, "local.json", R"({"$schema": "file:///abs/s.json"})"));
    WorkspaceMirror mirror(ws.root, {"json"}, {}, true);
    mirror.sync(ws.store);
    CHECK(mirrored(ws.root) == std::set<std::string>{"local.json"});
}

// Varsayılan yalnız Windows'ta engeller: Linux'ta //host yerel bir yoldur, SMB yoktur. Wine'da bu test
// engellemeyi, Linux'ta yansıtmayı doğrular.
TEST_CASE("by default network paths are blocked only on Windows") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "remote.json", R"({"$schema": "//server/share/s.json"})"));
    auto mirror = ws.mirror({"json"});
    mirror.sync(ws.store);
#ifdef _WIN32
    CHECK(tui::kBlockNetworkPaths);
    CHECK(mirrored(ws.root).empty());
#else
    CHECK_FALSE(tui::kBlockNetworkPaths);
    CHECK(mirrored(ws.root) == std::set<std::string>{"remote.json"});
#endif
}

TEST_CASE("the mirror follows save, rename, move, delete, trash and purge and drops stale files") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "a.py", "a = 1\n"));
    REQUIRE(test::seed_note(ws.store, "b.py", "b = 1\n"));
    REQUIRE(test::seed_note(ws.store, "c.py", "c = 1\n"));
    REQUIRE(test::seed_note(ws.store, "d.py", "d = 1\n"));
    auto mirror = ws.mirror();
    mirror.sync(ws.store);
    REQUIRE(mirrored(ws.root) == std::set<std::string>{"a.py", "b.py", "c.py", "d.py"});

    // Elle bırakılan yabancı dosya ve boşalan klasörü de gider: dizin yalnız yansıtmanındır.
    fs::create_directories(ws.root / "old");
    std::ofstream(ws.root / "old" / "x.py") << "stale";

    const auto a = ws.store.read(note("a.py"));
    REQUIRE(a.has_value());
    REQUIRE(ws.store.write(note("a.py"), "a = 2\n", a->revision).has_value());
    REQUIRE(ws.store.move(note("b.py"), note("pkg/b2.py")).has_value());
    REQUIRE(ws.store.remove(note("c.py")).has_value());
    const auto trashed = ws.store.trash(note("d.py"));
    REQUIRE(trashed.has_value());
    mirror.sync(ws.store);

    CHECK(mirrored(ws.root) == std::set<std::string>{"a.py", "pkg/b2.py"});
    CHECK(file_text(ws.root / "a.py") == "a = 2\n");
    CHECK_FALSE(fs::exists(ws.root / "old"));

    // Çöpten geri gelen not yeniden yansır; kalıcı silinen bir daha görünmez.
    REQUIRE(ws.store.restore(trashed->path).has_value());
    mirror.sync(ws.store);
    CHECK(mirrored(ws.root).contains("d.py"));
    const auto again = ws.store.trash(note("d.py"));
    REQUIRE(again.has_value());
    REQUIRE(ws.store.purge(again->path).has_value());
    mirror.sync(ws.store);
    CHECK(mirrored(ws.root) == std::set<std::string>{"a.py", "pkg/b2.py"});

    // Başka süreçten (senkron, CLI) gelen yazma da yeniden taramadan sonra yansır.
    test::upsert_note(ws.notes, "a.py", "a = 3\n");
    mirror.sync(ws.store);
    CHECK(file_text(ws.root / "a.py") == "a = 3\n");
}

TEST_CASE("a fresh mirror object re-syncs what an earlier run left behind") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "a.py", "a = 1\n"));
    ws.mirror().sync(ws.store);
    std::ofstream(ws.root / "a.py", std::ios::binary | std::ios::trunc) << "tampered";
    REQUIRE(test::seed_note(ws.store, "b.py", "b = 1\n"));
    ws.mirror().sync(ws.store);
    CHECK(file_text(ws.root / "a.py") == "a = 1\n");
    CHECK(file_text(ws.root / "b.py") == "b = 1\n");
}

namespace {

// Dizin bağı kurulamazsa (Windows'ta yetki ya da Wine) false; test bunu söyleyip atlar.
bool make_directory_link(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (ec)
        MESSAGE("directory symlink not supported here, case skipped: ", ec.message());
    return !ec;
}

// Bağın gösterdiği, yansıtmaya ait olmayan dizin: içinde korunması gereken bir dosya.
fs::path victim_directory(const fs::path& at) {
    fs::create_directories(at);
    std::ofstream(at / "keep.py", std::ios::binary) << "keep";
#ifndef _WIN32
    fs::permissions(at, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec);
#endif
    return at;
}

void check_untouched(const fs::path& victim) {
    CHECK(file_text(victim / "keep.py") == "keep");
    CHECK_FALSE(fs::exists(victim / "a.py"));
    CHECK_FALSE(fs::exists(victim / "src"));
#ifndef _WIN32
    CHECK((fs::status(victim).permissions() & fs::perms::others_read) == fs::perms::others_read);
#endif
}

}  // namespace

TEST_CASE("mirror paths stay inside the workspace even through a planted symlink") {
    Workspace ws;
    const fs::path outside = victim_directory(ws.dir.path() / "outside");
    fs::create_directories(ws.root);
    if (!make_directory_link(outside, ws.root / "src"))
        return;
    REQUIRE(test::seed_note(ws.store, "src/main.py", "x = 1\n"));
    REQUIRE(test::seed_note(ws.store, "lib/a.py", "y = 1\n"));

    CHECK_FALSE(tui::mirror_file(ws.root, note("src/main.py")).has_value());
    const auto inside = tui::mirror_file(ws.root, note("lib/a.py"));
    REQUIRE(inside.has_value());
    CHECK(inside->filename() == "a.py");

    ws.mirror().sync(ws.store);
    // Bağın içine ne yazılır ne oradan silinir; bağın dışındaki not yine yansır.
    CHECK(file_text(outside / "keep.py") == "keep");
    CHECK_FALSE(fs::exists(outside / "main.py"));
    CHECK(file_text(ws.root / "lib" / "a.py") == "y = 1\n");
}

TEST_CASE("a workspace root or lsp directory that is a link is refused and its target left alone") {
    for (const bool link_lsp : {false, true}) {
        CAPTURE(link_lsp);
        Workspace ws;
        const fs::path victim = victim_directory(ws.dir.path() / "victim");
        const fs::path link = link_lsp ? ws.root.parent_path() : ws.root;
        fs::create_directories(link.parent_path());
        if (!make_directory_link(victim, link))
            return;
        REQUIRE(test::seed_note(ws.store, "a.py", "a = 1\n"));
        REQUIRE(test::seed_note(ws.store, "src/b.py", "b = 1\n"));

        CHECK_FALSE(tui::mirror_file(ws.root, note("a.py")).has_value());
        ws.mirror().sync(ws.store);
        check_untouched(victim);
        if (link_lsp)
            CHECK_FALSE(fs::exists(victim / "workspace"));
    }
}

TEST_CASE("server caches in dot directories of the workspace survive a sync") {
    Workspace ws;
    REQUIRE(test::seed_note(ws.store, "a.py", "a = 1\n"));
    auto mirror = ws.mirror();
    mirror.sync(ws.store);
    fs::create_directories(ws.root / ".cache" / "clangd" / "index");
    std::ofstream(ws.root / ".cache" / "clangd" / "index" / "a.idx", std::ios::binary) << "index";
    std::ofstream(ws.root / ".clangd", std::ios::binary) << "cfg";
    mirror.sync(ws.store);
    CHECK(file_text(ws.root / ".cache" / "clangd" / "index" / "a.idx") == "index");
    CHECK(file_text(ws.root / ".clangd") == "cfg");
    CHECK(file_text(ws.root / "a.py") == "a = 1\n");
}

TEST_CASE("mirroring 2000 notes runs off the UI thread") {
    Workspace ws;
    constexpr std::size_t kNotes = 2000;
    std::vector<core::ImportedNote> notes;
    for (std::size_t i = 0; i < kNotes; ++i)
        notes.push_back({note("pkg" + std::to_string(i % 20) + "/m" + std::to_string(i) + ".py"),
                         "def f" + std::to_string(i) + "():\n    return " + std::string(200, '1') + "\n",
                         std::chrono::system_clock::now()});
    test::bulk_seed(ws.notes, notes);

    // Zamanlama: ilk eşitleme (yazma) ve değişiklik yokken ikinci eşitleme, aynı nesneyle ve yeni nesneyle.
    auto mirror = ws.mirror();
    const auto first_start = Clock::now();
    mirror.sync(ws.store);
    const auto first = Clock::now() - first_start;
    const auto second_start = Clock::now();
    mirror.sync(ws.store);
    const auto second = Clock::now() - second_start;
    const auto restart_start = Clock::now();
    ws.mirror().sync(ws.store);
    const auto restart = Clock::now() - restart_start;
    CHECK(mirrored(ws.root).size() == kNotes);
    MESSAGE("mirror 2000 notes: first ", std::chrono::duration_cast<std::chrono::milliseconds>(first).count(),
            " ms, unchanged ", std::chrono::duration_cast<std::chrono::milliseconds>(second).count(),
            " ms, restart ", std::chrono::duration_cast<std::chrono::milliseconds>(restart).count(), " ms");

    // UI tarafı yalnız istek bırakır: kurulum ve request, eşitleme sürerken bir kareden (16 ms) kısa döner.
    fs::remove_all(ws.root);
    const auto ui_start = Clock::now();
    tui::LspWorkspace background(ws.mirror(), ws.store, std::chrono::milliseconds(0));
    background.request();
    const auto ui_cost = Clock::now() - ui_start;
    MESSAGE("UI cost of starting the mirror: ",
            std::chrono::duration_cast<std::chrono::microseconds>(ui_cost).count(), " us");
    if (test::kWallClockBudgets)
        CHECK(ui_cost < std::chrono::milliseconds(16));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (mirrored(ws.root).size() < kNotes && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(mirrored(ws.root).size() == kNotes);
}
