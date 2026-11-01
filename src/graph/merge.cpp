#include "graph/merge.hpp"

#include "graph/oplog.hpp"
#include "graph/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace noteapp::graph::detail {

namespace {

// Tasarım: 5.000 çerçevede bir commit. Yazma kilidi arada bırakılır, uygulama beklemez.
constexpr std::size_t kFramesPerCommit = 5000;
// Yazma kilidi çerçeve sayısına değil işe göre de bırakılır: küçük çerçeveler bayt, ağır işlemler süre
// sınırına takılır.
constexpr std::size_t kBytesPerCommit = 64U * 1024U * 1024U;
constexpr std::chrono::milliseconds kTimePerCommit{1000};
// SQLite'ın meşgul bekleyişi 100 ms'ye kadar uyur (sqliteDefaultBusyCallback): pencere birkaç uyanışta bir
// denk gelir.
constexpr std::chrono::milliseconds kWriterWindow{20};
constexpr const char* kMergeLimitKey = "graph.merge_owner_limit";
// Kenar başına özellik x silme karşılaştırması. Aşan kenarın özellikleri düşer: küme fonksiyonu, cihazlar
// aynı karar.
constexpr std::size_t kMaxEdgePropDecisions = std::size_t{1} << 24U;

Error corrupt(std::string reason) {
    return Error{Error::Code::Corrupt, "graph.corrupt", {{"reason", std::move(reason)}}};
}

ApplyContext context_of(const DeviceId& device, const Frame& frame) {
    std::vector<Dot> seen = frame.deps;
    seen.push_back(Dot{device, frame.seq - 1});
    return ApplyContext{Dot{device, frame.seq}, frame.hlc, CausalContext(std::move(seen))};
}

bool sees(const ApplyContext& reader, const Dot& dot) {
    return dot == reader.dot || reader.seen.covers(dot);
}

bool sees_any(const ApplyContext& reader, const std::vector<Dot>& dots) {
    return std::ranges::any_of(dots, [&](const Dot& dot) { return sees(reader, dot); });
}

bool sees_all(const ApplyContext& reader, const std::vector<Dot>& dots) {
    return std::ranges::all_of(dots, [&](const Dot& dot) { return sees(reader, dot); });
}

// Düğümün dots'unu ilerleten işlemler. Kenar düğüme yazma sayılmaz: silinmiş düğümü canlandırmaz.
std::optional<NodeId> written_node(const Op& op) {
    if (const auto* put = std::get_if<NodePut>(&op))
        return put->id;
    if (const auto* add = std::get_if<LabelAdd>(&op))
        return add->node;
    if (const auto* del = std::get_if<LabelDel>(&op))
        return del->node;
    if (const auto* set = std::get_if<PropSet>(&op))
        if (const auto* node = std::get_if<NodeId>(&set->owner))
            return *node;
    return std::nullopt;
}

std::optional<EdgeId> propped_edge(const Op& op) {
    if (const auto* set = std::get_if<PropSet>(&op))
        if (const auto* edge = std::get_if<EdgeId>(&set->owner))
            return *edge;
    return std::nullopt;
}

Result<std::int64_t> last_op_id(Store& store) {
    Result<Statement> last = store.statement(StatementId::OpLastId);
    if (!last)
        return std::unexpected(last.error());
    const Result<bool> row = last->step();
    if (!row)
        return std::unexpected(row.error());
    return *row ? last->column_int(0) : std::int64_t{0};
}

// false: bu kimlikte çerçeve yok.
template <class Visit> Result<bool> visit_stored_frame(Store& store, std::int64_t id, Visit& visit) {
    DeviceId device{};
    Result<Frame> frame = [&]() -> Result<Frame> {
        Result<Statement> row = store.statement(StatementId::OpById);
        if (!row)
            return std::unexpected(row.error());
        row->bind_int(1, id);
        const Result<bool> found = row->step();
        if (!found)
            return std::unexpected(found.error());
        if (!*found)
            return Frame{};
        const std::span<const std::byte> blob = row->column_blob(0);
        if (blob.size() != device.size())
            return std::unexpected(corrupt("bad_op_device"));
        std::ranges::copy(blob, device.begin());
        Result<Frame> decoded = decode_frame(row->column_blob(2));
        if (!decoded && decoded.error().code != Error::Code::LimitExceeded)
            return std::unexpected(corrupt("bad_stored_frame"));
        return decoded;
    }();
    if (!frame)
        return std::unexpected(frame.error());
    if (frame->seq == 0)
        return false;
    if (const Result<void> visited = visit(device, *frame); !visited)
        return std::unexpected(visited.error());
    return true;
}

// ops'u yazılma sırasıyla dolaşır; bu sıra nedenseldir, çünkü her çerçeve teslim kapısından geçmiştir.
// Birincil anahtarda nokta okuma: aralık okumasının planı küçük tabloda sqlite_stat1 ile tam taramaya döner
// ve rowid INDEXED BY ile adlandırılamaz. Satır okunup ifade bırakılır: ziyaretçi aynı bağlantıda yazabilir.
template <class Visit>
Result<std::int64_t> for_each_stored_frame_after(Store& store, std::int64_t after, Visit&& visit) {
    const Result<std::int64_t> last = last_op_id(store);
    if (!last)
        return std::unexpected(last.error());
    for (std::int64_t id = after + 1; id <= *last; ++id) {
        const Result<bool> visited = visit_stored_frame(store, id, visit);
        if (!visited)
            return std::unexpected(visited.error());
    }
    return std::max(after, *last);
}

template <class Visit> Result<void> for_each_stored_frame(Store& store, Visit&& visit) {
    const Result<std::int64_t> done = for_each_stored_frame_after(store, 0, std::forward<Visit>(visit));
    if (!done)
        return std::unexpected(done.error());
    return {};
}

// Op kümesinden her işlemin kaderini verir; hiçbir karar geliş sırasına bakmaz. Sonuç küme aynıysa aynıdır.
// - Silme ancak sahibinin her yazmasını görmüşse ya da yazma onu görmüşse etkilidir.
// - Kenar kimliği: kendinden farklı kimliği görmüş put aday olamaz; adaylardan en küçüğü kazanır.
// - Kenar işlemi, ucun etkili silmesini görmemişse ya da ucun hiçbir yazmasını görmemişse düşer.
// - Kenar özelliği, ayakta bir put görmemişse düşer.
// Her karar O(işlem x cihaz): cihaz sayısı kMaxPeers ile sınırlı, görme cihaz başına tekdüze. Log düşmanca
// olsa da iç içe "her yazma x her silme" taraması yoktur. Girdi sayısı saklı çerçevelerin sahipleridir:
// sınırı Merger dayatır (IngestLimits::max_merge_owners), sığmayan çerçeve ops'a girmez.
class History {
public:
    void add(const DeviceId& device, const Frame& frame) {
        const std::size_t index = frames_.size();
        frames_.push_back(info_of(device, frame));
        for (std::size_t position = 0; position < frame.ops.size(); ++position)
            add_op(At{index, position}, frame.ops[position]);
    }

