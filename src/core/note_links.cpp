#include "core/note_store_impl.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <variant>

namespace noteapp::core {

using graph::NodeId;

namespace {

// Yol başına tek, sıralı: paralel kenar bir satırdır.
using PathSet = std::map<std::string, NotePath>;

graph::Result<std::set<NodeId>> neighbor_ids(graph::ReadTxn& txn, const NodeId& id,
                                             graph::Direction direction, std::string_view type) {
    auto cursor = txn.neighbors(id, direction, type);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::set<NodeId> ids;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return ids;
        ids.insert((*next)->node);
    }
}

constexpr std::string_view kNameSpace = " \t\r\n\f\v";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

std::string_view trim_name(std::string_view raw) {
    const auto first = raw.find_first_not_of(kNameSpace);
    if (first == std::string_view::npos)
        return {};
    return raw.substr(first, raw.find_last_not_of(kNameSpace) - first + 1);
}

// `at`taki kod noktası; geçersiz dizide valid false, bir bayt ilerler.
struct CodePoint {
    char32_t value = 0;
    std::size_t length = 1;
    bool valid = false;
};

CodePoint code_point_at(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (at + length <= text.size())
        if (const auto decoded = decode_utf8(text.substr(at, length)); decoded && decoded->size() == 1)
            return {(*decoded)[0], length, true};
    return {};
}

// Adda olamayan: C0, DEL ve C1 (satır sonu ya da sekme listede ve terminalde satırı bozar) ve görünmez biçim
// karakterleri (yön ve sıfır genişlik; metni farklı gösterir, Trojan Source). Etiket de bunlarda biter.
std::string_view forbidden_in_name(char32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
        return "control_char";
    if (find_hidden_control(cp) != nullptr)
        return "hidden_control";
    return {};
}

// Okunan ad: senkronla ya da Cypher'la gelmiş, yazma kuralından geçmemiş olabilir. Kırpılır, bozuk ya da
// yasak karakter U+FFFD olur, max_bytes'ta (kod noktası sınırında) kesilir. Graf değişmez: okuma yazmaz,
// cihazlar arasında gidip gelme olmaz. Anahtar (tag_key) bu biçimden türer: " uses" ile "uses" aynı addır.
std::string shown_name(std::string_view raw, std::size_t max_bytes) {
    const std::string_view name = trim_name(raw);
    std::string out;
    for (std::size_t at = 0; at < name.size();) {
        const CodePoint cp = code_point_at(name, at);
        const std::string_view piece =
            cp.valid && forbidden_in_name(cp.value).empty() ? name.substr(at, cp.length) : kReplacement;
        if (piece.size() > max_bytes - out.size())
            break;
        out += piece;
        at += cp.length;
    }
    return std::string(trim_name(out));
}

// RELATED_TO kenarı (SPEC-graph "Adlı ilişkiler"). src/dst kenarın kendi uçlarıdır.
struct Relation {
    graph::EdgeId id;
    NodeId src;
    NodeId dst;
    std::string name;  // shown_name; `name` yoksa ya da metin değilse (Cypher) adsız
    std::string key;   // tag_key(name)
    // Grafta saklı ad (metin değilse boş). Gösterim temizlenir ve kesilir: farklı saklı adlar aynı
    // görünebilir, "aynı ilişkinin kopyası" kararı bununla verilir.
    std::string stored;
    bool directed = false;
};

// Notun RELATED_TO kenarları; both false ise yalnız çıkanlar. Öz döngü yok sayılır. Özellikler gezinme
// bittikten sonra okunur: imleç açıkken başka okuma yapılmaz.
graph::Result<std::vector<Relation>> note_relations(graph::ReadTxn& txn, const NodeId& id, bool both,
                                                    std::size_t max_name_bytes) {
    std::vector<Relation> out;
    for (const graph::Direction direction : {graph::Direction::Out, graph::Direction::In}) {
        if (direction == graph::Direction::In && !both)
            break;
        auto cursor = txn.neighbors(id, direction, kRelatedTo);
        if (!cursor)
            return std::unexpected(cursor.error());
        while (true) {
            auto next = cursor->next();
            if (!next)
                return std::unexpected(next.error());
            if (!*next)
                break;
            const NodeId other = (*next)->node;
            if (other == id)
                continue;
            const bool outgoing = direction == graph::Direction::Out;
            out.push_back(Relation{(*next)->edge, outgoing ? id : other, outgoing ? other : id, {}, {}, {}});
        }
    }
    for (Relation& relation : out) {
        auto name = txn.property(relation.id, kName);
        if (!name)
            return std::unexpected(name.error());
        auto directed = txn.property(relation.id, kDirected);
        if (!directed)
            return std::unexpected(directed.error());
        if (const auto* text = std::get_if<std::string>(&*name)) {
            relation.name = shown_name(*text, max_name_bytes);
            relation.stored = *text;
        }
        relation.key = tag_key(relation.name);
        const bool* flag = std::get_if<bool>(&*directed);
        relation.directed = flag != nullptr && *flag;
    }
    return out;
}

// Yalnız canlı görüntüdeki notlar: çöp, ulaşılamayan not ve not olmayan düğüm orada yoktur.
void add_live_path(PathSet& paths, const Snapshot& snapshot, const NodeId& id, const NodeId& self) {
    if (id == self)
        return;
    if (const auto row = snapshot.live_by_id.find(id); row != snapshot.live_by_id.end()) {
        const NotePath& path = snapshot.live[row->second].info.path;
        paths.emplace(path.string(), path);
    }
}

std::vector<NotePath> sorted_paths(PathSet paths) {
    std::vector<NotePath> out;
    out.reserve(paths.size());
    for (auto& [text, path] : paths)
        out.push_back(std::move(path));
    return out;
}

graph::Result<PathSet> live_neighbors(graph::ReadTxn& txn, const Snapshot& snapshot, const NodeId& id,
                                      graph::Direction direction, std::string_view type) {
    auto ids = neighbor_ids(txn, id, direction, type);
    if (!ids)
        return std::unexpected(ids.error());
    PathSet paths;
    for (const NodeId& neighbor : *ids)
        add_live_path(paths, snapshot, neighbor, id);
    return paths;
}

// Kenar başına bir satır: aynı uç, ad ve yönlü paralel kenar (senkron) kimliği farklı iki satırdır; kaldırma
// ve değiştirme kimlikle tek kenarı hedefler.
graph::Result<std::vector<RelatedNote>> live_relations(graph::ReadTxn& txn, const Snapshot& snapshot,
                                                       const NodeId& id, std::size_t max_name_bytes) {
    auto relations = note_relations(txn, id, true, max_name_bytes);
    if (!relations)
        return std::unexpected(relations.error());
    std::vector<RelatedNote> out;
    for (Relation& relation : *relations) {
        const bool outgoing = relation.src == id;
        const auto row = snapshot.live_by_id.find(outgoing ? relation.dst : relation.src);
        if (row == snapshot.live_by_id.end())
            continue;
        const RelationDirection direction = !relation.directed ? RelationDirection::Undirected
                                            : outgoing         ? RelationDirection::Outgoing
                                                               : RelationDirection::Incoming;
        out.push_back(RelatedNote{snapshot.live[row->second].info.path, std::move(relation.name), direction,
                                  RelationId{relation.id.bytes}});
    }
    std::ranges::sort(out);
    return out;
}

// Metindeki hedefler yazma kuralıyla çözülür. Çözülmeyen eksiktir; çözülüp kenarı olmayan (hedef kaynaktan
// sonra oluştu) çıkan listesine girer: kaynak kaydedilene dek kenar yoktur ama not vardır.
graph::Result<std::vector<std::string>> add_text_targets(graph::ReadTxn& txn, const Snapshot& snapshot,
                                                         const NodeId& id, PathSet& outgoing) {
    auto body = txn.property(id, kBody);
    if (!body)
        return std::unexpected(body.error());
    std::vector<std::string> missing;
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*body);
    if (bytes == nullptr || looks_binary(bytes->data) || bytes->data.find("[[") == std::string::npos)
        return missing;
    const LinkIndex& index = link_index_of(snapshot);
    for (std::string& target : scan_links(bytes->data)) {
        const auto found = index.find(target);
        if (found == index.end())
            missing.push_back(std::move(target));
        else
            add_live_path(outgoing, snapshot, found->second, id);
    }
    return missing;
}

