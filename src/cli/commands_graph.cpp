// `note graph export | import` ve `note export --all`. Motor nottan habersizdir: JSONL biçimi
// düğüm, etiket, kenar ve özellikten ibarettir. Biçim (bir satır bir nesne):
//   {"k":"noteapp-graph","v":1}
//   {"k":"node","id":"<32 hex>","labels":["Note"],"props":{"name":{"s":"a"},"body":{"b":"<base64>"}}}
//   {"k":"edge","id":"<32 hex>","src":"<32 hex>","type":"IN_FOLDER","dst":"<32 hex>","props":{}}
// Değer: s metin, b bayt (base64), i tam sayı (metin), f double (metin), t zaman (ms, metin), p bool.
// Eşzamanlı head'lerden yalnız okunan değer yazılır: taşıma biçimidir, senkronun yerine geçmez.
#include "app/map_relations.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/atomic_file.hpp"
#include "core/graph_limits.hpp"
#include "core/i18n.hpp"
#include "core/json_parser.hpp"
#include "core/json_text.hpp"
#include "core/note_export.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "graph/graph.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::cli {

using core::append_json_string;
using core::base64_of;
using core::Json;
using core::JsonParser;

namespace {

namespace fs = std::filesystem;
namespace graph = noteapp::graph;

constexpr const char* kFormatTag = "noteapp-graph";
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

// İçe aktarmanın işlem partisi. Bir satır tek işlemdir (ya hepsi ya hiç): yazmalar yalnız sayılır, işlem
// bütçeyi ya da bayt payını doldurduysa satır bitince commit edilir. Satır çerçeveden çok işlem ya da büyük
// değer taşıyabilir (lead kararı 2, L4b): çerçeveleri yazan böler. Yarıda kalan içe aktarma yinelenince
// yapılmışı atlar; yarım satır kalmaz.
class ImportBatch {
public:
    static graph::Result<ImportBatch> open(graph::Graph& graph, std::size_t max_ops, std::size_t max_bytes) {
        auto txn = graph.write();
        if (!txn)
            return std::unexpected(txn.error());
        return ImportBatch(graph, std::move(*txn), max_ops, max_bytes);
    }

    graph::WriteTxn& txn() { return txn_; }
    // Bir sonraki yazma `bytes` kadar yer tutar; commit satır sonunda (end_line).
    graph::Result<void> make_room(std::size_t bytes) {
        ++ops_;
        bytes_ += bytes;
        return {};
    }
    // Satır bitti: işlem doluysa commit edilir, sonraki satır yeni işlemde başlar.
    graph::Result<void> end_line() {
        if (ops_ < max_ops_ && bytes_ < max_bytes_)
            return {};
        NOTEAPP_TRY(txn_.commit());
        auto next = graph_->write();
        if (!next)
            return std::unexpected(next.error());
        txn_ = std::move(*next);
        ops_ = 0;
        bytes_ = 0;
        return {};
    }
    graph::Result<void> commit() { return txn_.commit(); }

private:
    ImportBatch(graph::Graph& graph, graph::WriteTxn txn, std::size_t max_ops, std::size_t max_bytes)
        : graph_(&graph), txn_(std::move(txn)), max_ops_(std::max<std::size_t>(1, max_ops)),
          max_bytes_(max_bytes) {}