    void decide() {
        for (auto& [id, node] : nodes_) {
            keep_effective_deletes(node.deletes, node.writes);
            node.earliest_writes = earliest_per_device(node.writes);
            node.latest_deletes = latest_per_device(node.deletes);
        }
        for (auto& [id, edge] : edges_)
            decide_edge(edge);
    }

    bool keeps(std::size_t frame, std::size_t position, const Op& op) const {
        if (const auto* del = std::get_if<NodeDel>(&op)) {
            const auto node = nodes_.find(del->id);
            return node != nodes_.end() && std::ranges::binary_search(node->second.deletes, frame);
        }
        if (const auto* put = std::get_if<EdgePut>(&op))
            return edge_keeps(put->id, At{frame, position});
        if (const auto* del = std::get_if<EdgeDel>(&op)) {
            const auto edge = edges_.find(del->id);
            return edge != edges_.end() && std::ranges::binary_search(edge->second.deletes, frame);
        }
        if (const std::optional<EdgeId> edge = propped_edge(op))
            return edge_keeps(*edge, At{frame, position});
        return true;
    }

private:
    struct At {
        std::size_t frame;
        std::size_t op;
        auto operator<=>(const At&) const = default;
    };
    // seen: cihaz sıralı, cihaz başına en büyük seq; çerçevenin kendisi dahil.
    struct FrameInfo {
        Dot dot;
        std::vector<Dot> seen;
    };
    struct NodeHistory {
        std::vector<At> writes;
        std::vector<std::size_t> deletes;
        std::vector<At> earliest_writes;
        std::vector<std::size_t> latest_deletes;
    };
    struct PutAt {
        At at;
        NodeId src;
        std::string type;
        NodeId dst;
    };
    struct EdgeHistory {
        std::vector<PutAt> puts;
        std::vector<At> props;
        std::vector<std::size_t> deletes;
        std::vector<At> kept;
    };

    static FrameInfo info_of(const DeviceId& device, const Frame& frame) {
        std::vector<Dot> seen = frame.deps;
        seen.push_back(Dot{device, frame.seq});
        std::ranges::sort(seen);
        // Aynı cihazın en büyüğü sonda kalır: tersten tekilleştirilir.
        std::vector<Dot> unique;
        unique.reserve(seen.size());
        for (const Dot& dot : seen) {
            if (!unique.empty() && unique.back().dev == dot.dev)
                unique.back() = dot;
            else
                unique.push_back(dot);
        }
        return FrameInfo{Dot{device, frame.seq}, std::move(unique)};
    }

    std::uint64_t seen_on(std::size_t frame, const DeviceId& device) const {
        const std::vector<Dot>& seen = frames_[frame].seen;
        const auto found = std::ranges::lower_bound(seen, device, {}, &Dot::dev);
        return found != seen.end() && found->dev == device ? found->seq : 0;
    }

    static void add_unique(std::vector<std::size_t>& frames, std::size_t frame) {
        if (frames.empty() || frames.back() != frame)
            frames.push_back(frame);
    }

    // Çerçeve başına ilk konum yeter: "önceki yazma var mı" sorusu için en erkeni belirleyicidir. Düşmanca
    // çerçevenin 65.536 işlemi de karşılaştırmaları büyütmez.
    static void add_write(std::vector<At>& writes, At at) {
        if (writes.empty() || writes.back().frame != at.frame)
            writes.push_back(at);
    }

    void add_put(EdgeHistory& edge, At at, const EdgePut& put) {
        // Art arda gelen aynı kimlik karara bir şey katmaz; aynı çerçevede tekrar da çoğaltmaz.
        if (!edge.puts.empty() && edge.puts.back().at.frame == at.frame && edge.puts.back().src == put.src &&
            edge.puts.back().type == put.type && edge.puts.back().dst == put.dst)
            return;
        edge.puts.push_back(PutAt{at, put.src, put.type, put.dst});
    }

    void add_op(At at, const Op& op) {
        if (const std::optional<NodeId> node = written_node(op)) {
            add_write(nodes_[*node].writes, at);
        } else if (const auto* del = std::get_if<NodeDel>(&op)) {
            add_unique(nodes_[del->id].deletes, at.frame);
        } else if (const auto* put = std::get_if<EdgePut>(&op)) {
            add_put(edges_[put->id], at, *put);
        } else if (const auto* edge_del = std::get_if<EdgeDel>(&op)) {
            add_unique(edges_[edge_del->id].deletes, at.frame);
        } else if (const std::optional<EdgeId> edge = propped_edge(op)) {
            add_write(edges_[*edge].props, at);
        }
    }

    bool frame_sees(std::size_t reader, std::size_t written) const {
        return seen_on(reader, frames_[written].dot.dev) >= frames_[written].dot.seq;
    }

    // Aynı çerçevede konum sırası, farklı çerçevede görme.
    bool happened_before(At first, At second) const {
        return first.frame == second.frame ? first.op < second.op : frame_sees(second.frame, first.frame);
    }

    std::vector<At> earliest_per_device(const std::vector<At>& writes) const {
        std::map<DeviceId, At> firsts;
        for (const At write : writes)
            firsts.try_emplace(frames_[write.frame].dot.dev, write);
        std::vector<At> result;
        for (const auto& [device, at] : firsts)
            result.push_back(at);
        return result;
    }

    std::vector<std::size_t> latest_per_device(const std::vector<std::size_t>& deletes) const {
        std::map<DeviceId, std::size_t> lasts;
        for (const std::size_t del : deletes)
            lasts[frames_[del].dot.dev] = del;
        std::vector<std::size_t> result;
        for (const auto& [device, frame] : lasts)
            result.push_back(frame);
        return result;
    }