graph::Result<NoteLinks> read_links(graph::ReadTxn& txn, const Snapshot& snapshot, const NodeId& id,
                                    std::size_t max_name_bytes) {
    auto outgoing = live_neighbors(txn, snapshot, id, graph::Direction::Out, kLinksTo);
    if (!outgoing)
        return std::unexpected(outgoing.error());
    auto incoming = live_neighbors(txn, snapshot, id, graph::Direction::In, kLinksTo);
    if (!incoming)
        return std::unexpected(incoming.error());
    auto related = live_relations(txn, snapshot, id, max_name_bytes);
    if (!related)
        return std::unexpected(related.error());
    auto missing = add_text_targets(txn, snapshot, id, *outgoing);
    if (!missing)
        return std::unexpected(missing.error());
    auto tags = read_note_tags(txn, id);
    if (!tags)
        return std::unexpected(tags.error());
    return NoteLinks{sorted_paths(std::move(*outgoing)), sorted_paths(std::move(*incoming)),
                     std::move(*related), std::move(*missing), std::move(*tags)};
}

void sort_unique(std::vector<NoteEdge>& edges) {
    std::ranges::sort(edges);
    const auto duplicates = std::ranges::unique(edges);
    edges.erase(duplicates.begin(), duplicates.end());
}

// Kenar yalnız çıkan yönde okunur: her kenar kaynağında bir kez görülür. Yönsüz ilişkide from yol sırasıyla
// küçük olandır.
graph::Result<std::vector<NoteEdge>> read_relation_edges(graph::ReadTxn& txn, const Snapshot& snapshot,
                                                         std::size_t max_name_bytes) {
    std::vector<NoteEdge> edges;
    for (const NoteRow& row : snapshot.live) {
        auto relations = note_relations(txn, row.id, false, max_name_bytes);
        if (!relations)
            return std::unexpected(relations.error());
        for (Relation& relation : *relations) {
            const auto target = snapshot.live_by_id.find(relation.dst);
            if (target == snapshot.live_by_id.end())
                continue;
            NoteEdge edge{row.info.path,       snapshot.live[target->second].info.path,
                          EdgeKind::RelatedTo, std::move(relation.name),
                          relation.directed,   RelationId{relation.id.bytes}};
            if (!edge.directed && edge.to < edge.from)
                std::swap(edge.from, edge.to);
            edges.push_back(std::move(edge));
        }
    }
    sort_unique(edges);
    return edges;
}

