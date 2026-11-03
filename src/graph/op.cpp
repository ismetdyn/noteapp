#include "graph/op.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <optional>
#include <utility>

namespace noteapp::graph::detail {

namespace {

// Değer etiketleri kalıcı biçimdir: props satırında ve oplog'da yaşar, değiştirilmez.
enum class ValueTag : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Double = 3,
    String = 4,
    Bytes = 5,
    Time = 6
};
// İşlem etiketleri Op'un sırasıyla aynıdır; bu da kalıcıdır.
enum class OpTag : std::uint8_t { NodePut = 0, NodeDel, LabelAdd, LabelDel, EdgePut, EdgeDel, PropSet };
enum class OwnerTag : std::uint8_t { Node = 0, Edge = 1 };

constexpr std::size_t kIdBytes = 16;
constexpr std::size_t kDotBytes = kIdBytes + sizeof(std::uint64_t);
// En kısa kodlu işlem: etiket + tek kimlik (node_put, node_del, edge_del).
constexpr std::size_t kMinOpBytes = 1 + kIdBytes;

Error bad_frame(std::string reason) {
    return Error{Error::Code::BadFrame, "graph.bad_frame", {{"reason", std::move(reason)}}};
}

Error invalid_argument(std::string reason) {
    return Error{Error::Code::InvalidArgument, "graph.invalid_argument", {{"reason", std::move(reason)}}};
}

void put_u8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

template <class Unsigned> void put_le(std::string& out, Unsigned value) {
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        out.push_back(static_cast<char>(value >> (i * 8U)));
}

void put_id(std::string& out, const std::array<std::byte, kIdBytes>& id) {
    out.append(reinterpret_cast<const char*>(id.data()), id.size());
}

void put_text(std::string& out, std::string_view text) {
    put_le(out, static_cast<std::uint32_t>(text.size()));
    out.append(text);
}

// Her okuma sınır denetler; taşan okuma nullopt döner, hata ilk nullopt'ta üste çıkar.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    bool at_end() const { return data_.empty(); }
    std::size_t remaining() const { return data_.size(); }

    std::optional<std::span<const std::byte>> take(std::size_t count) {
        if (count > data_.size())
            return std::nullopt;
        const std::span<const std::byte> part = data_.first(count);
        data_ = data_.subspan(count);
        return part;
    }

    template <class Unsigned> std::optional<Unsigned> read_le() {
        const auto part = take(sizeof(Unsigned));
        if (!part)
            return std::nullopt;
        Unsigned value = 0;
        for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
            value = static_cast<Unsigned>(
                value | (static_cast<Unsigned>(std::to_integer<std::uint8_t>((*part)[i])) << (i * 8U)));
        return value;
    }

    std::optional<std::array<std::byte, kIdBytes>> read_id() {
        const auto part = take(kIdBytes);
        if (!part)
            return std::nullopt;
        std::array<std::byte, kIdBytes> id{};
        std::ranges::copy(*part, id.begin());
        return id;
    }

    std::optional<std::string_view> read_text() {
        const auto size = read_le<std::uint32_t>();
        if (!size)
            return std::nullopt;
        const auto part = take(*size);
        if (!part)
            return std::nullopt;
        return std::string_view(reinterpret_cast<const char*>(part->data()), part->size());
    }

private:
    std::span<const std::byte> data_;
};

std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xC2 && lead <= 0xDF)
        return 2;
    if (lead >= 0xE0 && lead <= 0xEF)
        return 3;
    if (lead >= 0xF0 && lead <= 0xF4)
        return 4;
    return 0;
}

// İkinci baytın aralığı aşırı uzun, vekil ve U+10FFFF üstünü eler (Unicode Tablo 3-7).
bool utf8_second_byte_ok(unsigned char lead, unsigned char second) {
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead == 0xE0)
        low = 0xA0;
    else if (lead == 0xED)
        high = 0x9F;
    else if (lead == 0xF0)
        low = 0x90;
    else if (lead == 0xF4)
        high = 0x8F;
    return second >= low && second <= high;
}

