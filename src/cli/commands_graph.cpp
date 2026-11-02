// `note graph migrate | export | import` ve `note export --all`. Motor nottan habersizdir: JSONL biçimi
// düğüm, etiket, kenar ve özellikten ibarettir. Biçim (bir satır bir nesne):
//   {"k":"noteapp-graph","v":1}
//   {"k":"node","id":"<32 hex>","labels":["Note"],"props":{"name":{"s":"a"},"body":{"b":"<base64>"}}}
//   {"k":"edge","id":"<32 hex>","src":"<32 hex>","type":"IN_FOLDER","dst":"<32 hex>","props":{}}
// Değer: s metin, b bayt (base64), i tam sayı (metin), f double (metin), t zaman (ms, metin), p bool.
// Eşzamanlı head'lerden yalnız okunan değer yazılır: taşıma biçimidir, senkronun yerine geçmez.
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "cli/json_parser.hpp"
#include "cli/json_text.hpp"
#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/migration.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "graph/graph.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::cli {

namespace {

namespace fs = std::filesystem;
namespace graph = noteapp::graph;

constexpr const char* kFormatTag = "noteapp-graph";
constexpr std::size_t kMaxLineBytes = 128U * 1024U * 1024U;  // 64 MiB gövde base64'te bu kadara sığar
constexpr std::size_t kBatchOps = 5000;                      // çerçeve sınırı 65.536 işlem
constexpr std::size_t kBatchBytes = 32U * 1024U * 1024U;     // çerçeve sınırı 64 MiB
constexpr std::string_view kRefusedExisting = "exists";

std::string to_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string hex_of(std::span<const std::byte> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    for (const std::byte value : bytes) {
        text += kDigits[std::to_integer<unsigned>(value) >> 4U];
        text += kDigits[std::to_integer<unsigned>(value) & 0xFU];
    }
    return text;
}

template <class Id> std::optional<Id> id_from_hex(std::string_view text) {
    Id id;
    if (text.size() != id.bytes.size() * 2)
        return std::nullopt;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        unsigned value = 0;
        const auto* begin = text.data() + i * 2;
        const auto [end, code] = std::from_chars(begin, begin + 2, value, 16);
        if (code != std::errc{} || end != begin + 2)
            return std::nullopt;
        id.bytes[i] = static_cast<std::byte>(value);
    }
    return id;
}

int base64_digit(char c) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto at = kAlphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// Dört karakterlik grup; geçersizse boş.
std::optional<unsigned> base64_group(std::string_view chunk, int pad) {
    unsigned group = 0;
    for (std::size_t j = 0; j < 4; ++j) {
        const bool padded = j >= 4U - static_cast<unsigned>(pad);
        const int value = padded ? 0 : base64_digit(chunk[j]);
        if (value < 0)
            return std::nullopt;
        group = (group << 6U) | static_cast<unsigned>(value);
    }
    return group;
}

std::optional<std::string> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0)
        return std::nullopt;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=' ? 1 : 0) + (text[i + 2] == '=' ? 1 : 0) : 0;
        const auto group = base64_group(text.substr(i, 4), pad);
        if (!group)
            return std::nullopt;
        out += static_cast<char>((*group >> 16U) & 0xFFU);
        if (pad < 2)
            out += static_cast<char>((*group >> 8U) & 0xFFU);
        if (pad < 1)
            out += static_cast<char>(*group & 0xFFU);
    }
    return out;
}

void append_value(std::string& out, const graph::Value& value) {
    std::visit(
        [&out]<class T>(const T& v) {
            out += '{';
            if constexpr (std::is_same_v<T, std::string>) {
                out += "\"s\":";
                append_json_string(out, v);
            } else if constexpr (std::is_same_v<T, graph::Bytes>) {
                out += "\"b\":\"" + base64_of(v.data) + "\"";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out += "\"i\":\"" + std::to_string(v) + "\"";
            } else if constexpr (std::is_same_v<T, double>) {
                std::array<char, 64> buffer{};
                const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
                out += "\"f\":\"" + (code == std::errc{} ? std::string(buffer.data(), end) : std::string()) +
                       "\"";
            } else if constexpr (std::is_same_v<T, graph::Timestamp>) {
                out += "\"t\":\"" + std::to_string(v.unix_ms) + "\"";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += std::string("\"p\":") + (v ? "true" : "false");
            }
            out += '}';
        },
        value);
}

