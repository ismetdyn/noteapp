#include "graph/cypher/expr.hpp"

#include "graph/store.hpp"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::graph::detail::cypher {

namespace {

using Tri = std::optional<bool>;

Error arithmetic_error(std::string reason, Position pos) {
    return located_error(Error::Code::InvalidArgument, "graph.cypher_arithmetic", pos,
                         {{"reason", std::move(reason)}});
}

QueryValue from_tri(Tri value) {
    return value ? QueryValue(*value) : QueryValue();
}

Tri tri_of(const QueryValue& value) {
    if (const bool* flag = std::get_if<bool>(&value.v))
        return *flag;
    return std::nullopt;
}

bool is_number(const QueryValue& value) {
    return std::holds_alternative<std::int64_t>(value.v) || std::holds_alternative<double>(value.v);
}

// int64'ü double'a çevirmek 2^53 üstünde bilgi kaybeder: kesin karşılaştırma.
int compare_int_double(std::int64_t integer, double real) {
    constexpr double kTwoTo63 = 9223372036854775808.0;
    if (real >= kTwoTo63)
        return -1;
    if (real < -kTwoTo63)
        return 1;
    const double whole = std::trunc(real);
    const auto truncated = static_cast<std::int64_t>(whole);
    if (integer != truncated)
        return integer < truncated ? -1 : 1;
    const double fraction = real - whole;
    if (fraction == 0)
        return 0;
    return fraction > 0 ? -1 : 1;
}

// NaN'de nullopt: kendine bile eşit değildir.
std::optional<int> compare_numbers(const QueryValue& left, const QueryValue& right) {
    const std::int64_t* left_int = std::get_if<std::int64_t>(&left.v);
    const std::int64_t* right_int = std::get_if<std::int64_t>(&right.v);
    if (left_int != nullptr && right_int != nullptr)
        return *left_int < *right_int ? -1 : (*left_int > *right_int ? 1 : 0);
    const double* left_real = std::get_if<double>(&left.v);
    const double* right_real = std::get_if<double>(&right.v);
    if ((left_real != nullptr && std::isnan(*left_real)) ||
        (right_real != nullptr && std::isnan(*right_real)))
        return std::nullopt;
    if (left_int != nullptr)
        return compare_int_double(*left_int, *right_real);
    if (right_int != nullptr)
        return -compare_int_double(*right_int, *left_real);
    return *left_real < *right_real ? -1 : (*left_real > *right_real ? 1 : 0);
}

int compare_bytes(std::string_view left, std::string_view right) {
    const int order = left.compare(right);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

// Aynı karşılaştırılabilir tipteki değerlerin sırası; karşılaştırılamazsa nullopt.
std::optional<int> compare_same_type(const QueryValue& left, const QueryValue& right) {
    if (is_number(left) && is_number(right))
        return compare_numbers(left, right);
    if (left.v.index() != right.v.index())
        return std::nullopt;
    if (const auto* text = std::get_if<std::string>(&left.v))
        return compare_bytes(*text, std::get<std::string>(right.v));
    if (const auto* bytes = std::get_if<Bytes>(&left.v))
        return compare_bytes(bytes->data, std::get<Bytes>(right.v).data);
    if (const auto* flag = std::get_if<bool>(&left.v))
        return static_cast<int>(*flag) - static_cast<int>(std::get<bool>(right.v));
    if (const auto* time = std::get_if<Timestamp>(&left.v)) {
        const std::int64_t other = std::get<Timestamp>(right.v).unix_ms;
        return time->unix_ms < other ? -1 : (time->unix_ms > other ? 1 : 0);
    }
    return std::nullopt;
}

Tri lists_equal(const QueryList& left, const QueryList& right) {
    if (left.items.size() != right.items.size())
        return false;
    bool unknown = false;
    for (std::size_t i = 0; i < left.items.size(); ++i) {
        const Tri equal = values_equal(left.items[i], right.items[i]);
        if (equal && !*equal)
            return false;
        unknown = unknown || !equal;
    }
    return unknown ? Tri{} : Tri{true};
}

QueryValue compare_op(BinaryOp op, const QueryValue& left, const QueryValue& right) {
    const std::optional<int> order = compare_same_type(left, right);
    if (!order)
        return {};
    switch (op) {
    case BinaryOp::Lt:
        return *order < 0;
    case BinaryOp::Gt:
        return *order > 0;
    case BinaryOp::Le:
        return *order <= 0;
    default:
        return *order >= 0;
    }
}

QueryValue membership(const QueryValue& needle, const QueryValue& haystack) {
    const auto* list = std::get_if<QueryList>(&haystack.v);
    if (list == nullptr)
        return {};
    if (list->items.empty())
        return false;
    bool unknown = false;
    for (const QueryValue& item : list->items) {
        const Tri equal = values_equal(needle, item);
        if (equal && *equal)
            return true;
        unknown = unknown || !equal;
    }
    return unknown ? QueryValue() : QueryValue(false);
}

std::optional<std::pair<std::string_view, std::string_view>> text_pair(const QueryValue& left,
                                                                       const QueryValue& right) {
    if (const auto* text = std::get_if<std::string>(&left.v))
        if (const auto* other = std::get_if<std::string>(&right.v))
            return std::pair<std::string_view, std::string_view>(*text, *other);
    if (const auto* bytes = std::get_if<Bytes>(&left.v))
        if (const auto* other = std::get_if<Bytes>(&right.v))
            return std::pair<std::string_view, std::string_view>(bytes->data, other->data);
    return std::nullopt;
}

QueryValue text_predicate(BinaryOp op, const QueryValue& left, const QueryValue& right) {
    const auto pair = text_pair(left, right);
    if (!pair)
        return {};
    const auto& [text, part] = *pair;
    if (op == BinaryOp::Contains)
        return text.find(part) != std::string_view::npos;
    return op == BinaryOp::StartsWith ? text.starts_with(part) : text.ends_with(part);
}

Result<QueryValue> int_add_sub_mul(BinaryOp op, std::int64_t left, std::int64_t right, Position pos) {
    std::int64_t result = 0;
    bool overflow = false;
    if (op == BinaryOp::Add)
        overflow = __builtin_add_overflow(left, right, &result);
    else if (op == BinaryOp::Sub)
        overflow = __builtin_sub_overflow(left, right, &result);
    else
        overflow = __builtin_mul_overflow(left, right, &result);
    if (overflow)
        return std::unexpected(arithmetic_error("overflow", pos));
    return QueryValue(result);
}

Result<QueryValue> int_div_mod(BinaryOp op, std::int64_t left, std::int64_t right, Position pos) {
    if (right == 0)
        return std::unexpected(arithmetic_error("division_by_zero", pos));
    // En küçük int64 / -1 donanımda tuzağa düşer (UB); % -1 de aynı bölmeyi yapar.
    if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
        return std::unexpected(arithmetic_error("overflow", pos));
    return QueryValue(op == BinaryOp::Div ? left / right : left % right);
}

double real_of(const QueryValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.v))
        return static_cast<double>(*integer);
    return std::get<double>(value.v);
}