void put_op(std::string& out, const NodePut& op) {
    put_u8(out, std::to_underlying(OpTag::NodePut));
    put_id(out, op.id.bytes);
}
void put_op(std::string& out, const NodeDel& op) {
    put_u8(out, std::to_underlying(OpTag::NodeDel));
    put_id(out, op.id.bytes);
}
void put_op(std::string& out, const LabelAdd& op) {
    put_u8(out, std::to_underlying(OpTag::LabelAdd));
    put_id(out, op.node.bytes);
    put_text(out, op.label);
}
void put_op(std::string& out, const LabelDel& op) {
    put_u8(out, std::to_underlying(OpTag::LabelDel));
    put_id(out, op.node.bytes);
    put_text(out, op.label);
}
void put_op(std::string& out, const EdgePut& op) {
    put_u8(out, std::to_underlying(OpTag::EdgePut));
    put_id(out, op.id.bytes);
    put_id(out, op.src.bytes);
    put_text(out, op.type);
    put_id(out, op.dst.bytes);
}
void put_op(std::string& out, const EdgeDel& op) {
    put_u8(out, std::to_underlying(OpTag::EdgeDel));
    put_id(out, op.id.bytes);
}
void put_op(std::string& out, const PropSet& op) {
    put_u8(out, std::to_underlying(OpTag::PropSet));
    if (const NodeId* node = std::get_if<NodeId>(&op.owner)) {
        put_u8(out, std::to_underlying(OwnerTag::Node));
        put_id(out, node->bytes);
    } else {
        put_u8(out, std::to_underlying(OwnerTag::Edge));
        put_id(out, std::get<EdgeId>(op.owner).bytes);
    }
    put_text(out, op.key);
    put_text(out, encode_value(op.value));
}

// Çözülen ad, yazma sınırıyla aynı kurala uyar: uzak cihaz yerelde üretilemeyecek bir ad kabul ettiremez.
Result<std::string> read_name(Reader& reader) {
    const auto text = reader.read_text();
    if (!text)
        return std::unexpected(bad_frame("truncated_name"));
    if (!validate_name(*text))
        return std::unexpected(bad_frame("bad_name"));
    return std::string(*text);
}

template <class Id> Result<Id> read_typed_id(Reader& reader) {
    const auto id = reader.read_id();
    if (!id)
        return std::unexpected(bad_frame("truncated_id"));
    return Id{*id};
}

Result<Op> read_label_op(Reader& reader, OpTag tag) {
    const Result<NodeId> node = read_typed_id<NodeId>(reader);
    if (!node)
        return std::unexpected(node.error());
    Result<std::string> label = read_name(reader);
    if (!label)
        return std::unexpected(label.error());
    if (tag == OpTag::LabelAdd)
        return LabelAdd{*node, std::move(*label)};
    return LabelDel{*node, std::move(*label)};
}

Result<Op> read_edge_put(Reader& reader) {
    const Result<EdgeId> id = read_typed_id<EdgeId>(reader);
    if (!id)
        return std::unexpected(id.error());
    const Result<NodeId> src = read_typed_id<NodeId>(reader);
    if (!src)
        return std::unexpected(src.error());
    Result<std::string> type = read_name(reader);
    if (!type)
        return std::unexpected(type.error());
    const Result<NodeId> dst = read_typed_id<NodeId>(reader);
    if (!dst)
        return std::unexpected(dst.error());
    return EdgePut{*id, *src, std::move(*type), *dst};
}

Result<OwnerId> read_owner(Reader& reader) {
    const auto tag = reader.read_le<std::uint8_t>();
    if (!tag)
        return std::unexpected(bad_frame("truncated_owner"));
    if (*tag == std::to_underlying(OwnerTag::Node)) {
        const Result<NodeId> node = read_typed_id<NodeId>(reader);
        if (!node)
            return std::unexpected(node.error());
        return OwnerId{*node};
    }
    if (*tag == std::to_underlying(OwnerTag::Edge)) {
        const Result<EdgeId> edge = read_typed_id<EdgeId>(reader);
        if (!edge)
            return std::unexpected(edge.error());
        return OwnerId{*edge};
    }
    return std::unexpected(bad_frame("unknown_owner"));
}

Result<Op> read_prop_set(Reader& reader) {
    const Result<OwnerId> owner = read_owner(reader);
    if (!owner)
        return std::unexpected(owner.error());
    Result<std::string> key = read_name(reader);
    if (!key)
        return std::unexpected(key.error());
    const auto encoded = reader.read_text();
    if (!encoded)
        return std::unexpected(bad_frame("truncated_value"));
    Result<Value> value = decode_value(std::as_bytes(std::span(encoded->data(), encoded->size())));
    if (!value)
        return std::unexpected(value.error());
    return PropSet{*owner, std::move(*key), std::move(*value)};
}

