// note query: Cypher sorgusu, tablo veya JSON çıktı (SPEC-cli "Graph sorgusu").
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/graph_limits.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "graph/graph.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace noteapp::cli {

using core::append_json_string;
using core::base64_of;

namespace {

namespace fs = std::filesystem;
using graph::QueryValue;

constexpr std::string_view kGraphFile = "graph.db";

struct QueryArgs {
    bool write = false;
    bool json = false;
    graph::Params params;
    std::vector<std::string> positional;
};

int usage_error() {
    std::cerr << core::tr("cli.query_usage") << '\n';
    return kExitUsage;
}

int bad_param(const std::string& param) {
    std::cerr << sanitize_for_terminal(core::tr("cli.query_bad_param", {{"param", param}})) << '\n';
    return kExitUsage;
}

bool is_plain_name(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9'))
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

template <class T> std::optional<T> parse_number(std::string_view text) {
    T value{};
    const char* end = text.data() + text.size();
    const auto [parsed_end, code] = std::from_chars(text.data(), end, value);
    if (code != std::errc{} || parsed_end != end)
        return std::nullopt;
    return value;
}

// Tırnak "bu metindir" der: k="12" sayı olmaz. Tırnaksız değerde en dar tip seçilir.
QueryValue typed_value(std::string_view text) {
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
        return std::string(text.substr(1, text.size() - 2));
    if (text == "null")
        return QueryValue{};
    if (text == "true" || text == "false")
        return text == "true";
    if (const auto integer = parse_number<std::int64_t>(text))
        return *integer;
    // from_chars "inf" ve "nan" de kabul eder: motor sonsuz taşımaz, bunlar metindir.
    if (const auto real = parse_number<double>(text); real && std::isfinite(*real))
        return *real;
    return std::string(text);
}

std::expected<void, int> add_param(QueryArgs& parsed, const std::string& param) {
    const std::size_t equals = param.find('=');
    if (equals == std::string::npos || !is_plain_name(std::string_view(param).substr(0, equals)))
        return std::unexpected(bad_param(param));
    parsed.params[param.substr(0, equals)] = typed_value(std::string_view(param).substr(equals + 1));
    return {};
}

std::expected<QueryArgs, int> parse_query_args(const std::vector<std::string>& rest) {
    QueryArgs parsed;
    bool only_positional = false;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_positional || arg.empty() || arg[0] != '-') {
            parsed.positional.push_back(arg);
        } else if (arg == "--") {
            only_positional = true;
        } else if (arg == "--write") {
            parsed.write = true;
        } else if (arg == "--json") {
            parsed.json = true;
        } else if (arg == "--param") {
            if (i + 1 >= rest.size()) {
                std::cerr << core::tr("cli.missing_value", {{"option", arg}}) << '\n';
                return std::unexpected(kExitUsage);
            }
            if (const auto added = add_param(parsed, rest[++i]); !added)
                return std::unexpected(added.error());
        } else {
            std::cerr << sanitize_for_terminal(core::tr("cli.unknown_option", {{"option", arg}})) << '\n';
            return std::unexpected(kExitUsage);
        }
    }
    if (parsed.positional.size() != 1)
        return std::unexpected(usage_error());
    return parsed;
}

// Sorgunun kendisi hatalıysa kullanım hatası, sınır veya çalışma anı hatası kullanıcı kaynaklı, depo hatası
// iç hata.
int exit_code_of(const graph::Error& error) {
    using Code = graph::Error::Code;
    switch (error.code) {
    case Code::Syntax:
    case Code::Unsupported:
    case Code::ReadOnlyViolation:
        return kExitUsage;
    case Code::InvalidArgument:
        return error.message_key == "graph.cypher_missing_param" ? kExitUsage : kExitUser;
    case Code::NotFound:
    case Code::LimitExceeded:
        return kExitUser;
    default:
        return kExitInternal;
    }
}

int report(const graph::Error& error) {
    // İleti sorgudan veya dosyadan gelen metin taşıyabilir: kaçış dizisi terminale geçmesin.
    std::cerr << sanitize_for_terminal(core::tr(error.message_key, error.args)) << '\n';
    return exit_code_of(error);
}

std::string hex_of(std::span<const std::byte> id) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::byte byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        out += kDigits[value >> 4U];
        out += kDigits[value & 0xFU];
    }
    return out;
}

std::string double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return code == std::errc{} ? std::string(buffer.data(), end) : std::string();
}

void append_json_tagged(std::string& out, std::string_view tag, std::string_view text) {
    out += "{\"$";
    out += tag;
    out += "\":";
    append_json_string(out, text);
    out += '}';
}

