#pragma once

// İç başlık. Uzak çerçevelerin birleştirilmesi: sonuç, op kümesinin saf fonksiyonudur (SPEC-graph
// "Birleşim"). Yalnız ingest içinde kullanılır: Graph::open ve yerel yazma ops'u taramaz.
#include "graph/error.hpp"
#include "graph/graph.hpp"
#include "graph/op.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace noteapp::graph::detail {

class Store;
struct ApplyContext;

inline constexpr std::size_t kDefaultMaxMergeOwners = std::size_t{1} << 23U;

// Sahip başına cihaz başına ilk ve son yazma. "Bir yazmayı görmüş mü" ve "hepsini görmüş mü" soruları için
// tüm yazmaları tutmak gerekmez.
struct WriteFacts {
    std::vector<Dot> earliest;
    std::vector<Dot> latest;
    std::vector<Dot> deletes;  // cihaz başına en son silme: görme aynı cihazda tekdüzedir
    void note_write(const Dot& dot);
    void note_delete(const Dot& dot);
};

struct EdgeFacts {
    WriteFacts writes;
    NodeId src;
    std::string type;
    NodeId dst;
    // Bir put düşmüş olabilir: özellik yazmasının hızlı yolu "put'u görmüş" sayamaz.
    bool suspect = false;
    // İki kimlik görüldü: satır, kazananı tutar ve ilk görülen kimlikle aynı olmayabilir. Yerinde uygulanmaz.
    bool contested = false;
};

// Açık bir yazma işlemi içinde yaşar. Çerçeveler nedensel sırayla verilir; sıra ve tekrar kapısı
// çağıranındır.
class Merger {
public:
    // O(ops): tüm çerçeveleri tarar. Yalnız ingest'in ilk uygulamasında, yazma kilidi alınmadan çağrılır.
    // Sahip sayısı max_owners'ı aşarsa LimitExceeded döner: durum belleği sınırlıdır.
    static Result<Merger> open(Store& store, std::size_t max_owners = kDefaultMaxMergeOwners);
    // Son taramadan beri eklenen çerçeveleri görür (başka süreçteki yerel yazma); kilit altında, O(yeni).
    Result<void> catch_up();

    // Çerçeveyi ops'a kaydeder ve duruma uygular. Yerel depolama hatası dışında hata dönmez: kötü niyetli
    // işlem her cihazda aynı kuralla etkisiz kalır.
    Result<void> apply_frame(const DeviceId& device, const Frame& frame, std::span<const std::byte> payload);
    // Bekleyen yeniden kurmayı yapar; işlem commit edilmeden önce çağrılır.
    Result<void> finish();
    // Son apply_frame'in kimliği çatışan kenar işlemi var mıydı.
    bool last_frame_had_identity_conflict() const { return last_identity_conflict_; }
    int rebuild_count() const { return rebuild_count_; }
    bool rebuild_needed() const { return needs_rebuild_; }
    std::size_t owner_count() const { return nodes_.size() + edges_.size(); }
    void request_rebuild() { needs_rebuild_ = true; }

private:
    Merger(Store& store, std::size_t max_owners) : store_(&store), max_owners_(max_owners) {}
    void note_frame(const ApplyContext& context, const Frame& frame);
    bool op_needs_rebuild(const ApplyContext& context, const Op& op) const;
    bool edge_put_is_unsafe(const ApplyContext& context, const EdgePut& put) const;
    bool edge_put_needs_rebuild(const ApplyContext& context, const EdgePut& put) const;
    bool edge_prop_is_unsafe(const ApplyContext& context, const EdgeId& edge) const;
    bool edge_identity_differs(const EdgePut& put) const;
    bool identity_conflict_in(const Frame& frame) const;
    Result<void> apply_in_savepoint(const ApplyContext& context, const Frame& frame,
                                    std::span<const std::byte> payload);
    Result<void> store_and_apply(const ApplyContext& context, const Frame& frame,
                                 std::span<const std::byte> payload);

    Store* store_;
    std::size_t max_owners_;
    std::map<NodeId, WriteFacts> nodes_;
    std::map<EdgeId, EdgeFacts> edges_;
    bool needs_rebuild_ = false;
    bool last_identity_conflict_ = false;
    std::int64_t scanned_id_ = 0;
    int rebuild_count_ = 0;
};

// Bir ingest'in okuyacağı toplam yeni çerçeve baytı: aşınca kalan günlükler durur ve sonraki ingest'te sürer.
// Sırayla tekrar edilen çerçeve sayılmaz: ops'taki baytlarla sınırlıdır ve her ingest'te yeniden okunur.
// max_merge_owners: birleşimin durum tuttuğu düğüm + kenar; aşan günlük merge_limit ile durur.
struct IngestLimits {
    std::uint64_t max_bytes = std::uint64_t{8} * 1024U * 1024U * 1024U;
    std::size_t max_merge_owners = kDefaultMaxMergeOwners;
};

Result<MergeReport> ingest_logs(Store& store, std::span<const std::filesystem::path> logs,
                                const IngestLimits& limits = {});

}  // namespace noteapp::graph::detail
