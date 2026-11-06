// Not ve ilişki özellikleri (SPEC-graph "Özellikler"): okuma, korumalı yazma, değer metni, `prop:` araması.
#include "core/note_props.hpp"

#include "core/note_store_impl.hpp"
#include "graph/op.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <ranges>
#include <system_error>
#include <tuple>

namespace noteapp::core {

namespace {

using graph::EdgeId;
using graph::NodeId;

constexpr std::string_view kTextPrefix = "text:";
constexpr std::string_view kNumberPrefix = "number:";
constexpr std::string_view kBoolPrefix = "bool:";
constexpr std::array kPrefixes = {kTextPrefix, kNumberPrefix, kBoolPrefix};

std::string reason_of(const graph::Error& error) {
    const auto found =
        std::ranges::find(error.args, std::string("reason"), &std::pair<std::string, std::string>::first);
    return found == error.args.end() ? std::string() : found->second;
}

Error key_error(std::string_view key, std::string_view reason) {
    return Error{Error::Code::InvalidPath,
                 "error.property_key",
                 {{"key", std::string(key)}, {"reason", std::string(reason)}}};
}

Error value_error(std::string_view reason) {
    return Error{Error::Code::InvalidPath, "error.property_value", {{"reason", std::string(reason)}}};
}

Error relation_not_found() {
    return Error{Error::Code::NotFound, "error.relation_not_found", {}};
}

// Yazmanın kuralı; gösterimin salt-okunur kararı da buradan (null deneme değeriyle). Sıra: anahtar, ilişkinin
// kendi anahtarları, sorgu çubuğunun koruması (aynı kanca), değer.
std::expected<void, Error> check_write(graph::ReadTxn& txn, const graph::OwnerId& owner, std::string_view key,
                                       const graph::Value& value, std::size_t name_bytes) {
    if (const auto valid = graph::detail::validate_name(key); !valid)
        return std::unexpected(key_error(key, reason_of(valid.error())));
    if (std::holds_alternative<EdgeId>(owner) && (key == kName || key == kDirected))
        return std::unexpected(
            Error{Error::Code::InvalidPath, "error.property_relation_reserved", {{"key", std::string(key)}}});
    const graph::QueryChange change{graph::QueryChange::Kind::SetProperty, owner, key, &value};
    if (const auto allowed = check_user_change(txn, change, name_bytes); !allowed) {
        if (allowed.error().code != graph::Error::Code::InvalidArgument)
            return std::unexpected(from_graph(allowed.error(), {}));
        return std::unexpected(
            Error{Error::Code::InvalidPath, "error.property_reserved", {{"key", std::string(key)}}});
    }
    if (const auto valid = graph::detail::validate_value(value); !valid)
        return std::unexpected(value_error(reason_of(valid.error())));
    return {};
}

// Eşzamanlı head'lerden kazanan: en büyük (hlc, device), ReadTxn::property'nin kuralı.
graph::Result<std::vector<Property>> read_properties(graph::ReadTxn& txn, const graph::OwnerId& owner,
                                                     std::size_t name_bytes) {
    auto heads = txn.properties(owner);  // (key, device, seq) sırası
    if (!heads)
        return std::unexpected(heads.error());
    std::vector<Property> out;
    for (auto first = heads->begin(); first != heads->end();) {
        const auto last = std::find_if(
            first, heads->end(), [&](const graph::PropertyHead& head) { return head.key != first->key; });
        const auto winner =
            std::max_element(first, last, [](const graph::PropertyHead& a, const graph::PropertyHead& b) {
                return std::tie(a.hlc, a.device) < std::tie(b.hlc, b.device);
            });
        if (winner->large || !std::holds_alternative<std::monostate>(winner->value)) {
            const graph::Value probe;  // silme denemesi: türden bağımsız koruma kararı
            const graph::QueryChange change{graph::QueryChange::Kind::SetProperty, owner, winner->key,
                                            &probe};
            const bool relation_key =
                std::holds_alternative<EdgeId>(owner) && (winner->key == kName || winner->key == kDirected);
            const auto allowed = check_user_change(txn, change, name_bytes);
            if (!allowed && allowed.error().code != graph::Error::Code::InvalidArgument)
                return std::unexpected(allowed.error());
            out.push_back(
                Property{winner->key, winner->value, winner->size, winner->large, relation_key || !allowed});
        }
        first = last;
    }
    return out;
}

// Aynı değer yeniden yazılmaz (günlük büyümez); olmayanı silmek NotFound.
std::expected<void, Error> apply(graph::WriteTxn& txn, const graph::OwnerId& owner, std::string_view key,
                                 const graph::Value& value, std::string_view path) {
    const auto current = txn.property(owner, key);
    if (!current)
        return std::unexpected(from_graph(current.error(), path));
    if (std::holds_alternative<std::monostate>(value) && std::holds_alternative<std::monostate>(*current))
        return std::unexpected(
            Error{Error::Code::NotFound, "error.property_not_found", {{"key", std::string(key)}}});
    if (*current == value)
        return {};
    NOTEAPP_TRY_GRAPH(txn.set_property(owner, key, value), path);
    return {};
}

// Canlı iki not arasındaki RELATED_TO; değilse NotFound.
graph::Result<bool> live_relation(graph::ReadTxn& txn, const Snapshot& snapshot, const EdgeId& id) {
    const auto ends = txn.edge(id);
    if (!ends)
        return std::unexpected(ends.error());
    return *ends && (*ends)->type == kRelatedTo && (*ends)->src != (*ends)->dst &&
           snapshot.live_by_id.contains((*ends)->src) && snapshot.live_by_id.contains((*ends)->dst);
}

template <class Number> std::optional<Number> whole_number(std::string_view text) {
    Number number{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (code != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return number;
}

std::optional<graph::Value> number_of(std::string_view text) {
    if (const auto integer = whole_number<std::int64_t>(text))
        return graph::Value{*integer};
    if (const auto real = whole_number<double>(text); real && std::isfinite(*real))
        return graph::Value{*real};
    return std::nullopt;
}

std::optional<graph::Value> bool_of(std::string_view text) {
    if (text == "true")
        return graph::Value{true};
    if (text == "false")
        return graph::Value{false};
    return std::nullopt;
}

graph::Value automatic(std::string_view text) {
    if (auto flag = bool_of(text))
        return std::move(*flag);
    if (auto number = number_of(text))
        return std::move(*number);
    return graph::Value{std::string(text)};
}

std::string real_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text = code == std::errc{} ? std::string(buffer.data(), end) : std::string();
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";  // "1" tamsayı okunurdu: tür gidiş-dönüşte korunur
    return text;
}

}  // namespace

std::expected<graph::Value, Error> parse_property(std::string_view text, std::optional<PropertyType> type) {
    if (!type) {
        for (const std::string_view prefix : kPrefixes)
            if (text.starts_with(prefix))
                return parse_property(text.substr(prefix.size()), prefix == kTextPrefix ? PropertyType::Text
                                                                  : prefix == kNumberPrefix
                                                                      ? PropertyType::Number
                                                                      : PropertyType::Bool);
        return automatic(text);
    }
    switch (*type) {
    case PropertyType::Number:
        if (auto number = number_of(text))
            return std::move(*number);
        return std::unexpected(value_error("not_number"));
    case PropertyType::Bool:
        if (auto flag = bool_of(text))
            return std::move(*flag);
        return std::unexpected(value_error("not_bool"));
    case PropertyType::Text:
        break;
    }
    return graph::Value{std::string(text)};
}

// 0000–9999 dışı ham milisaniyedir: chrono uç değerde fırlatmaz ama taşar (INT64_MIN "10850-06-21" çıkar,
// ölçüldü) ve %Y bu aralık dışında dört haneyi aşar.
std::string timestamp_text(std::int64_t unix_ms) {
    constexpr std::int64_t kFirstMs = -62'167'219'200'000;  // 0000-01-01T00:00:00Z
    constexpr std::int64_t kLastMs = 253'402'300'799'999;   // 9999-12-31T23:59:59.999Z
    if (unix_ms < kFirstMs || unix_ms > kLastMs)
        return "<time:" + std::to_string(unix_ms) + ">";
    return std::format("{:%F %T}",
                       std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(unix_ms)));
}

std::string property_text(const graph::Value& value) {
    return std::visit(
        []<class T>(const T& v) -> std::string {
            if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return real_text(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, graph::Bytes>)
                return "<bytes:" + std::to_string(v.data.size()) + ">";
            else if constexpr (std::is_same_v<T, graph::Timestamp>)
                return timestamp_text(v.unix_ms);
            else
                return "null";
        },
        value);
}

std::string property_input(const graph::Value& value) {
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        return property_text(value);
    const bool prefixed =
        std::ranges::any_of(kPrefixes, [&](std::string_view prefix) { return text->starts_with(prefix); });
    if (prefixed || !std::holds_alternative<std::string>(automatic(*text)))
        return std::string(kTextPrefix) + *text;
    return *text;
}

std::optional<PropertyType> property_type(const graph::Value& value) {
    if (std::holds_alternative<std::string>(value))
        return PropertyType::Text;
    if (std::holds_alternative<std::int64_t>(value) || std::holds_alternative<double>(value))
        return PropertyType::Number;
    if (std::holds_alternative<bool>(value))
        return PropertyType::Bool;
    return std::nullopt;
}

std::string_view property_type_name(PropertyType type) {
    switch (type) {
    case PropertyType::Number:
        return "number";
    case PropertyType::Bool:
        return "bool";
    case PropertyType::Text:
        break;
    }
    return "text";
}

std::optional<PropertyType> property_type_named(std::string_view name) {
    for (const PropertyType type : {PropertyType::Text, PropertyType::Number, PropertyType::Bool})
        if (property_type_name(type) == name)
            return type;
    return std::nullopt;
}

std::expected<std::vector<Property>, Error> note_properties(const NoteStore& store, const NotePath& note) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    const auto snapshot = impl.current();
    if (!snapshot)
        return std::unexpected(impl.last_error ? *impl.last_error : not_found(note.string()));
    const auto row = snapshot->live_by_path.find(note.string());
    if (row == snapshot->live_by_path.end())
        return std::unexpected(not_found(note.string()));
    auto txn = impl.opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), note.string()));
    auto properties = read_properties(*txn, snapshot->live[row->second].id, impl.relation_name_bytes);
    if (!properties)
        return std::unexpected(from_graph(properties.error(), note.string()));
    return std::move(*properties);
}