double real_arithmetic(BinaryOp op, double left, double right) {
    switch (op) {
    case BinaryOp::Add:
        return left + right;
    case BinaryOp::Sub:
        return left - right;
    case BinaryOp::Mul:
        return left * right;
    case BinaryOp::Div:
        return left / right;
    default:
        return std::fmod(left, right);
    }
}

Result<QueryValue> arithmetic(const Expr& expr, const QueryValue& left, const QueryValue& right) {
    if (!is_number(left) || !is_number(right))
        return QueryValue{};
    const auto* left_int = std::get_if<std::int64_t>(&left.v);
    const auto* right_int = std::get_if<std::int64_t>(&right.v);
    if (left_int != nullptr && right_int != nullptr) {
        if (expr.op == BinaryOp::Div || expr.op == BinaryOp::Mod)
            return int_div_mod(expr.op, *left_int, *right_int, expr.pos);
        return int_add_sub_mul(expr.op, *left_int, *right_int, expr.pos);
    }
    const double result = real_arithmetic(expr.op, real_of(left), real_of(right));
    // Sonsuz ve NaN üretilmez: saklanamaz, sıralamayı ve JSON'u bozar; hata açık konuşur.
    if (!std::isfinite(result))
        return std::unexpected(arithmetic_error("overflow", expr.pos));
    return QueryValue(result);
}

