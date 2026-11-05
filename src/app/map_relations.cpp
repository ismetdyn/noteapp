#include "app/map_relations.hpp"

#include "core/i18n.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace noteapp::app {

namespace {

using core::Error;

Error map_error(Error::Code code, const char* key,
                std::vector<std::pair<std::string, std::string>> args = {}) {
    return Error{code, key, std::move(args)};
}

std::expected<core::NotePath, Error> path_of(const MapNode& node) {
    if (node.kind != MapNodeKind::Note)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.invalid_pair"));
    return core::NotePath::parse(node.key);
}

// İki ucu da not, farklı notlar.
std::expected<std::pair<core::NotePath, core::NotePath>, Error> note_pair(const MapNode& from,
                                                                          const MapNode& to) {
    auto first = path_of(from);
    auto second = path_of(to);
    if (!first || !second || *first == *second)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.invalid_pair"));
    return std::pair(std::move(*first), std::move(*second));
}

struct NoteAndTag {
    core::NotePath note;
    std::string tag;  // tag_key
};

std::expected<NoteAndTag, Error> note_and_tag(const MapNode& from, const MapNode& to) {
    if (map_drop(from, to) != MapDrop::AddTag)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.invalid_pair"));
    const bool note_first = from.kind == MapNodeKind::Note;
    auto note = path_of(note_first ? from : to);
    if (!note)
        return std::unexpected(note.error());
    return NoteAndTag{std::move(*note), (note_first ? to : from).key};
}

bool has_tag(std::string_view path, std::string_view body, std::string_view key) {
    const std::vector<std::string> tags = core::note_tags(path, body);
    return std::ranges::any_of(tags, [&](const std::string& tag) { return core::tag_key(tag) == key; });
}

// Gövdenin sonuna kendi satırı olarak; satır sonu notunkidir (CRLF korunur).
std::string appended(std::string body, std::string_view line) {
    const std::string_view newline = body.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    if (!body.empty() && !body.ends_with('\n'))
        body += newline;
    body += line;
    body += newline;
    return body;
}

// Bağlantı ve etiket metne yazılır: ikili ya da kod notunun sonuna eklenen satır onu bozardı.
std::expected<core::NoteContent, Error> read_prose(const core::NoteStore& store, const core::NotePath& note) {
    auto content = store.read(note);
    if (!content)
        return std::unexpected(content.error());
    if (!core::is_prose_note(note.string()) || core::looks_binary(content->text))
        return std::unexpected(
            map_error(Error::Code::InvalidPath, "map.not_prose", {{"path", note.string()}}));
    return content;
}

std::expected<std::string, Error> add_link(core::NoteStore& store, const core::NotePath& from,
                                           const core::NotePath& to, const BeforeWrite& before_write) {
    auto content = read_prose(store, from);
    if (!content)
        return std::unexpected(content.error());
    const auto links = store.links(from);
    if (!links)
        return std::unexpected(links.error());
    if (std::ranges::find(links->outgoing, to) != links->outgoing.end())
        return std::unexpected(map_error(Error::Code::AlreadyExists, "map.already_linked",
                                         {{"from", from.string()}, {"to", to.string()}}));
    // Hedef adı güvenilmezdir (senkron): `|`, `#`, `[[`, `]]` ya da sondaki `]` [[...]]'ı başka hedef ya da
    // birden çok bağlantı okutur. Yazılmadan önce yeni gövde yazmanın tarayıcısıyla geri okunur: tam olarak
    // tek yeni anahtar, o da hedefe çözülmeli. Önceki kapanmamış `[[` (kod içinde de) bağlantıyı yutarsa da
    // reddedilir.
    const std::string target = to.string();
    if (target.find("[[") != std::string::npos || target.find("]]") != std::string::npos ||
        target.ends_with(']') || core::link_target(target) != target || store.resolve_link(target) != to)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.link_not_added", {{"to", target}}));
    std::string body = appended(content->text, "[[" + target + "]]");
    const std::vector<std::string> before = core::note_link_targets(content->text);
    const std::vector<std::string> after = core::note_link_targets(body);
    std::vector<std::string> added;
    std::ranges::set_difference(after, before, std::back_inserter(added));
    if (added != std::vector<std::string>{target} || after.size() != before.size() + 1)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.link_not_added", {{"to", target}}));
    if (before_write)
        before_write();
    const auto written = store.write(from, body, content->revision);
    if (!written)
        return std::unexpected(written.error());
    return core::tr("map.linked", {{"from", from.string()}, {"to", target}});
}

std::expected<std::string, Error> add_tag(core::NoteStore& store, const NoteAndTag& target,
                                          const BeforeWrite& before_write) {
    const std::string path = target.note.string();
    auto content = read_prose(store, target.note);
    if (!content)
        return std::unexpected(content.error());
    if (has_tag(path, content->text, target.tag))
        return std::unexpected(map_error(Error::Code::AlreadyExists, "map.already_tagged",
                                         {{"path", path}, {"tag", target.tag}}));
    std::string body = appended(std::move(content->text), "#" + target.tag);
    // Sonda kapanmamış kod çiti varsa eklenen satır etiket sayılmaz: yazılmaz.
    if (!has_tag(path, body, target.tag))
        return std::unexpected(
            map_error(Error::Code::InvalidPath, "map.tag_not_added", {{"path", path}, {"tag", target.tag}}));
    if (before_write)
        before_write();
    if (const auto written = store.write(target.note, body, content->revision); !written)
        return std::unexpected(written.error());
    return core::tr("map.tagged", {{"path", path}, {"tag", target.tag}});
}

std::expected<std::string, Error> remove_tag(core::NoteStore& store, const NoteAndTag& target,
                                             const BeforeWrite& before_write) {
    const std::string path = target.note.string();
    const auto content = store.read(target.note);
    if (!content)
        return std::unexpected(content.error());
    if (!has_tag(path, content->text, target.tag))
        return std::unexpected(
            map_error(Error::Code::NotFound, "map.not_tagged", {{"path", path}, {"tag", target.tag}}));
    const std::string body = core::remove_note_tag(path, content->text, target.tag);
    if (has_tag(path, body, target.tag))  // aynı tarayıcı: olmamalı, olursa yazılmaz
        return std::unexpected(
            map_error(Error::Code::InvalidPath, "map.not_tagged", {{"path", path}, {"tag", target.tag}}));
    if (before_write)
        before_write();
    if (const auto written = store.write(target.note, body, content->revision); !written)
        return std::unexpected(written.error());
    return core::tr("map.untagged", {{"path", path}, {"tag", target.tag}});
}

}  // namespace

