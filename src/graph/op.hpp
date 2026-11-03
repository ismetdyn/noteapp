#pragma once

// İç başlık. İşlem günlüğünün çerçevesi ve değer kodu: props satırı ve oplog aynı kodu kullanır. Oplog
// dosyasının zarfı (başlık, uzunluk, özet) G2'de oplog.*'dadır; burada yalnız payload vardır.
#include "graph/error.hpp"
#include "graph/graph.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noteapp::graph::detail {

using DeviceId = std::array<std::byte, 16>;

// Ad (etiket, kenar tipi, özellik anahtarı) protokolün parçasıdır, config değildir: farklı değerli iki
// cihazdan birinin geçerli saydığı adı öteki reddeder. Kullanıcı verisi (değer, gövde) bununla sınırlı
// değildir.
inline constexpr std::size_t kMaxNameBytes = 256;

// Çerçeve doğrulamasının sınırları (SPEC-graph "Güvenilmez girdi"); değerler GraphLimits'ten (config
// limits.graph.*) gelir. Varsayılan bugünkü korumadır: uzak çerçeve bununla çözülür; yerel yazma da aynısına
// uyar, yoksa bu cihazın çerçevesi uzak cihazda reddedilip nedensel zinciri durdururdu. Çözülmüş Op, kodlu
// halinin katları yer tutar: sınırsız ops 64 MiB'lik çerçevede ~3,9M Op (~400 MB) ayırabilir.
inline constexpr std::size_t kDefaultFrameBytes = GraphLimits{}.frame_bytes;
inline constexpr std::size_t kDefaultFrameOps = GraphLimits{}.frame_ops;
// Bağımlılık sınırı tanınan cihazın katıdır: frame_.deps tüm peers'ten kurulur, deps peers'ten küçük olsa
// her yerel commit düşerdi.
inline constexpr std::size_t kDepsPerPeer = 4;
inline constexpr std::size_t kDefaultDeps = kDepsPerPeer * GraphLimits{}.peers;

struct FrameLimits {
    std::size_t bytes = kDefaultFrameBytes;
    std::size_t ops = kDefaultFrameOps;
    std::size_t deps = kDefaultDeps;
};

// Bu cihazın kabul ettiği çerçeve: yazma ve uzak günlük aynı sınırla.
FrameLimits frame_limits_of(const GraphLimits& limits);
// Saklı (zaten kabul edilmiş) işlem: config sonradan küçülse de geçmiş okunur; yalnız biçim sınırlıdır.
inline constexpr FrameLimits kStoredFrameLimits{SIZE_MAX, SIZE_MAX, SIZE_MAX};
// Günlükten birleştirilen mantıksal çerçeve (L4b): bayt sınırı kayıtlarda ve grupta uygulanmıştır; işlem
// yoğunluğu kayıt başına aynı kalır (parça x frame_ops).
FrameLimits logical_frame_limits(const FrameLimits& record, std::size_t parts);

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
// Kodlu çerçevenin işlem sayısı, çözmeden (yazanın kendi saklı çerçevesi için). Biçim bozuksa nullopt.
std::optional<std::size_t> peek_op_count(std::span<const std::byte> payload);
// Güvenilmez girdiyi tam doğrular; hata BadFrame'dir ve hiçbir girdi süreci düşürmez.
Result<Frame> decode_frame(std::span<const std::byte> payload, const FrameLimits& limits = {});

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