std::expected<std::vector<Property>, Error> relation_properties(const NoteStore& store,
                                                                const RelationId& relation) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    const auto snapshot = impl.current();
    if (!snapshot)
        return std::unexpected(impl.last_error ? *impl.last_error : relation_not_found());
    auto txn = impl.opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), {}));
    const EdgeId id{relation.bytes};
    const auto live = live_relation(*txn, *snapshot, id);
    if (!live)
        return std::unexpected(from_graph(live.error(), {}));
    if (!*live)
        return std::unexpected(relation_not_found());
    auto properties = read_properties(*txn, id, impl.relation_name_bytes);
    if (!properties)
        return std::unexpected(from_graph(properties.error(), {}));
    return std::move(*properties);
}

std::expected<void, Error> set_note_property(NoteStore& store, const NotePath& note, std::string_view key,
                                             const graph::Value& value) {
    NoteStore::Impl& impl = store.impl();
    const std::string path = note.string();
    return impl.write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        const auto row = context.snapshot.live_by_path.find(path);
        if (row == context.snapshot.live_by_path.end())
            return std::unexpected(not_found(path));
        const NodeId id = context.snapshot.live[row->second].id;
        NOTEAPP_TRY(check_write(context.txn, id, key, value, impl.relation_name_bytes));
        return apply(context.txn, id, key, value, path);
    });
}