template <class Number> std::optional<Number> number_from(const std::string& text) {
    Number number{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [parsed_end, code] = std::from_chars(begin, end, number);
    if (code != std::errc{} || parsed_end != end)
        return std::nullopt;
    return number;
}

std::optional<graph::Value> numeric_value_from(const std::string& tag, const std::string& text) {
    if (tag == "f") {
        const auto number = number_from<double>(text);
        return number ? std::optional<graph::Value>(*number) : std::nullopt;
    }
    const auto number = number_from<std::int64_t>(text);
    if (!number || (tag != "i" && tag != "t"))
        return std::nullopt;
    return tag == "i" ? graph::Value{*number} : graph::Value{graph::Timestamp{*number}};
}

std::optional<graph::Value> value_from(const Json& json) {
    if (json.kind != Json::Kind::Object || json.fields.size() != 1)
        return std::nullopt;
    const auto& [tag, value] = *json.fields.begin();
    if (tag == "p")
        return value.kind == Json::Kind::Bool ? std::optional<graph::Value>(value.text == "true")
                                              : std::nullopt;
    if (value.kind != Json::Kind::String)
        return std::nullopt;
    if (tag == "s")
        return graph::Value{value.text};
    if (tag == "b") {
        auto bytes = base64_decode(value.text);
        return bytes ? std::optional<graph::Value>(graph::Bytes{std::move(*bytes)}) : std::nullopt;
    }
    return numeric_value_from(tag, value.text);
}

// Bir sahibin özellikleri: okunan head, büyük değer açılarak.
graph::Result<void> append_props(std::string& out, graph::ReadTxn& txn, const graph::OwnerId& owner) {
    auto heads = txn.properties(owner);
    if (!heads)
        return std::unexpected(heads.error());
    out += "\"props\":{";
    bool first = true;
    std::string previous;
    for (const graph::PropertyHead& head : *heads) {
        if (head.key == previous)
            continue;
        previous = head.key;
        auto value = txn.property(owner, head.key);
        if (!value)
            return std::unexpected(value.error());
        if (std::holds_alternative<std::monostate>(*value))
            continue;
        if (!first)
            out += ',';
        first = false;
        append_json_string(out, head.key);
        out += ':';
        append_value(out, *value);
    }
    out += '}';
    return {};
}

graph::Result<std::vector<graph::NodeId>> all_nodes(graph::ReadTxn& txn) {
    auto cursor = txn.nodes();
    if (!cursor)
        return std::unexpected(cursor.error());
    std::vector<graph::NodeId> ids;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return ids;
        ids.push_back(**next);
    }
}

graph::Result<void> write_node_line(graph::ReadTxn& txn, const graph::NodeId& id, std::ostream& output) {
    std::string line = "{\"k\":\"node\",\"id\":\"" + hex_of(id.bytes) + "\",\"labels\":[";
    auto labels = txn.labels(id);
    if (!labels)
        return std::unexpected(labels.error());
    for (std::size_t i = 0; i < labels->size(); ++i) {
        if (i > 0)
            line += ',';
        append_json_string(line, (*labels)[i]);
    }
    line += "],";
    NOTEAPP_TRY(append_props(line, txn, id));
    output << line << "}\n";
    return {};
}

graph::Result<void> write_edge_lines(graph::ReadTxn& txn, const graph::NodeId& id, std::ostream& output) {
    auto edges = txn.neighbors(id, graph::Direction::Out);
    if (!edges)
        return std::unexpected(edges.error());
    struct Row {
        graph::EdgeId edge;
        graph::NodeId node;
        std::string type;
    };
    std::vector<Row> rows;
    while (true) {
        auto next = edges->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        rows.push_back({(*next)->edge, (*next)->node, std::string((*next)->type)});
    }
    for (const Row& row : rows) {
        std::string line = "{\"k\":\"edge\",\"id\":\"" + hex_of(row.edge.bytes) + "\",\"src\":\"" +
                           hex_of(id.bytes) + "\",\"type\":";
        append_json_string(line, row.type);
        line += ",\"dst\":\"" + hex_of(row.node.bytes) + "\",";
        NOTEAPP_TRY(append_props(line, txn, row.edge));
        output << line << "}\n";
    }
    return {};
}