Result<QueryValue> eval_logical(const Expr& expr, const Row& row, ExecContext& context) {
    Result<QueryValue> left = eval(*expr.args[0], row, context);
    if (!left)
        return left;
    const Tri left_value = tri_of(*left);
    const bool stop_value = expr.op == BinaryOp::Or;
    if (left_value == stop_value)
        return QueryValue(stop_value);
    Result<QueryValue> right = eval(*expr.args[1], row, context);
    if (!right)
        return right;
    const Tri right_value = tri_of(*right);
    if (right_value == stop_value)
        return QueryValue(stop_value);
    if (!left_value || !right_value)
        return QueryValue{};
    return QueryValue(!stop_value);
}

Result<QueryValue> apply_binary(const Expr& expr, const QueryValue& left, const QueryValue& right) {
    switch (expr.op) {
    case BinaryOp::Eq:
        return from_tri(values_equal(left, right));
    case BinaryOp::Ne: {
        const Tri equal = values_equal(left, right);
        return equal ? QueryValue(!*equal) : QueryValue();
    }
    case BinaryOp::Lt:
    case BinaryOp::Gt:
    case BinaryOp::Le:
    case BinaryOp::Ge:
        return compare_op(expr.op, left, right);
    case BinaryOp::In:
        return membership(left, right);
    case BinaryOp::Contains:
    case BinaryOp::StartsWith:
    case BinaryOp::EndsWith:
        return text_predicate(expr.op, left, right);
    default:
        return arithmetic(expr, left, right);
    }
}

Result<QueryValue> eval_binary(const Expr& expr, const Row& row, ExecContext& context) {
    if (expr.op == BinaryOp::And || expr.op == BinaryOp::Or)
        return eval_logical(expr, row, context);
    Result<QueryValue> left = eval(*expr.args[0], row, context);
    if (!left)
        return left;
    Result<QueryValue> right = eval(*expr.args[1], row, context);
    if (!right)
        return right;
    return apply_binary(expr, *left, *right);
}

Result<QueryValue> eval_list(const Expr& expr, const Row& row, ExecContext& context) {
    QueryList list;
    list.items.reserve(expr.args.size());
    std::size_t bytes = 0;
    int depth = 0;
    for (const ExprPtr& arg : expr.args) {
        Result<QueryValue> item = eval(*arg, row, context);
        if (!item)
            return item;
        // Sorgu kısa olsa da WITH zinciriyle liste üstel büyüyebilir: kopya bütçeyi aşmadan durur.
        bytes += value_bytes(*item);
        depth = std::max(depth, value_depth(*item));
        if (context.budget.would_exceed(bytes) || depth >= kMaxValueDepth)
            return std::unexpected(limit_error("memory"));
        list.items.push_back(std::move(*item));
    }
    return QueryValue(std::move(list));
}

Result<QueryValue> property_of(const QueryValue& owner, const Expr& expr, ExecContext& context) {
    if (const auto* node = std::get_if<NodeId>(&owner.v))
        return context.memo.property(context.read, *node, expr.key_id, expr.name);
    if (const auto* edge = std::get_if<EdgeId>(&owner.v))
        return context.memo.property(context.read, *edge, expr.key_id, expr.name);
    return QueryValue{};
}

// Yuva yoksa hedef ifadedir: ORDER BY'da açılan diğer ad (`RETURN 5 AS x ORDER BY x.y`).
Result<QueryValue> eval_property(const Expr& expr, const Row& row, ExecContext& context) {
    if (expr.slot >= 0)
        return property_of(row[static_cast<std::size_t>(expr.slot)], expr, context);
    const Result<QueryValue> owner = eval(*expr.args[0], row, context);
    if (!owner)
        return owner;
    return property_of(*owner, expr, context);
}