Result<Op> read_op(Reader& reader) {
    const auto tag = reader.read_le<std::uint8_t>();
    if (!tag)
        return std::unexpected(bad_frame("truncated_op"));
    switch (static_cast<OpTag>(*tag)) {
    case OpTag::NodePut:
        return read_typed_id<NodeId>(reader).transform([](NodeId id) -> Op { return NodePut{id}; });
    case OpTag::NodeDel:
        return read_typed_id<NodeId>(reader).transform([](NodeId id) -> Op { return NodeDel{id}; });
    case OpTag::LabelAdd:
    case OpTag::LabelDel:
        return read_label_op(reader, static_cast<OpTag>(*tag));
    case OpTag::EdgePut:
        return read_edge_put(reader);
    case OpTag::EdgeDel:
        return read_typed_id<EdgeId>(reader).transform([](EdgeId id) -> Op { return EdgeDel{id}; });
    case OpTag::PropSet:
        return read_prop_set(reader);
    }
    return std::unexpected(bad_frame("unknown_op"));
}

Result<std::vector<Dot>> read_deps(Reader& reader, std::size_t max_deps) {
    const auto count = reader.read_le<std::uint32_t>();
    if (!count)
        return std::unexpected(bad_frame("truncated_deps"));
    if (*count > max_deps)
        return std::unexpected(bad_frame("too_many_deps"));
    // Sayı güvenilmezdir ve sınır kaldırılabilir: ayırmadan önce kalan baytla doğrulanır.
    if (*count > reader.remaining() / kDotBytes)
        return std::unexpected(bad_frame("truncated_deps"));
    std::vector<Dot> deps;
    deps.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto dev = reader.read_id();
        const auto seq = reader.read_le<std::uint64_t>();
        if (!dev || !seq)
            return std::unexpected(bad_frame("truncated_deps"));
        deps.push_back(Dot{*dev, *seq});
    }
    return deps;
}

// Sekiz baytlık sabit genişlikli türler. NaN reddi burada: uzak çerçeve yerelde yazılamayacak değer
// getiremez.
std::optional<Value> decode_fixed_value(ValueTag tag, std::span<const std::byte> payload) {
    if (payload.size() != sizeof(std::uint64_t))
        return std::nullopt;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- uzunluk yukarıda sekiz bayt denetlendi
    const std::uint64_t bits = *Reader(payload).read_le<std::uint64_t>();
    if (tag == ValueTag::Int)
        return Value{static_cast<std::int64_t>(bits)};
    if (tag == ValueTag::Time)
        return Value{Timestamp{static_cast<std::int64_t>(bits)}};
    const double number = std::bit_cast<double>(bits);
    if (!std::isfinite(number))
        return std::nullopt;
    return Value{number};
}

// API istisnasızdır, girdi uzaktan gelir: çözücünün ayırması bad_alloc'a düşerse süreç ölmez. Çerçevenin suçu
// olmadığından BadFrame değildir; yoksa sağlam çerçeve bozuk diye bildirilirdi.
template <class Decode> auto without_throwing(Decode&& decode) -> decltype(decode()) {
    try {
        return decode();
    } catch (const std::bad_alloc&) {
        return std::unexpected(out_of_memory());
    }
}

Result<Value> parse_value(std::span<const std::byte> encoded);
Result<Frame> parse_frame(std::span<const std::byte> payload, const FrameLimits& limits);
Result<std::vector<Dot>> parse_dots(std::span<const std::byte> encoded);

}  // namespace

Result<Value> decode_value(std::span<const std::byte> encoded) {
    return without_throwing([&] { return parse_value(encoded); });
}

// Biçim tavanı config'ten bağımsızdır: zarfın uzunluğu, deps ve ops sayıları u32'dir. Sınırsız da bunu
// aşamaz.
FrameLimits frame_limits_of(const GraphLimits& limits) {
    constexpr std::size_t kFormatMax = UINT32_MAX;
    const std::size_t deps =
        limits.peers > kFormatMax / kDepsPerPeer ? kFormatMax : limits.peers * kDepsPerPeer;
    return FrameLimits{std::min(limits.frame_bytes, kFormatMax), std::min(limits.frame_ops, kFormatMax),
                       deps};
}