std::expected<void, Error> set_relation_property(NoteStore& store, const RelationId& relation,
                                                 std::string_view key, const graph::Value& value) {
    NoteStore::Impl& impl = store.impl();
    const EdgeId id{relation.bytes};
    return impl.write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        const auto live = live_relation(context.txn, context.snapshot, id);
        if (!live)
            return std::unexpected(from_graph(live.error(), {}));
        if (!*live)
            return std::unexpected(relation_not_found());
        NOTEAPP_TRY(check_write(context.txn, id, key, value, impl.relation_name_bytes));
        return apply(context.txn, id, key, value, {});
    });
}

std::expected<std::shared_ptr<const PropertyNotes>, Error> notes_with_property(const NoteStore& store,
                                                                               std::string_view key) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    const auto snapshot = impl.current();
    if (!snapshot) {
        if (impl.last_error)
            return std::unexpected(*impl.last_error);
        return std::make_shared<const PropertyNotes>();
    }
    const std::string wanted(key);
    auto& order = snapshot->property_order;
    if (const auto cached = snapshot->property_notes.find(wanted); cached != snapshot->property_notes.end()) {
        if (const auto used = std::ranges::find(order, wanted); used != order.end())
            std::rotate(used, used + 1, order.end());  // yeniden kullanılan en yeni olur
        return cached->second;
    }
    PropertyNotes notes;
    // Uygulamanın anahtarları aranmaz: kendi süzgeçleri var (type:, created:) ve `body` her notun gövdesini
    // okuturdu.
    if (!protected_note_key(wanted)) {
        auto txn = impl.opened_graph().read();
        if (!txn)
            return std::unexpected(from_graph(txn.error(), {}));
        // Büyük değer yüklenmez: metni boyutudur (denetçinin gösterimi), `prop:k` yine eşleşir.
        for (const NoteRow& row : snapshot->live) {
            const auto head = txn->property_head(row.id, wanted);
            if (!head)
                return std::unexpected(from_graph(head.error(), row.info.path.string()));
            if (!*head)
                continue;
            if ((*head)->large)
                notes.emplace_back(row.info.path, "<" + std::to_string((*head)->size) + " bytes>");
            else if (!std::holds_alternative<std::monostate>((*head)->value))
                notes.emplace_back(row.info.path, property_text((*head)->value));
        }
        std::ranges::sort(notes, {}, &std::pair<NotePath, std::string>::first);
    }
    auto shared = std::make_shared<const PropertyNotes>(std::move(notes));
    // Yazdıkça her kısmi anahtar (`prop:s`, `prop:st`…) bir girdidir: en eski kullanılan düşer. Verilen liste
    // paylaşılır, düşen girdi onu kullananı geçersiz kılmaz.
    while (!order.empty() && order.size() >= impl.property_cache_keys) {
        snapshot->property_notes.erase(order.front());
        order.erase(order.begin());
    }
    snapshot->property_notes.emplace(wanted, shared);
    order.push_back(wanted);
    return shared;
}

}  // namespace noteapp::core