    // Silme d etkili: her yazma w için d, w'yi görür ya da w, d'yi görür. d'nin görmediği yazmalar cihaz
    // başına seq sonekidir; hepsinin d'yi görmesi, sonekin "d'nin cihazında gördüğü" asgarisine bakılarak
    // sorulur.
    bool sees_or_is_seen_by_all(std::size_t del, const std::map<DeviceId, std::vector<std::size_t>>& writes,
                                const std::map<DeviceId, std::vector<std::uint64_t>>& suffix_minimum) const {
        const Dot& dot = frames_[del].dot;
        for (const auto& [device, frames] : writes) {
            const std::uint64_t seen = seen_on(del, device);
            const auto first_unseen = std::ranges::upper_bound(
                frames, seen, {}, [&](std::size_t frame) { return frames_[frame].dot.seq; });
            if (first_unseen == frames.end())
                continue;
            const auto index = static_cast<std::size_t>(first_unseen - frames.begin());
            if (suffix_minimum.at(device)[index] < dot.seq)
                return false;
        }
        return true;
    }

    void keep_effective_deletes(std::vector<std::size_t>& deletes, const std::vector<At>& writes) const {
        if (deletes.empty() || writes.empty())
            return;
        std::map<DeviceId, std::vector<std::size_t>> by_device;
        for (const At write : writes)
            by_device[frames_[write.frame].dot.dev].push_back(write.frame);
        std::map<DeviceId, std::vector<std::size_t>> deletes_by_device;
        for (const std::size_t del : deletes)
            deletes_by_device[frames_[del].dot.dev].push_back(del);

        std::vector<std::size_t> effective;
        for (const auto& [deleter, device_deletes] : deletes_by_device) {
            std::map<DeviceId, std::vector<std::uint64_t>> suffix_minimum;
            for (const auto& [device, frames] : by_device) {
                std::vector<std::uint64_t> minimum(frames.size());
                std::uint64_t smallest = UINT64_MAX;
                for (std::size_t i = frames.size(); i-- > 0;) {
                    smallest = std::min(smallest, seen_on(frames[i], deleter));
                    minimum[i] = smallest;
                }
                suffix_minimum.emplace(device, std::move(minimum));
            }
            for (const std::size_t del : device_deletes)
                if (sees_or_is_seen_by_all(del, by_device, suffix_minimum))
                    effective.push_back(del);
        }
        std::ranges::sort(effective);
        deletes = std::move(effective);
    }

    static bool same_identity(const PutAt& first, const PutAt& second) {
        return first.src == second.src && first.type == second.type && first.dst == second.dst;
    }

    // Görerek kimlik değiştirmeye kalkan kaybeder; birbirini görmeyenler arasında küme fonksiyonu seçer.
    // Cihaz başına ilk put ve ondan farklı kimlikli ilk put yeter: başka kimliği görmek, bunlardan birini
    // görmek demektir.
    const PutAt* winning_put(const EdgeHistory& edge) const {
        struct FirstPuts {
            const PutAt* first = nullptr;
            const PutAt* first_other = nullptr;
        };
        std::map<DeviceId, FirstPuts> firsts;
        for (const PutAt& put : edge.puts) {
            FirstPuts& device = firsts[frames_[put.at.frame].dot.dev];
            if (device.first == nullptr)
                device.first = &put;
            else if (device.first_other == nullptr && !same_identity(*device.first, put))
                device.first_other = &put;
        }
        const auto saw_other_identity = [&](const PutAt& put) {
            return std::ranges::any_of(firsts, [&](const auto& entry) {
                const FirstPuts& device = entry.second;
                const PutAt* other = same_identity(*device.first, put) ? device.first_other : device.first;
                return other != nullptr && happened_before(other->at, put.at);
            });
        };
        const PutAt* winner = nullptr;
        for (const PutAt& put : edge.puts) {
            if (winner != nullptr &&
                std::tie(put.src, put.type, put.dst) >= std::tie(winner->src, winner->type, winner->dst))
                continue;
            if (!saw_other_identity(put))
                winner = &put;
        }
        return winner;
    }

    bool end_blocks(At at, const NodeId& end) const {
        const auto node = nodes_.find(end);
        if (node == nodes_.end())
            return true;
        const NodeHistory& history = node->second;
        const bool saw_a_write = std::ranges::any_of(history.earliest_writes,
                                                     [&](At write) { return happened_before(write, at); });
        return !saw_a_write || std::ranges::any_of(history.latest_deletes, [&](std::size_t del) {
            return !frame_sees(at.frame, del);
        });
    }

    // Cihazın en son görülen put'u yeter: bir silme onu görmediyse, daha sonrakileri de görmemiştir... tersi
    // de: daha eskisini görmeyen silme sonrakini hiç görmez, dolayısıyla eski geçerliyse yenisi de
    // geçerlidir.
    bool saw_live_put(At prop, const std::map<DeviceId, std::vector<At>>& live_by_device,
                      const std::vector<std::size_t>& deletes) const {
        return std::ranges::any_of(live_by_device, [&](const auto& entry) {
            const std::vector<At>& puts = entry.second;
            const std::uint64_t seen = seen_on(prop.frame, entry.first);
            auto after =
                std::ranges::upper_bound(puts, seen, {}, [&](At put) { return frames_[put.frame].dot.seq; });
            while (after != puts.begin() && !happened_before(*(after - 1), prop))
                --after;
            if (after == puts.begin())
                return false;
            const At put = *(after - 1);
            return std::ranges::none_of(deletes, [&](std::size_t del) {
                return frame_sees(prop.frame, del) && frame_sees(del, put.frame);
            });
        });
    }