MapDrop map_drop(const MapNode& from, const MapNode& to) {
    if (from.kind == to.kind && from.key == to.key)
        return MapDrop::None;
    if (from.kind == MapNodeKind::Note && to.kind == MapNodeKind::Note)
        return MapDrop::ChooseNoteEdge;
    if (from.kind != to.kind)
        return MapDrop::AddTag;
    return MapDrop::None;
}

bool map_edge_removable(MapEdgeKind kind) {
    return kind != MapEdgeKind::Link;
}

std::optional<core::NotePath> map_edit_note(const MapNode& from, const MapNode& to, MapEdgeKind kind) {
    if (kind == MapEdgeKind::Related)
        return std::nullopt;
    const MapNode& note = kind == MapEdgeKind::Tag && from.kind == MapNodeKind::Tag ? to : from;
    auto path = path_of(note);
    return path ? std::optional(std::move(*path)) : std::nullopt;
}

std::expected<std::string, Error> connect_map_nodes(core::NoteStore& store, const MapNode& from,
                                                    const MapNode& to, MapEdgeKind kind,
                                                    const BeforeWrite& before_write) {
    if (kind == MapEdgeKind::Tag) {
        const auto target = note_and_tag(from, to);
        if (!target)
            return std::unexpected(target.error());
        return add_tag(store, *target, before_write);
    }
    const auto pair = note_pair(from, to);
    if (!pair)
        return std::unexpected(pair.error());
    const auto& [first, second] = *pair;
    if (kind == MapEdgeKind::Link)
        return add_link(store, first, second, before_write);
    if (const auto added = store.add_relation(first, second); !added)
        return std::unexpected(added.error());
    return core::tr("map.related", {{"from", first.string()}, {"to", second.string()}});
}

std::expected<std::string, Error> disconnect_map_nodes(core::NoteStore& store, const MapNode& from,
                                                       const MapNode& to, MapEdgeKind kind,
                                                       const BeforeWrite& before_write) {
    if (kind == MapEdgeKind::Tag) {
        const auto target = note_and_tag(from, to);
        if (!target)
            return std::unexpected(target.error());
        return remove_tag(store, *target, before_write);
    }
    const auto pair = note_pair(from, to);
    if (!pair)
        return std::unexpected(pair.error());
    const auto& [first, second] = *pair;
    if (kind == MapEdgeKind::Link)
        return std::unexpected(map_error(Error::Code::InvalidPath, "map.link_in_text",
                                         {{"from", first.string()}, {"to", second.string()}}));
    if (const auto removed = store.remove_relation(first, second); !removed)
        return std::unexpected(removed.error());
    return core::tr("map.unrelated", {{"from", first.string()}, {"to", second.string()}});
}

std::optional<MapEdgeKind> map_edge_between(const NoteMap& map, const MapNode& first, const MapNode& second) {
    const auto same = [](const MapNode& a, const MapNode& b) { return a.kind == b.kind && a.key == b.key; };
    std::optional<MapEdgeKind> found;
    for (const MapEdge& edge : map.edges) {
        const MapNode& from = map.nodes[edge.from];
        const MapNode& to = map.nodes[edge.to];
        if (!(same(from, first) && same(to, second)) && !(same(from, second) && same(to, first)))
            continue;
        if (map_edge_removable(edge.kind))
            return edge.kind;
        found = edge.kind;
    }
    return found;
}

std::optional<std::size_t> map_edge_at(const NoteMap& map, std::span<const LayoutPoint> positions,
                                       LayoutPoint at, float reach) {
    std::optional<std::size_t> best;
    float best_distance = reach * reach;
    for (std::size_t i = 0; i < map.edges.size(); ++i) {
        const MapEdge& edge = map.edges[i];
        if (edge.from >= positions.size() || edge.to >= positions.size())
            continue;
        const LayoutPoint a = positions[edge.from];
        const LayoutPoint b = positions[edge.to];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = dx * dx + dy * dy;
        // Parçanın `at`a en yakın noktası: uçlar arasında kıstırılmış izdüşüm.
        const float t =
            length > 0.0F ? std::clamp(((at.x - a.x) * dx + (at.y - a.y) * dy) / length, 0.0F, 1.0F) : 0.0F;
        const float ex = a.x + t * dx - at.x;
        const float ey = a.y + t * dy - at.y;
        if (const float distance = ex * ex + ey * ey; distance <= best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

}  // namespace noteapp::app
