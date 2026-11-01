#pragma once

// İç başlık. İşlem günlüğünün çerçevesi ve değer kodu: props satırı ve oplog aynı kodu kullanır. Oplog
// dosyasının zarfı (başlık, uzunluk, özet) G2'de oplog.*'dadır; burada yalnız payload vardır.
#include "graph/error.hpp"
#include "graph/graph.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noteapp::graph::detail {

using DeviceId = std::array<std::byte, 16>;

// Limitler güvenilmez girdi sınırıdır (SPEC-graph "Güvenilmez girdi"); yerel yazma da aynılarına uyar, yoksa
// bu cihazın ürettiği çerçeve uzak cihazda reddedilip nedensel zinciri durdururdu.
inline constexpr std::size_t kMaxFrameBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxNameBytes = 256;
inline constexpr std::size_t kMaxDeps = 1024;
// Çözülmüş Op, kodlu halinin katları yer tutar: sınır olmasa 64 MiB'lik çerçeve ~3,9M Op (~400 MB) ayırırdı.
inline constexpr std::size_t kMaxFrameOps = 65536;
// Tanınan cihaz sınırı: frame_.deps tüm peers'ten kurulur, peers kMaxDeps'i aşarsa her yerel commit düşerdi.
inline constexpr std::size_t kMaxPeers = 256;
static_assert(kMaxPeers <= kMaxDeps);

// Bir yazmanın kimliği ya da (bağlamda) bir cihazdan görülen son seq.
struct Dot {
    DeviceId dev{};
    std::uint64_t seq = 0;
    auto operator<=>(const Dot&) const = default;
};

struct NodePut {
    NodeId id;
    bool operator==(const NodePut&) const = default;
};
struct NodeDel {
    NodeId id;
    bool operator==(const NodeDel&) const = default;
};
struct LabelAdd {
    NodeId node;
    std::string label;
    bool operator==(const LabelAdd&) const = default;
};
struct LabelDel {
    NodeId node;
    std::string label;
    bool operator==(const LabelDel&) const = default;
};
struct EdgePut {
    EdgeId id;
    NodeId src;
    std::string type;
    NodeId dst;
    bool operator==(const EdgePut&) const = default;
};
struct EdgeDel {
    EdgeId id;
    bool operator==(const EdgeDel&) const = default;
};
struct PropSet {
    OwnerId owner;
    std::string key;
    Value value;
    bool operator==(const PropSet&) const = default;
};

using Op = std::variant<NodePut, NodeDel, LabelAdd, LabelDel, EdgePut, EdgeDel, PropSet>;

// `dev` zarftan gelir (ops.dev, oplog başlığı). deps: bu yazmanın gördüğü diğer cihazlar; kendi cihazından
// seq-1 örtüktür.
struct Frame {
    std::uint64_t seq = 0;
    std::uint64_t hlc = 0;
    std::vector<Dot> deps;
    std::vector<Op> ops;
};

// Bellek bitince kurulabilen hata: kısa anahtar küçük dizge içinde kalır, args boştur; ayırma yapmaz.
inline Error out_of_memory() {
    return Error{Error::Code::LimitExceeded, "graph.oom", {}};
}

std::string encode_frame(const Frame& frame);
// Güvenilmez girdiyi tam doğrular; hata BadFrame'dir ve hiçbir girdi süreci düşürmez.
Result<Frame> decode_frame(std::span<const std::byte> payload);

// [tag][payload]. Karşılaştırma C++'tadır; depolama değeri opak BLOB görür.
std::string encode_value(const Value& value);
Result<Value> decode_value(std::span<const std::byte> encoded);

bool is_valid_utf8(std::string_view text);
Result<void> validate_name(std::string_view name);
Result<void> validate_value(const Value& value);

// (device, seq) dizisinin kalıcı biçimi; nodes/labels/edges.dots sütunları kullanır.
std::string encode_dots(std::span<const Dot> dots);
Result<std::vector<Dot>> decode_dots(std::span<const std::byte> encoded);

}  // namespace noteapp::graph::detail