    void decide_edge(EdgeHistory& edge) const {
        const PutAt* winner = winning_put(edge);
        if (winner == nullptr) {
            edge.deletes.clear();
            return;
        }
        const auto blocked = [&](At at) {
            return end_blocks(at, winner->src) || end_blocks(at, winner->dst);
        };
        std::map<DeviceId, std::vector<At>> live_by_device;
        edge.kept.clear();
        for (const PutAt& put : edge.puts) {
            if (same_identity(put, *winner) && !blocked(put.at)) {
                live_by_device[frames_[put.at.frame].dot.dev].push_back(put.at);
                edge.kept.push_back(put.at);
            }
        }
        // Silme yoksa karşılaştırma da yoktur: sınır hızlı yolun gördüğü durumu değiştirmez.
        const bool within_budget =
            edge.deletes.empty() || edge.props.size() <= kMaxEdgePropDecisions / edge.deletes.size();
        if (within_budget)
            for (const At prop : edge.props)
                if (!blocked(prop) && saw_live_put(prop, live_by_device, edge.deletes))
                    edge.kept.push_back(prop);
        std::ranges::sort(edge.kept);
        keep_effective_deletes(edge.deletes, edge.kept);
    }

    bool edge_keeps(const EdgeId& id, At at) const {
        const auto edge = edges_.find(id);
        return edge != edges_.end() && std::ranges::binary_search(edge->second.kept, at);
    }

    std::vector<FrameInfo> frames_;
    std::map<NodeId, NodeHistory> nodes_;
    std::map<EdgeId, EdgeHistory> edges_;
};

// ponytail: tüm graph ops'tan yeniden kurulur, O(ops). Yalnız yerinde uygulamanın aynı sonucu vereceği
// kanıtlanamayan uzak çerçevede koşar; sıklaşırsa kapsam etkilenen sahiplere daraltılır.
Result<void> rebuild(Store& store) {
    History history;
    const Result<void> scanned =
        for_each_stored_frame(store, [&](const DeviceId& device, const Frame& frame) -> Result<void> {
            history.add(device, frame);
            return {};
        });
    if (!scanned)
        return scanned;
    history.decide();
    if (const Result<void> cleared = store.clear_graph(); !cleared)
        return cleared;
    std::size_t index = 0;
    return for_each_stored_frame(store, [&](const DeviceId& device, const Frame& frame) -> Result<void> {
        const ApplyContext context = context_of(device, frame);
        for (std::size_t position = 0; position < frame.ops.size(); ++position) {
            if (!history.keeps(index, position, frame.ops[position]))
                continue;
            const Result<void> applied = store.apply(context, frame.ops[position]);
            // History kazananı seçti; kalan BadFrame yalnız görünmeyen bir yoldur ve çerçeve içeriği ingest'i
            // düşüremez. İşlem etkisiz kalır, karar yine küme fonksiyonudur.
            if (!applied && applied.error().code != Error::Code::BadFrame)
                return applied;
        }
        ++index;
        return {};
    });
}

const WriteFacts* facts_of(const std::map<NodeId, WriteFacts>& nodes, const NodeId& id) {
    const auto found = nodes.find(id);
    return found == nodes.end() ? nullptr : &found->second;
}

}  // namespace

void WriteFacts::note_delete(const Dot& dot) {
    const auto found = std::ranges::find_if(deletes, [&](const Dot& known) { return known.dev == dot.dev; });
    if (found != deletes.end())
        *found = dot;
    else
        deletes.push_back(dot);
}

void WriteFacts::note_write(const Dot& dot) {
    const auto same_device = [&](const Dot& known) { return known.dev == dot.dev; };
    if (std::ranges::none_of(earliest, same_device))
        earliest.push_back(dot);
    if (const auto found = std::ranges::find_if(latest, same_device); found != latest.end())
        *found = dot;
    else
        latest.push_back(dot);
}

Result<Merger> Merger::open(Store& store, std::size_t max_owners) {
    Merger merger(store, max_owners);
    if (const Result<void> caught_up = merger.catch_up(); !caught_up)
        return std::unexpected(caught_up.error());
    return merger;
}

Result<void> Merger::catch_up() {
    const Result<std::int64_t> scanned = for_each_stored_frame_after(
        *store_, scanned_id_, [&](const DeviceId& device, const Frame& frame) -> Result<void> {
            note_frame(context_of(device, frame), frame);
            if (owner_count() > max_owners_)
                return std::unexpected(Error{Error::Code::LimitExceeded, kMergeLimitKey, {}});
            return {};
        });
    if (!scanned)
        return std::unexpected(scanned.error());
    scanned_id_ = *scanned;
    return {};
}

void Merger::note_frame(const ApplyContext& context, const Frame& frame) {
    for (const Op& op : frame.ops) {
        if (const std::optional<NodeId> node = written_node(op)) {
            nodes_[*node].note_write(context.dot);
        } else if (const auto* del = std::get_if<NodeDel>(&op)) {
            nodes_[del->id].note_delete(context.dot);
        } else if (const auto* put = std::get_if<EdgePut>(&op)) {
            const bool unsafe = edge_put_is_unsafe(context, *put);
            const bool differs = edge_identity_differs(*put);
            const auto [edge, created] = edges_.try_emplace(put->id);
            edge->second.contested = edge->second.contested || differs;
            if (created || edge->second.type.empty()) {
                edge->second.src = put->src;
                edge->second.type = put->type;
                edge->second.dst = put->dst;
            }
            edge->second.suspect = edge->second.suspect || unsafe;
            edge->second.writes.note_write(context.dot);
        } else if (const auto* edge_del = std::get_if<EdgeDel>(&op)) {
            edges_[edge_del->id].writes.note_delete(context.dot);
        } else if (const std::optional<EdgeId> edge = propped_edge(op)) {
            edges_[*edge].writes.note_write(context.dot);
        }
    }
}

bool Merger::edge_identity_differs(const EdgePut& put) const {
    const auto edge = edges_.find(put.id);
    return edge != edges_.end() && !edge->second.type.empty() &&
           (edge->second.src != put.src || edge->second.type != put.type || edge->second.dst != put.dst);
}

bool Merger::edge_put_is_unsafe(const ApplyContext& context, const EdgePut& put) const {
    if (edge_identity_differs(put))
        return true;
    if (const auto edge = edges_.find(put.id);
        edge != edges_.end() && !sees_all(context, edge->second.writes.deletes))
        return true;
    for (const NodeId& end : {put.src, put.dst}) {
        const WriteFacts* facts = facts_of(nodes_, end);
        if (facts == nullptr || !sees_any(context, facts->earliest) || !sees_all(context, facts->deletes))
            return true;
    }
    return false;
}