graph::Result<void> write_export(graph::Graph& store, std::ostream& output) {
    auto txn = store.read();
    if (!txn)
        return std::unexpected(txn.error());
    output << "{\"k\":\"" << kFormatTag << "\",\"v\":1}\n";
    const auto ids = all_nodes(*txn);
    if (!ids)
        return std::unexpected(ids.error());
    for (const graph::NodeId& id : *ids)
        NOTEAPP_TRY(write_node_line(*txn, id, output));
    for (const graph::NodeId& id : *ids)
        NOTEAPP_TRY(write_edge_lines(*txn, id, output));
    output.flush();
    if (!output)
        return std::unexpected(graph::Error{graph::Error::Code::Io, "graph.write_failed", {}});
    return {};
}

int fail_graph(const graph::Error& error, const fs::path& path) {
    std::cerr << sanitize_for_terminal(core::format_error(core::from_graph(error, to_utf8(path)))) << '\n';
    return kExitInternal;
}

int bad_line(std::size_t line, std::string_view reason) {
    std::cerr << core::tr("cli.graph_bad_line",
                          {{"line", std::to_string(line)}, {"reason", std::string(reason)}})
              << '\n';
    return kExitUser;
}

graph::Result<void> set_props(graph::WriteTxn& txn, const graph::OwnerId& owner,
                              const std::map<std::string, graph::Value>& props, std::size_t& ops,
                              std::size_t& bytes) {
    for (const auto& [key, value] : props) {
        auto current = txn.property(owner, key);
        if (!current)
            return std::unexpected(current.error());
        if (*current == value)
            continue;  // tekrar içe aktarma işlem yazmaz
        if (auto set = txn.set_property(owner, key, value); !set)
            return set;
        ++ops;
        if (const graph::Bytes* blob = std::get_if<graph::Bytes>(&value))
            bytes += blob->data.size();
    }
    return {};
}

// Etiket, kenar tipi ve özellik anahtarı not görünümünün ad kapısından geçer: tek bileşen, denetim karakteri
// yok.
bool is_safe_schema_name(std::string_view name) {
    return name.find('/') == std::string_view::npos && core::NotePath::parse(name).has_value();
}

graph::Result<bool> props_differ(graph::WriteTxn& txn, const graph::OwnerId& owner,
                                 const std::map<std::string, graph::Value>& props) {
    for (const auto& [key, value] : props) {
        auto current = txn.property(owner, key);
        if (!current)
            return std::unexpected(current.error());
        if (*current != value)
            return true;
    }
    return false;
}

graph::Result<bool> node_would_change(graph::WriteTxn& txn, const graph::NodeId& id, const Json& labels,
                                      const std::map<std::string, graph::Value>& props) {
    auto present = txn.labels(id);
    if (!present)
        return std::unexpected(present.error());
    for (const Json& label : labels.items)
        if (std::ranges::find(*present, label.text) == present->end())
            return true;
    return props_differ(txn, id, props);
}

std::optional<std::map<std::string, graph::Value>> props_from(const Json& line) {
    const auto found = line.fields.find("props");
    if (found == line.fields.end() || found->second.kind != Json::Kind::Object)
        return std::nullopt;
    std::map<std::string, graph::Value> props;
    for (const auto& [key, encoded] : found->second.fields) {
        auto value = value_from(encoded);
        if (!value || !is_safe_schema_name(key))
            return std::nullopt;
        props.emplace(key, std::move(*value));
    }
    return props;
}

const std::string* text_field(const Json& line, const char* key) {
    const auto found = line.fields.find(key);
    return found == line.fields.end() || found->second.kind != Json::Kind::String ? nullptr
                                                                                  : &found->second.text;
}