FrameLimits logical_frame_limits(const FrameLimits& record, std::size_t parts) {
    const std::size_t count = std::max<std::size_t>(parts, 1);
    const std::size_t ops = record.ops > SIZE_MAX / count ? SIZE_MAX : record.ops * count;
    return FrameLimits{SIZE_MAX, ops, record.deps};
}

std::optional<std::size_t> peek_op_count(std::span<const std::byte> payload) {
    Reader reader(payload);
    if (!reader.take(2 * sizeof(std::uint64_t)))
        return std::nullopt;
    const auto deps = reader.read_le<std::uint32_t>();
    if (!deps || *deps > reader.remaining() / kDotBytes || !reader.take(*deps * kDotBytes))
        return std::nullopt;
    const auto ops = reader.read_le<std::uint32_t>();
    if (!ops)
        return std::nullopt;
    return *ops;
}

Result<Frame> decode_frame(std::span<const std::byte> payload, const FrameLimits& limits) {
    return without_throwing([&] { return parse_frame(payload, limits); });
}

Result<std::vector<Dot>> decode_dots(std::span<const std::byte> encoded) {
    return without_throwing([&] { return parse_dots(encoded); });
}

std::string encode_value(const Value& value) {
    std::string out;
    std::visit(
        [&out]<class T>(const T& v) {
            if constexpr (std::is_same_v<T, std::monostate>) {
                put_u8(out, std::to_underlying(ValueTag::Null));
            } else if constexpr (std::is_same_v<T, bool>) {
                put_u8(out, std::to_underlying(ValueTag::Bool));
                put_u8(out, v ? 1 : 0);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                put_u8(out, std::to_underlying(ValueTag::Int));
                put_le(out, static_cast<std::uint64_t>(v));
            } else if constexpr (std::is_same_v<T, double>) {
                put_u8(out, std::to_underlying(ValueTag::Double));
                put_le(out, std::bit_cast<std::uint64_t>(v));
            } else if constexpr (std::is_same_v<T, std::string>) {
                out.reserve(v.size() + 1);
                put_u8(out, std::to_underlying(ValueTag::String));
                out.append(v);
            } else if constexpr (std::is_same_v<T, Bytes>) {
                out.reserve(v.data.size() + 1);
                put_u8(out, std::to_underlying(ValueTag::Bytes));
                out.append(v.data);
            } else {
                static_assert(std::is_same_v<T, Timestamp>);
                put_u8(out, std::to_underlying(ValueTag::Time));
                put_le(out, static_cast<std::uint64_t>(v.unix_ms));
            }
        },
        value);
    return out;
}

namespace {

Result<Value> parse_value(std::span<const std::byte> encoded) {
    if (encoded.empty())
        return std::unexpected(bad_frame("empty_value"));
    const auto tag = static_cast<ValueTag>(std::to_integer<std::uint8_t>(encoded[0]));
    const std::span<const std::byte> payload = encoded.subspan(1);
    const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());

    std::optional<Value> value;
    switch (tag) {
    case ValueTag::Null:
        if (payload.empty())
            value = Value{};
        break;
    case ValueTag::Bool:
        if (payload.size() == 1 && std::to_integer<std::uint8_t>(payload[0]) <= 1)
            value = Value{std::to_integer<std::uint8_t>(payload[0]) == 1};
        break;
    case ValueTag::Int:
    case ValueTag::Double:
    case ValueTag::Time:
        value = decode_fixed_value(tag, payload);
        break;
    case ValueTag::String:
        if (is_valid_utf8(text))
            value = Value{std::string(text)};
        break;
    case ValueTag::Bytes:
        value = Value{Bytes{std::string(text)}};
        break;
    }
    if (!value)
        return std::unexpected(bad_frame("bad_value"));
    return std::move(*value);
}

}  // namespace

std::string encode_frame(const Frame& frame) {
    std::string out;
    put_le(out, frame.seq);
    put_le(out, frame.hlc);
    put_le(out, static_cast<std::uint32_t>(frame.deps.size()));
    for (const Dot& dep : frame.deps) {
        put_id(out, dep.dev);
        put_le(out, dep.seq);
    }
    put_le(out, static_cast<std::uint32_t>(frame.ops.size()));
    for (const Op& op : frame.ops)
        std::visit([&out](const auto& alternative) { put_op(out, alternative); }, op);
    return out;
}