// Özellik, gördüğü put'un ayakta olduğunu ancak hiç silme ve şüpheli put yoksa bilir. Görülmeyen kenar satırı
// da yoktur: yazma her iki yolda düşer, bu yüzden tanınmayan kenar güvenlidir.
bool Merger::edge_prop_is_unsafe(const ApplyContext& context, const EdgeId& id) const {
    const auto edge = edges_.find(id);
    if (edge == edges_.end())
        return false;
    if (edge->second.suspect || !edge->second.writes.deletes.empty() ||
        !sees_any(context, edge->second.writes.earliest))
        return true;
    for (const NodeId& end : {edge->second.src, edge->second.dst}) {
        const WriteFacts* facts = facts_of(nodes_, end);
        if (facts != nullptr && !facts->deletes.empty())
            return true;
    }
    return false;
}

// Tanınmayan uç: kenar her iki yolda düşer, güvenlidir. Tanınan uçta görme şartı aranır.
bool Merger::edge_put_needs_rebuild(const ApplyContext& context, const EdgePut& put) const {
    const auto edge = edges_.find(put.id);
    if (edge_identity_differs(put) ||
        (edge != edges_.end() && (edge->second.contested || !sees_all(context, edge->second.writes.deletes))))
        return true;
    return std::ranges::any_of(std::array{put.src, put.dst}, [&](const NodeId& end) {
        const WriteFacts* facts = facts_of(nodes_, end);
        return facts != nullptr &&
               (!sees_any(context, facts->earliest) || !sees_all(context, facts->deletes));
    });
}

// Yerinde uygulama, History ile aynı sonucu verdiği kanıtlanabilen işlemlerde kullanılır; kuşkuda true.
bool Merger::op_needs_rebuild(const ApplyContext& context, const Op& op) const {
    if (const std::optional<NodeId> node = written_node(op)) {
        const WriteFacts* facts = facts_of(nodes_, *node);
        return facts != nullptr && !sees_all(context, facts->deletes);
    }
    if (const auto* del = std::get_if<NodeDel>(&op)) {
        const WriteFacts* facts = facts_of(nodes_, del->id);
        return facts != nullptr && !sees_all(context, facts->latest);
    }
    if (const auto* put = std::get_if<EdgePut>(&op))
        return edge_put_needs_rebuild(context, *put);
    if (const auto* del = std::get_if<EdgeDel>(&op)) {
        const auto edge = edges_.find(del->id);
        return edge != edges_.end() && !sees_all(context, edge->second.writes.latest);
    }
    if (const std::optional<EdgeId> edge = propped_edge(op))
        return edge_prop_is_unsafe(context, *edge);
    return false;
}

// Çekişmeli kenara her put, ilk görülenle farklı olan ve aynı çerçevede kimlik değiştiren put bildirilir.
bool Merger::identity_conflict_in(const Frame& frame) const {
    std::map<EdgeId, const EdgePut*> in_frame;
    for (const Op& op : frame.ops) {
        const auto* put = std::get_if<EdgePut>(&op);
        if (put == nullptr)
            continue;
        const auto edge = edges_.find(put->id);
        if (edge_identity_differs(*put) || (edge != edges_.end() && edge->second.contested))
            return true;
        const auto [first, added] = in_frame.try_emplace(put->id, put);
        if (!added && (first->second->src != put->src || first->second->type != put->type ||
                       first->second->dst != put->dst))
            return true;
    }
    return false;
}

Result<void> Merger::store_and_apply(const ApplyContext& context, const Frame& frame,
                                     std::span<const std::byte> payload) {
    Result<Statement> stored = store_->statement(StatementId::OpPut);
    if (!stored)
        return std::unexpected(stored.error());
    stored->bind_blob(1, context.dot.dev)
        .bind_int(2, static_cast<std::int64_t>(context.dot.seq))
        .bind_blob(3, payload);
    if (const Result<void> run = stored->run(); !run)
        return run;
    if (const Result<void> applied = store_->set_applied(context.dot.dev, context.dot.seq); !applied)
        return applied;
    if (const Result<void> observed = store_->observe_hlc(frame.hlc); !observed)
        return observed;
    if (needs_rebuild_)
        return {};
    for (const Op& op : frame.ops)
        if (const Result<void> applied = store_->apply(context, op); !applied)
            return applied;
    return {};
}

Result<void> Merger::apply_frame(const DeviceId& device, const Frame& frame,
                                 std::span<const std::byte> payload) {
    const ApplyContext context = context_of(device, frame);
    last_identity_conflict_ = identity_conflict_in(frame);
    // Yeniden kurma bekliyorsa ara durum zaten geçersizdir: çerçeve yalnız kaydedilir, bitişte hepsi birlikte
    // kurulur. Kimlik çatışması da buraya düşer: kararı History verir, çerçeve kimseyi durduramaz.
    needs_rebuild_ =
        needs_rebuild_ || last_identity_conflict_ ||
        std::ranges::any_of(frame.ops, [&](const Op& op) { return op_needs_rebuild(context, op); });
    Result<void> applied = apply_in_savepoint(context, frame, payload);
    if (!applied && applied.error().code == Error::Code::BadFrame) {
        // Yerinde uygulama satırla çelişti: çerçeve içeriğidir, yerel hata değildir. Kararı yine History
        // verir.
        needs_rebuild_ = true;
        applied = apply_in_savepoint(context, frame, payload);
    }
    if (!applied)
        return applied;
    scanned_id_ = std::max(scanned_id_, store_->last_insert_id());
    note_frame(context, frame);
    return {};
}

Result<void> Merger::apply_in_savepoint(const ApplyContext& context, const Frame& frame,
                                        std::span<const std::byte> payload) {
    if (const Result<void> begun = store_->begin_savepoint(); !begun)
        return begun;
    const Result<void> applied = store_and_apply(context, frame, payload);
    if (!applied) {
        if (const Result<void> rolled_back = store_->rollback_savepoint(); !rolled_back)
            return rolled_back;
        return applied;
    }
    return store_->release_savepoint();
}

Result<void> Merger::finish() {
    if (!needs_rebuild_)
        return {};
    needs_rebuild_ = false;
    ++rebuild_count_;
    return rebuild(*store_);
}