graph::Result<std::vector<NoteEdge>> read_edges(graph::ReadTxn& txn, const Snapshot& snapshot,
                                                std::size_t max_name_bytes) {
    auto edges = read_relation_edges(txn, snapshot, max_name_bytes);
    if (!edges)
        return edges;
    for (const NoteRow& row : snapshot.live) {
        auto ids = neighbor_ids(txn, row.id, graph::Direction::Out, kLinksTo);
        if (!ids)
            return std::unexpected(ids.error());
        for (const NodeId& id : *ids) {
            const auto target = snapshot.live_by_id.find(id);
            if (id == row.id || target == snapshot.live_by_id.end())
                continue;
            edges->push_back(
                NoteEdge{row.info.path, snapshot.live[target->second].info.path, EdgeKind::LinksTo});
        }
    }
    sort_unique(*edges);
    return edges;
}

// Ad anahtarı başına kenar sayısı (paralel kenar da sayılır: read_relation_edges kimlik başına bir satır
// verir, panel ve harita da kenar başına çizer); gösterim en çok kullanılan yazım (map sırası: eşitlikte bayt
// sırası küçük olan).
std::vector<RelationName> count_relation_names(const std::vector<NoteEdge>& edges) {
    struct Tally {
        std::map<std::string, std::size_t> displays;
        std::size_t count = 0;
        std::size_t directed = 0;
    };
    std::map<std::string, Tally> tallies;
    for (const NoteEdge& edge : edges) {
        if (edge.name.empty())
            continue;
        Tally& tally = tallies[tag_key(edge.name)];
        ++tally.displays[edge.name];
        ++tally.count;
        tally.directed += edge.directed ? 1 : 0;
    }
    std::vector<RelationName> out;
    out.reserve(tallies.size());
    for (const auto& [key, tally] : tallies) {
        const auto most = std::ranges::max_element(
            tally.displays, [](const auto& left, const auto& right) { return left.second < right.second; });
        out.push_back(RelationName{key, most->first, tally.count, tally.directed});
    }
    // Anahtar sırası map'ten gelir: kararlı sıralama eşit sayıda onu korur.
    std::ranges::stable_sort(
        out, [](const RelationName& left, const RelationName& right) { return left.count > right.count; });
    return out;
}