namespace {

Result<Frame> parse_frame(std::span<const std::byte> payload, const FrameLimits& limits) {
    if (payload.size() > limits.bytes)
        return std::unexpected(bad_frame("frame_too_large"));
    Reader reader(payload);
    Frame frame;
    const auto seq = reader.read_le<std::uint64_t>();
    const auto hlc = reader.read_le<std::uint64_t>();
    if (!seq || !hlc)
        return std::unexpected(bad_frame("truncated_header"));
    if (*seq == 0)
        return std::unexpected(bad_frame("zero_seq"));
    frame.seq = *seq;
    frame.hlc = *hlc;

    Result<std::vector<Dot>> deps = read_deps(reader, limits.deps);
    if (!deps)
        return std::unexpected(deps.error());
    frame.deps = std::move(*deps);

    const auto count = reader.read_le<std::uint32_t>();
    if (!count)
        return std::unexpected(bad_frame("truncated_ops"));
    if (*count == 0)
        return std::unexpected(bad_frame("no_ops"));
    if (*count > limits.ops)
        return std::unexpected(bad_frame("too_many_ops"));
    // Sayı güvenilmezdir: önce kalan baytla doğrulanır (en kısa işlem kMinOpBytes), ancak sonra ayrılır.
    if (*count > reader.remaining() / kMinOpBytes)
        return std::unexpected(bad_frame("truncated_ops"));
    frame.ops.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        Result<Op> op = read_op(reader);
        if (!op)
            return std::unexpected(op.error());
        frame.ops.push_back(std::move(*op));
    }
    if (!reader.at_end())
        return std::unexpected(bad_frame("trailing_bytes"));
    return frame;
}

}  // namespace

bool is_valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = utf8_sequence_length(lead);
        if (length == 0 || length > text.size() - i)
            return false;
        if (length > 1 && !utf8_second_byte_ok(lead, static_cast<unsigned char>(text[i + 1])))
            return false;
        for (std::size_t j = 2; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0U) != 0x80U)
                return false;
        i += length;
    }
    return true;
}

Result<void> validate_name(std::string_view name) {
    if (name.empty())
        return std::unexpected(invalid_argument("empty_name"));
    if (name.size() > kMaxNameBytes)
        return std::unexpected(invalid_argument("name_too_long"));
    if (!is_valid_utf8(name))
        return std::unexpected(invalid_argument("name_not_utf8"));
    // SQLite TEXT'te gömülü NUL'lu değerin davranışı tanımsız bölgededir; ad için anlamı da yoktur.
    if (name.find('\0') != std::string_view::npos)
        return std::unexpected(invalid_argument("name_has_nul"));
    return {};
}

Result<void> validate_value(const Value& value) {
    if (const double* number = std::get_if<double>(&value); number != nullptr && !std::isfinite(*number))
        return std::unexpected(invalid_argument("non_finite"));
    if (const std::string* text = std::get_if<std::string>(&value); text != nullptr && !is_valid_utf8(*text))
        return std::unexpected(invalid_argument("value_not_utf8"));
    return {};
}

std::string encode_dots(std::span<const Dot> dots) {
    std::string out;
    out.reserve(dots.size() * kDotBytes);
    for (const Dot& dot : dots) {
        put_id(out, dot.dev);
        put_le(out, dot.seq);
    }
    return out;
}

namespace {

Result<std::vector<Dot>> parse_dots(std::span<const std::byte> encoded) {
    if (encoded.size() % kDotBytes != 0)
        return std::unexpected(Error{Error::Code::Corrupt, "graph.corrupt", {{"reason", "bad_dots"}}});
    Reader reader(encoded);
    std::vector<Dot> dots;
    dots.reserve(encoded.size() / kDotBytes);
    while (!reader.at_end()) {
        const auto dev = reader.read_id();
        const auto seq = reader.read_le<std::uint64_t>();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- boyut kDotBytes'ın katı: okuma tamamlanır
        dots.push_back(Dot{*dev, *seq});  // uzunluk yukarıda denetlendi
    }
    return dots;
}

}  // namespace

}  // namespace noteapp::graph::detail