graph::Result<void> write_node(graph::WriteTxn& txn, const graph::NodeId& id, const Json& labels,
                               const std::map<std::string, graph::Value>& props, std::size_t& ops,
                               std::size_t& bytes) {
    auto present = txn.labels(id);
    if (!present)
        return std::unexpected(present.error());
    for (const Json& label : labels.items) {
        if (std::ranges::find(*present, label.text) != present->end())
            continue;
        NOTEAPP_TRY(txn.add_label(id, label.text));
        ++ops;
    }
    return set_props(txn, id, props, ops, bytes);
}

bool labels_are_safe(const Json& labels) {
    return std::ranges::all_of(labels.items, [](const Json& label) {
        return label.kind == Json::Kind::String && is_safe_schema_name(label.text);
    });
}

graph::Result<std::optional<std::string>> import_node(graph::WriteTxn& txn, const Json& line, bool overwrite,
                                                      std::size_t& ops, std::size_t& bytes) {
    const std::string* id_text = text_field(line, "id");
    const auto id = id_text == nullptr ? std::nullopt : id_from_hex<graph::NodeId>(*id_text);
    const auto labels = line.fields.find("labels");
    const auto props = props_from(line);
    if (!id || labels == line.fields.end() || labels->second.kind != Json::Kind::Array || !props)
        return std::optional<std::string>("bad_node");
    if (!labels_are_safe(labels->second))
        return std::optional<std::string>("bad_label");
    auto exists = txn.has_node(*id);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists && !overwrite) {
        auto changes = node_would_change(txn, *id, labels->second, *props);
        if (!changes)
            return std::unexpected(changes.error());
        if (*changes)
            return std::optional<std::string>(kRefusedExisting);
    }
    if (!*exists) {
        NOTEAPP_TRY(txn.put_node(*id));
        ++ops;
    }
    NOTEAPP_TRY(write_node(txn, *id, labels->second, *props, ops, bytes));
    return std::optional<std::string>{};
}

graph::Result<bool> edge_exists(graph::WriteTxn& txn, const graph::EdgeId& id, const graph::NodeId& src,
                                std::string_view type) {
    auto cursor = txn.neighbors(src, graph::Direction::Out, type);
    if (!cursor)
        return std::unexpected(cursor.error());
    bool found = false;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return found;
        found = found || (*next)->edge == id;
    }
}

template <class Id> std::optional<Id> id_field(const Json& line, const char* key) {
    const std::string* text = text_field(line, key);
    return text == nullptr ? std::nullopt : id_from_hex<Id>(*text);
}

graph::Result<bool> has_both_nodes(graph::WriteTxn& txn, const graph::NodeId& src, const graph::NodeId& dst) {
    auto has_src = txn.has_node(src);
    if (!has_src || !*has_src)
        return has_src;
    return txn.has_node(dst);
}

graph::Result<std::optional<std::string>> import_edge(graph::WriteTxn& txn, const Json& line, bool overwrite,
                                                      std::size_t& ops, std::size_t& bytes) {
    const std::string* type = text_field(line, "type");
    const auto id = id_field<graph::EdgeId>(line, "id");
    const auto src = id_field<graph::NodeId>(line, "src");
    const auto dst = id_field<graph::NodeId>(line, "dst");
    const auto props = props_from(line);
    if (!id || !src || !dst || type == nullptr || !props || !is_safe_schema_name(*type))
        return std::optional<std::string>("bad_edge");
    auto has_ends = has_both_nodes(txn, *src, *dst);
    if (!has_ends)
        return std::unexpected(has_ends.error());
    if (!*has_ends)
        return std::optional<std::string>("edge_without_node");
    auto found = edge_exists(txn, *id, *src, *type);
    if (!found)
        return std::unexpected(found.error());
    auto changes = *found && !overwrite ? props_differ(txn, *id, *props) : graph::Result<bool>(false);
    if (!changes)
        return std::unexpected(changes.error());
    if (*changes)
        return std::optional<std::string>(kRefusedExisting);
    if (!*found) {
        NOTEAPP_TRY(txn.put_edge(*id, *src, *type, *dst));
        ++ops;
    }
    NOTEAPP_TRY(set_props(txn, *id, *props, ops, bytes));
    return std::optional<std::string>{};
}