std::expected<NodeId, Error> live_note_id(const Snapshot& snapshot, const NotePath& path) {
    const std::string key = path.string();
    const auto row = snapshot.live_by_path.find(key);
    if (row == snapshot.live_by_path.end())
        return std::unexpected(not_found(key));
    return snapshot.live[row->second].id;
}

// --- İlişki yazmaları ---

// Kenarın uçları: yönlüde from→to; yönsüzde küçük kimlik önce, A→B ve B→A aynı kenardır.
std::pair<NodeId, NodeId> relation_ends(const NodeId& from, const NodeId& to, bool directed) {
    if (directed || from < to)
        return {from, to};
    return {to, from};
}

// Kimlik uçlardan ve ad anahtarından türer: iki cihazın aynı kurulumu tek kenardır. Türetilmiş kimlik
// kullanılıyorsa (yeniden adlandırılmış kenar eski adın kimliğini taşır; senkronla gelen bir kenar kimliği
// başka uçlarla gasp etmiş olabilir; `taken`: bu işlemde silinen çift kenarları da) sabit uzunlukta sayaçla
// tuzlanır: `taban ‖ \0 ‖ sayaç (8 bayt LE)`. Ad denetim karakteri taşımaz, `\0` tuzu addan ayırır. Rastgele
// kimlik değildir: aynı durumu gören iki cihaz aynı kenara varır. Deneme başına bir nokta okuması; N kenarlık
// gasp zinciri doğrusal maliyettir.
graph::Result<graph::EdgeId> relation_id(graph::ReadTxn& txn, const NodeId& src, const NodeId& dst,
                                         std::string_view key, const std::set<graph::EdgeId>& taken) {
    const std::string base = id_text(src) + id_text(dst) + std::string(key);
    std::string canonical = base;
    for (std::uint64_t salt = 1;; ++salt) {
        const graph::EdgeId id = graph::derive_edge_id(kRelatedTo, as_bytes(canonical));
        if (!taken.contains(id)) {
            const auto ends = txn.edge(id);
            if (!ends)
                return std::unexpected(ends.error());
            if (!*ends)
                return id;
        }
        canonical = base;
        canonical += '\0';
        for (unsigned shift = 0; shift < 64; shift += 8)
            canonical += static_cast<char>((salt >> shift) & 0xFFU);
    }
}

// Çiftin iki yöndeki ilişkileri: senkronla iki cihazın ters yönde kurduğu aynı adlı ilişki de buradadır.
std::expected<std::vector<Relation>, Error> pair_relations(graph::ReadTxn& txn, const NodeId& one,
                                                           const NodeId& other, std::string_view path,
                                                           std::size_t max_name_bytes) {
    auto relations = note_relations(txn, one, true, max_name_bytes);
    if (!relations)
        return std::unexpected(from_graph(relations.error(), path));
    std::erase_if(*relations,
                  [&](const Relation& relation) { return relation.src != other && relation.dst != other; });
    return std::move(*relations);
}

std::vector<Relation> with_key(const std::vector<Relation>& relations, std::string_view key) {
    std::vector<Relation> out;
    std::ranges::copy_if(relations, std::back_inserter(out),
                         [&](const Relation& relation) { return relation.key == key; });
    return out;
}

std::expected<graph::EdgeId, Error> put_relation(graph::WriteTxn& txn, const NodeId& from, const NodeId& to,
                                                 const std::string& name, bool directed,
                                                 std::span<const Relation> pair, std::string_view path) {
    const auto [src, dst] = relation_ends(from, to, directed);
    std::set<graph::EdgeId> taken;
    for (const Relation& relation : pair)
        taken.insert(relation.id);
    const auto id = relation_id(txn, src, dst, tag_key(name), taken);
    if (!id)
        return std::unexpected(from_graph(id.error(), path));
    NOTEAPP_TRY_GRAPH(txn.put_edge(*id, src, kRelatedTo, dst), path);
    // Adsız ve yönsüz ilişki özellik taşımaz: boş ad ya da false yazılmaz.
    if (!name.empty())
        NOTEAPP_TRY_GRAPH(txn.set_property(*id, kName, graph::Value(name)), path);
    if (directed)
        NOTEAPP_TRY_GRAPH(txn.set_property(*id, kDirected, graph::Value(true)), path);
    return *id;
}