namespace {

// Bir günlük dosyasının ilerleyişi. Bellekte çerçeve tutulmaz: bekleyen çerçevenin yalnız kapı bilgisi kalır,
// sırası gelince dosyadan yeniden okunur ve özet yeniden doğrulanır.
struct LogCursor {
    std::filesystem::path path;
    DeviceId device{};
    std::uint64_t offset = 0;
    std::uint64_t last_seq = 0;
    std::uint64_t valid_end = 0;
    std::uint64_t file_bytes = 0;
    std::optional<std::pair<std::uint64_t, std::vector<Dot>>> waiting;  // seq, deps
    enum class Stop { None, BadFrame, PeerLimit, Budget, MergeLimit } stop = Stop::None;
    bool partial_tail = false;
};

// Rapor girdiyle büyür: düşmanca log her çerçevede kayıt ürettirebilir.
constexpr std::size_t kMaxReportEntries = 4096;
constexpr std::size_t kMaxTouchedProperties = std::size_t{1} << 20U;

// Ingest oturumu: yazma işlemi tembel açılır. Yeni çerçeve yoksa kilit alınmaz ve ops taranmaz.
class IngestSession {
public:
    IngestSession(Store& store, MergeReport& report, const IngestLimits& limits)
        : store_(store), report_(report), limits_(limits) {}

    IngestSession(const IngestSession&) = delete;
    IngestSession& operator=(const IngestSession&) = delete;

    ~IngestSession() {
        store_.end_txn();
        (void)store_.set_bulk_sync(false);  // asıl hata döndü; bağlantı FULL varsayımına dönsün
    }

    Result<void> run(std::span<const std::filesystem::path> paths) {
        if (const Result<void> bulk = store_.set_bulk_sync(true); !bulk)
            return bulk;
        for (const std::filesystem::path& path : paths)
            load(path);
        // Bir ingest'te kim kabul edilir, geliş sırasına değil cihaz kimliğine bağlıdır.
        std::ranges::stable_sort(logs_, {}, &LogCursor::device);
        if (const Result<void> resumed = resume_interrupted_rebuild(); !resumed)
            return resumed;
        const Result<std::uint64_t> peers = store_.peer_count();
        if (!peers)
            return std::unexpected(peers.error());
        known_peers_ = *peers;
        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (LogCursor& log : logs_) {
                const Result<bool> advanced = advance(log);
                if (!advanced)
                    return std::unexpected(advanced.error());
                progressed = progressed || *advanced;
            }
        }
        if (const Result<void> closed = close_batch(); !closed)
            return closed;
        if (const Result<void> rebuilt = finish_rebuild(); !rebuilt)
            return rebuilt;
        if (const Result<void> stalled = report_positions(); !stalled)
            return stalled;
        return report_conflicts();
    }

private:
    void report_bad(const std::filesystem::path& path, const DeviceId& device, std::uint64_t seq,
                    std::string reason) {
        if (report_.bad_frames.size() >= kMaxReportEntries) {
            ++report_.omitted;
            return;
        }
        report_.bad_frames.push_back(BadFrameReport{path, device, seq, std::move(reason)});
    }

    static std::string reason_of(const Error& error) {
        for (const auto& [name, value] : error.args)
            if (name == "reason" || name == "what")
                return value;
        return error.message_key;
    }

    Result<void> resume_interrupted_rebuild() {
        const Result<bool> interrupted = store_.rebuild_pending();
        if (!interrupted)
            return std::unexpected(interrupted.error());
        if (!*interrupted)
            return {};
        if (const Result<void> opened = ensure_merger(); !opened)
            return opened;
        merger_->request_rebuild();
        return {};
    }

    // Girdi güvenilmezdir: buradaki her sorun rapora gider, ingest'i düşürmez.
    void load(const std::filesystem::path& path) {
        const DeviceId unknown{};
        if (path.has_parent_path() && is_symbolic_link(path.parent_path())) {
            report_bad(path, unknown, 0, "log_directory_symlink");
            return;
        }
        Result<std::optional<LogReader>> opened = LogReader::open(path);
        if (!opened) {
            report_bad(path, unknown, 0, reason_of(opened.error()));
            return;
        }
        if (!opened->has_value())
            return;  // henüz gelmemiş günlük hata değildir
        LogReader& reader = **opened;
        const LogReader::Status header = reader.read_header();
        if (header == LogReader::Status::Damaged) {
            report_bad(path, unknown, 0, reason_of(reader.error()));
            return;
        }
        if (header == LogReader::Status::PartialTail) {
            report_.partial_tails.push_back(PartialTail{path, unknown, 0, reader.file_size()});
            return;
        }
        if (path.filename() != log_file_name(reader.device())) {
            report_bad(path, reader.device(), 0, "device_name_mismatch");
            return;
        }
        LogCursor log;
        log.path = path;
        log.device = reader.device();
        log.offset = reader.offset();
        log.valid_end = reader.offset();
        log.file_bytes = reader.file_size();
        logs_.push_back(std::move(log));
    }

    Result<bool> dependencies_met(std::span<const Dot> deps) {
        for (const Dot& dep : deps) {
            const Result<std::uint64_t> applied = store_.applied_seq(dep.dev);
            if (!applied)
                return std::unexpected(applied.error());
            if (dep.seq > *applied)
                return false;
        }
        return true;
    }

    Result<bool> matches_stored(const DeviceId& device, std::uint64_t seq,
                                std::span<const std::byte> payload) {
        Result<Statement> stored = store_.statement(StatementId::OpGet);
        if (!stored)
            return std::unexpected(stored.error());
        stored->bind_blob(1, device).bind_int(2, static_cast<std::int64_t>(seq));
        const Result<bool> row = stored->step();
        if (!row)
            return std::unexpected(row.error());
        return *row && std::ranges::equal(stored->column_blob(0), payload);
    }

    // Tarama yazma kilidi dışında: yalnız bu ingest'te bir kez. Kilit altında yalnız aradaki yeni çerçeveler
    // okunur.
    Result<void> ensure_merger() {
        if (merger_)
            return {};
        Result<Merger> merger = Merger::open(store_, limits_.max_merge_owners);
        if (!merger)
            return std::unexpected(merger.error());
        merger_.emplace(std::move(*merger));
        return {};
    }

