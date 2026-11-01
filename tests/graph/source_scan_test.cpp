// Mekanik kural 1 ve ilke 5: graph ruhu derleyiciye bırakılmaz, kaynak taranır. Tarayıcının kendisi de
// sınanır: hiçbir şey reddetmeyen tarayıcı yanlış yeşildir.
#include "graph/store.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cctype>
#include <doctest/doctest.h>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using noteapp::test::slurp;

namespace {

const fs::path kSourceDir = NOTEAPP_GRAPH_SOURCE_DIR;
const fs::path kInternalDir = NOTEAPP_GRAPH_INTERNAL_DIR;

std::vector<std::string> lower_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '_') {
            current += static_cast<char>(std::tolower(byte));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty())
        tokens.push_back(std::move(current));
    return tokens;
}

// Kod ve dizge metninde yasak SQL. Yorum hariç tutulmaz: "JOIN" yazısına graph kaynağında hiç gerek yok.
std::vector<std::string> sql_violations(std::string_view source) {
    std::vector<std::string> violations;
    const std::vector<std::string> tokens = lower_tokens(source);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "join")
            violations.emplace_back("JOIN");
        if (tokens[i] == "with" && i + 1 < tokens.size() && tokens[i + 1] == "recursive")
            violations.emplace_back("WITH RECURSIVE");
        for (const std::string_view dynamic :
             {"sqlite3_prepare", "sqlite3_prepare_v2", "sqlite3_prepare16", "sqlite3_exec", "sqlite3_mprintf",
              "sqlite3_snprintf", "sqlite3_vmprintf", "sqlite3_str_new"})
            if (tokens[i] == dynamic)
                violations.emplace_back(dynamic);
    }
    return violations;
}

// sqlite3_prepare_v3'ün tek çağrısı `Sql` türünden sabit metin alır.
std::vector<std::string> prepare_violations(std::string_view source) {
    constexpr std::string_view kCall = "sqlite3_prepare_v3(";
    std::vector<std::string> violations;
    for (std::size_t at = source.find(kCall); at != std::string_view::npos; at = source.find(kCall, at + 1)) {
        const std::size_t end = source.find(';', at);
        const std::string_view call = source.substr(at, end - at);
        if (call.find(".text") == std::string_view::npos)
            violations.emplace_back(call);
    }
    return violations;
}

std::vector<std::string> header_violations(std::string_view source) {
    std::vector<std::string> violations;
    for (const std::string& token : lower_tokens(source)) {
        if (token.find("sqlite") != std::string::npos)
            violations.push_back(token);
        for (const std::string_view concept_word :
             {"note", "notes", "folder", "links_to", "in_folder", "tagged"})
            if (token == concept_word)
                violations.push_back(token);
    }
    return violations;
}

std::vector<fs::path> files_with_extension(const fs::path& root,
                                           std::initializer_list<std::string_view> wanted) {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
        if (std::ranges::contains(wanted, entry.path().extension().string()))
            files.push_back(entry.path());
    return files;
}

std::vector<fs::path> graph_sources() {
    std::vector<fs::path> files = files_with_extension(kSourceDir, {".cpp", ".hpp"});
    std::ranges::move(files_with_extension(kInternalDir, {".cpp", ".hpp"}), std::back_inserter(files));
    return files;
}

}  // namespace