// İki canlı, farklı not; çift üzerindeki yazma `body(txn, first, second)` tek işlemdedir.
template <class F>
std::expected<void, Error> write_pair(NoteStore::Impl& impl, const NotePath& first, const NotePath& second,
                                      F&& body) {
    return impl.write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        const auto one = live_note_id(context.snapshot, first);
        if (!one)
            return std::unexpected(one.error());
        const auto other = live_note_id(context.snapshot, second);
        if (!other)
            return std::unexpected(other.error());
        if (*one == *other)
            return std::unexpected(invalid_path("self_relation", second.string()));
        return body(context.txn, *one, *other);
    });
}

Error relation_not_found() {
    return Error{Error::Code::NotFound, "error.relation_not_found", {}};
}

// Kimliği verilen ilişki üzerinde tek işlemlik yazma. Kenar yoksa, RELATED_TO değilse ya da bir ucu canlı not
// değilse (silindi, çöpte, başka cihazda değişti) NotFound. `body(context, target, pair, path)`: pair çiftin
// iki yöndeki tüm ilişkileri (target dahil), path hedef ucun yolu (hata metni).
template <class F>
std::expected<void, Error> write_relation(NoteStore::Impl& impl, const RelationId& relation, F&& body) {
    const graph::EdgeId id{relation.bytes};
    return impl.write<void>([&](WriteContext& context) -> std::expected<void, Error> {
        const auto ends = context.txn.edge(id);
        if (!ends)
            return std::unexpected(from_graph(ends.error(), {}));
        if (!*ends || (*ends)->type != kRelatedTo || (*ends)->src == (*ends)->dst)
            return std::unexpected(relation_not_found());
        const auto src = context.snapshot.live_by_id.find((*ends)->src);
        const auto dst = context.snapshot.live_by_id.find((*ends)->dst);
        if (src == context.snapshot.live_by_id.end() || dst == context.snapshot.live_by_id.end())
            return std::unexpected(relation_not_found());
        const std::string path = context.snapshot.live[dst->second].info.path.string();
        const auto pair =
            pair_relations(context.txn, (*ends)->src, (*ends)->dst, path, impl.relation_name_bytes);
        if (!pair)
            return std::unexpected(pair.error());
        const auto target = std::ranges::find(*pair, id, &Relation::id);
        if (target == pair->end())
            return std::unexpected(relation_not_found());
        return body(context, *target, *pair, path);
    });
}

Error invalid_relation_name(std::string_view name, std::string_view reason) {
    return Error{Error::Code::InvalidPath,
                 "error.invalid_relation_name",
                 {{"name", std::string(name)}, {"reason", std::string(reason)}}};
}

const std::vector<NotePath>* group_paths(const NoteLinks& links, LinkGroup group) {
    switch (group) {
    case LinkGroup::Incoming:
        return &links.incoming;
    case LinkGroup::Outgoing:
        return &links.outgoing;
    case LinkGroup::Related:
    case LinkGroup::Missing:
        break;
    }
    return nullptr;
}

void append_group(std::vector<LinkLine>& lines, LinkGroup group, std::size_t count) {
    if (count == 0)
        return;
    lines.push_back(LinkLine{group, true, count});
    for (std::size_t i = 0; i < count; ++i)
        lines.push_back(LinkLine{group, false, i});
}

}  // namespace

std::expected<std::string, Error> relation_name(std::string_view raw, std::size_t max_bytes) {
    const std::string_view name = trim_name(raw);
    for (std::size_t at = 0; at < name.size();) {
        const CodePoint cp = code_point_at(name, at);
        if (!cp.valid)
            return std::unexpected(invalid_relation_name(name, "invalid_utf8"));
        if (const std::string_view reason = forbidden_in_name(cp.value); !reason.empty())
            return std::unexpected(invalid_relation_name(name, reason));
        at += cp.length;
    }
    if (name.size() > max_bytes)
        return std::unexpected(invalid_relation_name(name, "too_long"));
    return std::string(name);
}