Result<QueryValue> eval_labels(const NodeId& id, ExecContext& context) {
    const Result<LabelsRef> labels = context.memo.labels(context.read, id);
    if (!labels)
        return std::unexpected(labels.error());
    QueryList list;
    for (const std::string& label : **labels)
        list.items.emplace_back(label);
    return QueryValue(std::move(list));
}

Result<QueryValue> eval_call(const Expr& expr, const Row& row, ExecContext& context) {
    Result<QueryValue> arg = eval(*expr.args[0], row, context);
    if (!arg)
        return arg;
    const auto* node = std::get_if<NodeId>(&arg->v);
    const auto* edge = std::get_if<EdgeId>(&arg->v);
    if (expr.fn == Function::Labels)
        return node != nullptr ? eval_labels(*node, context) : QueryValue{};
    if (expr.fn == Function::Id) {
        if (node != nullptr)
            return QueryValue(id_to_hex(node->bytes));
        return edge != nullptr ? QueryValue(id_to_hex(edge->bytes)) : QueryValue{};
    }
    if (edge == nullptr)
        return QueryValue{};
    const Result<std::optional<std::string>> type = read_edge_type(context, *edge);
    if (!type)
        return std::unexpected(type.error());
    return *type ? QueryValue(**type) : QueryValue{};
}

Result<QueryValue> eval_has_labels(const Expr& expr, const Row& row, ExecContext& context) {
    const Result<QueryValue> target = eval(*expr.args[0], row, context);
    if (!target)
        return target;
    const auto* node = std::get_if<NodeId>(&target->v);
    if (node == nullptr)
        return QueryValue{};
    const Result<LabelsRef> labels = context.memo.labels(context.read, *node);
    if (!labels)
        return std::unexpected(labels.error());
    const auto& have = **labels;
    return QueryValue(std::ranges::all_of(expr.labels, [&](const std::string& wanted) {
        return std::ranges::find(have, wanted) != have.end();
    }));
}

Result<QueryValue> eval_negate(const Expr& expr, const Row& row, ExecContext& context) {
    Result<QueryValue> value = eval(*expr.args[0], row, context);
    if (!value)
        return value;
    if (const auto* integer = std::get_if<std::int64_t>(&value->v)) {
        if (*integer == std::numeric_limits<std::int64_t>::min())
            return std::unexpected(arithmetic_error("overflow", expr.pos));
        return QueryValue(-*integer);
    }
    if (const auto* real = std::get_if<double>(&value->v))
        return QueryValue(-*real);
    return QueryValue{};
}

Result<QueryValue> eval_not(const Expr& expr, const Row& row, ExecContext& context) {
    Result<QueryValue> value = eval(*expr.args[0], row, context);
    if (!value)
        return value;
    const Tri truth = tri_of(*value);
    return truth ? QueryValue(!*truth) : QueryValue{};
}

Result<QueryValue> eval_null_test(const Expr& expr, const Row& row, ExecContext& context) {
    Result<QueryValue> value = eval(*expr.args[0], row, context);
    if (!value)
        return value;
    return QueryValue(value->is_null() == (expr.kind == Expr::Kind::IsNull));
}

// Toplam sıra için tip sırası (openCypher): düğüm, kenar, liste, zaman, bayt, metin, mantıksal, sayı, NaN,
// null.
int order_rank(const QueryValue& value) {
    return std::visit(
        [](const auto& v) -> int {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NodeId>)
                return 0;
            else if constexpr (std::is_same_v<T, EdgeId>)
                return 1;
            else if constexpr (std::is_same_v<T, QueryList>)
                return 2;
            else if constexpr (std::is_same_v<T, Timestamp>)
                return 3;
            else if constexpr (std::is_same_v<T, Bytes>)
                return 4;
            else if constexpr (std::is_same_v<T, std::string>)
                return 5;
            else if constexpr (std::is_same_v<T, bool>)
                return 6;
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return 7;
            else if constexpr (std::is_same_v<T, double>)
                return std::isnan(v) ? 8 : 7;
            else
                return 9;
        },
        value.v);
}