    Result<void> open_batch() {
        if (in_batch_)
            return {};
        if (const Result<void> ready = ensure_merger(); !ready)
            return ready;
        // Kilidi bırakmak yetmez: hemen geri alınırsa bekleyen yazıcı uyandığında yine dolu bulur ve aç
        // kalır.
        if (batches_closed_ > 0)
            std::this_thread::sleep_for(kWriterWindow);
        if (const Result<void> begun = store_.begin_write(); !begun)
            return begun;
        in_batch_ = true;
        frames_in_batch_ = 0;
        bytes_in_batch_ = 0;
        batch_started_ = std::chrono::steady_clock::now();
        return merger_->catch_up();
    }

    // Kilit bırakılır. Bekleyen yeniden kurma bayrakla kalıcıdır: çökerse sonraki ingest tamamlar. Ara durum
    // tutarlıdır ama eskidir: kaydedilmiş çerçeveler kurulana dek görünmez.
    Result<void> close_batch() {
        if (!in_batch_)
            return {};
        if (merger_->rebuild_needed())
            if (const Result<void> flagged = store_.set_rebuild_pending(true); !flagged)
                return flagged;
        in_batch_ = false;
        ++batches_closed_;
        return store_.commit_write();
    }

    Result<void> finish_rebuild() {
        if (!merger_ || !merger_->rebuild_needed())
            return {};
        if (const Result<void> opened = open_batch(); !opened)
            return opened;
        if (const Result<void> finished = merger_->finish(); !finished)
            return finished;
        if (const Result<void> cleared = store_.set_rebuild_pending(false); !cleared)
            return cleared;
        in_batch_ = false;
        ++batches_closed_;
        return store_.commit_write();
    }

    bool batch_is_full() const {
        return frames_in_batch_ >= kFramesPerCommit || bytes_in_batch_ >= kBytesPerCommit ||
               std::chrono::steady_clock::now() - batch_started_ >= kTimePerCommit;
    }

    void remember_touched(const Frame& frame) {
        for (const Op& op : frame.ops) {
            const auto* set = std::get_if<PropSet>(&op);
            if (set == nullptr)
                continue;
            if (touched_.size() >= kMaxTouchedProperties) {
                ++report_.omitted;
                return;
            }
            touched_.emplace(set->owner, set->key);
        }
    }

    Result<void> apply_frame(const LogCursor& log, const Frame& frame, std::span<const std::byte> payload) {
        if (const Result<void> opened = open_batch(); !opened)
            return opened;
        if (const Result<void> applied = merger_->apply_frame(log.device, frame, payload); !applied)
            return applied;
        if (merger_->last_frame_had_identity_conflict())
            report_bad(log.path, log.device, frame.seq, "edge_identity_conflict");
        remember_touched(frame);
        ++frames_in_batch_;
        bytes_in_batch_ += payload.size();
        return batch_is_full() ? close_batch() : Result<void>{};
    }

    // Yeni cihaz sınırın içindeyse kabul edilir. Sınırda durur ve raporlanır: yerel yazma rehin kalmaz.
    bool admit(LogCursor& log, std::uint64_t applied) {
        if (applied != 0 || log.device == store_.device())
            return true;
        if (known_peers_ >= kMaxPeers) {
            log.stop = LogCursor::Stop::PeerLimit;
            return false;
        }
        ++known_peers_;
        return true;
    }

    // Sırası gelmemiş çerçeve dosyadan yeniden okunmaz: kapı bilgisi yeter.
    Result<bool> still_waiting(const LogCursor& log) {
        if (!log.waiting)
            return false;
        const Result<std::uint64_t> applied = store_.applied_seq(log.device);
        if (!applied)
            return std::unexpected(applied.error());
        if (log.waiting->first != *applied + 1)
            return true;
        const Result<bool> ready = dependencies_met(log.waiting->second);
        if (!ready)
            return std::unexpected(ready.error());
        return !*ready;
    }

    // Çerçevenin her işlemi en çok bir yeni sahip getirir. Saklı geçmiş sınırı zaten aşmışsa yeni çerçeve de
    // sığmaz: tarama bu ingest'te bir kez yapılır.
    Result<bool> merger_has_room(const Frame& frame) {
        if (merge_limit_hit_)
            return false;
        if (const Result<void> ready = ensure_merger(); !ready) {
            if (ready.error().message_key != kMergeLimitKey)
                return std::unexpected(ready.error());
            merge_limit_hit_ = true;
            return false;
        }
        return merger_->owner_count() + frame.ops.size() <= limits_.max_merge_owners;
    }

    // Sırayla tekrar, matches_stored yüzünden ops'taki baytlarla sınırlıdır ve bütçeden düşmez: yoksa büyük
    // bir günlük her ingest'te bütçeyi yer, ötekileri sonsuza dek bekletirdi. Aynı çerçeveyi yinelemek
    // sayılır.
    bool charge_budget(const LogCursor& log, const Frame& frame, std::uint64_t applied, std::size_t bytes) {
        if (frame.seq <= applied && frame.seq > log.last_seq)
            return true;
        bytes_read_ += bytes;
        return bytes_read_ <= limits_.max_bytes;
    }

    enum class Step { Advance, Hold, Stop };

    // Çerçevenin kaderi: tekrar, uygulama ya da bekleme. Hata yalnız yerel depolamadandır.
    Result<Step> handle_frame(LogCursor& log, const Frame& frame, std::span<const std::byte> payload) {
        const Result<std::uint64_t> applied = store_.applied_seq(log.device);
        if (!applied)
            return std::unexpected(applied.error());
        if (!charge_budget(log, frame, *applied, payload.size())) {
            log.stop = LogCursor::Stop::Budget;
            return Step::Stop;
        }
        if (frame.seq <= *applied) {
            const Result<bool> same = matches_stored(log.device, frame.seq, payload);
            if (!same)
                return std::unexpected(same.error());
            if (*same)
                return Step::Advance;
            report_bad(log.path, log.device, frame.seq, "seq_digest_mismatch");
            log.stop = LogCursor::Stop::BadFrame;
            return Step::Stop;
        }
        const Result<bool> ready = dependencies_met(frame.deps);
        if (!ready)
            return std::unexpected(ready.error());
        if (frame.seq != *applied + 1 || !*ready) {
            log.waiting.emplace(frame.seq, frame.deps);
            return Step::Hold;
        }
        const Result<bool> room = merger_has_room(frame);
        if (!room)
            return std::unexpected(room.error());
        if (!*room) {
            log.stop = LogCursor::Stop::MergeLimit;
            return Step::Stop;
        }
        if (!admit(log, *applied))
            return Step::Stop;
        if (const Result<void> done = apply_frame(log, frame, payload); !done)
            return std::unexpected(done.error());
        applied_any_ = true;
        return Step::Advance;
    }