// Notlar sırdır: 0600, dışlayıcı oluşturma; bağ izlenmez, var olan dosyanın üzerine yazılmaz. Dönüş: çıkış
// kodu, dosya açıldıysa boş.
std::optional<int> open_export_file(const std::string& path, std::ofstream& file) {
    const fs::path target = from_utf8(path);
    const core::FileWriteResult created = core::write_new_file(target, {}, true);
    if (created.already_exists) {
        std::cerr << sanitize_for_terminal(core::format_error(core::already_exists(path))) << '\n';
        return kExitUser;
    }
    if (!created.ec)
        file.open(target, std::ios::binary | std::ios::in | std::ios::out);
    if (file.is_open())
        return std::nullopt;
    std::cerr << core::format_error(core::io_error(path, "cannot open file")) << '\n';
    return kExitInternal;
}

int run_graph_export(const std::vector<std::string>& rest) {
    std::optional<std::string> output_path;
    bool jsonl = false;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        if (rest[i] == "--format" && i + 1 < rest.size() && rest[i + 1] == "jsonl") {
            jsonl = true;
            ++i;
        } else if (rest[i] == "-o" && i + 1 < rest.size()) {
            output_path = rest[++i];
        } else {
            std::cerr << core::tr("cli.graph_usage") << '\n';
            return kExitUsage;
        }
    }
    if (!jsonl) {
        std::cerr << core::tr("cli.graph_usage") << '\n';
        return kExitUsage;
    }
    const fs::path db = core::data_dir() / "graph.db";
    std::error_code ec;
    if (!fs::exists(db, ec)) {
        std::cerr << core::tr("cli.query_no_database", {{"path", to_utf8(db)}}) << '\n';
        return kExitUser;
    }
    auto store = graph::Graph::open(db);
    if (!store)
        return fail_graph(store.error(), db);
    std::ofstream file;
    if (output_path)
        if (const auto stop = open_export_file(*output_path, file))
            return stop.value();
    if (auto written = write_export(*store, output_path ? static_cast<std::ostream&>(file) : std::cout);
        !written)
        return fail_graph(written.error(), db);
    return kExitOk;
}

enum class LineRead { Line, End, TooLong };

// getline önce satırın tamamını belleğe alırdı: sınır okurken uygulanır, fazlası atılmadan durulur.
LineRead read_bounded_line(std::istream& input, std::string& line) {
    line.clear();
    std::array<char, 65536> chunk{};
    while (true) {
        input.getline(chunk.data(), chunk.size());
        const std::size_t got = static_cast<std::size_t>(input.gcount());
        const bool complete = !input.fail();  // satır sonu tüketildi; gcount onu sayar
        if (input.eof() || complete) {
            line.append(chunk.data(), complete && !input.eof() ? got - 1 : got);
            if (line.size() > kMaxLineBytes)
                return LineRead::TooLong;
            return input.eof() && line.empty() ? LineRead::End : LineRead::Line;
        }
        line.append(chunk.data(), got);  // parça doldu: failbit
        if (line.size() > kMaxLineBytes)
            return LineRead::TooLong;
        input.clear();
    }
}

// İçe aktarmanın tek işlem partisi; satır satır uygulanır.
class GraphImporter {
public:
    GraphImporter(graph::Graph& store, fs::path db, bool overwrite)
        : store_(store), db_(std::move(db)), overwrite_(overwrite) {}