int compare_lists_for_order(const QueryList& left, const QueryList& right) {
    const std::size_t shared = std::min(left.items.size(), right.items.size());
    for (std::size_t i = 0; i < shared; ++i)
        if (const int order = compare_for_order(left.items[i], right.items[i]); order != 0)
            return order;
    if (left.items.size() == right.items.size())
        return 0;
    return left.items.size() < right.items.size() ? -1 : 1;
}

template <class Id> int compare_ids(const Id& left, const Id& right) {
    const auto order = left <=> right;
    return std::is_lt(order) ? -1 : (std::is_gt(order) ? 1 : 0);
}

void append_size(std::string& out, std::size_t size) {
    const auto value = static_cast<std::uint64_t>(size);
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void append_tagged_text(std::string& out, char tag, std::string_view text) {
    out += tag;
    append_size(out, text.size());
    out.append(text);
}

template <class T> void append_raw(std::string& out, char tag, const T& value) {
    out += tag;
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void append_number(std::string& out, const QueryValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.v)) {
        append_raw(out, 'i', *integer);
        return;
    }
    const double real = std::get<double>(value.v);
    constexpr double kTwoTo63 = 9223372036854775808.0;
    // 1 ve 1.0 `=` ile eşittir, aynı baytları vermeli; -0.0 da 0'dır.
    if (std::trunc(real) == real && real >= -kTwoTo63 && real < kTwoTo63) {
        append_raw(out, 'i', static_cast<std::int64_t>(real));
        return;
    }
    if (std::isnan(real))
        out += 'N';
    else
        append_raw(out, 'd', real);
}

}  // namespace

Result<QueryValue> eval(const Expr& expr, const Row& row, ExecContext& context) {
    switch (expr.kind) {
    case Expr::Kind::Literal:
        return expr.literal;
    case Expr::Kind::Var:
        return row[static_cast<std::size_t>(expr.slot)];
    case Expr::Kind::Property:
        return eval_property(expr, row, context);
    case Expr::Kind::List:
        return eval_list(expr, row, context);
    case Expr::Kind::Binary:
        return eval_binary(expr, row, context);
    case Expr::Kind::Not:
        return eval_not(expr, row, context);
    case Expr::Kind::Negate:
        return eval_negate(expr, row, context);
    case Expr::Kind::IsNull:
    case Expr::Kind::IsNotNull:
        return eval_null_test(expr, row, context);
    case Expr::Kind::Call:
        return eval_call(expr, row, context);
    case Expr::Kind::HasLabels:
        return eval_has_labels(expr, row, context);
    case Expr::Kind::Param:
    case Expr::Kind::Aggregate:
        break;
    }
    // Planlayıcı bu türleri bırakmaz: buraya gelmek iç bug'dır.
    return std::unexpected(located_error(Error::Code::InvalidArgument, "graph.invalid_argument", expr.pos,
                                         {{"reason", "unplanned_expression"}}));
}

Result<bool> eval_predicate(const Expr& expr, const Row& row, ExecContext& context) {
    const Result<QueryValue> value = eval(expr, row, context);
    if (!value)
        return std::unexpected(value.error());
    return tri_of(*value) == Tri{true};
}

Result<std::size_t> eval_count(const Expr& expr, ExecContext& context) {
    const Row no_row;
    const Result<QueryValue> value = eval(expr, no_row, context);
    if (!value)
        return std::unexpected(value.error());
    const auto* integer = std::get_if<std::int64_t>(&value->v);
    if (integer == nullptr || *integer < 0)
        return std::unexpected(
            located_error(Error::Code::InvalidArgument, "graph.cypher_bad_count", expr.pos));
    return static_cast<std::size_t>(*integer);
}

int compare_for_order(const QueryValue& left, const QueryValue& right) {
    const int left_rank = order_rank(left);
    const int right_rank = order_rank(right);
    if (left_rank != right_rank)
        return left_rank < right_rank ? -1 : 1;
    if (const auto* node = std::get_if<NodeId>(&left.v))
        return compare_ids(*node, std::get<NodeId>(right.v));
    if (const auto* edge = std::get_if<EdgeId>(&left.v))
        return compare_ids(*edge, std::get<EdgeId>(right.v));
    if (const auto* list = std::get_if<QueryList>(&left.v))
        return compare_lists_for_order(*list, std::get<QueryList>(right.v));
    return compare_same_type(left, right).value_or(0);
}

