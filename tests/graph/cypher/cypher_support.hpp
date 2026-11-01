#pragma once

// Cypher testlerinin ortağı: sonucu okunur satırlara çevirir, böylece beklenti tek satırda yazılır.
#include "../graph_test_support.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <doctest/doctest.h>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::graph::test {

inline std::string show(const QueryValue& value);

inline std::string show_list(const QueryList& list) {
    std::string out = "[";
    for (std::size_t i = 0; i < list.items.size(); ++i)
        out += (i == 0 ? "" : ",") + show(list.items[i]);
    return out + "]";
}

inline std::string hex_of(std::span<const std::byte> bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        char part[3];
        std::snprintf(part, sizeof part, "%02x", std::to_integer<unsigned>(b));
        out += part;
    }
    return out;
}

inline std::string show(const QueryValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return "d" + std::to_string(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, Bytes>)
                return "b:" + hex_of(as_bytes(v.data));
            else if constexpr (std::is_same_v<T, Timestamp>)
                return "t" + std::to_string(v.unix_ms);
            else if constexpr (std::is_same_v<T, NodeId> || std::is_same_v<T, EdgeId>)
                return hex_of(v.bytes);
            else
                return show_list(v);
        },
        value.v);
}

inline std::vector<std::string> lines(const QueryResult& result) {
    std::vector<std::string> out;
    for (const auto& row : result.rows) {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i)
            line += (i == 0 ? "" : "|") + show(row[i]);
        out.push_back(std::move(line));
    }
    return out;
}

inline std::vector<std::string> sorted(std::vector<std::string> rows) {
    std::ranges::sort(rows);
    return rows;
}

using Strings = std::vector<std::string>;

inline std::string describe_error(const Error& error) {
    std::string out = error.message_key;
    for (const auto& [name, value] : error.args)
        out += " " + name + "=" + value.substr(0, 80);
    return out;
}

inline QueryResult must_query(Graph& graph, std::string_view text, const Params& params = {},
                              QueryMode mode = QueryMode::ReadOnly, QueryLimits limits = {}) {
    Result<QueryResult> result = graph.query(text, params, mode, limits);
    INFO(std::string(text.substr(0, 200)));
    REQUIRE_MESSAGE(result.has_value(), (result.has_value() ? "" : describe_error(result.error())));
    return std::move(*result);
}

inline QueryResult must_write(Graph& graph, std::string_view text, const Params& params = {}) {
    return must_query(graph, text, params, QueryMode::ReadWrite);
}

inline std::string arg_of(const Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args)
        if (key == name)
            return value;
    return "";
}

// Hata beklenir: kodu, varsa konumu. Yanlış `ok` burada yakalanır.
inline void check_rejected(Graph& graph, std::string_view text, Error::Code code,
                           QueryMode mode = QueryMode::ReadOnly) {
    INFO(std::string(text.substr(0, 200)));
    const Result<QueryResult> result = graph.query(text, {}, mode);
    REQUIRE_FALSE(result.has_value());
    INFO(describe_error(result.error()));
    CHECK(result.error().code == code);
}

class GraphFixture {
public:
    GraphFixture() : graph(open_graph(dir.path() / "graph.db")) {}
    std::filesystem::path path() const { return dir.path() / "graph.db"; }

    noteapp::test::TempDir dir;
    Graph graph;
};

}  // namespace noteapp::graph::test