TEST_CASE("scanner_catches_the_forbidden_forms") {
    CHECK_FALSE(sql_violations(R"(constexpr Sql a = "SELECT 1 FROM x JOIN y";)").empty());
    CHECK_FALSE(sql_violations(R"(constexpr Sql a = "select 1 from x left join y";)").empty());
    CHECK_FALSE(sql_violations("WITH\n  RECURSIVE c AS (SELECT 1)").empty());
    CHECK_FALSE(sql_violations("sqlite3_exec(db, text.c_str(), nullptr, nullptr, nullptr);").empty());
    CHECK_FALSE(sql_violations("sqlite3_prepare_v2(db, built, -1, &s, nullptr);").empty());
    CHECK_FALSE(sql_violations("char* q = sqlite3_mprintf(\"%s\", x);").empty());
    CHECK_FALSE(prepare_violations("sqlite3_prepare_v3(db, built.c_str(), -1, 0, &s, nullptr);").empty());
    CHECK(prepare_violations("sqlite3_prepare_v3(db, sql.text, -1, 0, &s, nullptr);").empty());
    CHECK(sql_violations("disjoint rejoin WITHOUT ROWID with care").empty());

    CHECK_FALSE(header_violations("#include <sqlite3.h>").empty());
    CHECK_FALSE(header_violations("struct Note {};").empty());
    CHECK_FALSE(header_violations("// (:Folder)").empty());
    CHECK(header_violations("namespace noteapp::graph {} // not, annotate, denote").empty());
}

TEST_CASE("graph_sources_have_no_join_recursive_or_runtime_sql") {
    const std::vector<fs::path> files = graph_sources();
    // Tarama gerçekten kaynağı buldu mu: dizin yanlışsa boş liste yeşil görünürdü.
    REQUIRE(std::ranges::any_of(files, [](const fs::path& p) { return p.filename() == "store.cpp"; }));
    REQUIRE(files.size() >= 8);
    int prepare_calls = 0;
    for (const fs::path& file : files) {
        const std::string source = slurp(file);
        REQUIRE_FALSE(source.empty());
        INFO(file.string());
        CHECK(sql_violations(source).empty());
        CHECK(prepare_violations(source).empty());
        for (std::size_t at = source.find("sqlite3_prepare_v3("); at != std::string::npos;
             at = source.find("sqlite3_prepare_v3(", at + 1))
            ++prepare_calls;
    }
    CHECK(prepare_calls == 1);
}

TEST_CASE("only_the_storage_boundary_touches_sqlite_and_sql_is_a_consteval_type") {
    const std::string store = slurp(kSourceDir / "store.cpp");
    CHECK(store.find("consteval Sql(") != std::string::npos);
    for (const fs::path& file : graph_sources()) {
        const std::string name = file.filename().string();
        if (name == "store.cpp" || name == "store.hpp")
            continue;
        INFO(name);
        for (const std::string& token : lower_tokens(slurp(file)))
            CHECK(token.find("sqlite3") == std::string::npos);
    }
}

TEST_CASE("public_headers_have_no_sqlite_or_application_concepts") {
    // src/ PUBLIC include köküdür: elle yazılmış liste yeni başlığı kaçırırdı, altındaki her başlık taranır.
    const std::vector<fs::path> headers = files_with_extension(kSourceDir, {".hpp"});
    for (const std::string_view expected : {"error.hpp", "id.hpp", "value.hpp", "graph.hpp", "op.hpp"})
        REQUIRE(std::ranges::any_of(headers, [&](const fs::path& p) { return p.filename() == expected; }));
    for (const fs::path& header : headers) {
        const std::string source = slurp(header);
        REQUIRE_FALSE(source.empty());
        INFO(header.string());
        CHECK(header_violations(source) == std::vector<std::string>{});
    }
    // SQLite türü taşıyan başlık public kökte durmaz; taşınırsa yukarıdaki tarama da düşer.
    CHECK(fs::exists(kInternalDir / "store.hpp"));
    CHECK_FALSE(fs::exists(kSourceDir / "store.hpp"));
}

TEST_CASE("engine_sources_have_no_application_concepts_and_depend_on_no_project_module") {
    for (const fs::path& file : graph_sources()) {
        const std::string source = slurp(file);
        INFO(file.string());
        for (const std::string_view module :
             {"\"core/", "\"search/", "\"sync/", "\"detect/", "\"tui/", "\"gui/", "\"pdf/"})
            CHECK(source.find(module) == std::string::npos);
        for (const std::string& token : lower_tokens(source))
            for (const std::string_view concept_word :
                 {"note", "notes", "folder", "links_to", "in_folder", "tagged"})
                CHECK(token != concept_word);
    }
}