Tri values_equal(const QueryValue& left, const QueryValue& right) {
    if (left.is_null() || right.is_null())
        return std::nullopt;
    if (is_number(left) && is_number(right)) {
        const std::optional<int> order = compare_numbers(left, right);
        return order == 0;
    }
    if (left.v.index() != right.v.index())
        return false;
    if (const auto* list = std::get_if<QueryList>(&left.v))
        return lists_equal(*list, std::get<QueryList>(right.v));
    return left == right;
}

void append_canonical(std::string& out, const QueryValue& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += 'z';
            } else if constexpr (std::is_same_v<T, bool>) {
                out += v ? 'T' : 'F';
            } else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                append_number(out, value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                append_tagged_text(out, 's', v);
            } else if constexpr (std::is_same_v<T, Bytes>) {
                append_tagged_text(out, 'b', v.data);
            } else if constexpr (std::is_same_v<T, Timestamp>) {
                append_raw(out, 't', v.unix_ms);
            } else if constexpr (std::is_same_v<T, NodeId>) {
                append_raw(out, 'n', v.bytes);
            } else if constexpr (std::is_same_v<T, EdgeId>) {
                append_raw(out, 'e', v.bytes);
            } else {
                out += 'l';
                append_size(out, v.items.size());
                for (const QueryValue& item : v.items)
                    append_canonical(out, item);
            }
        },
        value.v);
}

QueryValue from_stored(Value stored) {
    return std::visit(
        [](auto&& alternative) { return QueryValue(std::forward<decltype(alternative)>(alternative)); },
        std::move(stored));
}

Result<Value> to_stored(const QueryValue& value, Position pos) {
    return std::visit(
        [&](const auto& v) -> Result<Value> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NodeId> || std::is_same_v<T, EdgeId> ||
                          std::is_same_v<T, QueryList>)
                return std::unexpected(
                    located_error(Error::Code::InvalidArgument, "graph.cypher_not_storable", pos));
            else
                return Value(v);
        },
        value.v);
}

std::size_t value_bytes(const QueryValue& value) {
    std::size_t bytes = sizeof(QueryValue);
    if (const auto* text = std::get_if<std::string>(&value.v))
        bytes += text->size();
    else if (const auto* raw = std::get_if<Bytes>(&value.v))
        bytes += raw->data.size();
    else if (const auto* list = std::get_if<QueryList>(&value.v))
        for (const QueryValue& item : list->items)
            bytes += value_bytes(item);
    return bytes;
}

std::size_t row_bytes(const Row& row) {
    std::size_t bytes = sizeof(Row);
    for (const QueryValue& value : row)
        bytes += value_bytes(value);
    return bytes;
}

int value_depth(const QueryValue& value) {
    const auto* list = std::get_if<QueryList>(&value.v);
    if (list == nullptr)
        return 0;
    int depth = 0;
    for (const QueryValue& item : list->items)
        depth = std::max(depth, value_depth(item));
    return depth + 1;
}

std::string id_to_hex(std::span<const std::byte> id) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(id.size() * 2);
    for (const std::byte byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        out += kDigits[value >> 4U];
        out += kDigits[value & 0xFU];
    }
    return out;
}

std::optional<std::array<std::byte, 16>> id_from_hex(std::string_view text) {
    std::array<std::byte, 16> id{};
    if (text.size() != id.size() * 2)
        return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i < id.size(); ++i) {
        const int high = digit(text[i * 2]);
        const int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        id[i] = static_cast<std::byte>(high * 16 + low);
    }
    return id;
}

Result<std::optional<std::string>> read_edge_type(ExecContext& context, const EdgeId& id) {
    Result<Statement> statement = context.store.statement(StatementId::EdgeGet);
    if (!statement)
        return std::unexpected(std::move(statement).error());
    statement->bind_blob(1, id.bytes);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return std::optional<std::string>{};
    return std::optional<std::string>(statement->column_text(1));
}

}  // namespace noteapp::graph::detail::cypher
