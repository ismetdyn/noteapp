#include "app/graph_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace noteapp::app {

namespace {

constexpr float kK = GraphLayout::kIdealEdge;
constexpr float kCutoff = 2.0F * kK;              // ızgara sürümü: itme yalnız bu yarıçapta (FR makalesi)
constexpr float kCooling = 0.95F;                 // adım başına sıcaklık çarpanı
constexpr float kInitialTemperatureShare = 0.1F;  // FR: ilk sıcaklık alan genişliğinin onda biri
constexpr float kWarmTemperature = kK;            // değişiklik ya da sürüklemede yerel düzeltme yeter
constexpr float kMinTemperature = 0.01F * kK;     // altındaki adım piksel altıdır: yerleşim durur
constexpr float kMinDistance = 0.01F * kK;        // sıfır uzaklıkta yön tanımsız: NaN olmasın
constexpr float kNeighbourOffset = 0.5F * kK;     // yeni düğüm eski komşusunun bu kadar yakınına konur
constexpr float kGoldenAngle = 2.39996323F;  // çakışan çiftlere sıradan türeyen, birbirinden ayrık yönler
// Merkeze doğrusal çekim (d başına): itme 2k'da kesildiğinden bağsız notlar yoksa haritayı seyreltir, görünüm
// küçülür ve etiketler kaybolurdu. Bir komşunun k uzaklıktaki itmesine (k) 50k uzakta eşit: büyük çizgeyi
// sıkıştırmaz (sıkışan çizgede itme çifti ve adım maliyeti artar).
constexpr float kGravity = 0.02F;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr int kHalfBits = 32;
constexpr std::uint64_t kLowMask = 0xFFFFFFFFULL;
constexpr double kHalfRange = 4294967296.0;  // 2^32

// FNV-1a: tohum sabit ve platformdan bağımsız (std::hash değil).
std::uint64_t key_hash(std::string_view key) {
    std::uint64_t hash = kFnvOffset;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

// Özetin iki yarısı [-0.5, 0.5) aralığında: düğüm sırasından bağımsız başlangıç konumu.
LayoutPoint unit_offset(std::string_view key) {
    const std::uint64_t hash = key_hash(key);
    return {static_cast<float>(static_cast<double>(hash & kLowMask) / kHalfRange - 0.5),
            static_cast<float>(static_cast<double>(hash >> kHalfBits) / kHalfRange - 0.5)};
}

std::int64_t cell_of(float value) {
    return static_cast<std::int64_t>(std::floor(value / kCutoff));
}

std::uint64_t cell_key(std::int64_t x, std::int64_t y) {
    return (static_cast<std::uint64_t>(x) << kHalfBits) ^ (static_cast<std::uint64_t>(y) & kLowMask);
}

// std::hypot taşmaya karşı yavaştır; konumlar sınırlı, kare toplamı float'ta taşmaz.
float length(LayoutPoint p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

}  // namespace

bool GraphLayout::set_graph(std::vector<std::string> keys, std::vector<Edge> edges) {
    if (keys == keys_ && edges == edges_)
        return false;
    std::unordered_map<std::string_view, std::size_t> previous;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        previous.emplace(keys_[i], i);
    const float side = kK * std::sqrt(static_cast<float>(std::max<std::size_t>(keys.size(), 1)));
    std::vector<LayoutPoint> positions(keys.size());
    std::vector<char> pinned(keys.size(), 0);
    std::vector<char> placed(keys.size(), 0);
    bool kept = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (const auto found = previous.find(keys[i]); found != previous.end()) {
            positions[i] = positions_[found->second];
            pinned[i] = pinned_[found->second];
            placed[i] = 1;
            kept = true;
            continue;
        }
        const LayoutPoint unit = unit_offset(keys[i]);
        positions[i] = {unit.x * side, unit.y * side};
    }
    // Eklenen not yerleşmiş çizgenin içinde, komşusunun yanında doğar: tüm harita yeniden dağılmaz.
    for (const Edge& edge : edges) {
        for (const auto& [node, other] : {std::pair{edge.from, edge.to}, std::pair{edge.to, edge.from}}) {
            if (placed[node] != 0 || placed[other] == 0)
                continue;
            const LayoutPoint unit = unit_offset(keys[node]);
            positions[node] = {positions[other].x + unit.x * 2.0F * kNeighbourOffset,
                               positions[other].y + unit.y * 2.0F * kNeighbourOffset};
            placed[node] = 1;
        }
    }
    keys_ = std::move(keys);
    edges_ = std::move(edges);
    positions_ = std::move(positions);
    pinned_ = std::move(pinned);
    const float initial = std::max(side * kInitialTemperatureShare, kWarmTemperature);
    temperature_ = keys_.empty() ? 0.0F : (kept ? std::max(temperature_, kWarmTemperature) : initial);
    return true;
}

bool GraphLayout::settled() const {
    return temperature_ < kMinTemperature;
}

bool GraphLayout::step(int iterations) {
    for (int i = 0; i < iterations && !settled(); ++i)
        iterate();
    return !settled();
}

void GraphLayout::pin(std::size_t node, LayoutPoint at) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y))  // fare koordinatından gelir: sınırda savunma
        return;
    positions_[node] = at;
    pinned_[node] = 1;
    temperature_ = std::max(temperature_, kWarmTemperature);
}