void append_json_value(std::string& out, const QueryValue& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out += std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                out += double_text(v);  // motor sonlu olmayan sayı üretmez
            } else if constexpr (std::is_same_v<T, std::string>) {
                append_json_string(out, v);
            } else if constexpr (std::is_same_v<T, graph::Bytes>) {
                append_json_tagged(out, "bytes", base64_of(v.data));
            } else if constexpr (std::is_same_v<T, graph::Timestamp>) {
                out += "{\"$timestamp\":" + std::to_string(v.unix_ms) + "}";
            } else if constexpr (std::is_same_v<T, graph::NodeId>) {
                append_json_tagged(out, "node", hex_of(v.bytes));
            } else if constexpr (std::is_same_v<T, graph::EdgeId>) {
                append_json_tagged(out, "edge", hex_of(v.bytes));
            } else {
                out += '[';
                bool first = true;
                for (const QueryValue& item : v.items) {
                    if (!first)
                        out += ',';
                    first = false;
                    append_json_value(out, item);
                }
                out += ']';
            }
        },
        value.v);
}

std::string to_json(const graph::QueryResult& result) {
    std::string out = "{\"columns\":[";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0)
            out += ',';
        append_json_string(out, result.columns[i]);
    }
    out += "],\"rows\":[";
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
        if (row > 0)
            out += ',';
        out += '[';
        for (std::size_t column = 0; column < result.rows[row].size(); ++column) {
            if (column > 0)
                out += ',';
            append_json_value(out, result.rows[row][column]);
        }
        out += ']';
    }
    out += "]}\n";
    return out;
}

// Hücre tek satırda kalır: terminal temizliğinden sonra kalan satır sonu ve sekme görünür kaçışa döner.
std::string table_text(std::string_view text) {
    std::string out;
    for (const char c : sanitize_for_terminal(text)) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out;
}

std::string cell_text(const QueryValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return double_text(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return table_text(v);
            } else if constexpr (std::is_same_v<T, graph::Bytes>) {
                return "<bytes:" + std::to_string(v.data.size()) + ">";
            } else if constexpr (std::is_same_v<T, graph::Timestamp>) {
                return std::to_string(v.unix_ms);
            } else if constexpr (std::is_same_v<T, graph::NodeId> || std::is_same_v<T, graph::EdgeId>) {
                return hex_of(v.bytes);
            } else {
                std::string out = "[";
                for (std::size_t i = 0; i < v.items.size(); ++i)
                    out += (i == 0 ? "" : ", ") + cell_text(v.items[i]);
                return out + "]";
            }
        },
        value.v);
}

// Genişlik kod noktası sayar; geniş karakterde hizalama yaklaşıktır, içerik doğrudur.
std::size_t display_width(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

std::string table_row(const std::vector<std::string>& cells, const std::vector<std::size_t>& widths) {
    std::string out;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        out += cells[i];
        if (i + 1 == cells.size())
            break;
        out.append(widths[i] - display_width(cells[i]), ' ');
        out += " | ";
    }
    out += '\n';
    return out;
}

std::string to_table(const graph::QueryResult& result) {
    if (result.columns.empty())
        return "";
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> header;
    header.reserve(result.columns.size());
    for (const std::string& column : result.columns)
        header.push_back(table_text(column));
    rows.push_back(std::move(header));
    for (const auto& row : result.rows) {
        std::vector<std::string> cells;
        cells.reserve(row.size());
        for (const QueryValue& value : row)
            cells.push_back(cell_text(value));
        rows.push_back(std::move(cells));
    }
    std::vector<std::size_t> widths(result.columns.size(), 0);
    for (const auto& row : rows)
        for (std::size_t i = 0; i < row.size(); ++i)
            widths[i] = std::max(widths[i], display_width(row[i]));
    std::string out;
    for (const auto& row : rows)
        out += table_row(row, widths);
    return out;
}

std::string to_utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Uygulamanın deposu: NoteStore'la aynı dosya, ayrı bağlantı (WAL; bağlantı başına tek iş parçacığı).
std::expected<graph::Graph, int> open_database(bool write, const core::Config& config) {
    const fs::path path = core::data_dir() / fs::path(kGraphFile);
    std::error_code code;
    if (!write && !fs::exists(path, code)) {
        std::cerr << sanitize_for_terminal(core::tr("cli.query_no_database", {{"path", to_utf8(path)}}))
                  << '\n';
        return std::unexpected(kExitUser);
    }
    if (write)
        code = core::create_private_directories(path.parent_path());
    auto opened = graph::Graph::open(path, core::graph_limits_of(config.limits.graph));
    if (!opened)
        return std::unexpected(report(opened.error()));
    return std::move(*opened);
}

}  // namespace

std::string query_output(const graph::QueryResult& result, bool json) {
    return json ? to_json(result) : to_table(result);
}

int run_query(const ParsedArgs& args, const core::Config& config) {
    auto parsed = parse_query_args(args.rest);
    if (!parsed)
        return parsed.error();
    auto database = open_database(parsed->write, config);
    if (!database)
        return database.error();
    const auto mode = parsed->write ? graph::QueryMode::ReadWrite : graph::QueryMode::ReadOnly;
    const auto result = database->query(parsed->positional.front(), parsed->params, mode,
                                        core::query_limits_of(config.limits.query));
    if (!result)
        return report(result.error());
    std::cout << query_output(*result, parsed->json);
    return kExitOk;
}

}  // namespace noteapp::cli