    // Çıkış kodu: dosya sonuna kadar her şey yolundaysa nullopt.
    std::optional<int> run(std::istream& input) {
        auto txn = store_.write();
        if (!txn)
            return fail_graph(txn.error(), db_);
        std::string line;
        while (true) {
            const LineRead got = read_bounded_line(input, line);
            if (got == LineRead::End)
                break;
            ++line_number_;
            if (got == LineRead::TooLong)
                return bad_line(line_number_, "too_long");
            if (const auto stop = apply_line(*txn, line))
                return stop;
            if (ops_ < kBatchOps && bytes_ < kBatchBytes)
                continue;
            if (auto committed = txn->commit(); !committed)
                return fail_graph(committed.error(), db_);
            txn = store_.write();
            if (!txn)
                return fail_graph(txn.error(), db_);
            ops_ = 0;
            bytes_ = 0;
        }
        if (line_number_ == 0)
            return bad_line(1, "not_a_graph_export");
        if (auto committed = txn->commit(); !committed)
            return fail_graph(committed.error(), db_);
        return std::nullopt;
    }
    std::size_t nodes() const { return nodes_; }
    std::size_t edges() const { return edges_; }
    std::size_t refused() const { return refused_; }

private:
    std::optional<int> check_header(const Json& parsed, const std::string& kind) const {
        if (kind != kFormatTag)
            return bad_line(line_number_, "not_a_graph_export");
        const auto version = parsed.fields.find("v");
        if (version == parsed.fields.end() || version->second.text != "1")
            return bad_line(line_number_, "unknown_version");
        return std::nullopt;
    }
    std::optional<int> apply_line(graph::WriteTxn& txn, const std::string& line) {
        if (line.empty())
            return std::nullopt;
        const auto parsed = JsonParser(line).parse();
        if (!parsed || parsed->kind != Json::Kind::Object)
            return bad_line(line_number_, "not_json");
        const std::string* kind = text_field(*parsed, "k");
        if (kind == nullptr)
            return bad_line(line_number_, "no_kind");
        if (line_number_ == 1)
            return check_header(*parsed, *kind);
        if (*kind != "node" && *kind != "edge")
            return bad_line(line_number_, "unknown_kind");
        const bool is_node = *kind == "node";
        const auto outcome = is_node ? import_node(txn, *parsed, overwrite_, ops_, bytes_)
                                     : import_edge(txn, *parsed, overwrite_, ops_, bytes_);
        if (!outcome)
            return fail_graph(outcome.error(), db_);
        if (*outcome && *outcome == kRefusedExisting) {
            ++refused_;
            std::cerr << core::tr("cli.graph_import_refused", {{"id", *text_field(*parsed, "id")}}) << '\n';
            return std::nullopt;
        }
        if (*outcome)
            return bad_line(line_number_, **outcome);
        ++(is_node ? nodes_ : edges_);
        return std::nullopt;
    }

    graph::Graph& store_;
    fs::path db_;
    bool overwrite_;
    std::size_t refused_ = 0;
    std::size_t line_number_ = 0;
    std::size_t ops_ = 0;
    std::size_t bytes_ = 0;
    std::size_t nodes_ = 0;
    std::size_t edges_ = 0;
};

int run_graph_import(const std::vector<std::string>& rest) {
    const bool overwrite = rest.size() == 3 && rest[1] == "--overwrite";
    if (rest.size() != (overwrite ? 3U : 2U)) {
        std::cerr << core::tr("cli.graph_usage") << '\n';
        return kExitUsage;
    }
    std::ifstream input(from_utf8(rest.back()), std::ios::binary);
    if (!input) {
        std::cerr << core::format_error(core::io_error(rest.back(), "cannot open file")) << '\n';
        return kExitUser;
    }
    const fs::path db = core::data_dir() / "graph.db";
    if (const std::error_code made = core::create_private_directories(db.parent_path()))
        std::cerr << sanitize_for_terminal(
                         core::format_error(core::io_error(to_utf8(db.parent_path()), made.message())))
                  << '\n';
    auto store = graph::Graph::open(db);
    if (!store)
        return fail_graph(store.error(), db);
    GraphImporter importer(*store, db, overwrite);
    if (const auto stop = importer.run(input))
        return *stop;
    std::cout << core::tr("cli.graph_imported", {{"nodes", std::to_string(importer.nodes())},
                                                 {"edges", std::to_string(importer.edges())}})
              << '\n';
    return importer.refused() == 0 ? kExitOk : kExitUser;
}