    // true: dosya sonuna gelindi ya da çerçeve bekliyor; döngüyü bitirir.
    bool finished_by_status(LogCursor& log, LogReader& reader, LogReader::Status status) {
        switch (status) {
        case LogReader::Status::Frame:
            return false;
        case LogReader::Status::End:
            break;
        case LogReader::Status::PartialTail:
            log.partial_tail = true;
            break;
        case LogReader::Status::Damaged:
            report_bad(log.path, log.device, 0, reason_of(reader.error()));
            log.stop = LogCursor::Stop::BadFrame;
            break;
        }
        log.offset = reader.file_size();
        return true;
    }

    // true: en az bir çerçeve uygulandı.
    Result<bool> advance(LogCursor& log) {
        if (log.stop != LogCursor::Stop::None || log.offset >= log.file_bytes)
            return false;
        const Result<bool> waiting = still_waiting(log);
        if (!waiting)
            return std::unexpected(waiting.error());
        if (*waiting)
            return false;
        log.waiting.reset();

        Result<std::optional<LogReader>> opened = LogReader::open(log.path);
        if (!opened || !opened->has_value()) {
            report_bad(log.path, log.device, 0, opened ? "log_vanished" : reason_of(opened.error()));
            log.stop = LogCursor::Stop::BadFrame;
            return false;
        }
        LogReader& reader = **opened;
        reader.seek(log.offset);
        applied_any_ = false;
        while (true) {
            const LogReader::Status status = reader.next();
            if (finished_by_status(log, reader, status))
                return applied_any_;
            const Result<Frame> frame = decode_frame(reader.payload());
            if (!frame) {
                if (frame.error().code == Error::Code::LimitExceeded)
                    return std::unexpected(frame.error());  // bellek, çerçevenin suçu değil
                report_bad(log.path, log.device, 0, reason_of(frame.error()));
                log.stop = LogCursor::Stop::BadFrame;
                return applied_any_;
            }
            const Result<Step> step = handle_frame(log, *frame, reader.payload());
            if (!step)
                return std::unexpected(step.error());
            if (*step != Step::Advance)
                return applied_any_;
            log.offset = reader.offset();
            log.valid_end = reader.offset();
            log.last_seq = frame->seq;
        }
    }

    static const char* stall_reason(const LogCursor& log) {
        switch (log.stop) {
        case LogCursor::Stop::PeerLimit:
            return "peer_limit";
        case LogCursor::Stop::BadFrame:
            return "bad_frame";
        case LogCursor::Stop::Budget:
            return "ingest_budget";
        case LogCursor::Stop::MergeLimit:
            return "merge_limit";
        case LogCursor::Stop::None:
            break;
        }
        return log.waiting ? "missing_dependency" : nullptr;
    }

    // Durma, yarım kuyruk ve cihaz başına baş: takılan veya geri alınan cihaz görünür.
    Result<void> report_positions() {
        std::map<DeviceId, std::uint64_t> log_heads;
        for (const LogCursor& log : logs_) {
            const Result<std::uint64_t> applied = store_.applied_seq(log.device);
            if (!applied)
                return std::unexpected(applied.error());
            if (const char* reason = stall_reason(log))
                report_.stalled.push_back(StalledLog{log.path, log.device, *applied, reason});
            if (log.partial_tail)
                report_.partial_tails.push_back(
                    PartialTail{log.path, log.device, log.valid_end, log.file_bytes});
            std::uint64_t& head = log_heads[log.device];
            head = std::max(head, log.waiting ? log.waiting->first : log.last_seq);
        }
        for (const auto& [device, log_seq] : log_heads) {
            const Result<std::uint64_t> applied = store_.applied_seq(device);
            if (!applied)
                return std::unexpected(applied.error());
            report_.heads.push_back(DeviceHead{device, log_seq, *applied});
        }
        return {};
    }

    // Çok head'li ve farklı değerli özellikler: uygulama bunlardan kopya kurar (SPEC-graph "body").
    Result<void> report_conflicts() {
        for (const auto& [owner, key] : touched_) {
            const std::span<const std::byte> owner_bytes =
                std::visit([](const auto& id) { return std::span<const std::byte>(id.bytes); }, owner);
            Result<Statement> versions = store_.statement(StatementId::PropVersions);
            if (!versions)
                return std::unexpected(versions.error());
            versions->bind_blob(1, owner_bytes).bind_text(2, key);
            std::optional<Value> first;
            bool differs = false;
            while (!differs) {
                const Result<bool> row = versions->step();
                if (!row)
                    return std::unexpected(row.error());
                if (!*row)
                    break;
                Result<Value> value = store_.load_value(versions->column_blob(3));
                if (!value)
                    return std::unexpected(value.error());
                differs = first.has_value() && *first != *value;
                if (!first)
                    first = std::move(*value);
            }
            if (!differs)
                continue;
            if (report_.conflicts.size() >= kMaxReportEntries)
                ++report_.omitted;
            else
                report_.conflicts.push_back(MergeConflict{owner, key});
        }
        return {};
    }

    Store& store_;
    MergeReport& report_;
    IngestLimits limits_;
    std::vector<LogCursor> logs_;
    std::optional<Merger> merger_;
    bool in_batch_ = false;
    bool applied_any_ = false;
    bool merge_limit_hit_ = false;
    int batches_closed_ = 0;
    std::size_t frames_in_batch_ = 0;
    std::size_t bytes_in_batch_ = 0;
    std::uint64_t bytes_read_ = 0;
    std::chrono::steady_clock::time_point batch_started_;
    std::uint64_t known_peers_ = 0;
    std::set<std::pair<OwnerId, std::string>> touched_;
};

}  // namespace

Result<MergeReport> ingest_logs(Store& store, std::span<const std::filesystem::path> logs,
                                const IngestLimits& limits) {
    MergeReport report;
    IngestSession session(store, report, limits);
    if (const Result<void> ran = session.run(logs); !ran)
        return std::unexpected(ran.error());
    return report;
}

}  // namespace noteapp::graph::detail