void GraphLayout::iterate() {
    const std::size_t count = positions_.size();
    displacement_.assign(count, {});
    // Izgara: (hücre, düğüm) sıralı dizi; hücre aralığı ikili aramayla. Kare başına bellek ayırmaz.
    cells_.clear();
    for (std::size_t i = 0; i < count; ++i)
        cells_.emplace_back(cell_key(cell_of(positions_[i].x), cell_of(positions_[i].y)), i);
    std::ranges::sort(cells_);

    const auto repel = [this](std::size_t i, std::size_t j) {
        LayoutPoint delta{positions_[i].x - positions_[j].x, positions_[i].y - positions_[j].y};
        float distance = length(delta);
        if (distance >= kCutoff)
            return;
        if (distance < kMinDistance) {
            const float angle = kGoldenAngle * static_cast<float>(i + j);
            delta = {kMinDistance * std::cos(angle), kMinDistance * std::sin(angle)};
            distance = kMinDistance;
        }
        const float scale = kK * kK / (distance * distance);  // k²/d, birim vektörle çarpılmış
        displacement_[i].x += delta.x * scale;
        displacement_[i].y += delta.y * scale;
        displacement_[j].x -= delta.x * scale;
        displacement_[j].y -= delta.y * scale;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t x = cell_of(positions_[i].x);
        const std::int64_t y = cell_of(positions_[i].y);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                const auto cell = std::ranges::equal_range(cells_, cell_key(x + dx, y + dy), {},
                                                           &std::pair<std::uint64_t, std::size_t>::first);
                for (const auto& [key, j] : cell) {
                    if (j > i)
                        repel(i, j);
                }
            }
        }
    }
    for (const Edge& edge : edges_) {
        const LayoutPoint delta{positions_[edge.from].x - positions_[edge.to].x,
                                positions_[edge.from].y - positions_[edge.to].y};
        const float scale = std::max(length(delta), kMinDistance) / kK;  // d²/k, birim vektörle çarpılmış
        displacement_[edge.from].x -= delta.x * scale;
        displacement_[edge.from].y -= delta.y * scale;
        displacement_[edge.to].x += delta.x * scale;
        displacement_[edge.to].y += delta.y * scale;
    }
    for (std::size_t i = 0; i < count; ++i) {
        displacement_[i].x -= positions_[i].x * kGravity;
        displacement_[i].y -= positions_[i].y * kGravity;
        const float moved = length(displacement_[i]);
        if (pinned_[i] != 0 || moved <= 0.0F)
            continue;
        const float scale = std::min(moved, temperature_) / moved;
        positions_[i].x += displacement_[i].x * scale;
        positions_[i].y += displacement_[i].y * scale;
    }
    temperature_ *= kCooling;
    ++iterations_done_;
}

double GraphLayout::energy() const {
    const auto distance = [this](std::size_t a, std::size_t b) {
        return std::max(static_cast<double>(
                            std::hypot(positions_[a].x - positions_[b].x, positions_[a].y - positions_[b].y)),
                        static_cast<double>(kMinDistance));
    };
    constexpr double k = kK;
    double total = 0.0;
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        for (std::size_t j = i + 1; j < positions_.size(); ++j) {
            if (const double d = distance(i, j); d < kCutoff)
                total += k * k * std::log(kCutoff / d);
        }
    }
    for (const Edge& edge : edges_) {
        const double d = distance(edge.from, edge.to);
        total += d * d * d / (3.0 * k);
    }
    for (const LayoutPoint p : positions_)
        total += kGravity * (static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y) / 2.0;
    return total;
}

}  // namespace noteapp::app