std::vector<LinkLine> link_lines(const NoteLinks& links) {
    std::vector<LinkLine> lines;
    lines.reserve(links.incoming.size() + links.outgoing.size() + links.related.size() +
                  links.missing.size() + 4);
    append_group(lines, LinkGroup::Incoming, links.incoming.size());
    append_group(lines, LinkGroup::Outgoing, links.outgoing.size());
    append_group(lines, LinkGroup::Related, links.related.size());
    append_group(lines, LinkGroup::Missing, links.missing.size());
    return lines;
}

const NotePath* link_path(const NoteLinks& links, const LinkLine& line) {
    if (line.is_header)
        return nullptr;
    if (line.group == LinkGroup::Related)
        return &links.related[line.index].path;
    const std::vector<NotePath>* paths = group_paths(links, line.group);
    return paths == nullptr ? nullptr : &(*paths)[line.index];
}

std::string link_text(const NoteLinks& links, const LinkLine& line) {
    if (line.is_header)
        return {};
    const NotePath* path = link_path(links, line);
    return path != nullptr ? path->string() : links.missing[line.index];
}

std::string_view link_group_key(LinkGroup group) {
    switch (group) {
    case LinkGroup::Incoming:
        return "links.incoming";
    case LinkGroup::Outgoing:
        return "links.outgoing";
    case LinkGroup::Related:
        return "links.related";
    case LinkGroup::Missing:
        break;
    }
    return "links.missing";
}

std::expected<NoteLinks, Error> NoteStore::links(const NotePath& path) const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot) {
        if (impl_->last_error)
            return std::unexpected(*impl_->last_error);
        return std::unexpected(not_found(path.string()));
    }
    const auto id = live_note_id(*snapshot, path);
    if (!id)
        return std::unexpected(id.error());
    auto txn = impl_->opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), path.string()));
    auto links = read_links(*txn, *snapshot, *id, impl_->relation_name_bytes);
    if (!links)
        return std::unexpected(from_graph(links.error(), path.string()));
    return std::move(*links);
}

std::expected<std::vector<NoteEdge>, Error> NoteStore::edges() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot) {
        if (impl_->last_error)
            return std::unexpected(*impl_->last_error);
        return std::vector<NoteEdge>{};
    }
    auto txn = impl_->opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), {}));
    auto edges = read_edges(*txn, *snapshot, impl_->relation_name_bytes);
    if (!edges)
        return std::unexpected(from_graph(edges.error(), {}));
    return std::move(*edges);
}

std::expected<std::shared_ptr<const std::vector<RelationName>>, Error> NoteStore::relation_names() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot) {
        if (impl_->last_error)
            return std::unexpected(*impl_->last_error);
        return std::make_shared<const std::vector<RelationName>>();
    }
    if (snapshot->relation_names)
        return snapshot->relation_names;
    auto txn = impl_->opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), {}));
    const auto edges = read_relation_edges(*txn, *snapshot, impl_->relation_name_bytes);
    if (!edges)
        return std::unexpected(from_graph(edges.error(), {}));
    snapshot->relation_names =
        std::make_shared<const std::vector<RelationName>>(count_relation_names(*edges));
    return snapshot->relation_names;
}

std::optional<NotePath> NoteStore::resolve_link(std::string_view target) const {
    const std::string_view key = link_target(target);
    if (key.empty())
        return std::nullopt;
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    if (!snapshot)
        return std::nullopt;
    const LinkIndex& index = link_index_of(*snapshot);
    const auto found = index.find(std::string(key));
    if (found == index.end())
        return std::nullopt;
    const auto row = snapshot->live_by_id.find(found->second);
    if (row == snapshot->live_by_id.end())
        return std::nullopt;
    return snapshot->live[row->second].info.path;
}

std::expected<RelationId, Error> NoteStore::add_relation(const NotePath& from, const NotePath& to,
                                                         const RelationSpec& relation) {
    const auto name = relation_name(relation.name, impl_->relation_name_bytes);
    if (!name)
        return std::unexpected(name.error());
    const std::string path = to.string();
    std::optional<graph::EdgeId> added;
    auto written = write_pair(
        *impl_, from, to,
        [&](graph::WriteTxn& txn, const NodeId& source, const NodeId& target) -> std::expected<void, Error> {
            const auto existing = pair_relations(txn, source, target, path, impl_->relation_name_bytes);
            if (!existing)
                return std::unexpected(existing.error());
            if (!with_key(*existing, tag_key(*name)).empty())
                return std::unexpected(already_exists(path));
            auto id = put_relation(txn, source, target, *name, relation.directed, *existing, path);
            if (!id)
                return std::unexpected(id.error());
            added = *id;
            return {};
        });
    if (!written)
        return std::unexpected(written.error());
    // write_pair başarıyla döndüyse gövde koştu ve kimliği yazdı.
    return RelationId{added.value().bytes};
}

