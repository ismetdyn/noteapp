// İlke 3: Cypher SQL'e çevrilmez. Kaynak taranır; tarayıcının kendisi de sınanır.
#include "support/test_support.hpp"

#include <algorithm>
#include <cctype>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using noteapp::test::slurp;

namespace {

const fs::path kCypherDir = fs::path(NOTEAPP_GRAPH_SOURCE_DIR) / "cypher";

std::vector<std::string> tokens_of(std::string_view text) {
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

// SQL metni üretmenin, dosya veya ağa ulaşmanın işaretleri. Kod ve dizge birlikte taranır.
std::vector<std::string> violations(std::string_view source) {
    std::vector<std::string> found;
    const std::vector<std::string> tokens = tokens_of(source);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        for (const std::string_view banned :
             {"sql", "select", "sqlite3", "fstream", "ifstream", "ofstream", "fopen", "popen", "socket",
              "connect", "curl", "filesystem", "regex", "getenv"})
            if (token == banned)
                found.push_back(token);
        const std::string_view next =
            i + 1 < tokens.size() ? std::string_view(tokens[i + 1]) : std::string_view();
        if (token == "pragma" && next != "once")
            found.push_back(token);
        if ((token == "insert" && next == "into") || (token == "delete" && next == "from") ||
            (token == "update" && next == "set"))
            found.push_back(token + " " + std::string(next));
    }
    return found;
}

// Doğrudan ifade kullanımı: gezinme ReadTxn üzerindendir, tek istisna kenar tipi okumasıdır.
std::vector<std::string> direct_statement_ids(std::string_view source) {
    constexpr std::string_view kMarker = "StatementId::";
    std::vector<std::string> ids;
    for (std::size_t at = source.find(kMarker); at != std::string_view::npos;
         at = source.find(kMarker, at + 1)) {
        std::size_t end = at + kMarker.size();
        while (end < source.size() && std::isalnum(static_cast<unsigned char>(source[end])) != 0)
            ++end;
        ids.emplace_back(source.substr(at + kMarker.size(), end - at - kMarker.size()));
    }
    return ids;
}

std::vector<fs::path> cypher_sources() {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(kCypherDir)) {
        const std::string extension = entry.path().extension().string();
        if (extension == ".cpp" || extension == ".hpp")
            files.push_back(entry.path());
    }
    return files;
}

}  // namespace

TEST_CASE("cypher_scanner_catches_the_forbidden_forms") {
    CHECK_FALSE(violations("std::string q = \"SELECT id FROM nodes\";").empty());
    CHECK_FALSE(violations("q += \" insert  into nodes\";").empty());
    CHECK_FALSE(violations("delete\nfrom x").empty());
    CHECK_FALSE(violations("#include <fstream>").empty());
    CHECK_FALSE(violations("std::regex r;").empty());
    CHECK_FALSE(violations("the sql text").empty());
    CHECK_FALSE(violations("PRAGMA query_only=ON").empty());
    CHECK(violations("#pragma once deleted selection updated connection").empty());
    CHECK(direct_statement_ids("StatementId::EdgeGet x StatementId::NodesAll") ==
          std::vector<std::string>{"EdgeGet", "NodesAll"});
}

TEST_CASE("cypher_is_not_translated_to_storage_language") {
    const std::vector<fs::path> files = cypher_sources();
    // Dizin yanlışsa boş liste yeşil görünürdü.
    for (const std::string_view expected :
         {"lexer.cpp", "parser.cpp", "planner.cpp", "operators.cpp", "expr.cpp"})
        REQUIRE(std::ranges::any_of(files, [&](const fs::path& p) { return p.filename() == expected; }));
    for (const fs::path& file : files) {
        const std::string source = slurp(file);
        REQUIRE_FALSE(source.empty());
        INFO(file.string());
        CHECK(violations(source) == std::vector<std::string>{});
        for (const std::string& id : direct_statement_ids(source))
            CHECK(id == "EdgeGet");
    }
}

TEST_CASE("query_header_has_no_storage_types") {
    const std::string header = slurp(fs::path(NOTEAPP_GRAPH_SOURCE_DIR) / "query.hpp");
    REQUIRE_FALSE(header.empty());
    for (const std::string& token : tokens_of(header))
        CHECK(token.find("sqlite") == std::string::npos);
}
