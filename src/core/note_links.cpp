#include "core/note_store_impl.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <variant>

namespace noteapp::core {

using graph::NodeId;

namespace {

// Yol başına tek, sıralı: paralel kenar ve iki yönlü ilişki bir satırdır.
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

graph::Result<NoteLinks> read_links(graph::ReadTxn& txn, const Snapshot& snapshot, const NodeId& id) {
    auto outgoing = live_neighbors(txn, snapshot, id, graph::Direction::Out, kLinksTo);
    if (!outgoing)
        return std::unexpected(outgoing.error());
    auto incoming = live_neighbors(txn, snapshot, id, graph::Direction::In, kLinksTo);
    if (!incoming)
        return std::unexpected(incoming.error());
    auto related = live_neighbors(txn, snapshot, id, graph::Direction::Both, kRelatedTo);
    if (!related)
        return std::unexpected(related.error());
    auto missing = add_text_targets(txn, snapshot, id, *outgoing);
    if (!missing)
        return std::unexpected(missing.error());
    auto tags = read_note_tags(txn, id);
    if (!tags)
        return std::unexpected(tags.error());
    return NoteLinks{sorted_paths(std::move(*outgoing)), sorted_paths(std::move(*incoming)),
                     sorted_paths(std::move(*related)), std::move(*missing), std::move(*tags)};
}

// Kenar yalnız çıkan yönde okunur: her kenar kaynağında bir kez görülür. İlişki iki cihazdan iki yönde
// kurulmuş olabilir; yönsüz tek satıra iner.
graph::Result<std::vector<NoteEdge>> read_edges(graph::ReadTxn& txn, const Snapshot& snapshot) {
    std::vector<NoteEdge> edges;
    for (const NoteRow& row : snapshot.live) {
        for (const auto& [type, kind] :
             {std::pair{kLinksTo, EdgeKind::LinksTo}, std::pair{kRelatedTo, EdgeKind::RelatedTo}}) {
            auto ids = neighbor_ids(txn, row.id, graph::Direction::Out, type);
            if (!ids)
                return std::unexpected(ids.error());
            for (const NodeId& id : *ids) {
                const auto target = snapshot.live_by_id.find(id);
                if (id == row.id || target == snapshot.live_by_id.end())
                    continue;
                NoteEdge edge{row.info.path, snapshot.live[target->second].info.path, kind};
                if (kind == EdgeKind::RelatedTo && edge.to < edge.from)
                    std::swap(edge.from, edge.to);
                edges.push_back(std::move(edge));
            }
        }
    }
    std::ranges::sort(edges);
    const auto duplicates = std::ranges::unique(edges);
    edges.erase(duplicates.begin(), duplicates.end());
    return edges;
}

std::expected<NodeId, Error> live_note_id(const Snapshot& snapshot, const NotePath& path) {
    const std::string key = path.string();
    const auto row = snapshot.live_by_path.find(key);
    if (row == snapshot.live_by_path.end())
        return std::unexpected(not_found(key));
    return snapshot.live[row->second].id;
}

const std::vector<NotePath>* group_paths(const NoteLinks& links, LinkGroup group) {
    switch (group) {
    case LinkGroup::Incoming:
        return &links.incoming;
    case LinkGroup::Outgoing:
        return &links.outgoing;
    case LinkGroup::Related:
        return &links.related;
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
    const std::vector<NotePath>* paths = line.is_header ? nullptr : group_paths(links, line.group);
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
    auto links = read_links(*txn, *snapshot, *id);
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
    auto edges = read_edges(*txn, *snapshot);
    if (!edges)
        return std::unexpected(from_graph(edges.error(), {}));
    return std::move(*edges);
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

std::expected<void, Error> NoteStore::add_relation(const NotePath& from, const NotePath& to) {
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            const auto source = live_note_id(context.snapshot, from);
            if (!source)
                return std::unexpected(source.error());
            const auto target = live_note_id(context.snapshot, to);
            if (!target)
                return std::unexpected(target.error());
            if (*source == *target)
                return std::unexpected(invalid_path("self_relation", to.string()));
            NOTEAPP_TRY(context.add_relation(*source, *target, to.string()));
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

std::expected<void, Error> NoteStore::remove_relation(const NotePath& first, const NotePath& second) {
    return impl_
        ->write<std::monostate>([&](WriteContext& context) -> std::expected<std::monostate, Error> {
            const auto one = live_note_id(context.snapshot, first);
            if (!one)
                return std::unexpected(one.error());
            const auto other = live_note_id(context.snapshot, second);
            if (!other)
                return std::unexpected(other.error());
            NOTEAPP_TRY(context.remove_relation(*one, *other, second.string()));
            return std::monostate{};
        })
        .transform([](std::monostate) {});
}

}  // namespace noteapp::core