std::expected<void, Error> NoteStore::rename_relation(const RelationId& relation, std::string_view new_name) {
    const auto renamed = relation_name(new_name, impl_->relation_name_bytes);
    if (!renamed)
        return std::unexpected(renamed.error());
    return write_relation(
        *impl_, relation,
        [&](WriteContext& context, const Relation& target, const std::vector<Relation>& pair,
            const std::string& path) -> std::expected<void, Error> {
            const std::string new_key = tag_key(*renamed);
            const bool taken = std::ranges::any_of(
                pair, [&](const Relation& other) { return other.id != target.id && other.key == new_key; });
            if (taken)
                return std::unexpected(already_exists(path));
            if (target.name == *renamed)
                return {};
            // Kimlik aynı kalır: eşzamanlı yeniden adlandırma aynı özelliğin head'leridir (LWW, raporlanır);
            // eşzamanlı silme bu yazmayı görmediğinden etkisizdir, ilişki yönüyle kalır.
            const graph::Value value = renamed->empty() ? graph::Value{} : graph::Value(*renamed);
            NOTEAPP_TRY_GRAPH(context.txn.set_property(target.id, kName, value), path);
            return {};
        });
}

std::expected<void, Error> NoteStore::set_relation_direction(const RelationId& relation,
                                                             const std::optional<NotePath>& source) {
    return write_relation(
        *impl_, relation,
        [&](WriteContext& context, const Relation& target, const std::vector<Relation>& pair,
            const std::string& path) -> std::expected<void, Error> {
            graph::WriteTxn& txn = context.txn;
            NodeId from = target.src;
            NodeId to = target.dst;
            if (source) {
                const std::string key = source->string();
                const auto row = live_note_id(context.snapshot, *source);
                if (!row || (*row != target.src && *row != target.dst))
                    return std::unexpected(invalid_path("not_relation_end", key));
                from = *row;
                to = from == target.src ? target.dst : target.src;
            }
            const bool directed = source.has_value();
            // Saklı adı aynı ilişkilerden (senkronla ters yönde kurulmuş kopya) uçları istenenle aynı olan
            // kalır, yalnız `directed`ı değişir; öbürleri silinir. Anahtar değil saklı ad karşılaştırılır:
            // gösterimi aynı (temizlenmiş, kesilmiş) ama saklı adı farklı ilişki ayrı ilişkidir, silinmez.
            // Hiçbiri tutmazsa kaynak değişiyordur: kimlik uçlardan türediğinden kenar aynı işlemde yeniden
            // kurulur.
            std::vector<Relation> matches;
            std::ranges::copy_if(pair, std::back_inserter(matches), [&](const Relation& candidate) {
                return candidate.stored == target.stored;
            });
            const auto [src, dst] = relation_ends(from, to, directed);
            const auto kept = std::ranges::find_if(matches, [&](const Relation& candidate) {
                return candidate.src == src && candidate.dst == dst;
            });
            for (auto candidate = matches.begin(); candidate != matches.end(); ++candidate)
                if (candidate != kept)
                    NOTEAPP_TRY_GRAPH(txn.delete_edge(candidate->id), path);
            if (kept == matches.end()) {
                const auto put = put_relation(txn, from, to, target.name, directed, pair, path);
                if (!put)
                    return std::unexpected(put.error());
                return {};
            }
            if (kept->directed != directed)
                NOTEAPP_TRY_GRAPH(
                    txn.set_property(kept->id, kDirected, directed ? graph::Value(true) : graph::Value{}),
                    path);
            return {};
        });
}

std::expected<void, Error> NoteStore::remove_relation(const RelationId& relation) {
    return write_relation(*impl_, relation,
                          [&](WriteContext& context, const Relation& target, const std::vector<Relation>&,
                              const std::string& path) -> std::expected<void, Error> {
                              NOTEAPP_TRY_GRAPH(context.txn.delete_edge(target.id), path);
                              return {};
                          });
}

}  // namespace noteapp::core
