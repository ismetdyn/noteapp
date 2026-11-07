// note query: Cypher sorgusu, tablo veya JSON çıktı (SPEC-cli "Graph sorgusu").
#include "app/map_query.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/graph_limits.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"

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

int usage() {
    return usage_error("query");
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
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error("query", arg));
            if (const auto added = add_param(parsed, rest[++i]); !added)
                return std::unexpected(added.error());
        } else {
            return std::unexpected(unknown_option_error("query", arg));
        }
    }
    if (parsed.positional.size() != 1)
        return std::unexpected(usage());
    return parsed;
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

// Hücreler TUI/GUI tablosuyla aynıdır (app::query_table): not yolu, `#etiket`, kenar uçlarıyla, UTC zaman.
std::string to_table(const core::UserQuery& query) {
    const app::QueryTable table = app::query_table(query);
    if (table.columns.empty())
        return "";
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> header;
    header.reserve(table.columns.size());
    for (const std::string& column : table.columns)
        header.push_back(table_text(column));
    rows.push_back(std::move(header));
    for (const auto& row : table.rows) {
        std::vector<std::string> cells;
        cells.reserve(row.size());
        for (const app::QueryCell& cell : row)
            cells.push_back(table_text(cell.text));
        rows.push_back(std::move(cells));
    }
    std::vector<std::size_t> widths(table.columns.size(), 0);
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

// Okuma depo yokken boş sonuç verirdi (core::run_user_query): CLI bunu açıkça söyler, dosya kurulmaz.
bool database_exists() {
    const fs::path path = core::data_dir() / fs::path(kGraphFile);
    std::error_code code;
    if (fs::exists(path, code))
        return true;
    std::cerr << sanitize_for_terminal(core::tr("cli.query_no_database", {{"path", to_utf8(path)}})) << '\n';
    return false;
}

// Hata çekirdekten (core::run_user_query) gelir: sorgunun kendi sorunu (sözdizimi, desteklenmeyen öğe, eksik
// parametre, `--write`sız yazma) 2, korunan veri, çalışma anı hatası ve sınır 1, depo hatası 3 (SPEC-cli "Graph
// sorgusu").
int query_exit_code(const core::Error& error) {
    if (error.code != core::Error::Code::InvalidQuery)
        return error.code == core::Error::Code::NotFound ? kExitUser : kExitInternal;
    constexpr std::array kUsageKeys = {
        std::string_view("graph.cypher_syntax"), std::string_view("graph.cypher_unsupported"),
        std::string_view("graph.cypher_missing_param"), std::string_view("error.query_writes")};
    return std::ranges::find(kUsageKeys, error.message_key) != kUsageKeys.end() ? kExitUsage : kExitUser;
}

int report(const core::Error& error) {
    // Arayüzün "onaylayın" iletisi yerine CLI'nin yolu: `--write`. İleti sorgudan metin taşıyabilir: temizlenir.
    const std::string text = error.message_key == "error.query_writes" ? core::tr("graph.cypher_read_only", error.args)
                                                                       : core::format_error(error);
    std::cerr << sanitize_for_terminal(text) << '\n';
    return query_exit_code(error);
}

}  // namespace

std::string query_output(const core::UserQuery& query, bool json) {
    return json ? to_json(query.result) : to_table(query);
}

// Okuma ve yazma TUI/GUI'nin sorgu çubuğuyla aynı çekirdekten geçer (core::run_user_query): yazma türetilmiş not
// verisini ve ilişki kurmayı reddeder (F3 incelemesi, lead kararı), sonuçtaki düğümler not yoluna adlanır.
int run_query(const ParsedArgs& args, const core::Config& config) {
    auto parsed = parse_query_args(args.rest);
    if (!parsed)
        return parsed.error();
    if (!parsed->write && !database_exists())
        return kExitUser;
    core::NoteStore store = core::NoteStore::for_config(config);
    const auto done =
        core::run_user_query(store, parsed->positional.front(),
                             parsed->write ? core::QueryAction::Write : core::QueryAction::Read,
                             core::query_limits_of(config.limits.query), parsed->params);
    if (!done)
        return report(done.error());
    std::cout << query_output(*done, parsed->json);
    if (done->naming_error)  // yazıldı ama düğümler adlanamadı: tablo kimlik gösterebilir, yazma başarıdır
        std::cerr << sanitize_for_terminal(core::format_error(*done->naming_error)) << '\n';
    return kExitOk;
}

}  // namespace noteapp::cli