constexpr std::string_view kIgnoreSkipped = "--ignore-skipped";
constexpr std::string_view kClearSkipped = "--clear-skipped";

void print_skipped(const char* key, const std::vector<core::SkippedFile>& files) {
    for (const core::SkippedFile& file : files)
        std::cerr << sanitize_for_terminal(
                         core::tr(key, {{"path", file.path}, {"reason", core::tr("reason." + file.reason)}}))
                  << '\n';
}

// Geçişte kalan dosyaları söyler: silinirse kaybolurlar. Dönüş çıkış kodu; kullanıcı kabul etmediyse
// kExitUser.
int report_skipped_list(const std::vector<core::SkippedFile>& files, bool ignore_skipped) {
    if (files.empty())
        return kExitOk;
    std::cerr << core::tr("cli.skipped_header", {{"count", std::to_string(files.size())}}) << '\n';
    print_skipped("cli.skipped_file", files);
    if (ignore_skipped)
        return kExitOk;
    std::cerr << core::tr("cli.skipped_exit_hint") << '\n';
    return kExitUser;
}

int report_skipped_files(bool ignore_skipped) {
    const auto files = core::read_skipped_files(core::data_dir());
    if (!files) {
        std::cerr << sanitize_for_terminal(core::format_error(files.error())) << '\n';
        return kExitInternal;
    }
    return report_skipped_list(*files, ignore_skipped);
}

struct RootSkipped {
    int exit_code = kExitOk;
    std::size_t listed = 0;
};

// Status yalnız geçerli kökü denetler; başka kökün kaydı burada çözülemez, yalnız sayısı söylenir.
RootSkipped report_skipped_of_root(const std::filesystem::path& notes_dir, bool ignore_skipped) {
    auto files = core::read_skipped_files(core::data_dir());
    if (!files) {
        std::cerr << sanitize_for_terminal(core::format_error(files.error())) << '\n';
        return {.exit_code = kExitInternal};
    }
    std::vector<core::SkippedFile> mine;
    for (core::SkippedFile& file : *files)
        if (core::skipped_file_is_under(file, notes_dir))
            mine.push_back(std::move(file));
    if (const std::size_t others = files->size() - mine.size(); others > 0)
        std::cerr << core::tr("cli.skipped_other_dirs", {{"count", std::to_string(others)}}) << '\n';
    return {report_skipped_list(mine, ignore_skipped), mine.size()};
}

int run_graph_clear_skipped() {
    const auto files = core::read_skipped_files(core::data_dir());
    if (!files) {
        std::cerr << sanitize_for_terminal(core::format_error(files.error())) << '\n';
        return kExitInternal;
    }
    print_skipped("cli.skipped_file", *files);
    if (auto cleared = core::clear_skipped_files(core::data_dir()); !cleared) {
        std::cerr << sanitize_for_terminal(core::format_error(cleared.error())) << '\n';
        return kExitInternal;
    }
    std::cout << core::tr("cli.skipped_cleared", {{"count", std::to_string(files->size())}}) << '\n';
    return kExitOk;
}

int run_graph_status(const core::Config& config, bool ignore_skipped) {
    const bool pending = core::legacy_notes_pending(config.notes_dir, core::data_dir());
    if (pending)
        std::cerr << sanitize_for_terminal(
                         core::tr("cli.graph_status_pending", {{"path", to_utf8(config.notes_dir)}}))
                  << '\n';
    const RootSkipped skipped = report_skipped_of_root(config.notes_dir, ignore_skipped);
    if (skipped.exit_code != kExitOk)
        return skipped.exit_code;
    if (pending)
        return kExitUser;
    if (skipped.listed > 0)  // kabul edildi, çözülmedi: "kalan yok" denemez
        std::cout << core::tr("cli.graph_status_accepted", {{"count", std::to_string(skipped.listed)}})
                  << '\n';
    else
        std::cout << core::tr("cli.graph_status_ok") << '\n';
    return kExitOk;
}