    graph::Graph* graph_;
    graph::WriteTxn txn_;
    std::size_t max_ops_;
    std::size_t max_bytes_;
    std::size_t ops_ = 0;
    std::size_t bytes_ = 0;
};

// Kodlu işlemin değer dışı payı için üst tahmin: iki kimlik (32 bayt), etiket ve uzunluk alanları.
constexpr std::size_t kOpOverheadBytes = 64;

std::size_t value_bytes(const graph::Value& value) {
    if (const auto* text = std::get_if<std::string>(&value))
        return text->size();
    if (const auto* blob = std::get_if<graph::Bytes>(&value))
        return blob->data.size();
    return sizeof(std::int64_t);
}

graph::Result<void> set_props(ImportBatch& batch, const graph::OwnerId& owner,
                              const std::map<std::string, graph::Value>& props) {
    for (const auto& [key, value] : props) {
        auto current = batch.txn().property(owner, key);
        if (!current)
            return std::unexpected(current.error());
        if (*current == value)
            continue;  // tekrar içe aktarma işlem yazmaz
        NOTEAPP_TRY(batch.make_room(kOpOverheadBytes + key.size() + value_bytes(value)));
        if (auto set = batch.txn().set_property(owner, key, value); !set)
            return set;
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

// Sıralı: çok etiketli satırda (çerçeveyi aşan satır) her etiket için doğrusal arama karesel olurdu.
graph::Result<std::vector<std::string>> sorted_labels(graph::WriteTxn& txn, const graph::NodeId& id) {
    auto present = txn.labels(id);
    if (present)
        std::ranges::sort(*present);
    return present;
}

graph::Result<bool> node_would_change(graph::WriteTxn& txn, const graph::NodeId& id, const Json& labels,
                                      const std::map<std::string, graph::Value>& props) {
    auto present = sorted_labels(txn, id);
    if (!present)
        return std::unexpected(present.error());
    for (const Json& label : labels.items)
        if (!std::ranges::binary_search(*present, label.text))
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

graph::Result<void> write_node(ImportBatch& batch, const graph::NodeId& id, const Json& labels,
                               const std::map<std::string, graph::Value>& props) {
    auto present = sorted_labels(batch.txn(), id);
    if (!present)
        return std::unexpected(present.error());
    for (const Json& label : labels.items) {
        if (std::ranges::binary_search(*present, label.text))
            continue;
        NOTEAPP_TRY(batch.make_room(kOpOverheadBytes + label.text.size()));
        NOTEAPP_TRY(batch.txn().add_label(id, label.text));
    }
    return set_props(batch, id, props);
}

bool labels_are_safe(const Json& labels) {
    return std::ranges::all_of(labels.items, [](const Json& label) {
        return label.kind == Json::Kind::String && is_safe_schema_name(label.text);
    });
}

graph::Result<std::optional<std::string>> import_node(ImportBatch& batch, const Json& line, bool overwrite) {
    graph::WriteTxn& txn = batch.txn();
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
        NOTEAPP_TRY(batch.make_room(kOpOverheadBytes));
        NOTEAPP_TRY(batch.txn().put_node(*id));
    }
    NOTEAPP_TRY(write_node(batch, *id, labels->second, *props));
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

graph::Result<std::optional<std::string>> import_edge(ImportBatch& batch, const Json& line, bool overwrite) {
    graph::WriteTxn& txn = batch.txn();
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
        NOTEAPP_TRY(batch.make_room(kOpOverheadBytes + type->size()));
        NOTEAPP_TRY(batch.txn().put_edge(*id, *src, *type, *dst));
    }
    NOTEAPP_TRY(set_props(batch, *id, *props));
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

int run_graph_export(const std::vector<std::string>& rest, const core::Config& config) {
    std::optional<std::string> output_path;
    bool jsonl = false;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        if (rest[i] == "--format" && i + 1 < rest.size() && rest[i + 1] == "jsonl") {
            jsonl = true;
            ++i;
        } else if ((rest[i] == "-o" || rest[i] == "--output") && i + 1 < rest.size()) {
            output_path = rest[++i];
        } else {
            return usage_error("graph");
        }
    }
    if (!jsonl)
        return usage_error("graph");
    const fs::path db = core::data_dir() / "graph.db";
    std::error_code ec;
    if (!fs::exists(db, ec)) {
        std::cerr << core::tr("cli.query_no_database", {{"path", to_utf8(db)}}) << '\n';
        return kExitUser;
    }
    auto store = graph::Graph::open(db, core::graph_limits_of(config.limits.graph));
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

// getline önce satırın tamamını belleğe alırdı: sınır (limits.graph.import_line_bytes) okurken uygulanır,
// fazlası atılmadan durulur.
LineRead read_bounded_line(std::istream& input, std::string& line, std::size_t max_bytes) {
    line.clear();
    std::array<char, 65536> chunk{};
    while (true) {
        input.getline(chunk.data(), chunk.size());
        const std::size_t got = static_cast<std::size_t>(input.gcount());
        const bool complete = !input.fail();  // satır sonu tüketildi; gcount onu sayar
        if (input.eof() || complete) {
            line.append(chunk.data(), complete && !input.eof() ? got - 1 : got);
            if (line.size() > max_bytes)
                return LineRead::TooLong;
            return input.eof() && line.empty() ? LineRead::End : LineRead::Line;
        }
        line.append(chunk.data(), got);  // parça doldu: failbit
        if (line.size() > max_bytes)
            return LineRead::TooLong;
        input.clear();
    }
}

// İçe aktarma satır satır uygulanır; işlemlere ImportBatch satır aralarında böler. Bütçe NoteStore'unkiyle
// aynıdır ([tuning] graph_batch_ops; çerçeve sınırı işlemi sınırlamaz, L4b); bayt payı etkin kayıt
// sınırının yarısıdır (Graph::max_frame_bytes: frame_bytes = 0 da depolamanın tek değer sınırına kırpılır).
class GraphImporter {
public:
    GraphImporter(graph::Graph& store, fs::path db, bool overwrite, const core::Config& config)
        : store_(store), db_(std::move(db)), overwrite_(overwrite),
          max_ops_(config.tuning.graph_batch_ops),
          max_bytes_(store.max_frame_bytes() / 2),
          max_line_bytes_(config.limits.graph.import_line_bytes.cap()),
          max_line_values_(config.limits.graph.import_line_values.cap()) {}

    // Çıkış kodu: dosya sonuna kadar her şey yolundaysa nullopt.
    std::optional<int> run(std::istream& input) {
        auto batch = ImportBatch::open(store_, max_ops_, max_bytes_);
        if (!batch)
            return fail_graph(batch.error(), db_);
        std::string line;
        while (true) {
            const LineRead got = read_bounded_line(input, line, max_line_bytes_);
            if (got == LineRead::End)
                break;
            ++line_number_;
            if (got == LineRead::TooLong)
                return bad_line(line_number_, "too_long");
            if (const auto stop = apply_line(*batch, line))
                return stop;
            if (auto ended = batch->end_line(); !ended)
                return fail_graph(ended.error(), db_);
        }
        if (line_number_ == 0)
            return bad_line(1, "not_a_graph_export");
        if (auto committed = batch->commit(); !committed)
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
    std::optional<int> apply_line(ImportBatch& batch, const std::string& line) {
        if (line.empty())
            return std::nullopt;
        const auto parsed = JsonParser(line, {.max_values = max_line_values_}).parse();
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
        const auto outcome =
            is_node ? import_node(batch, *parsed, overwrite_) : import_edge(batch, *parsed, overwrite_);
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
    std::size_t max_ops_;
    std::size_t max_bytes_;
    std::size_t max_line_bytes_;
    std::size_t max_line_values_;
    std::size_t refused_ = 0;
    std::size_t line_number_ = 0;
    std::size_t nodes_ = 0;
    std::size_t edges_ = 0;
};

int run_graph_import(const std::vector<std::string>& rest, const core::Config& config) {
    const bool overwrite = rest.size() == 3 && rest[1] == "--overwrite";
    if (rest.size() != (overwrite ? 3U : 2U))
        return usage_error("graph");
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
    auto store = graph::Graph::open(db, core::graph_limits_of(config.limits.graph));
    if (!store)
        return fail_graph(store.error(), db);
    GraphImporter importer(*store, db, overwrite, config);
    if (const auto stop = importer.run(input))
        return *stop;
    std::cout << core::tr("cli.graph_imported", {{"nodes", std::to_string(importer.nodes())},
                                                 {"edges", std::to_string(importer.edges())}})
              << '\n';
    return importer.refused() == 0 ? kExitOk : kExitUser;
}

}  // namespace

int run_graph(const ParsedArgs& args, const core::Config& config) {
    const std::string sub = args.rest.empty() ? std::string() : args.rest.front();
    if (sub == "export")
        return run_graph_export(args.rest, config);
    if (sub == "import")
        return run_graph_import(args.rest, config);
    return usage_error("graph");
}

namespace {

// --- note relation (SPEC-cli "İlişkiler"): kural NoteStore'un, ileti app::map_relations'ın (TUI/GUI ile aynı). ---

int relation_usage() {
    return usage_error("relation");
}

struct RelationArgs {
    std::vector<std::string> positional;
    std::optional<std::string> name;
    std::optional<std::string> id;
    bool directed = false;
    bool json = false;
};

// `allowed`: fiilin seçenekleri. `--` sonrası her şey konumludur (`-` ile başlayan ad böyle verilir).
std::expected<RelationArgs, int> parse_relation_args(const std::vector<std::string>& rest,
                                                     std::span<const std::string_view> allowed) {
    RelationArgs parsed;
    bool only_positional = false;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_positional || arg.empty() || arg[0] != '-') {
            parsed.positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_positional = true;
            continue;
        }
        if (std::ranges::find(allowed, arg) == allowed.end())
            return std::unexpected(unknown_option_error("relation", arg));
        if (arg == "--directed") {
            parsed.directed = true;
        } else if (arg == "--json") {
            parsed.json = true;
        } else if (i + 1 >= rest.size()) {
            return std::unexpected(missing_value_error("relation", arg));
        } else {
            (arg == "--name" ? parsed.name : parsed.id) = rest[++i];
        }
    }
    return parsed;
}

std::expected<core::NotePath, int> relation_note(const std::string& text) {
    auto path = core::NotePath::parse(text);
    if (!path)
        return std::unexpected(fail(path.error()));
    return std::move(*path);
}

app::MapNode map_note(const core::NotePath& path) {
    return {app::MapNodeKind::Note, path.string()};
}

std::string_view direction_word(core::RelationDirection direction) {
    switch (direction) {
    case core::RelationDirection::Outgoing:
        return "outgoing";
    case core::RelationDirection::Incoming:
        return "incoming";
    case core::RelationDirection::Undirected:
        break;
    }
    return "undirected";
}

// A ile B arasındaki tek ilişki: --id, --name (tag_key ile, boş = adsız) ya da çiftin tek ilişkisi.
std::expected<core::RelatedNote, int> pick_relation(const core::NoteStore& store, const core::NotePath& first,
                                                    const core::NotePath& second, const RelationArgs& args) {
    const auto links = store.links(first);
    if (!links)
        return std::unexpected(fail(links.error()));
    std::vector<core::RelatedNote> rows;
    std::ranges::copy_if(links->related, std::back_inserter(rows),
                         [&](const core::RelatedNote& row) { return row.path == second; });
    const auto pair_args = [&](std::vector<std::pair<std::string, std::string>> more) {
        more.emplace_back("from", first.string());
        more.emplace_back("to", second.string());
        return more;
    };
    if (args.id) {
        const auto id = id_from_hex<core::RelationId>(*args.id);
        if (!id) {
            std::cerr << sanitize_for_terminal(core::tr("cli.relation_bad_id", {{"id", *args.id}})) << '\n';
            return std::unexpected(kExitUsage);
        }
        const auto found = std::ranges::find(rows, *id, &core::RelatedNote::id);
        if (found == rows.end())
            return std::unexpected(fail(core::Error{core::Error::Code::NotFound, "error.relation_not_found", {}}));
        return *found;
    }
    if (args.name) {
        const auto name = core::relation_name(*args.name, SIZE_MAX);
        if (!name)
            return std::unexpected(fail(name.error()));
        const std::string key = core::tag_key(*name);
        const auto found =
            std::ranges::find_if(rows, [&](const core::RelatedNote& row) { return core::tag_key(row.name) == key; });
        if (found == rows.end()) {
            std::cerr << sanitize_for_terminal(core::tr("cli.relation_no_name", pair_args({{"name", *name}}))) << '\n';
            return std::unexpected(kExitUser);
        }
        return *found;
    }
    if (rows.size() == 1)
        return rows.front();
    const char* key = rows.empty() ? "cli.relation_none" : "cli.relation_ambiguous";
    std::cerr << sanitize_for_terminal(core::tr(key, pair_args({{"count", std::to_string(rows.size())}}))) << '\n';
    return std::unexpected(kExitUser);
}

int print_relation_done(const std::expected<std::string, core::Error>& done, const core::Config& config) {
    if (!done)
        return fail(done.error());
    std::cout << sanitize_for_terminal(*done) << '\n';
    start_background_sync(config, false);
    return kExitOk;
}

int run_relation_add(const RelationArgs& args, core::NoteStore& store, const core::Config& config) {
    if (args.positional.size() != 2)
        return relation_usage();
    const auto first = relation_note(args.positional[0]);
    if (!first)
        return first.error();
    const auto second = relation_note(args.positional[1]);
    if (!second)
        return second.error();
    return print_relation_done(app::relate_map_notes(store, map_note(*first), map_note(*second),
                                                     {args.name.value_or(""), args.directed}),
                               config);
}

int run_relation_rm(const RelationArgs& args, core::NoteStore& store, const core::Config& config) {
    if (args.positional.size() != 2 || (args.name && args.id))
        return relation_usage();
    const auto first = relation_note(args.positional[0]);
    if (!first)
        return first.error();
    const auto second = relation_note(args.positional[1]);
    if (!second)
        return second.error();
    const auto picked = pick_relation(store, *first, *second, args);
    if (!picked)
        return picked.error();
    return print_relation_done(app::unrelate_map_notes(store, map_note(*first), map_note(*second), picked->id),
                               config);
}

int run_relation_rename(const RelationArgs& args, core::NoteStore& store, const core::Config& config) {
    if (args.positional.size() != 4)
        return relation_usage();
    const auto first = relation_note(args.positional[0]);
    if (!first)
        return first.error();
    const auto second = relation_note(args.positional[1]);
    if (!second)
        return second.error();
    RelationArgs old_name;
    old_name.name = args.positional[2];
    const auto picked = pick_relation(store, *first, *second, old_name);
    if (!picked)
        return picked.error();
    return print_relation_done(
        app::rename_map_relation(store, *first, *second, picked->id, args.positional[3]), config);
}

// Adlar: GÖSTERİM<TAB>SAYI (note tags gibi); --json: name, display, count, directed.
int run_relation_names(const RelationArgs& args, const core::NoteStore& store) {
    const auto names = store.relation_names();
    if (!names)
        return fail(names.error());
    if (!args.json) {
        for (const core::RelationName& name : **names)
            std::cout << sanitize_for_terminal(name.display) << '\t' << name.count << '\n';
        return kExitOk;
    }
    std::string out = "[";
    for (const core::RelationName& name : **names) {
        out += out.size() > 1 ? ",{\"name\":" : "{\"name\":";
        append_json_string(out, name.key);
        out += ",\"display\":";
        append_json_string(out, name.display);
        out += ",\"count\":" + std::to_string(name.count) + ",\"directed\":" + std::to_string(name.directed) + "}";
    }
    std::cout << out << "]\n";
    return kExitOk;
}

// Notun ilişkileri: YOL<TAB>AD<TAB>YÖN<TAB>KİMLİK (kimlik `rm --id` içindir); --json aynı alanlar.
int run_relation_ls(const RelationArgs& args, const core::NoteStore& store) {
    if (args.positional.empty())
        return run_relation_names(args, store);
    if (args.positional.size() != 1)
        return relation_usage();
    const auto note = relation_note(args.positional[0]);
    if (!note)
        return note.error();
    const auto links = store.links(*note);
    if (!links)
        return fail(links.error());
    if (!args.json) {
        for (const core::RelatedNote& row : links->related)
            std::cout << sanitize_for_terminal(row.path.string()) << '\t' << sanitize_for_terminal(row.name) << '\t'
                      << direction_word(row.direction) << '\t' << hex_of(row.id.bytes) << '\n';
        return kExitOk;
    }
    std::string out = "[";
    for (const core::RelatedNote& row : links->related) {
        out += out.size() > 1 ? ",{\"path\":" : "{\"path\":";
        append_json_string(out, row.path.string());
        out += ",\"name\":";
        append_json_string(out, row.name);
        out += ",\"direction\":\"" + std::string(direction_word(row.direction)) + "\",\"id\":\"" +
               hex_of(row.id.bytes) + "\"}";
    }
    std::cout << out << "]\n";
    return kExitOk;
}

}  // namespace

std::optional<core::RelationId> relation_id_from_hex(std::string_view text) {
    return id_from_hex<core::RelationId>(text);
}

int run_relation(const ParsedArgs& args, const core::Config& config) {
    static constexpr std::array<std::string_view, 2> kRelationAdd{"--name", "--directed"};
    static constexpr std::array<std::string_view, 2> kRelationRm{"--name", "--id"};
    static constexpr std::array<std::string_view, 1> kRelationLs{"--json"};
    const std::string verb = args.rest.empty() ? std::string() : args.rest.front();
    if (verb == "prop")  // `relation prop get|set|rm KİMLİK …` (F4)
        return run_relation_prop(std::span(args.rest).subspan(1), config);
    std::span<const std::string_view> allowed;
    if (verb == "add")
        allowed = kRelationAdd;
    else if (verb == "rm")
        allowed = kRelationRm;
    else if (verb == "ls")
        allowed = kRelationLs;
    else if (verb != "rename")
        return relation_usage();
    const auto parsed = parse_relation_args(args.rest, allowed);
    if (!parsed)
        return parsed.error();
    core::NoteStore store = core::NoteStore::for_config(config);
    if (verb == "add")
        return run_relation_add(*parsed, store, config);
    if (verb == "rm")
        return run_relation_rm(*parsed, store, config);
    if (verb == "rename")
        return run_relation_rename(*parsed, store, config);
    return run_relation_ls(*parsed, store);
}

int run_export_all(const std::string& directory, const core::Config& config) {
    const core::NoteStore store = core::NoteStore::for_config(config);
    const auto report = core::export_all_notes(store, from_utf8(directory));
    if (!report) {
        std::cerr << sanitize_for_terminal(core::format_error(report.error())) << '\n';
        return kExitInternal;
    }
    std::cout << core::tr("cli.export_all_done", {{"count", std::to_string(report->written.size())},
                                                  {"path", sanitize_for_terminal(directory)}})
              << '\n';
    for (const core::RefusedFile& file : report->refused)
        std::cerr << sanitize_for_terminal(
                         core::tr("cli.export_all_refused",
                                  {{"path", file.path}, {"reason", core::tr("reason." + file.reason)}}))
                  << '\n';
    for (const core::UnreachableNote& lost : report->unreachable)
        std::cerr << sanitize_for_terminal(core::tr("cli.export_all_unreachable",
                                                    {{"id", lost.id},
                                                     {"name", lost.name},
                                                     {"reason", core::tr("reason." + lost.reason)}}))
                  << '\n';
    return report->refused.empty() && report->unreachable.empty() ? kExitOk : kExitUser;
}

}  // namespace noteapp::cli