int run_graph_status_command(const std::vector<std::string>& rest, const core::Config& config) {
    if (rest.size() == 1)
        return run_graph_status(config, false);
    if (rest.size() == 2 && rest[1] == kIgnoreSkipped)
        return run_graph_status(config, true);
    if (rest.size() == 2 && rest[1] == kClearSkipped)
        return run_graph_clear_skipped();
    std::cerr << core::tr("cli.graph_usage") << '\n';
    return kExitUsage;
}

int run_graph_migrate(const core::Config& config, bool skip_failed) {
    const auto report = core::migrate_legacy_notes(config.notes_dir, core::data_dir(), {skip_failed});
    if (!report) {
        std::cerr << sanitize_for_terminal(core::format_error(report.error())) << '\n';
        return kExitInternal;
    }
    std::cout << core::tr("cli.migrate_summary", {{"imported", std::to_string(report->imported)},
                                                  {"unchanged", std::to_string(report->unchanged)},
                                                  {"folders", std::to_string(report->folders)},
                                                  {"backup", sanitize_for_terminal(to_utf8(report->backup))}})
              << '\n';
    for (const core::NotePath& copy : report->conflict_copies)
        std::cout << sanitize_for_terminal(core::tr("cli.sync_conflict_copy", {{"path", copy.string()}}))
                  << '\n';
    print_skipped("cli.migrate_skipped", report->skipped);
    print_skipped("cli.migrate_failed", report->failed);
    std::cout << sanitize_for_terminal(
                     core::tr("cli.migrate_backup_hint", {{"path", to_utf8(report->backup)}}))
              << '\n';
    if (!report->complete) {
        std::cerr << core::tr("cli.migrate_incomplete") << '\n';
        return kExitUser;
    }
    std::error_code ec;
    const bool in_repo = fs::exists(config.notes_dir / ".git", ec);
    std::cout << sanitize_for_terminal(
                     core::tr(in_repo ? "cli.migrate_remove_repo" : "cli.migrate_remove_plain",
                              {{"path", to_utf8(config.notes_dir)}}))
              << '\n';
    if (const std::size_t left = report->skipped.size() + report->failed.size(); left > 0)
        std::cerr << core::tr("cli.migrate_left_behind", {{"count", std::to_string(left)}}) << '\n';
    return kExitOk;
}

}  // namespace

int run_graph(const ParsedArgs& args, const core::Config& config) {
    const std::string sub = args.rest.empty() ? std::string() : args.rest.front();
    if (sub == "migrate" && args.rest.size() == 1)
        return run_graph_migrate(config, false);
    if (sub == "migrate" && args.rest.size() == 2 && args.rest[1] == "--skip-failed")
        return run_graph_migrate(config, true);
    if (sub == "status")
        return run_graph_status_command(args.rest, config);
    if (sub == "export")
        return run_graph_export(args.rest);
    if (sub == "import")
        return run_graph_import(args.rest);
    std::cerr << core::tr("cli.graph_usage") << '\n';
    return kExitUsage;
}

int run_export_all(const std::string& directory, bool ignore_skipped, const core::Config& config) {
    const core::NoteStore store = core::NoteStore::for_config(config);
    const auto report = core::export_all_notes(store, from_utf8(directory));
    if (!report) {
        std::cerr << sanitize_for_terminal(core::format_error(report.error())) << '\n';
        return kExitInternal;
    }
    std::cout << core::tr("cli.export_all_done", {{"count", std::to_string(report->written.size())},
                                                  {"path", sanitize_for_terminal(directory)}})
              << '\n';
    print_skipped("cli.export_all_refused", report->refused);
    for (const core::UnreachableNote& lost : report->unreachable)
        std::cerr << sanitize_for_terminal(core::tr("cli.export_all_unreachable",
                                                    {{"id", lost.id},
                                                     {"name", lost.name},
                                                     {"reason", core::tr("reason." + lost.reason)}}))
                  << '\n';
    const int exported = report->refused.empty() && report->unreachable.empty() ? kExitOk : kExitUser;
    return std::max(exported, report_skipped_files(ignore_skipped));
}

}  // namespace noteapp::cli
